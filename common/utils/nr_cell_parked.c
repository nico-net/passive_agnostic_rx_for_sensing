/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Parked per-cell workers (MPCI-LAZY). Lives in the utils library next to the core pool (it uses nr_pool_site_home_attr); the cell core nr_cell.c stays pool free. */
#include "nr_cell.h"
#include "nr_core_pool.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static void *nr_cell_parked_main(void *a)
{
  nr_cell_parked_t *p = a;
  pthread_mutex_lock(&p->mu);
  while (p->state == 1)
    pthread_cond_wait(&p->cv, &p->mu);
  void *(*fn)(void *) = p->fn;
  void *arg = p->arg;
  pthread_mutex_unlock(&p->mu);
  return fn(arg);
}

int nr_cell_parked_prepare(nr_cell_parked_t *p, const char *what, void *(*fn)(void *), void *arg, int cell)
{
  static pthread_mutex_t init_mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&init_mu);
  if (!p->inited) {
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->inited = 1;
  }
  pthread_mutex_unlock(&init_mu);
  pthread_mutex_lock(&p->mu);
  if (p->state != 0) {
    pthread_mutex_unlock(&p->mu);
    return 0;
  }
  p->fn = fn;
  p->arg = arg;
  p->cell = cell;
  p->what = what;
  p->state = 1;
  /* like the A11 recovery worker: a lessee's single core must not be inherited, the worker places itself after arming */
  pthread_attr_t at;
  const int have_at = pthread_attr_init(&at) == 0;
  const int homed = have_at;
  if (homed)
    (void)nr_pool_site_home_attr(&at);
  if (have_at) { /* never inherit a real-time policy of the preparing thread */
    struct sched_param sp = {0};
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    pthread_attr_setschedparam(&at, &sp);
  }
  int rc = nr_cell_thread_create(&p->th, homed ? &at : NULL, nr_cell_parked_main, p, cell);
  if (have_at)
    pthread_attr_destroy(&at);
  if (rc != 0) {
    p->state = 0;
    pthread_mutex_unlock(&p->mu);
    fprintf(stderr, "[c%d] %s: parked worker prepare failed: step=thread_create errno=%d (%s)\n", cell, what, rc, strerror(rc));
    return rc;
  }
  pthread_mutex_unlock(&p->mu);
  return 0;
}

int nr_cell_parked_arm(nr_cell_parked_t *p)
{
  if (!p->inited)
    return -1;
  pthread_mutex_lock(&p->mu);
  if (p->state != 1) {
    pthread_mutex_unlock(&p->mu);
    return -1;
  }
  p->state = 2;
  pthread_cond_broadcast(&p->cv);
  pthread_mutex_unlock(&p->mu);
  return 0;
}

pthread_t nr_cell_parked_thread(const nr_cell_parked_t *p)
{
  return p->th;
}

int nr_cell_parked_state(const nr_cell_parked_t *p)
{
  return p->inited ? p->state : 0;
}

