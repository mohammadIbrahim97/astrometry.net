/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#ifndef TEST_FITSBIN_PAYLOAD_COMMON_H
#define TEST_FITSBIN_PAYLOAD_COMMON_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "fitsbin_internal.h"

#include "cutest.h"

#define PAYLOAD_FIXTURE_BYTES (128U * 1024U)
#define PAYLOAD_WRAPPER_RECORDS 16U

typedef struct payload_fixture {
    fitsbin_t* fitsbin;
    fitsbin_chunk_t* chunk;
    unsigned char bytes[PAYLOAD_FIXTURE_BYTES];
    char filename[128];
} payload_fixture_t;

typedef struct payload_planned_state {
    fitsbin_prefetch_range_t range;
    int result;
    int calls;
} payload_planned_state_t;

typedef struct payload_readahead_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int recording;
    int block;
    int release;
    int calls;
    int active;
    off_t offsets[PAYLOAD_WRAPPER_RECORDS];
    size_t requests[PAYLOAD_WRAPPER_RECORDS];
} payload_readahead_state_t;

typedef struct payload_madvise_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int recording;
    int block_call;
    int release;
    int calls;
    int active;
} payload_madvise_state_t;

extern payload_readahead_state_t payload_readahead;
extern payload_madvise_state_t payload_madvise;

void payload_readahead_reset(int recording);
void payload_readahead_block(void);
void payload_readahead_release(void);
int payload_readahead_wait_for_calls(
    int expected,
    int timeout_seconds);
void payload_madvise_reset(int recording);
void payload_madvise_block_at(int call);
void payload_madvise_release(void);
int payload_madvise_wait_for_call(
    int expected,
    int timeout_seconds);
int payload_planned_ranges(
    void* opaque,
    fitsbin_payload_io_cancel_check_fn cancelled,
    void* cancel_opaque,
    fitsbin_prefetch_range_t* ranges,
    size_t range_capacity,
    size_t* range_count);
int payload_ticket_collect(
    fitsbin_payload_io_ticket_t** ticket,
    int timeout_seconds);
int payload_fixture_open(payload_fixture_t* fixture);
void payload_fixture_close(payload_fixture_t* fixture);

#endif
