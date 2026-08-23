#ifndef KDTREE_PREFETCH_INTERNAL_H
#define KDTREE_PREFETCH_INTERNAL_H

#include <stddef.h>

#include "astrometry/kdtree.h"

/*
 * One immutable payload hint.
 *
 * mapping is an opaque mapping-owner identity. For normal index-backed
 * kdtree objects this is kd->io, currently backed by fitsbin_t.
 *
 * The emitter copies the hint before returning; the caller retains ownership.
 */
typedef struct kdtree_prefetch_hint {
    void *mapping;
    const void *address;
    size_t length;
} kdtree_prefetch_hint_t;

typedef enum kdtree_prefetch_emit_status {
    KDTREE_PREFETCH_EMIT_ERROR = -1,
    KDTREE_PREFETCH_EMIT_CONTINUE = 0,
    KDTREE_PREFETCH_EMIT_REFUSED = 1
} kdtree_prefetch_emit_status_t;

typedef enum kdtree_prefetch_prepare_status {
    KDTREE_PREFETCH_PREPARE_ERROR = -1,
    KDTREE_PREFETCH_PREPARE_NOT_APPLICABLE = 0,
    KDTREE_PREFETCH_PREPARE_COMPLETE = 1,
    KDTREE_PREFETCH_PREPARE_REFUSED = 2
} kdtree_prefetch_prepare_status_t;

typedef struct kdtree_prefetch_sink {
    void *userdata;

    int (*enabled)(void *userdata, void *mapping);

    int (*emit)(void *userdata,
                const kdtree_prefetch_hint_t *hint);
} kdtree_prefetch_sink_t;

/*
 * Mirror the scalar topology traversal and emit complete covering payload
 * hints. No query result is produced, payload is not dereferenced, and libkd
 * issues no I/O advice.
 *
 * Return:
 *  KDTREE_PREFETCH_PREPARE_COMPLETE when every emitted hint was accepted.
 *  KDTREE_PREFETCH_PREPARE_NOT_APPLICABLE when preparation is disabled.
 *  KDTREE_PREFETCH_PREPARE_REFUSED when the sink refuses a hint before full
 *  traversal completion; previously accepted hints are an incomplete prefix.
 *  KDTREE_PREFETCH_PREPARE_ERROR for invalid input or traversal failure.
 */
int kdtree_rangesearch_prefetch_prepare(
    const kdtree_t *kd,
    const void *query,
    double maxd2,
    int options,
    const kdtree_prefetch_sink_t *sink);

#endif
