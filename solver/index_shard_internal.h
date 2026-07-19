#ifndef ASTROMETRY_INDEX_SHARD_INTERNAL_H
#define ASTROMETRY_INDEX_SHARD_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "astrometry/bl.h"
#include "astrometry/index.h"
#include "astrometry/index_shard.h"
#include "kdtree_executor_internal.h"
#include "kdtree_prefetch_internal.h"
/*
 * Terminal status and ownership contract.
 *
 * INDEX_SHARD_SOLVE_LIFECYCLE_CONFLICT:
 *   The requested pass could not acquire the pool lifecycle. No serial
 *   fallback is permitted because a competing or incompatible execution
 *   context may still own the pool.
 *
 * INDEX_SHARD_SOLVE_TERMINAL_FAILURE:
 *   Execution failed after master-visible state may have been mutated.
 *   Serial fallback is forbidden.
 *
 * INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE:
 *   Execution failed before any worker result was transferred into
 *   master-visible state. Serial fallback is permitted.
 *
 * INDEX_SHARD_SOLVE_HANDLED:
 *   The parallel pass completed normally, either solved or unsolved.
 *
 * INDEX_SHARD_SOLVE_UNAVAILABLE:
 *   The parallel path was not active or available. The original serial
 *   path may be used.
 *
 * Ownership:
 *
 * - Workers own private result slots during computation.
 * - Workers never commit externally visible solver state.
 * - The reducer is the only writer of master-visible solution state.
 * - Losing worker products are destroyed during result disposal.
 * - Winning products transfer ownership exactly once during reduction.
 * - master_committed is published before any potentially mutating merge.
 * - Once master_committed is true, serial fallback is permanently forbidden
 *   for that pass.
 */
typedef enum index_shard_solve_status {
  INDEX_SHARD_SOLVE_LIFECYCLE_CONFLICT = -3,
  INDEX_SHARD_SOLVE_TERMINAL_FAILURE = -2,
  INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE = -1,
  INDEX_SHARD_SOLVE_HANDLED = 0,
  INDEX_SHARD_SOLVE_UNAVAILABLE = 1
} index_shard_solve_status_t;

typedef struct index_shard_hooks {
  index_t *(*get_index)(onefield_t *bp, size_t index_order);
  void (*done_with_index)(onefield_t *bp,
                          size_t index_order,
                          index_t *index);
  const char *(*get_index_name)(onefield_t *bp,
                                size_t index_order);

  /*
   * Worker-local context lifecycle.
   *
   * prepare_local_context() runs once per worker per submitted pass.
   * reset_local_context_for_task() runs before every one-index solve.
   * cleanup_local_context() runs once when the worker finishes the pass.
   */
  int (*prepare_local_context)(onefield_t *local_bp,
                               onefield_t *master_bp,
                               const solver_t *base_sp);

  void (*reset_local_context_for_task)(onefield_t *local_bp,
                                       bl *local_solutions);

  void (*cleanup_local_context)(onefield_t *local_bp);

  int (*solve_one_index)(onefield_t *local_bp, index_t *index);

  anbool (*analyze_solutions)(onefield_t *master_bp,
                              bl *solutions,
                              double *best_logodds,
                              int *best_fieldnum);

  int (*merge_solutions)(onefield_t *master_bp,
                         bl *solutions,
                         anbool *solved_out);

  void (*free_solutions)(bl *solutions);
} index_shard_hooks_t;

anbool index_shard_trace_enabled(void);

index_shard_solve_status_t
index_shard_solve(onefield_t *bp,
                  solver_t *base_sp,
                  size_t nindexes,
                  const index_shard_hooks_t *hooks);

/*
 * Private auxiliary executor contract.
 *
 * Groups own completion accounting for one Product-KD fork/join operation.
 * The shared index-shard worker pool owns accepted auxiliary tasks.
 */
typedef void (*index_shard_aux_task_fn)(void *userdata);

typedef struct index_shard_aux_group index_shard_aux_group_t;

index_shard_aux_group_t *index_shard_aux_group_new(void);
void index_shard_aux_group_free(index_shard_aux_group_t *group);

int index_shard_kdtree_executor_init(kdtree_task_executor_t *executor,
                                     index_shard_aux_group_t *group);

int index_shard_aux_available(void);

/*
 * Snapshot helper capacity for the current shard worker. Suggested subtasks
 * excludes the caller and represents either naturally spare configured
 * workers or, for a sufficiently large hypothesis wave, one worker eligible
 * to help at its next outer-task boundary. The group-aware executor performs
 * the final atomic reservation before dispatch.
 */
int index_shard_aux_capacity(kdtree_task_capacity_t *capacity);
/*
 * One batch-local session connecting a solver worker to the pool-shared
 * prefetch coordinator.
 *
 * pool is deliberately opaque outside index_shard.c.
 */
#define INDEX_SHARD_PREFETCH_LOCAL_PAGE_CAPACITY 64

typedef struct index_shard_prefetch_local_page {
  void *mapping;
  const void *map_base;

  uintptr_t page;
  size_t page_size;

  unsigned int priority;
  kdtree_prefetch_array_kind_t kind;
} index_shard_prefetch_local_page_t;

/*
 * Batch-local staging area.
 *
 * KD hint generation writes here without taking the shared coordinator
 * mutex. The batch is published to the pool coordinator only at the normal
 * sink-flush boundary.
 */
typedef struct index_shard_prefetch_session {
  void *pool;
  unsigned long generation;

  index_shard_prefetch_local_page_t
      pages[INDEX_SHARD_PREFETCH_LOCAL_PAGE_CAPACITY];

  size_t page_count;

  /*
   * Sticky across local publication. Cleared only after the coordinator gets
   * an opportunity to process the accumulated threshold.
   */
  int issue_requested;

  unsigned long long hints_emitted;
  unsigned long long hints_stale;
  unsigned long long hints_unmapped;

  unsigned long long pages_raw;
  unsigned long long pages_local_duplicate;
} index_shard_prefetch_session_t;
/*
 * Initializes a generic libkd prefetch sink backed by the current shard pool.
 *
 * Returns -1 when called outside an active shard worker or pass.
 */
int index_shard_kdtree_prefetch_sink_init(
    kdtree_prefetch_sink_t *sink,
    index_shard_prefetch_session_t *session);

#endif
