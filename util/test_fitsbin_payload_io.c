/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include "test_fitsbin_payload_common.h"

void test_fitsbin_payload_async_mapped_population(CuTest* ct);
void test_fitsbin_payload_deferred_mapped_plan(CuTest* ct);
void test_fitsbin_payload_queue_gap_coalescing(CuTest* ct);
void test_fitsbin_payload_exact_mapped_completion(CuTest* ct);
void test_fitsbin_payload_poll_transfers_ticket_ownership(CuTest* ct);
void test_fitsbin_payload_poll_failure_transfers_once(CuTest* ct);
void test_fitsbin_payload_poll_cancel_transfers_once(CuTest* ct);
