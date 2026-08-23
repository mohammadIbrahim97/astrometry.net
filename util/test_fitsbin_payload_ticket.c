/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include "test_fitsbin_payload_common.h"

static int submit_mapped_ticket(
    payload_fixture_t* fixture,
    fitsbin_payload_io_ticket_t** ticket);

#if defined(MADV_POPULATE_READ)
typedef struct payload_notifier_barrier_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int callback_entered;
    int callback_release;
    int callback_exited;
    int waiter_calling;
    int waiter_returned;
    int waiter_status;
    unsigned int sequence;
    unsigned int callback_enter_sequence;
    unsigned int waiter_call_sequence;
    unsigned int callback_release_sequence;
    unsigned int callback_exit_sequence;
    unsigned int waiter_return_sequence;
    unsigned long long completion_id;
} payload_notifier_barrier_state_t;

static int payload_notifier_barrier_wait(
    payload_notifier_barrier_state_t* state,
    const int* predicate) {
    struct timespec deadline;
    int status = 0;
    int reached;

    if (clock_gettime(CLOCK_REALTIME, &deadline)) {
        return -1;
    }
    deadline.tv_sec += 2;
    pthread_mutex_lock(&state->mutex);
    while (!*predicate && status != ETIMEDOUT) {
        status = pthread_cond_timedwait(
            &state->condition,
            &state->mutex,
            &deadline);
    }
    reached = *predicate;
    pthread_mutex_unlock(&state->mutex);
    return reached ? 0 : -1;
}

static void payload_notifier_barrier_callback(
    void* opaque,
    unsigned long long completion_id) {
    payload_notifier_barrier_state_t* state = opaque;

    pthread_mutex_lock(&state->mutex);
    state->callback_entered = 1;
    state->completion_id = completion_id;
    state->callback_enter_sequence = ++state->sequence;
    pthread_cond_broadcast(&state->condition);
    while (!state->callback_release) {
        pthread_cond_wait(&state->condition, &state->mutex);
    }
    state->callback_exited = 1;
    state->callback_exit_sequence = ++state->sequence;
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->mutex);
}

static void* payload_notifier_barrier_waiter(void* opaque) {
    payload_notifier_barrier_state_t* state = opaque;
    int status;

    pthread_mutex_lock(&state->mutex);
    state->waiter_calling = 1;
    state->waiter_call_sequence = ++state->sequence;
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->mutex);

    status = fitsbin_payload_io_wait_completion_notifier_idle(
        payload_notifier_barrier_callback, state);

    pthread_mutex_lock(&state->mutex);
    state->waiter_status = status;
    state->waiter_returned = 1;
    state->waiter_return_sequence = ++state->sequence;
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

static void test_payload_notifier_idle_barrier(CuTest* ct) {
    payload_notifier_barrier_state_t state;
    payload_fixture_t fixture;
    fitsbin_payload_io_ticket_t* ticket = NULL;
    pthread_t waiter;
    int failure = 0;
    int mutex_initialized = 0;
    int condition_initialized = 0;
    int fixture_opened = 0;
    int service_started = 0;
    int notifier_registered = 0;
    int waiter_started = 0;
    int poll_result = 0;
    int drain_result = 0;

    memset(&state, 0, sizeof(state));
    memset(&fixture, 0, sizeof(fixture));
    state.waiter_status = -1;
    if (pthread_mutex_init(&state.mutex, NULL)) {
        failure = 1;
        goto cleanup;
    }
    mutex_initialized = 1;
    if (pthread_cond_init(&state.condition, NULL)) {
        failure = 2;
        goto cleanup;
    }
    condition_initialized = 1;
    if (payload_fixture_open(&fixture)) {
        failure = 3;
        goto cleanup;
    }
    fixture_opened = 1;
    if (fitsbin_configure_index_mmap(fixture.fitsbin)) {
        failure = 4;
        goto cleanup;
    }
    fitsbin_payload_io_configure_workers(1);
    if (fitsbin_payload_io_service_start(1)) {
        failure = 5;
        goto cleanup;
    }
    service_started = 1;
    if (fitsbin_payload_io_set_completion_notifier(
            payload_notifier_barrier_callback, &state)) {
        failure = 6;
        goto cleanup;
    }
    notifier_registered = 1;

    if (submit_mapped_ticket(&fixture, &ticket) !=
            FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED || !ticket) {
        failure = 7;
        goto cleanup;
    }
    if (payload_notifier_barrier_wait(
            &state, &state.callback_entered)) {
        failure = 8;
        goto cleanup;
    }

    /* The provider callback is active, but its terminal ticket is collectible. */
    if (fitsbin_payload_io_ticket_poll_and_destroy(
            &ticket, &poll_result) != 1 || ticket || poll_result <= 0) {
        failure = 9;
        goto cleanup;
    }

    if (pthread_create(
            &waiter, NULL,
            payload_notifier_barrier_waiter, &state)) {
        failure = 10;
        goto cleanup;
    }
    waiter_started = 1;
    if (payload_notifier_barrier_wait(
            &state, &state.waiter_calling)) {
        failure = 11;
        goto cleanup;
    }

    pthread_mutex_lock(&state.mutex);
    if (state.waiter_returned) {
        failure = 12;
    }
    state.callback_release = 1;
    state.callback_release_sequence = ++state.sequence;
    pthread_cond_broadcast(&state.condition);
    pthread_mutex_unlock(&state.mutex);

    if (pthread_join(waiter, NULL)) {
        failure = 13;
    } else {
        waiter_started = 0;
    }
    if (!failure &&
        (state.waiter_status || !state.callback_exited ||
         !state.completion_id ||
         state.callback_enter_sequence >= state.waiter_call_sequence ||
         state.waiter_call_sequence >=
             state.callback_release_sequence ||
         state.callback_release_sequence >=
             state.waiter_return_sequence ||
         state.callback_exit_sequence >=
             state.waiter_return_sequence)) {
        failure = 14;
    }
    if (fitsbin_payload_io_clear_completion_notifier(
            payload_notifier_barrier_callback, &state)) {
        failure = 15;
        goto cleanup;
    }
    notifier_registered = 0;

cleanup:
    if (condition_initialized) {
        pthread_mutex_lock(&state.mutex);
        if (!state.callback_release) {
            state.callback_release = 1;
            state.callback_release_sequence = ++state.sequence;
        }
        pthread_cond_broadcast(&state.condition);
        pthread_mutex_unlock(&state.mutex);
    }
    if (ticket) {
        (void)fitsbin_payload_io_ticket_drain_and_destroy(
            &ticket, &drain_result);
    }
    if (waiter_started) {
        (void)pthread_join(waiter, NULL);
    }
    if (notifier_registered) {
        (void)fitsbin_payload_io_clear_completion_notifier(
            payload_notifier_barrier_callback, &state);
    }
    if (service_started) {
        fitsbin_payload_io_service_stop();
    }
    fitsbin_payload_io_configure_workers(1);
    if (fixture_opened) {
        payload_fixture_close(&fixture);
    }
    if (condition_initialized) {
        pthread_cond_destroy(&state.condition);
    }
    if (mutex_initialized) {
        pthread_mutex_destroy(&state.mutex);
    }
    CuAssertIntEquals_Msg(
        ct, "provider notifier idle barrier stage", 0, failure);
}
#endif

static int submit_mapped_ticket(
    payload_fixture_t* fixture,
    fitsbin_payload_io_ticket_t** ticket) {
    fitsbin_prefetch_range_t range;

    range.data = fixture->chunk->data;
    range.size = sizeof(fixture->bytes);
    return fitsbin_prefetch_ranges_submit(
        fixture->fitsbin,
        &range,
        1U,
        SIZE_MAX,
        ticket);
}

void test_fitsbin_payload_poll_transfers_ticket_ownership(CuTest* ct) {
    payload_fixture_t fixture;
    fitsbin_payload_io_ticket_t* ticket = NULL;
    int submitted;
    int poll_status;
    int result = 0;
    int result_errno;

    CuAssertIntEquals(ct, 0, payload_fixture_open(&fixture));
    CuAssertIntEquals(
        ct, 0, fitsbin_configure_index_mmap(fixture.fitsbin));
    fitsbin_payload_io_configure_workers(1);
    CuAssertIntEquals(
        ct, 0, fitsbin_payload_io_service_start(1));
    payload_readahead_block();
    submitted = submit_mapped_ticket(&fixture, &ticket);

#if defined(MADV_POPULATE_READ)
    CuAssertIntEquals(ct, 1, submitted);
    CuAssertPtrNotNull(ct, ticket);
    CuAssertIntEquals(
        ct, 0, payload_readahead_wait_for_calls(1, 2));
    poll_status = fitsbin_payload_io_ticket_poll_and_destroy(
        &ticket, &result);
    CuAssertIntEquals(ct, 0, poll_status);
    CuAssertPtrNotNull(ct, ticket);
#else
    CuAssertIntEquals(ct, 0, submitted);
#endif

    payload_readahead_release();
    fitsbin_payload_io_service_stop();
    payload_readahead_reset(0);
#if defined(MADV_POPULATE_READ)
    errno = 0;
    poll_status = fitsbin_payload_io_ticket_poll_and_destroy(
        &ticket, &result);
    result_errno = errno;
    CuAssertIntEquals(ct, 1, poll_status);
    CuAssert(ct, "mapped ticket did not become ready", result > 0);
    CuAssertIntEquals(ct, 0, result_errno);
    CuAssertPtrEquals(ct, NULL, ticket);
#endif
    fitsbin_payload_io_configure_workers(1);
    payload_fixture_close(&fixture);
#if defined(MADV_POPULATE_READ)
    test_payload_notifier_idle_barrier(ct);
#endif
}

void test_fitsbin_payload_poll_failure_transfers_once(CuTest* ct) {
    payload_fixture_t fixture;
    payload_planned_state_t plan;
    fitsbin_payload_io_ticket_t* ticket = NULL;
    int submitted;
    int poll_status;
    int result = 0;
    int result_errno;

    CuAssertIntEquals(ct, 0, payload_fixture_open(&fixture));
    CuAssertIntEquals(
        ct, 0, fitsbin_configure_index_mmap(fixture.fitsbin));
    memset(&plan, 0, sizeof(plan));
    plan.result = -1;
    fitsbin_payload_io_configure_workers(1);
    CuAssertIntEquals(
        ct, 0, fitsbin_payload_io_service_start(1));
    submitted = fitsbin_prefetch_ranges_planned_submit(
        fixture.fitsbin,
        payload_planned_ranges,
        &plan,
        1024U * 1024U,
        &ticket);
    fitsbin_payload_io_service_stop();

#if defined(MADV_POPULATE_READ)
    errno = 0;
    poll_status = fitsbin_payload_io_ticket_poll_and_destroy(
        &ticket, &result);
    result_errno = errno;
    CuAssertIntEquals(ct, 1, submitted);
    CuAssertIntEquals(ct, 1, poll_status);
    CuAssertIntEquals(ct, -1, result);
    CuAssertIntEquals(ct, EIO, result_errno);
    CuAssertPtrEquals(ct, NULL, ticket);
#else
    CuAssertIntEquals(ct, 0, submitted);
#endif
    fitsbin_payload_io_configure_workers(1);
    payload_fixture_close(&fixture);
}

void test_fitsbin_payload_poll_cancel_transfers_once(CuTest* ct) {
    payload_fixture_t fixture;
    fitsbin_payload_io_ticket_t* ticket = NULL;
    int submitted;
    int cancelled = 0;
    int poll_status;
    int result = -1;
    int result_errno;

    CuAssertIntEquals(ct, 0, payload_fixture_open(&fixture));
    CuAssertIntEquals(
        ct, 0, fitsbin_configure_index_mmap(fixture.fitsbin));
    fitsbin_payload_io_configure_workers(1);
    CuAssertIntEquals(
        ct, 0, fitsbin_payload_io_service_start(1));
    payload_readahead_block();
    submitted = submit_mapped_ticket(&fixture, &ticket);

#if defined(MADV_POPULATE_READ)
    CuAssertIntEquals(ct, 1, submitted);
    CuAssertPtrNotNull(ct, ticket);
    CuAssertIntEquals(
        ct, 0, payload_readahead_wait_for_calls(1, 2));
    cancelled = fitsbin_payload_io_ticket_cancel_async(ticket);
    CuAssertIntEquals(ct, 1, cancelled);
#else
    CuAssertIntEquals(ct, 0, submitted);
#endif
    payload_readahead_release();
    fitsbin_payload_io_service_stop();
    payload_readahead_reset(0);

#if defined(MADV_POPULATE_READ)
    errno = 0;
    poll_status = fitsbin_payload_io_ticket_poll_and_destroy(
        &ticket, &result);
    result_errno = errno;
    CuAssertIntEquals(ct, 1, poll_status);
    CuAssertIntEquals(ct, 0, result);
    CuAssertIntEquals(ct, ECANCELED, result_errno);
    CuAssertPtrEquals(ct, NULL, ticket);
#endif
    fitsbin_payload_io_configure_workers(1);
    payload_fixture_close(&fixture);
}
