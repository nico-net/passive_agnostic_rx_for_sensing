/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_admit.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NB 10                   /* duty ring: 10 buckets x 10 ms = 100 ms */
#define BUCKET_NS 10000000ull
#define MAX_BL 8
static const char *const names[NR_ADMIT_N] = {"janon", "uss", "nid", "csirs", "dmrs_scan", "dmrs_id", "probe", "ul_disc"};
static struct {
  _Atomic uint64_t epoch[NB], busy[NB], last_now;
  _Atomic uint64_t run, skip_busy, expired, floor;
} cls[NR_ADMIT_N];
static nr_admit_backlog_fn bl[MAX_BL];
static _Atomic int nbl, cfg_state = -1 /* -1 unread */, min_duty_pct = 5;
static int busy_thr[NR_ADMIT_N], floor_pct[NR_ADMIT_N]; /* per class: backlog that counts as busy, duty floor in % */

/* The critical-lane backlog getters, resolved at link time: absent in unit tests and in executables without that module. */
extern int nr_pdsch_passive_queue_backlog_real(void) __attribute__((weak));
extern int nr_pdcch_passive_queue_backlog(void) __attribute__((weak));
extern int nr_pusch_passive_queue_real_pending(void) __attribute__((weak));
extern int nr_gpu_budget_rt_active(void) __attribute__((weak));
static void cfg(void)
{
  if (atomic_load(&cfg_state) >= 0) return;
  /* DEFERRED/DO NOT USE: ISAC_BG_LANE not in the frozen config t26zE (2026-10-18); slated for removal */
  /* DEFERRED/DO NOT USE: ISAC_BG_MIN_DUTY_PCT not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *e = getenv("ISAC_BG_LANE"), *d = getenv("ISAC_BG_MIN_DUTY_PCT");
  atomic_store(&min_duty_pct, d && atoi(d) >= 0 ? atoi(d) : 5);
  for (int c = 0; c < NR_ADMIT_N; c++) { busy_thr[c] = 1; floor_pct[c] = atomic_load(&min_duty_pct); }
  /* the anonymous solver is the discovery engine on a busy cell: it yields only to a real backlog and keeps 60 % duty */
  /* DEFERRED/DO NOT USE: ISAC_BG_JANON_BACKLOG not in the frozen config t26zE (2026-10-18); slated for removal */
  /* DEFERRED/DO NOT USE: ISAC_BG_JANON_DUTY_PCT not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *jb = getenv("ISAC_BG_JANON_BACKLOG"), *jd = getenv("ISAC_BG_JANON_DUTY_PCT");
  busy_thr[NR_ADMIT_JANON] = jb && atoi(jb) > 0 ? atoi(jb) : 4;
  floor_pct[NR_ADMIT_JANON] = jd && atoi(jd) >= 0 ? atoi(jd) : 60;
  atomic_store(&cfg_state, e && atoi(e) == 1);
  if (atomic_load(&cfg_state)) {
    if (nr_pdsch_passive_queue_backlog_real) nr_admit_register_backlog(nr_pdsch_passive_queue_backlog_real);
    if (nr_pdcch_passive_queue_backlog) nr_admit_register_backlog(nr_pdcch_passive_queue_backlog);
    if (nr_pusch_passive_queue_real_pending) nr_admit_register_backlog(nr_pusch_passive_queue_real_pending);
    if (nr_gpu_budget_rt_active) nr_admit_register_backlog(nr_gpu_budget_rt_active);
  }
}
int nr_admit_on(void) { cfg(); return atomic_load(&cfg_state); }
uint64_t nr_admit_now_ns(void) /* 0 when off: no clock reads on the default path */
{
  if (!nr_admit_on()) return 0;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
void nr_admit_register_backlog(nr_admit_backlog_fn f)
{
  for (int i = 0; i < atomic_load(&nbl); i++) if (bl[i] == f) return;
  int i = atomic_fetch_add(&nbl, 1);
  if (i < MAX_BL) bl[i] = f; else atomic_store(&nbl, MAX_BL);
}
static int critical_busy(int thr)
{
  int sum = 0;
  for (int i = 0, n = atomic_load(&nbl); i < n && i < MAX_BL; i++) if (bl[i]) { const int v = bl[i](); if (v > 0) sum += v; }
  return sum >= thr;
}
/* shortcut: a bucket reset racing a concurrent add can lose one sample; the floor is a rate limiter, not an accountant. */
static void add_busy(int c, uint64_t now, uint64_t ns)
{
  const uint64_t ep = now / BUCKET_NS;
  const int i = ep % NB;
  if (atomic_load(&cls[c].epoch[i]) != ep) { atomic_store(&cls[c].busy[i], 0); atomic_store(&cls[c].epoch[i], ep); }
  atomic_fetch_add(&cls[c].busy[i], ns);
}
static uint64_t duty_ns(int c, uint64_t now)
{
  const uint64_t ep = now / BUCKET_NS;
  uint64_t s = 0;
  for (int i = 0; i < NB; i++) {
    const uint64_t e = atomic_load(&cls[c].epoch[i]);
    if (e <= ep && ep - e < NB) s += atomic_load(&cls[c].busy[i]);
  }
  return s;
}
int nr_admit_try(nr_admit_class_t c, uint64_t now, uint64_t deadline, int token)
{
  if (!nr_admit_on() || (unsigned)c >= NR_ADMIT_N) return 1;
  atomic_store(&cls[c].last_now, now);
  if (deadline && now >= deadline) { /* deadline 0 = no expiry */ atomic_fetch_add(&cls[c].expired, 1); return 0; }
  if (!token && critical_busy(busy_thr[c])) {
    if (duty_ns(c, now) >= (uint64_t)floor_pct[c] * (NB * BUCKET_NS / 100)) {
      atomic_fetch_add(&cls[c].skip_busy, 1);
      return 0;
    }
    atomic_fetch_add(&cls[c].floor, 1);
  }
  atomic_fetch_add(&cls[c].run, 1);
  return 1;
}
void nr_admit_done(nr_admit_class_t c, uint64_t busy_ns)
{
  if (nr_admit_on() && (unsigned)c < NR_ADMIT_N) add_busy(c, atomic_load(&cls[c].last_now), busy_ns);
}
void nr_admit_stats(char *buf, int n)
{
  int o = snprintf(buf, n, "ADMIT");
  for (int c = 0; c < NR_ADMIT_N && o > 0 && o < n; c++)
    o += snprintf(buf + o, n - o, " %s[run=%llu skip_busy=%llu expired=%llu floor=%llu]", names[c],
                  (unsigned long long)cls[c].run, (unsigned long long)cls[c].skip_busy,
                  (unsigned long long)cls[c].expired, (unsigned long long)cls[c].floor);
}
void nr_admit_reset_for_test(void)
{
  memset((void *)cls, 0, sizeof(cls));
  atomic_store(&nbl, 0);
  atomic_store(&cfg_state, -1);
}
void nr_admit_note_duty_for_test(nr_admit_class_t c, int pct)
{
  add_busy(c, 0, (uint64_t)pct * (NB * BUCKET_NS / 100));
}
