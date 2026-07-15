#ifndef INDEX_SHARD_CONFIG_H
#define INDEX_SHARD_CONFIG_H

#include <stddef.h>

#include "astrometry/an-bool.h"

typedef struct index_shard_config {
  anbool pthread_enabled;
  anbool trace_enabled;

  int worker_count;

  unsigned long kd_product_interval;
} index_shard_config_t;

/*
 * Return the immutable process configuration.
 *
 * Environment and platform defaults are resolved exactly once.
 */
const index_shard_config_t *index_shard_config_get(void);

/*
 * Return the configured worker count capped by the number of candidate
 * indexes in the current pass.
 */
int index_shard_config_effective_workers(size_t nindexes);

#endif
