/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "index_shard_private.h"
#include "astrometry/errors.h"
#include "astrometry/log.h"
#include "astrometry/fitsioutils.h"

static index_shard_pool_t *index_shard_global_pool = NULL;
static pthread_mutex_t index_shard_global_pool_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static int index_shard_shared_init(index_shard_thread_state_t *shared) {
  pthread_condattr_t condattr;

  memset(shared, 0, sizeof(index_shard_thread_state_t));

  if (pthread_mutex_init(&shared->queue_mutex, NULL)) {
    return -1;
  }
  if (pthread_cond_init(&shared->queue_cv, NULL)) {
    goto destroy_queue_mutex;
  }

  if (pthread_mutex_init(&shared->result_mutex, NULL)) {
    goto destroy_queue_cv;
  }

  if (pthread_condattr_init(&condattr)) {
    goto destroy_result_mutex;
  }
  if (pthread_condattr_setclock(
          &condattr,
          CLOCK_MONOTONIC)) {
    goto destroy_condattr;
  }
  if (pthread_cond_init(
          &shared->result_cv,
          &condattr)) {
    goto destroy_condattr;
  }
  pthread_condattr_destroy(&condattr);

  if (pthread_mutex_init(&shared->state_mutex, NULL)) {
    goto destroy_result_cv;
  }

  if (pthread_mutex_init(&shared->limit_mutex, NULL)) {
    goto destroy_state_mutex;
  }

  return 0;

destroy_state_mutex:
  pthread_mutex_destroy(&shared->state_mutex);
destroy_result_cv:
  pthread_cond_destroy(&shared->result_cv);
  goto destroy_result_mutex;
destroy_condattr:
  pthread_condattr_destroy(&condattr);
destroy_result_mutex:
  pthread_mutex_destroy(&shared->result_mutex);
destroy_queue_cv:
  pthread_cond_destroy(&shared->queue_cv);
destroy_queue_mutex:
  pthread_mutex_destroy(&shared->queue_mutex);
  return -1;
}

static void index_shard_shared_destroy(index_shard_thread_state_t *shared) {
  index_shard_completion_registry_destroy(shared);
  pthread_cond_destroy(&shared->queue_cv);
  pthread_mutex_destroy(&shared->queue_mutex);

  pthread_mutex_destroy(&shared->result_mutex);
  pthread_cond_destroy(&shared->result_cv);

  pthread_mutex_destroy(&shared->state_mutex);

  pthread_mutex_destroy(&shared->limit_mutex);
}

/*
 * Clear every pointer borrowed from a submitted pass.  The caller holds
 * control_mutex and has already established worker/callback quiescence.
 */
static void index_shard_clear_pass_state_locked(
    index_shard_pool_t *pool) {
  index_shard_thread_state_t *shared = &pool->shared;

  pthread_mutex_lock(&shared->queue_mutex);
  pthread_mutex_lock(&shared->result_mutex);
  pthread_mutex_lock(&shared->state_mutex);
  pthread_mutex_lock(&shared->limit_mutex);

  shared->bp = NULL;
  shared->hooks = NULL;
  shared->worker_view = NULL;
  shared->results = NULL;
  shared->nindexes = 0U;
  shared->canonical_scan_cursor = 0U;
  shared->outer_running = 0U;
  shared->producer_width = 0U;
  shared->active_workers = 0;
  shared->worker_count = 0;

  pthread_mutex_unlock(&shared->limit_mutex);
  pthread_mutex_unlock(&shared->state_mutex);
  pthread_mutex_unlock(&shared->result_mutex);
  pthread_mutex_unlock(&shared->queue_mutex);
}

static int index_shard_pass_quiescent_locked(
    index_shard_pool_t *pool) {
  index_shard_thread_state_t *shared = &pool->shared;
  int i;
  int quiescent = TRUE;

  pthread_mutex_lock(&shared->queue_mutex);
  pthread_mutex_lock(&shared->result_mutex);

  if (shared->active_workers || shared->outer_running ||
      shared->queue_waiters || shared->staged_groups_active ||
      shared->staged_tickets_active ||
      shared->staged_source_leases ||
      shared->staged_submit_callbacks_active ||
      shared->completion_active) {
    quiescent = FALSE;
  }
  if (!shared->active_workers) {
    for (i = 0; i < pool->worker_count; i++) {
      if (pool->contexts[i].published_staged_group ||
          pool->contexts[i].owner_waiting ||
          pool->contexts[i].owner_wake_pending ||
          pool->contexts[i].current_outer_active) {
        quiescent = FALSE;
        break;
      }
    }
  }

  pthread_mutex_unlock(&shared->result_mutex);
  pthread_mutex_unlock(&shared->queue_mutex);
  return quiescent;
}

#define INDEX_SHARD_RELEASE_RECHECK_NANOSECONDS 100000000L

static void index_shard_release_recheck_deadline(
    struct timespec *deadline) {
  clock_gettime(CLOCK_MONOTONIC, deadline);
  deadline->tv_nsec += INDEX_SHARD_RELEASE_RECHECK_NANOSECONDS;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
}

/*
 * A poisoned early release remains synchronous. Workers retain every borrowed
 * pass pointer while they cooperatively unwind. Once the shard counters are
 * zero, the provider fence closes the smaller interval in which a completion
 * callback has acquired pool as its opaque pointer but has not entered it yet.
 * No pool mutex may be held across that provider wait because the callback
 * acquires shared->queue_mutex.
 */
static int index_shard_wait_pass_quiescent(
    index_shard_pool_t *pool) {
  index_shard_thread_state_t *shared = &pool->shared;
  int notifier_status = 0;
  int reported = FALSE;

  while (1) {
    pthread_mutex_lock(&shared->result_mutex);
    while (shared->active_workers > 0) {
      pthread_cond_wait(&shared->result_cv, &shared->result_mutex);
    }
    pthread_mutex_unlock(&shared->result_mutex);

    if (!notifier_status && pool->payload_completion_registered &&
        fitsbin_payload_io_wait_completion_notifier_idle(
            index_shard_staged_completion_notify, pool)) {
      logerr("[index-shard] payload completion notifier fence failed\n");
      notifier_status = -1;
      pthread_mutex_lock(&pool->control_mutex);
      pool->poisoned = TRUE;
      pthread_mutex_unlock(&pool->control_mutex);
    }

    pthread_mutex_lock(&pool->control_mutex);
    if (index_shard_pass_quiescent_locked(pool)) {
      pthread_mutex_unlock(&pool->control_mutex);
      return notifier_status;
    }
    pool->poisoned = TRUE;
    pthread_mutex_unlock(&pool->control_mutex);

    if (!reported) {
      logerr("[index-shard] waiting for poisoned pass drain\n");
      reported = TRUE;
    }
    index_shard_request_fatal_stop(shared);

    pthread_mutex_lock(&shared->result_mutex);
    if (!shared->active_workers) {
      struct timespec deadline;

      index_shard_release_recheck_deadline(&deadline);
      (void)pthread_cond_timedwait(
          &shared->result_cv,
          &shared->result_mutex,
          &deadline);
    }
    pthread_mutex_unlock(&shared->result_mutex);
  }
}

/*
 * Bind one job to the process pool.  Width is fixed at pool creation; a later
 * incompatible job fails before any of its state is published.
 */
int index_shard_pool_bind_job(index_shard_pool_t *pool,
                              onefield_t *bp,
                              solver_t *sp) {
  int worker_count;

  if (!pool || !bp || !sp) {
    return -1;
  }
  worker_count = index_shard_get_worker_count(bp);

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  if (index_shard_global_pool != pool) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }
  pthread_mutex_lock(&pool->control_mutex);

  if (pool->shutdown || pool->stopping || pool->poisoned ||
      pool->job_active || pool->pass_active ||
      pool->owner_bp || pool->owner_sp ||
      worker_count != pool->worker_count ||
      !index_shard_pass_quiescent_locked(pool)) {
    pthread_mutex_unlock(&pool->control_mutex);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  pool->owner_bp = bp;
  pool->owner_sp = sp;
  pool->job_active = TRUE;
  pool->job_generation++;

  pthread_mutex_unlock(&pool->control_mutex);
  pthread_mutex_unlock(&index_shard_global_pool_mutex);
  return 0;
}

int index_shard_pool_job_width_compatible(
    const index_shard_pool_t *pool,
    const onefield_t *bp) {
  if (!pool || !bp) {
    return FALSE;
  }
  return pool->worker_count == index_shard_get_worker_count(bp);
}

/*
 * End a sequential job only after its final pass has released all borrowed
 * state.  Retained inverse permutations are flushed here to preserve the old
 * per-job cache boundary while the worker threads and payload service persist.
 */
int index_shard_pool_unbind_job(index_shard_pool_t *pool,
                                onefield_t *bp,
                                solver_t *sp) {
  int rc = 0;

  if (!pool || !bp || !sp) {
    return -1;
  }

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  if (index_shard_global_pool != pool) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }
  pthread_mutex_lock(&pool->control_mutex);

  if (!pool->job_active || pool->owner_bp != bp ||
      pool->owner_sp != sp || pool->pass_active ||
      !index_shard_pass_quiescent_locked(pool)) {
    logerr("[index-shard] job unbind lifecycle conflict\n");
    pool->poisoned = TRUE;
    rc = -1;
  } else if (pool->shared.bp || pool->shared.hooks ||
             pool->shared.worker_view || pool->shared.results) {
    logerr("[index-shard] job unbind found borrowed pass pointers\n");
    pool->poisoned = TRUE;
    rc = -1;
  } else {
    pthread_mutex_lock(&pool->inverse_cache_mutex);
    index_shard_inverse_cache_destroy(pool);
    pthread_mutex_unlock(&pool->inverse_cache_mutex);
    pool->owner_bp = NULL;
    pool->owner_sp = NULL;
    pool->job_active = FALSE;
  }

  pthread_mutex_unlock(&pool->control_mutex);
  pthread_mutex_unlock(&index_shard_global_pool_mutex);
  return rc;
}

void index_shard_pool_poison(index_shard_pool_t *pool) {
  if (!pool) {
    return;
  }
  pthread_mutex_lock(&pool->control_mutex);
  pool->poisoned = TRUE;
  pthread_mutex_unlock(&pool->control_mutex);
}

int index_shard_pool_is_poisoned(index_shard_pool_t *pool) {
  int poisoned;

  if (!pool) {
    return FALSE;
  }
  pthread_mutex_lock(&pool->control_mutex);
  poisoned = pool->poisoned;
  pthread_mutex_unlock(&pool->control_mutex);
  return poisoned;
}
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
index_shard_pool_acquire_status_t
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

  if (!pool->job_active ||
      pool->owner_bp != bp || pool->owner_sp != sp) {
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return INDEX_SHARD_POOL_ACQUIRE_CONFLICT;
  }

  pthread_mutex_lock(&pool->control_mutex);

  if (pool->shutdown || pool->stopping || pool->poisoned ||
      pool->pass_active) {
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
int index_shard_pool_release_pass(index_shard_pool_t *pool) {
  int quiescent = TRUE;
  int release_started = FALSE;
  int rc = 0;

  if (!pool) {
    return -1;
  }

  pthread_mutex_lock(&pool->control_mutex);

  if (!pool->pass_active || pool->pass_releasing) {
    logerr("[index-shard] pass release requested outside active ownership\n");
    pool->poisoned = TRUE;
    rc = -1;
  } else {
    pool->pass_releasing = TRUE;
    release_started = TRUE;
    quiescent = index_shard_pass_quiescent_locked(pool);
    if (!quiescent) {
      logerr("[index-shard] pass release before quiescence\n");
      pool->poisoned = TRUE;
      rc = -1;
    }
  }
  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);

  if (!release_started) {
    return rc;
  }
  if (!quiescent) {
    index_shard_request_fatal_stop(&pool->shared);
  }
  if (index_shard_wait_pass_quiescent(pool)) {
    rc = -1;
  }

  pthread_mutex_lock(&pool->control_mutex);
  index_shard_clear_pass_state_locked(pool);
  pool->pass_active = FALSE;
  pool->pass_releasing = FALSE;

  /*
   * pool_stop() may be waiting for pass_active to become false. Worker waiters
   * also use work_cv, but they re-check their generation predicate in a loop.
   */
  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);
  return rc;
}

static void index_shard_context_owner_cvs_destroy(
    index_shard_pool_t *pool) {
  int i;

  if (!pool || !pool->contexts) {
    return;
  }
  for (i = 0; i < pool->worker_count; i++) {
    if (!pool->contexts[i].owner_cv_ready) {
      continue;
    }
    pthread_cond_destroy(&pool->contexts[i].owner_cv);
    pool->contexts[i].owner_cv_ready = FALSE;
    pool->contexts[i].owner_waiting = FALSE;
    pool->contexts[i].owner_wake_pending = FALSE;
  }
}

/*
 * Create the process-lifetime worker pool.  No job pointers are retained by
 * this operation; bind_job() publishes them only after creation succeeds.
 */
int index_shard_pool_create(const onefield_t *config_bp,
                            index_shard_pool_t **pool_out) {
  index_shard_pool_t *pool = NULL;
  int i;
  int tls_status;
  int threads_started = 0;
  int worker_count;
  int payload_io_lanes;
  int payload_io_width;
  size_t producer_width;

  if (!pool_out) {
    return -1;
  }
  *pool_out = NULL;

  if (!config_bp || !index_shard_pthread_enabled(config_bp)) {
    ERROR("Cannot create index-shard pool without parallel configuration");
    return -1;
  }

  tls_status = index_shard_tls_ensure();
  if (tls_status) {
    logerr("[index-shard] cannot initialize worker TLS status=%i\n",
           tls_status);
    return -1;
  }

  /* Initialize process-wide lazy read-only state before workers start. */
  (void)fits_get_endian_string();

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  if (index_shard_global_pool) {
    logerr("[index-shard] process pool already exists\n");
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  worker_count = index_shard_get_worker_count(config_bp);

  pool = calloc(1, sizeof(index_shard_pool_t));
  if (!pool) {
    SYSERROR("Failed to allocate index-shard pool");
    goto unlock_failure;
  }

  pool->worker_count = worker_count;
  pool->inverse_cache_budget =
      index_shard_inverse_cache_budget();

  if (pthread_mutex_init(&pool->inverse_cache_mutex, NULL)) {
    goto free_pool;
  }

  // initialize shared state before workers can observe pool
  if (pthread_mutex_init(&pool->control_mutex, NULL)) {
    goto destroy_inverse_mutex;
  }

  if (pthread_cond_init(&pool->work_cv, NULL)) {
    goto destroy_control_mutex;
  }

  if (index_shard_shared_init(&pool->shared)) {
    goto destroy_work_cv;
  }
  pool->shared.pool = pool;
  if (index_shard_completion_registry_init(
          &pool->shared, worker_count)) {
    goto destroy_shared;
  }

  pool->threads = calloc((size_t)worker_count, sizeof(pthread_t));
  pool->contexts = calloc((size_t)worker_count,
                          sizeof(index_shard_worker_context_t));

  if (!pool->threads || !pool->contexts) {
    goto free_worker_storage;
  }

  // worker contexts and owner conditions are stable for pool lifetime.
  for (i = 0; i < worker_count; i++) {
    pool->contexts[i].worker_id = i;
    pool->contexts[i].generation_seen = 0;
    pool->contexts[i].pool = pool;
    if (pthread_cond_init(&pool->contexts[i].owner_cv, NULL)) {
      goto destroy_owner_cvs;
    }
    pool->contexts[i].owner_cv_ready = TRUE;
  }

  for (i = 0; i < worker_count; i++) {
    if (pthread_create(
            &pool->threads[i], NULL,
            index_shard_worker_main, &pool->contexts[i])) {
      goto stop_threads;
    }
    threads_started++;
  }

  pthread_mutex_lock(&pool->control_mutex);
  while (pool->ready_workers < worker_count) {
    pthread_cond_wait(&pool->work_cv, &pool->control_mutex);
  }
  tls_status = pool->tls_startup_error;
  if (tls_status) {
    pool->shutdown = TRUE;
    pthread_cond_broadcast(&pool->work_cv);
  }
  pthread_mutex_unlock(&pool->control_mutex);

  if (tls_status) {
    logerr("[index-shard] worker TLS startup failed status=%i\n",
           tls_status);
    goto join_threads;
  }

  /*
   * Completion lanes populate only packet-planned mapped pages. They are not
   * compute workers and do not own solver, index, or result state.
   */
  payload_io_lanes = worker_count;
  if (!fitsbin_payload_io_service_width()) {
    if (fitsbin_payload_io_service_start(payload_io_lanes)) {
      logverb("[index-shard] mapped-page completion unavailable; "
              "using native mmap demand\n");
    } else {
      pool->payload_io_owned = TRUE;
    }
  }
  if (fitsbin_payload_io_service_width() &&
      fitsbin_payload_io_mapped_population_supported() &&
      !fitsbin_payload_io_set_completion_notifier(
          index_shard_staged_completion_notify, pool)) {
    pool->payload_completion_registered = TRUE;
  } else if (fitsbin_payload_io_service_width() &&
             fitsbin_payload_io_mapped_population_supported()) {
    logverb("[index-shard] detached payload completion unavailable; "
            "retaining fixed helper reservation\n");
  } else if (fitsbin_payload_io_service_width()) {
    logverb("[index-shard] mapped-page population unsupported; "
            "retaining fixed helper reservation\n");
  }

  index_shard_global_pool = pool;
  *pool_out = pool;
  payload_io_width = fitsbin_payload_io_service_width();
  producer_width = index_shard_config_producer_width(
      worker_count,
      payload_io_width,
      pool->payload_completion_registered,
      FALSE);
  if (!producer_width) {
    logerr("[index-shard] invalid compute/delivery width plan "
           "workers=%i payload_io_width=%i detached=%i\n",
           worker_count,
           payload_io_width,
           pool->payload_completion_registered ? 1 : 0);
    producer_width =
        worker_count > 1 ? (size_t)worker_count - 1U : 1U;
  }
  /*
   * Detached completion does not reserve a compute lane. Every worker may
   * own outer work. A completed owner may execute one READY foreign package
   * before claiming again; otherwise outer admission remains the priority.
   */
  fitsbin_payload_io_configure_workers(
      pool->payload_completion_registered
          ? (int)producer_width
          : worker_count);

  logverb("[index-shard] workers=%i mode=pthread "
          "compute_width=%i producer_width=%zu helper_width=%zu "
          "payload_io_width=%i inverse_cache_budget=%zu\n",
          worker_count,
          worker_count,
          producer_width,
          (size_t)worker_count - producer_width,
          payload_io_width,
          pool->inverse_cache_budget);

  pthread_mutex_unlock(&index_shard_global_pool_mutex);
  return 0;

stop_threads:
  pthread_mutex_lock(&pool->control_mutex);
  pool->shutdown = TRUE;
  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);
join_threads:
  for (i = 0; i < threads_started; i++) {
    pthread_join(pool->threads[i], NULL);
  }
destroy_owner_cvs:
  index_shard_context_owner_cvs_destroy(pool);
free_worker_storage:
  free(pool->threads);
  free(pool->contexts);
destroy_shared:
  index_shard_shared_destroy(&pool->shared);
destroy_work_cv:
  pthread_cond_destroy(&pool->work_cv);
destroy_control_mutex:
  pthread_mutex_destroy(&pool->control_mutex);
destroy_inverse_mutex:
  pthread_mutex_destroy(&pool->inverse_cache_mutex);
free_pool:
  free(pool);
unlock_failure:
  pthread_mutex_unlock(&index_shard_global_pool_mutex);
  return -1;
}
/* Stop one explicitly owned pool and join all process-lifetime workers. */
int index_shard_pool_destroy(index_shard_pool_t *pool) {
  int i;
  int notifier_clear_failed = FALSE;

  if (!pool) {
    return 0;
  }

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  if (index_shard_global_pool != pool) {
    logerr("[index-shard] refusing to destroy an unregistered pool\n");
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  pthread_mutex_lock(&pool->control_mutex);

  if (pool->stopping) {
    pthread_mutex_unlock(&pool->control_mutex);
    pthread_mutex_unlock(&index_shard_global_pool_mutex);
    return -1;
  }

  pool->stopping = TRUE;
  pool->poisoned = TRUE;
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  while (pool->pass_active) {
    pthread_cond_wait(&pool->work_cv, &pool->control_mutex);
  }

  pool->shutdown = TRUE;
  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  if (index_shard_global_pool == pool) {
    index_shard_global_pool = NULL;
  }

  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  for (i = 0; i < pool->worker_count; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  if (pool->payload_completion_registered) {
    if (fitsbin_payload_io_clear_completion_notifier(
            index_shard_staged_completion_notify, pool)) {
      logerr("[index-shard] failed to clear payload completion notifier\n");
      notifier_clear_failed = TRUE;
    } else {
      pool->payload_completion_registered = FALSE;
    }
  }
  if (pool->payload_io_owned) {
    fitsbin_payload_io_service_stop();
  }
  if (notifier_clear_failed) {
    /*
     * The process-wide notifier may still retain pool as opaque state, or an
     * external clear may still be draining an active callback. Preserve the
     * stopped allocation rather than risk a callback use-after-free.
     */
    logerr("[index-shard] retaining stopped pool after notifier "
           "teardown failure\n");
    return -1;
  }
  free(pool->threads);
  index_shard_context_owner_cvs_destroy(pool);
  free(pool->contexts);
  index_shard_shared_destroy(&pool->shared);

  logverb("[index-shard] inverse-cache hits=%llu misses=%llu "
          "admitted=%llu refused=%llu evicted=%llu "
          "overcommit=%llu retained=%zu active=%zu "
          "retained_peak=%zu combined_peak=%zu budget=%zu\n",
          pool->inverse_cache_hits,
          pool->inverse_cache_misses,
          pool->inverse_cache_admitted,
          pool->inverse_cache_refused,
          pool->inverse_cache_evicted,
          pool->inverse_cache_overcommit,
          pool->inverse_cache_bytes,
          pool->inverse_active_bytes,
          pool->inverse_cache_peak_bytes,
          pool->inverse_combined_peak_bytes,
          pool->inverse_cache_budget);
  index_shard_inverse_cache_destroy(pool);
  pthread_mutex_destroy(&pool->inverse_cache_mutex);
  pthread_cond_destroy(&pool->work_cv);
  pthread_mutex_destroy(&pool->control_mutex);

  logverb("[index-shard] pthread-pool stop\n");

  free(pool);
  return 0;
}

/*
 * Compatibility entry points for focused onefield tests.  Production engine
 * code owns the returned pool explicitly and binds each sequential job.
 */
int index_shard_pool_start(onefield_t *bp, solver_t *sp) {
  index_shard_pool_t *pool;
  int created = FALSE;
  int already_bound = FALSE;

  if (!index_shard_pthread_enabled(bp)) {
    return 0;
  }
  if (!bp || !sp) {
    return -1;
  }

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  pool = index_shard_global_pool;
  if (pool) {
    pthread_mutex_lock(&pool->control_mutex);
    already_bound = pool->owner_bp == bp &&
        pool->owner_sp == sp && pool->job_active &&
        !pool->shutdown && !pool->stopping && !pool->poisoned;
    pthread_mutex_unlock(&pool->control_mutex);
  }
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  if (already_bound) {
    return 0;
  }
  if (!pool) {
    if (index_shard_pool_create(bp, &pool)) {
      return -1;
    }
    created = TRUE;
  }
  if (index_shard_pool_bind_job(pool, bp, sp)) {
    if (created) {
      (void)index_shard_pool_destroy(pool);
    }
    return -1;
  }
  return 0;
}

void index_shard_pool_stop(onefield_t *bp) {
  index_shard_pool_t *pool;
  solver_t *sp = NULL;

  pthread_mutex_lock(&index_shard_global_pool_mutex);
  pool = index_shard_global_pool;
  if (pool) {
    pthread_mutex_lock(&pool->control_mutex);
    if (pool->owner_bp == bp && pool->job_active) {
      sp = pool->owner_sp;
    }
    pthread_mutex_unlock(&pool->control_mutex);
  }
  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  if (!pool) {
    return;
  }
  if (!sp) {
    logerr("[index-shard] refusing to stop pool owned by another job\n");
    return;
  }
  if (index_shard_pool_unbind_job(pool, bp, sp)) {
    index_shard_pool_poison(pool);
  }
  (void)index_shard_pool_destroy(pool);
}

/*
 * Return true only when the compatibility pool belongs to bp and remains
 * available for a new pass reservation.
 */
int index_shard_pool_active(onefield_t *bp) {
  index_shard_pool_t *pool;
  int active = FALSE;

  pthread_mutex_lock(&index_shard_global_pool_mutex);

  pool = index_shard_global_pool;

  if (pool && pool->job_active && pool->owner_bp == bp) {
    pthread_mutex_lock(&pool->control_mutex);
    active = !pool->shutdown && !pool->stopping && !pool->poisoned;
    pthread_mutex_unlock(&pool->control_mutex);
  }

  pthread_mutex_unlock(&index_shard_global_pool_mutex);

  return active;
}
