/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "index_shard_private.h"
#include "astrometry/bl.h"
#include "astrometry/errors.h"
#include "astrometry/log.h"
#include "astrometry/tic.h"
#include "astrometry/fitsbin.h"
#include "astrometry/fitsioutils.h"
#ifdef TESTING_INDEX_SHARD_STAGED
/*
 * Deterministic tests for the private staged scheduler state machine.
 *
 * This translation unit is compiled only by the TESTING_INDEX_SHARD_STAGED
 * build, so production visibility and state layout remain unchanged.
 */

typedef struct index_shard_staged_retire_test_context {
  index_shard_thread_state_t *shared;
  size_t order[8];
  size_t calls;
  size_t task_zero_calls;
  anbool stop_during_retire;
} index_shard_staged_retire_test_context_t;

typedef struct index_shard_owner_wake_test_context {
  index_shard_thread_state_t *shared;
  index_shard_worker_context_t *worker;
  pthread_cond_t started_cv;
  anbool started;
  index_shard_work_selection_t selection;
  size_t index_order;
  index_shard_inner_claim_t claim;
} index_shard_owner_wake_test_context_t;

static void *index_shard_owner_wake_test_worker(void *opaque) {
  index_shard_owner_wake_test_context_t *context = opaque;
  struct timespec deadline;
  int wait_status = 0;

  if (clock_gettime(CLOCK_REALTIME, &deadline)) {
    context->selection = INDEX_SHARD_WORK_ERROR;
    return NULL;
  }
  deadline.tv_sec += 2;

  pthread_mutex_lock(&context->shared->queue_mutex);
  context->worker->owner_waiting = TRUE;
  context->started = TRUE;
  pthread_cond_signal(&context->started_cv);
  while (!context->worker->owner_wake_pending &&
         wait_status != ETIMEDOUT) {
    wait_status = pthread_cond_timedwait(
        &context->worker->owner_cv,
        &context->shared->queue_mutex,
        &deadline);
  }
  if (!context->worker->owner_wake_pending) {
    context->worker->owner_waiting = FALSE;
    context->selection = INDEX_SHARD_WORK_ERROR;
    pthread_mutex_unlock(&context->shared->queue_mutex);
    return NULL;
  }
  context->worker->owner_wake_pending = FALSE;
  context->worker->owner_waiting = FALSE;
  pthread_mutex_unlock(&context->shared->queue_mutex);

  context->selection = index_shard_select_work(
      context->worker, context->shared,
      &context->index_order, &context->claim);
  return NULL;
}

index_shard_staged_retire_status_t
index_shard_staged_retire_test_callback(
    const index_shard_staged_task_t *task,
    size_t task_index,
    void *opaque) {
  index_shard_staged_retire_test_context_t *context = opaque;

  if (!task || !context ||
      context->calls >= sizeof(context->order) /
          sizeof(context->order[0])) {
    return INDEX_SHARD_STAGED_RETIRE_ERROR;
  }
  context->order[context->calls++] = task_index;
  if (context->stop_during_retire) {
    pthread_mutex_lock(&context->shared->state_mutex);
    index_shard_publish_terminal_locked(
        context->shared, INDEX_SHARD_TERMINAL_CANCELLED);
    pthread_mutex_unlock(&context->shared->state_mutex);
    return INDEX_SHARD_STAGED_RETIRE_MORE;
  }
  if (task_index == 0U && context->task_zero_calls++ < 2U) {
    return INDEX_SHARD_STAGED_RETIRE_MORE;
  }
  return INDEX_SHARD_STAGED_RETIRE_OK;
}

int index_shard_staged_retire_test_init(
    index_shard_thread_state_t *shared) {
  int rc;

  memset(shared, 0, sizeof(*shared));
  rc = pthread_mutex_init(&shared->queue_mutex, NULL);
  if (rc) {
    return -1;
  }
  rc = pthread_mutex_init(&shared->state_mutex, NULL);
  if (rc) {
    pthread_mutex_destroy(&shared->queue_mutex);
    return -1;
  }
  rc = pthread_cond_init(&shared->queue_cv, NULL);
  if (rc) {
    pthread_mutex_destroy(&shared->state_mutex);
    pthread_mutex_destroy(&shared->queue_mutex);
    return -1;
  }
  shared->worker_count = 2;
  if (index_shard_completion_registry_init(shared, 2)) {
    pthread_cond_destroy(&shared->queue_cv);
    pthread_mutex_destroy(&shared->state_mutex);
    pthread_mutex_destroy(&shared->queue_mutex);
    return -1;
  }
  return 0;
}

void index_shard_staged_retire_test_destroy(
    index_shard_thread_state_t *shared) {
  index_shard_completion_registry_destroy(shared);
  pthread_cond_destroy(&shared->queue_cv);
  pthread_mutex_destroy(&shared->state_mutex);
  pthread_mutex_destroy(&shared->queue_mutex);
}

int index_shard_staged_retire_test_ready(
    index_shard_thread_state_t *shared,
    index_shard_staged_group_t *group,
    index_shard_staged_task_t *task) {
  int rc;

  pthread_mutex_lock(&shared->queue_mutex);
  rc = index_shard_staged_set_state_locked(
      shared, group, task, INDEX_SHARD_STAGED_TASK_RESULTS_READY);
  pthread_mutex_unlock(&shared->queue_mutex);
  return rc;
}

int index_shard_staged_retire_test_order(void) {
  index_shard_thread_state_t shared;
  index_shard_staged_group_t group;
  index_shard_staged_task_t tasks[2];
  index_shard_staged_retire_test_context_t context;
  int failures = 0;

  if (index_shard_staged_retire_test_init(&shared)) {
    return 1;
  }
  memset(&group, 0, sizeof(group));
  memset(tasks, 0, sizeof(tasks));
  memset(&context, 0, sizeof(context));
  group.tasks = tasks;
  group.task_count = 2U;
  group.retire = index_shard_staged_retire_test_callback;
  group.owner_context = &context;
  context.shared = &shared;
  pthread_mutex_lock(&shared.queue_mutex);
  failures += index_shard_staged_set_state_locked(
      &shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_PREPARE_READY) != 0;
  failures += index_shard_staged_set_state_locked(
      &shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_RESULTS_READY) != 0;
  pthread_mutex_unlock(&shared.queue_mutex);

  failures += index_shard_staged_retire_one(&shared, &group) != 1;
  failures += context.calls != 0U;
  failures += group.next_retire != 0U;

  failures += index_shard_staged_retire_test_ready(
      &shared, &group, &tasks[0]) != 0;
  failures += index_shard_staged_retire_one(&shared, &group) != 0;
  failures += tasks[0].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_PREPARE_READY;
  failures += group.next_retire != 0U;

  failures += index_shard_staged_retire_test_ready(
      &shared, &group, &tasks[0]) != 0;
  failures += index_shard_staged_retire_one(&shared, &group) != 0;
  failures += tasks[0].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_PREPARE_READY;
  failures += group.next_retire != 0U;

  failures += index_shard_staged_retire_test_ready(
      &shared, &group, &tasks[0]) != 0;
  failures += index_shard_staged_retire_one(&shared, &group) != 0;
  failures += tasks[0].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_RETIRED;
  failures += group.next_retire != 1U;

  failures += index_shard_staged_retire_one(&shared, &group) != 0;
  failures += tasks[1].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_RETIRED;
  failures += group.next_retire != 2U;
  failures += context.calls != 4U;
  failures += context.order[0] != 0U;
  failures += context.order[1] != 0U;
  failures += context.order[2] != 0U;
  failures += context.order[3] != 1U;
  failures += group.internal_error;
  failures += group.task_failed;

  index_shard_staged_retire_test_destroy(&shared);
  return failures;
}

int index_shard_staged_retire_test_terminal(void) {
  index_shard_thread_state_t shared;
  index_shard_staged_group_t group;
  index_shard_staged_task_t tasks[2];
  index_shard_staged_retire_test_context_t context;
  int failures = 0;

  if (index_shard_staged_retire_test_init(&shared)) {
    return 1;
  }
  memset(&group, 0, sizeof(group));
  memset(tasks, 0, sizeof(tasks));
  memset(&context, 0, sizeof(context));
  group.tasks = tasks;
  group.task_count = 2U;
  group.retire = index_shard_staged_retire_test_callback;
  group.owner_context = &context;
  context.shared = &shared;
  context.stop_during_retire = TRUE;
  pthread_mutex_lock(&shared.queue_mutex);
  failures += index_shard_staged_set_state_locked(
      &shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_RESULTS_READY) != 0;
  failures += index_shard_staged_set_state_locked(
      &shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  pthread_mutex_unlock(&shared.queue_mutex);

  failures += index_shard_staged_retire_one(&shared, &group) != -1;
  failures += context.calls != 1U;
  failures += context.order[0] != 0U;
  failures += group.next_retire != 0U;
  failures += tasks[0].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_STOPPED;
  failures += tasks[1].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_STOPPED;
  failures += !group.cancelling;
  failures += !group.stop_seen;
  failures += group.internal_error;
  failures += group.task_failed;

  index_shard_staged_retire_test_destroy(&shared);
  return failures;
}

int index_shard_staged_mask_selection_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t contexts[2];
  index_shard_staged_group_t groups[2];
  index_shard_staged_task_t tasks[2][2];
  index_shard_staged_claim_t claim;
  index_shard_thread_state_t *shared;
  int failures = 0;
  int i;

  memset(&pool, 0, sizeof(pool));
  memset(contexts, 0, sizeof(contexts));
  memset(groups, 0, sizeof(groups));
  memset(tasks, 0, sizeof(tasks));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }
  pool.worker_count = 2;
  pool.generation = 7U;
  pool.contexts = contexts;
  shared->pool = &pool;
  shared->worker_count = 2;
  for (i = 0; i < 2; i++) {
    contexts[i].worker_id = i;
    contexts[i].pool = &pool;
    contexts[i].generation_seen = pool.generation;
    contexts[i].current_outer_active = TRUE;
    contexts[i].current_index_order = i ? 3U : 5U;
    contexts[i].staged_group_epoch = 11U + (unsigned long long)i;
    contexts[i].published_staged_group = &groups[i];
    groups[i].pool = &pool;
    groups[i].tasks = tasks[i];
    groups[i].task_count = 2U;
    groups[i].generation = pool.generation;
    groups[i].owner_epoch = contexts[i].staged_group_epoch;
    groups[i].owner_worker = i;
    groups[i].owner_index_order = contexts[i].current_index_order;
  }

  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &groups[0], &tasks[0][0],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &groups[1], &tasks[1][1],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  failures += index_shard_staged_select_locked(
      &contexts[0], shared,
      INDEX_SHARD_STAGED_SELECT_COMPUTE,
      TRUE, FALSE, &claim) != 0;
  failures += claim.group != &groups[0];
  failures += claim.task_index != 0U;
  failures += claim.kind != INDEX_SHARD_STAGED_CLAIM_EXECUTE;
  failures += !claim.owner_claim;
  failures += groups[0].compute_ready_mask != UINT64_C(0);
  failures += tasks[0][0].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_EXECUTING;
  failures += index_shard_staged_select_locked(
      &contexts[0], shared,
      INDEX_SHARD_STAGED_SELECT_COMPUTE,
      FALSE, TRUE, &claim) != 0;
  failures += claim.group != &groups[1];
  failures += claim.task_index != 1U;
  failures += claim.kind != INDEX_SHARD_STAGED_CLAIM_EXECUTE;
  failures += groups[1].compute_ready_mask != UINT64_C(0);
  failures += tasks[1][1].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_EXECUTING;
  pthread_mutex_unlock(&shared->queue_mutex);

  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_ready_priority_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t contexts[2];
  index_shard_staged_group_t group;
  index_shard_staged_task_t tasks[2];
  index_shard_thread_state_t *shared;
  index_shard_inner_claim_t claim;
  size_t index_order = SIZE_MAX;
  index_shard_work_selection_t selection;
  int failures = 0;

  memset(&pool, 0, sizeof(pool));
  memset(contexts, 0, sizeof(contexts));
  memset(&group, 0, sizeof(group));
  memset(tasks, 0, sizeof(tasks));
  memset(&claim, 0, sizeof(claim));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }

  pool.worker_count = 2;
  pool.generation = 8U;
  pool.contexts = contexts;
  shared->pool = &pool;
  shared->worker_count = 2;
  shared->nindexes = 3U;
  shared->outer_running = 1U;
  shared->producer_width = 2U;
  shared->canonical_scan_cursor = 1U;

  contexts[0].worker_id = 0;
  contexts[0].pool = &pool;
  contexts[0].generation_seen = pool.generation;
  contexts[0].current_outer_active = TRUE;
  contexts[0].current_index_order = 0U;
  contexts[0].staged_group_epoch = 31U;
  contexts[0].published_staged_group = &group;
  contexts[1].worker_id = 1;
  contexts[1].pool = &pool;
  contexts[1].generation_seen = pool.generation;

  group.pool = &pool;
  group.tasks = tasks;
  group.task_count = 2U;
  group.generation = pool.generation;
  group.owner_epoch = contexts[0].staged_group_epoch;
  group.owner_worker = 0;
  group.owner_index_order = 0U;

  /* Without executable inner work, claim the canonical outer task. */
  selection = index_shard_select_work(
      &contexts[1], shared, &index_order, &claim);
  failures += selection != INDEX_SHARD_WORK_OUTER;
  failures += index_order != 1U;
  failures += shared->canonical_scan_cursor != 2U;
  failures += shared->outer_running != 2U;

  /* Completing an outer task permits one READY handoff. */
  pthread_mutex_lock(&shared->queue_mutex);
  shared->outer_running--;
  contexts[1].ready_before_outer_eligible = TRUE;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  pthread_mutex_unlock(&shared->queue_mutex);

  memset(&claim, 0, sizeof(claim));
  index_order = SIZE_MAX;
  selection = index_shard_select_work(
      &contexts[1], shared, &index_order, &claim);
  failures += selection != INDEX_SHARD_WORK_INNER;
  failures += claim.group != &group;
  failures += claim.task_index != 0U;
  failures += claim.owner_claim;
  failures += shared->canonical_scan_cursor != 2U;
  failures += contexts[1].ready_before_outer_eligible;

  failures += index_shard_staged_complete_claim(
      shared, &claim,
      INDEX_SHARD_STAGED_EXECUTE_OK, 0ULL) != 0;

  /* The bounded handoff is consumed, so outer admission regains priority. */
  memset(&claim, 0, sizeof(claim));
  index_order = SIZE_MAX;
  selection = index_shard_select_work(
      &contexts[1], shared, &index_order, &claim);
  failures += selection != INDEX_SHARD_WORK_OUTER;
  failures += index_order != 2U;
  failures += shared->canonical_scan_cursor != 3U;
  failures += shared->outer_running != 2U;

  /* Terminal state always wins without consuming or claiming work. */
  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  pthread_mutex_unlock(&shared->queue_mutex);
  pthread_mutex_lock(&shared->state_mutex);
  index_shard_publish_terminal_locked(
      shared, INDEX_SHARD_TERMINAL_CANCELLED);
  pthread_mutex_unlock(&shared->state_mutex);
  selection = index_shard_select_work(
      &contexts[1], shared, &index_order, &claim);
  failures += selection != INDEX_SHARD_WORK_DONE;
  failures += tasks[0].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY;

  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_STOPPED) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_STOPPED) != 0;
  pthread_mutex_unlock(&shared->queue_mutex);
  contexts[0].published_staged_group = NULL;
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_outer_cap_liveness_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t contexts[4];
  index_shard_staged_group_t group;
  index_shard_staged_task_t task;
  index_shard_thread_state_t *shared;
  index_shard_inner_claim_t claim;
  size_t index_order = SIZE_MAX;
  index_shard_work_selection_t selection;
  int failures = 0;
  int i;

  memset(&pool, 0, sizeof(pool));
  memset(contexts, 0, sizeof(contexts));
  memset(&group, 0, sizeof(group));
  memset(&task, 0, sizeof(task));
  memset(&claim, 0, sizeof(claim));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }

  pool.worker_count = 4;
  pool.generation = 9U;
  pool.contexts = contexts;
  shared->pool = &pool;
  shared->worker_count = 4;
  shared->nindexes = 4U;
  shared->outer_running = 2U;
  shared->producer_width = 2U;
  shared->canonical_scan_cursor = 2U;
  for (i = 0; i < 4; i++) {
    contexts[i].worker_id = i;
    contexts[i].pool = &pool;
    contexts[i].generation_seen = pool.generation;
  }

  pthread_mutex_lock(&shared->queue_mutex);
  shared->queue_waiters = 1U;
  index_shard_queue_signal_locked(shared);
  shared->outer_running--;
  index_shard_queue_signal_locked(shared);
  failures += index_shard_claim_outer_locked(
      &contexts[2], shared, 3U,
      &index_order) == 0;
  failures += shared->outer_running != 1U;
  failures += shared->canonical_scan_cursor != 2U;
  failures += index_shard_claim_outer_locked(
      &contexts[2], shared, 2U,
      &index_order) != 0;
  failures += index_order != 2U;
  failures += shared->outer_running != 2U;
  failures += shared->canonical_scan_cursor != 3U;

  contexts[0].current_outer_active = TRUE;
  contexts[0].current_index_order = 0U;
  contexts[0].staged_group_epoch = 41U;
  contexts[0].published_staged_group = &group;
  group.pool = &pool;
  group.tasks = &task;
  group.task_count = 1U;
  group.generation = pool.generation;
  group.owner_epoch = contexts[0].staged_group_epoch;
  group.owner_worker = 0;
  group.owner_index_order = 0U;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &task,
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  pthread_mutex_unlock(&shared->queue_mutex);

  selection = index_shard_select_work(
      &contexts[3], shared, &index_order, &claim);
  failures += selection != INDEX_SHARD_WORK_INNER;
  failures += claim.group != &group;
  failures += claim.task_index != 0U;
  failures += claim.owner_claim;
  failures += shared->canonical_scan_cursor != 3U;
  failures += shared->outer_running != 2U;

  failures += index_shard_staged_complete_claim(
      shared, &claim,
      INDEX_SHARD_STAGED_EXECUTE_OK, 0ULL) != 0;
  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &group, &task,
      INDEX_SHARD_STAGED_TASK_STOPPED) != 0;
  shared->queue_waiters = 0U;
  pthread_mutex_unlock(&shared->queue_mutex);
  contexts[0].published_staged_group = NULL;
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_owner_helper_wake_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t contexts[3];
  index_shard_staged_group_t groups[2];
  index_shard_staged_task_t tasks[3];
  index_shard_owner_wake_test_context_t wake_context;
  index_shard_thread_state_t *shared;
  pthread_t waiter;
  anbool started_cv_ready = FALSE;
  anbool waiter_created = FALSE;
  int failures = 0;
  int i;

  memset(&pool, 0, sizeof(pool));
  memset(contexts, 0, sizeof(contexts));
  memset(groups, 0, sizeof(groups));
  memset(tasks, 0, sizeof(tasks));
  memset(&wake_context, 0, sizeof(wake_context));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }
  pool.worker_count = 3;
  pool.generation = 10U;
  pool.contexts = contexts;
  shared->pool = &pool;
  shared->worker_count = 3;
  shared->producer_width = 1U;
  for (i = 0; i < 3; i++) {
    contexts[i].worker_id = i;
    contexts[i].pool = &pool;
    contexts[i].generation_seen = pool.generation;
    contexts[i].current_outer_active = TRUE;
    contexts[i].current_index_order = (size_t)i;
    contexts[i].owner_cv_ready =
        pthread_cond_init(&contexts[i].owner_cv, NULL) == 0;
    if (!contexts[i].owner_cv_ready) {
      failures++;
    }
  }
  for (i = 0; i < 2; i++) {
    contexts[i].staged_group_epoch =
        51U + (unsigned long long)i;
    contexts[i].published_staged_group = &groups[i];
    groups[i].pool = &pool;
    groups[i].tasks = i ? &tasks[2] : tasks;
    groups[i].task_count = i ? 1U : 2U;
    groups[i].generation = pool.generation;
    groups[i].owner_epoch = contexts[i].staged_group_epoch;
    groups[i].owner_worker = i;
    groups[i].owner_index_order = contexts[i].current_index_order;
  }

  wake_context.shared = shared;
  wake_context.worker = &contexts[1];
  started_cv_ready =
      pthread_cond_init(&wake_context.started_cv, NULL) == 0;
  failures += !started_cv_ready;
  pthread_mutex_lock(&shared->queue_mutex);
  if (!failures &&
      pthread_create(&waiter, NULL,
                     index_shard_owner_wake_test_worker,
                     &wake_context) == 0) {
    waiter_created = TRUE;
    while (!wake_context.started) {
      pthread_cond_wait(&wake_context.started_cv, &shared->queue_mutex);
    }
  } else {
    failures++;
  }
  failures += index_shard_staged_set_state_locked(
      shared, &groups[0], &tasks[0],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &groups[0], &tasks[1],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  contexts[0].owner_waiting = TRUE;
  index_shard_notify_progress_locked(shared, 0);
  failures += !contexts[0].owner_wake_pending;
  failures += !contexts[1].owner_wake_pending;
  failures += contexts[2].owner_wake_pending;
  contexts[2].owner_waiting = TRUE;
  index_shard_notify_progress_locked(shared, 0);
  failures += contexts[2].owner_wake_pending;
  contexts[2].owner_waiting = FALSE;
  pthread_mutex_unlock(&shared->queue_mutex);
  if (waiter_created) {
    failures += pthread_join(waiter, NULL) != 0;
    failures += wake_context.selection != INDEX_SHARD_WORK_INNER;
    failures += wake_context.claim.group != &groups[0];
    failures += wake_context.claim.owner_claim;
    failures += index_shard_staged_complete_claim(
        shared, &wake_context.claim,
        INDEX_SHARD_STAGED_EXECUTE_MORE, 0ULL) != 0;
  }

  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &groups[0], &tasks[0],
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY) != 0;
  contexts[0].owner_wake_pending = FALSE;
  contexts[1].owner_wake_pending = FALSE;
  contexts[1].owner_waiting = TRUE;
  failures += index_shard_staged_set_state_locked(
      shared, &groups[0], &tasks[1],
      INDEX_SHARD_STAGED_TASK_STOPPED) != 0;
  index_shard_notify_progress_locked(shared, 0);
  failures += !contexts[0].owner_wake_pending;
  failures += contexts[1].owner_wake_pending;

  contexts[0].owner_wake_pending = FALSE;
  contexts[1].owner_wake_pending = FALSE;
  contexts[0].owner_waiting = FALSE;
  index_shard_notify_progress_locked(shared, 0);
  failures += contexts[0].owner_wake_pending;
  failures += !contexts[1].owner_wake_pending;

  failures += index_shard_staged_set_state_locked(
      shared, &groups[0], &tasks[0],
      INDEX_SHARD_STAGED_TASK_STOPPED) != 0;
  pthread_mutex_unlock(&shared->queue_mutex);

  contexts[0].published_staged_group = NULL;
  contexts[1].published_staged_group = NULL;
  if (started_cv_ready) {
    pthread_cond_destroy(&wake_context.started_cv);
  }
  for (i = 0; i < 3; i++) {
    if (contexts[i].owner_cv_ready) {
      pthread_cond_destroy(&contexts[i].owner_cv);
    }
  }
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_staged_submit_rearm_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t contexts[2];
  index_shard_staged_group_t groups[2];
  index_shard_staged_task_t tasks[2][2];
  index_shard_thread_state_t *shared;
  int owner = -1;
  int failures = 0;
  int i;
  int j;

  memset(&pool, 0, sizeof(pool));
  memset(contexts, 0, sizeof(contexts));
  memset(groups, 0, sizeof(groups));
  memset(tasks, 0, sizeof(tasks));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }
  pool.worker_count = 2;
  pool.generation = 8U;
  pool.contexts = contexts;
  shared->pool = &pool;
  shared->worker_count = 2;

  for (i = 0; i < 2; i++) {
    contexts[i].worker_id = i;
    contexts[i].pool = &pool;
    contexts[i].generation_seen = pool.generation;
    contexts[i].current_outer_active = TRUE;
    contexts[i].current_index_order = i ? 3U : 5U;
    contexts[i].staged_group_epoch = 21U + (unsigned long long)i;
    contexts[i].published_staged_group = &groups[i];
    groups[i].pool = &pool;
    groups[i].tasks = tasks[i];
    groups[i].task_count = 2U;
    groups[i].generation = pool.generation;
    groups[i].owner_epoch = contexts[i].staged_group_epoch;
    groups[i].owner_worker = i;
    groups[i].owner_index_order = contexts[i].current_index_order;
  }

  pthread_mutex_lock(&shared->queue_mutex);
  for (i = 0; i < 2; i++) {
    for (j = 0; j < 2; j++) {
      failures += index_shard_staged_set_state_locked(
          shared,
          &groups[i],
          &tasks[i][j],
          INDEX_SHARD_STAGED_TASK_SUBMIT_READY) != 0;
      failures += index_shard_staged_set_submit_wait_locked(
          &groups[i], &tasks[i][j], TRUE) != 0;
    }
  }

  failures += index_shard_staged_rearm_one_submit_waiter_locked(
      &pool, &owner) != 1;
  failures += owner != 1;
  failures += groups[1].submit_ready_mask != UINT64_C(1);
  failures += groups[1].submit_wait_mask != UINT64_C(2);
  failures += groups[1].submit_credit_mask != UINT64_C(1);
  failures += groups[0].submit_ready_mask != UINT64_C(0);
  failures += groups[0].submit_wait_mask != UINT64_C(3);

  owner = -1;
  failures += index_shard_staged_rearm_one_submit_waiter_locked(
      &pool, &owner) != 1;
  failures += owner != 1;
  failures += groups[1].submit_ready_mask != UINT64_C(3);
  failures += groups[1].submit_wait_mask != UINT64_C(0);
  failures += groups[1].submit_credit_mask != UINT64_C(3);

  owner = -1;
  failures += index_shard_staged_rearm_one_submit_waiter_locked(
      &pool, &owner) != 1;
  failures += owner != 0;
  failures += groups[0].submit_ready_mask != UINT64_C(1);
  failures += groups[0].submit_wait_mask != UINT64_C(2);
  failures += groups[0].submit_credit_mask != UINT64_C(1);

  owner = -1;
  failures += index_shard_staged_rearm_one_submit_waiter_locked(
      &pool, &owner) != 1;
  failures += owner != 0;
  failures += groups[0].submit_ready_mask != UINT64_C(3);
  failures += groups[0].submit_wait_mask != UINT64_C(0);
  failures += groups[0].submit_credit_mask != UINT64_C(3);

  owner = 0;
  failures += index_shard_staged_rearm_one_submit_waiter_locked(
      &pool, &owner) != 0;
  failures += owner != -1;
  failures += !shared->staged_submit_backpressure;
  pthread_mutex_unlock(&shared->queue_mutex);

  contexts[0].published_staged_group = NULL;
  contexts[1].published_staged_group = NULL;
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_staged_submit_handoff_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t context;
  index_shard_staged_group_t group;
  index_shard_staged_task_t tasks[2];
  index_shard_staged_claim_t claim;
  index_shard_thread_state_t *shared;
  int failures = 0;

  memset(&pool, 0, sizeof(pool));
  memset(&context, 0, sizeof(context));
  memset(&group, 0, sizeof(group));
  memset(tasks, 0, sizeof(tasks));
  memset(&claim, 0, sizeof(claim));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }
  pool.worker_count = 1;
  pool.generation = 9U;
  pool.contexts = &context;
  shared->pool = &pool;
  shared->worker_count = 1;
  context.worker_id = 0;
  context.pool = &pool;
  context.generation_seen = pool.generation;
  context.current_outer_active = TRUE;
  context.current_index_order = 4U;
  context.staged_group_epoch = 31U;
  context.published_staged_group = &group;
  group.pool = &pool;
  group.tasks = tasks;
  group.task_count = 2U;
  group.generation = pool.generation;
  group.owner_epoch = context.staged_group_epoch;
  group.owner_worker = 0;
  group.owner_index_order = context.current_index_order;

  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_SUBMITTING) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_SUBMIT_READY) != 0;
  failures += index_shard_staged_set_submit_wait_locked(
      &group, &tasks[1], TRUE) != 0;
  group.running_count = 1U;
  shared->staged_submit_callbacks_active = 1U;
  pthread_mutex_unlock(&shared->queue_mutex);

  claim.group = &group;
  claim.task_index = 0U;
  claim.kind = INDEX_SHARD_STAGED_CLAIM_SUBMIT;
  claim.owner_claim = TRUE;
  claim.submit_credit = TRUE;
  failures += index_shard_staged_complete_claim(
      shared,
      &claim,
      INDEX_SHARD_STAGED_SUBMIT_COMPUTE_READY,
      0ULL) != 0;

  pthread_mutex_lock(&shared->queue_mutex);
  failures += tasks[0].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY;
  failures += group.compute_ready_mask != UINT64_C(1);
  failures += tasks[1].scheduler_state !=
      INDEX_SHARD_STAGED_TASK_SUBMIT_READY;
  failures += group.submit_ready_mask != UINT64_C(2);
  failures += group.submit_wait_mask != UINT64_C(0);
  failures += group.submit_credit_mask != UINT64_C(2);
  failures += group.running_count != 0U;
  failures += shared->staged_submit_callbacks_active != 0U;
  pthread_mutex_unlock(&shared->queue_mutex);

  context.published_staged_group = NULL;
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_staged_submit_backpressure_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t context;
  index_shard_staged_group_t group;
  index_shard_staged_task_t tasks[3];
  index_shard_staged_claim_t claim;
  index_shard_staged_claim_t next_claim;
  index_shard_thread_state_t *shared;
  int rearmed_owner = -1;
  int failures = 0;
  size_t i;

  memset(&pool, 0, sizeof(pool));
  memset(&context, 0, sizeof(context));
  memset(&group, 0, sizeof(group));
  memset(tasks, 0, sizeof(tasks));
  memset(&claim, 0, sizeof(claim));
  memset(&next_claim, 0, sizeof(next_claim));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }
  pool.worker_count = 1;
  pool.generation = 10U;
  pool.contexts = &context;
  shared->pool = &pool;
  shared->worker_count = 1;
  context.worker_id = 0;
  context.pool = &pool;
  context.generation_seen = pool.generation;
  context.current_outer_active = TRUE;
  context.current_index_order = 6U;
  context.staged_group_epoch = 41U;
  context.published_staged_group = &group;
  group.pool = &pool;
  group.tasks = tasks;
  group.task_count = 3U;
  group.generation = pool.generation;
  group.owner_epoch = context.staged_group_epoch;
  group.owner_worker = 0;
  group.owner_index_order = context.current_index_order;

  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_SUBMITTING) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_SUBMIT_READY) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[2],
      INDEX_SHARD_STAGED_TASK_SUBMIT_READY) != 0;
  group.running_count = 1U;
  shared->staged_submit_callbacks_active = 1U;
  pthread_mutex_unlock(&shared->queue_mutex);

  claim.group = &group;
  claim.task_index = 0U;
  claim.kind = INDEX_SHARD_STAGED_CLAIM_SUBMIT;
  claim.owner_claim = TRUE;
  failures += index_shard_staged_complete_claim(
      shared,
      &claim,
      INDEX_SHARD_STAGED_SUBMIT_RETRY,
      0ULL) != 0;

  pthread_mutex_lock(&shared->queue_mutex);
  failures += !shared->staged_submit_backpressure;
  failures += group.submit_ready_mask != UINT64_C(0);
  failures += group.submit_wait_mask != UINT64_C(7);
  failures += group.submit_credit_mask != UINT64_C(0);
  failures += index_shard_staged_rearm_one_submit_waiter_locked(
      &pool, &rearmed_owner) != 1;
  failures += rearmed_owner != 0;
  failures += group.submit_ready_mask != UINT64_C(1);
  failures += group.submit_wait_mask != UINT64_C(6);
  failures += group.submit_credit_mask != UINT64_C(1);
  failures += index_shard_staged_select_locked(
      &context, shared,
      INDEX_SHARD_STAGED_SELECT_SUBMIT,
      TRUE, FALSE, &next_claim) != 0;
  failures += next_claim.task_index != 0U;
  failures += !next_claim.submit_credit;
  failures += group.submit_ready_mask != UINT64_C(0);
  failures += group.submit_credit_mask != UINT64_C(0);
  failures += index_shard_staged_select_locked(
      &context, shared,
      INDEX_SHARD_STAGED_SELECT_SUBMIT,
      TRUE, FALSE, &claim) != 1;

  group.running_count = 0U;
  shared->staged_submit_callbacks_active = 0U;
  for (i = 0U; i < 3U; i++) {
    failures += index_shard_staged_set_state_locked(
        shared, &group, &tasks[i],
        INDEX_SHARD_STAGED_TASK_STOPPED) != 0;
  }
  index_shard_staged_refresh_submit_backpressure_locked(&pool);
  failures += shared->staged_submit_backpressure;
  pthread_mutex_unlock(&shared->queue_mutex);

  context.published_staged_group = NULL;
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_completion_registry_test(void) {
  index_shard_pool_t pool;
  index_shard_worker_context_t contexts[2];
  index_shard_staged_group_t group;
  index_shard_staged_task_t tasks[2];
  index_shard_thread_state_t *shared;
  anbool early = FALSE;
  int failures = 0;

  memset(&pool, 0, sizeof(pool));
  memset(contexts, 0, sizeof(contexts));
  memset(&group, 0, sizeof(group));
  memset(tasks, 0, sizeof(tasks));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }
  pool.worker_count = 2;
  pool.generation = 9U;
  pool.contexts = contexts;
  shared->pool = &pool;
  shared->worker_count = 2;
  contexts[0].worker_id = 0;
  contexts[0].pool = &pool;
  contexts[0].generation_seen = pool.generation;
  contexts[0].current_outer_active = TRUE;
  contexts[0].current_index_order = 4U;
  contexts[0].staged_group_epoch = 13U;
  contexts[0].published_staged_group = &group;
  group.pool = &pool;
  group.tasks = tasks;
  group.task_count = 2U;
  group.generation = pool.generation;
  group.owner_epoch = contexts[0].staged_group_epoch;
  group.owner_worker = 0;
  group.owner_index_order = contexts[0].current_index_order;

  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[0],
      INDEX_SHARD_STAGED_TASK_IO_SUBMITTED) != 0;
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_SUBMIT_READY) != 0;
  failures += index_shard_staged_set_submit_wait_locked(
      &group, &tasks[1], TRUE) != 0;
  tasks[0].completion_id = 101U;
  failures += index_shard_completion_registry_register_locked(
      shared, &group, 0U, 101U, &early) != 0;
  failures += early;
  pthread_mutex_unlock(&shared->queue_mutex);

  index_shard_staged_completion_notify(&pool, 101U);

  pthread_mutex_lock(&shared->queue_mutex);
  failures += !(group.completion_pending_mask & UINT64_C(1));
  failures += group.submit_wait_mask != UINT64_C(0);
  failures += group.submit_ready_mask != UINT64_C(2);
  failures += group.submit_credit_mask != UINT64_C(2);
  failures += index_shard_completion_registry_remove_locked(
      shared, &group, 0U, 101U) != 0;
  tasks[0].completion_id = 0U;
  failures += index_shard_staged_set_completion_pending_locked(
      &group, &tasks[0], FALSE) != 0;

  shared->staged_submit_callbacks_active = 1U;
  pthread_mutex_unlock(&shared->queue_mutex);
  index_shard_staged_completion_notify(&pool, 202U);
  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared, &group, &tasks[1],
      INDEX_SHARD_STAGED_TASK_IO_SUBMITTED) != 0;
  tasks[1].completion_id = 202U;
  early = FALSE;
  failures += index_shard_completion_registry_register_locked(
      shared, &group, 1U, 202U, &early) != 0;
  failures += !early;
  shared->staged_submit_callbacks_active = 0U;
  failures += index_shard_staged_set_completion_pending_locked(
      &group, &tasks[1], early) != 0;
  failures += index_shard_completion_registry_remove_locked(
      shared, &group, 1U, 202U) != 0;
  tasks[1].completion_id = 0U;
  failures += index_shard_staged_set_completion_pending_locked(
      &group, &tasks[1], FALSE) != 0;
  failures += shared->completion_active != 0U;
  failures += group.completion_registry_entries != 0U;
  pthread_mutex_unlock(&shared->queue_mutex);

  contexts[0].published_staged_group = NULL;
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

index_shard_staged_io_status_t
index_shard_inline_poll_test_callback(
    const void *input,
    size_t input_bytes,
    void *output,
    size_t output_bytes) {
  (void)input;
  (void)input_bytes;
  if (!output || output_bytes !=
          sizeof(index_shard_staged_io_status_t)) {
    return INDEX_SHARD_STAGED_IO_ERROR;
  }
  return *(index_shard_staged_io_status_t *)output;
}

int index_shard_completion_inline_poll_case(
    index_shard_staged_io_status_t poll_status,
    index_shard_staged_task_state_t expected_state,
    anbool expect_stop,
    anbool expect_error) {
  static const index_shard_staged_ops_t ops = {
      "completion-inline-poll-test",
      NULL,
      NULL,
      index_shard_inline_poll_test_callback,
      NULL,
      NULL,
      NULL};
  index_shard_pool_t pool;
  index_shard_worker_context_t contexts[2];
  index_shard_staged_group_t group;
  index_shard_staged_task_t task;
  index_shard_thread_state_t *shared;
  anbool early = FALSE;
  int failures = 0;

  memset(&pool, 0, sizeof(pool));
  memset(contexts, 0, sizeof(contexts));
  memset(&group, 0, sizeof(group));
  memset(&task, 0, sizeof(task));
  shared = &pool.shared;
  if (index_shard_staged_retire_test_init(shared)) {
    return 1;
  }
  pool.worker_count = 2;
  pool.generation = 11U;
  pool.contexts = contexts;
  shared->pool = &pool;
  shared->worker_count = 2;
  shared->staged_tickets_active = 1U;
  shared->staged_source_leases = 1U;
  contexts[0].worker_id = 0;
  contexts[0].pool = &pool;
  contexts[0].generation_seen = pool.generation;
  contexts[0].current_outer_active = TRUE;
  contexts[0].current_index_order = 6U;
  contexts[0].staged_group_epoch = 17U;
  contexts[0].published_staged_group = &group;
  group.pool = &pool;
  group.ops = &ops;
  group.tasks = &task;
  group.task_count = 1U;
  group.generation = pool.generation;
  group.owner_epoch = contexts[0].staged_group_epoch;
  group.owner_worker = 0;
  group.owner_index_order = contexts[0].current_index_order;
  group.io_submitted = 1U;
  task.output = &poll_status;
  task.output_bytes = sizeof(poll_status);

  pthread_mutex_lock(&shared->queue_mutex);
  failures += index_shard_staged_set_state_locked(
      shared,
      &group,
      &task,
      INDEX_SHARD_STAGED_TASK_IO_SUBMITTED) != 0;
  task.completion_id = 303U;
  failures += index_shard_completion_registry_register_locked(
      shared, &group, 0U, task.completion_id, &early) != 0;
  failures += early;
  pthread_mutex_unlock(&shared->queue_mutex);

  index_shard_staged_completion_notify(&pool, 303U);

  pthread_mutex_lock(&shared->queue_mutex);
  failures += task.scheduler_state != expected_state;
  failures += task.completion_id != 0U;
  failures += group.completion_pending_mask != UINT64_C(0);
  failures += group.io_submitted != 0U;
  failures += group.running_count != 0U;
  failures += group.completion_registry_entries != 0U;
  failures += shared->staged_tickets_active != 0U;
  failures += shared->staged_source_leases != 0U;
  failures += group.stop_seen != expect_stop;
  failures += group.task_failed != expect_error;
  failures += group.internal_error != expect_error;
  failures += shared->completion_registry_error != expect_error;
  failures += shared->completion_active != 0U;
  pthread_mutex_unlock(&shared->queue_mutex);

  contexts[0].published_staged_group = NULL;
  index_shard_staged_retire_test_destroy(shared);
  return failures;
}

int index_shard_completion_inline_poll_test(void) {
  return index_shard_completion_inline_poll_case(
      INDEX_SHARD_STAGED_IO_READY,
      INDEX_SHARD_STAGED_TASK_COMPUTE_READY,
      FALSE,
      FALSE) +
      index_shard_completion_inline_poll_case(
          INDEX_SHARD_STAGED_IO_FAILED,
          INDEX_SHARD_STAGED_TASK_OWNER_READY,
          FALSE,
          FALSE) +
      index_shard_completion_inline_poll_case(
          INDEX_SHARD_STAGED_IO_CANCELLED,
          INDEX_SHARD_STAGED_TASK_STOPPED,
          TRUE,
          FALSE) +
      index_shard_completion_inline_poll_case(
          INDEX_SHARD_STAGED_IO_ERROR,
          INDEX_SHARD_STAGED_TASK_FAILED,
          FALSE,
          TRUE);
}

int index_shard_test_staged_retire_more(void) {
  return index_shard_staged_retire_test_order() +
      index_shard_staged_retire_test_terminal() +
      index_shard_staged_mask_selection_test() +
      index_shard_ready_priority_test() +
      index_shard_outer_cap_liveness_test() +
      index_shard_owner_helper_wake_test() +
      index_shard_staged_submit_rearm_test() +
      index_shard_staged_submit_handoff_test() +
      index_shard_staged_submit_backpressure_test() +
      index_shard_completion_registry_test() +
      index_shard_completion_inline_poll_test();
}
#endif
