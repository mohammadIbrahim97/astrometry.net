/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "keywords.h"
#include "fitsbin.h"
#include "fitsbin_internal.h"
#include "fitsioutils.h"
#include "ioutils.h"
#include "fitsfile.h"
#include "errors.h"
#include "an-endian.h"
#include "tic.h"
#include "log.h"

/* A negative value preserves the serial NORMAL mapping behavior. */
static ASTROMETRY_THREAD_LOCAL int fitsbin_thread_mmap_advice = -1;

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

static const char* fitsbin_mmap_region_name(
    fitsbin_mmap_region_t region) {
    switch (region) {
    case FITSBIN_MMAP_REGION_PAYLOAD:
        return "payload";

    case FITSBIN_MMAP_REGION_TOPOLOGY:
        return "topology";
    }

    return "unknown";
}

fitsbin_mmap_advice_t fitsbin_get_mmap_advice(
    const fitsbin_t* fb) {
    if (!fb) {
        return FITSBIN_MMAP_ADVICE_NORMAL;
    }

    return fb->mmap_advice;
}

fitsbin_mmap_advice_t fitsbin_get_chunk_mmap_advice(
    const fitsbin_t* fb,
    const fitsbin_chunk_t* chunk) {
    if (!chunk) {
        return FITSBIN_MMAP_ADVICE_NORMAL;
    }

    /*
     * Compact tree topology has useful traversal locality. Sparse payload
     * follows the selected index policy; bounded mapped-page population does
     * not change that stable demand fallback.
     */
    if (chunk->mmap_region == FITSBIN_MMAP_REGION_TOPOLOGY) {
        return FITSBIN_MMAP_ADVICE_NORMAL;
    }
    return fitsbin_get_mmap_advice(fb);
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

    return FITSBIN_MMAP_ADVICE_NORMAL;
}

static int fitsbin_mmap_os_advice(fitsbin_mmap_advice_t advice) {

    switch (advice) {
    case FITSBIN_MMAP_ADVICE_NORMAL:
        return MADV_NORMAL;

    case FITSBIN_MMAP_ADVICE_RANDOM:
#ifdef MADV_RANDOM
        return MADV_RANDOM;
#else
        return MADV_NORMAL;
#endif
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
     * A shard worker installs its pass policy before opening an index. Serial
     * callers have no thread-local override and retain NORMAL.
    */
    fb->mmap_advice = fitsbin_mmap_current_advice();
    fb->mmap_advice_failed = FALSE;

    /*
     * Whole-file prefetch and buffered solver warming remain
     * disconnected. A successful bounded preparation must mean that all
     * planned pages have been populated, not merely queued for readahead.
     */
#if defined(MADV_POPULATE_READ)
    fb->mmap_prefetch_enabled = TRUE;
#else
    fb->mmap_prefetch_enabled = FALSE;
#endif
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
        errno = EINVAL;
        return -1;
    }

    *map_base = NULL;
    *map_size = 0;
    *range_start = NULL;
    *range_size = 0;

    if (!fb->chunks) {
        return 0;
    }

    request_start = (uintptr_t)data;

    for (i = 0; i < fitsbin_n_chunks(fb); i++) {
        fitsbin_chunk_t* chunk = fitsbin_get_chunk(fb, i);
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

int fitsbin_set_mmap_range_advice(
    fitsbin_t* fb,
    const void* data,
    size_t size,
    fitsbin_mmap_advice_t advice) {
    const void* map_base;
    const void* range_start;
    size_t map_size;
    size_t range_size;
    size_t page_size;
    uintptr_t map_begin;
    uintptr_t map_end;
    uintptr_t begin;
    uintptr_t end;
    uintptr_t remainder;
    int resolved;
    int native_advice;

    if (!fb || !data || !size ||
        (advice != FITSBIN_MMAP_ADVICE_NORMAL &&
         advice != FITSBIN_MMAP_ADVICE_RANDOM)) {
        errno = EINVAL;
        return -1;
    }
    resolved = fitsbin_resolve_mapped_range(
        fb,
        data,
        size,
        &map_base,
        &map_size,
        &range_start,
        &range_size);
    if (resolved <= 0) {
        return resolved;
    }
    if (range_size != size) {
        errno = ERANGE;
        return -1;
    }

    page_size = fb->mmap_page_size;
    if (!page_size) {
        long detected_page_size = sysconf(_SC_PAGESIZE);

        if (detected_page_size <= 0) {
            errno = EINVAL;
            return -1;
        }
        page_size = (size_t)detected_page_size;
    }

    map_begin = (uintptr_t)map_base;
    if (map_size > UINTPTR_MAX - map_begin) {
        errno = EOVERFLOW;
        return -1;
    }
    map_end = map_begin + map_size;
    begin = (uintptr_t)range_start;
    if (range_size > UINTPTR_MAX - begin) {
        end = map_end;
    } else {
        end = begin + range_size;
    }

    begin -= begin % (uintptr_t)page_size;
    if (begin < map_begin) {
        begin = map_begin;
    }
    remainder = end % (uintptr_t)page_size;
    if (remainder) {
        uintptr_t padding = (uintptr_t)page_size - remainder;

        if (padding > map_end - end) {
            end = map_end;
        } else {
            end += padding;
        }
    }
    if (end > map_end) {
        end = map_end;
    }
    if (end <= begin) {
        return 0;
    }

    native_advice = fitsbin_mmap_os_advice(advice);
    if (madvise((void*)begin, (size_t)(end - begin), native_advice)) {
        return -1;
    }
    return 1;
}

static void fitsbin_restore_normal_mmap_advice(
    fitsbin_t* fb) {
    int i;

    if (!fb) {
        return;
    }
    fb->mmap_advice = FITSBIN_MMAP_ADVICE_NORMAL;
    fb->mmap_advice_failed = TRUE;
    if (!fb->chunks) {
        return;
    }
    for (i = 0; i < bl_size(fb->chunks); i++) {
        fitsbin_chunk_t* chunk = bl_access_const(fb->chunks, i);

        if (!chunk || !chunk->map || !chunk->mapsize) {
            continue;
        }
        if (madvise(chunk->map, chunk->mapsize, MADV_NORMAL)) {
            logmsg("Warning: madvise(MADV_NORMAL) fallback failed for %s "
                   "table %s region %s: %s\n",
                   fb->filename ? fb->filename : "(unknown)",
                   chunk->tablename ? chunk->tablename : "(unknown)",
                   fitsbin_mmap_region_name(chunk->mmap_region),
                   strerror(errno));
        }
    }
}

int fitsbin_set_mmap_advice(
    fitsbin_t* fb,
    fitsbin_mmap_advice_t advice,
    anbool reapply_existing) {
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
    if (fb->mmap_advice == advice &&
        (!reapply_existing ||
         !fb->mmap_advice_failed)) {
        return 0;
    }

    fb->mmap_advice = advice;

    if (!reapply_existing) {
        if (!fb->chunks ||
            bl_size(fb->chunks) == 0) {
            fb->mmap_advice_failed = FALSE;
        }
        return 0;
    }

    if (!fb->chunks ||
        bl_size(fb->chunks) == 0) {
        fb->mmap_advice_failed = FALSE;
        return 0;
    }

    /*
     * Reapply the selected index policy to every existing mapped chunk.
     * Use the same chunk traversal expression used by fitsbin_close().
     */
    for (i = 0; i < bl_size(fb->chunks); i++) {
        fitsbin_chunk_t* chunk = bl_access_const(fb->chunks, i);
        fitsbin_mmap_advice_t chunk_advice;
        int native_advice;

        if (!chunk || !chunk->map || !chunk->mapsize) {
            continue;
        }

        chunk_advice = fitsbin_get_chunk_mmap_advice(
            fb, chunk);
        native_advice =
            fitsbin_mmap_os_advice(chunk_advice);

        if (madvise(chunk->map, chunk->mapsize, native_advice) != 0) {
            int saved_errno = errno;

            if (!first_error) {
                first_error = saved_errno;
            }

            logmsg("Warning: madvise(%s) failed for %s table %s "
                   "region %s: %s\n",
                   chunk_advice == FITSBIN_MMAP_ADVICE_RANDOM
                       ? "MADV_RANDOM"
                       : "MADV_NORMAL",
                   fb->filename ? fb->filename : "(unknown)",
                   chunk->tablename ? chunk->tablename : "(unknown)",
                   fitsbin_mmap_region_name(chunk->mmap_region),
                   strerror(saved_errno));
        }
    }

    if (first_error) {
        fitsbin_restore_normal_mmap_advice(fb);
        errno = first_error;
        return -1;
    }

    fb->mmap_advice_failed = FALSE;
    return 0;
}
// Apply the file mapping advice before the first table access.
void fitsbin_apply_mmap_advice(fitsbin_t* fb, fitsbin_chunk_t* chunk) {
    fitsbin_mmap_advice_t advice;
    const char* filename;
    const char* tablename;

    advice = fitsbin_get_chunk_mmap_advice(fb, chunk);

    if (advice == FITSBIN_MMAP_ADVICE_NORMAL ||
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
    if (advice == FITSBIN_MMAP_ADVICE_RANDOM) {
        if (madvise(chunk->map, chunk->mapsize, MADV_RANDOM) != 0) {
            logmsg("Warning: madvise(MADV_RANDOM) failed for %s table %s "
                   "region %s: %s; using normal mmap advice for this "
                   "file.\n",
                   filename,
                   tablename,
                   fitsbin_mmap_region_name(chunk->mmap_region),
                   strerror(errno));

            fitsbin_restore_normal_mmap_advice(fb);
            return;
        }

        debug("Applied MADV_RANDOM to %zu bytes for %s table %s "
              "region %s.\n",
              chunk->mapsize,
              filename,
              tablename,
              fitsbin_mmap_region_name(chunk->mmap_region));
    }
#else
    (void)filename;
    (void)tablename;
#endif
}
