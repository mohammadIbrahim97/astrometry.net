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
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <sched.h>

#include "astrometry/index_shard.h"
#include "astrometry/bl.h"
#include "astrometry/errors.h"
#include "astrometry/log.h"
#include "astrometry/tic.h"
#include "astrometry/kdtree_phase_a.h"

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

  double wall_seconds; // task wall time, excludes reducer wait
  float cpu_seconds;   // process CPU delta around this task

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
  size_t next_task; // next task in simple ordered queue

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
  size_t earliest_solved_order; // prevents claiming later work after solve

  int limit_reported; // avoid repeated CPU-limit log spam

  double pass_wall_start;
  float pass_cpu_start;
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
};

typedef struct index_shard_aux_queue {
  pthread_mutex_t mutex;
  pthread_cond_t cv;

  index_shard_aux_task_t *head;
  index_shard_aux_task_t *tail;

  size_t pending;
  size_t max_pending;

  unsigned long long submitted_total;
  unsigned long long executed_total;
  unsigned long long rejected_total;

  int stopping;
} index_shard_aux_queue_t;
// ANCHOR INDEX-SHARD: pool-state
/*
 * Persistent worker pool for one engine job.
 *
 * The pool survives across multiple onefield_run() submissions.  Workers sleep
 * between generations and wake when index_shard_pool_submit() increments
 * generation.
 */
struct index_shard_pool {
  onefield_t *bp;
  solver_t *base_sp;

  int worker_count;
  pthread_t *threads;
  index_shard_worker_context_t *contexts;

  pthread_mutex_t control_mutex;
  pthread_cond_t work_cv;

  int shutdown;
  unsigned long generation; // pass submission counter

  index_shard_thread_state_t shared;
  index_shard_aux_queue_t auxq;

  pthread_mutex_t aux_mutex;
  pthread_cond_t aux_cv;
  index_shard_aux_task_t *aux_head;
  index_shard_aux_task_t *aux_tail;
  int aux_stopping;
} ;

static index_shard_pool_t *index_shard_global_pool = NULL;
static pthread_mutex_t index_shard_global_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_key_t index_shard_tls_key;
static pthread_once_t index_shard_tls_once = PTHREAD_ONCE_INIT;

static int index_shard_aux_queue_init(index_shard_aux_queue_t *q,
                                      size_t max_pending);
static void index_shard_aux_queue_destroy(index_shard_aux_queue_t *q);
static int index_shard_aux_queue_push(index_shard_aux_queue_t *q,
                                      index_shard_aux_task_t *task);
static index_shard_aux_task_t *index_shard_aux_queue_try_pop(index_shard_aux_queue_t *q);
static index_shard_aux_task_t *index_shard_aux_try_pop(index_shard_pool_t *pool);
static void index_shard_aux_execute_one(index_shard_aux_task_t *task);
static void index_shard_aux_execute(index_shard_aux_task_t *task);
static int index_shard_aux_submit(index_shard_aux_group_t *group,
                                  kdtree_task_fn fn,
                                  void *userdata);
static int index_shard_kdtree_capacity(void *userdata,
                                       kdtree_task_capacity_t *capacity);
static int index_shard_aux_wait(index_shard_aux_group_t *group);

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
 * SECTION INDEX-SHARD: env
 *
 * Runtime controls:
 *   ASTROMETRY_INDEX_SHARDING=pthread  -> enable pthread shard mode
 *   ASTROMETRY_INDEX_SHARD_WORKERS=N   -> requested worker count
 *   ASTROMETRY_INDEX_SHARD_TRACE=1     -> verbose per-task diagnostics
 *
 * Avoid adding more env knobs unless the behavior is truly experimental.
 */
anbool index_shard_pthread_enabled(void) {
  const char *mode = getenv("ASTROMETRY_INDEX_SHARDING");
  return mode && !strcmp(mode, "pthread");
}

anbool index_shard_trace_enabled(void) {
  const char *trace = getenv("ASTROMETRY_INDEX_SHARD_TRACE");

  if (!trace || !trace[0])
    return FALSE;

  return (!strcmp(trace, "1") || !strcasecmp(trace, "true") || !strcasecmp(trace, "yes"));
}

static int index_shard_get_worker_count(size_t nindexes) {
  // cap by nindexes for small passes
  // default to online CPU count, fallback to 2 if sysconf is unavailable
  const char *env = getenv("ASTROMETRY_INDEX_SHARD_WORKERS");
  long n = 0;

  if (env && env[0]) {
    char *end = NULL;

    errno = 0;
    n = strtol(env, &end, 10);

    if (errno || end == env || n <= 0) {
      logmsg("[index-shard] invalid ASTROMETRY_INDEX_SHARD_WORKERS=%s; using default\n", env);
      n = 0;
    }
  }

  if (n <= 0) {
#ifdef _SC_NPROCESSORS_ONLN
    n = sysconf(_SC_NPROCESSORS_ONLN);
#else
    n = 2;
#endif
  }

  if (n <= 0)
    n = 2;

  if (nindexes && (size_t)n > nindexes)
    n = (long)nindexes;

  if (n < 1)
    n = 1;

  if (n > INT_MAX)
    n = INT_MAX;

  return (int)n;
}

typedef struct index_shard_pass_state_snapshot {
  int stop_requested;
  int fatal_error;
  int solved_published;
  int master_committed;
} index_shard_pass_state_snapshot_t;

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
  pthread_mutex_lock(&shared->state_mutex);
  shared->stop_requested = TRUE;
  pthread_mutex_unlock(&shared->state_mutex);

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

// ANCHOR INDEX-SHARD: global-cpu-limit
/*
 * Process-wide CPU budget check.
 *
 * bp->total_cpulimit is not divided by worker count.  With N active threads,
 * CPU time accumulates roughly N times faster than wall time.
 */
static int index_shard_check_global_cpu_limit(index_shard_thread_state_t *shared) {
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
  } else if (bp->total_cpulimit > 0.0) {
    float now = get_cpu_usage();
    double elapsed = (double)(now - bp->cpu_total_start);

    if (elapsed >= bp->total_cpulimit) {
      bp->hit_total_cpulimit = TRUE;
      hit = TRUE;

      if (!shared->limit_reported) {
        shared->limit_reported = TRUE;
        logmsg("Total CPU time limit reached!\n");
        logmsg("[index-shard] cpu-budget reached total_cpulimit=%g elapsed=%.3f\n",
               bp->total_cpulimit, elapsed);
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

  if (index_shard_check_global_cpu_limit(&ctx->pool->shared)) {
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

/*
 * SECTION INDEX-SHARD: queue
 */
// ANCHOR INDEX-SHARD: build-task-plan
/*
 * Build ordered one-index tasks for the current pass.
 *
 * No cost sorting, grouping, or chunking here.  The task order mirrors the
 * candidate index order received from onefield/engine.
 */
static index_shard_task_t *index_shard_build_task_plan(size_t nindexes) {
  size_t i;
  index_shard_task_t *tasks;

  if (!nindexes)
    return NULL;

  tasks = calloc(nindexes, sizeof(index_shard_task_t));
  if (!tasks) {
    SYSERROR("Failed to allocate index-shard task plan");
    return NULL;
  }

  for (i = 0; i < nindexes; i++)
    tasks[i].index_order = i;

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
                                 size_t *index_order) {
  index_shard_pass_state_snapshot_t state;

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

  if (shared->have_solved_order &&
      shared->tasks[shared->next_task].index_order >
          shared->earliest_solved_order) {
    pthread_mutex_unlock(&shared->queue_mutex);
    return FALSE;
  }

  *index_order = shared->tasks[shared->next_task].index_order;
  shared->next_task++;
  shared->running_tasks++;

  if (index_shard_trace_enabled()) {
    logmsg("[index-shard] claim index_order=%zu running=%i active_limit=%i\n",
           *index_order, shared->running_tasks, shared->active_limit);
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
static void index_shard_release_active_credit(index_shard_thread_state_t *shared) {
  // release queue credit before publishing completion
  // completed flag is the reducer visibility boundary
  pthread_mutex_lock(&shared->queue_mutex);

  if (shared->running_tasks > 0)
    shared->running_tasks--;

  pthread_cond_broadcast(&shared->queue_cv);
  pthread_mutex_unlock(&shared->queue_mutex);
}

static void index_shard_mark_result_completed(index_shard_thread_state_t *shared,
                                              size_t index_order) {
  /*
   * NOTE INDEX-SHARD: claimed-task-invariant
   *
   * Every claimed task must release its active credit exactly once and mark
   * completion exactly once.  Otherwise the reducer can wait forever.
   */
  index_shard_release_active_credit(shared);

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
  // local_bp reused across tasks, but solutions change per task
  shared->hooks->reset_local_context_for_task(&ctx->local_bp, result->solutions);

  index = index_shard_worker_get_index(ctx, shared, index_order);
  if (!index) {
    ERROR("Failed to load index order %zu", index_order);
    result->failed = TRUE;
    result->rc = -1;
    return -1;
  }

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
  index_shard_capture_solution_analysis(shared, result);

  if (ctx->local_bp.single_field_solved)
    result->solved = TRUE;

  // release through original onefield ownership hook
  if (shared->hooks->done_with_index)
    shared->hooks->done_with_index(shared->bp, index_order, index);

  if (index_shard_trace_enabled()) {
    logmsg("[index-shard] worker=%i start index_order=%zu index=%s\n", ctx->worker_id, index_order,
           index->indexname ? index->indexname : "(null)");
  }

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

  if (!ctx)
    return NULL;

  pool = ctx->pool;
  if (!pool)
    return NULL;

  index_shard_current_worker_pool = pool;

  while (1) {
    index_shard_thread_state_t *shared;
    size_t index_order;
    index_shard_aux_task_t *aux_task;

    /*
     * Unified pool: run pending inner work before claiming new outer work.
     * This is not a reserved helper thread; every worker follows this rule.
     */
    aux_task = index_shard_aux_queue_try_pop(&pool->auxq);
    if (aux_task) {
      index_shard_aux_execute_one(aux_task);
      continue;
    }

    pthread_mutex_lock(&pool->control_mutex);

    while (!pool->shutdown && ctx->generation_seen == pool->generation)
      pthread_cond_wait(&pool->work_cv, &pool->control_mutex);

    if (pool->shutdown) {
      pthread_mutex_unlock(&pool->control_mutex);
      break;
    }

    ctx->generation_seen = pool->generation;
    shared = &pool->shared;

    if (ctx->worker_id >= shared->worker_count) {
      pthread_mutex_unlock(&pool->control_mutex);
      continue;
    }

    pthread_mutex_unlock(&pool->control_mutex);

    if (index_shard_worker_prepare_pass(ctx, shared)) {
      index_shard_request_fatal_stop(shared);
      index_shard_worker_done(shared);
      continue;
    }

    while (index_shard_claim_one(shared, &index_order)) {
      index_shard_result_t *result = &shared->results[index_order];

      if (index_shard_check_global_cpu_limit(shared) ||
          index_shard_master_stop_requested(shared)) {
        index_shard_master_limit_or_cancel_requested(
            shared,
            &result->hit_total_cpulimit,
            &result->cancelled);
        index_shard_mark_result_completed(shared, index_order);
        break;
      }
      if (index_shard_run_one_with_worker_context(ctx, shared,
                                                  index_order, result)) {
        index_shard_mark_result_completed(shared, index_order);
        index_shard_request_fatal_stop(shared);
        break;
      }

      if (result->solved) {
        logmsg("[index-shard] solved-candidate worker=%i index_order=%zu "
               "best_logodds=%.3f field=%i wall=%.3f cpu=%.3f\n",
               ctx->worker_id, index_order,
               result->best_logodds, result->best_fieldnum,
               result->wall_seconds, (double)result->cpu_seconds);

        index_shard_publish_solved(shared, index_order);
        index_shard_request_stop(shared);
      }

      index_shard_check_global_cpu_limit(shared);
      index_shard_mark_result_completed(shared, index_order);
    }

    index_shard_worker_cleanup_pass(ctx, shared);
    index_shard_worker_done(shared);

    /*
     * After pass accounting is complete, this worker can again help inner work.
     */
    aux_task = index_shard_aux_queue_try_pop(&pool->auxq);
    if (aux_task)
      index_shard_aux_execute_one(aux_task);
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
  if (!index_shard_pthread_enabled())
    return 0;

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  if (index_shard_global_pool) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return 0;
  }

  worker_count = index_shard_get_worker_count(0);

  pool = calloc(1, sizeof(index_shard_pool_t));
  if (!pool) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    SYSERROR("Failed to allocate index-shard pool");
    return -1;
  }

  pool->bp = bp;
  pool->base_sp = sp;
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
  aux_max_pending = pool->worker_count * pool->worker_count;

  if (aux_max_pending < pool->worker_count)
    aux_max_pending = pool->worker_count;

   if (index_shard_aux_queue_init(&pool->auxq, aux_max_pending)) {
    index_shard_shared_destroy(&pool->shared);
    pthread_cond_destroy(&pool->work_cv);
    pthread_mutex_destroy(&pool->control_mutex);
    free(pool);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  pool->threads = calloc((size_t)worker_count, sizeof(pthread_t));
  pool->contexts = calloc((size_t)worker_count, sizeof(index_shard_worker_context_t));

  if (!pool->threads || !pool->contexts) {
    free(pool->threads);
    free(pool->contexts);
    index_shard_shared_destroy(&pool->shared);
    index_shard_aux_queue_destroy(&pool->auxq);
    pthread_cond_destroy(&pool->work_cv);
    pthread_mutex_destroy(&pool->control_mutex);
    free(pool);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    SYSERROR("Failed to allocate index-shard workers");
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

      for (j = 0; j < i; j++)
        pthread_join(pool->threads[j], NULL);

      free(pool->threads);
      free(pool->contexts);
      index_shard_shared_destroy(&pool->shared);
      pthread_cond_destroy(&pool->work_cv);
      pthread_mutex_destroy(&pool->control_mutex);
      free(pool);
      pthread_mutex_unlock(&index_shard_global_pool_mutex);
      SYSERROR("Failed to create index-shard worker");
      return -1;
    }
  }

  index_shard_global_pool = pool;

  logmsg("[index-shard] pthread-pool start workers=%i scheduler=ordered chunk=1\n", worker_count);

  pthread_mutex_unlock(&index_shard_global_pool_mutex);
  return 0;
}
// ANCHOR INDEX-SHARD: pool-stop
/*
 * Stop pool and join all workers.
 *
 */
void index_shard_pool_stop(onefield_t *bp) {
  index_shard_pool_t *pool;
  int i;

  (void)bp;
  // detach global pointer first so no new submit can find this pool
  pthread_mutex_lock(&index_shard_global_pool_mutex);
  pool = index_shard_global_pool;
  index_shard_global_pool = NULL;
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  if (!pool)
    return;

  pthread_mutex_lock(&pool->control_mutex);
  pool->shutdown = TRUE;
  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);


  pthread_mutex_lock(&pool->auxq.mutex);
  pool->auxq.stopping = 1;
  pthread_cond_broadcast(&pool->auxq.cv);
  pthread_mutex_unlock(&pool->auxq.mutex);
  // wake sleeping workers so they can exit normally
  for (i = 0; i < pool->worker_count; i++)
    pthread_join(pool->threads[i], NULL);

  free(pool->threads);
  free(pool->contexts);

  logmsg("[index-shard] auxq submitted=%llu executed=%llu rejected=%llu "
       "pending=%zu max_pending=%zu\n",
       pool->auxq.submitted_total,
       pool->auxq.executed_total,
       pool->auxq.rejected_total,
       pool->auxq.pending,
       pool->auxq.max_pending);

  index_shard_shared_destroy(&pool->shared);
  index_shard_aux_queue_destroy(&pool->auxq);

  pthread_cond_destroy(&pool->work_cv);
  pthread_mutex_destroy(&pool->control_mutex);

  logmsg("[index-shard] pthread-pool stop\n");

  free(pool);
}

// ANCHOR INDEX-SHARD: pool-active
/*
 * Lightweight guard used by onefield_run() before entering pthread path.
 */
int index_shard_pool_active(onefield_t *bp) {
  int active;

  (void)bp;

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  active = (index_shard_global_pool != NULL);
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
                                   unsigned char *completed) {
  index_shard_thread_state_t *shared = &pool->shared;
  int worker_count = index_shard_get_worker_count(nindexes);

  // current pass may use fewer workers than the pool owns
  if (worker_count > pool->worker_count)
    worker_count = pool->worker_count;

  pthread_mutex_lock(&pool->control_mutex);

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

  // pass timing excludes final solve-field output generation
  shared->pass_wall_start = timenow();
  shared->pass_cpu_start = get_cpu_usage();

  pthread_mutex_unlock(&shared->limit_mutex);
  pthread_mutex_unlock(&shared->state_mutex);
  pthread_mutex_unlock(&shared->result_mutex);
  pthread_mutex_unlock(&shared->queue_mutex);

  // generation publish wakes workers for this pass
  pool->bp = bp;
  pool->base_sp = base_sp;
  pool->generation++;

  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);

  logmsg("[index-shard] pthread-pool submit workers=%i candidates=%zu "
         "startobj=%i endobj=%i scheduler=ordered chunk=1 active_limit=%i\n",
         worker_count, nindexes, base_sp->startobj, base_sp->endobj, shared->active_limit);

  return 0;
}

/*
 * SECTION INDEX-SHARD: entry
 */

// ANCHOR INDEX-SHARD: entry
/*
 * Execute pthread index-shard pass for one onefield_run() call.
 *
 * Returns:
 *   0  -> shard path handled the pass
 *   1  -> pthread mode inactive / pool inactive, caller may use fallback
 *  -1  -> hard shard failure
 */
index_shard_solve_status_t index_shard_solve(onefield_t *bp,
                  solver_t *base_sp,
                  size_t nindexes,
                  const index_shard_hooks_t *hooks) {
  index_shard_pool_t *pool;
  index_shard_task_t *tasks = NULL;
  index_shard_result_t *results = NULL;
  unsigned char *completed = NULL;
  size_t i;
  int rc = 0;
  index_shard_pass_state_snapshot_t state;
  index_shard_solve_status_t status = INDEX_SHARD_SOLVE_HANDLED;

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

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  pool = index_shard_global_pool;
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  if (!pool) {
    logmsg("[index-shard] pthread mode requested but pool inactive\n");
    return INDEX_SHARD_SOLVE_UNAVAILABLE;
  }

  kdtree_phase_a_reset();

  tasks = index_shard_build_task_plan(nindexes);
  results = calloc(nindexes, sizeof(index_shard_result_t));
  completed = calloc(nindexes, sizeof(unsigned char));

  if (!tasks || !results || !completed) {
    SYSERROR("Failed to allocate index-shard pass state");

    free(tasks);
    free(results);
    free(completed);

    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }
  // submit wakes workers, reducer runs on caller thread
  rc = index_shard_pool_submit(pool, bp, base_sp, nindexes, hooks, tasks, results, completed);

  if (!rc) {
    rc = index_shard_pool_reduce_online(pool);
  }

  index_shard_pass_state_snapshot(&pool->shared, &state);

  if (rc) {
    if (state.master_committed) {
      status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
    } else {
      status = INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
    }
  }

  // dispose unmerged worker results after all workers have left pass
  for (i = 0; i < nindexes; i++)
    index_shard_result_dispose(&results[i], hooks);

  free(tasks);
  free(results);
  free(completed);

  // pool metrics measure only shard execution, not FITS/plot output
  double pool_wall = timenow() - pool->shared.pass_wall_start;
  float pool_cpu = get_cpu_usage() - pool->shared.pass_cpu_start;
  double pool_cpu_pct = 0.0;

  if (pool_wall > 0.0)
    pool_cpu_pct = (100.0 * (double)pool_cpu) / pool_wall;

  logmsg("[index-shard] pthread-pool done candidates=%zu reduced=%zu solved=%i "
         "total_cpu=%i cancelled=%i rc=%i status=%i master_committed=%i "
         "pool_wall=%.3f pool_cpu=%.3f pool_cpu_pct=%.1f%%\n",
         nindexes,
         pool->shared.next_reduce,
         bp->single_field_solved,
         bp->hit_total_cpulimit,
         bp->cancelled,
         rc,
         (int)status,
         state.master_committed,
         pool_wall,
         (double)pool_cpu,
         pool_cpu_pct);

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

    logmsg("[kd-phase-a] calls=%llu product=%llu fallback=%llu\n",
           (unsigned long long)kd_stats.calls,
           (unsigned long long)kd_stats.product_calls,
           (unsigned long long)kd_stats.fallback_calls);

    logmsg("[kd-phase-a] nodes=%llu leaves=%llu points=%llu matches=%llu "
           "avg_nodes=%.2f avg_leaves=%.2f avg_points=%.2f\n",
           (unsigned long long)kd_stats.nodes_visited,
           (unsigned long long)kd_stats.leaves_visited,
           (unsigned long long)kd_stats.points_tested,
           (unsigned long long)kd_stats.matches_found,
           avg_nodes,
           avg_leaves,
           avg_points);

    logmsg("[kd-phase-a] frontier_total=%llu submitted=%llu inline=%llu\n",
           (unsigned long long)kd_stats.frontier_total,
           (unsigned long long)kd_stats.tasks_submitted,
           (unsigned long long)kd_stats.tasks_inline);

    logmsg("[kd-phase-a] wall_total_ms=%.3f cpu_total_ms=%.3f "
           "wall_min_us=%.3f wall_max_us=%.3f\n",
           wall_total_ms,
           cpu_total_ms,
           wall_min_us,
           wall_max_us);

    logmsg("[kd-phase-a] wall_us histogram "
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

  return status;
}


//Aux Helpers
static int index_shard_aux_queue_init(index_shard_aux_queue_t *q,
                                      size_t max_pending) {
  memset(q, 0, sizeof(*q));

  if (pthread_mutex_init(&q->mutex, NULL))
    return -1;

  if (pthread_cond_init(&q->cv, NULL)) {
    pthread_mutex_destroy(&q->mutex);
    return -1;
  }

  q->max_pending = max_pending ? max_pending : 64;
  return 0;
}

static void index_shard_aux_queue_destroy(index_shard_aux_queue_t *q) {
  index_shard_aux_task_t *task;
  index_shard_aux_task_t *next;

  if (!q)
    return;

  pthread_mutex_lock(&q->mutex);
  task = q->head;
  q->head = NULL;
  q->tail = NULL;
  pthread_mutex_unlock(&q->mutex);

  while (task) {
    next = task->next;
    free(task);
    task = next;
  }

  pthread_cond_destroy(&q->cv);
  pthread_mutex_destroy(&q->mutex);
}

static int index_shard_aux_queue_push(index_shard_aux_queue_t *q,
                                      index_shard_aux_task_t *task) {
  if (!q || !task)
    return -1;

  task->next = NULL;

  pthread_mutex_lock(&q->mutex);

  if (q->stopping || q->pending >= q->max_pending) {
    q->rejected_total++;
    pthread_mutex_unlock(&q->mutex);
    return -1;
  }

  if (q->tail)
    q->tail->next = task;
  else
    q->head = task;

  q->tail = task;
  q->pending++;
  q->submitted_total++;

  pthread_cond_signal(&q->cv);
  pthread_mutex_unlock(&q->mutex);

  return 0;
}

static index_shard_aux_task_t *index_shard_aux_queue_try_pop(index_shard_aux_queue_t *q) {
  index_shard_aux_task_t *task;

  if (!q)
    return NULL;

  pthread_mutex_lock(&q->mutex);

  task = q->head;
  if (task) {
    q->head = task->next;
    if (!q->head)
      q->tail = NULL;
    task->next = NULL;

    if (q->pending > 0)
      q->pending--;
  }

  pthread_mutex_unlock(&q->mutex);
  return task;
}

//ANCHOR - Aux Group Life Cycle
index_shard_aux_group_t *index_shard_aux_group_new(void) {
    index_shard_aux_group_t *group;

    group = calloc(1, sizeof(index_shard_aux_group_t));
    if (!group)
        return NULL;

    if (pthread_mutex_init(&group->mutex, NULL)) {
        free(group);
        return NULL;
    }

    if (pthread_cond_init(&group->cv, NULL)) {
        pthread_mutex_destroy(&group->mutex);
        free(group);
        return NULL;
    }

    group->pending = 0;
    group->failed = 0;
    group->closed = 0;
    group->pool = index_shard_current_worker_pool;

    return group;
}

void index_shard_aux_group_free(index_shard_aux_group_t *group) {
    if (!group)
        return;

    pthread_mutex_lock(&group->mutex);
    group->closed = 1;

    while (group->pending > 0)
        pthread_cond_wait(&group->cv, &group->mutex);

    pthread_mutex_unlock(&group->mutex);

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

  if (!group || !fn)
    return -1;

  pool = group->pool;
  if (!pool)
    return -1;

  task = calloc(1, sizeof(index_shard_aux_task_t));
  if (!task)
    return -1;

  task->fn = fn;
  task->userdata = task_userdata;
  task->group = group;
  task->next = NULL;

  pthread_mutex_lock(&group->mutex);

  if (group->closed) {
    pthread_mutex_unlock(&group->mutex);
    free(task);
    return -1;
  }

  group->pending++;

  pthread_mutex_unlock(&group->mutex);

  if (index_shard_aux_queue_push(&pool->auxq, task)) {
    pthread_mutex_lock(&group->mutex);

    if (group->pending > 0)
      group->pending--;

    group->failed = 1;
    pthread_cond_broadcast(&group->cv);

    pthread_mutex_unlock(&group->mutex);

    free(task);
    return -1;
  }

  return 0;
}

static int index_shard_kdtree_wait(void *userdata) {
  index_shard_aux_group_t *group = userdata;
  index_shard_pool_t *pool;
  int failed = 0;

  if (!group)
    return -1;

  pool = group->pool;
  if (!pool)
    return -1;

  while (1) {
    index_shard_aux_task_t *task = NULL;
    int pending;

    pthread_mutex_lock(&group->mutex);
    pending = group->pending;

    if (pending == 0) {
      failed = group->failed;
      pthread_mutex_unlock(&group->mutex);
      break;
    }

    pthread_mutex_unlock(&group->mutex);

    /*
     * Cooperative wait:
     *
     * Do not block permanently on group->cv.  This worker is part of the
     * unified pool, so while its own group is pending it must keep draining
     * global aux work.
     */
    task = index_shard_aux_queue_try_pop(&pool->auxq);
    if (task) {
      index_shard_aux_execute_one(task);
      continue;
    }

    /*
     * No work was visible right now.  Yield instead of sleeping forever on
     * this group's private condition.  This is intentionally conservative:
     * correctness first, performance later.
     */
    sched_yield();
  }

  return failed ? -1 : 0;
}

int index_shard_kdtree_executor_init(kdtree_task_executor_t *executor,
                                     index_shard_aux_group_t *group) {
  index_shard_pool_t *pool;

  if (!executor || !group)
    return -1;

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  pool = index_shard_global_pool;
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  if (!pool)
    return -1;

  executor->userdata = group;
  executor->submit = index_shard_kdtree_submit;
  executor->wait = index_shard_kdtree_wait;
  executor->capacity = index_shard_kdtree_capacity;

  return 0;
}


//ANCHOR - Task execution from aux

static void index_shard_aux_group_done(index_shard_aux_group_t *group,
                                       int failed) {
    if (!group)
        return;

    pthread_mutex_lock(&group->mutex);

    if (failed)
        group->failed = 1;

    if (group->pending > 0)
        group->pending--;

    pthread_cond_broadcast(&group->cv);
    pthread_mutex_unlock(&group->mutex);
}

static void index_shard_aux_execute_one(index_shard_aux_task_t *task) {
  int failed = 0;

  if (!task)
    return;

  if (task->fn)
    task->fn(task->userdata);
  else
    failed = 1;

  index_shard_aux_group_done(task->group, failed);
  free(task);
}
//ANCHOR - Public API

int index_shard_aux_available(void) {
  int available = FALSE;

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  available = (index_shard_global_pool != NULL);
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  return available;
}

static int index_shard_aux_submit(index_shard_aux_group_t *group,
                                  kdtree_task_fn fn,
                                  void *userdata) {
  index_shard_pool_t *pool;
  index_shard_aux_task_t *task;

  if (!group || !fn)
    return -1;

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  pool = index_shard_global_pool;
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  if (!pool)
    return -1;

  task = calloc(1, sizeof(index_shard_aux_task_t));
  if (!task)
    return -1;

  task->fn = fn;
  task->userdata = userdata;
  task->group = group;

  pthread_mutex_lock(&group->mutex);
  if (group->closed) {
    pthread_mutex_unlock(&group->mutex);
    free(task);
    return -1;
  }
  group->pending++;
  pthread_mutex_unlock(&group->mutex);

  pthread_mutex_lock(&pool->aux_mutex);

  if (pool->aux_stopping) {
    pthread_mutex_unlock(&pool->aux_mutex);

    pthread_mutex_lock(&group->mutex);
    group->pending--;
    group->failed = 1;
    pthread_cond_broadcast(&group->cv);
    pthread_mutex_unlock(&group->mutex);

    free(task);
    return -1;
  }

  if (pool->aux_tail)
    pool->aux_tail->next = task;
  else
    pool->aux_head = task;

  pool->aux_tail = task;

  pthread_cond_broadcast(&pool->aux_cv);
  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->aux_mutex);

  return 0;
}

static index_shard_aux_task_t *index_shard_aux_try_pop(index_shard_pool_t *pool) {
  index_shard_aux_task_t *task;

  if (!pool)
    return NULL;

  pthread_mutex_lock(&pool->aux_mutex);

  task = pool->aux_head;
  if (task) {
    pool->aux_head = task->next;
    if (!pool->aux_head)
      pool->aux_tail = NULL;
    task->next = NULL;
  }

  pthread_mutex_unlock(&pool->aux_mutex);
  return task;
}

static void index_shard_aux_task_done(index_shard_aux_group_t *group, int failed) {
  if (!group)
    return;

  pthread_mutex_lock(&group->mutex);

  if (failed)
    group->failed = 1;

  if (group->pending > 0)
    group->pending--;

  pthread_cond_broadcast(&group->cv);
  pthread_mutex_unlock(&group->mutex);
}

static void index_shard_aux_execute(index_shard_aux_task_t *task) {
    int failed = 0;

    if (!task)
        return;

    if (task->fn)
        task->fn(task->userdata);
    else
        failed = 1;

    index_shard_aux_group_done(task->group, failed);
    free(task);
}

static int index_shard_aux_wait(index_shard_aux_group_t *group) {
  index_shard_pool_t *pool;
  int failed;

  if (!group)
    return -1;

  for (;;) {
    index_shard_aux_task_t *task = NULL;

    pthread_mutex_lock(&group->mutex);
    if (group->pending == 0) {
      failed = group->failed;
      group->closed = 1;
      pthread_mutex_unlock(&group->mutex);
      return failed ? -1 : 0;
    }
    pthread_mutex_unlock(&group->mutex);

    pthread_mutex_lock(&index_shard_global_pool_mutex);
    pool = index_shard_global_pool;
    pthread_mutex_unlock(&index_shard_global_pool_mutex);

    if (pool)
      task = index_shard_aux_try_pop(pool);

    if (task) {
      index_shard_aux_execute(task);
      continue;
    }

    pthread_mutex_lock(&group->mutex);
    if (group->pending > 0)
      pthread_cond_wait(&group->cv, &group->mutex);
    pthread_mutex_unlock(&group->mutex);
  }
}


static int index_shard_kdtree_capacity(void *userdata,
                                       kdtree_task_capacity_t *capacity) {
  index_shard_aux_group_t *group = userdata;
  index_shard_pool_t *pool;
  index_shard_thread_state_t *shared;

  size_t pending;
  size_t max_pending;
  size_t room;

  size_t workers_total;
  size_t workers_effective;
  size_t workers_active;
  size_t outer_running;
  size_t spare_workers;

  index_shard_pass_state_snapshot_t state;
  int outer_work_claimable;
  int have_solved_order;

  if (!group || !capacity) {
    return -1;
  }

  memset(capacity, 0, sizeof(*capacity));

  pool = group->pool;

  if (!pool) {
    return -1;
  }

  shared = &pool->shared;

  /*
   * Snapshot the outer scheduler state.
   *
   * queue_mutex protects:
   *   - next_task / ntasks
   *   - running_tasks
   *   - active_limit
   *   - have_solved_order / earliest_solved_order
   *
   * state_mutex protects:
   *   - stop_requested
   *   - fatal_error
   *   - solved_published
   *
   * result_mutex protects:
   *   - active_workers
   *
   * Preserve the established queue -> result lock order.
   */
  pthread_mutex_lock(&shared->queue_mutex);
  pthread_mutex_lock(&shared->result_mutex);

  workers_total = 0;
  workers_effective = 0;
  workers_active = 0;
  outer_running = 0;
  spare_workers = 0;

  if (shared->worker_count > 0) {
    workers_total = (size_t)shared->worker_count;
  }

  workers_effective = workers_total;

  if (shared->active_limit > 0 &&
      (size_t)shared->active_limit < workers_effective) {
    workers_effective = (size_t)shared->active_limit;
  }

  if (shared->active_workers > 0) {
    workers_active = (size_t)shared->active_workers;
  }

  if (workers_active < workers_effective) {
    workers_effective = workers_active;
  }

  if (shared->running_tasks > 0) {
    outer_running = (size_t)shared->running_tasks;
  }

  index_shard_pass_state_snapshot(shared, &state);
  have_solved_order = shared->have_solved_order;

  outer_work_claimable = FALSE;

  if (!state.stop_requested &&
      !state.fatal_error &&
      !state.solved_published &&
      !have_solved_order &&
      shared->next_task < shared->ntasks) {
    outer_work_claimable = TRUE;
  }

  /*
   * Conservative spare-capacity policy:
   *
   * If any unclaimed outer index work remains, it has priority over inner
   * CodeKD subtasks.  Inner work is allowed only after the outer queue has no
   * further claimable work and some participating workers are not occupied by
   * running outer tasks.
   */
  if (!state.stop_requested &&
      !state.fatal_error &&
      !state.solved_published &&
      !have_solved_order &&
      !outer_work_claimable &&
      workers_effective > outer_running) {
    spare_workers = workers_effective - outer_running;
  }

  pthread_mutex_unlock(&shared->result_mutex);
  pthread_mutex_unlock(&shared->queue_mutex);

  /*
   * Snapshot bounded aux-queue capacity separately.  Do not hold scheduler
   * locks while acquiring the aux-queue mutex.
   */
  pthread_mutex_lock(&pool->auxq.mutex);

  pending = pool->auxq.pending;
  max_pending = pool->auxq.max_pending;

  pthread_mutex_unlock(&pool->auxq.mutex);

  if (max_pending > pending) {
    room = max_pending - pending;
  } else {
    room = 0;
  }

  capacity->workers_total = workers_total;
  capacity->aux_pending = pending;
  capacity->aux_room = room;

  /*
   * One product subtree executes inline in the parent.  suggested_subtasks
   * therefore counts only subtasks that may be offered asynchronously.
   */
  capacity->suggested_subtasks = spare_workers;

  if (capacity->suggested_subtasks > room) {
    capacity->suggested_subtasks = room;
  }

  return 0;
}
