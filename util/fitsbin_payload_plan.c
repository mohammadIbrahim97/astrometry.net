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
#include <strings.h>
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

int fitsbin_prefetch_row_budget(
    const fitsbin_t* fb,
    size_t row_size,
    size_t row_count,
    size_t* byte_budget) {
    size_t page_size = fb ? fb->mmap_page_size : 0U;
    size_t per_row;

    if (!row_size || !row_count || !byte_budget) {
        errno = EINVAL;
        return -1;
    }
    if (!page_size) {
        long detected = sysconf(_SC_PAGESIZE);

        if (detected <= 0) {
            errno = EINVAL;
            return -1;
        }
        page_size = (size_t)detected;
    }
    if (page_size > (SIZE_MAX - row_size) / 2U) {
        errno = EOVERFLOW;
        return -1;
    }
    per_row = row_size + 2U * page_size;
    if (row_count > SIZE_MAX / per_row) {
        errno = EOVERFLOW;
        return -1;
    }
    *byte_budget = row_count * per_row;
    return 0;
}

static int fitsbin_compare_file_span(
    const void* left,
    const void* right) {
    const fitsbin_file_span_t* lhs = left;
    const fitsbin_file_span_t* rhs = right;

    if (lhs->begin < rhs->begin) {
        return -1;
    }
    if (lhs->begin > rhs->begin) {
        return 1;
    }
    if (lhs->end < rhs->end) {
        return -1;
    }
    if (lhs->end > rhs->end) {
        return 1;
    }
    return 0;
}

static int fitsbin_compare_mapped_span(
    const void* left,
    const void* right) {
    const fitsbin_mapped_span_t* lhs = left;
    const fitsbin_mapped_span_t* rhs = right;

    if (lhs->map_begin < rhs->map_begin) {
        return -1;
    }
    if (lhs->map_begin > rhs->map_begin) {
        return 1;
    }
    if (lhs->begin < rhs->begin) {
        return -1;
    }
    if (lhs->begin > rhs->begin) {
        return 1;
    }
    if (lhs->end < rhs->end) {
        return -1;
    }
    if (lhs->end > rhs->end) {
        return 1;
    }
    return 0;
}

int fitsbin_prepare_mapped_spans(
    fitsbin_t* fb,
    const fitsbin_prefetch_range_t* ranges,
    size_t range_count,
    size_t byte_budget,
    fitsbin_mapped_span_t* spans,
    size_t span_capacity,
    size_t* span_count,
    size_t* byte_count) {
    size_t accepted = 0U;
    size_t merged = 0U;
    size_t coalesced = 0U;
    size_t aligned_bytes = 0U;
    size_t exact_bytes = 0U;
    size_t gap_budget = 0U;
    size_t gap_bytes = 0U;
    size_t gap_merges = 0U;
    size_t page_size;
    size_t i;

    if (!span_count || !byte_count) {
        errno = EINVAL;
        return -1;
    }
    *span_count = 0U;
    *byte_count = 0U;
    if (!fb || !ranges || !range_count || !byte_budget ||
        !spans || range_count > span_capacity) {
        errno = range_count > span_capacity ? E2BIG : EINVAL;
        return -1;
    }
    page_size = fb->mmap_page_size;
    if (!page_size) {
        long detected = sysconf(_SC_PAGESIZE);

        if (detected <= 0) {
            errno = EINVAL;
            return -1;
        }
        page_size = (size_t)detected;
    }

    for (i = 0U; i < range_count; i++) {
        const void* map_base;
        const void* range_start;
        size_t map_size;
        size_t range_size;
        uintptr_t map_begin;
        uintptr_t map_end;
        uintptr_t begin;
        uintptr_t end;
        uintptr_t remainder;
        int resolved;

        if (!ranges[i].data || !ranges[i].size) {
            errno = EINVAL;
            return -1;
        }
        resolved = fitsbin_resolve_mapped_range(
            fb,
            ranges[i].data,
            ranges[i].size,
            &map_base,
            &map_size,
            &range_start,
            &range_size);
        if (resolved != 1 || range_size != ranges[i].size) {
            if (!resolved) {
                errno = ERANGE;
            }
            return -1;
        }
        map_begin = (uintptr_t)map_base;
        if (map_size > UINTPTR_MAX - map_begin) {
            errno = EOVERFLOW;
            return -1;
        }
        map_end = map_begin + map_size;
        begin = (uintptr_t)range_start;
        if (range_size > UINTPTR_MAX - begin) {
            errno = EOVERFLOW;
            return -1;
        }
        end = begin + range_size;
        begin -= begin % (uintptr_t)page_size;
        if (begin < map_begin) {
            begin = map_begin;
        }
        remainder = end % (uintptr_t)page_size;
        if (remainder) {
            uintptr_t padding =
                (uintptr_t)page_size - remainder;

            end = padding > map_end - end
                ? map_end
                : end + padding;
        }
        if (end > map_end) {
            end = map_end;
        }
        if (end <= begin) {
            errno = ERANGE;
            return -1;
        }
        spans[accepted].map_begin = map_begin;
        spans[accepted].map_end = map_end;
        spans[accepted].begin = begin;
        spans[accepted].end = end;
        accepted++;
    }

    qsort(
        spans,
        accepted,
        sizeof(spans[0]),
        fitsbin_compare_mapped_span);
    for (i = 0U; i < accepted; i++) {
        if (merged &&
            spans[i].map_begin == spans[merged - 1U].map_begin &&
            spans[i].map_end == spans[merged - 1U].map_end &&
            spans[i].begin <= spans[merged - 1U].end) {
            if (spans[i].end > spans[merged - 1U].end) {
                spans[merged - 1U].end = spans[i].end;
            }
            continue;
        }
        spans[merged++] = spans[i];
    }
    for (i = 0U; i < merged; i++) {
        size_t span_bytes =
            (size_t)(spans[i].end - spans[i].begin);

        if (exact_bytes > byte_budget ||
            span_bytes > byte_budget - exact_bytes) {
            errno = E2BIG;
            return -1;
        }
        exact_bytes += span_bytes;
    }
    gap_budget = MIN(
        byte_budget - exact_bytes,
        exact_bytes /
            FITSBIN_PAYLOAD_MAPPED_COALESCE_BUDGET_DIVISOR);
    for (i = 0U; i < merged; i++) {
        if (coalesced &&
            spans[i].map_begin ==
                spans[coalesced - 1U].map_begin &&
            spans[i].map_end ==
                spans[coalesced - 1U].map_end &&
            spans[i].begin > spans[coalesced - 1U].end) {
            size_t gap = (size_t)(
                spans[i].begin - spans[coalesced - 1U].end);

            if (gap <=
                    page_size *
                        FITSBIN_PAYLOAD_MAPPED_COALESCE_GAP_PAGES &&
                gap_bytes <= gap_budget &&
                gap <= gap_budget - gap_bytes) {
                spans[coalesced - 1U].end = spans[i].end;
                gap_bytes += gap;
                gap_merges++;
                continue;
            }
        }
        spans[coalesced++] = spans[i];
    }
    for (i = 0U; i < coalesced; i++) {
        size_t span_bytes =
            (size_t)(spans[i].end - spans[i].begin);
        if (aligned_bytes > byte_budget ||
            span_bytes > byte_budget - aligned_bytes) {
            errno = E2BIG;
            return -1;
        }
        aligned_bytes += span_bytes;
    }
    if (coalesced > merged || gap_merges > merged ||
        coalesced + gap_merges != merged ||
        exact_bytes > SIZE_MAX - gap_bytes ||
        aligned_bytes != exact_bytes + gap_bytes ||
        aligned_bytes > byte_budget) {
        errno = EFAULT;
        return -1;
    }
    *span_count = coalesced;
    *byte_count = aligned_bytes;
    return 0;
}

int fitsbin_prepare_mapped_file_spans(
    fitsbin_t* fb,
    const fitsbin_mapped_span_t* mapped,
    size_t mapped_count,
    size_t byte_budget,
    fitsbin_file_span_t* file_spans,
    size_t file_span_capacity,
    size_t* file_span_count,
    size_t* byte_count) {
    struct stat source;
    size_t accepted = 0U;
    size_t merged = 0U;
    size_t coalesced = 0U;
    size_t exact_bytes = 0U;
    size_t gap_budget = 0U;
    size_t added_gap_bytes = 0U;
    size_t added_gap_count = 0U;
    size_t actual_bytes = 0U;
    size_t page_size;
    size_t maximum_gap;
    size_t i;

    if (!file_span_count || !byte_count) {
        errno = EINVAL;
        return -1;
    }
    *file_span_count = 0U;
    *byte_count = 0U;
    if (!fb || !mapped || !mapped_count || !byte_budget ||
        !file_spans || mapped_count > file_span_capacity) {
        errno = mapped_count > file_span_capacity
            ? E2BIG
            : EINVAL;
        return -1;
    }
    if (fitsbin_get_open_file_stat(fb, &source)) {
        return -1;
    }
    if (source.st_size <= 0) {
        errno = EINVAL;
        return -1;
    }
    page_size = fb->mmap_page_size;
    if (!page_size) {
        long detected = sysconf(_SC_PAGESIZE);

        if (detected <= 0) {
            errno = EINVAL;
            return -1;
        }
        page_size = (size_t)detected;
    }
    if (page_size > SIZE_MAX /
            FITSBIN_PAYLOAD_QUEUE_COALESCE_GAP_PAGES) {
        errno = EOVERFLOW;
        return -1;
    }
    maximum_gap = page_size *
        FITSBIN_PAYLOAD_QUEUE_COALESCE_GAP_PAGES;

    for (i = 0U; i < mapped_count; i++) {
        const fitsbin_mapped_span_t* span = &mapped[i];
        fitsbin_chunk_t* chunk = NULL;
        uintptr_t data_begin;
        size_t data_map_offset;
        size_t span_begin_offset;
        size_t span_end_offset;
        off_t map_file_begin;
        off_t file_begin;
        off_t file_end;
        int chunk_index;

        if (span->map_end <= span->map_begin ||
            span->begin < span->map_begin ||
            span->end <= span->begin ||
            span->end > span->map_end) {
            errno = ERANGE;
            return -1;
        }
        for (chunk_index = 0;
             chunk_index < fitsbin_n_chunks(fb);
             chunk_index++) {
            fitsbin_chunk_t* candidate =
                fitsbin_get_chunk(fb, chunk_index);
            uintptr_t candidate_begin;
            uintptr_t candidate_size;

            if (!candidate || !candidate->map ||
                !candidate->data || !candidate->mapsize ||
                !candidate->data_file_size) {
                continue;
            }
            candidate_begin = (uintptr_t)candidate->map;
            candidate_size =
                span->map_end - span->map_begin;
            if (candidate_size > SIZE_MAX) {
                errno = EOVERFLOW;
                return -1;
            }
            if (candidate_begin == span->map_begin &&
                candidate->mapsize ==
                    (size_t)candidate_size) {
                chunk = candidate;
                break;
            }
        }
        if (!chunk) {
            errno = ERANGE;
            return -1;
        }
        data_begin = (uintptr_t)chunk->data;
        if (data_begin < span->map_begin ||
            data_begin > span->map_end) {
            errno = ERANGE;
            return -1;
        }
        if (chunk->data_file_offset < 0 ||
            chunk->data_file_offset > source.st_size ||
            chunk->data_file_size > (size_t)LLONG_MAX ||
            chunk->data_file_size >
                (size_t)(span->map_end - data_begin) ||
            (off_t)chunk->data_file_size >
                source.st_size - chunk->data_file_offset) {
            errno = ERANGE;
            return -1;
        }
        data_map_offset =
            (size_t)(data_begin - span->map_begin);
        if (chunk->data_file_offset < 0 ||
            data_map_offset > (size_t)LLONG_MAX ||
            chunk->data_file_offset < (off_t)data_map_offset) {
            errno = EOVERFLOW;
            return -1;
        }
        map_file_begin = chunk->data_file_offset -
            (off_t)data_map_offset;
        span_begin_offset =
            (size_t)(span->begin - span->map_begin);
        span_end_offset =
            (size_t)(span->end - span->map_begin);
        if (span_begin_offset > (size_t)LLONG_MAX ||
            span_end_offset > (size_t)LLONG_MAX ||
            map_file_begin >
                (off_t)LLONG_MAX - (off_t)span_end_offset) {
            errno = EOVERFLOW;
            return -1;
        }
        file_begin = map_file_begin +
            (off_t)span_begin_offset;
        file_end = map_file_begin +
            (off_t)span_end_offset;
        if (file_begin < 0 || file_begin >= source.st_size) {
            errno = ERANGE;
            return -1;
        }
        file_end = MIN(file_end, source.st_size);
        if (file_end <= file_begin) {
            errno = ERANGE;
            return -1;
        }
        file_spans[accepted].begin = file_begin;
        file_spans[accepted].end = file_end;
        accepted++;
    }

    qsort(file_spans,
          accepted,
          sizeof(file_spans[0]),
          fitsbin_compare_file_span);
    for (i = 0U; i < accepted; i++) {
        if (merged &&
            file_spans[i].begin <=
                file_spans[merged - 1U].end) {
            if (file_spans[i].end >
                file_spans[merged - 1U].end) {
                file_spans[merged - 1U].end =
                    file_spans[i].end;
            }
            continue;
        }
        file_spans[merged++] = file_spans[i];
    }
    for (i = 0U; i < merged; i++) {
        off_t span_length =
            file_spans[i].end - file_spans[i].begin;
        size_t span_bytes;

        if ((uintmax_t)span_length > SIZE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        span_bytes = (size_t)span_length;

        if (exact_bytes > byte_budget ||
            span_bytes > byte_budget - exact_bytes) {
            errno = E2BIG;
            return -1;
        }
        exact_bytes += span_bytes;
    }
    gap_budget = MIN(
        byte_budget - exact_bytes,
        exact_bytes /
            FITSBIN_PAYLOAD_QUEUE_COALESCE_BUDGET_DIVISOR);
    for (i = 0U; i < merged; i++) {
        if (coalesced &&
            file_spans[i].begin >
                file_spans[coalesced - 1U].end) {
            off_t gap_offset = file_spans[i].begin -
                file_spans[coalesced - 1U].end;

            if ((uintmax_t)gap_offset <= SIZE_MAX) {
                size_t gap = (size_t)gap_offset;

                if (gap <= maximum_gap &&
                    added_gap_bytes <= gap_budget &&
                    gap <= gap_budget - added_gap_bytes) {
                    file_spans[coalesced - 1U].end =
                        file_spans[i].end;
                    added_gap_bytes += gap;
                    added_gap_count++;
                    continue;
                }
            }
        }
        file_spans[coalesced++] = file_spans[i];
    }
    for (i = 0U; i < coalesced; i++) {
        off_t span_length =
            file_spans[i].end - file_spans[i].begin;
        size_t span_bytes;

        if ((uintmax_t)span_length > SIZE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        span_bytes = (size_t)span_length;

        if (actual_bytes > byte_budget ||
            span_bytes > byte_budget - actual_bytes) {
            errno = E2BIG;
            return -1;
        }
        actual_bytes += span_bytes;
    }
    if (coalesced > merged || added_gap_count > merged ||
        coalesced + added_gap_count != merged ||
        exact_bytes > SIZE_MAX - added_gap_bytes ||
        actual_bytes != exact_bytes + added_gap_bytes ||
        actual_bytes > byte_budget) {
        errno = EFAULT;
        return -1;
    }
    *file_span_count = coalesced;
    *byte_count = actual_bytes;
    return 0;
}
