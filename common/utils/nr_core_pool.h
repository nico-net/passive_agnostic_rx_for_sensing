/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * One shared pool of EXCLUSIVE cores ("tokens") with priority admission. Always on; no env switch.
 *
 * MODEL. The pool owns N cores. A thread leases ONE token (nr_pool_acquire), pins itself to that core while it holds it and, on release,
 * returns to the home mask (the float lane, never a pool core: no wake-up on a core somebody else now owns). At most N threads hold a
 * lease, so no two pool threads time-share a core. Consumers keep their stacks and thread-local state: the pool decides who runs, never
 * moves a job. Waiters are granted in strict priority (SCAN, DECODE, RETRY, DISC, HOUSE); inside a priority round-robin across cells,
 * inside a cell oldest deadline first. No preemption (no RT class). Guarantees:
 *  (a) EXCLUSIVITY. nr_pool_enforce() removes the pool cores from the mask of every task that is not a registered pool member (at start-up,
 *      then about once a second from the scan consumers), threadCreate() refuses an explicit pool core for a non-member, and the site pool
 *      never contains the RT receive / radio-ring / UHD-control cores. Members sit on the home mask unless they hold a lease.
 *  (b) A DECODE ARRIVAL DOES NOT WAIT FOR SCANS. Leases are capped per class so that a token is always left for each urgent class:
 *      scan+low <= N-1 (the last token is the decode reserve, a scan cannot take it), low <= N-2, UL decode: all decode+low <= N-1 (the scan
 *      reserve). DL first-pass decode: DL alone <= N-1, low classes not counted (lows yield to a waiting decode at their next chunk step), and
 *      the last free token stays the scan reserve unless a scan already holds a lease. DL therefore holds min(#DL consumers, N-1) tokens: at
 *      N=6 with 5 DL consumers all five run at once and a scan still gets the sixth. UL shares what DL leaves: with D >= N-1 DL consumers UL
 *      waits for a DL release (one DL job) instead of owning a reserve.
 *      Waiters older than age_ns[prio] are promoted (starvation bound; they still never exceed busy < N).
 *  (c) NO DISCOVERY/SOLVER UNDER A SCAN LEASE. Discovery work leases class DISC on its own thread; a thread asking for a different class
 *      while it holds a lease is counted in stats.nested; so is scan-side discovery/solver work that still runs inline under a SCAN lease
 *      (nr_pool_site_note_scan_extra), i.e. nested > 0 on air is expected until that work moves to its own DISC thread.
 *  (d) WORKS WITH NO SETTING: the core list is the union of the existing site map (ISAC_PDSCH_CORES, ISAC_PUSCH_CORES; defaults built in)
 *      minus the RT/IO cores; a host with fewer cores gets the allowed cores that remain.
 * Chunking: long low-priority work calls nr_pool_yield() between steps. A GPU poll sleep (nr_gpu_poll_relax -> nr_pool_site_gpu_wait) gives the
 * token away only inside an explicit give scope (see below); a wait that must hand the token to the thread it waits for uses park/unpark.
 * Clock: CLOCK_MONOTONIC ns.
 */
#ifndef NR_CORE_POOL_H
#define NR_CORE_POOL_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NR_POOL_SCAN = 0, NR_POOL_DECODE, NR_POOL_RETRY, NR_POOL_DISC, NR_POOL_HOUSE, NR_POOL_NPRIO };
/* OR into prio for first-pass UL decodes (own cap: all decode + low <= N-1; DL is counted separately, see (b)). */
#define NR_POOL_F_UL 0x100
#define NR_POOL_MAX_TOKENS 32
#define NR_POOL_MAX_CELLS 16
#define NR_POOL_WAIT_BINS 24 /* log2(us) histogram of the lease wait: bin b = [2^(b-1), 2^b) us, bin 0 = < 1 us */

typedef struct {
  uint64_t age_ns[NR_POOL_NPRIO]; /* promotion age per priority; 0 = never promoted (SCAN, and DECODE by default) */
  bool pin;                       /* sched_setaffinity on grant/release (tests turn it off on hosts without the cores) */
} nr_pool_cfg_t;

typedef struct {
  uint64_t leases[NR_POOL_NPRIO], waited[NR_POOL_NPRIO], promoted[NR_POOL_NPRIO], yields, nested, gpu_gives, enforced;
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
/* Remove the pool cores from every task of this process that is not a registered member (see (a)); returns the number of tasks changed. */
int nr_pool_enforce(nr_pool_t *);
void nr_pool_member_enter(nr_pool_t *); /* register the calling thread as a pool member and send it to the home mask */
int nr_pool_total_waiting(const nr_pool_t *);
int nr_pool_free(const nr_pool_t *);   /* tokens free now (racy, for hints) */
bool nr_pool_core_in_pool(const nr_pool_t *, int core);
/* Waiter of a strictly more urgent class exists: a chunked job should return its token at its next step boundary. Lock-free. */
bool nr_pool_should_yield(const nr_pool_t *, int prio);
/* Release and re-acquire (same class/cell/deadline) iff should_yield; returns whether it yielded. */
bool nr_pool_yield(nr_pool_t *);
void nr_pool_stats(nr_pool_t *, nr_pool_stats_t *out);
int nr_pool_stats_line(nr_pool_t *, char *buf, int len); /* one log line: per class leases, wait p50/p99/max, busy cores */
int nr_pool_size(const nr_pool_t *);
int nr_pool_core_of(const nr_pool_t *, int token);

/* Site pool: built once on first use from the site core map (always non-NULL on a host with >= 1 allowed core). Thread-safe. */
nr_pool_t *nr_pool_site(void);
void nr_pool_site_member_enter(void);
int nr_pool_site_free(void);             /* -1 when no pool */
int nr_pool_site_filter_core(int core); /* threadCreate hook: an explicit pool core for a non-member becomes -1 (counted + logged once) */
/* Poll-loop hook. Gives the token away for one poll sleep ONLY inside a give scope (begin/end, nestable, per thread) and only when a waiter
 * that the freed token could actually be granted to exists; otherwise it just sleeps. Open a scope only around a long GPU wait that holds no
 * lock another token holder blocks on and has no deadline abandonment (a re-acquire may take longer than the poll sleep). */
void nr_pool_site_gpu_wait(void);
void nr_pool_gpu_wait(nr_pool_t *);
void nr_pool_site_gpu_give_begin(void);
void nr_pool_site_gpu_give_end(void);
/* Park: release the caller's lease (if it holds one) across a wait for another pool user, remembering class/cell/deadline; unpark re-acquires
 * the same class (blocking). Needed when the waited-for thread needs a token too (e.g. scan side_finish at N=1). */
typedef struct { int prio, cell; uint64_t deadline; bool held; } nr_pool_park_t;
void nr_pool_park(nr_pool_t *, nr_pool_park_t *);
void nr_pool_unpark(nr_pool_t *, const nr_pool_park_t *);
void nr_pool_note_scan_extra(nr_pool_t *);
void nr_pool_site_park(nr_pool_park_t *);
void nr_pool_site_unpark(const nr_pool_park_t *);
/* The caller does scan-side extra work (discovery, solver) inline: counted in stats.nested when it holds a SCAN lease of the site pool. */
void nr_pool_site_note_scan_extra(void);
/* threadCreate hook: when the caller holds a site lease, restrict the new thread's attr to the home mask (not the lessee's single core). True if set. */
bool nr_pool_site_home_attr(pthread_attr_t *);
void nr_pool_site_enforce_tick(void);    /* rate-limited (1/s) nr_pool_enforce(); cheap when called often */
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
