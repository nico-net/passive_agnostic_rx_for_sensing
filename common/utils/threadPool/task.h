/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef TASK_WORK_STEALING_THREAD_POOL_H
#define TASK_WORK_STEALING_THREAD_POOL_H

typedef struct {
  void* args;
  void (*func)(void* args);
  int cell; /* mpci audit: cell index of the thread that pushed the task (stamped by pushTpool); the worker binds itself to it while it runs the task. 0 for every single-cell caller */
} task_t;

#endif
