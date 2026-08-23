/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <stdio.h>
#include <string.h>

#include "index_shard_private.h"
#include "astrometry/engine.h"

static int failures = 0;

typedef struct pool_release_test_state {
  index_shard_pool_t *pool;
  int status;
} pool_release_test_state_t;

typedef struct pool_test_view {
  index_t index;
} pool_test_view_t;

static pool_test_view_t pool_test_view;

static index_shard_hook_result_t pool_test_get_index(
    const void *worker_view,
    size_t index_order,
    index_t **index_out) {
  const pool_test_view_t *view = worker_view;
  index_shard_hook_result_t result = {
      INDEX_SHARD_HOOK_COMPLETED_UNSOLVED, 0};

  (void)index_order;
  *index_out = (index_t*)&view->index;
  return result;
}

static index_shard_hook_result_t pool_test_done_with_index(
    onefield_t *bp,
    size_t index_order,
    index_t *index) {
  index_shard_hook_result_t result = {
      INDEX_SHARD_HOOK_COMPLETED_UNSOLVED, 0};

  (void)bp;
  (void)index_order;
  (void)index;
  return result;
}

static int pool_test_create_worker_view(onefield_t *bp,
                                        const solver_t *sp,
                                        void **view_out) {
  (void)bp;
  (void)sp;
  *view_out = &pool_test_view;
  return 0;
}

static void pool_test_destroy_worker_view(void *view) {
  (void)view;
}

static int pool_test_prepare_local_context(onefield_t *local_bp,
                                           const void *view) {
  (void)view;
  onefield_init(local_bp);
  solver_set_default_values(&local_bp->solver);
  return 0;
}

static void pool_test_reset_local_context(onefield_t *local_bp,
                                          bl *solutions) {
  (void)local_bp;
  (void)solutions;
}

static void pool_test_cleanup_local_context(onefield_t *local_bp) {
  solver_cleanup(&local_bp->solver);
  onefield_cleanup(local_bp);
}

static index_shard_hook_result_t pool_test_solve_one_index(
    onefield_t *local_bp,
    index_t *index) {
  index_shard_hook_result_t result = {
      INDEX_SHARD_HOOK_COMPLETED_UNSOLVED, 0};

  (void)local_bp;
  (void)index;
  return result;
}

static index_shard_hook_result_t pool_test_analyze_solutions(
    onefield_t *bp,
    bl *solutions,
    double *best_logodds,
    int *best_fieldnum) {
  index_shard_hook_result_t result = {
      INDEX_SHARD_HOOK_COMPLETED_UNSOLVED, 0};

  (void)bp;
  (void)solutions;
  *best_logodds = 0.0;
  *best_fieldnum = 0;
  return result;
}

static int pool_test_merge_solutions(onefield_t *bp,
                                     bl *solutions,
                                     anbool *solved_out) {
  (void)bp;
  (void)solutions;
  *solved_out = FALSE;
  return 0;
}

static void pool_test_free_solutions(bl *solutions) {
  bl_free(solutions);
}

static const index_shard_hooks_t pool_test_hooks = {
  .get_index = pool_test_get_index,
  .done_with_index = pool_test_done_with_index,
  .create_worker_view = pool_test_create_worker_view,
  .destroy_worker_view = pool_test_destroy_worker_view,
  .prepare_local_context = pool_test_prepare_local_context,
  .reset_local_context_for_task = pool_test_reset_local_context,
  .cleanup_local_context = pool_test_cleanup_local_context,
  .solve_one_index = pool_test_solve_one_index,
  .analyze_solutions = pool_test_analyze_solutions,
  .merge_solutions = pool_test_merge_solutions,
  .free_solutions = pool_test_free_solutions,
};

#define CHECK(expression)                                                \
  do {                                                                   \
    if (!(expression)) {                                                 \
      fprintf(stderr, "CHECK failed at %s:%i: %s\n",                 \
              __FILE__, __LINE__, #expression);                         \
      failures++;                                                        \
    }                                                                    \
  } while (0)

static void init_field(onefield_t *bp, int workers) {
  onefield_init(bp);
  solver_set_default_values(&bp->solver);
  bp->index_shard_workers = workers;
}

static void *pool_release_test_main(void *opaque) {
  pool_release_test_state_t *state = opaque;

  state->status = index_shard_pool_release_pass(state->pool);
  return NULL;
}

int main(void) {
  index_shard_pool_t *pool = NULL;
  index_shard_pool_t *acquired = NULL;
  onefield_t first;
  onefield_t second;
  onefield_t ineligible;
  onefield_t wrong_width;
  pthread_t stable_threads[2];
  pthread_t release_thread;
  pthread_t *stable_thread_array;
  index_shard_result_t release_result;
  pool_release_test_state_t release_state;
  index_shard_solve_status_t solve_status;
  int release_thread_status;
  engine_t *engine = NULL;

  init_field(&first, 2);
  init_field(&second, 2);
  init_field(&ineligible, 1);
  init_field(&wrong_width, 3);
  memset(&pool_test_view, 0, sizeof(pool_test_view));
  pool_test_view.index.indexname = "pool-lifecycle-index";

  CHECK(index_shard_pool_create(&first, &pool) == 0);
  CHECK(pool != NULL);
  if (!pool) {
    goto cleanup_fields;
  }
  CHECK(pool->owner_bp == NULL);
  CHECK(pool->owner_sp == NULL);
  CHECK(pool->job_active == FALSE);
  CHECK(pool->pass_active == FALSE);
  CHECK(pool->generation == 0UL);
  CHECK(index_shard_pool_job_width_compatible(pool, &first));
  CHECK(index_shard_pool_job_width_compatible(pool, &second));
  CHECK(!index_shard_pool_job_width_compatible(pool, &wrong_width));
  if (pool->payload_completion_registered) {
    CHECK(fitsbin_payload_io_wait_completion_notifier_idle(
        index_shard_staged_completion_notify, pool) == 0);
  }
  stable_thread_array = pool->threads;
  stable_threads[0] = pool->threads[0];
  stable_threads[1] = pool->threads[1];

  CHECK(index_shard_pool_bind_job(
      pool, &first, &first.solver) == 0);
  CHECK(pool->job_active == TRUE);
  CHECK(pool->owner_bp == &first);
  CHECK(pool->owner_sp == &first.solver);
  CHECK(pool->job_generation == 1UL);
  CHECK(index_shard_pool_active(&first));

  CHECK(index_shard_pool_bind_job(
      pool, &second, &second.solver) != 0);
  solve_status = index_shard_solve(
      &first, &first.solver, 1U, &pool_test_hooks);
  CHECK(solve_status == INDEX_SHARD_SOLVE_HANDLED);
  CHECK(pool->generation == 1UL);
  CHECK(pool->pass_active == FALSE);
  CHECK(pool->shared.bp == NULL);
  CHECK(pool->shared.hooks == NULL);
  CHECK(pool->shared.worker_view == NULL);
  CHECK(pool->shared.results == NULL);
  CHECK(pool->shared.nindexes == 0U);

  solve_status = index_shard_solve(
      &first, &first.solver, 1U, &pool_test_hooks);
  CHECK(solve_status == INDEX_SHARD_SOLVE_HANDLED);
  CHECK(pool->generation == 2UL);

  CHECK(index_shard_pool_unbind_job(
      pool, &first, &first.solver) == 0);
  CHECK(pool->job_active == FALSE);
  CHECK(pool->owner_bp == NULL);
  CHECK(pool->owner_sp == NULL);
  CHECK(pool->generation == 2UL);
  CHECK(!index_shard_pool_active(&first));

  /* A W1/ineligible job bypasses the idle W2 pool without disturbing it. */
  solve_status = index_shard_solve(
      &ineligible, &ineligible.solver, 1U, &pool_test_hooks);
  CHECK(solve_status == INDEX_SHARD_SOLVE_UNAVAILABLE);
  CHECK(pool->generation == 2UL);
  CHECK(pool->job_generation == 1UL);
  CHECK(pool->threads == stable_thread_array);
  CHECK(pthread_equal(pool->threads[0], stable_threads[0]));
  CHECK(pthread_equal(pool->threads[1], stable_threads[1]));

  CHECK(index_shard_pool_bind_job(
      pool, &wrong_width, &wrong_width.solver) != 0);
  CHECK(!index_shard_pool_is_poisoned(pool));
  CHECK(index_shard_pool_bind_job(
      pool, &second, &second.solver) == 0);
  CHECK(pool->job_generation == 2UL);
  CHECK(pool->generation == 2UL);
  CHECK(index_shard_pool_active(&second));
  solve_status = index_shard_solve(
      &second, &second.solver, 1U, &pool_test_hooks);
  CHECK(solve_status == INDEX_SHARD_SOLVE_HANDLED);
  CHECK(pool->generation == 3UL);
  CHECK(pool->threads == stable_thread_array);
  CHECK(pthread_equal(pool->threads[0], stable_threads[0]));
  CHECK(pthread_equal(pool->threads[1], stable_threads[1]));

  /*
   * Hold one synthetic participant past release entry. The releaser must
   * poison and wait without clearing any pass-owned pointer. Retiring the
   * participant is the deterministic latch that permits final cleanup.
   */
  acquired = NULL;
  CHECK(index_shard_pool_acquire_pass(
      &second, &second.solver, &acquired) ==
      INDEX_SHARD_POOL_ACQUIRE_OK);
  CHECK(acquired == pool);
  memset(&release_result, 0, sizeof(release_result));
  pthread_mutex_lock(&pool->shared.queue_mutex);
  pthread_mutex_lock(&pool->shared.result_mutex);
  pool->shared.bp = &second;
  pool->shared.hooks = &pool_test_hooks;
  pool->shared.worker_view = &pool_test_view;
  pool->shared.results = &release_result;
  pool->shared.nindexes = 1U;
  pool->shared.producer_width = 1U;
  pool->shared.worker_count = pool->worker_count;
  pool->shared.active_workers = 1;
  pthread_mutex_unlock(&pool->shared.result_mutex);
  pthread_mutex_unlock(&pool->shared.queue_mutex);

  release_state.pool = pool;
  release_state.status = 0;
  release_thread_status = pthread_create(
      &release_thread, NULL, pool_release_test_main, &release_state);
  CHECK(release_thread_status == 0);
  if (!release_thread_status) {
    pthread_mutex_lock(&pool->control_mutex);
    while (!pool->pass_releasing) {
      pthread_cond_wait(&pool->work_cv, &pool->control_mutex);
    }
    CHECK(pool->pass_active == TRUE);
    CHECK(pool->shared.bp == &second);
    CHECK(pool->shared.hooks == &pool_test_hooks);
    CHECK(pool->shared.worker_view == &pool_test_view);
    CHECK(pool->shared.results == &release_result);
    pthread_mutex_unlock(&pool->control_mutex);

    pthread_mutex_lock(&pool->shared.result_mutex);
    pool->shared.active_workers = 0;
    pthread_cond_broadcast(&pool->shared.result_cv);
    pthread_mutex_unlock(&pool->shared.result_mutex);
    CHECK(pthread_join(release_thread, NULL) == 0);
    CHECK(release_state.status != 0);
  } else {
    pthread_mutex_lock(&pool->shared.result_mutex);
    pool->shared.active_workers = 0;
    pthread_cond_broadcast(&pool->shared.result_cv);
    pthread_mutex_unlock(&pool->shared.result_mutex);
    CHECK(index_shard_pool_release_pass(pool) != 0);
  }
  CHECK(pool->pass_active == FALSE);
  CHECK(pool->pass_releasing == FALSE);
  CHECK(pool->shared.bp == NULL);
  CHECK(pool->shared.hooks == NULL);
  CHECK(pool->shared.worker_view == NULL);
  CHECK(pool->shared.results == NULL);
  if (pool->payload_completion_registered) {
    CHECK(fitsbin_payload_io_wait_completion_notifier_idle(
        index_shard_staged_completion_notify, pool) == 0);
  }

  index_shard_pool_poison(pool);
  CHECK(index_shard_pool_is_poisoned(pool));
  CHECK(index_shard_pool_acquire_pass(
      &second, &second.solver, &acquired) ==
      INDEX_SHARD_POOL_ACQUIRE_CONFLICT);
  CHECK(acquired == NULL);
  CHECK(index_shard_pool_unbind_job(
      pool, &second, &second.solver) == 0);

  CHECK(index_shard_pool_destroy(pool) == 0);
  pool = NULL;

  /* engine_free() is the production process-lifetime destruction boundary. */
  engine = engine_new();
  CHECK(engine != NULL);
  if (engine) {
    CHECK(index_shard_pool_create(
        &first, &engine->index_shard_pool) == 0);
    CHECK(engine->index_shard_pool != NULL);
    engine_free(engine);
    engine = NULL;

    /* The process singleton must be available after engine teardown. */
    CHECK(index_shard_pool_create(&first, &pool) == 0);
    CHECK(pool != NULL);
    CHECK(index_shard_pool_destroy(pool) == 0);
    pool = NULL;
  }

cleanup_fields:
  if (engine) {
    engine_free(engine);
  }
  if (pool) {
    (void)index_shard_pool_destroy(pool);
  }
  solver_cleanup(&first.solver);
  onefield_cleanup(&first);
  solver_cleanup(&second.solver);
  onefield_cleanup(&second);
  solver_cleanup(&ineligible.solver);
  onefield_cleanup(&ineligible);
  solver_cleanup(&wrong_width.solver);
  onefield_cleanup(&wrong_width);

  if (failures) {
    fprintf(stderr,
            "INDEX_SHARD_POOL_LIFECYCLE_TEST_FAILED failures=%i\n",
            failures);
    return 1;
  }
  printf("INDEX_SHARD_POOL_LIFECYCLE_TEST_OK "
         "creations=1 jobs=2 passes=3 generation=3 "
         "stable_threads=2 drained_release=1 ineligible_w1=1 "
         "engine_teardown=1\n");
  return 0;
}
