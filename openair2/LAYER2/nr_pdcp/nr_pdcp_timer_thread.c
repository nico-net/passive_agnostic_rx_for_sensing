/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#define _GNU_SOURCE
#include "nr_pdcp_timer_thread.h"

#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>

#include "LOG/log.h"

static pthread_mutex_t   timer_thread_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    timer_thread_cond    = PTHREAD_COND_INITIALIZER;
static volatile uint64_t timer_thread_curtime = 0;
static bool timer_stop, timer_started;
static pthread_t timer_thread;

static void *nr_pdcp_timer_thread(void *_nr_pdcp_ue_manager)
{
  pthread_setname_np(pthread_self(),"pdcp_timer");
  nr_pdcp_ue_manager_t *nr_pdcp_ue_manager = (nr_pdcp_ue_manager_t *)_nr_pdcp_ue_manager;
  nr_pdcp_ue_t         **ue_list;
  int                  ue_count;
  int                  i;
  int                  j;
  uint64_t             curtime = 0;

  while (1) {
    if (pthread_mutex_lock(&timer_thread_mutex) != 0) abort();
    while (curtime == timer_thread_curtime && !timer_stop)
      if (pthread_cond_wait(&timer_thread_cond, &timer_thread_mutex) != 0) abort();
    if (timer_stop) {
      pthread_mutex_unlock(&timer_thread_mutex);
      break;
    }
    curtime = timer_thread_curtime;
    if (pthread_mutex_unlock(&timer_thread_mutex) != 0) abort();

    nr_pdcp_manager_lock(nr_pdcp_ue_manager);

    ue_list  = nr_pdcp_manager_get_ue_list(nr_pdcp_ue_manager);
    ue_count = nr_pdcp_manager_get_ue_count(nr_pdcp_ue_manager);
    for (i = 0; i < ue_count; i++) {
      for (j = 0; j < 2; j++) {
        if (ue_list[i]->srb[j] != NULL)
          ue_list[i]->srb[j]->set_time(ue_list[i]->srb[j], curtime);
      }
      for (j = 0; j < 5; j++) {
        if (ue_list[i]->drb[j] != NULL)
          ue_list[i]->drb[j]->set_time(ue_list[i]->drb[j], curtime);
      }
    }

    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  }

  return NULL;
}

void nr_pdcp_init_timer_thread(nr_pdcp_ue_manager_t *nr_pdcp_ue_manager)
{
  if (pthread_create(&timer_thread, NULL, nr_pdcp_timer_thread, nr_pdcp_ue_manager) != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
  timer_started = true;
}

void nr_pdcp_stop_timer_thread(void)
{
  if (!timer_started)
    return;
  pthread_mutex_lock(&timer_thread_mutex);
  timer_stop = true;
  pthread_cond_broadcast(&timer_thread_cond);
  pthread_mutex_unlock(&timer_thread_mutex);
  pthread_join(timer_thread, NULL);
  timer_started = false;
}

void nr_pdcp_ms_tick(void)
{
  if (pthread_mutex_lock(&timer_thread_mutex)) abort();
  timer_thread_curtime++;
  if (pthread_cond_broadcast(&timer_thread_cond)) abort();
  if (pthread_mutex_unlock(&timer_thread_mutex)) abort();
}
