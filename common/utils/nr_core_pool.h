/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * One shared pool of EXCLUSIVE cores ("tokens") with priority admission. Always on; no env switch.
 *
 * MODEL. The pool owns N cores. A thread leases ONE token (nr_pool_acquire), pins itself to that core while it holds it and, on release,
 * returns to the home mask (the float lane, never a pool core: no wake-up on a core somebody else now owns). At most N threads hold a
 * lease, so no two pool threads time-share a core. Consumers keep their stacks and thread-local state: the pool decides who runs, never
 * moves a job. Waiters are granted in strict priority (SCAN, DECODE, RETRY, DISC, HOUSE); inside a priority the waiting cell with the lowest
 * decayed token-time usage U_c first (half-life NR_POOL_USAGE_HALFLIFE_NS in nr_core_pool.c, equal weights, ties by round-robin across cells),
 * inside a cell oldest deadline first. Work conserving: an idle cell's share goes to the others at once. No preemption (no RT class). Guarantees:
 *  (a) EXCLUSIVITY. nr_pool_enforce() removes the pool cores from the mask of every task that is not a registered pool member (at start-up,
 *      then about once a second from the scan consumers), threadCreate() refuses an explicit pool core for a non-member, and the site pool
 *      never contains the RT receive / radio-ring / UHD-control cores. Members sit on the home mask unless they hold a lease.
 *  (b) A DECODE ARRIVAL DOES NOT WAIT FOR SCANS. Leases are capped per class so that a token is always left for each urgent class:
 *      scan+low <= N-1 (the last token is the decode reserve, a scan cannot take it), low <= N-2, UL decode: all decode+low <= N-1 (the scan
 *      reserve). DL first-pass decode: DL alone <= N-1, low classes not counted (lows yield to a waiting decode at their next chunk step), and
 *      the last free token stays the scan reserve unless a scan already holds a lease. DL therefore holds min(#DL consumers, N-1) tokens: at
 *      N=6 with 5 DL consumers all five run at once and a scan still gets the sixth. UL shares what DL leaves: with D >= N-1 DL consumers UL
 *      waits for a DL release (one DL job). While a UL decode waits DL gives up its last core: DL is capped at N-2 (scan reserve unchanged),
 *      so a new DL is refused and the next DL release goes to UL, never to the next DL.
 *      With A active cells (a lease or waiter within the last 100 ms) DL is also capped per cell at max(1, N-1-(A-1)), and a cell that holds
 *      a DL token leaves one for each other cell that waits for its first: every active cell reaches a DL token within one release (the UL rule
 *      above stays global; the global DL sum stays <= N-1). With A = 1 this is exactly the single-cell rule.
 *      Waiters older than age_ns[prio] are promoted (starvation bound; they still never exceed busy < N). With fewer than 6 tokens the default
 *      config switches DISC and HOUSE promotion off (nr_pool_cfg_default_n).
 *  (c) NO DISCOVERY/SOLVER UNDER A SCAN LEASE. Discovery work leases class DISC on its own thread; a thread asking for a different class
 *      while it holds a lease is counted in stats.nested. nested also counts the grid-reading discovery passes still run inline under a SCAN
 *      lease (nr_pool_site_note_scan_extra: discovery occasion, autodiscover pass, blind CSI-RS). Decode-gating steps (DCI length sweeps, UL
 *      layout, UL discovery grants, DM-RS scan filter, PDSCH solver) are real-time decode work and run under the scan lease by design: not counted.
 *      A non-zero nested on air is a known fail (the inline discovery passes), not a decode-gating step.
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
/* OR into prio for first-pass UL decodes (own cap: all decode + low <= N-1; DL is counted separately and yields its last core to a waiting UL, see (b)). */
#define NR_POOL_F_UL 0x100
#define NR_POOL_MAX_TOKENS 32
#define NR_POOL_MAX_CELLS 16
#define NR_POOL_CLS_UL NR_POOL_NPRIO
#define NR_POOL_NCLS (NR_POOL_NPRIO + 1)
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
  /* per cell (B1): all classes together */
  uint64_t leases_cell[NR_POOL_MAX_CELLS], waited_cell[NR_POOL_MAX_CELLS], wait_max_ns_cell[NR_POOL_MAX_CELLS], hold_ns_cell[NR_POOL_MAX_CELLS];
  uint64_t wait_hist_cell[NR_POOL_MAX_CELLS][NR_POOL_WAIT_BINS];
  /* per cell and class (MPCI-DASH P1): index = class, NR_POOL_CLS_UL for first-pass UL decodes (they are DECODE leases with the UL flag; the DECODE column is DL only) */
  uint64_t leases_cell_cls[NR_POOL_MAX_CELLS][NR_POOL_NCLS], hold_ns_cell_cls[NR_POOL_MAX_CELLS][NR_POOL_NCLS];
} nr_pool_stats_t;

typedef struct nr_pool nr_pool_t;
nr_pool_t *nr_pool_create(const int *cores, const int *capacity /* NULL = equal */, int n, const nr_pool_cfg_t *cfg /* NULL = defaults */);
void nr_pool_destroy(nr_pool_t *);
nr_pool_cfg_t nr_pool_cfg_default(void);
/* Default config for a pool of n tokens (what nr_pool_create(.., NULL) uses): as nr_pool_cfg_default(), but DISC/HOUSE age promotion off when n < 6
 * (FLOAT-LANE-POOL-20261021 section 3). An explicit cfg passed to nr_pool_create is used as given. */
nr_pool_cfg_t nr_pool_cfg_default_n(int n);
/* Test hook for unit tests only: replace the pool clock (CLOCK_MONOTONIC ns) by a virtual one; NULL restores it. Set only while no thread uses a pool. */
void nr_pool_set_clock_for_test(uint64_t (*fn)(void));

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
/* F1 (MPCI-DASH): the same line split in its two owners. _proc: the process-wide part (n= yields= ... house[...]) WITHOUT the per-cell blocks, printed by one owner
 * per process. _cell: ONE per-cell block, "SENSING: CORE_POOL cell=<cell> pci=<pci> cell<cell>[n= waited= hold_ms= wait_us p50<= p99<= max=]", printed by
 * each cell's own thread; returns 0 (nothing written) when that cell has not leased. nr_pool_stats_line = _proc + every cell block (N=1: _proc only). */
int nr_pool_stats_line_proc(nr_pool_t *, char *buf, int len);
int nr_pool_stats_line_cell(nr_pool_t *, int cell, int pci, char *buf, int len);
/* P1 CELL_CPU: window state of one cell's line (owned by the caller, one per cell) and the formatter. The first call only primes the window (returns 0);
 * n_cells <= 1 returns 0 and writes nothing. Grammar in nr_core_pool.c. */
typedef struct {
  uint64_t t_ns, busy_ns;
  uint64_t cls_hold_ns[NR_POOL_NCLS], cls_leases[NR_POOL_NCLS], cell_hold_ns, cell_leases, cell_waited;
  uint64_t wait_hist[NR_POOL_WAIT_BINS];
  int primed;
} nr_pool_cpu_win_t;
int nr_pool_cell_cpu_line(nr_pool_t *, int cell, int pci, int n_cells, uint64_t now_ns, nr_pool_cpu_win_t *win, char *buf, int len);
uint64_t nr_pool_cell_hold_ns(nr_pool_t *, int cell); /* cumulative token-time (ns) of the cell, all classes (MPCI-DASH P9) */
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

/* FLOAT LANE (mpci B5, FLOAT-LANE-POOL-20261021 sections 3 and 5). The discovery / background workers (janon, nID, USS hash, dmrsScan, dmrsId,
 * csirsDisc: class DISC; ulBgDisc, UL NID bg: class HOUSE) lease a token per job instead of living on a dedicated lane, so N cells' workers
 * share the pool and none pins to one core. A host with fewer than NR_POOL_FLOAT_MERGE_MIN_TOKENS tokens (W <= 5 cpus, tokens = W - home)
 * keeps the dedicated lane (nr_pool_site_float_merged() == false): then every float call below is a no-op and the workers behave as before.
 * The rule is a function of the pool size only; there is no switch. A lease is per job and never spans a cond wait; no FIFO/RR. */
#define NR_POOL_FLOAT_MERGE_MIN_TOKENS 5
void nr_pool_site_set_for_test(nr_pool_t *p); /* unit tests only: use p as the site pool (NULL = back to the lazily built one); set while no thread uses the site pool */
bool nr_pool_float_merged_n(int tokens);   /* pure: floats lease from a pool of `tokens` tokens */
bool nr_pool_site_float_merged(void);      /* the site pool exists and is large enough (builds the site pool on first use) */
/* Lease for one float job (prio NR_POOL_DISC or NR_POOL_HOUSE) on behalf of `cell` (the worker's bound cell, nr_cell_idx()); enq_ns = arrival
 * time of the job (0 = now). Returns the core, or -1 when not merged (no lease taken). Pair with nr_pool_site_float_release(). */
int nr_pool_site_float_acquire(int prio, int cell, uint64_t enq_ns);
void nr_pool_site_float_release(void);     /* no-op when the caller holds no lease */
bool nr_pool_site_float_yield(void);       /* chunk boundary of a long job: give the token to a more urgent waiter, re-acquire; false when not merged / nothing waits */
#ifdef __cplusplus
}
#endif
#endif
