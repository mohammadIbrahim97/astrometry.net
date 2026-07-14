#ifndef ASTROMETRY_INDEX_SHARD_H
#define ASTROMETRY_INDEX_SHARD_H

#include <stddef.h>

#include "astrometry/bl.h"
#include "astrometry/index.h"
#include "astrometry/onefield.h"
#include "astrometry/solver.h"
#include "astrometry/an-bool.h"
#include "astrometry/kdtree_executor.h"

/*
 * Result of one index-shard solve pass.
 *
 * A serial fallback is safe only when no worker result has been transferred
 * into master-visible onefield state.
 */
typedef enum index_shard_solve_status {
  INDEX_SHARD_SOLVE_TERMINAL_FAILURE = -2,
  INDEX_SHARD_SOLVE_PRECOMMIT_FAILURE = -1,
  INDEX_SHARD_SOLVE_HANDLED = 0,
  INDEX_SHARD_SOLVE_UNAVAILABLE = 1
} index_shard_solve_status_t;


typedef struct index_shard_hooks {
  index_t *(*get_index)(onefield_t *bp, size_t index_order);
  void (*done_with_index)(onefield_t *bp, size_t index_order, index_t *index);

  /*
     * Worker-local context lifecycle.
     * prepare_local_context() is called once per worker per submitted pass.
     * reset_local_context_for_task() is called before every one-index solve.
     * cleanup_local_context() is called once when the worker finishes the pass.
     */
  int (*prepare_local_context)(onefield_t *local_bp, onefield_t *master_bp,
                               const solver_t *base_sp);

  void (*reset_local_context_for_task)(onefield_t *local_bp, bl *local_solutions);

  void (*cleanup_local_context)(onefield_t *local_bp);

  int (*solve_one_index)(onefield_t *local_bp, index_t *index);

  anbool (*analyze_solutions)(onefield_t *master_bp, bl *solutions, double *best_logodds,
                              int *best_fieldnum);

  int (*merge_solutions)(onefield_t *master_bp, bl *solutions, anbool *solved_out);

  void (*free_solutions)(bl *solutions);
} index_shard_hooks_t;

anbool index_shard_pthread_enabled(void);
anbool index_shard_trace_enabled(void);

int index_shard_pool_start(onefield_t *bp, solver_t *sp);
void index_shard_pool_stop(onefield_t *bp);
int index_shard_pool_active(onefield_t *bp);

index_shard_solve_status_t index_shard_solve(onefield_t *bp,
                  solver_t *base_sp,
                  size_t nindexes,
                  const index_shard_hooks_t *hooks);

void index_shard_poll_from_callback(onefield_t *bp);
typedef void (*index_shard_aux_task_fn)(void *userdata);

typedef struct index_shard_aux_group index_shard_aux_group_t;

index_shard_aux_group_t *index_shard_aux_group_new(void);
void index_shard_aux_group_free(index_shard_aux_group_t *group);
int index_shard_kdtree_executor_init(kdtree_task_executor_t *executor,
                                     index_shard_aux_group_t *group);
int index_shard_aux_available(void);
#endif
