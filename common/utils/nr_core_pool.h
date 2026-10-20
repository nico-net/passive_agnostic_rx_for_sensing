/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * One shared pool of exclusive cores ("tokens") with priority admission, replacing one pinned core per role.
 *
 * MODEL. The pool owns N cores. A thread that wants to compute leases ONE token (nr_pool_acquire), pins itself to that core while it
 * holds it, and gives it back (nr_pool_release). At most N threads hold a lease, so no two pool threads ever time-share a core. The
 * consumers keep their own stacks and thread-local state (FEP/chest estimates, CUDA state, spin accounting): the pool schedules who
 * runs, never moves a job. Waiters are granted a free token in strict priority (SCAN, DECODE, RETRY, DISC, HOUSE); inside a priority
 * round-robin across cells, inside a cell oldest deadline first. No preemption (no RT class): a high-priority waiter waits for the
 * next release, which is bounded by (a) a reserve: lower classes (>= reserve_from) are never granted the last `reserve` tokens, so
 * a SCAN/DECODE arrival finds a free core unless that many high-priority jobs are already running; (b) chunking: long low-priority
 * work calls nr_pool_yield() between steps, which releases and re-queues when a more urgent waiter exists; (c) aging: a waiter older
 * than age_ns[prio] is promoted ahead of strict order (starvation bound), and ignores the reserve.
 * Tokens carry a capacity (cpu_capacity): SCAN/DECODE get the biggest free core, lower classes the smallest.
 * A GPU wait inside a job should be bracketed by nr_pool_release()/nr_pool_acquire() so a waiting decode frees its core.
 * Pool inactive (nr_pool_site() == NULL, ISAC_POOL_CORES unset): every call below is a no-op, and the receiver keeps per-role cores.
 * Clock: CLOCK_MONOTONIC ns.
 */
#ifndef NR_CORE_POOL_H
#define NR_CORE_POOL_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NR_POOL_SCAN = 0, NR_POOL_DECODE, NR_POOL_RETRY, NR_POOL_DISC, NR_POOL_HOUSE, NR_POOL_NPRIO };
#define NR_POOL_MAX_TOKENS 32
#define NR_POOL_MAX_CELLS 16
#define NR_POOL_WAIT_BINS 24 /* log2(us) histogram of the lease wait: bin b = [2^(b-1), 2^b) us, bin 0 = < 1 us */

typedef struct {
  uint64_t age_ns[NR_POOL_NPRIO]; /* promotion age per priority; 0 = never promoted (SCAN, and DECODE by default) */
  int reserve;                    /* tokens kept free of classes >= reserve_from */
  int reserve_from;
  bool pin;                       /* sched_setaffinity on grant (tests turn it off on hosts without the cores) */
} nr_pool_cfg_t;

typedef struct {
  uint64_t leases[NR_POOL_NPRIO], waited[NR_POOL_NPRIO], promoted[NR_POOL_NPRIO], yields;
  uint64_t wait_max_ns[NR_POOL_NPRIO], hold_ns[NR_POOL_NPRIO];
  uint64_t wait_hist[NR_POOL_NPRIO][NR_POOL_WAIT_BINS];
  uint64_t busy_ns; /* sum of lease hold time over all tokens */
} nr_pool_stats_t;

typedef struct nr_pool nr_pool_t;
nr_pool_t *nr_pool_create(const int *cores, const int *capacity /* NULL = equal */, int n, const nr_pool_cfg_t *cfg /* NULL = defaults */);
void nr_pool_destroy(nr_pool_t *);
nr_pool_cfg_t nr_pool_cfg_default(void);

/* Lease a token for the calling thread; blocks until granted. Returns the core. Idempotent: a thread that already holds a lease keeps
 * it (and its original priority) and gets its core back. deadline_ns: CLOCK_MONOTONIC time the work is due or arrived (smaller = more
 * urgent, 0 = now). cell: 0..NR_POOL_MAX_CELLS-1. */
int nr_pool_acquire(nr_pool_t *, int prio, int cell, uint64_t deadline_ns);
/* Non-blocking: -1 when no token may be granted now (none free, or only the reserve is left for this class). */
int nr_pool_try_acquire(nr_pool_t *, int prio, int cell, uint64_t deadline_ns);
void nr_pool_release(nr_pool_t *); /* no-op when the caller holds no lease of this pool */
bool nr_pool_holds(const nr_pool_t *);
/* Waiter of a strictly more urgent class exists: a chunked job should return its token at its next step boundary. Lock-free. */
bool nr_pool_should_yield(const nr_pool_t *, int prio);
/* Release and re-acquire (same class/cell/deadline) iff should_yield; returns whether it yielded. */
bool nr_pool_yield(nr_pool_t *);
void nr_pool_stats(nr_pool_t *, nr_pool_stats_t *out);
int nr_pool_stats_line(nr_pool_t *, char *buf, int len); /* one log line: per class leases, wait p50/p99/max, busy cores */
int nr_pool_size(const nr_pool_t *);
int nr_pool_core_of(const nr_pool_t *, int token);

/* Site pool: built once from ISAC_POOL_CORES (cpu list, e.g. "6-11"; capacity from sysfs cpu_capacity). NULL = off. Thread-safe. */
nr_pool_t *nr_pool_site(void);
/* Wrappers on the site pool; all no-ops (return -1 / false) when it is off. */
int nr_pool_site_acquire(int prio, int cell, uint64_t deadline_ns);
int nr_pool_site_try_acquire(int prio, int cell, uint64_t deadline_ns);
void nr_pool_site_release(void);
bool nr_pool_site_yield(void);
uint64_t nr_pool_now_ns(void);
#ifdef __cplusplus
}
#endif
#endif
