/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "index_shard_private.h"
#include "astrometry/log.h"
#include "astrometry/fitsbin.h"

/* queue_mutex must be held. */
static anbool index_shard_outer_claimable_locked(
    const index_shard_thread_state_t *shared) {
  return shared && shared->producer_width &&
      shared->outer_running < shared->producer_width &&
      shared->canonical_scan_cursor < shared->nindexes;
}

/* queue_mutex must be held. */
static anbool index_shard_queue_work_available_locked(
    const index_shard_thread_state_t *shared) {
  int owner_worker;

  if (!shared || !shared->pool) {
    return FALSE;
  }
  if (index_shard_outer_claimable_locked(shared)) {
    return TRUE;
  }
  for (owner_worker = 0;
       owner_worker < shared->worker_count;
       owner_worker++) {
    const index_shard_worker_context_t *owner =
        &shared->pool->contexts[owner_worker];
    const index_shard_staged_group_t *staged =
        owner->published_staged_group;
    uint64_t io_ready;

    if (!staged) {
      continue;
    }
    io_ready = staged->io_submitted_mask &
        staged->completion_pending_mask;
    if (staged->cancelling) {
      io_ready |= staged->io_submitted_mask &
          ~staged->cancel_sent_mask;
    }
    if (io_ready || staged->compute_ready_mask ||
        (shared->staged_submit_backpressure
             ? staged->submit_credit_mask
             : staged->submit_ready_mask) ||
        staged->prepare_ready_mask) {
      return TRUE;
    }
  }
  return FALSE;
}

static size_t index_shard_ready_count(uint64_t mask) {
  size_t count = 0U;

  while (mask) {
    mask &= mask - UINT64_C(1);
    count++;
  }
  return count;
}

/* queue_mutex must be held. */
static anbool index_shard_group_compute_executable_locked(
    const index_shard_staged_group_t *group) {
  return group && !group->cancelling && !group->task_failed &&
      !group->internal_error && !group->stop_seen;
}

/*
 * queue_mutex must be held. Owners wait on private condition variables while
 * their published groups are waiting for delivery. Wake one such owner when
 * READY compute exceeds the workers already routed to it. The awakened owner
 * still claims through the normal global selector, so task and result order do
 * not change.
 */
static void index_shard_owner_helper_signal_locked(
    index_shard_thread_state_t *shared) {
  size_t ready_count = 0U;
  size_t routed_count;
  int owner_worker;

  if (!shared || !shared->pool) {
    return;
  }
  routed_count = shared->queue_waiters ? 1U : 0U;
  for (owner_worker = 0;
       owner_worker < shared->worker_count;
       owner_worker++) {
    index_shard_worker_context_t *owner =
        &shared->pool->contexts[owner_worker];
    index_shard_staged_group_t *group =
        owner->published_staged_group;
    size_t group_ready;

    if (!index_shard_group_compute_executable_locked(group)) {
      continue;
    }
    group_ready = index_shard_ready_count(
        group->compute_ready_mask);
    if (ready_count > SIZE_MAX - group_ready) {
      ready_count = SIZE_MAX;
    } else {
      ready_count += group_ready;
    }
    if (owner->owner_wake_pending ||
        (group_ready && owner->owner_cv_ready &&
         owner->owner_waiting)) {
      if (routed_count != SIZE_MAX) {
        routed_count++;
      }
    }
  }
  if (ready_count <= routed_count) {
    return;
  }

  for (owner_worker = 0;
       owner_worker < shared->worker_count;
       owner_worker++) {
    index_shard_worker_context_t *owner =
        &shared->pool->contexts[owner_worker];
    index_shard_staged_group_t *group =
        owner->published_staged_group;

    if (!owner->owner_cv_ready || !owner->owner_waiting ||
        owner->owner_wake_pending ||
        (index_shard_group_compute_executable_locked(group) &&
         group->compute_ready_mask)) {
      continue;
    }
    owner->owner_wake_pending = TRUE;
    pthread_cond_signal(&owner->owner_cv);
    return;
  }
}

/* queue_mutex must be held. */
void index_shard_queue_signal_locked(
    index_shard_thread_state_t *shared) {
  if (!shared) {
    return;
  }
  if (!index_shard_queue_work_available_locked(shared)) {
    return;
  }
  if (shared->queue_waiters) {
    pthread_cond_signal(&shared->queue_cv);
  }
  index_shard_owner_helper_signal_locked(shared);
}

/* queue_mutex must be held. */
void index_shard_owner_signal_locked(
    index_shard_thread_state_t *shared,
    int owner_worker) {
  index_shard_worker_context_t *owner;

  if (!shared || !shared->pool || owner_worker < 0 ||
      owner_worker >= shared->worker_count) {
    return;
  }
  owner = &shared->pool->contexts[owner_worker];
  if (!owner->owner_cv_ready || !owner->owner_waiting) {
    return;
  }
  if (owner->owner_wake_pending) {
    return;
  }
  owner->owner_wake_pending = TRUE;
  pthread_cond_signal(&owner->owner_cv);
}

/* queue_mutex must be held. */
void index_shard_notify_progress_locked(
    index_shard_thread_state_t *shared,
    int owner_worker) {
  index_shard_queue_signal_locked(shared);
  index_shard_owner_signal_locked(shared, owner_worker);
}

/*
 * Global terminal/lifecycle events must wake every worker and every owner.
 * Ordinary task transitions use targeted signals above.
 * queue_mutex must be held.
 */
void index_shard_queue_broadcast_locked(
    index_shard_thread_state_t *shared) {
  int owner_worker;

  if (!shared) {
    return;
  }
  if (shared->queue_waiters) {
    pthread_cond_broadcast(&shared->queue_cv);
  }
  if (!shared->pool) {
    return;
  }
  for (owner_worker = 0;
       owner_worker < shared->worker_count;
       owner_worker++) {
    index_shard_worker_context_t *owner =
        &shared->pool->contexts[owner_worker];

    if (!owner->owner_cv_ready || !owner->owner_waiting) {
      continue;
    }
    if (owner->owner_wake_pending) {
      continue;
    }
    owner->owner_wake_pending = TRUE;
    pthread_cond_broadcast(&owner->owner_cv);
  }
}

void index_shard_wake_pass_waiters(index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->result_mutex);
  pthread_cond_broadcast(&shared->result_cv);
  pthread_mutex_unlock(&shared->result_mutex);
}

void index_shard_wake_queue_waiters(
    index_shard_thread_state_t *shared) {
  pthread_mutex_lock(&shared->queue_mutex);
  index_shard_queue_broadcast_locked(shared);
  pthread_mutex_unlock(&shared->queue_mutex);
}

/* queue_mutex must be held. */
static int index_shard_ready_select_locked(
    index_shard_worker_context_t *worker,
    index_shard_thread_state_t *shared,
    anbool allow_owner,
    index_shard_inner_claim_t *claim) {
  if (!worker || !shared || !claim) {
    return -1;
  }
  memset(claim, 0, sizeof(*claim));
  return index_shard_staged_select_locked(
      worker, shared, INDEX_SHARD_STAGED_SELECT_COMPUTE,
      allow_owner, !allow_owner, claim);
}

/* queue_mutex must be held. */
static int index_shard_delivery_select_locked(
    index_shard_worker_context_t *worker,
    index_shard_thread_state_t *shared,
    anbool allow_owner,
    index_shard_inner_claim_t *claim) {
  int rc;

  if (!worker || !shared || !claim) {
    return -1;
  }
  memset(claim, 0, sizeof(*claim));
  rc = index_shard_staged_select_locked(
      worker, shared, INDEX_SHARD_STAGED_SELECT_IO,
      allow_owner, !allow_owner, claim);
  if (rc <= 0) {
    return rc;
  }
  rc = index_shard_staged_select_locked(
      worker, shared, INDEX_SHARD_STAGED_SELECT_SUBMIT,
      allow_owner, !allow_owner, claim);
  if (rc <= 0) {
    return rc;
  }
  return index_shard_staged_select_locked(
      worker, shared, INDEX_SHARD_STAGED_SELECT_PREPARE,
      allow_owner, !allow_owner, claim);
}

/*
 * Owner loops use the same READY-first protocol without passing through
 * outer admission. No callback runs while queue_mutex is held.
 */
int index_shard_inner_select_locked(
    index_shard_worker_context_t *worker,
    index_shard_thread_state_t *shared,
    anbool allow_owner,
    index_shard_inner_claim_t *claim) {
  int rc = index_shard_ready_select_locked(
      worker, shared, allow_owner, claim);

  if (rc <= 0) {
    return rc;
  }
  return index_shard_delivery_select_locked(
      worker, shared, allow_owner, claim);
}

/*
 * queue_mutex must be held. Advancing canonical_scan_cursor assigns each
 * outer index at most once and outer_running bounds concurrent owners.
 */
int index_shard_claim_outer_locked(
    index_shard_worker_context_t *worker,
    index_shard_thread_state_t *shared,
    size_t candidate,
    size_t *index_order) {
  if (candidate >= shared->nindexes ||
      candidate != shared->canonical_scan_cursor ||
      shared->outer_running >= shared->producer_width) {
    return -1;
  }
  worker->ready_before_outer_eligible = FALSE;
  shared->outer_running++;
  shared->canonical_scan_cursor++;
  *index_order = candidate;
  if (index_shard_trace_enabled()) {
    logmsg("[index-shard] claim index_order=%zu lane=producer "
           "worker=%i owners=%zu producer_width=%zu "
           "outer_unclaimed=%zu payload=%s\n",
           candidate,
           worker->worker_id,
           shared->outer_running,
           shared->producer_width,
           shared->nindexes - shared->canonical_scan_cursor,
           fitsbin_mmap_advice_name(FITSBIN_MMAP_ADVICE_RANDOM));
  }
  /* Another worker may still execute READY work or fill outer capacity. */
  index_shard_queue_signal_locked(shared);
  return 0;
}

/*
 * Select work from the current band without transferring index ownership.
 * Outer admission normally takes priority so inner work cannot leave owner
 * capacity unused. After completing an outer task, a worker may execute one
 * already-READY foreign package before claiming another index. The reducer
 * remains the only authority for master result mutation.
 */
index_shard_work_selection_t
index_shard_select_work(
    index_shard_worker_context_t *worker,
    index_shard_thread_state_t *shared,
    size_t *index_order,
    index_shard_inner_claim_t *inner_claim) {
  if (!worker || !shared || !index_order || !inner_claim ||
      !shared->producer_width) {
    return INDEX_SHARD_WORK_ERROR;
  }
  if (worker->worker_id < 0 ||
      worker->worker_id >= shared->worker_count) {
    return INDEX_SHARD_WORK_ERROR;
  }

  pthread_mutex_lock(&shared->queue_mutex);
  while (1) {
    size_t candidate;
    int inner_selection;

    /* Linearize every claim before or after terminal publication. */
    pthread_mutex_lock(&shared->state_mutex);
    if (shared->terminal_cause != INDEX_SHARD_TERMINAL_NONE) {
      pthread_mutex_unlock(&shared->state_mutex);
      pthread_mutex_unlock(&shared->queue_mutex);
      return INDEX_SHARD_WORK_DONE;
    }

    if (worker->ready_before_outer_eligible &&
        index_shard_outer_claimable_locked(shared)) {
      worker->ready_before_outer_eligible = FALSE;
      inner_selection = index_shard_ready_select_locked(
          worker, shared, FALSE, inner_claim);
      if (inner_selection < 0) {
        pthread_mutex_unlock(&shared->state_mutex);
        pthread_mutex_unlock(&shared->queue_mutex);
        return INDEX_SHARD_WORK_ERROR;
      }
      if (!inner_selection) {
        pthread_mutex_unlock(&shared->state_mutex);
        pthread_mutex_unlock(&shared->queue_mutex);
        return INDEX_SHARD_WORK_INNER;
      }
    }

    candidate = shared->canonical_scan_cursor;
    if (index_shard_outer_claimable_locked(shared)) {
      if (index_shard_claim_outer_locked(
              worker,
              shared,
              candidate,
              index_order)) {
        pthread_mutex_unlock(&shared->state_mutex);
        pthread_mutex_unlock(&shared->queue_mutex);
        return INDEX_SHARD_WORK_ERROR;
      }
      pthread_mutex_unlock(&shared->state_mutex);
      pthread_mutex_unlock(&shared->queue_mutex);
      return INDEX_SHARD_WORK_OUTER;
    }

    inner_selection = index_shard_inner_select_locked(
        worker, shared, FALSE, inner_claim);
    if (inner_selection < 0) {
      pthread_mutex_unlock(&shared->state_mutex);
      pthread_mutex_unlock(&shared->queue_mutex);
      return INDEX_SHARD_WORK_ERROR;
    }
    if (!inner_selection) {
      pthread_mutex_unlock(&shared->state_mutex);
      pthread_mutex_unlock(&shared->queue_mutex);
      return INDEX_SHARD_WORK_INNER;
    }

    if (!shared->outer_running) {
      pthread_mutex_unlock(&shared->state_mutex);
      pthread_mutex_unlock(&shared->queue_mutex);
      return INDEX_SHARD_WORK_DONE;
    }
    pthread_mutex_unlock(&shared->state_mutex);
    shared->queue_waiters++;
    inner_selection = pthread_cond_wait(
        &shared->queue_cv, &shared->queue_mutex);
    if (!shared->queue_waiters) {
      pthread_mutex_unlock(&shared->queue_mutex);
      return INDEX_SHARD_WORK_ERROR;
    }
    shared->queue_waiters--;
    if (inner_selection) {
      pthread_mutex_unlock(&shared->queue_mutex);
      return INDEX_SHARD_WORK_ERROR;
    }
  }
}
