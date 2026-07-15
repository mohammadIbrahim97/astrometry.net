#ifndef KDTREE_PHASE_A_INTERNAL_H
#define KDTREE_PHASE_A_INTERNAL_H

#include <stdint.h>

#define KDTREE_PHASE_A_HISTOGRAM_BUCKETS 10

typedef struct kdtree_phase_a_query_sample {
    uint64_t nodes_visited;
    uint64_t leaves_visited;
    uint64_t points_tested;
    uint64_t matches_found;

    uint64_t frontier_size;
    uint64_t tasks_submitted;
    uint64_t tasks_inline;

    uint64_t wall_ns;
    uint64_t cpu_ns;

    int product_used;
    int fallback_used;
} kdtree_phase_a_query_sample_t;

typedef struct kdtree_phase_a_stats {
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
} kdtree_phase_a_stats_t;

void kdtree_phase_a_reset(void);

void kdtree_phase_a_record(
    const kdtree_phase_a_query_sample_t *sample);

void kdtree_phase_a_snapshot(
    kdtree_phase_a_stats_t *stats);

#endif
