#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "astrometry/log.h"

#include "index_shard_config.h"

static pthread_once_t index_shard_config_once = PTHREAD_ONCE_INIT;
static index_shard_config_t index_shard_process_config;

static long index_shard_config_default_workers(void) {
  long workers;

#ifdef _SC_NPROCESSORS_ONLN
  workers = sysconf(_SC_NPROCESSORS_ONLN);
#else
  workers = 2;
#endif

  if (workers <= 0) {
    workers = 2;
  }

  if (workers > INT_MAX) {
    workers = INT_MAX;
  }

  return workers;
}

static void index_shard_config_initialize(void) {
  const char *worker_value;
  long workers = 0;

  memset(&index_shard_process_config,
         0,
         sizeof(index_shard_process_config));

  /*
   * Production UX exposes exactly one control: the total worker budget.
   * Every algorithmic policy below is resolved internally and is therefore
   * identical across command lines, scripts and benchmark environments.
   */
  worker_value = getenv("ASTROMETRY_INDEX_SHARD_WORKERS");

  if (worker_value && worker_value[0]) {
    char *end = NULL;

    errno = 0;
    workers = strtol(worker_value, &end, 10);

    if (errno ||
        end == worker_value ||
        *end != '\0' ||
        workers <= 0) {
      logmsg("[index-shard] invalid "
             "ASTROMETRY_INDEX_SHARD_WORKERS=%s; using automatic default\n",
             worker_value);
      workers = 0;
    }
  }

  if (workers <= 0) {
    workers = index_shard_config_default_workers();
  }

  if (workers > INT_MAX) {
    workers = INT_MAX;
  }

  index_shard_process_config.worker_count = (int)workers;

  /* One worker uses the original serial execution path automatically. */
  index_shard_process_config.pthread_enabled = workers > 1;

  /* Preserve the currently validated production policies. */
  index_shard_process_config.discovery_frontier_enabled = FALSE;
  index_shard_process_config.hypothesis_parallel_enabled = TRUE;
  index_shard_process_config.inner_lending_enabled = TRUE;

  /*
   * One-shot CodeKD callers retain the scalar run-to-completion fast path.
   * The continuation API remains internally available with a zero budget.
   */
  index_shard_process_config.kd_continuation_enabled = TRUE;
  index_shard_process_config.kd_continuation_node_budget = 0;

  /* Product sampling remains disabled in the production policy. */
  index_shard_process_config.kd_product_interval = 0;
}

const index_shard_config_t *index_shard_config_get(void) {
  pthread_once(&index_shard_config_once,
               index_shard_config_initialize);

  return &index_shard_process_config;
}

int index_shard_config_effective_workers(size_t nindexes) {
  const index_shard_config_t *config;
  int workers;

  config = index_shard_config_get();
  workers = config->worker_count;

  if (nindexes && (size_t)workers > nindexes) {
    workers = (int)nindexes;
  }

  if (workers < 1) {
    workers = 1;
  }

  return workers;
}
