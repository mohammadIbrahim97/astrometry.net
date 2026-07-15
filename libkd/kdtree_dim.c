/*
 # This file is part of libkd.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>

#include "kdtree.h"
#include "kdtree_internal.h"
#include "kdtree_product_internal.h"
#include "kdtree_phase_a_internal.h"

KD_DECLARE(kdtree_build_2, kdtree_t*, (kdtree_t* kd, void *data, int N, int D, int Nleaf, int treetype, unsigned int options, double* minval, double* maxval));

/* Build a tree from an array of data, of size N*D*sizeof(real) */
/* If the root node is level 0, then maxlevel is the level at which there may
 * not be enough points to keep the tree complete (i.e. last level) */
kdtree_t* KDFUNC(kdtree_build_2)
     (kdtree_t* kd, void *data, int N, int D, int Nleaf,
      int treetype, unsigned int options,
      double* minval, double* maxval) {

    KD_DISPATCH(kdtree_build_2, treetype, kd=, (kd, data, N, D, Nleaf, treetype, options, minval, maxval));
    return kd;
}

/* Range seach */
kdtree_qres_t* KDFUNC(kdtree_rangesearch)
     (const kdtree_t *kd, const void *pt, double maxd2) {
    return KDFUNC(kdtree_rangesearch_options_reuse)(kd, NULL, pt, maxd2, KD_OPTIONS_COMPUTE_DISTS | KD_OPTIONS_SORT_DISTS);
}

kdtree_qres_t* KDFUNC(kdtree_rangesearch_nosort)
     (const kdtree_t *kd, const void *pt, double maxd2) {
    return KDFUNC(kdtree_rangesearch_options_reuse)(kd, NULL, pt, maxd2, KD_OPTIONS_COMPUTE_DISTS);
}

kdtree_qres_t* KDFUNC(kdtree_rangesearch_options)
     (const kdtree_t *kd, const void *pt, double maxd2, int options) {
    return KDFUNC(kdtree_rangesearch_options_reuse)(kd, NULL, pt, maxd2, options);
}

KD_DECLARE(kdtree_rangesearch_options, kdtree_qres_t*, (const kdtree_t* kd, kdtree_qres_t* res, const void* pt, double maxd2, int options));

kdtree_qres_t* KDFUNC(kdtree_rangesearch_options_reuse)
     (const kdtree_t *kd, kdtree_qres_t* res, const void *pt, double maxd2, int options) {
    assert(kd->fun.rangesearch);
    return kd->fun.rangesearch(kd, res, pt, maxd2, options);
}

KD_DECLARE(kdtree_rangesearch_options_reuse_product,
           kdtree_qres_t*,
           (const kdtree_t *kd,
            kdtree_qres_t *res,
            const void *pt,
            double maxd2,
            int options,
            const kdtree_task_executor_t *executor));

kdtree_qres_t* KDFUNC(kdtree_rangesearch_options_reuse_product)
     (const kdtree_t *kd,
      kdtree_qres_t *res,
      const void *pt,
      double maxd2,
      int options,
      const kdtree_task_executor_t *executor) {
    if (!kd)
        return NULL;

    KD_DISPATCH(kdtree_rangesearch_options_reuse_product,
                kd->treetype,
                res=,
                (kd, res, pt, maxd2, options, executor));

    return res;
}
typedef struct kdtree_phase_a_atomic_stats {
    uint64_t calls;
    uint64_t product_calls;
    uint64_t fallback_calls;

    uint64_t nodes_visited;
    uint64_t leaves_visited;
    uint64_t points_tested;
    uint64_t matches_found;

    uint64_t frontier_total;
    uint64_t tasks_submitted;
    uint64_t tasks_inline;

    uint64_t wall_ns_total;
    uint64_t cpu_ns_total;

    uint64_t wall_ns_min;
    uint64_t wall_ns_max;

    uint64_t histogram[KDTREE_PHASE_A_HISTOGRAM_BUCKETS];
} kdtree_phase_a_atomic_stats_t;

static kdtree_phase_a_atomic_stats_t kdtree_phase_a_global;
static int kdtree_phase_a_histogram_bucket(uint64_t wall_ns) {
    const uint64_t us = wall_ns / UINT64_C(1000);

    if (us < UINT64_C(1)) {
        return 0;
    }

    if (us < UINT64_C(5)) {
        return 1;
    }

    if (us < UINT64_C(10)) {
        return 2;
    }

    if (us < UINT64_C(50)) {
        return 3;
    }

    if (us < UINT64_C(100)) {
        return 4;
    }

    if (us < UINT64_C(500)) {
        return 5;
    }

    if (us < UINT64_C(1000)) {
        return 6;
    }

    if (us < UINT64_C(5000)) {
        return 7;
    }

    if (us < UINT64_C(10000)) {
        return 8;
    }

    return 9;
}

static void kdtree_phase_a_atomic_min(uint64_t *target,
                                      uint64_t value) {
    uint64_t current;

    current = __atomic_load_n(target, __ATOMIC_RELAXED);

    while ((current == 0 || value < current) &&
           !__atomic_compare_exchange_n(target,
                                        &current,
                                        value,
                                        0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

static void kdtree_phase_a_atomic_max(uint64_t *target,
                                      uint64_t value) {
    uint64_t current;

    current = __atomic_load_n(target, __ATOMIC_RELAXED);

    while (value > current &&
           !__atomic_compare_exchange_n(target,
                                        &current,
                                        value,
                                        0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

void kdtree_phase_a_reset(void) {
    int i;

    __atomic_store_n(&kdtree_phase_a_global.calls,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.product_calls,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.fallback_calls,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.nodes_visited,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.leaves_visited,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.points_tested,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.matches_found,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.frontier_total,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.tasks_submitted,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.tasks_inline,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.wall_ns_total,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.cpu_ns_total,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.wall_ns_min,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kdtree_phase_a_global.wall_ns_max,
                     UINT64_C(0),
                     __ATOMIC_RELAXED);

    for (i = 0; i < KDTREE_PHASE_A_HISTOGRAM_BUCKETS; i++) {
        __atomic_store_n(&kdtree_phase_a_global.histogram[i],
                         UINT64_C(0),
                         __ATOMIC_RELAXED);
    }
}

void kdtree_phase_a_record(
    const kdtree_phase_a_query_sample_t *sample) {
    int bucket;

    if (!sample) {
        return;
    }

    __atomic_fetch_add(&kdtree_phase_a_global.calls,
                       UINT64_C(1),
                       __ATOMIC_RELAXED);

    if (sample->product_used) {
        __atomic_fetch_add(&kdtree_phase_a_global.product_calls,
                           UINT64_C(1),
                           __ATOMIC_RELAXED);
    }

    if (sample->fallback_used) {
        __atomic_fetch_add(&kdtree_phase_a_global.fallback_calls,
                           UINT64_C(1),
                           __ATOMIC_RELAXED);
    }

    __atomic_fetch_add(&kdtree_phase_a_global.nodes_visited,
                       sample->nodes_visited,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.leaves_visited,
                       sample->leaves_visited,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.points_tested,
                       sample->points_tested,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.matches_found,
                       sample->matches_found,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.frontier_total,
                       sample->frontier_size,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.tasks_submitted,
                       sample->tasks_submitted,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.tasks_inline,
                       sample->tasks_inline,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.wall_ns_total,
                       sample->wall_ns,
                       __ATOMIC_RELAXED);

    __atomic_fetch_add(&kdtree_phase_a_global.cpu_ns_total,
                       sample->cpu_ns,
                       __ATOMIC_RELAXED);

    kdtree_phase_a_atomic_min(&kdtree_phase_a_global.wall_ns_min,
                              sample->wall_ns);

    kdtree_phase_a_atomic_max(&kdtree_phase_a_global.wall_ns_max,
                              sample->wall_ns);

    bucket = kdtree_phase_a_histogram_bucket(sample->wall_ns);

    __atomic_fetch_add(&kdtree_phase_a_global.histogram[bucket],
                       UINT64_C(1),
                       __ATOMIC_RELAXED);
}

void kdtree_phase_a_snapshot(kdtree_phase_a_stats_t *stats) {
    int i;

    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    stats->calls =
        __atomic_load_n(&kdtree_phase_a_global.calls,
                        __ATOMIC_RELAXED);

    stats->product_calls =
        __atomic_load_n(&kdtree_phase_a_global.product_calls,
                        __ATOMIC_RELAXED);

    stats->fallback_calls =
        __atomic_load_n(&kdtree_phase_a_global.fallback_calls,
                        __ATOMIC_RELAXED);

    stats->nodes_visited =
        __atomic_load_n(&kdtree_phase_a_global.nodes_visited,
                        __ATOMIC_RELAXED);

    stats->leaves_visited =
        __atomic_load_n(&kdtree_phase_a_global.leaves_visited,
                        __ATOMIC_RELAXED);

    stats->points_tested =
        __atomic_load_n(&kdtree_phase_a_global.points_tested,
                        __ATOMIC_RELAXED);

    stats->matches_found =
        __atomic_load_n(&kdtree_phase_a_global.matches_found,
                        __ATOMIC_RELAXED);

    stats->frontier_total =
        __atomic_load_n(&kdtree_phase_a_global.frontier_total,
                        __ATOMIC_RELAXED);

    stats->tasks_submitted =
        __atomic_load_n(&kdtree_phase_a_global.tasks_submitted,
                        __ATOMIC_RELAXED);

    stats->tasks_inline =
        __atomic_load_n(&kdtree_phase_a_global.tasks_inline,
                        __ATOMIC_RELAXED);

    stats->wall_ns_total =
        __atomic_load_n(&kdtree_phase_a_global.wall_ns_total,
                        __ATOMIC_RELAXED);

    stats->cpu_ns_total =
        __atomic_load_n(&kdtree_phase_a_global.cpu_ns_total,
                        __ATOMIC_RELAXED);

    stats->wall_ns_min =
        __atomic_load_n(&kdtree_phase_a_global.wall_ns_min,
                        __ATOMIC_RELAXED);

    stats->wall_ns_max =
        __atomic_load_n(&kdtree_phase_a_global.wall_ns_max,
                        __ATOMIC_RELAXED);

    for (i = 0; i < KDTREE_PHASE_A_HISTOGRAM_BUCKETS; i++) {
        stats->histogram[i] =
            __atomic_load_n(&kdtree_phase_a_global.histogram[i],
                            __ATOMIC_RELAXED);
    }
}
