/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */
#ifndef SOLVER_HYPOTHESIS_INTERNAL_H
#define SOLVER_HYPOTHESIS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "astrometry/an-bool.h"
#include "astrometry/kdtree.h"
#include "astrometry/solver.h"

/*
 * Keep a short ordered owner prefix for first-hit latency. Helper ranges must
 * contain enough independent CodeKD hypotheses to amortize unified-pool
 * dispatch. These are architectural constants, not user-facing tuning.
 */
#define SOLVER_HYPOTHESIS_OWNER_PREFIX 8U
#define SOLVER_HYPOTHESIS_MIN_TASK_GRAIN 32U

/*
 * The bounded wave limits speculative look-ahead. Each helper receives a
 * coarse contiguous range, never one dispatch per CodeKD query.
 */
#define SOLVER_HYPOTHESIS_MIN_PARALLEL_BATCH 40U
#define SOLVER_HYPOTHESIS_MAX_PARALLEL_BATCH 512U

typedef struct solver_hypothesis_descriptor {
    double code[DCMAX];
    int stars[DQMAX];

    uint64_t sequence;
    double tol2;
    int dimquad;
    int quads_tried;
    anbool current_parity;
} solver_hypothesis_descriptor_t;

typedef enum solver_hypothesis_result_state {
    SOLVER_HYPOTHESIS_RESULT_EMPTY = 0,
    SOLVER_HYPOTHESIS_RESULT_READY,
    SOLVER_HYPOTHESIS_RESULT_REDUCED
} solver_hypothesis_result_state_t;

typedef enum solver_hypothesis_context_state {
    SOLVER_HYPOTHESIS_CONTEXT_IDLE = 0,
    SOLVER_HYPOTHESIS_CONTEXT_BUILDING,
    SOLVER_HYPOTHESIS_CONTEXT_FROZEN,
    SOLVER_HYPOTHESIS_CONTEXT_EXECUTING,
    SOLVER_HYPOTHESIS_CONTEXT_COMPLETE,
    SOLVER_HYPOTHESIS_CONTEXT_STOPPED,
    SOLVER_HYPOTHESIS_CONTEXT_FAILED
} solver_hypothesis_context_state_t;

typedef struct solver_hypothesis_result_slot {
    kdtree_qres_t storage;
    kdtree_qres_t* result;
    uint64_t sequence;
    unsigned long generation;
    solver_hypothesis_result_state_t state;
} solver_hypothesis_result_slot_t;

typedef struct solver_hypothesis_task_range {
    size_t begin;
    size_t end;
    uint64_t first_sequence;
    uint64_t last_sequence;
    unsigned long generation;
} solver_hypothesis_task_range_t;

typedef struct solver_hypothesis_metrics {
    unsigned long long batches;
    unsigned long long batches_completed;
    unsigned long long batches_stopped;
    unsigned long long batches_failed;
    unsigned long long hypotheses_generated;
    unsigned long long hypotheses_executed;
    unsigned long long hypotheses_reduced;
    unsigned long long task_ranges_planned;
    unsigned long long task_ranges_executed;
    unsigned long long task_ranges_submitted;
    unsigned long long task_ranges_inline;
    unsigned long long parallel_batches;
    unsigned long long parallel_batches_observed;
    unsigned long long parallel_hypotheses;
    unsigned long long allocation_failures;
    unsigned long long search_failures;

    size_t max_batch_hypotheses;
    size_t max_task_ranges;
    size_t max_parallel_ranges;
} solver_hypothesis_metrics_t;

/*
 * Lifetime and ownership contract
 * -------------------------------
 *
 * The context belongs to one solver_run() invocation.  A batch is bound to
 * the currently active index before descriptors are appended. Descriptors,
 * task ranges, and result slots are context-owned and never tied to a pthread
 * worker identity. Coarse ranges can therefore move between available shard
 * workers while one owner retains ordered reduction for each index solve.
 *
 * Task parallelism is represented by immutable contiguous task ranges.
 * Data parallelism is represented by the contiguous descriptor/result arrays
 * owned by each range. Helpers perform immutable CodeKD search only; the
 * owner reduces READY slots strictly in generation order.
 */
typedef struct solver_hypothesis_context {
    solver_t* solver;
    const index_t* bound_index;

    solver_hypothesis_descriptor_t* descriptors;
    solver_hypothesis_result_slot_t* results;
    solver_hypothesis_task_range_t* tasks;

    size_t count;
    size_t capacity;
    size_t task_count;
    size_t task_capacity;
    size_t next_reduce;
    size_t batch_target;

    anbool parallel_candidate;
    anbool parallel_execution;

    uint64_t next_sequence;
    uint64_t batch_first_sequence;
    unsigned long batch_generation;
    solver_hypothesis_context_state_t state;

    solver_hypothesis_metrics_t metrics;
} solver_hypothesis_context_t;

#endif
