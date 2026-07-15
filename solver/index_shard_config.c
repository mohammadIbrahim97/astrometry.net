#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

static unsigned long index_shard_config_product_interval(
    const char *value) {
  char *end = NULL;
  unsigned long interval;

  if (!value ||
      !value[0] ||
      value[0] < '0' ||
      value[0] > '9') {
    return 0;
  }

  errno = 0;
  interval = strtoul(value, &end, 10);

  if (errno ||
      !end ||
      end == value ||
      *end != '\0') {
    return 0;
  }

  return interval;
}

static void index_shard_config_initialize(void) {
  const char *mode;
  const char *trace;
  const char *worker_value;
  const char *product_interval;
  long workers = 0;

  memset(&index_shard_process_config,
         0,
         sizeof(index_shard_process_config));

  mode = getenv("ASTROMETRY_INDEX_SHARDING");

  index_shard_process_config.pthread_enabled =
      mode && !strcmp(mode, "pthread");

  trace = getenv("ASTROMETRY_INDEX_SHARD_TRACE");

  if (trace && trace[0]) {
    index_shard_process_config.trace_enabled =
        !strcmp(trace, "1") ||
        !strcasecmp(trace, "true") ||
        !strcasecmp(trace, "yes");
  }

  worker_value = getenv("ASTROMETRY_INDEX_SHARD_WORKERS");

  if (worker_value && worker_value[0]) {
    char *end = NULL;

    errno = 0;
    workers = strtol(worker_value, &end, 10);

    /*
     * Preserve the existing parser semantics: positive numeric prefixes are
     * accepted even when trailing characters exist.
     */
    if (errno || end == worker_value || workers <= 0) {
      logmsg("[index-shard] invalid "
             "ASTROMETRY_INDEX_SHARD_WORKERS=%s; using default\n",
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

  product_interval = getenv("ASTROMETRY_KD_PRODUCT_INTERVAL");

  index_shard_process_config.kd_product_interval =
      index_shard_config_product_interval(product_interval);
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
