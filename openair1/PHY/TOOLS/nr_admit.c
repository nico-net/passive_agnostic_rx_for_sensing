/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_admit.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Per-class counters, one row per cell (index = nr_cell_idx() of the calling thread; weak hook so a binary without the cell module counts in row 0). */
#define ADMIT_CELLS 16 /* = NR_CELL_MAX (common/utils/nr_cell.h); utils is not included here, the hook returns an index that is clamped */
extern int nr_cell_idx_fn(void) __attribute__((weak));
extern int nr_isac_mode(void) __attribute__((weak)); /* 0 = legacy */
static struct { uint64_t att, skip, rec, exp; } g_cls[ADMIT_CELLS][NR_ADMIT_N];
static int cur_cell(void)
{
  const int k = nr_cell_idx_fn ? nr_cell_idx_fn() : 0;
  return k >= 0 && k < ADMIT_CELLS ? k : 0;
}
#define CNT(f, c) __atomic_fetch_add(&g_cls[cur_cell()][c].f, 1, __ATOMIC_RELAXED)
#define LD(f, c) __atomic_load_n(&g_cls[cur_cell()][c].f, __ATOMIC_RELAXED)

/* Admission of background (speculative / discovery) work. mpci B6: the gate is the value order of the core pool under overload (common/utils/nr_core_pool.h,
 * SHED). With one cell, or while the pool has a free token or no urgent waiter, everything runs and nothing is counted: the behaviour of before. While the
 * pool serves >= 2 active cells and DECODE / SCAN / RETRY waiters have no token to take, the background classes of the lowest tiers are skipped (the callers
 * already handle a skip: "the next one is fresher", the data stays in the producer's ring). No flag; the decision is a function of the pool state only.
 * The pool is reached through weak symbols: a binary without the core pool (unit tests of other modules) always runs. */
extern int nr_pool_site_shed_tiers(void) __attribute__((weak));
extern void nr_pool_site_shed_note(int tier) __attribute__((weak));
extern int nr_pool_site_active_cells(void) __attribute__((weak));
extern void nr_pool_site_shed_counts(uint64_t out[3]) __attribute__((weak));

/* Value tier of an admission class (lowest value first); -1 = never shed. Justification in nr_core_pool.h (SHED). The decode-gating helpers (DM-RS scan, USS
 * hash, solver probe) are not shed: they decide whether a grant can be decoded, and PDSCH CRC is never traded for latency. */
int nr_admit_tier(nr_admit_class_t c)
{
  switch (c) {
    case NR_ADMIT_UL_DISC: return 0;
    case NR_ADMIT_CSIRS:
    case NR_ADMIT_DMRS_ID: return 1;
    case NR_ADMIT_JANON:
    case NR_ADMIT_NID: return 2;
    default: return -1;
  }
}

int nr_admit_on(void) { return nr_pool_site_active_cells && nr_pool_site_active_cells() >= 2; }
uint64_t nr_admit_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void nr_admit_register_backlog(nr_admit_backlog_fn f) { (void)f; }
int nr_admit_try(nr_admit_class_t c, uint64_t now, uint64_t deadline, int token)
{
  (void)now; (void)deadline; (void)token; /* freshness stays with each producer (its own queue and age check) */
  const int tier = nr_admit_tier(c);
  if (nr_admit_on()) CNT(att, c); /* single cell: nothing counted, output as before */
  if (tier < 0 || !nr_pool_site_shed_tiers) return 1;
  if (nr_pool_site_shed_tiers() <= tier) return 1;
  if (nr_pool_site_shed_note) nr_pool_site_shed_note(tier);
  CNT(skip, c);
  return 0;
}
void nr_admit_done(nr_admit_class_t c, uint64_t busy_ns) { (void)c; (void)busy_ns; }
void nr_admit_class_counts(nr_admit_class_t c, uint64_t *a, uint64_t *s, uint64_t *r, uint64_t *e)
{
  *a = LD(att, c); *s = LD(skip, c); *r = LD(rec, c); *e = LD(exp, c);
}
void nr_admit_reset_for_test(void) { memset(g_cls, 0, sizeof g_cls); }
void nr_admit_note_duty_for_test(nr_admit_class_t c, int pct) { (void)c; (void)pct; }

static void skipq_lock(nr_admit_skipq_t *q) { while (__atomic_exchange_n(&q->lock, 1, __ATOMIC_ACQUIRE)) {} }
static void skipq_unlock(nr_admit_skipq_t *q) { __atomic_store_n(&q->lock, 0, __ATOMIC_RELEASE); }
/* drop items past their deadline from the head (oldest first; deadlines grow with arrival), caller holds the lock */
static void skipq_purge(nr_admit_class_t c, nr_admit_skipq_t *q, uint64_t now, void (*free_fn)(void *))
{
  while (q->n > 0 && q->deadline[q->head] < now) {
    free_fn(q->p[q->head]);
    q->head = (q->head + 1) % NR_ADMIT_SKIPQ_DEPTH;
    q->n--;
    CNT(exp, c);
  }
}
/* shutdown: release what is still waiting (counted expired) */
void nr_admit_skipq_drain(nr_admit_class_t c, nr_admit_skipq_t *q, void (*free_fn)(void *))
{
  skipq_lock(q);
  while (q->n > 0) {
    free_fn(q->p[q->head]);
    q->head = (q->head + 1) % NR_ADMIT_SKIPQ_DEPTH;
    q->n--;
    CNT(exp, c);
  }
  skipq_unlock(q);
}
void nr_admit_skipq_push(nr_admit_class_t c, nr_admit_skipq_t *q, void *item, uint64_t deadline, void (*free_fn)(void *))
{
  skipq_lock(q);
  if (q->n == NR_ADMIT_SKIPQ_DEPTH) {
    free_fn(q->p[q->head]);
    q->head = (q->head + 1) % NR_ADMIT_SKIPQ_DEPTH;
    q->n--;
    CNT(exp, c);
  }
  const int at = (q->head + q->n) % NR_ADMIT_SKIPQ_DEPTH;
  q->p[at] = item;
  q->deadline[at] = deadline;
  q->n++;
  skipq_unlock(q);
}
void *nr_admit_skipq_pop(nr_admit_class_t c, nr_admit_skipq_t *q, uint64_t now, void (*free_fn)(void *))
{
  void *it = NULL;
  skipq_lock(q);
  skipq_purge(c, q, now, free_fn);
  if (q->n > 0 && (!nr_pool_site_shed_tiers || nr_pool_site_shed_tiers() == 0)) {
    it = q->p[q->head];
    q->head = (q->head + 1) % NR_ADMIT_SKIPQ_DEPTH;
    q->n--;
    CNT(rec, c);
  }
  skipq_unlock(q);
  return it;
}
int nr_admit_skipq_len(const nr_admit_skipq_t *q) { return __atomic_load_n(&q->n, __ATOMIC_RELAXED); }

void nr_admit_stats(char *buf, int n)
{
  if (n <= 0) return;
  buf[0] = 0;
  uint64_t s[3] = {0, 0, 0};
  if (nr_pool_site_shed_counts) nr_pool_site_shed_counts(s);
  int o = 0;
  static const char *const nm[NR_ADMIT_N] = {"janon", "uss", "nid", "csirs", "dmrs_scan", "dmrs_id", "probe", "ul_disc"};
  if (s[0] || s[1] || s[2]) o = snprintf(buf, (size_t)n, "ADMIT shed tier0(housekeeping)=%llu tier1(csirs,dmrs_id)=%llu tier2(janon,nid)=%llu", (unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2]);
  if (o < 0 || o >= n) return;
  for (int c = 0; c < NR_ADMIT_N && o < n && nr_isac_mode && nr_isac_mode() != 0; c++) { /* legacy mode: the line stays as before */
    uint64_t a, k, r, e;
    nr_admit_class_counts((nr_admit_class_t)c, &a, &k, &r, &e);
    if (!a && !e) continue;
    const int w = snprintf(buf + o, (size_t)(n - o), "%s%s[attempts=%llu skipped=%llu recovered=%llu expired=%llu]", o ? " " : "ADMIT ", nm[c],
                           (unsigned long long)a, (unsigned long long)k, (unsigned long long)r, (unsigned long long)e);
    if (w < 0 || w >= n - o) break;
    o += w;
  }
}
