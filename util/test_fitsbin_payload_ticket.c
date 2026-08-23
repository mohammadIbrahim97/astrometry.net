/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include "test_fitsbin_payload_common.h"

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
