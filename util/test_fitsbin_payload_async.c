/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include "test_fitsbin_payload_common.h"

void test_fitsbin_payload_async_mapped_population(CuTest* ct) {
    payload_fixture_t fixture;
    fitsbin_prefetch_range_t range;
    fitsbin_payload_io_ticket_t* ticket = NULL;
    int repeated = -1;
    int repeated_waited = -1;
    int submitted;
    int waited = -1;

    CuAssertIntEquals(ct, 0, payload_fixture_open(&fixture));
    CuAssertIntEquals(
        ct,
        0,
        fitsbin_configure_index_mmap(fixture.fitsbin));
    range.data = fixture.chunk->data;
    range.size = sizeof(fixture.bytes);
    fitsbin_payload_io_configure_workers(2);
    CuAssertIntEquals(
        ct, 0, fitsbin_payload_io_service_start(1));

    submitted = fitsbin_prefetch_ranges_submit(
        fixture.fitsbin,
        &range,
        1U,
        SIZE_MAX,
        &ticket);
    if (ticket) {
        waited = payload_ticket_collect(
            &ticket, 2);
    }
#if defined(MADV_POPULATE_READ)
    repeated = fitsbin_prefetch_ranges_submit(
        fixture.fitsbin,
        &range,
        1U,
        SIZE_MAX,
        &ticket);
    if (ticket) {
        repeated_waited = payload_ticket_collect(
            &ticket, 2);
    }
#endif
    fitsbin_payload_io_service_stop();
    fitsbin_payload_io_configure_workers(1);

#if defined(MADV_POPULATE_READ)
    CuAssertIntEquals(ct, 1, submitted);
    CuAssert(ct, "async mapped population failed", waited > 0);
    CuAssertIntEquals(
        ct, FITSBIN_PAYLOAD_IO_SUBMIT_QUEUED, repeated);
    CuAssert(ct, "repeated mapped population failed",
             repeated_waited > 0);
    CuAssertPtrEquals(ct, NULL, ticket);
#else
    CuAssertIntEquals(ct, 0, submitted);
    CuAssertIntEquals(ct, -1, waited);
#endif
    CuAssert(
        ct,
        "async mapped population changed payload bytes",
        !memcmp((const unsigned char*)fixture.chunk->data,
                fixture.bytes,
                sizeof(fixture.bytes)));

    payload_fixture_close(&fixture);
}
