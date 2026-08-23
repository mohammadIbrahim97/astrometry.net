/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "index_shard_private.h"
#include "solver_profile_internal.h"
#include "astrometry/errors.h"
#include "astrometry/log.h"

/*
 * Submit one onefield_run() pass to the persistent pool.
 *
 * This resets shared pass state, publishes result arrays, then increments
 * generation to wake workers.
 */
static int index_shard_pool_submit(
    index_shard_pool_t *pool,
    onefield_t *bp,
    solver_t *base_sp,
    size_t nindexes,
    const index_shard_hooks_t *hooks,
    const void *worker_view,
    index_shard_result_t *results) {
  index_shard_thread_state_t *shared = &pool->shared;
  int worker_count = pool->worker_count;
  int i;
  int payload_io_width;
  size_t pass_producer_width;
  anbool pass_exact_demand;

  if (!worker_view) {
    return -1;
  }

  pthread_mutex_lock(&pool->control_mutex);

  if (!pool->pass_active || pool->shutdown ||
      pool->owner_bp != bp || pool->owner_sp != base_sp) {
    pthread_mutex_unlock(&pool->control_mutex);
    return -1;
  }
  payload_io_width = fitsbin_payload_io_service_width();
  pass_exact_demand = index_shard_config_exact_demand_pass(
      pool->payload_completion_registered,
      payload_io_width,
      fitsbin_payload_io_mapped_population_supported(),
      bp->indexnames ? (size_t)sl_size(bp->indexnames) : 0U,
      bp->indexes ? (size_t)pl_size(bp->indexes) : 0U);
  pass_producer_width = index_shard_config_producer_width(
      worker_count,
      payload_io_width,
      pool->payload_completion_registered,
      pass_exact_demand);
  if (!pass_producer_width) {
    logerr("[index-shard] invalid pass width plan "
           "workers=%i payload_io_width=%i detached=%i "
           "exact_demand=%i\n",
           worker_count,
           payload_io_width,
           pool->payload_completion_registered ? 1 : 0,
           pass_exact_demand ? 1 : 0);
    pthread_mutex_unlock(&pool->control_mutex);
    return -1;
  }
  pthread_mutex_lock(&shared->queue_mutex);
  for (i = 0; i < worker_count; i++) {
    if (pool->contexts[i].published_staged_group ||
        pool->contexts[i].owner_waiting ||
        pool->contexts[i].owner_wake_pending) {
      logerr("[index-shard] inner state remained active "
             "before pass worker=%i\n", i);
      pthread_mutex_unlock(&shared->queue_mutex);
      pthread_mutex_unlock(&pool->control_mutex);
      return -1;
    }
  }
  if (shared->completion_active ||
      shared->staged_submit_callbacks_active) {
    logerr("[index-shard] completion registry remained before pass "
           "active=%zu submit_callbacks=%zu\n",
           shared->completion_active,
           shared->staged_submit_callbacks_active);
    pthread_mutex_unlock(&shared->queue_mutex);
    pthread_mutex_unlock(&pool->control_mutex);
    return -1;
  }
  if (shared->queue_waiters) {
    logerr("[index-shard] queue waiters remained before pass "
           "count=%zu\n", shared->queue_waiters);
    pthread_mutex_unlock(&shared->queue_mutex);
    pthread_mutex_unlock(&pool->control_mutex);
    return -1;
  }
  if (shared->staged_groups_active ||
      shared->staged_tickets_active ||
      shared->staged_source_leases ||
      shared->staged_submit_backpressure) {
    logerr("[index-shard] inner activity remained before pass "
           "staged_groups=%zu tickets=%zu leases=%zu "
           "submit_backpressure=%i\n",
           shared->staged_groups_active,
           shared->staged_tickets_active,
           shared->staged_source_leases,
           shared->staged_submit_backpressure);
    pthread_mutex_unlock(&shared->queue_mutex);
    pthread_mutex_unlock(&pool->control_mutex);
    return -1;
  }
  pthread_mutex_lock(&shared->result_mutex);
  pthread_mutex_lock(&shared->state_mutex);
  pthread_mutex_lock(&shared->limit_mutex);

  shared->bp = bp;
  shared->hooks = hooks;
  shared->worker_view = worker_view;
  shared->nindexes = nindexes;
  shared->canonical_scan_cursor = 0U;
  shared->outer_running = 0U;
  shared->producer_width = MIN(
      pass_producer_width, nindexes);
  shared->queue_waiters = 0U;
  shared->staged_groups_active = 0U;
  shared->staged_tickets_active = 0U;
  shared->staged_source_leases = 0U;
  shared->staged_submit_backpressure = FALSE;
  shared->staged_submit_callbacks_active = 0U;
  shared->completion_registry_error = FALSE;
  shared->task_local_failures = 0U;
  shared->global_integrity_failures = 0U;

  shared->results = results;
  shared->next_completion_sequence = 0U;
  shared->next_candidate_sequence = 0U;

  shared->worker_count = worker_count;
  shared->active_workers = worker_count;

  shared->winner_selected = FALSE;
  shared->solved_published = FALSE;
  shared->master_committed = FALSE;
  shared->terminal_cause = INDEX_SHARD_TERMINAL_NONE;
  __atomic_store_n(
      &shared->worker_stop_requested,
      FALSE,
      __ATOMIC_RELEASE);
  shared->selected_index_order = nindexes;
  shared->selected_candidate_sequence = 0U;
  shared->have_committed_result = FALSE;
  shared->committed_index_order = 0U;
  shared->limit_reported = FALSE;

  pthread_mutex_unlock(&shared->limit_mutex);
  pthread_mutex_unlock(&shared->state_mutex);
  pthread_mutex_unlock(&shared->result_mutex);
  pthread_mutex_unlock(&shared->queue_mutex);

  /*
   * The prior generation is quiescent and the next one is not visible yet.
   * Match explicit demand admission to its cold-owner width without holding
   * a shard mutex across the payload provider's own lock.
   */
  fitsbin_payload_io_configure_workers(
      pass_exact_demand
          ? (int)pass_producer_width
          : worker_count);

  // Generation publication is the hard band barrier release.
  pool->generation++;
  pthread_cond_broadcast(&pool->work_cv);
  pthread_mutex_unlock(&pool->control_mutex);

  logverb("[index-shard] pthread-pool submit compute_width=%i "
          "producer_width=%zu helper_width=%zu "
          "candidates=%zu engine_pass=%zu "
          "depth_index=%zu scale_index=%zu startobj=%i endobj=%i "
          "scheduler=first-valid-completion inner_scheduler=ordered-codekd-packets "
          "mmap_advice=%s "
          "mmap_scope=payload-random-topology-normal "
          "mmap_policy=parallel-random-serial-normal "
          "page_delivery=%s payload_io=%s payload_io_width=%i "
          "outer_admission=%s\n",
          worker_count,
          shared->producer_width,
          (size_t)worker_count - shared->producer_width,
          nindexes,
          bp->engine_pass_ordinal,
          bp->engine_depth_index,
          bp->engine_scale_index,
          base_sp->startobj,
          base_sp->endobj,
          fitsbin_mmap_advice_name(FITSBIN_MMAP_ADVICE_RANDOM),
          pool->payload_completion_registered
              ? "detached-bounded-mapped-completion"
              : "native-mmap-demand",
          payload_io_width > 0 ? "kernel-page-cache" : "native-mmap",
          payload_io_width,
          !pool->payload_completion_registered
              ? "reserved-helper"
              : (pass_exact_demand
                     ? "provider-bounded-exact-demand"
                     : "full-producer"));

  return 0;
}

/*
 * Execute one complete index-shard pass.
 *
 * Terminal status is classified by failure scope and the master mutation
 * boundary. Only an unavailable path or an isolated task-local failure proven
 * to occur before master commit may return control to the serial path. A
 * global-integrity failure is terminal regardless of winner timing.
 */
index_shard_solve_status_t
index_shard_solve(onefield_t *bp,
                  solver_t *base_sp,
                  size_t nindexes,
                  const index_shard_hooks_t *hooks) {
  index_shard_pool_t *pool;
  index_shard_result_t *results;
  void *worker_view = NULL;
  size_t i;
  int acquire_rc;
  int rc = 0;
  index_shard_pass_state_snapshot_t state;
  solver_profile_t solver_profile;
  index_shard_solve_status_t status = INDEX_SHARD_SOLVE_HANDLED;
  anbool clean_exhaustion_required;
  anbool worker_cancelled = FALSE;

  if (!index_shard_pthread_enabled(bp)) {
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
    logverb("[index-shard] pthread mode requested but pool inactive\n");
    return INDEX_SHARD_SOLVE_UNAVAILABLE;
  }

  if (acquire_rc != INDEX_SHARD_POOL_ACQUIRE_OK) {
    logerr("[index-shard] pool ownership or pass lifecycle conflict; "
           "serial fallback suppressed\n");
    return INDEX_SHARD_SOLVE_LIFECYCLE_CONFLICT;
  }

  if (!hooks->create_worker_view ||
      !hooks->destroy_worker_view ||
      !hooks->prepare_local_context ||
      hooks->create_worker_view(
          bp, base_sp, &worker_view) ||
      !worker_view) {
    logerr("[index-shard] failed to create immutable worker view\n");
    if (worker_view && hooks->destroy_worker_view) {
      hooks->destroy_worker_view(worker_view);
    }
    index_shard_pool_release_pass(pool);
    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }

  if (nindexes > SIZE_MAX / sizeof(*results)) {
    logerr("[index-shard] result-array size overflow\n");
    hooks->destroy_worker_view(worker_view);
    index_shard_pool_release_pass(pool);
    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }
  results = calloc(nindexes, sizeof(*results));
  if (!results) {
    SYSERROR("Failed to allocate index-shard pass state");
    hooks->destroy_worker_view(worker_view);
    index_shard_pool_release_pass(pool);
    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }
  // Submit releases the hard current-band barrier to persistent workers.
  rc = index_shard_pool_submit(
      pool,
      bp,
      base_sp,
      nindexes,
      hooks,
      worker_view,
      results);

  if (rc) {
    free(results);
    hooks->destroy_worker_view(worker_view);
    index_shard_pool_release_pass(pool);
    return INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE;
  }

  rc = index_shard_pool_reduce_first_valid(pool);

  index_shard_pass_state_snapshot(&pool->shared, &state);
  clean_exhaustion_required =
      state.terminal_cause == INDEX_SHARD_TERMINAL_NONE;
  pthread_mutex_lock(&pool->shared.queue_mutex);
  for (i = 0U; i < (size_t)pool->shared.worker_count; i++) {
    if (pool->contexts[i].published_staged_group) {
      logerr("[index-shard] inner state remained active "
             "after worker quiescence worker=%zu\n", i);
      rc = -1;
      status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
    }
  }
  if (pool->shared.queue_waiters) {
    logerr("[index-shard] queue waiters remained after "
           "worker quiescence count=%zu\n",
           pool->shared.queue_waiters);
    rc = -1;
    status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
  }
  if (pool->shared.staged_groups_active ||
      pool->shared.staged_tickets_active ||
      pool->shared.staged_source_leases) {
    logerr("[index-shard] staged activity remained after "
           "worker quiescence groups=%zu tickets=%zu leases=%zu\n",
           pool->shared.staged_groups_active,
           pool->shared.staged_tickets_active,
           pool->shared.staged_source_leases);
    rc = -1;
    status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
  }
  if (pool->shared.completion_active ||
      pool->shared.staged_submit_callbacks_active ||
      pool->shared.completion_registry_error) {
    logerr("[index-shard] completion registry remained after "
           "worker quiescence active=%zu submit_callbacks=%zu "
           "error=%i\n",
           pool->shared.completion_active,
           pool->shared.staged_submit_callbacks_active,
           pool->shared.completion_registry_error ? 1 : 0);
    rc = -1;
    status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
  }
  if (pool->shared.outer_running) {
    logerr("[index-shard] pass ended with %zu outer owners\n",
           pool->shared.outer_running);
    rc = -1;
  }
  if (clean_exhaustion_required &&
      pool->shared.canonical_scan_cursor != nindexes) {
    logerr("[index-shard] clean pass ended before claim exhaustion "
           "cursor=%zu candidates=%zu\n",
           pool->shared.canonical_scan_cursor,
           nindexes);
    rc = -1;
  }
  if (clean_exhaustion_required) {
    for (i = 0U; i < nindexes; i++) {
      if (results[i].completion_sequence) {
        continue;
      }
      logerr("[index-shard] clean pass ended with unfinished "
             "index_order=%zu\n", i);
      rc = -1;
      break;
    }
  }
  pthread_mutex_unlock(&pool->shared.queue_mutex);

  memset(&solver_profile, 0, sizeof(solver_profile));

  /*
   * Worker-local cancellation is published only after every result slot is
   * immutable. Master onefield state remains reducer/caller-owned, while the
   * limit mutex preserves the existing synchronization discipline for the
   * cancellation flag.
  */
  for (i = 0; i < nindexes; i++) {
    solver_profile_accumulate(
        &solver_profile,
        &results[i].solver_profile);

    if (results[i].cancelled) {
      worker_cancelled = TRUE;
    }
  }

  if (worker_cancelled && !state.winner_selected) {
    pthread_mutex_lock(&pool->shared.limit_mutex);
    bp->cancelled = TRUE;
    pthread_mutex_unlock(&pool->shared.limit_mutex);
  }

  index_shard_pass_state_snapshot(&pool->shared, &state);

  if (state.terminal_cause == INDEX_SHARD_TERMINAL_GLOBAL_INTEGRITY ||
      state.global_integrity_failures) {
    rc = -1;
    status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
  } else if (rc &&
             status != INDEX_SHARD_SOLVE_TERMINAL_FAILURE) {
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

  if (status == INDEX_SHARD_SOLVE_HANDLED &&
      rc == 0 &&
      state.solved_published) {
    if (index_shard_report_committed_solution(
            bp,
            nindexes,
            &pool->shared,
            hooks,
            results)) {
      rc = -1;
      status = INDEX_SHARD_SOLVE_TERMINAL_FAILURE;
    }
  }

  // dispose unmerged worker results after all workers have left pass
  for (i = 0; i < nindexes; i++) {
    index_shard_result_dispose(&results[i], hooks);
  }

  logverb("[index-shard] solver-pass generation=%lu candidates=%zu "
          "failed=%i codekd_calls=%llu codekd_hits=%llu "
          "resolve_calls=%llu verify_calls=%llu alloc_failures=%llu "
          "max_parallel=%zu helper_tasks=%llu "
          "windows=%llu "
          "hypothesis_order=%016llx kd_result_order=%016llx "
          "candidate_order=%016llx\n",
          pool->generation,
          nindexes,
          solver_profile.execution_failed ? 1 : 0,
          solver_profile.codekd_calls,
          solver_profile.codekd_hits,
          solver_profile.resolve_calls,
          solver_profile.verify_calls,
          solver_profile.allocation_failures,
          solver_profile.max_parallel_ranges,
          solver_profile.ab_helper_tasks,
          solver_profile.candidate_delivery_windows,
          solver_profile.hypothesis_order_hash,
          solver_profile.kd_result_order_hash,
          solver_profile.candidate_order_hash);

  pthread_mutex_lock(&pool->shared.queue_mutex);
  pool->shared.producer_width = 0U;
  pthread_mutex_unlock(&pool->shared.queue_mutex);
  pool->shared.worker_view = NULL;
  hooks->destroy_worker_view(worker_view);
  worker_view = NULL;
  free(results);
  index_shard_pool_release_pass(pool);
  return status;
}
