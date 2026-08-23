/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include "test_fitsbin_payload_common.h"

payload_readahead_state_t payload_readahead = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    0,
    0,
    0,
    0,
    {0},
    {0}
};

payload_madvise_state_t payload_madvise = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    0,
    0,
    0,
    0,
    0
};

extern ssize_t __real_readahead(
    int fd,
    off_t offset,
    size_t size);

ssize_t __wrap_readahead(
    int fd,
    off_t offset,
    size_t size) {
    int call = -1;
    int block;

    pthread_mutex_lock(&payload_readahead.mutex);
    if (payload_readahead.recording) {
        call = payload_readahead.calls++;
        if ((size_t)call < PAYLOAD_WRAPPER_RECORDS) {
            payload_readahead.offsets[call] = offset;
            payload_readahead.requests[call] = size;
        }
    }
    block = payload_readahead.block;
    if (block) {
        payload_readahead.active++;
        pthread_cond_broadcast(&payload_readahead.condition);
        while (!payload_readahead.release) {
            pthread_cond_wait(
                &payload_readahead.condition,
                &payload_readahead.mutex);
        }
        payload_readahead.active--;
        pthread_cond_broadcast(&payload_readahead.condition);
    }
    pthread_mutex_unlock(&payload_readahead.mutex);
    return __real_readahead(fd, offset, size);
}

extern int __real_madvise(
    void* address,
    size_t length,
    int advice);

int __wrap_madvise(
    void* address,
    size_t length,
    int advice) {
    int call = 0;

    pthread_mutex_lock(&payload_madvise.mutex);
#if defined(MADV_POPULATE_READ)
    if (payload_madvise.recording &&
        advice == MADV_POPULATE_READ) {
        call = ++payload_madvise.calls;
        pthread_cond_broadcast(&payload_madvise.condition);
        if (call == payload_madvise.block_call) {
            payload_madvise.active = 1;
            pthread_cond_broadcast(&payload_madvise.condition);
            while (!payload_madvise.release) {
                pthread_cond_wait(
                    &payload_madvise.condition,
                    &payload_madvise.mutex);
            }
            payload_madvise.active = 0;
            pthread_cond_broadcast(&payload_madvise.condition);
        }
    }
#else
    (void)call;
#endif
    pthread_mutex_unlock(&payload_madvise.mutex);
    return __real_madvise(address, length, advice);
}

void payload_readahead_reset(int recording) {
    pthread_mutex_lock(&payload_readahead.mutex);
    payload_readahead.recording = recording;
    payload_readahead.block = 0;
    payload_readahead.release = 0;
    payload_readahead.calls = 0;
    payload_readahead.active = 0;
    memset(payload_readahead.offsets, 0,
           sizeof(payload_readahead.offsets));
    memset(payload_readahead.requests, 0,
           sizeof(payload_readahead.requests));
    pthread_mutex_unlock(&payload_readahead.mutex);
}

void payload_readahead_block(void) {
    payload_readahead_reset(1);
    pthread_mutex_lock(&payload_readahead.mutex);
    payload_readahead.block = 1;
    pthread_mutex_unlock(&payload_readahead.mutex);
}

void payload_readahead_release(void) {
    pthread_mutex_lock(&payload_readahead.mutex);
    payload_readahead.release = 1;
    pthread_cond_broadcast(&payload_readahead.condition);
    pthread_mutex_unlock(&payload_readahead.mutex);
}

int payload_readahead_wait_for_calls(
    int expected,
    int timeout_seconds) {
    struct timespec deadline;
    int status = 0;
    int reached;

    if (clock_gettime(CLOCK_REALTIME, &deadline)) {
        return -1;
    }
    deadline.tv_sec += timeout_seconds;
    pthread_mutex_lock(&payload_readahead.mutex);
    while ((payload_readahead.calls < expected ||
            payload_readahead.active < expected) &&
           status != ETIMEDOUT) {
        status = pthread_cond_timedwait(
            &payload_readahead.condition,
            &payload_readahead.mutex,
            &deadline);
    }
    reached = payload_readahead.calls >= expected &&
        payload_readahead.active >= expected;
    pthread_mutex_unlock(&payload_readahead.mutex);
    return reached ? 0 : -1;
}

void payload_madvise_reset(int recording) {
    pthread_mutex_lock(&payload_madvise.mutex);
    payload_madvise.recording = recording;
    payload_madvise.block_call = 0;
    payload_madvise.release = 0;
    payload_madvise.calls = 0;
    payload_madvise.active = 0;
    pthread_mutex_unlock(&payload_madvise.mutex);
}

void payload_madvise_block_at(int call) {
    payload_madvise_reset(1);
    pthread_mutex_lock(&payload_madvise.mutex);
    payload_madvise.block_call = call;
    pthread_mutex_unlock(&payload_madvise.mutex);
}

void payload_madvise_release(void) {
    pthread_mutex_lock(&payload_madvise.mutex);
    payload_madvise.release = 1;
    pthread_cond_broadcast(&payload_madvise.condition);
    pthread_mutex_unlock(&payload_madvise.mutex);
}

int payload_madvise_wait_for_call(
    int expected,
    int timeout_seconds) {
    struct timespec deadline;
    int status = 0;
    int reached;

    if (clock_gettime(CLOCK_REALTIME, &deadline)) {
        return -1;
    }
    deadline.tv_sec += timeout_seconds;
    pthread_mutex_lock(&payload_madvise.mutex);
    while ((payload_madvise.calls < expected ||
            !payload_madvise.active) &&
           status != ETIMEDOUT) {
        status = pthread_cond_timedwait(
            &payload_madvise.condition,
            &payload_madvise.mutex,
            &deadline);
    }
    reached = payload_madvise.calls >= expected &&
        payload_madvise.active;
    pthread_mutex_unlock(&payload_madvise.mutex);
    return reached ? 0 : -1;
}

int payload_planned_ranges(
    void* opaque,
    fitsbin_payload_io_cancel_check_fn cancelled,
    void* cancel_opaque,
    fitsbin_prefetch_range_t* ranges,
    size_t range_capacity,
    size_t* range_count) {
    payload_planned_state_t* state = opaque;

    if (!state || !cancelled || !ranges || !range_count ||
        !range_capacity) {
        errno = EINVAL;
        return -1;
    }
    state->calls++;
    *range_count = 0U;
    if (cancelled(cancel_opaque)) {
        errno = ECANCELED;
        return -1;
    }
    if (state->result < 0) {
        errno = EIO;
        return -1;
    }
    if (!state->result) {
        return 0;
    }
    ranges[0] = state->range;
    *range_count = 1U;
    return 1;
}

int payload_ticket_collect(
    fitsbin_payload_io_ticket_t** ticket,
    int timeout_seconds) {
    struct timespec deadline;
    struct timespec pause = {0, 1000000L};
    int result = 0;
    int status;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline)) {
        return -1;
    }
    deadline.tv_sec += timeout_seconds;
    while (1) {
        status = fitsbin_payload_io_ticket_poll_and_destroy(
            ticket, &result);
        if (status > 0) {
            return result;
        }
        if (status < 0) {
            return -1;
        }
        {
            struct timespec now;

            if (clock_gettime(CLOCK_MONOTONIC, &now) ||
                now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec)) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        nanosleep(&pause, NULL);
    }
}

int payload_fixture_open(payload_fixture_t* fixture) {
    fitsbin_t* output;
    fitsbin_chunk_t output_chunk;
    fitsbin_chunk_t input_chunk;
    int fd;
    size_t i;

    memset(fixture, 0, sizeof(*fixture));
    snprintf(
        fixture->filename,
        sizeof(fixture->filename),
        "/tmp/test-fitsbin-payload-io.XXXXXX");
    fd = mkstemp(fixture->filename);
    if (fd < 0) {
        return -1;
    }
    close(fd);

    for (i = 0U; i < sizeof(fixture->bytes); i++) {
        fixture->bytes[i] =
            (unsigned char)((i * 37U + 11U) & 0xffU);
    }

    output = fitsbin_open_for_writing(
        fixture->filename);
    if (!output) {
        goto fail;
    }
    fitsbin_chunk_init(&output_chunk);
    output_chunk.tablename = "payload-io";
    output_chunk.itemsize = 1;
    output_chunk.nrows = (int)sizeof(fixture->bytes);
    output_chunk.data = fixture->bytes;
    if (fitsbin_write_primary_header(output) ||
        fitsbin_write_chunk(output, &output_chunk) ||
        fitsbin_fix_primary_header(output)) {
        fitsbin_close(output);
        output = NULL;
        fitsbin_chunk_clean(&output_chunk);
        goto fail;
    }
    if (fitsbin_close(output)) {
        output = NULL;
        fitsbin_chunk_clean(&output_chunk);
        goto fail;
    }
    output = NULL;
    fitsbin_chunk_clean(&output_chunk);

    fixture->fitsbin = fitsbin_open(fixture->filename);
    if (!fixture->fitsbin) {
        goto fail;
    }
    fitsbin_chunk_init(&input_chunk);
    input_chunk.tablename = "payload-io";
    if (fitsbin_read_chunk(
            fixture->fitsbin,
            &input_chunk)) {
        goto fail;
    }
    fixture->chunk = fitsbin_get_chunk(
        fixture->fitsbin, 0);
    if (!fixture->chunk ||
        !fixture->chunk->data ||
        fixture->chunk->data_file_size !=
            sizeof(fixture->bytes)) {
        goto fail;
    }
    return 0;

fail:
    if (output) {
        fitsbin_close(output);
    }
    if (fixture->fitsbin) {
        fitsbin_close(fixture->fitsbin);
        fixture->fitsbin = NULL;
    }
    unlink(fixture->filename);
    return -1;
}

void payload_fixture_close(
    payload_fixture_t* fixture) {
    if (fixture->fitsbin) {
        fitsbin_close(fixture->fitsbin);
    }
    unlink(fixture->filename);
    memset(fixture, 0, sizeof(*fixture));
}
