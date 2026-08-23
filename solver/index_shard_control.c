/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>

#include "index_shard_private.h"
#include "astrometry/log.h"
#include "astrometry/tic.h"

static pthread_key_t index_shard_tls_key;
static pthread_once_t index_shard_tls_once = PTHREAD_ONCE_INIT;
static int index_shard_tls_key_status = EAGAIN;

/*
 * Solver callbacks receive only local onefield state. TLS links them to the
 * worker context for cooperative global-stop polling.
 */
static void index_shard_make_tls_key(void) {
  index_shard_tls_key_status =
      pthread_key_create(&index_shard_tls_key, NULL);
}

int index_shard_tls_ensure(void) {
  int once_status =
      pthread_once(&index_shard_tls_once,
                   index_shard_make_tls_key);

  if (once_status) {
    return once_status;
  }
  return index_shard_tls_key_status;
}

int index_shard_set_tls(index_shard_worker_context_t *ctx) {
  int status = index_shard_tls_ensure();

  if (status) {
    return status;
  }
  return pthread_setspecific(index_shard_tls_key, ctx);
}

index_shard_worker_context_t *index_shard_get_tls(void) {
  if (index_shard_tls_ensure()) {
    return NULL;
  }
  return pthread_getspecific(index_shard_tls_key);
}

anbool index_shard_worker_context_active(void) {
  return index_shard_get_tls() != NULL;
}

anbool index_shard_worker_stop_requested(void) {
  index_shard_worker_context_t *ctx = index_shard_get_tls();

  if (!ctx || !ctx->pool) {
    return FALSE;
  }

  return __atomic_load_n(
      &ctx->pool->shared.worker_stop_requested,
      __ATOMIC_ACQUIRE) != 0;
}

/* The engine resolves one immutable worker count before engine_run_job(). */
anbool index_shard_pthread_enabled(const onefield_t *bp) {
  return bp && bp->index_shard_workers > 1;
}

anbool index_shard_trace_enabled(void) {
  /*
   * Detailed scheduler traces follow the ordinary command-line verbosity
   * model. Two -v flags select LOG_ALL without another environment control.
   */
  return log_get_level() >= LOG_ALL;
}

int index_shard_get_worker_count(const onefield_t *bp) {
  if (!bp) {
    return 1;
  }

  return index_shard_config_effective_workers(
      bp->index_shard_workers);
}

/*
 * Take one synchronized snapshot of pass termination state.
 *
 * A caller may already hold queue_mutex or result_mutex. No path may acquire
 * either lock while holding state_mutex.
 */
void index_shard_pass_state_snapshot(
    index_shard_thread_state_t *shared,
    index_shard_pass_state_snapshot_t *snapshot) {
  assert(shared);
  assert(snapshot);

  pthread_mutex_lock(&shared->state_mutex);
  snapshot->winner_selected = shared->winner_selected;
  snapshot->solved_published = shared->solved_published;
  snapshot->master_committed = shared->master_committed;
  snapshot->terminal_cause = shared->terminal_cause;
  snapshot->selected_index_order = shared->selected_index_order;
  snapshot->selected_candidate_sequence =
      shared->selected_candidate_sequence;
  snapshot->task_local_failures = shared->task_local_failures;
  snapshot->global_integrity_failures =
      shared->global_integrity_failures;
  pthread_mutex_unlock(&shared->state_mutex);
}

/* state_mutex must be held. */
void index_shard_publish_terminal_locked(
    index_shard_thread_state_t *shared,
    index_shard_terminal_cause_t cause) {
  assert(shared);
  if (cause == INDEX_SHARD_TERMINAL_NONE) {
    return;
  }

  if (cause == INDEX_SHARD_TERMINAL_GLOBAL_INTEGRITY) {
    shared->terminal_cause = cause;
    return;
  }

  if (shared->terminal_cause != INDEX_SHARD_TERMINAL_NONE) {
    return;
  }

  shared->terminal_cause = cause;
}

void index_shard_publish_worker_stop(
    index_shard_thread_state_t *shared) {
  __atomic_store_n(
      &shared->worker_stop_requested,
      TRUE,
      __ATOMIC_RELEASE);
  index_shard_wake_pass_waiters(shared);
  index_shard_wake_queue_waiters(shared);
}

/*
 * Publish a hard worker/module failure and stop the current pass.
 */
void index_shard_request_fatal_stop(index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->state_mutex);
  index_shard_publish_terminal_locked(
      shared, INDEX_SHARD_TERMINAL_GLOBAL_INTEGRITY);
  pthread_mutex_unlock(&shared->state_mutex);

  index_shard_publish_worker_stop(shared);
}

/*
 * Publish that the reducer committed a valid solved result.
 *
 * First-valid selection already stopped new work. This publication records
 * that the reducer transferred the selected result into master state.
 */
void index_shard_publish_committed_solve(
    index_shard_thread_state_t *shared) {
  int valid;

  pthread_mutex_lock(&shared->state_mutex);
  valid = shared->winner_selected &&
      shared->terminal_cause == INDEX_SHARD_TERMINAL_WINNER;
  if (valid) {
    shared->solved_published = TRUE;
  } else {
    index_shard_publish_terminal_locked(
        shared, INDEX_SHARD_TERMINAL_GLOBAL_INTEGRITY);
  }
  pthread_mutex_unlock(&shared->state_mutex);

  index_shard_publish_worker_stop(shared);
  if (!valid) {
    logerr("[index-shard] invalid committed-solution terminal state\n");
  }
}
/*
 * Mark the point after which serial fallback is no longer safe.
 *
 * The merge hook is not transactional.  Once it begins transferring a
 * non-empty worker result into master-visible state, a later failure must not
 * cause the caller to rerun the original serial pass.
 */
void index_shard_mark_master_committed(
    index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->state_mutex);
  shared->master_committed = TRUE;
  pthread_mutex_unlock(&shared->state_mutex);
}

int index_shard_master_limit_or_cancel_requested(
    index_shard_thread_state_t *shared,
    anbool *hit_total_cpulimit,
    anbool *hit_total_timelimit,
    anbool *cancelled) {
  onefield_t *bp = shared->bp;
  int stop;

  pthread_mutex_lock(&shared->limit_mutex);

  if (hit_total_cpulimit) {
    *hit_total_cpulimit = bp->hit_total_cpulimit;
  }

  if (hit_total_timelimit) {
    *hit_total_timelimit = bp->hit_total_timelimit;
  }

  if (cancelled) {
    *cancelled = bp->cancelled;
  }

  stop = bp->hit_total_cpulimit || bp->hit_total_timelimit || bp->cancelled;

  pthread_mutex_unlock(&shared->limit_mutex);

  return stop;
}

/*
 * Read-only stop predicate used by workers before expensive work.
 *
 * First-valid selection is recorded as a terminal cause. Workers therefore
 * do not read master bp->single_field_solved concurrently with the reducer.
 */
int index_shard_master_stop_requested(index_shard_thread_state_t *shared) {
  index_shard_pass_state_snapshot_t state;

  index_shard_pass_state_snapshot(shared, &state);

  if (state.terminal_cause != INDEX_SHARD_TERMINAL_NONE) {
    return TRUE;
  }

  return index_shard_master_limit_or_cancel_requested(
      shared, NULL, NULL, NULL);
}

/* state_mutex and limit_mutex must be held. */
index_shard_terminal_cause_t
index_shard_sample_terminal_locked(
    index_shard_thread_state_t *shared,
    const index_shard_result_t *result,
    double *elapsed,
    int *report) {
  onefield_t *bp;
  index_shard_terminal_cause_t cause =
      INDEX_SHARD_TERMINAL_NONE;

  assert(shared);
  bp = shared->bp;
  assert(bp);

  if (elapsed) {
    *elapsed = 0.0;
  }
  if (report) {
    *report = FALSE;
  }

  if (result && result->cancelled) {
    bp->cancelled = TRUE;
  }
  if (result && result->hit_total_timelimit) {
    bp->hit_total_timelimit = TRUE;
  }
  if (result && result->hit_total_cpulimit) {
    bp->hit_total_cpulimit = TRUE;
  }

  if (bp->cancelled) {
    cause = INDEX_SHARD_TERMINAL_CANCELLED;
  } else if (bp->hit_total_timelimit) {
    cause = INDEX_SHARD_TERMINAL_WALL_LIMIT;
  } else if (bp->hit_total_cpulimit) {
    cause = INDEX_SHARD_TERMINAL_CPU_LIMIT;
  } else if (bp->total_timelimit > 0.0) {
    double now = monotonic_seconds();
    double sampled = now - bp->time_total_start;

    if (elapsed) {
      *elapsed = sampled;
    }
    if (now >= 0.0 && sampled >= bp->total_timelimit) {
      bp->hit_total_timelimit = TRUE;
      cause = INDEX_SHARD_TERMINAL_WALL_LIMIT;
    }
  }

  if (cause == INDEX_SHARD_TERMINAL_NONE &&
      bp->total_cpulimit > 0.0) {
    double sampled =
        (double)(get_cpu_usage() - bp->cpu_total_start);

    if (elapsed) {
      *elapsed = sampled;
    }
    if (sampled >= bp->total_cpulimit) {
      bp->hit_total_cpulimit = TRUE;
      cause = INDEX_SHARD_TERMINAL_CPU_LIMIT;
    }
  }

  if ((cause == INDEX_SHARD_TERMINAL_WALL_LIMIT ||
       cause == INDEX_SHARD_TERMINAL_CPU_LIMIT) &&
      !shared->limit_reported) {
    shared->limit_reported = TRUE;
    if (report) {
      *report = TRUE;
    }
  }

  return cause;
}
/*
 * Process-wide elapsed-time and CPU-budget checks.
 *
 * total_timelimit is one shared monotonic wall-clock deadline and is not
 * divided by worker count. total_cpulimit is aggregate process CPU time; with
 * N active threads it can be consumed roughly N times faster than wall time.
 */
int index_shard_check_global_limits(index_shard_thread_state_t *shared) {
  onefield_t *bp = shared->bp;
  index_shard_terminal_cause_t cause = INDEX_SHARD_TERMINAL_NONE;
  double elapsed = 0.0;
  int report = FALSE;

  /*
   * state_mutex is the terminal-event arbiter. Holding it while the master
   * limit flags are inspected and updated closes the former interval between
   * deadline publication and cooperative-stop publication.
   */
  pthread_mutex_lock(&shared->state_mutex);
  if (shared->terminal_cause != INDEX_SHARD_TERMINAL_NONE) {
    pthread_mutex_unlock(&shared->state_mutex);
    return TRUE;
  }

  pthread_mutex_lock(&shared->limit_mutex);
  cause = index_shard_sample_terminal_locked(
      shared, NULL, &elapsed, &report);
  if (cause != INDEX_SHARD_TERMINAL_NONE) {
    index_shard_publish_terminal_locked(shared, cause);
  }
  pthread_mutex_unlock(&shared->limit_mutex);
  pthread_mutex_unlock(&shared->state_mutex);

  if (cause == INDEX_SHARD_TERMINAL_NONE) {
    return FALSE;
  }

  index_shard_publish_worker_stop(shared);
  if (report && cause == INDEX_SHARD_TERMINAL_WALL_LIMIT) {
    logmsg("Total wall-clock time limit reached!\n");
    logverb("[index-shard] wall-limit reached total_timelimit=%g "
            "elapsed=%.6f\n",
            bp->total_timelimit,
            elapsed);
  } else if (report && cause == INDEX_SHARD_TERMINAL_CPU_LIMIT) {
    logmsg("Total CPU time limit reached!\n");
    logverb("[index-shard] cpu-budget reached total_cpulimit=%g "
            "elapsed=%.6f\n",
            bp->total_cpulimit,
            elapsed);
  }
  return TRUE;
}

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

  if (!ctx || !ctx->pool) {
    return;
  }

  if (index_shard_check_global_limits(&ctx->pool->shared)) {
    bp->solver.quit_now = TRUE;
    return;
  }

  if (index_shard_master_stop_requested(&ctx->pool->shared)) {
    bp->solver.quit_now = TRUE;
  }
}
