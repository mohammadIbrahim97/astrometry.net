/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#include "keywords.h"
#include "fitsbin.h"
#include "fitsioutils.h"
#include "ioutils.h"
#include "fitsfile.h"
#include "errors.h"
#include "an-endian.h"
#include "tic.h"
#include "log.h"

// For in-memory: storage of previously-written extensions.
struct fitsext {
    qfits_header* header;
    char* tablename;
    bl* items;
};
typedef struct fitsext fitsext_t;

qfits_header* fitsbin_get_header(const fitsbin_t* fb, int ext) {
    assert(fb->fits);
    return anqfits_get_header(fb->fits, ext);
}

int fitsbin_get_datinfo(fitsbin_t* fb, int ext, off_t* pstart, off_t* psize) {
    assert(fb->fits);
    if (pstart)
        *pstart = anqfits_data_start(fb->fits, ext);
    if (psize)
        *psize = anqfits_data_size(fb->fits, ext);
    return 0;
}

const qfits_table* fitsbin_get_table_const(fitsbin_t* fb, int ext) {
    assert(fb->fits);
    return anqfits_get_table_const(fb->fits, ext);
}

int fitsbin_n_ext(const fitsbin_t* fb) {
    assert(fb->fits);
    return anqfits_n_ext(fb->fits);
}

FILE* fitsbin_get_fid(fitsbin_t* fb) {
    return fb->fid;
}

static int nchunks(fitsbin_t* fb) {
    return bl_size(fb->chunks);
}

static fitsbin_chunk_t* get_chunk(fitsbin_t* fb, int i) {
    if (i >= bl_size(fb->chunks)) {
        ERROR("Attempt to get chunk %i from a fitsbin with only %zu chunks",
              i, bl_size(fb->chunks));
        return NULL;
    }
    if (i < 0) {
        ERROR("Attempt to get fitsbin chunk %i", i);
        return NULL;
    }
    return bl_access(fb->chunks, i);
}

static fitsbin_t* new_fitsbin(const char* fn) {
    fitsbin_t* fb;
    fb = calloc(1, sizeof(fitsbin_t));
    if (!fb)
        return NULL;
    fb->chunks = bl_new(4, sizeof(fitsbin_chunk_t));
    if (!fn)
        // Can't make it NULL or qfits freaks out.
        fb->filename = strdup("");
    else
        fb->filename = strdup(fn);
    return fb;
}
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define ASTROMETRY_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__)
#define ASTROMETRY_THREAD_LOCAL __thread
#else
#error "A thread-local storage implementation is required"
#endif

/*
 * A negative value means that this thread has no pass-level override.
 * Mapping code then uses the configured fixed/default advice.
 */
static ASTROMETRY_THREAD_LOCAL int fitsbin_thread_mmap_advice = -1;

/*
 * Preserve the current default here. If the existing default is random,
 * leave this as random. If it is normal, change only this initializer.
 */


static fitsbin_mmap_advice_t fitsbin_mmap_policy_initial_advice(
    fitsbin_mmap_policy_t policy) {
    switch (policy) {
    case FITSBIN_MMAP_POLICY_FIXED_NORMAL:
        return FITSBIN_MMAP_ADVICE_NORMAL;

    case FITSBIN_MMAP_POLICY_FIXED_RANDOM:
    case FITSBIN_MMAP_POLICY_ADAPTIVE:
#ifdef MADV_RANDOM
        return FITSBIN_MMAP_ADVICE_RANDOM;
#else
        return FITSBIN_MMAP_ADVICE_NORMAL;
#endif
    }

    return FITSBIN_MMAP_ADVICE_NORMAL;
}

fitsbin_mmap_policy_t fitsbin_mmap_policy_parse(
    const char* value) {
    if (!value || !value[0]) {
        return FITSBIN_MMAP_POLICY_FIXED_RANDOM;
    }

    if (!strcasecmp(value, "normal")) {
        return FITSBIN_MMAP_POLICY_FIXED_NORMAL;
    }

    if (!strcasecmp(value, "random")) {
        return FITSBIN_MMAP_POLICY_FIXED_RANDOM;
    }

    if (!strcasecmp(value, "adaptive")) {
        return FITSBIN_MMAP_POLICY_ADAPTIVE;
    }

    return FITSBIN_MMAP_POLICY_FIXED_RANDOM;
}

fitsbin_mmap_policy_t fitsbin_get_configured_mmap_policy(void) {
    /*
     * Production uses the measured RANDOM baseline automatically. The
     * historical mmap environment switches are intentionally ignored.
     */
    return FITSBIN_MMAP_POLICY_FIXED_RANDOM;
}

const char* fitsbin_mmap_policy_name(
    fitsbin_mmap_policy_t policy) {
    switch (policy) {
    case FITSBIN_MMAP_POLICY_FIXED_NORMAL:
        return "normal";

    case FITSBIN_MMAP_POLICY_FIXED_RANDOM:
        return "random";

    case FITSBIN_MMAP_POLICY_ADAPTIVE:
        return "adaptive";
    }

    return "unknown";
}

const char* fitsbin_mmap_advice_name(
    fitsbin_mmap_advice_t advice) {
    switch (advice) {
    case FITSBIN_MMAP_ADVICE_NORMAL:
        return "normal";

    case FITSBIN_MMAP_ADVICE_RANDOM:
        return "random";
    }

    return "unknown";
}

void fitsbin_mmap_advice_state_reset(
    fitsbin_mmap_advice_state_t* state) {
    if (!state) {
        return;
    }

    state->effective_advice =
        fitsbin_mmap_policy_initial_advice(state->policy);

    state->pass_number = 0;
    state->completed_clean_unsolved_passes = 0;
    state->transition_count = 0;
}

void fitsbin_mmap_advice_state_init(
    fitsbin_mmap_advice_state_t* state,
    fitsbin_mmap_policy_t policy) {
    if (!state) {
        return;
    }

    state->policy = policy;
    fitsbin_mmap_advice_state_reset(state);
}

fitsbin_mmap_advice_t fitsbin_mmap_advice_state_begin_pass(
    const fitsbin_mmap_advice_state_t* state) {
    if (!state) {
        return fitsbin_mmap_policy_initial_advice(
            fitsbin_get_configured_mmap_policy());
    }

    return state->effective_advice;
}

void fitsbin_mmap_set_thread_advice(
    fitsbin_mmap_advice_t advice) {
    fitsbin_thread_mmap_advice = (int)advice;
}

void fitsbin_mmap_clear_thread_advice(void) {
    fitsbin_thread_mmap_advice = -1;
}

fitsbin_mmap_advice_t fitsbin_mmap_current_advice(void) {
    if (fitsbin_thread_mmap_advice >= 0) {
        return (fitsbin_mmap_advice_t)
            fitsbin_thread_mmap_advice;
    }

    return fitsbin_mmap_policy_initial_advice(
        fitsbin_get_configured_mmap_policy());
}

anbool fitsbin_mmap_policy_complete_pass(
    fitsbin_mmap_advice_state_t* state,
    anbool pass_completed,
    anbool pass_exhaustive,
    anbool pass_solved,
    anbool pass_cancelled,
    int pass_rc,
    int pass_status) {
    if (!state) {
        return FALSE;
    }

    /*
     * pass_number counts fully returned pass attempts, including solved or
     * unsuccessful attempts. Partial execution is not counted.
     */
    if (pass_completed) {
        state->pass_number++;
    }

    /*
     * Only a complete, exhaustive, clean, unsolved pass is evidence for
     * changing the following pass.
     */
    if (!pass_completed ||
        !pass_exhaustive ||
        pass_solved ||
        pass_cancelled ||
        pass_rc != 0 ||
        pass_status != 0) {
        return FALSE;
    }

    if (state->policy != FITSBIN_MMAP_POLICY_ADAPTIVE) {
        return FALSE;
    }

    state->completed_clean_unsolved_passes++;

    if (state->effective_advice ==
        FITSBIN_MMAP_ADVICE_NORMAL) {
        return FALSE;
    }

    state->effective_advice =
        FITSBIN_MMAP_ADVICE_NORMAL;

    state->transition_count++;

    return TRUE;
}

static int fitsbin_mmap_os_advice(fitsbin_mmap_advice_t advice) {

    switch (advice) {
    case FITSBIN_MMAP_ADVICE_NORMAL:
        return MADV_NORMAL;

    case FITSBIN_MMAP_ADVICE_RANDOM:
        return MADV_RANDOM;
    }



    return MADV_NORMAL;
}

int fitsbin_configure_index_mmap(fitsbin_t* fb) {
    long page_size;

    if (!fb) {
        return -1;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        fb->mmap_page_size = (size_t)page_size;
    } else {
        fb->mmap_page_size = 0;
    }

    /*
     * A shard worker may install the current field-pass advice in thread-local
     * state. Outside a shard worker this resolves to the fixed production
     * policy. No mmap environment controls are consulted.
     */
    fb->mmap_advice = fitsbin_mmap_current_advice();
    fb->mmap_advice_failed = FALSE;

    /* Explicit prefetch remains disabled by the production policy. */
    fb->mmap_prefetch_enabled = FALSE;
    fb->mmap_prefetch_failed = FALSE;

    return 0;
}
int fitsbin_resolve_mapped_range(fitsbin_t* fb,
                                 const void* data,
                                 size_t size,
                                 const void** map_base,
                                 size_t* map_size,
                                 const void** range_start,
                                 size_t* range_size) {
    uintptr_t request_start;
    int i;

    if (!fb ||
        !data ||
        !size ||
        !map_base ||
        !map_size ||
        !range_start ||
        !range_size) {
        return -1;
    }

    *map_base = NULL;
    *map_size = 0;
    *range_start = NULL;
    *range_size = 0;

    request_start = (uintptr_t)data;

    for (i = 0; i < nchunks(fb); i++) {
        fitsbin_chunk_t* chunk = get_chunk(fb, i);
        uintptr_t mapping_start;
        uintptr_t mapping_end;
        size_t clipped_size;

        if (!chunk ||
            !chunk->map ||
            !chunk->mapsize) {
            continue;
        }

        mapping_start = (uintptr_t)chunk->map;

        if (chunk->mapsize > UINTPTR_MAX - mapping_start) {
            continue;
        }

        mapping_end = mapping_start + chunk->mapsize;

        if (request_start < mapping_start ||
            request_start >= mapping_end) {
            continue;
        }

        clipped_size = size;

        if (clipped_size > mapping_end - request_start) {
            clipped_size = (size_t)(mapping_end - request_start);
        }

        if (!clipped_size) {
            return 0;
        }

        *map_base = chunk->map;
        *map_size = chunk->mapsize;
        *range_start = data;
        *range_size = clipped_size;

        return 1;
    }

    return 0;
}


int fitsbin_set_mmap_advice(
    fitsbin_t* fb,
    fitsbin_mmap_advice_t advice,
    anbool reapply_existing) {
    int native_advice;
    int first_error = 0;
    int i;

    if (!fb) {
        errno = EINVAL;
        return -1;
    }

    if (advice != FITSBIN_MMAP_ADVICE_NORMAL &&
        advice != FITSBIN_MMAP_ADVICE_RANDOM) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Avoid repeatedly issuing madvise() when several code paths encounter
     * the same fitsbin object during one pass.
     */
    if (fb->mmap_advice == advice)
        return 0;

    fb->mmap_advice = advice;

    if (!reapply_existing)
        return 0;

    native_advice = fitsbin_mmap_os_advice(advice);

    /*
     * Use the same chunk traversal expression used by fitsbin_close().
     * In the current block-list layout this is bl_size()/bl_access().
     */
    for (i = 0; i < bl_size(fb->chunks); i++) {
        fitsbin_chunk_t* chunk = bl_access(fb->chunks, i);

        if (!chunk || !chunk->map || !chunk->mapsize)
            continue;

        if (madvise(chunk->map, chunk->mapsize, native_advice) != 0) {
            int saved_errno = errno;

            if (!first_error)
                first_error = saved_errno;

            logmsg("Warning: madvise(%s) failed for %s table %s: %s\n",
                   advice == FITSBIN_MMAP_ADVICE_RANDOM
                       ? "MADV_RANDOM"
                       : "MADV_NORMAL",
                   fb->filename ? fb->filename : "(unknown)",
                   chunk->tablename ? chunk->tablename : "(unknown)",
                   strerror(saved_errno));
        }
    }

    if (first_error) {
        errno = first_error;
        return -1;
    }

    return 0;
}
int fitsbin_prefetch_data(fitsbin_t* fb, const void* data, size_t size) {
#ifdef MADV_WILLNEED
    uintptr_t request_start;
    uintptr_t request_end;
    size_t page_size;
    int i;

    if (!fb || !data || !size ||
        fb->mmap_advice != FITSBIN_MMAP_ADVICE_RANDOM ||
        !fb->mmap_prefetch_enabled || fb->mmap_prefetch_failed) {
        return 0;
    }

    request_start = (uintptr_t)data;
    page_size = fb->mmap_page_size;

    if (!page_size) {
        long detected_page_size = sysconf(_SC_PAGESIZE);

        if (detected_page_size <= 0) {
            fb->mmap_prefetch_failed = TRUE;
            return -1;
        }

        page_size = (size_t)detected_page_size;
    }

    for (i = 0; i < nchunks(fb); i++) {
        fitsbin_chunk_t* chunk = get_chunk(fb, i);
        uintptr_t map_start;
        uintptr_t map_end;
        uintptr_t advise_start;
        uintptr_t advise_end;
        uintptr_t remainder;

        if (!chunk || !chunk->map || !chunk->mapsize) {
            continue;
        }

        map_start = (uintptr_t)chunk->map;
        if (chunk->mapsize > UINTPTR_MAX - map_start) {
            continue;
        }
        map_end = map_start + chunk->mapsize;

        if (request_start < map_start || request_start >= map_end) {
            continue;
        }

        if (size > map_end - request_start) {
            request_end = map_end;
        } else {
            request_end = request_start + size;
        }

        advise_start =
            request_start - request_start % (uintptr_t)page_size;
        advise_end = request_end;
        remainder = advise_end % (uintptr_t)page_size;

        if (remainder) {
            uintptr_t padding = (uintptr_t)page_size - remainder;

            if (padding > map_end - advise_end) {
                advise_end = map_end;
            } else {
                advise_end += padding;
            }
        }

        if (advise_end <= advise_start) {
            return 0;
        }

        if (madvise((void*)advise_start,
                    (size_t)(advise_end - advise_start),
                    MADV_WILLNEED)) {
            const char* filename =
                fb->filename ? fb->filename : "(unknown file)";
            const char* tablename =
                chunk->tablename ? chunk->tablename : "(unknown table)";

            logmsg("Warning: madvise(MADV_WILLNEED) failed for %s "
                   "table %s: %s; disabling mmap prefetch for this file.\n",
                   filename, tablename, strerror(errno));
            fb->mmap_prefetch_failed = TRUE;
            return -1;
        }

        return 1;
    }
#else
    (void)fb;
    (void)data;
    (void)size;
#endif

    return 0;
}

// Apply the configured policy after mmap and before the first table access.
static void apply_mmap_advice(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    const char* filename;
    const char* tablename;

    if (fb->mmap_advice == FITSBIN_MMAP_ADVICE_NORMAL ||
        fb->mmap_advice_failed) {
        return;
    }

    filename = fb->filename != NULL
        ? fb->filename
        : "(unknown file)";

    tablename = chunk->tablename != NULL
        ? chunk->tablename
        : "(unknown table)";

#ifdef MADV_RANDOM
    if (fb->mmap_advice == FITSBIN_MMAP_ADVICE_RANDOM) {
        if (madvise(chunk->map, chunk->mapsize, MADV_RANDOM) != 0) {
            logmsg("Warning: madvise(MADV_RANDOM) failed for %s table %s: "
                   "%s; using normal mmap advice for this file.\n",
                   filename, tablename, strerror(errno));

            fb->mmap_advice_failed = TRUE;
            fb->mmap_advice = FITSBIN_MMAP_ADVICE_NORMAL;
            return;
        }

        debug("Applied MADV_RANDOM to %zu bytes for %s table %s.\n",
              chunk->mapsize, filename, tablename);
    }
#else
    (void)filename;
    (void)tablename;
#endif
}
static anbool in_memory(fitsbin_t* fb) {
    return fb->inmemory;
}


static void free_chunk(fitsbin_chunk_t* chunk) {
    if (!chunk) return;
    free(chunk->tablename_copy);
    if (chunk->header)
        qfits_header_destroy(chunk->header);
    if (chunk->map) {
        if (munmap(chunk->map, chunk->mapsize)) {
            SYSERROR("Failed to munmap fitsbin chunk");
        }
    }
}

void fitsbin_chunk_init(fitsbin_chunk_t* chunk) {
    memset(chunk, 0, sizeof(fitsbin_chunk_t));
}

void fitsbin_chunk_clean(fitsbin_chunk_t* chunk) {
    free_chunk(chunk);
}

void fitsbin_chunk_reset(fitsbin_chunk_t* chunk) {
    fitsbin_chunk_clean(chunk);
    fitsbin_chunk_init(chunk);
}

fitsbin_chunk_t* fitsbin_get_chunk(fitsbin_t* fb, int chunk) {
    return get_chunk(fb, chunk);
}

int fitsbin_n_chunks(fitsbin_t* fb) {
    return nchunks(fb);
}

fitsbin_chunk_t* fitsbin_add_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    chunk = bl_append(fb->chunks, chunk);
    chunk->tablename_copy = strdup(chunk->tablename);
    chunk->tablename = chunk->tablename_copy;
    return chunk;
}

off_t fitsbin_get_data_start(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    return chunk->header_end;
}

int fitsbin_close_fd(fitsbin_t* fb) {
    if (!fb) return 0;
    if (fb->fid) {
        if (fclose(fb->fid)) {
            SYSERROR("Error closing fitsbin file");
            return -1;
        }
        fb->fid = NULL;
    }
    return 0;
}

int fitsbin_close(fitsbin_t* fb) {
    int i;
    int rtn = 0;
    if (!fb) return rtn;
    rtn = fitsbin_close_fd(fb);
    if (fb->primheader)
        qfits_header_destroy(fb->primheader);
    for (i=0; i<nchunks(fb); i++) {
        if (in_memory(fb)) {
            free(get_chunk(fb, i)->data);
        }
        free_chunk(get_chunk(fb, i));
    }
    free(fb->filename);
    if (fb->chunks)
        bl_free(fb->chunks);

    if (in_memory(fb)) {
        for (i=0; i<bl_size(fb->extensions); i++) {
            fitsext_t* ext = bl_access(fb->extensions, i);
            bl_free(ext->items);
            qfits_header_destroy(ext->header);
            free(ext->tablename);
        }
        bl_free(fb->extensions);
        bl_free(fb->items);
    }

    if (fb->tables) {
        for (i=0; i<fb->Next; i++) {
            if (!fb->tables[i])
                continue;
            qfits_table_close(fb->tables[i]);
        }
        free(fb->tables);
    }

    free(fb);
    return rtn;
}

int fitsbin_write_primary_header(fitsbin_t* fb) {
    if (in_memory(fb)) return 0;
    return fitsfile_write_primary_header(fb->fid, fb->primheader,
                                         &fb->primheader_end, fb->filename);
}

int fitsbin_write_primary_header_to(fitsbin_t* fb, FILE* fid) {
    off_t end;
    return fitsfile_write_primary_header(fid, fb->primheader, &end, "");
}

qfits_header* fitsbin_get_primary_header(const fitsbin_t* fb) {
    return fb->primheader;
}

void fitsbin_set_primary_header(fitsbin_t* fb, const qfits_header* hdr) {
    qfits_header_destroy(fb->primheader);
    fb->primheader = qfits_header_copy(hdr);
}

int fitsbin_fix_primary_header(fitsbin_t* fb) {
    if (in_memory(fb)) return 0;
    return fitsfile_fix_primary_header(fb->fid, fb->primheader,
                                       &fb->primheader_end, fb->filename);
}

qfits_header* fitsbin_get_chunk_header(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    qfits_table* table;
    int tablesize;
    qfits_header* hdr;
    int ncols = 1;
    char* fn = NULL;

    if (chunk->header)
        return chunk->header;

    // Create the new header.

    if (fb)
        fn = fb->filename;
    if (!fn)
        fn = "";
    // the table header
    tablesize = chunk->itemsize * chunk->nrows * ncols;
    table = qfits_table_new(fn, QFITS_BINTABLE, tablesize, ncols, chunk->nrows);
    assert(table);
    qfits_col_fill(table->col, chunk->itemsize, 0, 1,
                   chunk->forced_type ? chunk->forced_type : TFITS_BIN_TYPE_A,
                   chunk->tablename, "", "", "", 0, 0, 0, 0, 0);
    hdr = qfits_table_ext_header_default(table);
    qfits_table_close(table);
    chunk->header = hdr;
    return hdr;
}

static int write_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk, int flipped) {
    int N;
    if (fitsbin_write_chunk_header(fb, chunk)) {
        return -1;
    }
    N = chunk->nrows;
    if (!flipped) {
        if (fitsbin_write_items(fb, chunk, chunk->data, chunk->nrows))
            return -1;
    } else {
        // endian-flip words of the data of length "flipped", write them,
        // then flip them back to the way they were.

        // NO, copy to temp array, flip it, write it.

        // this is slow, but it won't be run very often...

        int i, j;
        int nper = chunk->itemsize / flipped;
        char tempdata[chunk->itemsize];
        assert(chunk->itemsize >= flipped);
        assert(nper * flipped == chunk->itemsize);
        for (i=0; i<N; i++) {
            // copy it...
            memcpy(tempdata, chunk->data + i*chunk->itemsize, chunk->itemsize);
            // swap it...
            for (j=0; j<nper; j++)
                endian_swap(tempdata + j*flipped, flipped);
            // write it...
            fitsbin_write_item(fb, chunk, tempdata);
        }
    }
    chunk->nrows -= N;
    if (fitsbin_fix_chunk_header(fb, chunk)) {
        return -1;
    }
    return 0;
}

int fitsbin_write_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    return write_chunk(fb, chunk, 0);
}

int fitsbin_write_chunk_to(fitsbin_t* fb, fitsbin_chunk_t* chunk, FILE* fid) {
    //logmsg("fitsbin_write_chunk_to: table %s, itemsize %i, nrows %i, header_start %lu, header_end %lu\n", chunk->tablename_copy, chunk->itemsize, chunk->nrows, chunk->header_start, chunk->header_end);
    //off_t off = ftello(fid);
    //logmsg("offset: %lu\n", off);
    if (fitsbin_write_chunk_header_to(fb, chunk, fid) ||
        fitsbin_write_items_to(chunk, chunk->data, chunk->nrows, fid))
        return -1;
    return 0;
}

int fitsbin_write_chunk_flipped(fitsbin_t* fb, fitsbin_chunk_t* chunk,
                                int wordsize) {
    return write_chunk(fb, chunk, wordsize);
}

int fitsbin_write_chunk_header(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    qfits_header* hdr;
    hdr = fitsbin_get_chunk_header(fb, chunk);
    if (in_memory(fb)) return 0;
    if (fitsfile_write_header(fb->fid, hdr,
                              &chunk->header_start, &chunk->header_end,
                              -1, fb->filename)) {
        return -1;
    }
    return 0;
}

int fitsbin_write_chunk_header_to(fitsbin_t* fb, fitsbin_chunk_t* chunk, FILE* fid) {
    off_t start, end;
    qfits_header* hdr;
    hdr = fitsbin_get_chunk_header(fb, chunk);
    if (fitsfile_write_header(fid, hdr, &start, &end, -1, ""))
        return -1;
    return 0;
}

int fitsbin_fix_chunk_header(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    // update NAXIS2 to reflect the number of rows written.
    fits_header_mod_int(chunk->header, "NAXIS2", chunk->nrows, NULL);

    // HACK -- leverage the fact that this is the last function called for each chunk...
    if (in_memory(fb)) {
        // Save this chunk.
        fitsext_t ext;
        // table, header, items
        if (!fb->extensions)
            fb->extensions = bl_new(4, sizeof(fitsext_t));
        ext.header = qfits_header_copy(chunk->header);
        ext.items = fb->items;
        ext.tablename = strdup(chunk->tablename);
        bl_append(fb->extensions, &ext);
        fb->items = NULL;
        return 0;
    }

    if (fitsfile_fix_header(fb->fid, chunk->header,
                            &chunk->header_start, &chunk->header_end,
                            -1, fb->filename)) {
        return -1;
    }
    return 0;
}

int fitsbin_write_items_to(fitsbin_chunk_t* chunk, void* data, int N, FILE* fid) {
    off_t offset;
    if (fwrite(data, chunk->itemsize, N, fid) != N) {
        SYSERROR("Failed to write %i items", N);
        return -1;
    }
    offset = ftello(fid);
    fits_pad_file(fid);
    if (fseeko(fid, offset, SEEK_SET)) {
        SYSERROR("Failed to fseeko in fitsbin_write_items_to.");
        return -1;
    }
    return 0;
}

int fitsbin_write_items(fitsbin_t* fb, fitsbin_chunk_t* chunk, void* data, int N) {
    if (in_memory(fb)) {
        int i;
        char* src = data;
        if (!fb->items)
            fb->items = bl_new(1024, chunk->itemsize);
        for (i=0; i<N; i++) {
            bl_append(fb->items, src);
            src += chunk->itemsize;
        }
    } else {
        if (fitsbin_write_items_to(chunk, data, N, fb->fid))
            return -1;
    }
    chunk->nrows += N;
    return 0;
}

int fitsbin_write_item(fitsbin_t* fb, fitsbin_chunk_t* chunk, void* data) {
    return fitsbin_write_items(fb, chunk, data, 1);
}

// Like fitsioutils.c : fits_find_table_column(), but using our cache...
static int find_table_column(fitsbin_t* fb, const char* colname, off_t* pstart, off_t* psize, int* pext) {
    int i;
    for (i=1; i<fb->Next; i++) {
        int c;
        const qfits_table* table = fitsbin_get_table_const(fb, i);
        if (!table)
            continue;
        c = fits_find_column(table, colname);
        if (c == -1)
            continue;
        if (fitsbin_get_datinfo(fb, i, pstart, psize)) {
            ERROR("error getting start/size for ext %i in file %s.\n", i, fb->filename);
            return -1;
        }
        if (pext) *pext = i;
        return 0;
    }
    debug("searched %i extensions in file %s but didn't find a table with a column \"%s\".\n",
          fb->Next, fb->filename, colname);
    return -1;
}

static int read_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    off_t tabstart=0, tabsize=0;
    int ext;
    size_t expected = 0;
    int mode, flags;
    off_t mapstart;
    int mapoffset;
    int table_nrows;
    int table_rowsize;
    fitsext_t* inmemext = NULL;

    if (in_memory(fb)) {
        int i;
        anbool gotit = FALSE;
        for (i=0; i<bl_size(fb->extensions); i++) {
            inmemext = bl_access(fb->extensions, i);
            if (strcasecmp(inmemext->tablename, chunk->tablename))
                continue;
            // found it!
            gotit = TRUE;
            break;
        }
        if (!gotit && chunk->required) {
            ERROR("Couldn't find table \"%s\"", chunk->tablename);
            return -1;
        }
        table_nrows = bl_size(inmemext->items);
        table_rowsize = bl_datasize(inmemext->items);
        chunk->header = qfits_header_copy(inmemext->header);

    } else {
        //double t0;
        //t0 = timenow();
        if (find_table_column(fb, chunk->tablename, &tabstart, &tabsize, &ext)) {
            if (chunk->required)
                ERROR("Couldn't find table \"%s\" in file \"%s\"",
                      chunk->tablename, fb->filename);
            return -1;
        }
        //debug("fits_find_table_column(%s) took %g ms\n", chunk->tablename, 1000 * (timenow() - t0));

        //t0 = timenow();
        chunk->header = fitsbin_get_header(fb, ext);
        if (!chunk->header) {
            ERROR("Couldn't read FITS header from file \"%s\" extension %i", fb->filename, ext);
            return -1;
        }
        //debug("reading chunk header (%s) took %g ms\n", chunk->tablename, 1000 * (timenow() - t0));
        table_nrows = fitsbin_get_table_const(fb, ext)->nr;
        table_rowsize = fitsbin_get_table_const(fb, ext)->tab_w;
    }

    if (!chunk->itemsize)
        chunk->itemsize = table_rowsize;
    if (!chunk->nrows)
        chunk->nrows = table_nrows;

    if (chunk->callback_read_header &&
        chunk->callback_read_header(fb, chunk)) {
        ERROR("fitsbin callback_read_header failed");
        return -1;
    }

    if (chunk->nrows != table_nrows) {
        ERROR("Table %s in file %s: expected %i data items (ie, rows), found %i",
              chunk->tablename, fb->filename, chunk->nrows, table_nrows);
        return -1;
    }

    if (chunk->itemsize != table_rowsize) {
        ERROR("Table %s in file %s: expected data size %i (ie, row width in bytes), found %i",
              chunk->tablename, fb->filename, chunk->itemsize, table_rowsize);
        return -1;
    }

    expected = (size_t)chunk->itemsize * (size_t)chunk->nrows;
    if (in_memory(fb)) {
        int i;
        chunk->data = malloc(expected);
        for (i=0; i<chunk->nrows; i++) {
            memcpy(((char*)chunk->data) + (size_t)i * (size_t)chunk->itemsize,
                   bl_access(inmemext->items, i), chunk->itemsize);
        }
        // delete inmemext->items ?

    } else {

        if (fits_bytes_needed(expected) != tabsize) {
            ERROR("Expected table size (%zu => %i FITS blocks) is not equal to "
                  "size of table \"%s\" (%zu => %i FITS blocks).",
                  expected, fits_blocks_needed(expected),
                  chunk->tablename, (size_t)tabsize,
                  (int)(tabsize / (off_t)FITS_BLOCK_SIZE));
            return -1;
        }
        get_mmap_size(tabstart, tabsize, &mapstart, &(chunk->mapsize), &mapoffset);
        mode = PROT_READ;
        flags = MAP_SHARED;
        chunk->map = mmap(0, chunk->mapsize, mode, flags, fileno(fb->fid), mapstart);
        if (chunk->map == MAP_FAILED) {
            SYSERROR("Couldn't mmap file \"%s\"", fb->filename);
            chunk->map = NULL;
            return -1;
        }
        apply_mmap_advice(fb, chunk);
        chunk->data = chunk->map + mapoffset;
    }
    return 0;
}

int fitsbin_read_chunk(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    if (read_chunk(fb, chunk))
        return -1;
    fitsbin_add_chunk(fb, chunk);
    return 0;
}

int fitsbin_read(fitsbin_t* fb) {
    int i;

    for (i=0; i<nchunks(fb); i++) {
        fitsbin_chunk_t* chunk = get_chunk(fb, i);
        if (read_chunk(fb, chunk)) {
            if (chunk->required)
                goto bailout;
        }
    }
    return 0;

 bailout:
    return -1;
}

char* fitsbin_get_filename(const fitsbin_t* fb) {
    return fb->filename;
}

fitsbin_t* fitsbin_open_fits(anqfits_t* fits) {
    fitsbin_t* fb;
    fb = new_fitsbin(fits->filename);
    if (!fb)
        return fb;
    fb->fid = fopen(fits->filename, "rb");
    if (!fb->fid) {
        SYSERROR("Failed to open file \"%s\"", fits->filename);
        goto bailout;
    }
    fb->Next = anqfits_n_ext(fits);
    debug("N ext: %i\n", fb->Next);
    fb->fits = fits;
    fb->primheader = fitsbin_get_header(fb, 0);
    if (!fb->primheader) {
        ERROR("Couldn't read primary FITS header from file \"%s\"", fits->filename);
        goto bailout;
    }
    return fb;
 bailout:
    fitsbin_close(fb);
    return NULL;
}

fitsbin_t* fitsbin_open(const char* fn) {
    anqfits_t* fits;
    fits = anqfits_open(fn);
    if (!fits) {
        ERROR("Failed to open file \"%s\"", fn);
        return NULL;
    }
    return fitsbin_open_fits(fits);
}

fitsbin_t* fitsbin_open_in_memory() {
    fitsbin_t* fb;

    fb = new_fitsbin(NULL);
    if (!fb)
        return NULL;
    fb->primheader = qfits_table_prim_header_default();
    fb->inmemory = TRUE;
    return fb;
}

int fitsbin_switch_to_reading(fitsbin_t* fb) {
    int i;

    // clear the current chunk data??
    for (i=0; i<nchunks(fb); i++) {
        fitsbin_chunk_t* chunk = get_chunk(fb, i);
        if (chunk->header)
            qfits_header_destroy(chunk->header);
    }

    return 0;
}

fitsbin_t* fitsbin_open_for_writing(const char* fn) {
    fitsbin_t* fb;

    fb = new_fitsbin(fn);
    if (!fb)
        return NULL;
    fb->primheader = qfits_table_prim_header_default();
    fb->fid = fopen(fb->filename, "wb");
    if (!fb->fid) {
        SYSERROR("Couldn't open file \"%s\" for output", fb->filename);
        fitsbin_close(fb);
        return NULL;
    }
    return fb;
}

