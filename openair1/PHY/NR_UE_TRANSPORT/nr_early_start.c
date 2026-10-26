/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_early_start.h"
#include <string.h>
#include <time.h>

__thread int nr_es_tls_early;
__thread int nr_es_tls_symbols_per_slot = 14;
nr_es_banks_t nr_es_banks_cells[NR_CELL_MAX];
nr_es_progress_t nr_es_progress_cells[NR_CELL_MAX];
nr_es_done_t nr_es_done_cells[NR_CELL_MAX];
nr_slot_gate_t nr_slot_gate_cells[NR_CELL_MAX] = {[0 ... NR_CELL_MAX - 1] = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .complete = -1}};

bool nr_es_banks_publish(nr_es_banks_t *b, const nr_es_cs_t *cs, int n)
{
  if (n < 0) n = 0;
  if (n > NR_ES_MAX_BANKS) n = NR_ES_MAX_BANKS;
  int same = (n == b->n);
  for (int i = 0; same && i < n; i++) same = cs[i].first_symbol == b->cs[i].first_symbol && cs[i].duration == b->cs[i].duration;
  if (same) return false; /* the common case: nothing written, the RX thread's seqlock never sees a writer */
  int exp = 0;
  while (!atomic_compare_exchange_weak(&b->wlock, &exp, 1)) exp = 0;
  atomic_fetch_add_explicit(&b->seq, 1, memory_order_acq_rel);
  b->n = n;
  memcpy(b->cs, cs, sizeof(cs[0]) * (size_t)n);
  atomic_fetch_add_explicit(&b->seq, 1, memory_order_release);
  atomic_fetch_add_explicit(&b->gen, 1, memory_order_relaxed);
  atomic_store_explicit(&b->wlock, 0, memory_order_release);
  return true;
}

bool nr_es_banks_read(const nr_es_banks_t *b, nr_es_cs_t *out, int *n)
{
  for (int tries = 0; tries < 4; tries++) {
    const uint32_t s0 = atomic_load_explicit(&b->seq, memory_order_acquire);
    if (s0 & 1) continue;
    const int nn = b->n;
    nr_es_cs_t tmp[NR_ES_MAX_BANKS];
    memcpy(tmp, b->cs, sizeof(tmp));
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&b->seq, memory_order_relaxed) == s0) {
      memcpy(out, tmp, sizeof(tmp));
      *n = nn;
      return true;
    }
  }
  return false;
}

int nr_es_plan(const nr_es_cs_t *cs, int n, int symbols_per_slot, const uint32_t *cum, uint32_t first_sym_samples,
               nr_es_point_t *pts, int max_pts)
{
  int np = 0;
  for (int i = 0; i < n && i < NR_ES_MAX_BANKS; i++) {
    if (cs[i].duration <= 0 || cs[i].first_symbol < 0) continue;
    const int se = cs[i].first_symbol + cs[i].duration;
    if (se >= symbols_per_slot) continue; /* ends with the slot: fires at slot end, as today */
    int k = 0;
    while (k < np && pts[k].sym_end < se) k++;
    if (k < np && pts[k].sym_end == se) { pts[k].mask |= 1u << i; continue; }
    if (np == max_pts) continue; /* more distinct points than the cap: the rest fires at slot end */
    memmove(&pts[k + 1], &pts[k], sizeof(pts[0]) * (size_t)(np - k));
    const uint32_t off = cum[se] > first_sym_samples ? cum[se] - first_sym_samples : 0;
    pts[k] = (nr_es_point_t){.sym_end = se, .sample_off = (int)off, .mask = 1u << i};
    np++;
  }
  return np;
}

void nr_slot_gate_init(nr_slot_gate_t *g)
{
  pthread_mutex_init(&g->mu, NULL);
  pthread_cond_init(&g->cv, NULL);
  atomic_store(&g->complete, -1);
  atomic_store(&g->waiters, 0);
}

void nr_slot_gate_complete(nr_slot_gate_t *g, long slot)
{
  atomic_store_explicit(&g->complete, slot, memory_order_release);
  if (atomic_load_explicit(&g->waiters, memory_order_acquire) == 0) return;
  pthread_mutex_lock(&g->mu); /* orders the store against a waiter between its predicate check and its wait */
  pthread_mutex_unlock(&g->mu);
  pthread_cond_broadcast(&g->cv);
}

bool nr_slot_gate_wait(nr_slot_gate_t *g, long slot, int max_ms)
{
  if (atomic_load_explicit(&g->complete, memory_order_acquire) >= slot) return true;
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_sec += max_ms / 1000;
  dl.tv_nsec += (long)(max_ms % 1000) * 1000000L;
  if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
  pthread_mutex_lock(&g->mu);
  atomic_fetch_add(&g->waiters, 1);
  bool ok = atomic_load_explicit(&g->complete, memory_order_acquire) >= slot;
  while (!ok) {
    if (pthread_cond_timedwait(&g->cv, &g->mu, &dl) != 0) {
      ok = atomic_load_explicit(&g->complete, memory_order_acquire) >= slot;
      break;
    }
    ok = atomic_load_explicit(&g->complete, memory_order_acquire) >= slot;
  }
  atomic_fetch_sub(&g->waiters, 1);
  pthread_mutex_unlock(&g->mu);
  return ok;
}
