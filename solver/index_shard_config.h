#ifndef INDEX_SHARD_CONFIG_H
#define INDEX_SHARD_CONFIG_H

#include <stddef.h>

#include "astrometry/an-bool.h"

typedef struct index_shard_config {
  anbool pthread_enabled;
  anbool discovery_frontier_enabled;
  anbool hypothesis_parallel_enabled;
  anbool inner_lending_enabled;

  int worker_count;

  /*
   * The continuation API remains available for exact stepped traversal.
   * Production one-shot queries use the scalar fast path represented by a
   * zero node budget until coarse hypothesis tasks activate resumability.
   */
  anbool kd_continuation_enabled;
  size_t kd_continuation_node_budget;

  unsigned long kd_product_interval;
} index_shard_config_t;

/*
 * Return the immutable process configuration.
 *
 * ASTROMETRY_INDEX_SHARD_WORKERS is the only environment input. Platform
 * defaults and all internal policies are resolved exactly once.
 */
const index_shard_config_t *index_shard_config_get(void);

/*
 * Return the configured worker count capped by the number of candidate
 * indexes in the current pass.
 */
int index_shard_config_effective_workers(size_t nindexes);

#endif
