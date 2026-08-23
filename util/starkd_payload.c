/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "starkd.h"
#include "starkd_internal.h"
#include "fitsbin_internal.h"
#include "kdtree.h"
#include "kdtree_fits_io.h"
#include "starutil.h"
#include "fitsbin.h"
#include "fitstable.h"
#include "errors.h"
#include "tic.h"
#include "log.h"
#include "ioutils.h"
#include "fitsioutils.h"

typedef struct startree_star_plan {
    fitsbin_prefetch_range_t* ranges;
    size_t capacity;
    size_t count;
    size_t byte_budget;
} startree_star_plan_t;

static int startree_plan_stars(
    const startree_t* s,
    const unsigned int* starids,
    int nstars,
    startree_star_plan_t* plan) {
    size_t row_size;
    int ndata;
    int i;

    if (!plan || !plan->ranges || !plan->capacity ||
        !s) {
        errno = EINVAL;
        return -1;
    }
    plan->count = 0U;
    plan->byte_budget = 0U;
    if (!s || !s->tree || !starids || nstars <= 0 ||
        !s->tree->io || !s->tree->io_is_fitsbin ||
        !s->tree->data.any) {
        return 0;
    }
    ndata = startree_data_count_internal(s);
    if (ndata <= 0) {
        return 0;
    }
    if ((size_t)nstars > plan->capacity) {
        errno = E2BIG;
        return -1;
    }
    if (s->tree->perm && !s->inverse_perm) {
        errno = EAGAIN;
        return 0;
    }
    row_size = kdtree_sizeof_data(s->tree) / (size_t)ndata;
    if (fitsbin_prefetch_row_budget(
            s->tree->io,
            row_size,
            (size_t)nstars,
            &plan->byte_budget)) {
        return -1;
    }
    for (i = 0; i < nstars; i++) {
        unsigned int starid = starids[i];
        int data_index;

        if (starid >= (unsigned int)ndata) {
            errno = EINVAL;
            return -1;
        }
        data_index = s->inverse_perm
            ? s->inverse_perm[starid]
            : (int)starid;
        if (data_index < 0 || data_index >= ndata) {
            errno = EINVAL;
            return -1;
        }
        plan->ranges[i].data =
            kdtree_get_data(s->tree, data_index);
        plan->ranges[i].size = row_size;
    }
    plan->count = (size_t)nstars;
    return 1;
}

int startree_prefetch_stars_ready_submit(
    const startree_t* s,
    const unsigned int* starids,
    int nstars,
    fitsbin_payload_io_ticket_t** ticket) {
    fitsbin_prefetch_range_t
        ranges[FITSBIN_PREFETCH_RANGE_LIMIT];
    startree_star_plan_t plan = {
        .ranges = ranges,
        .capacity = sizeof(ranges) / sizeof(ranges[0]),
    };
    int status;

    if (!ticket) {
        errno = EINVAL;
        return -1;
    }
    *ticket = NULL;
    status = startree_plan_stars(s, starids, nstars, &plan);
    if (status <= 0) {
        return status;
    }
    return fitsbin_prefetch_ranges_submit(
        s->tree->io,
        plan.ranges,
        plan.count,
        plan.byte_budget,
        ticket);
}

int startree_get_ready(
    const startree_t* s,
    int starid,
    double* posn) {
    int data_index;
    int ndata;

    if (!s || !s->tree || !posn) {
        errno = EINVAL;
        return -1;
    }
    ndata = startree_data_count_internal(s);
    if (starid < 0 || starid >= ndata) {
        errno = EINVAL;
        return -1;
    }
    if (s->tree->perm && !s->inverse_perm) {
        errno = EAGAIN;
        return -1;
    }
    data_index = s->inverse_perm
        ? s->inverse_perm[starid]
        : starid;
    if (data_index < 0 || data_index >= ndata) {
        errno = EINVAL;
        return -1;
    }
    kdtree_copy_data_double(s->tree, data_index, 1, posn);
    return 0;
}
