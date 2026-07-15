#ifndef ASTROMETRY_INDEX_SHARD_H
#define ASTROMETRY_INDEX_SHARD_H

#include "astrometry/an-bool.h"
#include "astrometry/onefield.h"
#include "astrometry/solver.h"

/*
 * Solver-facing index-shard lifecycle.
 *
 * This interface deliberately excludes worker, queue, reducer, auxiliary-task,
 * and Product-KD implementation details.
 */
anbool index_shard_pthread_enabled(void);

int index_shard_pool_start(onefield_t *bp, solver_t *sp);
void index_shard_pool_stop(onefield_t *bp);
int index_shard_pool_active(onefield_t *bp);

void index_shard_poll_from_callback(onefield_t *bp);

#endif
