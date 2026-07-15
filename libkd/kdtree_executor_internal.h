#ifndef KDTREE_EXECUTOR_INTERNAL_H
#define KDTREE_EXECUTOR_INTERNAL_H

#include <stddef.h>

typedef void (*kdtree_task_fn)(void *task_userdata);

typedef struct kdtree_task_capacity {
  size_t workers_total;
  size_t aux_pending;
  size_t aux_room;
  size_t suggested_subtasks;
} kdtree_task_capacity_t;

typedef struct kdtree_task_executor {
  void *userdata;

  int (*submit)(void *userdata,
                kdtree_task_fn fn,
                void *task_userdata);

  int (*wait)(void *userdata);

  int (*capacity)(void *userdata,
                  kdtree_task_capacity_t *capacity);
} kdtree_task_executor_t;

#endif
