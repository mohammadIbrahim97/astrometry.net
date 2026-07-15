#ifndef KDTREE_PRODUCT_INTERNAL_H
#define KDTREE_PRODUCT_INTERNAL_H

#include "astrometry/kdtree.h"
#include "kdtree_executor_internal.h"

/*
 * Private Product-KD search entry point.
 *
 * The caller retains ownership of the executor. Every successfully accepted
 * callback must complete before this function returns.
 */
kdtree_qres_t* KDFUNC(kdtree_rangesearch_options_reuse_product)
    (const kdtree_t *kd,
     kdtree_qres_t *res,
     const void *pt,
     double maxd2,
     int options,
     const kdtree_task_executor_t *executor);

#endif
