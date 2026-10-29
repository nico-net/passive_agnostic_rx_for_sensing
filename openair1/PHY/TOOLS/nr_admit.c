/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_admit.h"
#include <stdio.h>
#include <time.h>

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
  if (tier < 0 || !nr_pool_site_shed_tiers) return 1;
  if (nr_pool_site_shed_tiers() <= tier) return 1;
  if (nr_pool_site_shed_note) nr_pool_site_shed_note(tier);
  return 0;
}
void nr_admit_done(nr_admit_class_t c, uint64_t busy_ns) { (void)c; (void)busy_ns; }
void nr_admit_stats(char *buf, int n)
{
  if (n <= 0) return;
  buf[0] = 0;
  uint64_t s[3] = {0, 0, 0};
  if (nr_pool_site_shed_counts) nr_pool_site_shed_counts(s);
  if (s[0] || s[1] || s[2]) snprintf(buf, (size_t)n, "ADMIT shed tier0(housekeeping)=%llu tier1(csirs,dmrs_id)=%llu tier2(janon,nid)=%llu", (unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2]);
}
