#ifndef KDTREE_CONTINUATION_INTERNAL_H
#define KDTREE_CONTINUATION_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "astrometry/kdtree.h"

/*
 * The existing scalar range search uses a fixed 100-entry node stack and
 * supports at most 100 dimensions in its local scratch arrays.  The
 * continuation preserves those exact structural limits rather than creating
 * a second set of traversal bounds.
 */
#define KDTREE_RANGESEARCH_CONTINUATION_STACK_MAX 100
#define KDTREE_RANGESEARCH_CONTINUATION_DIM_MAX 100

/*
 * kdtree_rangesearch_continuation_execute() uses this sentinel for the
 * run-to-completion compatibility path.  Without a wave scheduler there is
 * no useful suspension point, so the established scalar implementation is
 * the correct zero-overhead fast path.  Bounded callers use init()/step()
 * directly with a strictly positive node budget.
 */
#define KDTREE_RANGESEARCH_CONTINUATION_RUN_TO_COMPLETION ((size_t)0)

typedef enum kdtree_rangesearch_continuation_init_status {
    KDTREE_RANGESEARCH_CONTINUATION_INIT_ERROR = -1,
    KDTREE_RANGESEARCH_CONTINUATION_INIT_OK = 0,
    KDTREE_RANGESEARCH_CONTINUATION_INIT_UNSUPPORTED = 1
} kdtree_rangesearch_continuation_init_status_t;

typedef enum kdtree_rangesearch_continuation_status {
    KDTREE_RANGESEARCH_CONTINUATION_ERROR = -1,
    KDTREE_RANGESEARCH_CONTINUATION_UNINITIALIZED = 0,
    KDTREE_RANGESEARCH_CONTINUATION_MORE = 1,
    KDTREE_RANGESEARCH_CONTINUATION_DONE = 2,
    KDTREE_RANGESEARCH_CONTINUATION_FINISHED = 3
} kdtree_rangesearch_continuation_status_t;

typedef union kdtree_rangesearch_continuation_tquery {
    double d[KDTREE_RANGESEARCH_CONTINUATION_DIM_MAX];
    float f[KDTREE_RANGESEARCH_CONTINUATION_DIM_MAX];
    uint64_t l[KDTREE_RANGESEARCH_CONTINUATION_DIM_MAX];
    uint32_t u[KDTREE_RANGESEARCH_CONTINUATION_DIM_MAX];
    uint16_t s[KDTREE_RANGESEARCH_CONTINUATION_DIM_MAX];
} kdtree_rangesearch_continuation_tquery_t;

typedef union kdtree_rangesearch_continuation_tree_scalar {
    double d;
    float f;
    uint64_t l;
    uint32_t u;
    uint16_t s;
} kdtree_rangesearch_continuation_tree_scalar_t;

/*
 * Explicit state for one split-tree scalar range search.
 *
 * Ownership:
 *   - tree and query remain caller-owned and must outlive the continuation;
 *   - result is borrowed when supplied to init and owned otherwise;
 *   - finish transfers the result back to the caller;
 *   - cleanup is valid after partial initialization, error, or abandonment.
 *
 * This state is intentionally worker/query-local.  It contains no mutex,
 * global registry, scheduler state, mmap pointer, or I/O policy.
 */
typedef struct kdtree_rangesearch_continuation {
    const kdtree_t *tree;
    const void *query;
    kdtree_qres_t *result;

    double maxd2;
    double maxdist;

    int options;
    int dimension;

    int nodestack[KDTREE_RANGESEARCH_CONTINUATION_STACK_MAX];
    int stackpos;

    int do_dists;
    int do_points;
    int use_tquery;
    int use_tsplit;

    int owns_result;
    int result_released;

    size_t nodes_visited;

    kdtree_rangesearch_continuation_tquery_t tquery;
    kdtree_rangesearch_continuation_tree_scalar_t tlinf;
    kdtree_rangesearch_continuation_status_t status;
} kdtree_rangesearch_continuation_t;

static inline void kdtree_rangesearch_continuation_zero(
    kdtree_rangesearch_continuation_t *continuation) {
    if (continuation) {
        memset(continuation, 0, sizeof(*continuation));
    }
}

static inline size_t kdtree_rangesearch_continuation_pending_nodes(
    const kdtree_rangesearch_continuation_t *continuation) {
    if (!continuation || continuation->stackpos < 0) {
        return 0;
    }

    return (size_t)continuation->stackpos + 1U;
}

/*
 * Peek a pending node in actual execution order.  offset zero is the next
 * node that step() will consume.  This accessor is read-only and will feed
 * exact page extraction in the following gate.
 */
static inline int kdtree_rangesearch_continuation_peek_node(
    const kdtree_rangesearch_continuation_t *continuation,
    size_t offset,
    int *nodeid) {
    size_t pending;

    if (!continuation || !nodeid) {
        return -1;
    }

    pending = kdtree_rangesearch_continuation_pending_nodes(continuation);

    if (offset >= pending) {
        return 0;
    }

    *nodeid = continuation->nodestack[
        continuation->stackpos - (int)offset];

    return 1;
}

static inline kdtree_rangesearch_continuation_init_status_t
kdtree_rangesearch_continuation_init(
    kdtree_rangesearch_continuation_t *continuation,
    const kdtree_t *tree,
    kdtree_qres_t *reuse,
    const void *query,
    double maxd2,
    int options) {
    if (!continuation || !tree || !query ||
        !tree->fun.rangesearch_continuation_init) {
        return KDTREE_RANGESEARCH_CONTINUATION_INIT_ERROR;
    }

    return (kdtree_rangesearch_continuation_init_status_t)
        tree->fun.rangesearch_continuation_init(
            continuation,
            tree,
            reuse,
            query,
            maxd2,
            options);
}

static inline kdtree_rangesearch_continuation_status_t
kdtree_rangesearch_continuation_step(
    kdtree_rangesearch_continuation_t *continuation,
    size_t node_budget) {
    if (!continuation || !continuation->tree ||
        !continuation->tree->fun.rangesearch_continuation_step) {
        return KDTREE_RANGESEARCH_CONTINUATION_ERROR;
    }

    return (kdtree_rangesearch_continuation_status_t)
        continuation->tree->fun.rangesearch_continuation_step(
            continuation,
            node_budget);
}

static inline kdtree_qres_t *kdtree_rangesearch_continuation_finish(
    kdtree_rangesearch_continuation_t *continuation) {
    if (!continuation || !continuation->tree ||
        !continuation->tree->fun.rangesearch_continuation_finish) {
        return NULL;
    }

    return continuation->tree->fun.rangesearch_continuation_finish(
        continuation);
}

static inline void kdtree_rangesearch_continuation_cleanup(
    kdtree_rangesearch_continuation_t *continuation) {
    if (!continuation) {
        return;
    }

    if (continuation->tree &&
        continuation->tree->fun.rangesearch_continuation_cleanup) {
        continuation->tree->fun.rangesearch_continuation_cleanup(
            continuation);
        return;
    }

    kdtree_rangesearch_continuation_zero(continuation);
}

/*
 * Bounded compatibility helper for tests and non-wave callers that explicitly
 * request resumable execution.  Production one-shot calls bypass this frame.
 */
static inline kdtree_qres_t *
kdtree_rangesearch_continuation_execute_bounded(
    const kdtree_t *tree,
    kdtree_qres_t *reuse,
    const void *query,
    double maxd2,
    int options,
    size_t node_budget) {
    kdtree_rangesearch_continuation_t continuation;
    kdtree_rangesearch_continuation_init_status_t init_status;
    kdtree_rangesearch_continuation_status_t status;
    kdtree_qres_t *result;

    if (!node_budget) {
        return NULL;
    }

    init_status = kdtree_rangesearch_continuation_init(
        &continuation,
        tree,
        reuse,
        query,
        maxd2,
        options);

    if (init_status != KDTREE_RANGESEARCH_CONTINUATION_INIT_OK) {
        kdtree_rangesearch_continuation_cleanup(&continuation);
        return NULL;
    }

    do {
        status = kdtree_rangesearch_continuation_step(
            &continuation,
            node_budget);
    } while (status == KDTREE_RANGESEARCH_CONTINUATION_MORE);

    if (status != KDTREE_RANGESEARCH_CONTINUATION_DONE) {
        kdtree_rangesearch_continuation_cleanup(&continuation);
        return NULL;
    }

    result = kdtree_rangesearch_continuation_finish(&continuation);
    kdtree_rangesearch_continuation_cleanup(&continuation);

    return result;
}

static inline kdtree_qres_t *kdtree_rangesearch_continuation_execute(
    const kdtree_t *tree,
    kdtree_qres_t *reuse,
    const void *query,
    double maxd2,
    int options,
    size_t node_budget) {
    /*
     * Keep the fast branch free of the large continuation stack object.  The
     * bounded helper owns that frame only when a caller genuinely requests
     * resumable execution.
     */
    if (node_budget ==
        KDTREE_RANGESEARCH_CONTINUATION_RUN_TO_COMPLETION) {
        return kdtree_rangesearch_options_reuse(
            tree,
            reuse,
            query,
            maxd2,
            options);
    }

    return kdtree_rangesearch_continuation_execute_bounded(
        tree,
        reuse,
        query,
        maxd2,
        options,
        node_budget);
}

#endif
