/*
 * SECTION INDEX-SHARD: module-overview
 *
 * pthread index-sharding for onefield_run()
 *
 * This module executes one candidate index as one shard task.  It does not
 * split the image, xylist, field stars, quads, or verification math.
 *
 * Ownership model:
 *   - worker threads compute local shard results
 *   - reducer thread merges results into the master onefield_t
 *   - master onefield_t remains the final source of truth
 *
 * Threading model:
 *   - one persistent worker pool per engine job
 *   - one submitted pass per onefield_run() call
 *   - one task = one candidate index
 *   - no pthread_cancel
 *   - stop is cooperative through shared flags + solver.quit_now
 *
 * Safety constraints:
 *   - no shared solver_t between workers
 *   - no worker writes directly into master bp->solutions
 *   - no persistent full index_t cache in production path
 *   - index load/release follows the original onefield ownership hooks
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "index_shard_internal.h"
#include "index_shard_config.h"
#include "astrometry/bl.h"
#include "astrometry/errors.h"
#include "astrometry/log.h"
#include "astrometry/tic.h"
#include "kdtree_phase_a_internal.h"
#include "astrometry/fitsbin.h"
#include "kdtree_prefetch_internal.h"

enum {
  INDEX_SHARD_DISCOVERY_FRONTIER_LIMIT = 1,
  INDEX_SHARD_SINGLE_WORKER_ORDERED_QUANTUM = 2
};

/*
 * Bound the time an owner will wait for an unclaimed lending reservation.
 * This is a scheduler safety bound, not an image- or index-specific rule.
 */
static const double INDEX_SHARD_LEND_OWNER_WAIT_SECONDS = 0.005;

/*
 * SECTION INDEX-SHARD: types
 */
// ANCHOR INDEX-SHARD: result-state
/*
 * Result produced by exactly one shard task.
 *
 * The worker owns this object until completed[index_order] is published.
 * The reducer may then transfer MatchObj payloads into master bp->solutions.
 *
 * Important:
 *   - solutions is worker-local until merge
 *   - merged prevents double-free / double-merge
 *   - solved means this shard produced an accepted candidate or local solve flag
 */
typedef struct index_shard_result {
  bl *solutions; // worker-local MatchObj list for this index

  int failed; // hard task failure, not normal "did not solve"
  int rc;

  anbool solved; // accepted solution detected for this shard
  anbool has_accepted_solution;

  double best_logodds; // diagnostic + future usefulness hint
  int best_fieldnum;

  /*
   * solve wall covers solve_one_index() only.
   *
   * cpu_seconds is a process-wide CPU delta and is diagnostic only when
   * pthread workers overlap; it is not exclusive CPU time for this task.
   */
  double wall_seconds;
  float cpu_seconds;

   /*
   * Full outer-task timing covers local reset, index acquisition, solving,
   * result analysis, and index release. It excludes queue wait and reducer
   * wait.
   */
  anbool task_started;
  int worker_id;
  double task_wall_seconds;
  double task_start_since_pass;
  double task_finish_since_pass;

  /*
   * Non-overlapping wall-time attribution inside the outer task.
   *
   * solve_seconds is represented by wall_seconds above to preserve the
   * existing result layout and diagnostics.
   */
  double reset_seconds;
  double acquire_seconds;
  double analyze_seconds;
  double release_seconds;

  anbool hit_total_cpulimit;

  anbool cancelled;

  size_t index_order; // original candidate index order in onefield pass
  int merged;         // reducer already consumed/transferred this result
} index_shard_result_t;

// ANCHOR INDEX-SHARD: task-state
/*
 * Minimal shard task descriptor.
 *
 * Keep this intentionally small.  The task only identifies which candidate
 * index from the current onefield pass should be tried.
 */
typedef struct index_shard_task {
  size_t index_order;
  anbool discovery_frontier;
} index_shard_task_t;

// ANCHOR INDEX-SHARD: shared-pass-state
/*
 * Shared state for one submitted onefield_run() pass.
 *
 * Lifetime:
 *   - initialized by index_shard_pool_submit()
 *   - read/updated by workers + reducer during one pass
 *   - task/result arrays are owned by index_shard_solve()
 *
 * Locking:
 *   - queue_mutex protects task claiming, task credit, and solved frontier
 *   - result_mutex protects completed slots + active worker count
 *   - state_mutex protects stop/fatal/committed-solve pass state
 *   - limit_mutex protects process-wide CPU-limit publication
 *
 * Do not store per-worker heavy data here.  Per-worker context belongs in
 * index_shard_worker_context_t.
 */
typedef struct index_shard_thread_state {
  onefield_t *bp;                   // master bp, reducer-owned for writes
  const solver_t *base_sp;          // read-only template for local solvers
  const index_shard_hooks_t *hooks; // bridge back into onefield.c

  size_t nindexes;

  index_shard_task_t *tasks;
  size_t ntasks;
  size_t next_task; // total tasks claimed from both immutable lanes
  size_t next_frontier_task;
  size_t next_ordered_task;
  size_t task_family_count;
  size_t task_frontier_count;
  size_t ordered_since_frontier;
  int frontier_running;

  index_shard_result_t *results;
  unsigned char *completed; // result slot is visible to reducer
  size_t next_reduce;       // ordered prefix reducer cursor

  pthread_mutex_t queue_mutex;
  pthread_cond_t queue_cv;

  pthread_mutex_t result_mutex;
  pthread_cond_t result_cv;

  pthread_mutex_t state_mutex;

  pthread_mutex_t limit_mutex;

  int worker_count;
  int active_workers; // workers still participating in pass
  int running_tasks;  // claimed but not completed tasks
  int active_limit;   // concurrency cap, usually worker_count
  int max_active_workers;

  int stop_requested;   // cooperative stop, no new claims
  int fatal_error;      // hard worker/module failure
  int solved_published; // reducer committed a valid solved result
  int master_committed;

  int have_solved_order;        // solved frontier exists
  size_t earliest_solved_order; // earliest original order known to solve

  int limit_reported; // avoid repeated CPU-limit log spam

  double pass_wall_start;
  float pass_cpu_start;

  fitsbin_mmap_advice_t mmap_advice;
  unsigned int mmap_pass_number;

  struct rusage pass_rusage_start;
  int pass_rusage_valid;
} index_shard_thread_state_t;

typedef struct index_shard_pool index_shard_pool_t;
static __thread index_shard_pool_t *index_shard_current_worker_pool = NULL;
// ANCHOR INDEX-SHARD: worker-context-state
/*
 * Private state for one pthread worker.
 *
 * local_bp is reused across all tasks within one submitted pass.  This avoids
 * repeated xylist open/close + local solver allocation per index.
 *
 * Important:
 *   - local_bp must never publish directly into master bp->solutions
 *   - local_context_generation ties local_bp to the active pool generation
 *   - no persistent full index_t cache here in the production path
 */
typedef struct index_shard_worker_context {
  int worker_id;
  unsigned long generation_seen;
  struct index_shard_pool *pool;

  onefield_t local_bp; // worker-local onefield copy
  int local_context_ready;
  unsigned long local_context_generation;

} index_shard_worker_context_t;
typedef struct index_shard_aux_task {
  index_shard_aux_task_fn fn;
  void *userdata;
  struct index_shard_aux_group *group;
  struct index_shard_aux_task *next;
} index_shard_aux_task_t;

struct index_shard_aux_group {
  pthread_mutex_t mutex;
  pthread_cond_t cv;

  index_shard_pool_t *pool;

  int pending;
  int failed;
  int closed;
  int lend_slot;
  int lend_claimed;
  unsigned long progress;
};

typedef struct index_shard_aux_queue {
  pthread_mutex_t mutex;

  index_shard_aux_task_t *head;
  index_shard_aux_task_t *tail;

  size_t pending;
  size_t max_pending;

  unsigned long long submitted_total;
  unsigned long long executed_total;
  unsigned long long rejected_total;
  unsigned long long cancelled_total;

  int stopping;
} index_shard_aux_queue_t;

typedef struct index_shard_aux_metrics_snapshot {
  unsigned long long submitted;
  unsigned long long executed;
  unsigned long long rejected;
  unsigned long long cancelled;

  size_t pending;
  size_t max_pending;
} index_shard_aux_metrics_snapshot_t;
/*
 * Shared prefetch coordinator safety ceilings.
 *
 * These are memory/I/O bounds, not workload-specific activation thresholds.
 * For four workers and 4-KiB pages:
 *
 *   issue budget:   4 * 64 pages = 1 MiB per flush
 *   collection cap: 4 MiB worth of raw page candidates
 *
 * Metadata pages always outrank leaf payload pages.
 */
#define INDEX_SHARD_PREFETCH_ISSUE_PAGES_PER_WORKER 64
#define INDEX_SHARD_PREFETCH_COLLECT_MULTIPLIER 4
#define INDEX_SHARD_PREFETCH_RECENT_MULTIPLIER 2
/*
 * Absolute safety ceiling on kernel-advice issue windows per pass.
 *
 * This is not a workload-selection threshold. It prevents prefetch traffic
 * from becoming an unbounded second workload.
 */
#define INDEX_SHARD_PREFETCH_MAX_ISSUE_WINDOWS_PER_PASS 64

typedef struct index_shard_prefetch_page {
  fitsbin_t *fb;
  const void *map_base;

  uintptr_t page;
  size_t page_size;

  unsigned int priority;
  kdtree_prefetch_array_kind_t kind;
} index_shard_prefetch_page_t;

typedef struct index_shard_prefetch_metrics {
  unsigned long long hints_emitted;
  unsigned long long hints_stale;
  unsigned long long hints_unmapped;

  unsigned long long pages_raw;
  unsigned long long pages_unique;
  unsigned long long pages_duplicate;
  unsigned long long pages_selected;

  unsigned long long pages_collection_dropped;
  unsigned long long pages_budget_dropped;

  unsigned long long metadata_pages_selected;
  unsigned long long leaf_pages_selected;

  /*
   * publish_calls counts worker-local batches transferred into the shared
   * accumulator. flushes counts actual kernel-advice issue windows.
   */
  unsigned long long publish_calls;
  unsigned long long publish_empty;
  unsigned long long issue_below_threshold;

  unsigned long long flushes;
  unsigned long long ranges_issued;
  unsigned long long bytes_issued;
  unsigned long long prefetch_failures;

  unsigned long long pass_budget_exhausted;
   /*
   * Mapping-lifetime barriers prevent deferred page descriptors from
   * surviving the index/mmap object that owns them.
   */
  unsigned long long mapping_barriers;
  unsigned long long pending_pages_purged;
  unsigned long long recent_pages_purged;
} index_shard_prefetch_metrics_t;

typedef struct index_shard_prefetch_metrics_snapshot {
  index_shard_prefetch_metrics_t totals;

  size_t pending;

  size_t issue_page_budget;
  size_t issue_threshold_pages;

  size_t pass_page_budget;
  size_t pass_pages_issued;

  size_t collection_capacity;
  size_t recent_capacity;
} index_shard_prefetch_metrics_snapshot_t;

typedef struct index_shard_prefetch_coordinator {
  pthread_mutex_t mutex;
  pthread_mutex_t flush_mutex;

  int initialized;
  unsigned long generation;

  index_shard_prefetch_page_t *pending;
  index_shard_prefetch_page_t *snapshot;
  index_shard_prefetch_page_t *selected;
  index_shard_prefetch_page_t *recent;

  size_t pending_count;
  size_t pending_capacity;

  size_t issue_page_budget;
  size_t issue_threshold_pages;

  /*
   * Hard per-pass ceiling. Reset at every pool generation.
   */
  size_t pass_page_budget;
  size_t pass_pages_issued;

  size_t recent_count;
  size_t recent_capacity;
  size_t recent_next;

  index_shard_prefetch_metrics_t metrics;
} index_shard_prefetch_coordinator_t;

static int index_shard_prefetch_coordinator_init(
    index_shard_prefetch_coordinator_t *coordinator,
    int worker_count);

static void index_shard_prefetch_coordinator_destroy(
    index_shard_prefetch_coordinator_t *coordinator);

static void index_shard_prefetch_coordinator_reset(
    index_shard_prefetch_coordinator_t *coordinator,
    unsigned long generation);

static void index_shard_prefetch_metrics_snapshot(
    index_shard_prefetch_coordinator_t *coordinator,
    index_shard_prefetch_metrics_snapshot_t *snapshot);

// ANCHOR INDEX-SHARD: pool-state
/*
 * Persistent worker pool for one engine job.
 *
 * The pool survives across multiple onefield_run() submissions.  Workers sleep
 * between generations and wake when index_shard_pool_submit() increments
 * generation.
 */
struct index_shard_pool {
  onefield_t *owner_bp;
  solver_t *owner_sp;

  int worker_count;
  pthread_t *threads;
  index_shard_worker_context_t *contexts;

  pthread_mutex_t control_mutex;
  pthread_cond_t work_cv;

  int shutdown;
  int stopping;
  int pass_active;
  unsigned long generation; // pass submission counter

  index_shard_thread_state_t shared;
  index_shard_aux_queue_t auxq;

  /*
   * One configured worker may be lent at an outer-task boundary to the group
   * holding lend_group. The token never creates a thread and never changes
   * the ordered outer task plan. Protected by auxq.mutex.
   */
  struct index_shard_aux_group *lend_group;
  unsigned long long lend_acquired_total;
  unsigned long long lend_busy_total;
  unsigned long long lend_tasks_total;
  unsigned long long lend_fallback_total;

  /*
   * One mapping-aware prefetch coordinator shared by all solver workers.
   */
  index_shard_prefetch_coordinator_t prefetch;

} ;

static index_shard_pool_t *index_shard_global_pool = NULL;
static pthread_mutex_t index_shard_global_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_key_t index_shard_tls_key;
static pthread_once_t index_shard_tls_once = PTHREAD_ONCE_INIT;

static int index_shard_aux_queue_init(index_shard_aux_queue_t *q,
                                      size_t max_pending);
static void index_shard_aux_queue_destroy(index_shard_aux_queue_t *q);
static int index_shard_aux_queue_push(index_shard_pool_t *pool,
                                      index_shard_aux_task_t *task);
static index_shard_aux_task_t *
index_shard_aux_queue_try_pop(index_shard_aux_queue_t *q,
                              const index_shard_aux_group_t *exclude_group,
                              int lend_only,
                              int skip_lent);
static int index_shard_help_lent_once(index_shard_pool_t *pool);
static void index_shard_aux_group_done(index_shard_aux_group_t *group,
                                       int failed);
static void index_shard_aux_cancel_list(index_shard_aux_task_t *task);
static void index_shard_aux_execute_one(index_shard_aux_task_t *task);
static int index_shard_kdtree_wait(void *userdata);
static int index_shard_pool_capacity(index_shard_pool_t *pool,
                                     index_shard_aux_group_t *group,
                                     kdtree_task_capacity_t *capacity);
static int index_shard_kdtree_capacity(void *userdata,
                                       kdtree_task_capacity_t *capacity);

/*
 * SECTION INDEX-SHARD: tls - thread logical singleton
 *
 * TLS links a running worker callback back to its pool state.
 *
 * solve_fields()/solver_run() call into onefield callbacks.  Those callbacks
 * receive only local onefield_t, so TLS is used to check the global shard stop
 * state and set local_bp->solver.quit_now.
 */
static void index_shard_make_tls_key(void) {
  (void)pthread_key_create(&index_shard_tls_key,
                           NULL); // set before solve_one_index(), clear immediately after return
}

static void index_shard_set_tls(index_shard_worker_context_t *ctx) {
  // NULL outside shard worker threads
  pthread_once(&index_shard_tls_once, index_shard_make_tls_key);
  pthread_setspecific(index_shard_tls_key, ctx);
}

static index_shard_worker_context_t *index_shard_get_tls(void) {
  pthread_once(&index_shard_tls_once, index_shard_make_tls_key);
  return pthread_getspecific(index_shard_tls_key);
}

/*
 * SECTION INDEX-SHARD: configuration
 *
 * Process environment and platform defaults are resolved once by the private
 * configuration module. Hot execution paths consume immutable values only.
 */
anbool index_shard_pthread_enabled(void) {
  return index_shard_config_get()->pthread_enabled;
}

anbool index_shard_trace_enabled(void) {
  /*
   * Detailed scheduler traces follow the ordinary command-line verbosity
   * model. Two -v flags select LOG_ALL without another environment control.
   */
  return log_get_level() >= LOG_ALL;
}

static int index_shard_get_worker_count(size_t nindexes) {
  return index_shard_config_effective_workers(nindexes);
}

typedef struct index_shard_pass_state_snapshot {
  int stop_requested;
  int fatal_error;
  int solved_published;
  int master_committed;
} index_shard_pass_state_snapshot_t;

typedef struct index_shard_pass_metrics_snapshot {
  size_t reduced;

  double wall_seconds;
  float cpu_seconds;
  double cpu_percent;

  int resource_available;
  double user_seconds;
  double system_seconds;

  long minor_faults;
  long major_faults;
  long voluntary_context_switches;
  long involuntary_context_switches;
} index_shard_pass_metrics_snapshot_t;

typedef struct index_shard_task_profile_snapshot {
  size_t executed;
  int quantiles_available;

  double task_p50_seconds;
  double task_p90_seconds;
  double task_p99_seconds;
  double task_max_seconds;

  double max_solve_seconds;
  size_t max_index_order;
  int max_worker_id;

  double max_to_p50;
  double max_pool_percent;

  double serial_tail_seconds;
  double serial_tail_percent;
  size_t tail_index_order;
  int tail_worker_id;
} index_shard_task_profile_snapshot_t;

typedef struct index_shard_phase_profile_snapshot {
  size_t executed;
  int quantiles_available;

  double task_wall_total;

  double reset_total;
  double acquire_total;
  double solve_total;
  double analyze_total;
  double release_total;
  double other_total;

  double reset_percent;
  double acquire_percent;
  double solve_percent;
  double analyze_percent;
  double release_percent;
  double other_percent;

  double acquire_p50;
  double acquire_p90;
  double acquire_p99;
  double acquire_max;

  double solve_p50;
  double solve_p90;
  double solve_p99;
  double solve_max;
} index_shard_phase_profile_snapshot_t;

// ANCHOR INDEX-SHARD: pass-state-snapshot
/*
 * Take one synchronized snapshot of pass termination state.
 *
 * state_mutex is the only lock that protects these fields.  Callers may hold
 * queue_mutex or result_mutex while taking this snapshot; no function may hold
 * state_mutex and then acquire either of those locks.
 */
static void index_shard_pass_state_snapshot(index_shard_thread_state_t *shared,
                                            index_shard_pass_state_snapshot_t *snapshot) {
  assert(shared);
  assert(snapshot);

  pthread_mutex_lock(&shared->state_mutex);

  snapshot->stop_requested = shared->stop_requested;
  snapshot->fatal_error = shared->fatal_error;
  snapshot->solved_published = shared->solved_published;
  snapshot->master_committed = shared->master_committed;

  pthread_mutex_unlock(&shared->state_mutex);
}

/*
 * Snapshot completed-pass metrics.
 *
 * This function is called only after index_shard_pool_reduce_online() has
 * returned and all participating workers have left the pass.
 */
static double index_shard_timeval_delta_seconds(
    const struct timeval *finish,
    const struct timeval *start) {
  double seconds;

  assert(finish);
  assert(start);

  seconds =
      (double)(finish->tv_sec - start->tv_sec) +
      ((double)(finish->tv_usec - start->tv_usec) / 1000000.0);

  if (seconds < 0.0) {
    return 0.0;
  }

  return seconds;
}

static long index_shard_nonnegative_long_delta(long finish,
                                                long start) {
  if (finish < start) {
    return 0;
  }

  return finish - start;
}

/*
 * Snapshot completed-pass timing and process resource usage.
 *
 * The rusage counters cover the complete process while the pthread pass is
 * active. They are suitable for pass-level attribution but deliberately not
 * treated as exclusive per-task measurements.
 */
static void index_shard_pass_metrics_snapshot(
    index_shard_thread_state_t *shared,
    index_shard_pass_metrics_snapshot_t *snapshot) {
  struct rusage finish;

  assert(shared);
  assert(snapshot);

  memset(snapshot, 0, sizeof(*snapshot));

  snapshot->reduced = shared->next_reduce;
  snapshot->wall_seconds =
      timenow() - shared->pass_wall_start;
  snapshot->cpu_seconds =
      get_cpu_usage() - shared->pass_cpu_start;

  if (snapshot->wall_seconds > 0.0) {
    snapshot->cpu_percent =
        (100.0 * (double)snapshot->cpu_seconds) /
        snapshot->wall_seconds;
  }

  if (!shared->pass_rusage_valid ||
      getrusage(RUSAGE_SELF, &finish)) {
    return;
  }

  snapshot->resource_available = TRUE;

  snapshot->user_seconds =
      index_shard_timeval_delta_seconds(
          &finish.ru_utime,
          &shared->pass_rusage_start.ru_utime);

  snapshot->system_seconds =
      index_shard_timeval_delta_seconds(
          &finish.ru_stime,
          &shared->pass_rusage_start.ru_stime);

  snapshot->minor_faults =
      index_shard_nonnegative_long_delta(
          finish.ru_minflt,
          shared->pass_rusage_start.ru_minflt);

  snapshot->major_faults =
      index_shard_nonnegative_long_delta(
          finish.ru_majflt,
          shared->pass_rusage_start.ru_majflt);

  snapshot->voluntary_context_switches =
      index_shard_nonnegative_long_delta(
          finish.ru_nvcsw,
          shared->pass_rusage_start.ru_nvcsw);

  snapshot->involuntary_context_switches =
      index_shard_nonnegative_long_delta(
          finish.ru_nivcsw,
          shared->pass_rusage_start.ru_nivcsw);
}
/*
 * Compare task durations for percentile calculation.
 */
static int index_shard_compare_double(const void *left,
                                      const void *right) {
  const double lhs = *(const double *)left;
  const double rhs = *(const double *)right;

  if (lhs < rhs) {
    return -1;
  }

  if (lhs > rhs) {
    return 1;
  }

  return 0;
}

/*
 * Return the zero-based nearest-rank percentile index.
 *
 * The calculation avoids multiplying the full sample count by the percentile,
 * so it remains safe for large size_t values.
 */
static size_t index_shard_percentile_index(size_t count,
                                           unsigned int percentile) {
  size_t rank;
  size_t whole;
  size_t remainder;

  assert(count > 0);
  assert(percentile >= 1);
  assert(percentile <= 100);

  whole = (count / 100) * percentile;
  remainder = (count % 100) * percentile;
  rank = whole + ((remainder + 99) / 100);

  if (rank == 0) {
    rank = 1;
  }

  if (rank > count) {
    rank = count;
  }

  return rank - 1;
}

/*
 * Build a completed-pass profile from immutable worker result slots.
 *
 * This runs only after index_shard_pool_reduce_online() has waited for every
 * participating worker to leave the pass. No task modifies result storage at
 * this point.
 *
 * The terminal serial tail is the interval after every other measured task
 * has finished while the latest-finishing task is still running. If that task
 * started after the second-latest completion, its own start time is used as
 * the lower bound.
 */
static void index_shard_task_profile_snapshot(
    const index_shard_result_t *results,
    size_t nresults,
    double pool_wall_seconds,
    index_shard_task_profile_snapshot_t *snapshot) {
  double *durations = NULL;
  size_t sample_count = 0;
  size_t i;

  anbool have_max = FALSE;
  anbool have_latest = FALSE;
  anbool have_second_latest = FALSE;

  double latest_finish = 0.0;
  double latest_start = 0.0;
  double second_latest_finish = 0.0;

  memset(snapshot, 0, sizeof(*snapshot));

  snapshot->max_worker_id = -1;
  snapshot->tail_worker_id = -1;

  if (!results || !nresults) {
    return;
  }

  if (nresults <= ((size_t)-1) / sizeof(*durations)) {
    durations = malloc(nresults * sizeof(*durations));
  }

  for (i = 0; i < nresults; i++) {
    const index_shard_result_t *result = &results[i];

    if (!result->task_started) {
      continue;
    }

    if (!isfinite(result->task_wall_seconds) ||
        !isfinite(result->task_start_since_pass) ||
        !isfinite(result->task_finish_since_pass) ||
        result->task_wall_seconds < 0.0 ||
        result->task_finish_since_pass < result->task_start_since_pass) {
      continue;
    }

    snapshot->executed++;

    if (durations) {
      durations[sample_count++] = result->task_wall_seconds;
    }

    if (!have_max ||
        result->task_wall_seconds > snapshot->task_max_seconds) {
      have_max = TRUE;

      snapshot->task_max_seconds = result->task_wall_seconds;
      snapshot->max_solve_seconds = result->wall_seconds;
      snapshot->max_index_order = result->index_order;
      snapshot->max_worker_id = result->worker_id;
    }

    if (!have_latest ||
        result->task_finish_since_pass > latest_finish) {
      if (have_latest) {
        second_latest_finish = latest_finish;
        have_second_latest = TRUE;
      }

      have_latest = TRUE;
      latest_finish = result->task_finish_since_pass;
      latest_start = result->task_start_since_pass;

      snapshot->tail_index_order = result->index_order;
      snapshot->tail_worker_id = result->worker_id;
    } else if (!have_second_latest ||
               result->task_finish_since_pass > second_latest_finish) {
      second_latest_finish = result->task_finish_since_pass;
      have_second_latest = TRUE;
    }
  }

  if (durations &&
      sample_count > 0 &&
      sample_count == snapshot->executed) {
    qsort(durations,
          sample_count,
          sizeof(*durations),
          index_shard_compare_double);

    snapshot->quantiles_available = TRUE;

    snapshot->task_p50_seconds =
        durations[index_shard_percentile_index(sample_count, 50)];

    snapshot->task_p90_seconds =
        durations[index_shard_percentile_index(sample_count, 90)];

    snapshot->task_p99_seconds =
        durations[index_shard_percentile_index(sample_count, 99)];

    if (snapshot->task_p50_seconds > 0.0) {
      snapshot->max_to_p50 =
          snapshot->task_max_seconds /
          snapshot->task_p50_seconds;
    }
  }

  if (pool_wall_seconds > 0.0) {
    snapshot->max_pool_percent =
        (100.0 * snapshot->task_max_seconds) /
        pool_wall_seconds;
  }

  if (have_latest) {
    double tail_start = latest_start;

    if (have_second_latest &&
        second_latest_finish > tail_start) {
      tail_start = second_latest_finish;
    }

    if (latest_finish > tail_start) {
      snapshot->serial_tail_seconds =
          latest_finish - tail_start;
    }

    if (pool_wall_seconds > 0.0) {
      snapshot->serial_tail_percent =
          (100.0 * snapshot->serial_tail_seconds) /
          pool_wall_seconds;
    }
  }

  free(durations);
}

/*
 * Attribute completed outer-task wall time to reset, index acquisition,
 * solving, result analysis, and index release.
 *
 * Phase totals are sums of per-task wall durations and may exceed pool wall
 * time because pthread workers execute concurrently. Percentages are therefore
 * relative to summed task wall, not elapsed pass wall.
 */
static void index_shard_phase_profile_snapshot(
    const index_shard_result_t *results,
    size_t nresults,
    index_shard_phase_profile_snapshot_t *snapshot) {
  double *acquire_durations = NULL;
  double *solve_durations = NULL;
  size_t sample_count = 0;
  size_t i;
  double measured_total;

  memset(snapshot, 0, sizeof(*snapshot));

  if (!results || !nresults) {
    return;
  }

  if (nresults <= ((size_t)-1) / sizeof(*acquire_durations)) {
    acquire_durations =
        malloc(nresults * sizeof(*acquire_durations));

    solve_durations =
        malloc(nresults * sizeof(*solve_durations));
  }

  if (!acquire_durations || !solve_durations) {
    free(acquire_durations);
    free(solve_durations);

    acquire_durations = NULL;
    solve_durations = NULL;
  }

  for (i = 0; i < nresults; i++) {
    const index_shard_result_t *result = &results[i];

    if (!result->task_started ||
        !isfinite(result->task_wall_seconds) ||
        result->task_wall_seconds < 0.0) {
      continue;
    }

    snapshot->executed++;
    snapshot->task_wall_total += result->task_wall_seconds;

    if (isfinite(result->reset_seconds) &&
        result->reset_seconds >= 0.0) {
      snapshot->reset_total += result->reset_seconds;
    }

    if (isfinite(result->acquire_seconds) &&
        result->acquire_seconds >= 0.0) {
      snapshot->acquire_total += result->acquire_seconds;
    }

    if (isfinite(result->wall_seconds) &&
        result->wall_seconds >= 0.0) {
      snapshot->solve_total += result->wall_seconds;
    }

    if (isfinite(result->analyze_seconds) &&
        result->analyze_seconds >= 0.0) {
      snapshot->analyze_total += result->analyze_seconds;
    }

    if (isfinite(result->release_seconds) &&
        result->release_seconds >= 0.0) {
      snapshot->release_total += result->release_seconds;
    }

    if (acquire_durations &&
        solve_durations &&
        isfinite(result->acquire_seconds) &&
        result->acquire_seconds >= 0.0 &&
        isfinite(result->wall_seconds) &&
        result->wall_seconds >= 0.0) {
      acquire_durations[sample_count] =
          result->acquire_seconds;

      solve_durations[sample_count] =
          result->wall_seconds;

      sample_count++;
    }
  }

  measured_total =
      snapshot->reset_total +
      snapshot->acquire_total +
      snapshot->solve_total +
      snapshot->analyze_total +
      snapshot->release_total;

  if (snapshot->task_wall_total > measured_total) {
    snapshot->other_total =
        snapshot->task_wall_total - measured_total;
  }

  if (snapshot->task_wall_total > 0.0) {
    snapshot->reset_percent =
        (100.0 * snapshot->reset_total) /
        snapshot->task_wall_total;

    snapshot->acquire_percent =
        (100.0 * snapshot->acquire_total) /
        snapshot->task_wall_total;

    snapshot->solve_percent =
        (100.0 * snapshot->solve_total) /
        snapshot->task_wall_total;

    snapshot->analyze_percent =
        (100.0 * snapshot->analyze_total) /
        snapshot->task_wall_total;

    snapshot->release_percent =
        (100.0 * snapshot->release_total) /
        snapshot->task_wall_total;

    snapshot->other_percent =
        (100.0 * snapshot->other_total) /
        snapshot->task_wall_total;
  }

  if (acquire_durations &&
      solve_durations &&
      sample_count > 0 &&
      sample_count == snapshot->executed) {
    qsort(acquire_durations,
          sample_count,
          sizeof(*acquire_durations),
          index_shard_compare_double);

    qsort(solve_durations,
          sample_count,
          sizeof(*solve_durations),
          index_shard_compare_double);

    snapshot->quantiles_available = TRUE;

    snapshot->acquire_p50 =
        acquire_durations[
            index_shard_percentile_index(sample_count, 50)];

    snapshot->acquire_p90 =
        acquire_durations[
            index_shard_percentile_index(sample_count, 90)];

    snapshot->acquire_p99 =
        acquire_durations[
            index_shard_percentile_index(sample_count, 99)];

    snapshot->acquire_max =
        acquire_durations[sample_count - 1];

    snapshot->solve_p50 =
        solve_durations[
            index_shard_percentile_index(sample_count, 50)];

    snapshot->solve_p90 =
        solve_durations[
            index_shard_percentile_index(sample_count, 90)];

    snapshot->solve_p99 =
        solve_durations[
            index_shard_percentile_index(sample_count, 99)];

    snapshot->solve_max =
        solve_durations[sample_count - 1];
  }

  free(acquire_durations);
  free(solve_durations);
}
/*
 * Take one synchronized snapshot of auxiliary queue accounting.
 *
 * submitted counts accepted tasks only. Rejected tasks are therefore
 * deliberately excluded from the accepted-task completion equation.
 */
static void index_shard_aux_metrics_snapshot(
    index_shard_aux_queue_t *queue,
    index_shard_aux_metrics_snapshot_t *snapshot) {
  assert(queue);
  assert(snapshot);

  pthread_mutex_lock(&queue->mutex);

  snapshot->submitted = queue->submitted_total;
  snapshot->executed = queue->executed_total;
  snapshot->rejected = queue->rejected_total;
  snapshot->cancelled = queue->cancelled_total;
  snapshot->pending = queue->pending;
  snapshot->max_pending = queue->max_pending;

  pthread_mutex_unlock(&queue->mutex);
}

// ANCHOR INDEX-SHARD: wake-pass-waiters
static void index_shard_wake_pass_waiters(index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->queue_mutex);
  pthread_cond_broadcast(&shared->queue_cv);
  pthread_mutex_unlock(&shared->queue_mutex);

  pthread_mutex_lock(&shared->result_mutex);
  pthread_cond_broadcast(&shared->result_cv);
  pthread_mutex_unlock(&shared->result_mutex);
}

// ANCHOR INDEX-SHARD: request-stop
/*
 * Cooperative global stop.
 *
 * Stop means:
 *   - workers should not claim new tasks
 *   - reducer should wake and merge any solved/completed result
 *   - already-running solver calls must exit through callback polling
 *
 * This is intentionally not pthread_cancel.
 */
static void index_shard_request_stop(index_shard_thread_state_t *shared) {
  int was_stopped;

  pthread_mutex_lock(&shared->state_mutex);
  was_stopped = shared->stop_requested;
  shared->stop_requested = TRUE;
  pthread_mutex_unlock(&shared->state_mutex);

  if (!was_stopped && index_shard_trace_enabled()) {
    logmsg("[index-shard] stop-request pass_wall=%.3f\n",
           timenow() - shared->pass_wall_start);
  }

  index_shard_wake_pass_waiters(shared);
}

// ANCHOR INDEX-SHARD: request-fatal-stop
/*
 * Publish a hard worker/module failure and stop the current pass.
 */
static void index_shard_request_fatal_stop(index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->state_mutex);
  shared->fatal_error = TRUE;
  shared->stop_requested = TRUE;
  pthread_mutex_unlock(&shared->state_mutex);

  index_shard_wake_pass_waiters(shared);
}

// ANCHOR INDEX-SHARD: publish-committed-solve
/*
 * Publish that the reducer committed a valid solved result.
 *
 * Quick commit is intentional project policy. Once a solved result becomes
 * master-visible, no new shard tasks should be claimed.
 */
static void index_shard_publish_committed_solve(
    index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->state_mutex);
  shared->solved_published = TRUE;
  shared->stop_requested = TRUE;
  pthread_mutex_unlock(&shared->state_mutex);

  index_shard_wake_pass_waiters(shared);
}
/*
 * Mark the point after which serial fallback is no longer safe.
 *
 * The merge hook is not transactional.  Once it begins transferring a
 * non-empty worker result into master-visible state, a later failure must not
 * cause the caller to rerun the original serial pass.
 */
static void index_shard_mark_master_committed(
    index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->state_mutex);
  shared->master_committed = TRUE;
  pthread_mutex_unlock(&shared->state_mutex);
}

// ANCHOR INDEX-SHARD: master-limit-state
static int index_shard_master_limit_or_cancel_requested(index_shard_thread_state_t *shared,
                                                        anbool *hit_total_cpulimit,
                                                        anbool *cancelled) {
  onefield_t *bp = shared->bp;
  int stop;

  pthread_mutex_lock(&shared->limit_mutex);

  if (hit_total_cpulimit) {
    *hit_total_cpulimit = bp->hit_total_cpulimit;
  }

  if (cancelled) {
    *cancelled = bp->cancelled;
  }

  stop = bp->hit_total_cpulimit || bp->hit_total_timelimit || bp->cancelled;

  pthread_mutex_unlock(&shared->limit_mutex);

  return stop;
}

// ANCHOR INDEX-SHARD: master-stop-check
/*
 * Read-only stop predicate used by workers before expensive work.
 *
 * Solver completion is mirrored through solved_published.  Workers therefore
 * do not read master bp->single_field_solved concurrently with the reducer.
 */
static int index_shard_master_stop_requested(index_shard_thread_state_t *shared) {
  index_shard_pass_state_snapshot_t state;

  index_shard_pass_state_snapshot(shared, &state);

  if (state.stop_requested || state.fatal_error || state.solved_published) {
    return TRUE;
  }

  return index_shard_master_limit_or_cancel_requested(shared, NULL, NULL);
}

// ANCHOR INDEX-SHARD: global-limits
/*
 * Process-wide elapsed-time and CPU-budget checks.
 *
 * total_timelimit is one shared monotonic wall-clock deadline and is not
 * divided by worker count. total_cpulimit is aggregate process CPU time; with
 * N active threads it can be consumed roughly N times faster than wall time.
 */
static int index_shard_check_global_limits(index_shard_thread_state_t *shared) {
  onefield_t *bp = shared->bp;
  index_shard_pass_state_snapshot_t state;
  int hit = FALSE;

  index_shard_pass_state_snapshot(shared, &state);

  if (state.stop_requested || state.fatal_error || state.solved_published) {
    return TRUE;
  }

  pthread_mutex_lock(&shared->limit_mutex);

  if (bp->cancelled || bp->hit_total_cpulimit || bp->hit_total_timelimit) {
    hit = TRUE;
  } else {
    if (bp->total_timelimit > 0.0) {
      double now = monotonic_seconds();

      if (now >= 0.0 &&
          now - bp->time_total_start >= bp->total_timelimit) {
        bp->hit_total_timelimit = TRUE;
        hit = TRUE;

        if (!shared->limit_reported) {
          shared->limit_reported = TRUE;
          logmsg("Total wall-clock time limit reached!\n");
          logmsg("[index-shard] wall-limit reached total_timelimit=%g "
                 "elapsed=%.3f\n",
                 bp->total_timelimit,
                 now - bp->time_total_start);
        }
      }
    }

    if (!hit && bp->total_cpulimit > 0.0) {
      float now = get_cpu_usage();
      double elapsed = (double)(now - bp->cpu_total_start);

      if (elapsed >= bp->total_cpulimit) {
        bp->hit_total_cpulimit = TRUE;
        hit = TRUE;

        if (!shared->limit_reported) {
          shared->limit_reported = TRUE;
          logmsg("Total CPU time limit reached!\n");
          logmsg("[index-shard] cpu-budget reached total_cpulimit=%g "
                 "elapsed=%.3f\n",
                 bp->total_cpulimit,
                 elapsed);
        }
      }
    }
  }

  pthread_mutex_unlock(&shared->limit_mutex);

  if (hit) {
    index_shard_request_stop(shared);
  }

  return hit;
}

// ANCHOR INDEX-SHARD: callback-poll
/*
 * Called from onefield callbacks/timer paths while solver_run() is active.
 *
 * This is the fast-stop path for workers already inside solver code.
 * It maps global stop -> local solver.quit_now.
 */
void index_shard_poll_from_callback(onefield_t *bp) {
  // non-worker callback, nothing to do
  // local solver should unwind normally through existing quit path
  index_shard_worker_context_t *ctx = index_shard_get_tls();

  if (!ctx || !ctx->pool)
    return;

  if (index_shard_check_global_limits(&ctx->pool->shared)) {
    bp->solver.quit_now = TRUE;
    return;
  }

  if (index_shard_master_stop_requested(&ctx->pool->shared))
    bp->solver.quit_now = TRUE;
}

/*
 * SECTION INDEX-SHARD: result
 */

// ANCHOR INDEX-SHARD: find-completed-solved-locked
static ssize_t index_shard_find_completed_solved_locked(index_shard_thread_state_t *shared) {
  size_t i;

  /*
   * Caller must hold shared->result_mutex.
   * Completed result slots are immutable after completed[index_order] = TRUE.
   */
  for (i = 0; i < shared->nindexes; i++) {
    if (!shared->completed[i])
      continue;

    if (shared->results[i].merged)
      continue;

    if (shared->results[i].solved)
      return (ssize_t)i;
  }

  return -1;
}
// ANCHOR INDEX-SHARD: result-init
/*
 * Initialize one result slot before a worker starts solving an index.
 *
 * best_logodds starts at -HUGE_VAL so diagnostics can distinguish "no match"
 * from a real low-confidence match.
 */
static void index_shard_result_init(index_shard_result_t *result, size_t index_order) {
  memset(result, 0, sizeof(index_shard_result_t));

  result->index_order = index_order;
  result->best_logodds = -HUGE_VAL;
  result->best_fieldnum = -1;

  result->solutions = bl_new(4, sizeof(MatchObj));
}

static void index_shard_result_dispose(index_shard_result_t *result,
                                       const index_shard_hooks_t *hooks) {
  // reducer already transferred ownership to master bp
  if (!result || !result->solutions)
    return;

  if (result->merged) {
    bl_free(result->solutions);
    result->solutions = NULL;
    return;
  }

  if (hooks && hooks->free_solutions) {
    hooks->free_solutions(result->solutions);
    result->solutions = NULL;
    return;
  }

  bl_free(result->solutions);
  result->solutions = NULL;
}

/*
 * Close the full outer-task timing interval.
 *
 * One timenow() call is used for both task duration and position within the
 * pass, keeping the hot-path instrumentation minimal.
 */
static void index_shard_result_finish_task(
    index_shard_result_t *result,
    const index_shard_thread_state_t *shared,
    double task_wall_start) {
  double task_wall_finish;

  assert(result);
  assert(shared);

  task_wall_finish = timenow();

  result->task_wall_seconds =
      task_wall_finish - task_wall_start;

  result->task_finish_since_pass =
      task_wall_finish - shared->pass_wall_start;
}

// ANCHOR INDEX-SHARD: analyze-result
/*
 * Inspect worker-local solutions without merging them.
 *
 * This marks solved/best_logodds early so the worker can request global stop
 * before the ordered reducer reaches this slot.
 */
static void index_shard_capture_solution_analysis(index_shard_thread_state_t *shared,
                                                  index_shard_result_t *result) {
  if (!shared->hooks || !shared->hooks->analyze_solutions)
    return;

  result->solved = shared->hooks->analyze_solutions(shared->bp, result->solutions,
                                                    &result->best_logodds, &result->best_fieldnum);

  result->has_accepted_solution = result->solved;
}
// ANCHOR INDEX-SHARD: reduce-one-result
/*
 * Transfer one completed worker result into master onefield state.
 *
 * Only the reducer calls this.  Workers never append directly into
 * master_bp->solutions.
 *
 * The merge hook is not transactional.  Before transferring a non-empty
 * worker result, mark the pass as master-committed so a later failure cannot
 * trigger an unsafe serial rerun.
 */
static int index_shard_reduce_one_result(index_shard_thread_state_t *shared,
                                         index_shard_result_t *result) {
  anbool solved = FALSE;
  int may_mutate_master = FALSE;

  if (!result || result->merged) {
    return 0;
  }

  if (!shared->hooks || !shared->hooks->merge_solutions) {
    result->failed = TRUE;
    result->rc = -1;
    return -1;
  }

  if (result->solutions && bl_size(result->solutions) > 0) {
    may_mutate_master = TRUE;
  }

  if (result->solved) {
    may_mutate_master = TRUE;
  }

  if (may_mutate_master) {
    index_shard_mark_master_committed(shared);
  }

  if (shared->hooks->merge_solutions(shared->bp,
                                     result->solutions,
                                     &solved)) {
    result->failed = TRUE;
    result->rc = -1;
    return -1;
  }

  result->merged = TRUE;

  if (solved || result->solved) {
    result->solved = TRUE;
    shared->bp->single_field_solved = TRUE;
    index_shard_publish_committed_solve(shared);
  }

  return 0;
}
// ANCHOR INDEX-SHARD: find-completed-solved-locked
/*
 * Find any completed solved result, including out-of-order results.
 *
 * Caller must hold result_mutex.  Used by fast solved-stop so the reducer does
 * not wait for the ordered prefix when a later index already solved.
 */
static ssize_t index_shard_find_completed_solved(index_shard_thread_state_t *shared) {
  size_t i;

  for (i = 0; i < shared->nindexes; i++) {
    if (!shared->completed[i])
      continue;

    if (shared->results[i].merged)
      continue;

    if (shared->results[i].solved)
      return (ssize_t)i;
  }

  return -1;
}

// ANCHOR INDEX-SHARD: worker-get-index
/*
 * Load one index for one shard task through original onefield hooks.
 *
 * No persistent index_t cache here.  Full-index caching caused unacceptable
 * RSS growth because candidate sets can contain hundreds of heavy indexes.
 */
static index_t *index_shard_worker_get_index(index_shard_worker_context_t *ctx,
                                             index_shard_thread_state_t *shared,
                                             size_t index_order) {
  // original onefield ownership path, released after task
  index_t *index;

  if (!shared->hooks || !shared->hooks->get_index)
    return NULL;

  index = shared->hooks->get_index(shared->bp, index_order);

  if (index_shard_trace_enabled() && index) {
    logmsg("[index-shard] worker=%i load index_order=%zu index=%s\n", ctx->worker_id, index_order,
           index->indexname ? index->indexname : "(null)");
  }

  return index;
}

static void index_shard_apply_index_mmap_advice(
    index_t* index,
    fitsbin_mmap_advice_t advice) {
    fitsbin_t* fb;

    if (!index) {
        return;
    }

    /*
     * Code KD tree.
     *
     * kdtree_fits_t is a typedef of fitsbin_t, and kdtree_t::io owns the
     * corresponding FITS mapping.
     */
    if (index->codekd &&
        index->codekd->tree &&
        index->codekd->tree->io) {
        fb = (fitsbin_t*)index->codekd->tree->io;

        (void)fitsbin_set_mmap_advice(
            fb,
            advice,
            TRUE);
    }

    /*
     * Star KD tree.
     */
    if (index->starkd &&
        index->starkd->tree &&
        index->starkd->tree->io) {
        fb = (fitsbin_t*)index->starkd->tree->io;

        (void)fitsbin_set_mmap_advice(
            fb,
            advice,
            TRUE);
    }

    /*
     * Quad table.
     */
    if (index->quads &&
        index->quads->fb) {
        (void)fitsbin_set_mmap_advice(
            index->quads->fb,
            advice,
            TRUE);
    }
}

/*
 * SECTION INDEX-SHARD: queue
 */
// ANCHOR INDEX-SHARD: build-task-plan
/*
 * Return true only for a standard unsplit Astrometry.net index name.
 *
 * A split index has a second dash followed by its HEALPix number.  Such a file
 * covers one sky region, so trying one arbitrary member cannot discover the
 * usefulness of the whole scale family.  Keep split and unknown names in
 * their original order.  Only an index-<digits> basename is genuinely
 * suitable for the all-sky discovery lane.
 */
static anbool index_shard_task_is_all_sky(const char *index_name) {
  const char *base;
  const char *number;

  if (!index_name || !index_name[0]) {
    return FALSE;
  }

  base = strrchr(index_name, '/');
  base = base ? base + 1 : index_name;

  if (strncmp(base, "index-", 6)) {
    return FALSE;
  }

  number = base + 6;

  if (*number < '0' || *number > '9') {
    return FALSE;
  }

  do {
    number++;
  } while (*number >= '0' && *number <= '9');

  return (*number == '\0' || *number == '.');
}

static void index_shard_build_ordered_task_plan(
    index_shard_task_t *tasks,
    size_t nindexes) {
  size_t i;

  assert(tasks || !nindexes);

  for (i = 0; i < nindexes; i++) {
    tasks[i].index_order = i;
    tasks[i].discovery_frontier = FALSE;
  }
}

/*
 * Build the one-index task plan for the current pass.
 *
 * The production plan has two immutable lanes.  Genuinely all-sky indexes
 * form a compact discovery lane; split and unknown indexes form an ordered
 * lane.  Claiming interleaves the lanes without dropping, duplicating, or
 * modifying solver work.  Result storage and reduction remain indexed by the
 * original candidate order.
 */
static index_shard_task_t *index_shard_build_task_plan(
    onefield_t *bp,
    size_t nindexes,
    const index_shard_hooks_t *hooks,
    size_t *family_count_out,
    size_t *frontier_count_out) {
  index_shard_task_t *tasks;
  unsigned char *in_frontier = NULL;
  size_t frontier_count = 0;
  size_t i;

  if (family_count_out) {
    *family_count_out = 0;
  }

  if (frontier_count_out) {
    *frontier_count_out = 0;
  }

  if (!nindexes) {
    return NULL;
  }

  tasks = calloc(nindexes, sizeof(index_shard_task_t));
  if (!tasks) {
    SYSERROR("Failed to allocate index-shard task plan");
    return NULL;
  }

  index_shard_build_ordered_task_plan(tasks, nindexes);

  if (!index_shard_config_get()->discovery_frontier_enabled ||
      !hooks ||
      !hooks->get_index_name) {
    return tasks;
  }

  in_frontier = calloc(nindexes, sizeof(unsigned char));

  if (!in_frontier) {
    logmsg("[index-shard] discovery-frontier allocation failed; "
           "using ordered task plan\n");
    return tasks;
  }

  for (i = 0; i < nindexes; i++) {
    const char *index_name = hooks->get_index_name(bp, i);

    if (index_shard_task_is_all_sky(index_name)) {
      tasks[frontier_count].index_order = i;
      tasks[frontier_count].discovery_frontier = TRUE;
      frontier_count++;
      in_frontier[i] = TRUE;
    }
  }

  {
    size_t task_index = frontier_count;

    for (i = 0; i < nindexes; i++) {
      if (!in_frontier[i]) {
        tasks[task_index].index_order = i;
        tasks[task_index].discovery_frontier = FALSE;
        task_index++;
      }
    }

    assert(task_index == nindexes);
  }

  if (family_count_out) {
    *family_count_out = frontier_count;
  }

  if (frontier_count_out) {
    *frontier_count_out = frontier_count;
  }

  free(in_frontier);

  return tasks;
}
// ANCHOR INDEX-SHARD: claim-one
/*
 * Claim one shard task.
 *
 * Invariant:
 *   - each index_order is claimed at most once
 *   - running_tasks increments before returning a task
 *   - stop/fatal prevents new claims
 *
 * active_limit is a concurrency cap, not a ramp scheduler.
 */
static int index_shard_claim_one(index_shard_thread_state_t *shared,
                                 size_t *index_order,
                                 anbool *frontier_task) {
  index_shard_pass_state_snapshot_t state;
  anbool frontier_available;
  anbool ordered_available;
  anbool claim_frontier = FALSE;
  index_shard_task_t *task;

  assert(index_order);
  assert(frontier_task);

  pthread_mutex_lock(&shared->queue_mutex);

  while (shared->running_tasks >= shared->active_limit) {
    index_shard_pass_state_snapshot(shared, &state);

    if (state.stop_requested || state.fatal_error || state.solved_published) {
      pthread_mutex_unlock(&shared->queue_mutex);
      return FALSE;
    }

    pthread_cond_wait(&shared->queue_cv, &shared->queue_mutex);
  }

  index_shard_pass_state_snapshot(shared, &state);

  if (state.stop_requested || state.fatal_error || state.solved_published) {
    pthread_mutex_unlock(&shared->queue_mutex);
    return FALSE;
  }

  if (shared->next_task >= shared->ntasks) {
    pthread_mutex_unlock(&shared->queue_mutex);
    return FALSE;
  }

  if (shared->have_solved_order) {
    pthread_mutex_unlock(&shared->queue_mutex);
    return FALSE;
  }

  frontier_available =
      (shared->next_frontier_task < shared->task_frontier_count);
  ordered_available = (shared->next_ordered_task < shared->ntasks);

  if (frontier_available) {
    if (!ordered_available) {
      claim_frontier = TRUE;
    } else if (shared->worker_count == 1) {
      if (shared->next_frontier_task == 0 ||
          shared->ordered_since_frontier >=
              INDEX_SHARD_SINGLE_WORKER_ORDERED_QUANTUM) {
        claim_frontier = TRUE;
      }
    } else if (shared->frontier_running <
               INDEX_SHARD_DISCOVERY_FRONTIER_LIMIT) {
      claim_frontier = TRUE;
    }
  }

  if (claim_frontier) {
    task = &shared->tasks[shared->next_frontier_task];
    shared->next_frontier_task++;
    shared->frontier_running++;
    shared->ordered_since_frontier = 0;
  } else if (ordered_available) {
    task = &shared->tasks[shared->next_ordered_task];
    shared->next_ordered_task++;

    if (shared->worker_count == 1 && frontier_available) {
      shared->ordered_since_frontier++;
    }
  } else {
    pthread_mutex_unlock(&shared->queue_mutex);
    return FALSE;
  }

  *index_order = task->index_order;
  *frontier_task = task->discovery_frontier;
  shared->next_task++;
  shared->running_tasks++;

  if (index_shard_trace_enabled()) {
    logmsg("[index-shard] claim index_order=%zu lane=%s running=%i "
           "frontier_running=%i active_limit=%i wall_since_pass=%.3f\n",
           *index_order,
           *frontier_task ? "all-sky" : "ordered",
           shared->running_tasks,
           shared->frontier_running,
           shared->active_limit,
           timenow() - shared->pass_wall_start);
  }

  pthread_mutex_unlock(&shared->queue_mutex);
  return TRUE;
}
// ANCHOR INDEX-SHARD: release-credit
/*
 * Release one in-flight task credit.
 *
 * Must be called exactly once for each successful claim_one().
 */
static void index_shard_release_active_credit(index_shard_thread_state_t *shared,
                                              anbool frontier_task) {
  // release queue credit before publishing completion
  // completed flag is the reducer visibility boundary
  pthread_mutex_lock(&shared->queue_mutex);

  if (shared->running_tasks > 0) {
    shared->running_tasks--;
  }

  if (frontier_task) {
    assert(shared->frontier_running > 0);
    shared->frontier_running--;
  }

  pthread_cond_broadcast(&shared->queue_cv);
  pthread_mutex_unlock(&shared->queue_mutex);
}

static void index_shard_mark_result_completed(index_shard_thread_state_t *shared,
                                              size_t index_order,
                                              anbool frontier_task) {
  /*
   * NOTE INDEX-SHARD: claimed-task-invariant
   *
   * Every claimed task must release its active credit exactly once and mark
   * completion exactly once.  Otherwise the reducer can wait forever.
   */
  index_shard_release_active_credit(shared, frontier_task);

  pthread_mutex_lock(&shared->result_mutex);
  shared->completed[index_order] = TRUE;
  pthread_cond_broadcast(&shared->result_cv);
  pthread_mutex_unlock(&shared->result_mutex);
}

// ANCHOR INDEX-SHARD: publish-solved
/*
 * Publish solved frontier.
 *
 * This prevents workers from claiming later index orders once any solved
 * candidate is known.  The actual solution merge still belongs to the reducer.
 */
static void index_shard_publish_solved(index_shard_thread_state_t *shared, size_t index_order) {
  pthread_mutex_lock(&shared->queue_mutex);

  if (!shared->have_solved_order || index_order < shared->earliest_solved_order) {
    shared->have_solved_order = TRUE;
    shared->earliest_solved_order = index_order;
  }

  pthread_cond_broadcast(&shared->queue_cv);
  pthread_mutex_unlock(&shared->queue_mutex);
}
/*
 * SECTION INDEX-SHARD: worker-context
 *
 * Worker-local context reuse.
 *
 * local_bp/local solver are prepared once per submitted pass and reused across
 * many one-index tasks.  This removes repeated xylist open/close and solver
 * index-list allocation from the per-index hot path.
 */
static int index_shard_worker_prepare_pass(index_shard_worker_context_t *ctx,
                                           index_shard_thread_state_t *shared) {
  // old local context belongs to a previous generation -> cleanup first
  if (ctx->local_context_ready && ctx->local_context_generation == ctx->generation_seen)
    return 0;

  if (ctx->local_context_ready) {
    if (shared->hooks && shared->hooks->cleanup_local_context)
      shared->hooks->cleanup_local_context(&ctx->local_bp);

    ctx->local_context_ready = FALSE;
  }

  if (!shared->hooks || !shared->hooks->prepare_local_context)
    return -1;

  if (shared->hooks->prepare_local_context(&ctx->local_bp, shared->bp, shared->base_sp))
    return -1;
  // hook copies stable master config + opens worker-local xylist
  ctx->local_context_ready = TRUE;
  ctx->local_context_generation = ctx->generation_seen;

  return 0;
}
// ANCHOR INDEX-SHARD: worker-cleanup-pass
/*
 * Release worker-local pass context after this generation is finished.
 *
 * Does not touch master bp and does not free result slots.
 */
static void index_shard_worker_cleanup_pass(index_shard_worker_context_t *ctx,
                                            index_shard_thread_state_t *shared) {
  if (!ctx->local_context_ready)
    return;

  if (shared->hooks && shared->hooks->cleanup_local_context)
    shared->hooks->cleanup_local_context(&ctx->local_bp);

  memset(&ctx->local_bp, 0, sizeof(onefield_t));
  ctx->local_context_ready = FALSE;
  ctx->local_context_generation = 0;
}

// ANCHOR INDEX-SHARD: run-one-index
/*
 * Execute one index shard in one worker.
 *
 * This function owns only local computation:
 *   - reset local context for this result slot
 *   - load one index
 *   - run solver against local_bp
 *   - analyze local solutions
 *   - release index
 *
 * It does not merge into master bp.
 */
static int index_shard_run_one_with_worker_context(index_shard_worker_context_t *ctx,
                                                   index_shard_thread_state_t *shared,
                                                   size_t index_order,
                                                   index_shard_result_t *result) {
  // one result slot belongs to this task
  index_t *index = NULL;
  double task_wall_start;
  double phase_wall_start;
  double wall_start;
  float cpu_start;
  int rc = 0;
  int cached_index = FALSE;

  index_shard_result_init(result, index_order);

  if (!result->solutions) {
    result->failed = TRUE;
    result->rc = -1;
    return -1;
  }

  if (!ctx->local_context_ready) {
    result->failed = TRUE;
    result->rc = -1;
    return -1;
  }

  if (!shared->hooks || !shared->hooks->reset_local_context_for_task ||
      !shared->hooks->solve_one_index) {
    result->failed = TRUE;
    result->rc = -1;
    return -1;
  }

  /*
   * Full outer-task timing starts before local reset and index acquisition.
   * Queue wait and reducer wait are deliberately excluded.
   */
  task_wall_start = timenow();

  result->task_started = TRUE;
  result->worker_id = ctx->worker_id;
  result->task_start_since_pass =
      task_wall_start - shared->pass_wall_start;

   // local_bp reused across tasks, but solutions change per task
  phase_wall_start = timenow();

  shared->hooks->reset_local_context_for_task(&ctx->local_bp, result->solutions);

  result->reset_seconds = timenow() - phase_wall_start;

  phase_wall_start = timenow();

  /*
   * get_index() opens and mmaps the index components. Install the immutable
   * pass advice before acquisition so every new mapping receives the correct
   * policy immediately.
   */
  fitsbin_mmap_set_thread_advice(
      shared->mmap_advice);

  index = index_shard_worker_get_index(
      ctx,
      shared,
      index_order);

  fitsbin_mmap_clear_thread_advice();

  if (!index) {
    result->acquire_seconds = timenow() - phase_wall_start;

    ERROR("Failed to load index order %zu", index_order);
    result->failed = TRUE;
    result->rc = -1;

    index_shard_result_finish_task(result,
                                   shared,
                                   task_wall_start);
    return -1;
  }

  /*
   * Reapply to any component that the onefield hook reused rather than opened
   * during this acquisition.
   */
  index_shard_apply_index_mmap_advice(
      index,
      shared->mmap_advice);

  result->acquire_seconds = timenow() - phase_wall_start;

  if (index_shard_trace_enabled()) {
    logmsg("[index-shard] worker=%i start index_order=%zu cached=%i index=%s\n", ctx->worker_id,
           index_order, cached_index, index->indexname ? index->indexname : "(null)");
  }
  // time only the actual one-index solve section
  wall_start = timenow();
  cpu_start = get_cpu_usage();

  // TLS lets onefield callbacks see global stop state
  index_shard_set_tls(ctx);
  rc = shared->hooks->solve_one_index(&ctx->local_bp, index);
  index_shard_set_tls(NULL);

  result->wall_seconds = timenow() - wall_start;
  result->cpu_seconds = get_cpu_usage() - cpu_start;

  result->hit_total_cpulimit = ctx->local_bp.hit_total_cpulimit;
  result->cancelled = ctx->local_bp.cancelled;
  result->rc = rc;

  // analyze before reducer so worker can trigger fast stop
  phase_wall_start = timenow();

  index_shard_capture_solution_analysis(shared, result);

  if (ctx->local_bp.single_field_solved) {
    result->solved = TRUE;
  }

  result->analyze_seconds =
      timenow() - phase_wall_start;

  /*
   * Log the index name before done_with_index() releases index ownership.
   */
  if (index_shard_trace_enabled()) {
    logmsg("[index-shard] worker=%i finish index_order=%zu index=%s\n",
           ctx->worker_id,
           index_order,
           index->indexname ? index->indexname : "(null)");
  }

  // Release through the original onefield ownership hook.
  phase_wall_start = timenow();
  if (shared->hooks->done_with_index) {
    shared->hooks->done_with_index(shared->bp, index_order, index);
    index = NULL;
  }

  result->release_seconds =
      timenow() - phase_wall_start;

  index_shard_result_finish_task(result,
                                 shared,
                                 task_wall_start);
  if (rc) {
    result->failed = TRUE;
    result->rc = rc;
    return rc;
  }

  return 0;
}
// ANCHOR INDEX-SHARD: worker-done
/*
 * Publish that this worker is done with the submitted pass.
 *
 * Reducer waits on active_workers reaching zero before final cleanup/drain.
 */
static void index_shard_worker_done(index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->result_mutex);

  if (shared->active_workers > 0)
    shared->active_workers--;

  pthread_cond_broadcast(&shared->result_cv);
  pthread_mutex_unlock(&shared->result_mutex);
}
// ANCHOR INDEX-SHARD: worker-main
/*
 * Persistent worker loop.
 *
 * Worker sleeps until pool generation changes, prepares local pass context,
 * claims one-index tasks, then cleans up local context when the pass ends.
 */
static void *index_shard_worker_main(void *userdata) {
  index_shard_worker_context_t *ctx = userdata;
  index_shard_pool_t *pool;

  if (!ctx) {
    return NULL;
  }

  pool = ctx->pool;
  if (!pool) {
    return NULL;
  }

  index_shard_current_worker_pool = pool;

  while (1) {
    index_shard_thread_state_t *shared = NULL;
    size_t index_order;
    anbool frontier_task;
    index_shard_aux_task_t *aux_task = NULL;
    int run_pass = FALSE;

    pthread_mutex_lock(&pool->control_mutex);

    while (!pool->shutdown) {
      /*
       * A new outer pass always wins over queued inner work.
       *
       * Both pass submission and auxiliary enqueue notify work_cv while
       * holding control_mutex. This is the worker lost-wakeup boundary.
       */
      if (ctx->generation_seen != pool->generation) {
        ctx->generation_seen = pool->generation;
        shared = &pool->shared;

        if (ctx->worker_id < shared->worker_count) {
          run_pass = TRUE;
        }
        break;
      }

      aux_task = index_shard_aux_queue_try_pop(
          &pool->auxq,
          NULL,
          FALSE,
          FALSE);
      if (aux_task) {
        break;
      }

      pthread_cond_wait(&pool->work_cv, &pool->control_mutex);
    }

    if (pool->shutdown) {
      pthread_mutex_unlock(&pool->control_mutex);
      break;
    }

    pthread_mutex_unlock(&pool->control_mutex);

    if (aux_task) {
      index_shard_aux_execute_one(aux_task);
      continue;
    }

    if (!run_pass) {
      continue;
    }

    if (index_shard_worker_prepare_pass(ctx, shared)) {
      index_shard_request_fatal_stop(shared);
      index_shard_worker_done(shared);
      continue;
    }

    while (1) {
      index_shard_result_t *result;

      /*
       * A completed outer task has released its queue credit. Before this
       * worker claims the next ordered index, it may execute the one helper
       * range reserved by an active large inner wave. After exactly one range
       * it returns here and resumes the unchanged outer claim sequence.
       */
      (void)index_shard_help_lent_once(pool);

      if (!index_shard_claim_one(shared,
                                 &index_order,
                                 &frontier_task)) {
        break;
      }

      result = &shared->results[index_order];

      if (index_shard_check_global_limits(shared) ||
          index_shard_master_stop_requested(shared)) {
        index_shard_master_limit_or_cancel_requested(
            shared,
            &result->hit_total_cpulimit,
            &result->cancelled);
        index_shard_mark_result_completed(shared,
                                          index_order,
                                          frontier_task);
        break;
      }

      if (index_shard_run_one_with_worker_context(ctx,
                                                  shared,
                                                  index_order,
                                                  result)) {
        index_shard_mark_result_completed(shared,
                                          index_order,
                                          frontier_task);
        index_shard_request_fatal_stop(shared);
        break;
      }

      if (result->solved) {
        logmsg("[index-shard] solved-candidate worker=%i index_order=%zu "
               "best_logodds=%.3f field=%i wall=%.3f cpu=%.3f\n",
               ctx->worker_id,
               index_order,
               result->best_logodds,
               result->best_fieldnum,
               result->wall_seconds,
               (double)result->cpu_seconds);

        index_shard_publish_solved(shared, index_order);
        index_shard_request_stop(shared);
      }

      if (index_shard_trace_enabled()) {
        logmsg("[index-shard] complete worker=%i index_order=%zu solved=%i "
               "failed=%i wall=%.3f cpu=%.3f pass_wall=%.3f\n",
               ctx->worker_id,
               index_order,
               result->solved,
               result->failed,
               result->wall_seconds,
               (double)result->cpu_seconds,
               timenow() - shared->pass_wall_start);
      }

      index_shard_check_global_limits(shared);
      index_shard_mark_result_completed(shared,
                                        index_order,
                                        frontier_task);
    }

    index_shard_worker_cleanup_pass(ctx, shared);
    index_shard_worker_done(shared);
  }

  index_shard_current_worker_pool = NULL;
  return NULL;
}

/*
 * SECTION INDEX-SHARD: reducer
 *
 * Main-thread result publication.
 *
 * Workers fill result slots.  The reducer is the only path that transfers
 * MatchObj data into master bp->solutions and updates final master solved state.
 */

// ANCHOR INDEX-SHARD: reduce-completed-solved
/*
 * Merge a completed solved result even if it is not the ordered prefix.
 *
 * This is the fast solved-stop path.  It avoids waiting for earlier unsolved
 * indexes once a later index has already produced an accepted solution.
 */
static int index_shard_reduce_completed_solved(index_shard_thread_state_t *shared) {
  ssize_t solved_i;

  pthread_mutex_lock(&shared->result_mutex);
  solved_i = index_shard_find_completed_solved_locked(shared);
  pthread_mutex_unlock(&shared->result_mutex);

  if (solved_i < 0)
    return 0;

  if (index_shard_reduce_one_result(shared, &shared->results[solved_i]))
    return -1;

  logmsg("[index-shard] reduce solved index_order=%zu best_logodds=%.3f field=%i\n",
         shared->results[solved_i].index_order, shared->results[solved_i].best_logodds,
         shared->results[solved_i].best_fieldnum);

  shared->bp->single_field_solved = TRUE;
  index_shard_request_stop(shared);
  return 1;
}
// ANCHOR INDEX-SHARD: reduce-online
/*
 * Online reducer for one submitted pass.
 *
 * Normal mode reduces the completed prefix.  Fast solved-stop mode commits any
 * completed valid solved result immediately; exact serial index order is not a
 * required project invariant.
 */
static int index_shard_pool_reduce_online(index_shard_pool_t *pool) {
  index_shard_thread_state_t *shared = &pool->shared;
  int rc = 0;

  while (shared->next_reduce < shared->nindexes) {
    index_shard_pass_state_snapshot_t state;
    int can_reduce = FALSE;
    int workers_done = FALSE;
    int fatal = FALSE;
    ssize_t solved_i = -1;

    pthread_mutex_lock(&shared->result_mutex);

    while (!shared->completed[shared->next_reduce] &&
           shared->active_workers > 0) {
      index_shard_pass_state_snapshot(shared, &state);

      if (state.fatal_error) {
        break;
      }

      if (state.stop_requested) {
        solved_i = index_shard_find_completed_solved_locked(shared);

        if (solved_i >= 0) {
          break;
        }
      }

      pthread_cond_wait(&shared->result_cv, &shared->result_mutex);
    }

    if (solved_i < 0) {
      solved_i = index_shard_find_completed_solved_locked(shared);
    }

    can_reduce = shared->completed[shared->next_reduce];
    workers_done = (shared->active_workers == 0);

    index_shard_pass_state_snapshot(shared, &state);
    fatal = state.fatal_error;

    pthread_mutex_unlock(&shared->result_mutex);

    if (solved_i >= 0) {
      index_shard_result_t *solved_result = &shared->results[solved_i];

      if (index_shard_reduce_one_result(shared, solved_result)) {
        rc = -1;
        index_shard_request_fatal_stop(shared);
        break;
      }

      logmsg("[index-shard] reduce solved index_order=%zu "
             "best_logodds=%.3f field=%i\n",
             solved_result->index_order,
             solved_result->best_logodds,
             solved_result->best_fieldnum);

      index_shard_request_stop(shared);
      break;
    }

    if (fatal) {
      rc = -1;
      index_shard_request_stop(shared);
      break;
    }

    if (can_reduce) {
      index_shard_result_t *result =
          &shared->results[shared->next_reduce];

      if (index_shard_trace_enabled()) {
        logmsg("[index-shard] reduce index_order=%zu solved=%i failed=%i\n",
               shared->next_reduce,
               result->solved,
               result->failed);
      }

      if (result->failed) {
        rc = -1;
        index_shard_request_fatal_stop(shared);
        break;
      }

      if (index_shard_reduce_one_result(shared, result)) {
        rc = -1;
        index_shard_request_fatal_stop(shared);
        break;
      }

      shared->next_reduce++;

      if (index_shard_master_stop_requested(shared)) {
        index_shard_request_stop(shared);
        break;
      }

      continue;
    }

    if (workers_done) {
      break;
    }
  }

  pthread_mutex_lock(&shared->result_mutex);

  while (shared->active_workers > 0) {
    pthread_cond_wait(&shared->result_cv, &shared->result_mutex);
  }

  pthread_mutex_unlock(&shared->result_mutex);

  while (shared->next_reduce < shared->nindexes &&
         shared->completed[shared->next_reduce]) {
    index_shard_pass_state_snapshot_t state;
    index_shard_result_t *result =
        &shared->results[shared->next_reduce];

    if (!result->failed && !result->merged) {
      if (index_shard_reduce_one_result(shared, result)) {
        rc = -1;
        break;
      }
    }

    shared->next_reduce++;

    index_shard_pass_state_snapshot(shared, &state);

    if (state.solved_published) {
      break;
    }
  }

  return rc;
}
/*
 * SECTION INDEX-SHARD: pool
 *
 * Pool lifecycle and pass submission.
 *
 * The pool is created once per engine job and reused across onefield_run()
 * calls.  Each submitted pass increments generation to wake workers.
 */

// ANCHOR INDEX-SHARD: shared-init
/*
 * Initialize synchronization primitives for the reusable shared pass state.
 */
static int index_shard_shared_init(index_shard_thread_state_t *shared) {
  memset(shared, 0, sizeof(index_shard_thread_state_t));

  if (pthread_mutex_init(&shared->queue_mutex, NULL)) {
    return -1;
  }

  if (pthread_cond_init(&shared->queue_cv, NULL)) {
    return -1;
  }

  if (pthread_mutex_init(&shared->result_mutex, NULL)) {
    return -1;
  }

  if (pthread_cond_init(&shared->result_cv, NULL)) {
    return -1;
  }

  if (pthread_mutex_init(&shared->state_mutex, NULL)) {
    return -1;
  }

  if (pthread_mutex_init(&shared->limit_mutex, NULL)) {
    return -1;
  }

  return 0;
}

// ANCHOR INDEX-SHARD: shared-destroy
/*
 * Destroy synchronization primitives after all workers have joined.
 */
static void index_shard_shared_destroy(index_shard_thread_state_t *shared) {
  pthread_mutex_destroy(&shared->queue_mutex);
  pthread_cond_destroy(&shared->queue_cv);

  pthread_mutex_destroy(&shared->result_mutex);
  pthread_cond_destroy(&shared->result_cv);

  pthread_mutex_destroy(&shared->state_mutex);

  pthread_mutex_destroy(&shared->limit_mutex);
}

typedef enum index_shard_pool_acquire_status {
  INDEX_SHARD_POOL_ACQUIRE_CONFLICT = -1,
  INDEX_SHARD_POOL_ACQUIRE_OK = 0,
  INDEX_SHARD_POOL_ACQUIRE_UNAVAILABLE = 1
} index_shard_pool_acquire_status_t;

/*
 * Reserve the persistent pool for one submitted pass.
 *
 * Lock order:
 *   index_shard_global_pool_mutex -> pool->control_mutex
 *
 * This prevents a second submission from overwriting shared pass pointers and
 * also keeps pool_stop() from destroying the pool while the caller still uses
 * it.
 */
static index_shard_pool_acquire_status_t
index_shard_pool_acquire_pass(onefield_t *bp,
                              solver_t *sp,
                              index_shard_pool_t **pool_out) {
  index_shard_pool_t *pool;

  if (!pool_out) {
    return INDEX_SHARD_POOL_ACQUIRE_CONFLICT;
  }

  *pool_out = NULL;

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  pool = index_shard_global_pool;

  if (!pool) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return INDEX_SHARD_POOL_ACQUIRE_UNAVAILABLE;
  }

  if (pool->owner_bp != bp || pool->owner_sp != sp) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return INDEX_SHARD_POOL_ACQUIRE_CONFLICT;
  }

  pthread_mutex_lock(&pool->control_mutex);

  if (pool->shutdown || pool->stopping || pool->pass_active) {
    pthread_mutex_unlock(&pool->control_mutex);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return INDEX_SHARD_POOL_ACQUIRE_CONFLICT;
  }

  pool->pass_active = TRUE;
  *pool_out = pool;

  pthread_mutex_unlock(&pool->control_mutex);
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  return INDEX_SHARD_POOL_ACQUIRE_OK;
}

/*
 * Release one pass reservation after all caller-side work that dereferences
 * the pool has completed.
 */
static void index_shard_pool_release_pass(index_shard_pool_t *pool) {
  if (!pool) {
    return;
  }

  pthread_mutex_lock(&pool->control_mutex);

  if (!pool->pass_active) {
    logerr("[index-shard] pass release requested with no active pass\n");
  } else {
    pool->pass_active = FALSE;
  }

  /*
   * pool_stop() may be waiting for pass_active to become false. Worker waiters
   * also use work_cv, but they re-check their generation predicate in a loop.
   */
  pthread_cond_broadcast(&pool->work_cv);

  pthread_mutex_unlock(&pool->control_mutex);
}

// ANCHOR INDEX-SHARD: pool-start
/*
 * Create persistent worker pool.
 *
 * Workers are created once and sleep until the first pass is submitted.
 */
int index_shard_pool_start(onefield_t *bp, solver_t *sp) {
  index_shard_pool_t *pool;
  int i;
  int worker_count;

   // pool already active for this engine job
  if (!index_shard_pthread_enabled()) {
    return 0;
  }

  if (!bp || !sp) {
    ERROR("Cannot start index-shard pool without owner state");
    return -1;
  }

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  if (index_shard_global_pool) {
    int reusable;

    pool = index_shard_global_pool;

    if (pool->owner_bp != bp || pool->owner_sp != sp) {
      logerr("[index-shard] global pool already belongs to another engine job\n");
      pthread_mutex_unlock(&index_shard_global_pool_mutex);
      return -1;
    }

    pthread_mutex_lock(&pool->control_mutex);
    reusable = !pool->shutdown && !pool->stopping;
    pthread_mutex_unlock(&pool->control_mutex);

    pthread_mutex_unlock(&index_shard_global_pool_mutex);

    if (!reusable) {
      logerr("[index-shard] owner pool is stopping or shut down\n");
      return -1;
    }

    return 0;
  }

  worker_count = index_shard_get_worker_count(0);

  pool = calloc(1, sizeof(index_shard_pool_t));
  if (!pool) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    SYSERROR("Failed to allocate index-shard pool");
    return -1;
  }

  pool->owner_bp = bp;
  pool->owner_sp = sp;
  pool->worker_count = worker_count;

  // initialize shared state before workers can observe pool
  if (pthread_mutex_init(&pool->control_mutex, NULL)) {
    free(pool);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  if (pthread_cond_init(&pool->work_cv, NULL)) {
    pthread_mutex_destroy(&pool->control_mutex);
    free(pool);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  if (index_shard_shared_init(&pool->shared)) {
    pthread_cond_destroy(&pool->work_cv);
    pthread_mutex_destroy(&pool->control_mutex);
    free(pool);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  size_t aux_max_pending;

  aux_max_pending =
      (size_t)pool->worker_count * (size_t)pool->worker_count;

  if (aux_max_pending < (size_t)pool->worker_count) {
    aux_max_pending = (size_t)pool->worker_count;
  }

  if (index_shard_aux_queue_init(&pool->auxq, aux_max_pending)) {
    index_shard_shared_destroy(&pool->shared);
    pthread_cond_destroy(&pool->work_cv);
    pthread_mutex_destroy(&pool->control_mutex);
    free(pool);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  if (index_shard_prefetch_coordinator_init(
          &pool->prefetch,
          worker_count)) {
    index_shard_aux_queue_destroy(&pool->auxq);
    index_shard_shared_destroy(&pool->shared);
    pthread_cond_destroy(&pool->work_cv);
    pthread_mutex_destroy(&pool->control_mutex);
    free(pool);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  pool->threads = calloc((size_t)worker_count, sizeof(pthread_t));
  pool->contexts = calloc((size_t)worker_count,
                          sizeof(index_shard_worker_context_t));

    if (!pool->threads || !pool->contexts) {
      free(pool->threads);
      free(pool->contexts);

      index_shard_prefetch_coordinator_destroy(&pool->prefetch);
      index_shard_aux_queue_destroy(&pool->auxq);
      index_shard_shared_destroy(&pool->shared);

      pthread_cond_destroy(&pool->work_cv);
      pthread_mutex_destroy(&pool->control_mutex);

      free(pool);

      pthread_mutex_unlock(&index_shard_global_pool_mutex);
      return -1;
    }

  // worker contexts are stable for lifetime of the pool
  for (i = 0; i < worker_count; i++) {
    pool->contexts[i].worker_id = i;
    pool->contexts[i].generation_seen = 0;
    pool->contexts[i].pool = pool;

    if (pthread_create(&pool->threads[i], NULL, index_shard_worker_main, &pool->contexts[i])) {
      int j;

      pthread_mutex_lock(&pool->control_mutex);
      pool->shutdown = TRUE;
      pthread_cond_broadcast(&pool->work_cv);
      pthread_mutex_unlock(&pool->control_mutex);

            for (j = 0; j < i; j++) {
        pthread_join(pool->threads[j], NULL);
      }

       free(pool->threads);
      free(pool->contexts);

      index_shard_prefetch_coordinator_destroy(&pool->prefetch);
      index_shard_aux_queue_destroy(&pool->auxq);
      index_shard_shared_destroy(&pool->shared);

      pthread_cond_destroy(&pool->work_cv);
      pthread_mutex_destroy(&pool->control_mutex);

      free(pool);

      pthread_mutex_unlock(&index_shard_global_pool_mutex);
      return -1;
    }
  }

  index_shard_global_pool = pool;

  logmsg("[index-shard] workers=%i mode=pthread\n",
         worker_count);

  pthread_mutex_unlock(&index_shard_global_pool_mutex);
  return 0;
}
// ANCHOR INDEX-SHARD: pool-stop
/*
 * Stop the pool belonging to bp and join all workers.
 *
 * New passes are rejected as soon as stopping is published. If one pass is
 * already active, shutdown waits for that pass reservation to be released.
 */
void index_shard_pool_stop(onefield_t *bp) {
  index_shard_pool_t *pool;
  index_shard_aux_task_t *cancelled_tasks = NULL;
  index_shard_aux_task_t *task;
  index_shard_aux_metrics_snapshot_t aux_metrics;
  unsigned long long lend_acquired_total;
  unsigned long long lend_busy_total;
  unsigned long long lend_fallback_total;
  unsigned long long lend_tasks_total;
  int lend_group_live;
  size_t cancelled_count = 0;
  int i;

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  pool = index_shard_global_pool;

  if (!pool) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return;
  }

  if (pool->owner_bp != bp) {
    logerr("[index-shard] refusing to stop pool owned by another engine job\n");
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return;
  }

  pthread_mutex_lock(&pool->control_mutex);

  if (pool->stopping) {
    pthread_mutex_unlock(&pool->control_mutex);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return;
  }

  pool->stopping = TRUE;

  /*
   * An active pass may still call compatibility APIs that consult the
   * process-global pool pointer. Release the global mutex before waiting.
   */
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  while (pool->pass_active) {
    pthread_cond_wait(&pool->work_cv, &pool->control_mutex);
  }

  pool->shutdown = TRUE;

  /*
   * Detach queued auxiliary work using the same control -> auxq lock order
   * used by enqueue and worker wakeup.
   *
   * Every accepted task must end as either executed or cancelled. Cancelling
   * a task also releases its group's pending credit.
   */
  pthread_mutex_lock(&pool->auxq.mutex);

  pool->auxq.stopping = TRUE;
  cancelled_tasks = pool->auxq.head;
  pool->auxq.head = NULL;
  pool->auxq.tail = NULL;

  for (task = cancelled_tasks; task; task = task->next) {
    cancelled_count++;
  }

  pool->auxq.pending = 0;
  pool->auxq.cancelled_total += cancelled_count;

  pthread_mutex_unlock(&pool->auxq.mutex);

  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);

  index_shard_aux_cancel_list(cancelled_tasks);

  /*
   * No active pass can retain the pool now. Detach it before freeing worker
   * storage so later submissions cannot acquire this instance.
   */
  pthread_mutex_lock(&index_shard_global_pool_mutex);

  if (index_shard_global_pool == pool) {
    index_shard_global_pool = NULL;
  }

  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  for (i = 0; i < pool->worker_count; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  free(pool->threads);
  free(pool->contexts);
  index_shard_aux_metrics_snapshot(&pool->auxq, &aux_metrics);

  pthread_mutex_lock(&pool->auxq.mutex);

  lend_acquired_total = pool->lend_acquired_total;
  lend_busy_total = pool->lend_busy_total;
  lend_tasks_total = pool->lend_tasks_total;
  lend_fallback_total = pool->lend_fallback_total;
  lend_group_live = pool->lend_group != NULL;

  pthread_mutex_unlock(&pool->auxq.mutex);

  logverb("[index-shard] auxq submitted=%llu executed=%llu rejected=%llu "
         "cancelled=%llu pending=%zu max_pending=%zu\n",
         aux_metrics.submitted,
         aux_metrics.executed,
         aux_metrics.rejected,
         aux_metrics.cancelled,
         aux_metrics.pending,
         aux_metrics.max_pending);

  if (aux_metrics.pending != 0 ||
      aux_metrics.submitted !=
          aux_metrics.executed + aux_metrics.cancelled) {
    logerr("[index-shard] auxq accounting mismatch at shutdown\n");
  }

  logverb("[index-shard] inner-lending acquired=%llu busy=%llu "
         "lent_tasks=%llu owner_fallback=%llu live_group=%i\n",
         lend_acquired_total,
         lend_busy_total,
         lend_tasks_total,
         lend_fallback_total,
         lend_group_live);

  if (lend_group_live) {
    logerr("[index-shard] inner lending group live at shutdown\n");
  }

  index_shard_shared_destroy(&pool->shared);
  index_shard_prefetch_coordinator_destroy(&pool->prefetch);
  index_shard_aux_queue_destroy(&pool->auxq);

  pthread_cond_destroy(&pool->work_cv);
  pthread_mutex_destroy(&pool->control_mutex);

  logverb("[index-shard] pthread-pool stop\n");

  free(pool);
}

// ANCHOR INDEX-SHARD: pool-active
/*
 * Return true only when the compatibility pool belongs to bp and remains
 * available for a new pass reservation.
 */
int index_shard_pool_active(onefield_t *bp) {
  index_shard_pool_t *pool;
  int active = FALSE;

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  pool = index_shard_global_pool;

  if (pool && pool->owner_bp == bp) {
    pthread_mutex_lock(&pool->control_mutex);
    active = !pool->shutdown && !pool->stopping;
    pthread_mutex_unlock(&pool->control_mutex);
  }

  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  return active;
}

// ANCHOR INDEX-SHARD: pool-submit
/*
 * Submit one onefield_run() pass to the persistent pool.
 *
 * This resets shared pass state, publishes task/result arrays, then increments
 * generation to wake workers.
 */
static int index_shard_pool_submit(index_shard_pool_t *pool, onefield_t *bp, solver_t *base_sp,
                                   size_t nindexes, const index_shard_hooks_t *hooks,
                                   index_shard_task_t *tasks, index_shard_result_t *results,
                                   unsigned char *completed,
                                   size_t task_family_count,
                                   size_t task_frontier_count) {
  index_shard_thread_state_t *shared = &pool->shared;
  int worker_count = index_shard_get_worker_count(nindexes);

  // current pass may use fewer workers than the pool owns
  if (worker_count > pool->worker_count)
    worker_count = pool->worker_count;

   pthread_mutex_lock(&pool->control_mutex);

  if (!pool->pass_active || pool->shutdown ||
      pool->owner_bp != bp || pool->owner_sp != base_sp) {
    pthread_mutex_unlock(&pool->control_mutex);
    return -1;
  }

  pthread_mutex_lock(&shared->queue_mutex);
  pthread_mutex_lock(&shared->result_mutex);
  pthread_mutex_lock(&shared->state_mutex);
  pthread_mutex_lock(&shared->limit_mutex);

  shared->bp = bp;
  shared->base_sp = base_sp;
  shared->hooks = hooks;

  shared->nindexes = nindexes;
  shared->tasks = tasks;
  shared->ntasks = nindexes;
  shared->next_task = 0;
  shared->next_frontier_task = 0;
  shared->next_ordered_task = task_frontier_count;
  shared->task_family_count = task_family_count;
  shared->task_frontier_count = task_frontier_count;
  shared->ordered_since_frontier = 0;
  shared->frontier_running = 0;

  shared->results = results;
  shared->completed = completed;
  shared->next_reduce = 0;

  shared->worker_count = worker_count;
  shared->active_workers = worker_count;
  shared->running_tasks = 0;

  // active_limit is a cap, not adaptive ramp logic
  shared->active_limit = worker_count;
  shared->max_active_workers = worker_count;

  shared->stop_requested = FALSE;
  shared->fatal_error = FALSE;
  shared->solved_published = FALSE;
  shared->master_committed = FALSE;

  shared->have_solved_order = FALSE;
  shared->earliest_solved_order = 0;

  shared->limit_reported = FALSE;

   /*
   * Policy state belongs to the master solver and changes only after a fully
   * quiesced pass. Workers consume this immutable pass snapshot.
   */
  shared->mmap_advice =
      fitsbin_mmap_advice_state_begin_pass(
          &base_sp->index_mmap_policy);

  shared->mmap_pass_number =
      base_sp->index_mmap_policy.pass_number;

  // pass timing excludes final solve-field output generation
  shared->pass_wall_start = timenow();
  shared->pass_cpu_start = get_cpu_usage();

  shared->pass_rusage_valid =
      (getrusage(RUSAGE_SELF,
                 &shared->pass_rusage_start) == 0);

  pthread_mutex_unlock(&shared->limit_mutex);
  pthread_mutex_unlock(&shared->state_mutex);
  pthread_mutex_unlock(&shared->result_mutex);
  pthread_mutex_unlock(&shared->queue_mutex);

  // generation publish wakes workers for this pass
  pool->generation++;

  /*
   * Every pass starts with an empty hint set, empty recent-page window and
   * fresh accounting. This occurs before workers observe the new generation.
   */
  index_shard_prefetch_coordinator_reset(
      &pool->prefetch,
      pool->generation);

  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);

  logverb("[index-shard] pthread-pool submit workers=%i pool_workers=%i "
         "candidates=%zu "
         "startobj=%i endobj=%i scheduler=%s chunk=1 "
         "families=%zu frontier=%zu frontier_limit=%i "
         "single_worker_ordered_quantum=%i "
         "active_limit=%i mmap_pass=%u mmap_advice=%s "
         "kd_continuation=%i kd_node_budget=%zu\n",
         worker_count,
         pool->worker_count,
         nindexes,
         base_sp->startobj,
         base_sp->endobj,
         shared->task_frontier_count
             ? "discovery-frontier"
             : "ordered",
         shared->task_family_count,
         shared->task_frontier_count,
         INDEX_SHARD_DISCOVERY_FRONTIER_LIMIT,
         INDEX_SHARD_SINGLE_WORKER_ORDERED_QUANTUM,
         shared->active_limit,
         shared->mmap_pass_number,
         fitsbin_mmap_advice_name(shared->mmap_advice),
         index_shard_config_get()->kd_continuation_enabled ? 1 : 0,
         index_shard_config_get()->kd_continuation_node_budget);

  if (index_shard_trace_enabled() &&
      shared->task_frontier_count) {
    size_t frontier_position;

    for (frontier_position = 0;
         frontier_position < shared->task_frontier_count;
         frontier_position++) {
      size_t index_order = tasks[frontier_position].index_order;
      const char *index_name = hooks->get_index_name(bp, index_order);

      logmsg("[index-shard] discovery-frontier position=%zu "
             "index_order=%zu index=%s\n",
             frontier_position,
             index_order,
             index_name ? index_name : "(null)");
    }
  }

  return 0;
}

/*
 * SECTION INDEX-SHARD: entry
 */

// ANCHOR INDEX-SHARD: entry
/*
 * Execute one complete index-shard pass.
 *
 * Terminal status is classified according to whether master-visible solver
 * state has already been mutated. Only an unavailable path or a failure
 * proven to occur before master commit may return control to the serial path.
 */
index_shard_solve_status_t index_shard_solve(onefield_t *bp,
                  solver_t *base_sp,
                  size_t nindexes,
                  const index_shard_hooks_t *hooks) {
  index_shard_pool_t *pool;
  index_shard_task_t *tasks = NULL;
  index_shard_result_t *results = NULL;
  unsigned char *completed = NULL;
  size_t task_family_count = 0;
  size_t task_frontier_count = 0;
  size_t i;
  int acquire_rc;
  int rc = 0;
  index_shard_pass_state_snapshot_t state;
  index_shard_pass_metrics_snapshot_t pass_metrics;
  index_shard_task_profile_snapshot_t task_profile;
  index_shard_prefetch_metrics_snapshot_t prefetch_metrics;
  index_shard_phase_profile_snapshot_t phase_profile;
  index_shard_solve_status_t status = INDEX_SHARD_SOLVE_HANDLED;
  anbool pass_completed;
  anbool pass_exhaustive;
  anbool pass_solved;
  anbool pass_cancelled;
  anbool mmap_transitioned;

  if (!index_shard_pthread_enabled()) {
    return INDEX_SHARD_SOLVE_UNAVAILABLE;
  }

  // no candidate indexes, nothing to do
  if (!nindexes) {
    return INDEX_SHARD_SOLVE_HANDLED;
  }

  if (!hooks) {
    ERROR("index-shard hooks are NULL");
    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }

  acquire_rc = index_shard_pool_acquire_pass(bp, base_sp, &pool);

  if (acquire_rc == INDEX_SHARD_POOL_ACQUIRE_UNAVAILABLE) {
    logmsg("[index-shard] pthread mode requested but pool inactive\n");
    return INDEX_SHARD_SOLVE_UNAVAILABLE;
  }

  if (acquire_rc != INDEX_SHARD_POOL_ACQUIRE_OK) {
    logerr("[index-shard] pool ownership or pass lifecycle conflict; "
           "serial fallback suppressed\n");
    return INDEX_SHARD_SOLVE_LIFECYCLE_CONFLICT;
  }

  kdtree_phase_a_reset();

  tasks = index_shard_build_task_plan(
      bp,
      nindexes,
      hooks,
      &task_family_count,
      &task_frontier_count);
  results = calloc(nindexes, sizeof(index_shard_result_t));
  completed = calloc(nindexes, sizeof(unsigned char));

  if (!tasks || !results || !completed) {
    SYSERROR("Failed to allocate index-shard pass state");

    free(tasks);
    free(results);
    free(completed);

    index_shard_pool_release_pass(pool);
    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }
   // submit wakes workers, reducer runs on caller thread
  rc = index_shard_pool_submit(pool, bp, base_sp, nindexes, hooks,
                               tasks, results, completed,
                               task_family_count,
                               task_frontier_count);

  if (rc) {
    free(tasks);
    free(results);
    free(completed);

    index_shard_pool_release_pass(pool);
    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }

  rc = index_shard_pool_reduce_online(pool);
  index_shard_pass_state_snapshot(&pool->shared, &state);

  if (rc) {
    if (state.master_committed) {
      status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
    } else {
      status = INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
    }
  }

   /*
   * Defensive ownership invariant: once the reducer has crossed the master
   * mutation boundary, a precommit failure classification is impossible.
   */
  if (state.master_committed &&
      status == INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE) {
    logerr("[index-shard] invalid precommit status after master commit; "
           "promoting to terminal failure\n");
    status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
  }

   /*
   * Workers have left the pass and result slots are now immutable. Snapshot
   * pass timing and task-duration distribution before destroying results.
   */
  index_shard_pass_metrics_snapshot(&pool->shared,
                                    &pass_metrics);

  index_shard_task_profile_snapshot(results,
                                    nindexes,
                                    pass_metrics.wall_seconds,
                                    &task_profile);

  index_shard_phase_profile_snapshot(results,
                                     nindexes,
                                     &phase_profile);

  index_shard_prefetch_metrics_snapshot( &pool->prefetch, &prefetch_metrics);

  /*
   * index_shard_pool_reduce_online() has returned and every participating
   * worker has left this generation. The pass outcome is now immutable.
   */
  pass_completed =
      task_profile.executed == nindexes;

  pass_exhaustive =
      pass_completed &&
      pass_metrics.reduced == nindexes;

  pass_solved =
      state.master_committed ||
      bp->single_field_solved;

  pass_cancelled =
      !pass_solved &&
      (bp->cancelled ||
       bp->hit_total_cpulimit ||
       bp->hit_total_timelimit ||
       state.stop_requested);

  mmap_transitioned =
      fitsbin_mmap_policy_complete_pass(
          &base_sp->index_mmap_policy,
          pass_completed,
          pass_exhaustive,
          pass_solved,
          pass_cancelled,
          rc,
          (int)status);
  // dispose unmerged worker results after all workers have left pass
  for (i = 0; i < nindexes; i++) {
    index_shard_result_dispose(&results[i], hooks);
  }

  free(tasks);
  free(results);
  free(completed);

  logmsg("[index-shard] done workers=%i solved=%i "
         "wall=%.3f cpu=%.3f utilization=%.1f%%\n",
         pool->worker_count,
         bp->single_field_solved,
         pass_metrics.wall_seconds,
         (double)pass_metrics.cpu_seconds,
         pass_metrics.cpu_percent);

  logverb("[index-shard] pass-detail candidates=%zu reduced=%zu "
          "total_cpu=%i total_wall=%i cancelled=%i rc=%i status=%i "
          "master_committed=%i\n",
          nindexes,
          pass_metrics.reduced,
          bp->hit_total_cpulimit,
          bp->hit_total_timelimit,
          bp->cancelled,
          rc,
          (int)status,
          state.master_committed);

  logverb("[index-shard] mmap-policy "
         "policy=%s effective=%s pass=%u "
         "clean_unsolved_passes=%u transitions=%u "
         "transitioned=%i completed=%i exhaustive=%i "
         "solved=%i cancelled=%i\n",
         fitsbin_mmap_policy_name(
             base_sp->index_mmap_policy.policy),
         fitsbin_mmap_advice_name(
             base_sp->index_mmap_policy.effective_advice),
         base_sp->index_mmap_policy.pass_number,
         base_sp->index_mmap_policy
             .completed_clean_unsolved_passes,
         base_sp->index_mmap_policy.transition_count,
         mmap_transitioned ? 1 : 0,
         pass_completed ? 1 : 0,
         pass_exhaustive ? 1 : 0,
         pass_solved ? 1 : 0,
         pass_cancelled ? 1 : 0);

   if (!task_profile.executed) {
    logverb("[index-shard] task-profile executed=0\n");
  } else if (task_profile.quantiles_available) {
    logverb("[index-shard] task-profile executed=%zu "
           "task_p50=%.3f task_p90=%.3f task_p99=%.3f "
           "task_max=%.3f max_order=%zu max_worker=%i "
           "max_solve=%.3f skew_max_p50=%.1f "
           "max_pool_pct=%.1f%% serial_tail=%.3f "
           "serial_tail_pct=%.1f%% tail_order=%zu tail_worker=%i\n",
           task_profile.executed,
           task_profile.task_p50_seconds,
           task_profile.task_p90_seconds,
           task_profile.task_p99_seconds,
           task_profile.task_max_seconds,
           task_profile.max_index_order,
           task_profile.max_worker_id,
           task_profile.max_solve_seconds,
           task_profile.max_to_p50,
           task_profile.max_pool_percent,
           task_profile.serial_tail_seconds,
           task_profile.serial_tail_percent,
           task_profile.tail_index_order,
           task_profile.tail_worker_id);
  } else {
    logverb("[index-shard] task-profile executed=%zu "
           "quantiles=unavailable task_max=%.3f "
           "max_order=%zu max_worker=%i max_solve=%.3f "
           "max_pool_pct=%.1f%% serial_tail=%.3f "
           "serial_tail_pct=%.1f%% tail_order=%zu tail_worker=%i\n",
           task_profile.executed,
           task_profile.task_max_seconds,
           task_profile.max_index_order,
           task_profile.max_worker_id,
           task_profile.max_solve_seconds,
           task_profile.max_pool_percent,
           task_profile.serial_tail_seconds,
           task_profile.serial_tail_percent,
           task_profile.tail_index_order,
           task_profile.tail_worker_id);
  }

  if (!phase_profile.executed) {
    logverb("[index-shard] phase-profile executed=0\n");
  } else if (phase_profile.quantiles_available) {
    logverb("[index-shard] phase-profile executed=%zu task_sum=%.3f "
           "reset=%.3f(%.1f%%) acquire=%.3f(%.1f%%) "
           "solve=%.3f(%.1f%%) analyze=%.3f(%.1f%%) "
           "release=%.3f(%.1f%%) other=%.3f(%.1f%%) "
           "acquire_p50=%.3f acquire_p90=%.3f "
           "acquire_p99=%.3f acquire_max=%.3f "
           "solve_p50=%.3f solve_p90=%.3f "
           "solve_p99=%.3f solve_max=%.3f\n",
           phase_profile.executed,
           phase_profile.task_wall_total,
           phase_profile.reset_total,
           phase_profile.reset_percent,
           phase_profile.acquire_total,
           phase_profile.acquire_percent,
           phase_profile.solve_total,
           phase_profile.solve_percent,
           phase_profile.analyze_total,
           phase_profile.analyze_percent,
           phase_profile.release_total,
           phase_profile.release_percent,
           phase_profile.other_total,
           phase_profile.other_percent,
           phase_profile.acquire_p50,
           phase_profile.acquire_p90,
           phase_profile.acquire_p99,
           phase_profile.acquire_max,
           phase_profile.solve_p50,
           phase_profile.solve_p90,
           phase_profile.solve_p99,
           phase_profile.solve_max);
  } else {
    logverb("[index-shard] phase-profile executed=%zu task_sum=%.3f "
           "reset=%.3f(%.1f%%) acquire=%.3f(%.1f%%) "
           "solve=%.3f(%.1f%%) analyze=%.3f(%.1f%%) "
           "release=%.3f(%.1f%%) other=%.3f(%.1f%%) "
           "quantiles=unavailable\n",
           phase_profile.executed,
           phase_profile.task_wall_total,
           phase_profile.reset_total,
           phase_profile.reset_percent,
           phase_profile.acquire_total,
           phase_profile.acquire_percent,
           phase_profile.solve_total,
           phase_profile.solve_percent,
           phase_profile.analyze_total,
           phase_profile.analyze_percent,
           phase_profile.release_total,
           phase_profile.release_percent,
           phase_profile.other_total,
           phase_profile.other_percent);
  }

  if (pass_metrics.resource_available) {
    logverb("[index-shard] pass-resource user=%.3f sys=%.3f "
           "minflt=%ld majflt=%ld nvcsw=%ld nivcsw=%ld\n",
           pass_metrics.user_seconds,
           pass_metrics.system_seconds,
           pass_metrics.minor_faults,
           pass_metrics.major_faults,
           pass_metrics.voluntary_context_switches,
           pass_metrics.involuntary_context_switches);
  } else {
    logverb("[index-shard] pass-resource unavailable\n");
  }

  logverb("[index-shard] prefetch-coordinator "
         "hints=%llu stale=%llu unmapped=%llu "
         "raw_pages=%llu unique_pages=%llu dup_pages=%llu "
         "selected_pages=%llu collect_drop=%llu budget_drop=%llu "
         "metadata_pages=%llu leaf_pages=%llu "
         "publishes=%llu publish_empty=%llu below_threshold=%llu "
         "flushes=%llu ranges=%llu bytes=%llu failures=%llu "
         "pass_budget_exhausted=%llu "
         "pending=%zu issue_budget_pages=%zu "
         "issue_threshold_pages=%zu "
         "pass_budget_pages=%zu pass_pages_issued=%zu "
         "collection_capacity=%zu recent_capacity=%zu\n"
         "mapping_barriers=%llu pending_purged=%llu recent_purged=%llu",
         prefetch_metrics.totals.hints_emitted,
         prefetch_metrics.totals.hints_stale,
         prefetch_metrics.totals.hints_unmapped,
         prefetch_metrics.totals.pages_raw,
         prefetch_metrics.totals.pages_unique,
         prefetch_metrics.totals.pages_duplicate,
         prefetch_metrics.totals.pages_selected,
         prefetch_metrics.totals.pages_collection_dropped,
         prefetch_metrics.totals.pages_budget_dropped,
         prefetch_metrics.totals.metadata_pages_selected,
         prefetch_metrics.totals.leaf_pages_selected,
         prefetch_metrics.totals.publish_calls,
         prefetch_metrics.totals.publish_empty,
         prefetch_metrics.totals.issue_below_threshold,
         prefetch_metrics.totals.flushes,
         prefetch_metrics.totals.ranges_issued,
         prefetch_metrics.totals.bytes_issued,
         prefetch_metrics.totals.prefetch_failures,
         prefetch_metrics.totals.pass_budget_exhausted,
         prefetch_metrics.pending,
         prefetch_metrics.issue_page_budget,
         prefetch_metrics.issue_threshold_pages,
         prefetch_metrics.pass_page_budget,
         prefetch_metrics.pass_pages_issued,
         prefetch_metrics.collection_capacity,
         prefetch_metrics.recent_capacity,
         prefetch_metrics.totals.mapping_barriers,
         prefetch_metrics.totals.pending_pages_purged,
         prefetch_metrics.totals.recent_pages_purged);

  kdtree_phase_a_stats_t kd_stats;
    double wall_total_ms;
    double cpu_total_ms;
    double wall_min_us;
    double wall_max_us;
    double avg_points;
    double avg_nodes;
    double avg_leaves;

    kdtree_phase_a_snapshot(&kd_stats);

    wall_total_ms =
        (double)kd_stats.wall_ns_total / 1000000.0;

    cpu_total_ms =
        (double)kd_stats.cpu_ns_total / 1000000.0;

    wall_min_us =
        (double)kd_stats.wall_ns_min / 1000.0;

    wall_max_us =
        (double)kd_stats.wall_ns_max / 1000.0;

    avg_points = 0.0;
    avg_nodes = 0.0;
    avg_leaves = 0.0;

    if (kd_stats.calls > 0) {
        avg_points =
            (double)kd_stats.points_tested /
            (double)kd_stats.calls;

        avg_nodes =
            (double)kd_stats.nodes_visited /
            (double)kd_stats.calls;

        avg_leaves =
            (double)kd_stats.leaves_visited /
            (double)kd_stats.calls;
    }

    logverb("[kd-phase-a] calls=%llu product=%llu fallback=%llu\n",
           (unsigned long long)kd_stats.calls,
           (unsigned long long)kd_stats.product_calls,
           (unsigned long long)kd_stats.fallback_calls);

    logverb("[kd-phase-a] nodes=%llu leaves=%llu points=%llu matches=%llu "
           "avg_nodes=%.2f avg_leaves=%.2f avg_points=%.2f\n",
           (unsigned long long)kd_stats.nodes_visited,
           (unsigned long long)kd_stats.leaves_visited,
           (unsigned long long)kd_stats.points_tested,
           (unsigned long long)kd_stats.matches_found,
           avg_nodes,
           avg_leaves,
           avg_points);

    logverb("[kd-phase-a] frontier_total=%llu submitted=%llu inline=%llu\n",
           (unsigned long long)kd_stats.frontier_total,
           (unsigned long long)kd_stats.tasks_submitted,
           (unsigned long long)kd_stats.tasks_inline);

    logverb("[kd-phase-a] wall_total_ms=%.3f cpu_total_ms=%.3f "
           "wall_min_us=%.3f wall_max_us=%.3f\n",
           wall_total_ms,
           cpu_total_ms,
           wall_min_us,
           wall_max_us);

    logverb("[kd-phase-a] wall_us histogram "
           "<1=%llu 1-5=%llu 5-10=%llu 10-50=%llu "
           "50-100=%llu 100-500=%llu 500-1000=%llu "
           "1-5ms=%llu 5-10ms=%llu >=10ms=%llu\n",
           (unsigned long long)kd_stats.histogram[0],
           (unsigned long long)kd_stats.histogram[1],
           (unsigned long long)kd_stats.histogram[2],
           (unsigned long long)kd_stats.histogram[3],
           (unsigned long long)kd_stats.histogram[4],
           (unsigned long long)kd_stats.histogram[5],
           (unsigned long long)kd_stats.histogram[6],
           (unsigned long long)kd_stats.histogram[7],
           (unsigned long long)kd_stats.histogram[8],
           (unsigned long long)kd_stats.histogram[9]);

  index_shard_pool_release_pass(pool);
  return status;
}


/*
 * SECTION INDEX-SHARD: auxiliary task executor
 *
 * Bounded inner work that reuses the configured index-shard worker pool.
 * In-flight outer work is never preempted; at most one worker can help at an
 * index boundary before returning to the ordered outer queue. A waiting owner
 * also executes ordinary queued work cooperatively.
 */
static int index_shard_aux_queue_init(index_shard_aux_queue_t *q,
                                      size_t max_pending) {
  memset(q, 0, sizeof(*q));

  if (pthread_mutex_init(&q->mutex, NULL)) {
    return -1;
  }

  q->max_pending = max_pending ? max_pending : 64;
  return 0;
}

static void index_shard_aux_queue_destroy(index_shard_aux_queue_t *q) {
  index_shard_aux_task_t *task;
  index_shard_aux_task_t *cursor;
  size_t cancelled_count = 0;

  if (!q) {
    return;
  }

  pthread_mutex_lock(&q->mutex);

  task = q->head;
  q->head = NULL;
  q->tail = NULL;
  q->stopping = TRUE;

  for (cursor = task; cursor; cursor = cursor->next) {
    cancelled_count++;
  }

  q->pending = 0;
  q->cancelled_total += cancelled_count;

  pthread_mutex_unlock(&q->mutex);

  index_shard_aux_cancel_list(task);
  pthread_mutex_destroy(&q->mutex);
}

static int index_shard_aux_queue_push(index_shard_pool_t *pool,
                                      index_shard_aux_task_t *task) {
  index_shard_aux_queue_t *q;
  int accepted = FALSE;

  if (!pool || !task) {
    return -1;
  }

  q = &pool->auxq;
  task->next = NULL;

  /*
   * control_mutex is the common worker sleep/wakeup boundary. Holding it
   * across enqueue and notification prevents a worker from missing an arrival
   * between its queue check and pthread_cond_wait().
   */
  pthread_mutex_lock(&pool->control_mutex);
  pthread_mutex_lock(&q->mutex);

  if (pool->shutdown ||
      pool->stopping ||
      !pool->pass_active ||
      q->stopping ||
      q->pending >= q->max_pending) {
    q->rejected_total++;
  } else {
    if (q->tail) {
      q->tail->next = task;
    } else {
      q->head = task;
    }

    q->tail = task;
    q->pending++;
    q->submitted_total++;
    accepted = TRUE;
  }

  pthread_mutex_unlock(&q->mutex);

  if (accepted) {
    pthread_cond_broadcast(&pool->work_cv);
  }

  pthread_mutex_unlock(&pool->control_mutex);

  return accepted ? 0 : -1;
}

static index_shard_aux_task_t *
index_shard_aux_queue_try_pop(index_shard_aux_queue_t *q,
                              const index_shard_aux_group_t *exclude_group,
                              int lend_only,
                              int skip_lent) {
  index_shard_aux_task_t *task;
  index_shard_aux_task_t *previous = NULL;

  if (!q) {
    return NULL;
  }

  pthread_mutex_lock(&q->mutex);

  task = q->head;

  /*
   * A boundary lender scans only for work belonging to the group holding the
   * one lend token. The owner waiting on that same group excludes its own
   * reserved range so it cannot consume the range before a lender arrives.
   */
  while (task &&
         ((exclude_group && task->group == exclude_group) ||
          (lend_only &&
           (!task->group ||
            !task->group->lend_slot ||
            task->group->lend_claimed)) ||
          (skip_lent &&
           task->group && task->group->lend_slot))) {
    previous = task;
    task = task->next;
  }

  if (task) {
    /*
     * Claim publication and owner timeout arbitration share q->mutex. Once a
     * configured worker marks the range claimed, the owner may no longer
     * revoke the lending token and execute that same range inline.
     */
    if (task->group && task->group->lend_slot) {
      task->group->lend_claimed = TRUE;
    }

    if (previous) {
      previous->next = task->next;
    } else {
      q->head = task->next;
    }

    if (q->tail == task) {
      q->tail = previous;
    }

    task->next = NULL;

    if (q->pending > 0) {
      q->pending--;
    }
  }

  pthread_mutex_unlock(&q->mutex);
  return task;
}

/*
 * Lend one configured outer worker at an index-task boundary.
 *
 * The caller has not claimed another index, so no outer task is displaced
 * after acquisition. Exactly one coarse helper range is executed, then the
 * caller immediately returns to the ordered outer claim loop.
 */
static int index_shard_help_lent_once(index_shard_pool_t *pool) {
  index_shard_aux_task_t *task;

  if (!pool ||
      !index_shard_config_get()->inner_lending_enabled) {
    return FALSE;
  }

  task = index_shard_aux_queue_try_pop(&pool->auxq,
                                       NULL,
                                       TRUE,
                                       FALSE);

  if (!task) {
    return FALSE;
  }

  index_shard_aux_execute_one(task);
  return TRUE;
}

/*
 * SECTION INDEX-SHARD: shared prefetch coordinator
 */

static anbool index_shard_prefetch_is_metadata(
    kdtree_prefetch_array_kind_t kind) {
  return kind == KDTREE_PREFETCH_ARRAY_SPLIT ||
      kind == KDTREE_PREFETCH_ARRAY_SPLITDIM ||
      kind == KDTREE_PREFETCH_ARRAY_BBOX ||
      kind == KDTREE_PREFETCH_ARRAY_LR;
}

static anbool index_shard_prefetch_same_page(
    const index_shard_prefetch_page_t *left,
    const index_shard_prefetch_page_t *right) {
  if (!left || !right) {
    return FALSE;
  }

  return left->fb == right->fb &&
      left->map_base == right->map_base &&
      left->page == right->page;
}

static int index_shard_prefetch_page_key_compare(
    const void *left,
    const void *right) {
  const index_shard_prefetch_page_t *lhs = left;
  const index_shard_prefetch_page_t *rhs = right;

  uintptr_t lhs_fb;
  uintptr_t rhs_fb;
  uintptr_t lhs_map;
  uintptr_t rhs_map;

  lhs_fb = (uintptr_t)lhs->fb;
  rhs_fb = (uintptr_t)rhs->fb;

  if (lhs_fb < rhs_fb) {
    return -1;
  }

  if (lhs_fb > rhs_fb) {
    return 1;
  }

  lhs_map = (uintptr_t)lhs->map_base;
  rhs_map = (uintptr_t)rhs->map_base;

  if (lhs_map < rhs_map) {
    return -1;
  }

  if (lhs_map > rhs_map) {
    return 1;
  }

  if (lhs->page < rhs->page) {
    return -1;
  }

  if (lhs->page > rhs->page) {
    return 1;
  }

  if (lhs->priority < rhs->priority) {
    return -1;
  }

  if (lhs->priority > rhs->priority) {
    return 1;
  }

  if (lhs->kind < rhs->kind) {
    return -1;
  }

  if (lhs->kind > rhs->kind) {
    return 1;
  }

  return 0;
}

static int index_shard_prefetch_page_priority_compare(
    const void *left,
    const void *right) {
  const index_shard_prefetch_page_t *lhs = left;
  const index_shard_prefetch_page_t *rhs = right;

  if (lhs->priority < rhs->priority) {
    return -1;
  }

  if (lhs->priority > rhs->priority) {
    return 1;
  }

  return index_shard_prefetch_page_key_compare(left, right);
}

static int index_shard_prefetch_coordinator_init(
    index_shard_prefetch_coordinator_t *coordinator,
    int worker_count) {
  size_t workers;
  size_t issue_page_budget;
  size_t pending_capacity;
  size_t recent_capacity;
  size_t pass_page_budget;

  if (!coordinator || worker_count <= 0) {
    return -1;
  }

  memset(coordinator, 0, sizeof(*coordinator));

  workers = (size_t)worker_count;

  if (workers >
      SIZE_MAX / INDEX_SHARD_PREFETCH_ISSUE_PAGES_PER_WORKER) {
    return -1;
  }

  issue_page_budget =
      workers * INDEX_SHARD_PREFETCH_ISSUE_PAGES_PER_WORKER;

  if (issue_page_budget >
      SIZE_MAX / INDEX_SHARD_PREFETCH_COLLECT_MULTIPLIER) {
    return -1;
  }

  pending_capacity =
      issue_page_budget * INDEX_SHARD_PREFETCH_COLLECT_MULTIPLIER;

  if (issue_page_budget >
      SIZE_MAX / INDEX_SHARD_PREFETCH_RECENT_MULTIPLIER) {
    return -1;
  }

  recent_capacity =
      issue_page_budget * INDEX_SHARD_PREFETCH_RECENT_MULTIPLIER;

  if (issue_page_budget >
      SIZE_MAX / INDEX_SHARD_PREFETCH_MAX_ISSUE_WINDOWS_PER_PASS) {
    return -1;
  }

  pass_page_budget =
      issue_page_budget *
      INDEX_SHARD_PREFETCH_MAX_ISSUE_WINDOWS_PER_PASS;

  if (pthread_mutex_init(&coordinator->mutex, NULL)) {
    return -1;
  }

  if (pthread_mutex_init(&coordinator->flush_mutex, NULL)) {
    pthread_mutex_destroy(&coordinator->mutex);
    return -1;
  }

  coordinator->pending =
      calloc(pending_capacity,
             sizeof(*coordinator->pending));

  coordinator->snapshot =
      calloc(pending_capacity,
             sizeof(*coordinator->snapshot));

  coordinator->selected =
      calloc(issue_page_budget,
             sizeof(*coordinator->selected));

  coordinator->recent =
      calloc(recent_capacity,
             sizeof(*coordinator->recent));

  if (!coordinator->pending ||
      !coordinator->snapshot ||
      !coordinator->selected ||
      !coordinator->recent) {
    free(coordinator->pending);
    free(coordinator->snapshot);
    free(coordinator->selected);
    free(coordinator->recent);

    pthread_mutex_destroy(&coordinator->flush_mutex);
    pthread_mutex_destroy(&coordinator->mutex);

    memset(coordinator, 0, sizeof(*coordinator));
    return -1;
  }

  coordinator->pending_capacity = pending_capacity;

  coordinator->issue_page_budget = issue_page_budget;

  /*
   * Do not issue until one complete issue window has accumulated.
   * With four workers this is 256 raw page entries.
   */
  coordinator->issue_threshold_pages = issue_page_budget;

  coordinator->pass_page_budget = pass_page_budget;

  coordinator->recent_capacity = recent_capacity;

  coordinator->initialized = TRUE;

  return 0;
}

static void index_shard_prefetch_coordinator_destroy(
    index_shard_prefetch_coordinator_t *coordinator) {
  if (!coordinator || !coordinator->initialized) {
    return;
  }

  free(coordinator->pending);
  free(coordinator->snapshot);
  free(coordinator->selected);
  free(coordinator->recent);

  coordinator->pending = NULL;
  coordinator->snapshot = NULL;
  coordinator->selected = NULL;
  coordinator->recent = NULL;

  pthread_mutex_destroy(&coordinator->flush_mutex);
  pthread_mutex_destroy(&coordinator->mutex);

  memset(coordinator, 0, sizeof(*coordinator));
}

static void index_shard_prefetch_coordinator_reset(
    index_shard_prefetch_coordinator_t *coordinator,
    unsigned long generation) {
  if (!coordinator || !coordinator->initialized) {
    return;
  }

  pthread_mutex_lock(&coordinator->flush_mutex);
  pthread_mutex_lock(&coordinator->mutex);

  coordinator->generation = generation;

  coordinator->pending_count = 0;

  coordinator->pass_pages_issued = 0;

  coordinator->recent_count = 0;
  coordinator->recent_next = 0;

  memset(&coordinator->metrics,
         0,
         sizeof(coordinator->metrics));

  pthread_mutex_unlock(&coordinator->mutex);
  pthread_mutex_unlock(&coordinator->flush_mutex);
}

static void index_shard_prefetch_metrics_snapshot(
    index_shard_prefetch_coordinator_t *coordinator,
    index_shard_prefetch_metrics_snapshot_t *snapshot) {
  if (!snapshot) {
    return;
  }

  memset(snapshot, 0, sizeof(*snapshot));

  if (!coordinator || !coordinator->initialized) {
    return;
  }

  pthread_mutex_lock(&coordinator->mutex);

  snapshot->totals = coordinator->metrics;

  snapshot->pending = coordinator->pending_count;

  snapshot->issue_page_budget =
      coordinator->issue_page_budget;

  snapshot->issue_threshold_pages =
      coordinator->issue_threshold_pages;

  snapshot->pass_page_budget =
      coordinator->pass_page_budget;

  snapshot->pass_pages_issued =
      coordinator->pass_pages_issued;

  snapshot->collection_capacity =
      coordinator->pending_capacity;

  snapshot->recent_capacity =
      coordinator->recent_capacity;

  pthread_mutex_unlock(&coordinator->mutex);
}

static index_shard_pool_t *index_shard_prefetch_session_pool(
    const index_shard_prefetch_session_t *session) {
  if (!session) {
    return NULL;
  }

  return (index_shard_pool_t *)session->pool;
}

static anbool index_shard_prefetch_session_usable(
    const index_shard_prefetch_session_t *session) {
  index_shard_pool_t *pool;
  index_shard_thread_state_t *shared;
  int usable;

  pool = index_shard_prefetch_session_pool(session);

  if (!pool) {
    return FALSE;
  }

  pthread_mutex_lock(&pool->control_mutex);

  usable =
      pool->pass_active &&
      !pool->shutdown &&
      !pool->stopping &&
      pool->generation == session->generation;

  pthread_mutex_unlock(&pool->control_mutex);

  if (!usable) {
    return FALSE;
  }

  shared = &pool->shared;

  pthread_mutex_lock(&shared->state_mutex);

  usable =
      !shared->stop_requested &&
      !shared->fatal_error &&
      !shared->solved_published;

  pthread_mutex_unlock(&shared->state_mutex);

  return usable;
}

static int index_shard_prefetch_sink_enabled(
    void *userdata,
    void *mapping) {
  index_shard_prefetch_session_t *session = userdata;
  fitsbin_t *fb = mapping;

  /*
   * Check immutable per-file policy first. The OFF path performs no pool
   * locking and no predictive traversal.
   */
  if (!fb ||
      fb->mmap_advice != FITSBIN_MMAP_ADVICE_RANDOM ||
      !fb->mmap_prefetch_enabled ||
      !fb->mmap_page_size) {
    return FALSE;
  }

  return index_shard_prefetch_session_usable(session);
}

static size_t index_shard_prefetch_find_worst_priority(
    const index_shard_prefetch_coordinator_t *coordinator) {
  size_t worst = 0;
  size_t i;

  assert(coordinator);
  assert(coordinator->pending_count > 0);

  for (i = 1; i < coordinator->pending_count; i++) {
    if (coordinator->pending[i].priority >
        coordinator->pending[worst].priority) {
      worst = i;
    }
  }

  return worst;
}
static anbool index_shard_prefetch_session_has_page(
    const index_shard_prefetch_session_t *session,
    void *mapping,
    const void *map_base,
    uintptr_t page) {
  size_t i;

  if (!session) {
    return FALSE;
  }

  for (i = 0; i < session->page_count; i++) {
    const index_shard_prefetch_local_page_t *candidate =
        &session->pages[i];

    if (candidate->mapping == mapping &&
        candidate->map_base == map_base &&
        candidate->page == page) {
      return TRUE;
    }
  }

  return FALSE;
}

static void index_shard_prefetch_session_clear(
    index_shard_prefetch_session_t *session) {
  if (!session) {
    return;
  }

  session->page_count = 0;

  session->hints_emitted = 0;
  session->hints_stale = 0;
  session->hints_unmapped = 0;

  session->pages_raw = 0;
  session->pages_local_duplicate = 0;
}
/*
 * Transfers one worker-local page batch into the pool-shared accumulator.
 *
 * No madvise() call occurs here. Kernel advice is issued separately only
 * after the shared accumulation threshold is reached.
 *
 * Return:
 *   1  shared issue threshold reached
 *   0  published, but threshold not reached
 *  -1  invalid or unavailable session
 */
static int index_shard_prefetch_session_publish(
    index_shard_prefetch_session_t *session) {
  index_shard_pool_t *pool;
  index_shard_prefetch_coordinator_t *coordinator;

  int threshold_reached = FALSE;
  size_t i;

  pool = index_shard_prefetch_session_pool(session);

  if (!pool) {
    return -1;
  }

  coordinator = &pool->prefetch;

  if (!coordinator->initialized) {
    index_shard_prefetch_session_clear(session);
    return -1;
  }

  if (!session->page_count &&
      !session->hints_emitted &&
      !session->hints_stale &&
      !session->hints_unmapped &&
      !session->pages_raw &&
      !session->pages_local_duplicate) {
    return 0;
  }

  if (!index_shard_prefetch_session_usable(session)) {
    pthread_mutex_lock(&coordinator->mutex);

    coordinator->metrics.hints_stale +=
        session->hints_emitted +
        session->hints_stale;

    coordinator->metrics.publish_calls++;

    pthread_mutex_unlock(&coordinator->mutex);

    index_shard_prefetch_session_clear(session);
    return 0;
  }

  pthread_mutex_lock(&coordinator->mutex);

  coordinator->metrics.publish_calls++;

  coordinator->metrics.hints_emitted +=
      session->hints_emitted;

  coordinator->metrics.hints_stale +=
      session->hints_stale;

  coordinator->metrics.hints_unmapped +=
      session->hints_unmapped;

  coordinator->metrics.pages_raw +=
      session->pages_raw;

  coordinator->metrics.pages_duplicate +=
      session->pages_local_duplicate;

  if (!session->page_count) {
    coordinator->metrics.publish_empty++;
  }

  for (i = 0; i < session->page_count; i++) {
    index_shard_prefetch_local_page_t *local =
        &session->pages[i];

    index_shard_prefetch_page_t page;

    /*
     * Once the pass-wide issue ceiling is exhausted, additional speculation
     * is discarded instead of becoming an unbounded pending backlog.
     */
    if (coordinator->pass_pages_issued >=
        coordinator->pass_page_budget) {
      coordinator->metrics.pages_budget_dropped++;
      continue;
    }

    memset(&page, 0, sizeof(page));

    page.fb = (fitsbin_t *)local->mapping;
    page.map_base = local->map_base;

    page.page = local->page;
    page.page_size = local->page_size;

    page.priority = local->priority;
    page.kind = local->kind;

    if (coordinator->pending_count <
        coordinator->pending_capacity) {
      coordinator->pending[
          coordinator->pending_count++] = page;

      continue;
    }

    /*
     * Gate 13.1 is metadata-only, but keep the existing priority-preserving
     * saturation behavior for future precision tiers.
     */
    {
      size_t worst =
          index_shard_prefetch_find_worst_priority(coordinator);

      if (page.priority <
          coordinator->pending[worst].priority) {
        coordinator->pending[worst] = page;
      } else {
        coordinator->metrics.pages_collection_dropped++;
      }
    }
  }

  threshold_reached =
      coordinator->pending_count >=
      coordinator->issue_threshold_pages;

  pthread_mutex_unlock(&coordinator->mutex);

  index_shard_prefetch_session_clear(session);

  return threshold_reached;
}
static int index_shard_prefetch_sink_emit(
    void *userdata,
    const kdtree_prefetch_hint_t *hint) {
  index_shard_prefetch_session_t *session = userdata;

  fitsbin_t *fb;

  const void *map_base;
  size_t map_size;

  const void *range_data;
  size_t range_size;

  uintptr_t map_start;
  uintptr_t map_end;

  uintptr_t request_start;
  uintptr_t request_end;

  uintptr_t first_page;
  uintptr_t end_page;

  size_t page_size;
  size_t page_count;
  size_t i;

  int mapped;

  if (!session ||
      !hint ||
      !hint->mapping ||
      !hint->address ||
      !hint->length) {
    return -1;
  }

  if (!index_shard_prefetch_session_usable(session)) {
    session->hints_stale++;
    return 1;
  }

  fb = hint->mapping;

  if (fb->mmap_advice != FITSBIN_MMAP_ADVICE_RANDOM ||
      !fb->mmap_prefetch_enabled ||
      !fb->mmap_page_size) {
    return 1;
  }

  session->hints_emitted++;

  mapped = fitsbin_resolve_mapped_range(
      fb,
      hint->address,
      hint->length,
      &map_base,
      &map_size,
      &range_data,
      &range_size);

  if (mapped <= 0) {
    session->hints_unmapped++;

    return mapped < 0 ? -1 : 1;
  }

  page_size = fb->mmap_page_size;

  map_start = (uintptr_t)map_base;

  if (map_size > UINTPTR_MAX - map_start) {
    return 1;
  }

  map_end = map_start + map_size;

  request_start = (uintptr_t)range_data;

  if (range_size > UINTPTR_MAX - request_start) {
    return 1;
  }

  request_end = request_start + range_size;

  first_page =
      request_start -
      request_start % (uintptr_t)page_size;

  if (first_page < map_start) {
    first_page = map_start;
  }

  end_page = request_end;

  if (end_page % (uintptr_t)page_size) {
    uintptr_t padding =
        (uintptr_t)page_size -
        end_page % (uintptr_t)page_size;

    if (padding > map_end - end_page) {
      end_page = map_end;
    } else {
      end_page += padding;
    }
  }

  if (end_page > map_end) {
    end_page = map_end;
  }

  if (end_page <= first_page) {
    return 1;
  }

  page_count =
      (size_t)((end_page - first_page) /
               (uintptr_t)page_size);

  if (!page_count) {
    return 1;
  }

  session->pages_raw += page_count;

  for (i = 0; i < page_count; i++) {
    uintptr_t page_address;

    page_address =
        first_page +
        (uintptr_t)i * (uintptr_t)page_size;

    if (index_shard_prefetch_session_has_page(
        session,
        fb,
        map_base,
        page_address)) {
      session->pages_local_duplicate++;
      continue;
    }

    /*
     * A full local buffer is published into the shared accumulator, but this
     * still does not necessarily issue kernel advice.
     */
     if (session->page_count >=
        INDEX_SHARD_PREFETCH_LOCAL_PAGE_CAPACITY) {
      int publish_rc =
          index_shard_prefetch_session_publish(session);

      if (publish_rc < 0) {
        return -1;
      }

      if (publish_rc > 0) {
        session->issue_requested = TRUE;
      }
    }

    if (session->page_count <
        INDEX_SHARD_PREFETCH_LOCAL_PAGE_CAPACITY) {
      index_shard_prefetch_local_page_t *page =
          &session->pages[session->page_count++];

      memset(page, 0, sizeof(*page));

      page->mapping = fb;
      page->map_base = map_base;

      page->page = page_address;
      page->page_size = page_size;

      page->priority = hint->priority;
      page->kind = hint->kind;
    }
  }

  return 0;
}

static anbool index_shard_prefetch_recent_contains(
    const index_shard_prefetch_coordinator_t *coordinator,
    const index_shard_prefetch_page_t *page) {
  size_t i;

  if (!coordinator || !page) {
    return FALSE;
  }

  for (i = 0; i < coordinator->recent_count; i++) {
    if (index_shard_prefetch_same_page(
        &coordinator->recent[i],
        page)) {
      return TRUE;
    }
  }

  return FALSE;
}

static void index_shard_prefetch_recent_add(
    index_shard_prefetch_coordinator_t *coordinator,
    const index_shard_prefetch_page_t *page) {
  if (!coordinator ||
      !page ||
      !coordinator->recent_capacity) {
    return;
  }

  if (coordinator->recent_count <
      coordinator->recent_capacity) {
    coordinator->recent[
        coordinator->recent_count++] = *page;

    return;
  }

  coordinator->recent[
      coordinator->recent_next] = *page;

  coordinator->recent_next++;

  if (coordinator->recent_next >=
      coordinator->recent_capacity) {
    coordinator->recent_next = 0;
  }
}

static int index_shard_prefetch_coordinator_issue(
    index_shard_prefetch_session_t *session) {
  index_shard_pool_t *pool;
  index_shard_prefetch_coordinator_t *coordinator;

  size_t snapshot_count;
  size_t unique_count;
  size_t filtered_count;
  size_t selected_count;

  size_t duplicate_count = 0;
  size_t budget_dropped = 0;

  size_t remaining_pass_budget;

  unsigned long long ranges_issued = 0;
  unsigned long long bytes_issued = 0;
  unsigned long long prefetch_failures = 0;

  size_t i;

  pool = index_shard_prefetch_session_pool(session);

  if (!pool) {
    return -1;
  }

  coordinator = &pool->prefetch;

  if (!coordinator->initialized) {
    return 0;
  }

  pthread_mutex_lock(&coordinator->flush_mutex);

  if (!index_shard_prefetch_session_usable(session)) {
    pthread_mutex_lock(&coordinator->mutex);

    coordinator->metrics.hints_stale +=
        coordinator->pending_count;

    coordinator->pending_count = 0;

    pthread_mutex_unlock(&coordinator->mutex);
    pthread_mutex_unlock(&coordinator->flush_mutex);

    return 0;
  }

  pthread_mutex_lock(&coordinator->mutex);

  if (coordinator->generation != session->generation) {
    coordinator->metrics.hints_stale +=
        coordinator->pending_count;

    coordinator->pending_count = 0;

    pthread_mutex_unlock(&coordinator->mutex);
    pthread_mutex_unlock(&coordinator->flush_mutex);

    return 0;
  }

  /*
   * The decisive Gate 13.1 behavior: no kernel advice for tiny batches.
   */
  if (coordinator->pending_count <
      coordinator->issue_threshold_pages) {
    coordinator->metrics.issue_below_threshold++;

    pthread_mutex_unlock(&coordinator->mutex);
    pthread_mutex_unlock(&coordinator->flush_mutex);

    return 0;
  }

  if (coordinator->pass_pages_issued >=
      coordinator->pass_page_budget) {
    coordinator->metrics.pages_budget_dropped +=
        coordinator->pending_count;

    coordinator->metrics.pass_budget_exhausted++;

    coordinator->pending_count = 0;

    pthread_mutex_unlock(&coordinator->mutex);
    pthread_mutex_unlock(&coordinator->flush_mutex);

    return 0;
  }

  snapshot_count = coordinator->pending_count;

  memcpy(coordinator->snapshot,
         coordinator->pending,
         snapshot_count * sizeof(*coordinator->snapshot));

  coordinator->pending_count = 0;

  pthread_mutex_unlock(&coordinator->mutex);

  /*
   * Cross-query and cross-worker page deduplication.
   */
  qsort(coordinator->snapshot,
        snapshot_count,
        sizeof(*coordinator->snapshot),
        index_shard_prefetch_page_key_compare);

  unique_count = 0;

  for (i = 0; i < snapshot_count; i++) {
    if (unique_count > 0 &&
        index_shard_prefetch_same_page(
            &coordinator->snapshot[unique_count - 1],
            &coordinator->snapshot[i])) {
      duplicate_count++;
      continue;
    }

    coordinator->snapshot[unique_count++] =
        coordinator->snapshot[i];
  }

  /*
   * Suppress pages recently issued by this pass generation.
   */
  filtered_count = 0;

  pthread_mutex_lock(&coordinator->mutex);

  for (i = 0; i < unique_count; i++) {
    if (index_shard_prefetch_recent_contains(
        coordinator,
        &coordinator->snapshot[i])) {
      duplicate_count++;
      continue;
    }

    coordinator->snapshot[filtered_count++] =
        coordinator->snapshot[i];
  }

  coordinator->metrics.pages_duplicate += duplicate_count;
  coordinator->metrics.pages_unique += filtered_count;

  remaining_pass_budget =
      coordinator->pass_page_budget -
      coordinator->pass_pages_issued;

  pthread_mutex_unlock(&coordinator->mutex);

  if (!filtered_count) {
    pthread_mutex_unlock(&coordinator->flush_mutex);
    return 0;
  }

  qsort(coordinator->snapshot,
        filtered_count,
        sizeof(*coordinator->snapshot),
        index_shard_prefetch_page_priority_compare);

  selected_count = filtered_count;

  if (selected_count > coordinator->issue_page_budget) {
    budget_dropped +=
        selected_count - coordinator->issue_page_budget;

    selected_count = coordinator->issue_page_budget;
  }

  if (selected_count > remaining_pass_budget) {
    budget_dropped +=
        selected_count - remaining_pass_budget;

    selected_count = remaining_pass_budget;
  }

  if (!selected_count) {
    pthread_mutex_lock(&coordinator->mutex);

    coordinator->metrics.pages_budget_dropped +=
        budget_dropped;

    coordinator->metrics.pass_budget_exhausted++;

    pthread_mutex_unlock(&coordinator->mutex);
    pthread_mutex_unlock(&coordinator->flush_mutex);

    return 0;
  }

  memcpy(coordinator->selected,
         coordinator->snapshot,
         selected_count * sizeof(*coordinator->selected));

  pthread_mutex_lock(&coordinator->mutex);

  coordinator->metrics.flushes++;

  coordinator->metrics.pages_selected +=
      selected_count;

  coordinator->metrics.pages_budget_dropped +=
      budget_dropped;

  coordinator->pass_pages_issued += selected_count;

  for (i = 0; i < selected_count; i++) {
    index_shard_prefetch_recent_add(
        coordinator,
        &coordinator->selected[i]);

    if (index_shard_prefetch_is_metadata(
        coordinator->selected[i].kind)) {
      coordinator->metrics.metadata_pages_selected++;
    } else {
      coordinator->metrics.leaf_pages_selected++;
    }
  }

  pthread_mutex_unlock(&coordinator->mutex);

  /*
   * Merge only adjacent pages within the same actual mmap region.
   */
  qsort(coordinator->selected,
        selected_count,
        sizeof(*coordinator->selected),
        index_shard_prefetch_page_key_compare);

  i = 0;

  while (i < selected_count) {
    index_shard_prefetch_page_t *first =
        &coordinator->selected[i];

    uintptr_t range_start = first->page;
    uintptr_t range_end;

    size_t j = i + 1;
    int rc;

    if (first->page_size >
        UINTPTR_MAX - range_start) {
      i = j;
      continue;
    }

    range_end = range_start + first->page_size;

    while (j < selected_count) {
      index_shard_prefetch_page_t *next =
          &coordinator->selected[j];

      if (next->fb != first->fb ||
          next->map_base != first->map_base ||
          next->page_size != first->page_size ||
          next->page != range_end) {
        break;
      }

      if (next->page_size >
          UINTPTR_MAX - range_end) {
        break;
      }

      range_end += next->page_size;
      j++;
    }

    rc = fitsbin_prefetch_data(
        first->fb,
        (const void *)range_start,
        (size_t)(range_end - range_start));

    ranges_issued++;

    bytes_issued +=
        (unsigned long long)(range_end - range_start);

    if (rc < 0) {
      prefetch_failures++;
    }

    i = j;
  }

  pthread_mutex_lock(&coordinator->mutex);

  coordinator->metrics.ranges_issued +=
      ranges_issued;

  coordinator->metrics.bytes_issued +=
      bytes_issued;

  coordinator->metrics.prefetch_failures +=
      prefetch_failures;

  if (coordinator->pass_pages_issued >=
      coordinator->pass_page_budget) {
    coordinator->metrics.pass_budget_exhausted++;
  }

  pthread_mutex_unlock(&coordinator->mutex);

  pthread_mutex_unlock(&coordinator->flush_mutex);

  return 0;
}

static int index_shard_prefetch_sink_flush(void *userdata) {
  index_shard_prefetch_session_t *session = userdata;
  int publish_rc;
  int issue_rc;

  if (!session) {
    return -1;
  }

  /*
   * Publish the tail of the current worker-local batch.
   */
  publish_rc =
      index_shard_prefetch_session_publish(session);

  if (publish_rc < 0) {
    return -1;
  }

  if (publish_rc > 0) {
    session->issue_requested = TRUE;
  }

  /*
   * A full local publication may have crossed the shared threshold before
   * this final flush. Preserve that notification until the coordinator gets
   * one issue opportunity.
   */
  if (!session->issue_requested) {
    return 0;
  }

  session->issue_requested = FALSE;

  issue_rc =
      index_shard_prefetch_coordinator_issue(session);

  return issue_rc;
}

int index_shard_kdtree_prefetch_sink_init(
    kdtree_prefetch_sink_t *sink,
    index_shard_prefetch_session_t *session) {
  index_shard_pool_t *pool;
  int usable;

  if (!sink || !session) {
    return -1;
  }

  memset(sink, 0, sizeof(*sink));
  memset(session, 0, sizeof(*session));

  pool = index_shard_current_worker_pool;

 if (!pool || !pool->prefetch.initialized) {
  return -1;
}

  pthread_mutex_lock(&pool->control_mutex);

  usable =
      pool->pass_active &&
      !pool->shutdown &&
      !pool->stopping;

  if (usable) {
    session->pool = pool;
    session->generation = pool->generation;
  }

  pthread_mutex_unlock(&pool->control_mutex);

  if (!usable) {
    return -1;
  }

  sink->userdata = session;
  sink->enabled = index_shard_prefetch_sink_enabled;
  sink->emit = index_shard_prefetch_sink_emit;
  sink->flush = index_shard_prefetch_sink_flush;

  return 0;
}

// ANCHOR INDEX-SHARD: auxiliary-group-lifecycle

index_shard_aux_group_t *index_shard_aux_group_new(void) {
  index_shard_aux_group_t *group;
  index_shard_pool_t *pool;
  int usable;

  pool = index_shard_current_worker_pool;

  if (!pool) {
    return NULL;
  }

  pthread_mutex_lock(&pool->control_mutex);
  usable = pool->pass_active && !pool->shutdown && !pool->stopping;
  pthread_mutex_unlock(&pool->control_mutex);

  if (!usable) {
    return NULL;
  }

  group = calloc(1, sizeof(index_shard_aux_group_t));

  if (!group) {
    return NULL;
  }

  if (pthread_mutex_init(&group->mutex, NULL)) {
    free(group);
    return NULL;
  }

  if (pthread_cond_init(&group->cv, NULL)) {
    pthread_mutex_destroy(&group->mutex);
    free(group);
    return NULL;
  }

  group->pool = pool;
  return group;
}

void index_shard_aux_group_free(index_shard_aux_group_t *group) {
  index_shard_pool_t *pool;

  if (!group) {
    return;
  }

  pool = group->pool;

  pthread_mutex_lock(&group->mutex);
  group->closed = TRUE;
  pthread_mutex_unlock(&group->mutex);

  /*
   * Normal callers wait explicitly. This cooperative wait is the lifecycle
   * backstop that prevents freeing a group with accepted tasks outstanding.
   */
  (void)index_shard_kdtree_wait(group);

  /*
   * pending is now zero, so no queued task can retain this group. Release the
   * single lending token before destroying the group storage.
   */
  if (pool) {
    pthread_mutex_lock(&pool->auxq.mutex);

    if (pool->lend_group == group) {
      pool->lend_group = NULL;
    }

    group->lend_slot = FALSE;
    group->lend_claimed = FALSE;
    pthread_mutex_unlock(&pool->auxq.mutex);
  }

  pthread_cond_destroy(&group->cv);
  pthread_mutex_destroy(&group->mutex);
  free(group);
}

static int index_shard_kdtree_submit(void *userdata,
                                     kdtree_task_fn fn,
                                     void *task_userdata) {
  index_shard_aux_group_t *group = userdata;
  index_shard_aux_task_t *task;
  index_shard_pool_t *pool;

  if (!group || !fn) {
    return -1;
  }

  pool = group->pool;

  if (!pool) {
    return -1;
  }

  task = calloc(1, sizeof(index_shard_aux_task_t));

  if (!task) {
    return -1;
  }

  task->fn = fn;
  task->userdata = task_userdata;
  task->group = group;

  /*
   * Reserve pending credit before publishing the task. Completion can happen
   * immediately after enqueue, so reservation must precede publication.
   */
  pthread_mutex_lock(&group->mutex);

  if (group->closed) {
    pthread_mutex_unlock(&group->mutex);
    free(task);
    return -1;
  }

  group->pending++;

  pthread_mutex_unlock(&group->mutex);

  if (index_shard_aux_queue_push(pool, task)) {
    /*
     * Submission rejection means the caller may execute this work inline.
     * Roll back the reservation, but do not mark the group failed.
     */
    pthread_mutex_lock(&group->mutex);

    if (group->pending > 0) {
      group->pending--;
    } else {
      group->failed = TRUE;
      logerr("[index-shard] aux group pending underflow after rejection\n");
    }

    group->progress++;
    pthread_cond_broadcast(&group->cv);

    pthread_mutex_unlock(&group->mutex);

    free(task);
    return -1;
  }

  pthread_mutex_lock(&group->mutex);

  group->progress++;
  pthread_cond_broadcast(&group->cv);

  pthread_mutex_unlock(&group->mutex);

  return 0;
}

/*
 * Wait on a default pthread condition variable for at most seconds.
 *
 * The lending gate uses a wall-clock timeout only as a deadlock/stall bound;
 * it does not influence hypothesis ordering or scientific acceptance.
 */
static int index_shard_cond_wait_seconds(pthread_cond_t *cv,
                                         pthread_mutex_t *mutex,
                                         double seconds) {
  struct timespec wake;
  time_t whole_seconds;
  long nanoseconds;

  if (!cv || !mutex || seconds <= 0.0) {
    return ETIMEDOUT;
  }

  if (clock_gettime(CLOCK_REALTIME, &wake)) {
    return errno ? errno : EINVAL;
  }

  whole_seconds = (time_t)seconds;
  nanoseconds =
      (long)((seconds - (double)whole_seconds) * 1000000000.0);

  wake.tv_sec += whole_seconds;
  wake.tv_nsec += nanoseconds;

  if (wake.tv_nsec >= 1000000000L) {
    wake.tv_sec++;
    wake.tv_nsec -= 1000000000L;
  }

  return pthread_cond_timedwait(cv, mutex, &wake);
}

static int index_shard_kdtree_wait(void *userdata) {
  index_shard_aux_group_t *group = userdata;
  index_shard_pool_t *pool;
  double lend_wait_deadline = 0.0;
  int failed = FALSE;

  if (!group) {
    return -1;
  }

  pool = group->pool;

  if (!pool) {
    return -1;
  }

  while (1) {
    index_shard_aux_task_t *task;
    index_shard_pass_state_snapshot_t state;
    int lend_active;
    int lend_claimed;
    int pass_healthy;
    int pending;
    unsigned long observed_progress;

    pthread_mutex_lock(&group->mutex);

    pending = group->pending;
    observed_progress = group->progress;

    if (pending == 0) {
      failed = group->failed;
      pthread_mutex_unlock(&group->mutex);
      break;
    }

    pthread_mutex_unlock(&group->mutex);

    /*
     * The owner must leave its reserved helper range queued long enough for a
     * configured outer worker to reach an index boundary and borrow it. If
     * the pass becomes unhealthy, release the reservation and execute inline
     * so cancellation or fatal shutdown can never deadlock the fork/join.
     */
    pthread_mutex_lock(&pool->shared.queue_mutex);
    index_shard_pass_state_snapshot(&pool->shared, &state);
    pass_healthy =
        !state.stop_requested &&
        !state.fatal_error &&
        !state.solved_published &&
        !pool->shared.have_solved_order;
    pthread_mutex_unlock(&pool->shared.queue_mutex);

    pthread_mutex_lock(&pool->auxq.mutex);
    lend_active =
        pool->lend_group == group && group->lend_slot;
    lend_claimed = lend_active && group->lend_claimed;

    if (lend_active && !lend_claimed) {
      double now = timenow();

      if (lend_wait_deadline <= 0.0) {
        lend_wait_deadline =
            now + INDEX_SHARD_LEND_OWNER_WAIT_SECONDS;
      }

      /*
       * Revoke only an unclaimed reservation. The same auxq mutex protects a
       * lender's claim publication, so this cannot duplicate helper work.
       */
      if (!pass_healthy || now >= lend_wait_deadline) {
        pool->lend_group = NULL;
        group->lend_slot = FALSE;
        group->lend_claimed = FALSE;
        pool->lend_fallback_total++;
        lend_active = FALSE;
        lend_claimed = FALSE;
        lend_wait_deadline = 0.0;
      }
    } else if (!lend_active || lend_claimed) {
      lend_wait_deadline = 0.0;
    }

    pthread_mutex_unlock(&pool->auxq.mutex);

    /*
     * Without a lend token, retain the original cooperative no-deadlock
     * behavior. With a token, the owner may help other groups but excludes
     * its own reserved range.
     */
    task = index_shard_aux_queue_try_pop(
        &pool->auxq,
        lend_active ? group : NULL,
        FALSE,
        TRUE);

    if (task) {
      index_shard_aux_execute_one(task);
      continue;
    }

    /*
     * The queue is empty. Outstanding work is either executing or between
     * pending reservation and enqueue. Wait for progress instead of spinning.
     */
    pthread_mutex_lock(&group->mutex);

    while (group->pending > 0 &&
           group->progress == observed_progress) {
      if (lend_active && !lend_claimed) {
        double remaining = lend_wait_deadline - timenow();

        if (remaining <= 0.0) {
          break;
        }

        (void)index_shard_cond_wait_seconds(
            &group->cv,
            &group->mutex,
            remaining);
        break;
      }

      pthread_cond_wait(&group->cv, &group->mutex);
    }

    pthread_mutex_unlock(&group->mutex);
  }

  return failed ? -1 : 0;
}

int index_shard_kdtree_executor_init(kdtree_task_executor_t *executor,
                                     index_shard_aux_group_t *group) {
  index_shard_pool_t *pool;
  int usable;

  if (!executor || !group) {
    return -1;
  }

  pool = group->pool;

  if (!pool) {
    return -1;
  }

  pthread_mutex_lock(&pool->control_mutex);
  usable = pool->pass_active && !pool->shutdown && !pool->stopping;
  pthread_mutex_unlock(&pool->control_mutex);

  if (!usable) {
    return -1;
  }

  executor->userdata = group;
  executor->submit = index_shard_kdtree_submit;
  executor->wait = index_shard_kdtree_wait;
  executor->capacity = index_shard_kdtree_capacity;

  return 0;
}

// ANCHOR INDEX-SHARD: auxiliary-task-execution

static void index_shard_aux_group_done(index_shard_aux_group_t *group,
                                       int failed) {
  if (!group) {
    return;
  }

  pthread_mutex_lock(&group->mutex);

  if (failed) {
    group->failed = TRUE;
  }

  if (group->pending > 0) {
    group->pending--;
  } else {
    group->failed = TRUE;
    logerr("[index-shard] aux group pending underflow on completion\n");
  }

  group->progress++;
  pthread_cond_broadcast(&group->cv);

  pthread_mutex_unlock(&group->mutex);
}

static void index_shard_aux_cancel_list(index_shard_aux_task_t *task) {
  while (task) {
    index_shard_aux_task_t *next = task->next;

    index_shard_aux_group_done(task->group, TRUE);
    free(task);
    task = next;
  }
}

static void index_shard_aux_execute_one(index_shard_aux_task_t *task) {
  index_shard_pool_t *pool = NULL;
  int failed = FALSE;

  if (!task) {
    return;
  }

  if (task->group) {
    pool = task->group->pool;
  }

  if (task->fn) {
    task->fn(task->userdata);
  } else {
    failed = TRUE;
  }

  if (pool) {
    pthread_mutex_lock(&pool->auxq.mutex);

    if (task->group &&
        task->group->lend_slot &&
        task->group->lend_claimed) {
      pool->lend_tasks_total++;
    }

    pool->auxq.executed_total++;
    pthread_mutex_unlock(&pool->auxq.mutex);
  }

  index_shard_aux_group_done(task->group, failed);
  free(task);
}

// ANCHOR INDEX-SHARD: auxiliary-public-api

int index_shard_aux_available(void) {
  index_shard_pool_t *pool;
  int available;

  pool = index_shard_current_worker_pool;

  if (!pool) {
    return FALSE;
  }

  pthread_mutex_lock(&pool->control_mutex);
  available = pool->pass_active && !pool->shutdown && !pool->stopping;
  pthread_mutex_unlock(&pool->control_mutex);

  return available;
}

int index_shard_aux_capacity(kdtree_task_capacity_t *capacity) {
  index_shard_pool_t *pool;

  if (!capacity) {
    return -1;
  }

  pool = index_shard_current_worker_pool;

  if (!pool) {
    memset(capacity, 0, sizeof(*capacity));
    return -1;
  }

  return index_shard_pool_capacity(pool, NULL, capacity);
}

static int index_shard_pool_capacity(index_shard_pool_t *pool,
                                     index_shard_aux_group_t *group,
                                     kdtree_task_capacity_t *capacity) {
  index_shard_thread_state_t *shared;

  size_t pending;
  size_t max_pending;
  size_t room;

  size_t workers_total;
  size_t outer_active_limit;
  size_t outer_running;
  size_t spare_workers;

  index_shard_pass_state_snapshot_t state;
  int healthy;
  int outer_work_claimable;
  int have_solved_order;
  int lend_available;

  if (!pool || !capacity) {
    return -1;
  }

  memset(capacity, 0, sizeof(*capacity));

  shared = &pool->shared;

  /*
   * Snapshot the outer scheduler.
   *
   * The pool-wide configured worker count is the total CPU budget. The pass
   * worker count and active_limit cap outer index solves only; they may be
   * smaller than the pool when few indexes apply. Such unassigned workers,
   * plus workers which have finished their outer claims, can help inner work.
   * active_workers only counts participants still completing the outer pass.
   */
  pthread_mutex_lock(&shared->queue_mutex);

  workers_total = 0;
  outer_active_limit = 0;
  outer_running = 0;
  spare_workers = 0;

  if (pool->worker_count > 0) {
    workers_total = (size_t)pool->worker_count;
  }

  if (shared->running_tasks > 0) {
    outer_running = (size_t)shared->running_tasks;
  }

  if (shared->active_limit > 0) {
    outer_active_limit = (size_t)shared->active_limit;
  }

  index_shard_pass_state_snapshot(shared, &state);
  have_solved_order = shared->have_solved_order;

  outer_work_claimable = FALSE;
  healthy =
      !state.stop_requested &&
      !state.fatal_error &&
      !state.solved_published &&
      !have_solved_order;

  if (healthy &&
      shared->next_task < shared->ntasks) {
    outer_work_claimable = TRUE;
  }

  /*
   * Natural spare-worker capacity appears only after the outer queue is
   * exhausted. The single boundary-lending exception is evaluated separately
   * below and never interrupts an in-flight outer task.
   */
  if (healthy &&
      !outer_work_claimable &&
      workers_total > outer_running) {
    spare_workers = workers_total - outer_running;
  }

  pthread_mutex_unlock(&shared->queue_mutex);

  pthread_mutex_lock(&pool->auxq.mutex);

  pending = pool->auxq.pending;
  max_pending = pool->auxq.max_pending;

  if (max_pending > pending) {
    room = max_pending - pending;
  } else {
    room = 0;
  }

  lend_available =
      index_shard_config_get()->inner_lending_enabled &&
      healthy &&
      room > 0 &&
      outer_work_claimable &&
      workers_total > 1 &&
      outer_active_limit > 1 &&
      outer_running > 1 &&
      (!pool->lend_group || pool->lend_group == group);

  /*
   * Naturally idle configured workers remain the first choice. If the outer
   * queue drains after a group acquires the token, convert the group back to
   * ordinary spare-worker execution before publishing its helper range.
   */
  if (spare_workers > 0 &&
      group &&
      pool->lend_group == group) {
    pool->lend_group = NULL;
    group->lend_slot = FALSE;
    group->lend_claimed = FALSE;
  }

  capacity->workers_total = workers_total;
  capacity->aux_pending = pending;
  capacity->aux_room = room;

  /*
   * One product subtree executes inline in the parent. This count represents
   * only additional subtrees that may be offered asynchronously.
   */
  capacity->suggested_subtasks = spare_workers;

  /*
   * When all workers currently own outer indexes and more ordered indexes
   * remain, offer exactly one helper range. The group-aware executor probe
   * atomically reserves that range. A worker finishing its current index then
   * executes the range before claiming the next index and returns immediately
   * to the outer queue. No thread is added and no in-flight index is stopped.
   */
  if (capacity->suggested_subtasks == 0 && lend_available) {
    capacity->suggested_subtasks = 1;

    if (group && !pool->lend_group) {
      pool->lend_group = group;
      group->lend_slot = TRUE;
      group->lend_claimed = FALSE;
      pool->lend_acquired_total++;
    }
  } else if (capacity->suggested_subtasks == 0 &&
             index_shard_config_get()->inner_lending_enabled &&
             healthy &&
             outer_work_claimable &&
             pool->lend_group &&
             pool->lend_group != group) {
    pool->lend_busy_total++;
  }

  if (capacity->suggested_subtasks > room) {
    capacity->suggested_subtasks = room;
  }

  pthread_mutex_unlock(&pool->auxq.mutex);

  return 0;
}

static int index_shard_kdtree_capacity(void *userdata,
                                       kdtree_task_capacity_t *capacity) {
  index_shard_aux_group_t *group = userdata;

  if (!group) {
    return -1;
  }

  return index_shard_pool_capacity(group->pool, group, capacity);
}
