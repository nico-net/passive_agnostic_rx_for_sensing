/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "ru_park.h"
#include "shm_iq_ring.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

_Static_assert(RU_PARK_PAUSED == SHM_IQ_READ_PAUSED, "ru_park and shm_iq must agree on the paused return");

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static atomic_int g_parked[RU_PARK_MAX_CELLS];
static atomic_ulong g_waits[RU_PARK_MAX_CELLS];
static unsigned long g_wake_seq; /* guarded by g_mu */

static int slot_of(int cell) { return cell >= 0 && cell < RU_PARK_MAX_CELLS ? cell : 0; }

int ru_park_read(const ru_park_gate_t *g, int cell, int (*read_once)(void *), void *arg)
{
  const int c = slot_of(cell);
  for (;;) {
    if (!g->enter(g->ctx, 0)) {
      atomic_store(&g_parked[c], 0);
      return -1;
    }
    const int r = read_once(arg);
    g->leave(g->ctx);
    if (r != RU_PARK_PAUSED) {
      atomic_store(&g_parked[c], 0);
      return r;
    }
    if (g->stopping(g->ctx)) {
      atomic_store(&g_parked[c], 0);
      return -1;
    }
    atomic_store(&g_parked[c], 1);
    atomic_fetch_add(&g_waits[c], 1);
    /* parked outside the gate: wait for a wake or the poll period (a missed wake costs at most one period) */
    pthread_mutex_lock(&g_mu);
    const unsigned long seq = g_wake_seq;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (long)RU_PARK_POLL_MS * 1000000L;
    t.tv_sec += t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    while (seq == g_wake_seq && pthread_cond_timedwait(&g_cv, &g_mu, &t) == 0) {
    }
    pthread_mutex_unlock(&g_mu);
  }
}

void ru_park_wake(void)
{
  pthread_mutex_lock(&g_mu);
  g_wake_seq++;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
}

int ru_park_parked(int cell) { return atomic_load(&g_parked[slot_of(cell)]); }
unsigned long ru_park_waits(int cell) { return atomic_load(&g_waits[slot_of(cell)]); }
void ru_park_reset(void)
{
  for (int i = 0; i < RU_PARK_MAX_CELLS; i++) {
    atomic_store(&g_parked[i], 0);
    atomic_store(&g_waits[i], 0);
  }
}

static atomic_int g_tune_pending[RU_PARK_MAX_CELLS];
static ru_tune_t g_tune[RU_PARK_MAX_CELLS];
static pthread_mutex_t g_tune_mu = PTHREAD_MUTEX_INITIALIZER;

void ru_tune_post(int cell, const ru_tune_t *t)
{
  const int c = slot_of(cell);
  pthread_mutex_lock(&g_tune_mu);
  g_tune[c] = *t;
  atomic_store(&g_tune_pending[c], 1);
  pthread_mutex_unlock(&g_tune_mu);
}

int ru_tune_take(int cell, ru_tune_t *t)
{
  const int c = slot_of(cell);
  if (!atomic_load_explicit(&g_tune_pending[c], memory_order_acquire)) return 0;
  pthread_mutex_lock(&g_tune_mu);
  const int had = atomic_exchange(&g_tune_pending[c], 0);
  if (had) *t = g_tune[c];
  pthread_mutex_unlock(&g_tune_mu);
  return had;
}

static ru_cell_hook_t g_hooks[RU_CELL_HOOKS_MAX];
static int g_nhooks;
static pthread_mutex_t g_hook_mu = PTHREAD_MUTEX_INITIALIZER;

int ru_cell_hook_register(const ru_cell_hook_t *h)
{
  int rc = -1;
  pthread_mutex_lock(&g_hook_mu);
  if (g_nhooks < RU_CELL_HOOKS_MAX) {
    g_hooks[g_nhooks++] = *h;
    rc = 0;
  }
  pthread_mutex_unlock(&g_hook_mu);
  return rc;
}
void ru_cell_quiesce(int cell)
{
  for (int i = 0; i < g_nhooks; i++)
    if (g_hooks[i].quiesce) g_hooks[i].quiesce(g_hooks[i].ctx, cell);
}
void ru_cell_resume(int cell)
{
  for (int i = g_nhooks - 1; i >= 0; i--)
    if (g_hooks[i].resume) g_hooks[i].resume(g_hooks[i].ctx, cell);
}
