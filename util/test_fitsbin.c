/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fitsbin.h"
#include "fitsioutils.h"

#include "cutest.h"

static char* get_tmpfile(int i) {
    static char fn[256];
    sprintf(fn, "/tmp/test-fitsbin-%i", i);
    return fn;
}

// Verify that legacy mmap environment variables no longer alter production.
void test_fitsbin_mmap_advice(CuTest* ct) {
    static const char* envname = "ASTROMETRY_INDEX_MMAP_ADVICE";
    const char* oldvalue;
    char* saved = NULL;
    fitsbin_t fb;
    int result;
    int restore_result;

    oldvalue = getenv(envname);
    if (oldvalue) {
        saved = strdup(oldvalue);
        CuAssertPtrNotNull(ct, saved);
    }

    result = setenv(envname, "normal", 1);
    CuAssertIntEquals(ct, 0, result);

    fitsbin_mmap_clear_thread_advice();
    memset(&fb, 0, sizeof(fb));

    result = fitsbin_configure_index_mmap(&fb);
    CuAssertIntEquals(ct, 0, result);

    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_POLICY_FIXED_RANDOM,
        fitsbin_get_configured_mmap_policy());

#ifdef MADV_RANDOM
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        fb.mmap_advice);
#else
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_NORMAL,
        fb.mmap_advice);
#endif

    if (saved) {
        restore_result = setenv(envname, saved, 1);
    } else {
        restore_result = unsetenv(envname);
    }

    free(saved);
    CuAssertIntEquals(ct, 0, restore_result);
}

void test_fitsbin_mmap_prefetch(CuTest* ct) {
    static const char* envname = "ASTROMETRY_INDEX_MMAP_PREFETCH";
    const char* oldvalue;
    char* saved = NULL;
    fitsbin_t fb;
    int result;
    int restore_result;

    oldvalue = getenv(envname);
    if (oldvalue) {
        saved = strdup(oldvalue);
        CuAssertPtrNotNull(ct, saved);
    }

    result = setenv(envname, "1", 1);
    CuAssertIntEquals(ct, 0, result);

    memset(&fb, 0, sizeof(fb));
    result = fitsbin_configure_index_mmap(&fb);

    CuAssertIntEquals(ct, 0, result);
    CuAssertIntEquals(ct, FALSE, fb.mmap_prefetch_enabled);

    if (saved) {
        restore_result = setenv(envname, saved, 1);
    } else {
        restore_result = unsetenv(envname);
    }

    free(saved);
    CuAssertIntEquals(ct, 0, restore_result);
}

void test_fitsbin_mmap_adaptive_policy(CuTest* ct) {
    fitsbin_mmap_advice_state_t state;
    anbool transitioned;

    fitsbin_mmap_advice_state_init(
        &state,
        FITSBIN_MMAP_POLICY_ADAPTIVE);

    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        state.effective_advice);

    CuAssertIntEquals(ct, 0, state.pass_number);
    CuAssertIntEquals(
        ct,
        0,
        state.completed_clean_unsolved_passes);

    CuAssertIntEquals(ct, 0, state.transition_count);

    /*
     * Partial pass.
     */
    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        FALSE,
        FALSE,
        FALSE,
        FALSE,
        0,
        0);

    CuAssertIntEquals(ct, FALSE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        state.effective_advice);

    /*
     * Cancelled pass.
     */
    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        TRUE,
        TRUE,
        FALSE,
        TRUE,
        0,
        0);

    CuAssertIntEquals(ct, FALSE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        state.effective_advice);

    /*
     * Failed pass.
     */
    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        TRUE,
        TRUE,
        FALSE,
        FALSE,
        -1,
        0);

    CuAssertIntEquals(ct, FALSE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        state.effective_advice);

    /*
     * Solved pass.
     */
    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        TRUE,
        FALSE,
        TRUE,
        FALSE,
        0,
        0);

    CuAssertIntEquals(ct, FALSE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        state.effective_advice);

    /*
     * First clean, exhaustive, unsolved pass.
     */
    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        TRUE,
        TRUE,
        FALSE,
        FALSE,
        0,
        0);

    CuAssertIntEquals(ct, TRUE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_NORMAL,
        state.effective_advice);

    CuAssertIntEquals(
        ct,
        1,
        state.completed_clean_unsolved_passes);

    CuAssertIntEquals(ct, 1, state.transition_count);

    /*
     * Later clean pass remains NORMAL and does not transition again.
     */
    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        TRUE,
        TRUE,
        FALSE,
        FALSE,
        0,
        0);

    CuAssertIntEquals(ct, FALSE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_NORMAL,
        state.effective_advice);

    CuAssertIntEquals(ct, 1, state.transition_count);

    /*
     * New field.
     */
    fitsbin_mmap_advice_state_reset(&state);

    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        state.effective_advice);

    CuAssertIntEquals(ct, 0, state.pass_number);
    CuAssertIntEquals(
        ct,
        0,
        state.completed_clean_unsolved_passes);

    CuAssertIntEquals(ct, 0, state.transition_count);

    /*
     * Fixed policies never transition.
     */
    fitsbin_mmap_advice_state_init(
        &state,
        FITSBIN_MMAP_POLICY_FIXED_NORMAL);

    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        TRUE,
        TRUE,
        FALSE,
        FALSE,
        0,
        0);

    CuAssertIntEquals(ct, FALSE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_NORMAL,
        state.effective_advice);

    fitsbin_mmap_advice_state_init(
        &state,
        FITSBIN_MMAP_POLICY_FIXED_RANDOM);

    transitioned = fitsbin_mmap_policy_complete_pass(
        &state,
        TRUE,
        TRUE,
        FALSE,
        FALSE,
        0,
        0);

    CuAssertIntEquals(ct, FALSE, transitioned);
    CuAssertIntEquals(
        ct,
        FITSBIN_MMAP_ADVICE_RANDOM,
        state.effective_advice);
}

void test_fitsbin_1(CuTest* ct) {
    fitsbin_t* in, *out;
    int i;
    int N = 6;
    double outdata[6];
    double* indata;
    char* fn;
    fitsbin_chunk_t chunk;

    fn = get_tmpfile(0);
    out = fitsbin_open_for_writing(fn);
    CuAssertPtrNotNull(ct, out);

    CuAssertIntEquals(ct, 0, fitsbin_write_primary_header(out));

    for (i=0; i<N; i++) {
        outdata[i] = i*i;
    }

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test1";
    chunk.itemsize = sizeof(double);
    chunk.nrows = N;
    chunk.data = outdata;

    CuAssertIntEquals(ct, 0, fitsbin_write_chunk(out, &chunk));
    CuAssertIntEquals(ct, fitsbin_fix_primary_header(out), 0);
    CuAssertIntEquals(ct, fitsbin_close(out), 0);

    fitsbin_chunk_clean(&chunk);

    // writing shouldn't affect the data values
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, outdata[i], i*i);
    }

    in = fitsbin_open(fn);
    CuAssertPtrNotNull(ct, in);

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test1";

    CuAssertIntEquals(ct, 0, fitsbin_read_chunk(in, &chunk));
    CuAssertIntEquals(ct, sizeof(double), chunk.itemsize);
    CuAssertIntEquals(ct, N, chunk.nrows);
    indata = chunk.data;
    CuAssertPtrNotNull(ct, indata);
    CuAssertIntEquals(ct, 0, memcmp(outdata, indata, sizeof(outdata)));
    CuAssertIntEquals(ct, 0, fitsbin_close(in));
}

void test_fitsbin_2(CuTest* ct) {
    fitsbin_t* in, *out;
    int i;
    int N = 6;
    double outdata[6];
    double* indata;
    char* fn;
    fitsbin_chunk_t chunk;
    fitsbin_chunk_t* ch;

    fn = get_tmpfile(0);
    printf("Writing to %s\n", fn);
    out = fitsbin_open_for_writing(fn);
    CuAssertPtrNotNull(ct, out);

    CuAssertIntEquals(ct, 0, fitsbin_write_primary_header(out));

    for (i=0; i<N; i++) {
        outdata[i] = i*i;
    }

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test2";
    chunk.itemsize = 1;
    //chunk.nrows = N * sizeof(double);
    chunk.data = outdata;

    CuAssertIntEquals(ct, 0, fitsbin_write_chunk_header(out, &chunk));
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, 0, fitsbin_write_items(out, &chunk, &outdata[i], sizeof(double)));
    }
    CuAssertIntEquals(ct, 0, fitsbin_fix_chunk_header(out, &chunk));

    fitsbin_chunk_reset(&chunk);
    chunk.tablename = "test2B";
    chunk.itemsize = sizeof(double);
    //chunk.nrows = N;
    chunk.data = outdata;

    CuAssertIntEquals(ct, 0, fitsbin_write_chunk_header(out, &chunk));
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, 0, fitsbin_write_items(out, &chunk, outdata + (N-1) - i, 1));
    }
    CuAssertIntEquals(ct, 0, fitsbin_fix_chunk_header(out, &chunk));

    CuAssertIntEquals(ct, fitsbin_close(out), 0);

    fitsbin_chunk_clean(&chunk);

    // writing shouldn't affect the data values
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, outdata[i], i*i);
    }

    in = fitsbin_open(fn);
    CuAssertPtrNotNull(ct, in);

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test2";
    fitsbin_add_chunk(in, &chunk);
    chunk.tablename = "test2B";
    fitsbin_add_chunk(in, &chunk);

    CuAssertIntEquals(ct, 0, fitsbin_read(in));

    ch = fitsbin_get_chunk(in, 0);
    CuAssertIntEquals(ct, 1, ch->itemsize);
    CuAssertIntEquals(ct, N * sizeof(double), ch->nrows);
    indata = ch->data;
    CuAssertPtrNotNull(ct, indata);
    CuAssertIntEquals(ct, 0, memcmp(outdata, indata, sizeof(outdata)));

    ch = fitsbin_get_chunk(in, 1);
    CuAssertIntEquals(ct, sizeof(double), ch->itemsize);
    CuAssertIntEquals(ct, N, ch->nrows);
    indata = ch->data;
    CuAssertPtrNotNull(ct, indata);
    for (i=0; i<N; i++) {
        CuAssertDblEquals(ct, outdata[N-1 - i], indata[i], 1e-10);
    }

    CuAssertIntEquals(ct, 0, fitsbin_close(in));
}



void test_inmemory_fitsbin_1(CuTest* ct) {
    fitsbin_t* fb;
    int i;
    int N = 6;
    double outdata[6];
    double* indata;
    fitsbin_chunk_t chunk;

    fb = fitsbin_open_in_memory();
    CuAssertPtrNotNull(ct, fb);

    CuAssertIntEquals(ct, 0, fitsbin_write_primary_header(fb));

    for (i=0; i<N; i++) {
        outdata[i] = i*i;
    }

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test1";
    chunk.itemsize = sizeof(double);
    chunk.nrows = N;
    chunk.data = outdata;

    CuAssertIntEquals(ct, 0, fitsbin_write_chunk(fb, &chunk));
    CuAssertIntEquals(ct, 0, fitsbin_fix_primary_header(fb));

    fitsbin_chunk_clean(&chunk);

    // writing shouldn't affect the data values
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, outdata[i], i*i);
    }

    CuAssertIntEquals(ct, 0, fitsbin_switch_to_reading(fb));

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test1";

    CuAssertIntEquals(ct, 0, fitsbin_read_chunk(fb, &chunk));
    CuAssertIntEquals(ct, sizeof(double), chunk.itemsize);
    CuAssertIntEquals(ct, N, chunk.nrows);
    indata = chunk.data;
    CuAssertPtrNotNull(ct, indata);
    CuAssertIntEquals(ct, 0, memcmp(outdata, indata, sizeof(outdata)));

    CuAssertIntEquals(ct, 0, fitsbin_close(fb));
}

void test_inmemory_fitsbin_2(CuTest* ct) {
    fitsbin_t* fb;
    int i;
    int N = 6;
    double outdata[6];
    double* indata;
    fitsbin_chunk_t chunk;
    fitsbin_chunk_t* ch;

    fb = fitsbin_open_in_memory();
    CuAssertPtrNotNull(ct, fb);

    CuAssertIntEquals(ct, 0, fitsbin_write_primary_header(fb));

    for (i=0; i<N; i++) {
        outdata[i] = i*i;
    }

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test2";
    chunk.itemsize = 1;
    //chunk.nrows = N * sizeof(double);
    chunk.data = outdata;

    CuAssertIntEquals(ct, 0, fitsbin_write_chunk_header(fb, &chunk));
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, 0, fitsbin_write_items(fb, &chunk, &outdata[i], sizeof(double)));
    }
    CuAssertIntEquals(ct, 0, fitsbin_fix_chunk_header(fb, &chunk));

    fitsbin_chunk_reset(&chunk);
    chunk.tablename = "test2B";
    chunk.itemsize = sizeof(double);
    //chunk.nrows = N;
    chunk.data = outdata;

    CuAssertIntEquals(ct, 0, fitsbin_write_chunk_header(fb, &chunk));
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, 0, fitsbin_write_items(fb, &chunk, outdata + (N-1) - i, 1));
    }
    CuAssertIntEquals(ct, 0, fitsbin_fix_chunk_header(fb, &chunk));

    fitsbin_chunk_clean(&chunk);

    // writing shouldn't affect the data values
    for (i=0; i<N; i++) {
        CuAssertIntEquals(ct, outdata[i], i*i);
    }

    CuAssertIntEquals(ct, 0, fitsbin_switch_to_reading(fb));

    fitsbin_chunk_init(&chunk);
    chunk.tablename = "test2";
    fitsbin_add_chunk(fb, &chunk);
    chunk.tablename = "test2B";
    fitsbin_add_chunk(fb, &chunk);

    CuAssertIntEquals(ct, 0, fitsbin_read(fb));

    ch = fitsbin_get_chunk(fb, 0);
    CuAssertIntEquals(ct, 1, ch->itemsize);
    CuAssertIntEquals(ct, N * sizeof(double), ch->nrows);
    indata = ch->data;
    CuAssertPtrNotNull(ct, indata);
    CuAssertIntEquals(ct, 0, memcmp(outdata, indata, sizeof(outdata)));

    ch = fitsbin_get_chunk(fb, 1);
    CuAssertIntEquals(ct, sizeof(double), ch->itemsize);
    CuAssertIntEquals(ct, N, ch->nrows);
    indata = ch->data;
    CuAssertPtrNotNull(ct, indata);
    for (i=0; i<N; i++) {
        CuAssertDblEquals(ct, outdata[N-1 - i], indata[i], 1e-10);
    }

    CuAssertIntEquals(ct, 0, fitsbin_close(fb));
}





