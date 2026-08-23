/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include "test_fitsbin_payload_common.h"

void test_fitsbin_payload_deferred_mapped_plan(CuTest* ct) {
    payload_fixture_t fixture;
    fitsbin_payload_io_ticket_t* ticket = NULL;
    payload_planned_state_t plan;
    int submitted;
    int waited = -1;

    CuAssertIntEquals(ct, 0, payload_fixture_open(&fixture));
    CuAssertIntEquals(
        ct,
        0,
        fitsbin_configure_index_mmap(fixture.fitsbin));
    memset(&plan, 0, sizeof(plan));
    plan.range.data = fixture.chunk->data;
    plan.range.size = sizeof(fixture.bytes);
    plan.result = 1;
    fitsbin_payload_io_configure_workers(2);
    CuAssertIntEquals(
        ct, 0, fitsbin_payload_io_service_start(1));

    submitted = fitsbin_prefetch_ranges_planned_submit(
        fixture.fitsbin,
        payload_planned_ranges,
        &plan,
        1024U * 1024U,
        &ticket);
    if (ticket) {
        waited = payload_ticket_collect(
            &ticket, 2);
    }

#if defined(MADV_POPULATE_READ)
    CuAssertIntEquals(ct, 1, submitted);
    CuAssert(ct, "deferred mapped plan failed", waited > 0);
    CuAssertIntEquals(ct, 1, plan.calls);

    submitted = fitsbin_prefetch_ranges_planned_submit(
        fixture.fitsbin,
        payload_planned_ranges,
        &plan,
        1024U * 1024U,
        &ticket);
    waited = -1;
    if (ticket) {
        waited = payload_ticket_collect(
            &ticket, 2);
    }
    CuAssertIntEquals(ct, 1, submitted);
    CuAssertIntEquals(ct, 1, waited);
    CuAssertIntEquals(ct, 2, plan.calls);

    plan.result = 0;
    submitted = fitsbin_prefetch_ranges_planned_submit(
        fixture.fitsbin,
        payload_planned_ranges,
        &plan,
        1024U * 1024U,
        &ticket);
    waited = -1;
    if (ticket) {
        waited = payload_ticket_collect(
            &ticket, 2);
    }
    CuAssertIntEquals(ct, 1, submitted);
    CuAssertIntEquals(ct, 1, waited);
    CuAssertIntEquals(ct, 3, plan.calls);
#else
    CuAssertIntEquals(ct, 0, submitted);
    CuAssertIntEquals(ct, -1, waited);
    CuAssertIntEquals(ct, 0, plan.calls);
#endif

    fitsbin_payload_io_service_stop();
    fitsbin_payload_io_configure_workers(1);
    CuAssert(
        ct,
        "deferred mapped plan changed payload bytes",
        !memcmp((const unsigned char*)fixture.chunk->data,
                fixture.bytes,
                sizeof(fixture.bytes)));

    payload_fixture_close(&fixture);
}

void test_fitsbin_payload_queue_gap_coalescing(CuTest* ct) {
    payload_fixture_t fixture;
    fitsbin_prefetch_range_t ranges[2];
    fitsbin_prefetch_range_t gap_range;
    fitsbin_payload_io_ticket_t* ticket = NULL;
    size_t page_size;
    size_t first_offset;
    size_t first_request = 0U;
    uintptr_t data_address;
    long detected_page_size;
    int submitted;
    int waited = -1;
    int calls = 0;

    CuAssertIntEquals(ct, 0, payload_fixture_open(&fixture));
    detected_page_size = sysconf(_SC_PAGESIZE);
    CuAssert(ct, "invalid test page size", detected_page_size > 0);
    page_size = (size_t)detected_page_size;
    data_address = (uintptr_t)fixture.chunk->data;
    first_offset = (size_t)(data_address % page_size);
    if (first_offset) {
        first_offset = page_size - first_offset;
    }
    CuAssert(
        ct,
        "payload fixture is too small for a two-page queue gap",
        sizeof(fixture.bytes) >
            first_offset + 18U * page_size);
    CuAssertIntEquals(
        ct,
        0,
        fitsbin_configure_index_mmap(fixture.fitsbin));
    CuAssertIntEquals(
        ct,
        0,
        fitsbin_set_mmap_advice(
            fixture.fitsbin,
            FITSBIN_MMAP_ADVICE_RANDOM,
            TRUE));
    fitsbin_payload_io_configure_workers(1);
    CuAssertIntEquals(
        ct, 0, fitsbin_payload_io_service_start(1));

    ranges[0].data =
        (const unsigned char*)fixture.chunk->data + first_offset;
    ranges[0].size = 8U * page_size;
    ranges[1].data =
        (const unsigned char*)fixture.chunk->data +
        first_offset + 10U * page_size;
    ranges[1].size = 8U * page_size;
    payload_readahead_reset(1);
    submitted = fitsbin_prefetch_ranges_submit(
        fixture.fitsbin,
        ranges,
        2U,
        20U * page_size,
        &ticket);
    if (ticket) {
        waited = payload_ticket_collect(
            &ticket, 2);
    }
    pthread_mutex_lock(&payload_readahead.mutex);
    calls = payload_readahead.calls;
    first_request = payload_readahead.requests[0];
    pthread_mutex_unlock(&payload_readahead.mutex);

#if defined(MADV_POPULATE_READ)
    CuAssertIntEquals(
        ct, FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED, submitted);
    CuAssert(ct, "queue-gap mapped population failed", waited > 0);
    CuAssertIntEquals(ct, 1, calls);
    CuAssert(
        ct,
        "file-offset queue did not cover the bounded gap",
        first_request == 18U * page_size);

    /*
     * The storage request covered this page, but mapped completion did not.
     * A later exact request must still pass through the population barrier.
     */
    gap_range.data =
        (const unsigned char*)fixture.chunk->data +
        first_offset + 8U * page_size + 1U;
    gap_range.size = 1U;
    payload_readahead_reset(1);
    submitted = fitsbin_prefetch_ranges_submit(
        fixture.fitsbin,
        &gap_range,
        1U,
        2U * page_size,
        &ticket);
    waited = -1;
    if (ticket) {
        waited = payload_ticket_collect(
            &ticket, 2);
    }
    CuAssertIntEquals(
        ct, FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED, submitted);
    CuAssert(ct, "queue-gap exact population failed", waited > 0);
#else
    CuAssertIntEquals(
        ct, FITSBIN_PAYLOAD_IO_SUBMIT_UNAVAILABLE, submitted);
    CuAssertIntEquals(ct, -1, waited);
#endif

    payload_readahead_reset(0);
    fitsbin_payload_io_service_stop();
    fitsbin_payload_io_configure_workers(1);
    CuAssert(
        ct,
        "queue-gap preparation changed payload bytes",
        !memcmp((const unsigned char*)fixture.chunk->data,
                fixture.bytes,
                sizeof(fixture.bytes)));
    payload_fixture_close(&fixture);
}

void test_fitsbin_payload_exact_mapped_completion(CuTest* ct) {
    payload_fixture_t fixture;
    fitsbin_prefetch_range_t ranges[2];
    fitsbin_payload_io_ticket_t* ticket = NULL;
    size_t page_size;
    size_t first_offset;
    uintptr_t data_address;
    long detected_page_size;
    int first_submitted;
    int first_ticket_present;
#if defined(MADV_POPULATE_READ)
    int block_waited = -1;
    int poll_result = 0;
    int poll_status = -1;
    int poll_ticket_present = -1;
    int timeout_waited = 0;
    int timeout_errno = 0;
    int timeout_ticket_present = -1;
    int first_waited = -1;
    int first_drain_status = 0;
    int first_drain_result = 0;
    int first_released = 0;
    int first_calls = -1;
    int second_submitted = -1;
    int second_waited = -1;
    int second_drain_status = 0;
    int second_drain_result = 0;
    int second_released = 0;
    int second_calls = -1;
    int final_cleanup_result = 0;
    int final_cleanup_status = 1;
    int final_released;
#endif

    CuAssertIntEquals(ct, 0, payload_fixture_open(&fixture));
    detected_page_size = sysconf(_SC_PAGESIZE);
    CuAssert(ct, "invalid test page size", detected_page_size > 0);
    page_size = (size_t)detected_page_size;
    data_address = (uintptr_t)fixture.chunk->data;
    first_offset = (size_t)(data_address % page_size);
    if (first_offset) {
        first_offset = page_size - first_offset;
    }
    CuAssert(
        ct,
        "payload fixture is too small for two mapped spans",
        sizeof(fixture.bytes) > first_offset + 12U * page_size);
    CuAssertIntEquals(
        ct, 0, fitsbin_configure_index_mmap(fixture.fitsbin));
    fitsbin_payload_io_configure_workers(1);
    CuAssertIntEquals(
        ct, 0, fitsbin_payload_io_service_start(1));

    ranges[0].data =
        (const unsigned char*)fixture.chunk->data + first_offset;
    ranges[0].size = page_size;
    ranges[1].data =
        (const unsigned char*)fixture.chunk->data +
        first_offset + 10U * page_size;
    ranges[1].size = page_size;
    payload_madvise_block_at(2);
    first_submitted = fitsbin_prefetch_ranges_submit(
        fixture.fitsbin, ranges, 2U,
        4U * page_size, &ticket);
    first_ticket_present = ticket != NULL;

#if defined(MADV_POPULATE_READ)
    if (first_submitted == FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED &&
        ticket) {
        block_waited = payload_madvise_wait_for_call(2, 2);
        if (!block_waited) {
            poll_status = fitsbin_payload_io_ticket_poll_and_destroy(
                &ticket, &poll_result);
            poll_ticket_present = ticket != NULL;
            timeout_waited = payload_ticket_collect(&ticket, 0);
            timeout_errno = errno;
            timeout_ticket_present = ticket != NULL;
        }
    }
    payload_madvise_release();
    if (ticket) {
        first_waited = payload_ticket_collect(&ticket, 2);
    }
    if (ticket) {
        first_drain_status =
            fitsbin_payload_io_ticket_drain_and_destroy(
                &ticket, &first_drain_result);
    }
    first_released = ticket == NULL;
    pthread_mutex_lock(&payload_madvise.mutex);
    first_calls = payload_madvise.calls;
    pthread_mutex_unlock(&payload_madvise.mutex);

    payload_madvise_reset(1);
    if (!ticket) {
        second_submitted = fitsbin_prefetch_ranges_submit(
            fixture.fitsbin, ranges, 2U,
            4U * page_size, &ticket);
    }
    if (ticket) {
        second_waited = payload_ticket_collect(&ticket, 2);
    }
    if (ticket) {
        second_drain_status =
            fitsbin_payload_io_ticket_drain_and_destroy(
                &ticket, &second_drain_result);
    }
    second_released = ticket == NULL;
    pthread_mutex_lock(&payload_madvise.mutex);
    second_calls = payload_madvise.calls;
    pthread_mutex_unlock(&payload_madvise.mutex);
#endif

    payload_madvise_release();
    payload_madvise_reset(0);
    fitsbin_payload_io_service_stop();
#if defined(MADV_POPULATE_READ)
    if (ticket) {
        final_cleanup_status =
            fitsbin_payload_io_ticket_poll_and_destroy(
                &ticket, &final_cleanup_result);
    }
    final_released = ticket == NULL;
#endif
    fitsbin_payload_io_configure_workers(1);
    payload_fixture_close(&fixture);

#if defined(MADV_POPULATE_READ)
    CuAssertIntEquals(
        ct, FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED,
        first_submitted);
    CuAssert(ct, "first exact-population ticket missing",
             first_ticket_present);
    CuAssertIntEquals(ct, 0, block_waited);
    CuAssertIntEquals(ct, 0, poll_status);
    CuAssert(ct, "blocked ticket completed prematurely",
             poll_ticket_present);
    CuAssertIntEquals(ct, -1, timeout_waited);
    CuAssertIntEquals(ct, ETIMEDOUT, timeout_errno);
    CuAssert(ct, "timed-out ticket lost ownership",
             timeout_ticket_present);
    CuAssert(ct, "second mapped span did not complete",
             first_waited > 0);
    CuAssert(ct, "first mapped ticket cleanup failed",
             first_waited >= 0 || first_drain_status > 0);
    CuAssert(ct, "first mapped ticket was not released",
             first_released);
    CuAssertIntEquals(ct, 2, first_calls);
    CuAssertIntEquals(
        ct, FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED,
        second_submitted);
    CuAssert(ct, "repeated exact population failed",
             second_waited > 0);
    CuAssert(ct, "second mapped ticket cleanup failed",
             second_waited >= 0 || second_drain_status > 0);
    CuAssert(ct, "second mapped ticket was not released",
             second_released);
    CuAssertIntEquals(ct, 2, second_calls);
    CuAssert(ct, "mapped ticket survived service shutdown",
             final_cleanup_status > 0 && final_released);
#else
    CuAssertIntEquals(
        ct, FITSBIN_PAYLOAD_IO_SUBMIT_UNAVAILABLE,
        first_submitted);
    CuAssert(ct, "unsupported exact-population ticket",
             !first_ticket_present);
#endif
}
