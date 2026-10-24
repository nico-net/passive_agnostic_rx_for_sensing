/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_HOST_MEMBUDGET_H
#define NR_HOST_MEMBUDGET_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One host-RAM budget for the receiver's big allocations (the companion of nr_gpu_membudget.h, which budgets device memory).
 *
 * Why: the receiver pre-faults its pools before RX (mlockall, no allocation during RX). At 273 PRB / 4 RX / rank 4 the GrantWork LLR pool alone is
 * consumers x depth x 16 buffers of 2.9 MB = 12 GB [MEASURED, sens6 273 PRB: gw_buffers=4096 llr_bytes=2935296], on a 14.8 GB host: swap storm, then the OOM killer.
 *
 * Budget:  avail  = min(MemAvailable, cgroup headroom)   (swap is never counted: the receiver must not swap)
 *          B      = 0.80 * avail - NR_HOST_MB_RESERVE     (the process image, thread stacks, libraries, the logs, the gNB or other processes)
 * cgroup headroom = min over the cgroup and its ancestors of (min(memory.max, memory.high) - memory.current)  (v2; v1: memory.limit_in_bytes - memory.usage_in_bytes).
 * Unified memory (the GPU shares the RAM): the device modules' committed bytes (nr_gpu_membudget) come out of the same pool, so B = that minus the GPU commitments
 *   (the platform layer sets nr_host_membudget_set_unified(). A 128 GB unified host: B is above every host need, nothing is scaled).
 * Unknown (no /proc/meminfo): unlimited. A share that covers a module's full need leaves its size unchanged (DGX Spark: every pool at today's size).
 *
 * Modules: the pools that scale (GrantWork LLR pool: only throughput depends on its size, a miss goes to the CPU path) have a weight; the fixed ones (queue arenas, discovery
 * rings, time-domain pool, pinned mirrors) commit their real bytes first through nr_host_membudget_commit() and are not scaled. A scalable module gets
 * share = (B - committed fixed) * weight / 100 and sizes its knob with nr_host_membudget_fit() (floor: below it the module is not ready: a one-time log, the CPU path). */
typedef enum {
  NR_HOST_MEM_GW_POOL, /* GrantWork LLR pool (scalable, sized last from what is left) */
  NR_HOST_MEM_QUEUE,   /* PDSCH queue arenas: rxdataF + owned IQ per consumer (fixed) */
  NR_HOST_MEM_CSIRS,   /* CSI-RS discovery frequency-domain ring (+ snapshot ring when used): shrinkable at start, elastic at run time */
  NR_HOST_MEM_TD,      /* CSI-RS discovery time-domain slot pool + FFT batch buffers (shrinkable at start) */
  NR_HOST_MEM_PINNED,  /* CUDA pinned host mirrors of the GPU pass (cudaHostAlloc, fixed) */
  NR_HOST_MEM_USRP,    /* USRP decoupled RX ring (ISAC_USRP_RING_MS, shrinkable; reserved before the radio starts) */
  NR_HOST_MEM_SNAP,    /* async-recovery IQ snapshot holders (shrinkable) */
  NR_HOST_MEM_OTHER,
  NR_HOST_MEM_N
} nr_host_mem_module_t;
#define NR_HOST_MB_RESERVE ((size_t)1536 << 20)
/* Hard limit: the fixed modules plus the floors of the shrinkable ones must stay below avail - NR_HOST_MB_HARD_RESERVE, else the receiver refuses to start
 * (a clear message before the radio is touched instead of a swap storm / OOM kill during RX). */
#define NR_HOST_MB_HARD_RESERVE ((size_t)512 << 20)
static const char *const nr_host_mem_name[NR_HOST_MEM_N] = {"gw_pool", "queue", "csirs", "td", "pinned", "usrp_ring", "snap", "other"};

/* ---- pure functions ---- */
static inline size_t nr_host_membudget_calc(size_t avail)
{
  if (avail == SIZE_MAX) return SIZE_MAX;
  const size_t usable = avail / 5 * 4; /* 0.80 */
  return usable > NR_HOST_MB_RESERVE ? usable - NR_HOST_MB_RESERVE : 0;
}
/* Budget of a live re-probe taken late in start-up (the process image, CUDA context and early buffers are already resident and inside `live`, so the 1.5 GB start-up
 * reserve is not charged again): 0.90 * live - NR_HOST_MB_HARD_RESERVE (the margin for the run-time growth: thread stacks, heap, logs; measured +0.1-0.4 GB on sens6). */
static inline size_t nr_host_membudget_live_calc(size_t live)
{
  if (live == SIZE_MAX) return SIZE_MAX;
  const size_t usable = live / 10 * 9;
  return usable > NR_HOST_MB_HARD_RESERVE ? usable - NR_HOST_MB_HARD_RESERVE : 0;
}
/* Largest k in [lo, hi] with fixed + per_unit * k <= share; 0 when even lo does not fit; hi unchanged whenever it fits (a large budget never changes a size). */
static inline long nr_host_membudget_fit(size_t share, size_t fixed, size_t per_unit, long lo, long hi)
{
  if (share == SIZE_MAX || fixed + per_unit * (size_t)hi <= share) return hi;
  if (share <= fixed || !per_unit) return 0;
  const long k = (long)((share - fixed) / per_unit);
  return k < lo ? 0 : k;
}
/* Fit a module into pct % of a headroom, keeping what it already holds (own): k in [lo, hi] (0 = below the floor). */
static inline long nr_host_membudget_fit_pct(size_t headroom, size_t own, int pct, size_t fixed, size_t per_unit, long lo, long hi)
{
  if (headroom == SIZE_MAX) return hi;
  const size_t room = (headroom / 100) * (size_t)pct + own;
  return nr_host_membudget_fit(room, fixed, per_unit, lo, hi);
}
/* mlockall plan (common/utils/system.c lock_memory_to_ram): FULL = MCL_CURRENT|MCL_FUTURE (upstream behaviour: every mapped page is faulted in and locked now),
 * ONFAULT = MCL_CURRENT|MCL_FUTURE|MCL_ONFAULT (pages are locked when first touched; the receiver pre-faults its pools itself, so only never-touched mappings stay out).
 * FULL only when the not-yet-resident accessible mappings (`unfaulted`) fit the budget of the memory available now; else faulting them all would push the host
 * into swap / the OOM killer. Unknown availability (SIZE_MAX): FULL (unchanged on hosts without /proc/meminfo; the 128 GB DGX Spark is always FULL). */
enum { NR_HOST_MLOCK_FULL = 0, NR_HOST_MLOCK_ONFAULT = 1 };
static inline int nr_host_mlock_plan(size_t unfaulted, size_t live_avail)
{
  if (live_avail == SIZE_MAX) return NR_HOST_MLOCK_FULL;
  return unfaulted <= nr_host_membudget_calc(live_avail) ? NR_HOST_MLOCK_FULL : NR_HOST_MLOCK_ONFAULT;
}
/* Parsers (the probe is a thin reader on top; fixtures feed them text). Return 0 on success. "max" / absent limit -> *out = SIZE_MAX. */
int nr_host_parse_meminfo(const char *text, size_t *mem_available);                  /* "MemAvailable:   123 kB" -> bytes */
int nr_host_parse_limit(const char *text, size_t *out);                               /* "max\n" or a byte count (v1: values above 2^62 mean none) */
int nr_host_parse_cgroup_path(const char *text, int v1, char *path, size_t n);        /* /proc/self/cgroup: v2 "0::/p", v1 "N:memory:/p" (also "N:cpu,memory:/p") */
int nr_host_parse_hugetlb(const char *text, size_t *total, size_t *free_b);          /* HugePages_Total/Free x Hugepagesize -> bytes (the pool is outside MemAvailable) */
/* Bytes that mlockall(MCL_CURRENT) would fault in: sum of (Size - Rss) over the accessible mappings of a /proc/<pid>/smaps text (PROT_NONE reservations, device
 * mappings (/dev/..., VM_IO / VM_PFNMAP) and hugetlb mappings are not populated by mlock and are skipped). */
size_t nr_host_parse_smaps_unfaulted(const char *text);
/* Probe rooted at proc_root ("/proc") and cg_root ("/sys/fs/cgroup"): the available bytes (SIZE_MAX = unknown) and which source limited it. */
typedef struct { size_t mem_available, cgroup_headroom, avail, hugetlb_total, hugetlb_free; char limited_by[24]; } nr_host_probe_t;
void nr_host_probe_at(const char *proc_root, const char *cg_root, nr_host_probe_t *out);

/* ---- process state ---- */
size_t nr_host_membudget_total(void);                           /* B, bytes; SIZE_MAX = unlimited. The probe runs once (NR_HOST_MEM_BUDGET_MB emulates a host with that much available memory) */
size_t nr_host_membudget_share(nr_host_mem_module_t m);         /* scalable modules: (B - committed fixed) * weight / 100; others: B. SIZE_MAX = unlimited */
void nr_host_membudget_commit(nr_host_mem_module_t m, size_t bytes, double scale); /* the bytes the module took (0 with scale 0 = not ready) */
size_t nr_host_membudget_committed(void);                       /* sum of the committed bytes */
void nr_host_membudget_reserve(nr_host_mem_module_t m, size_t bytes); /* a commitment whose memory is not allocated yet (USRP ring before the radio starts): counted in
                                                                        the budget like a commit, and also deducted from the live re-probe (which cannot see it yet) */
size_t nr_host_membudget_headroom(void);                        /* B - all committed bytes (0 when over); SIZE_MAX = unlimited */
size_t nr_host_membudget_hard_limit(void);                      /* avail - NR_HOST_MB_HARD_RESERVE (minus unified GPU commitments); SIZE_MAX = unlimited */
int nr_host_membudget_over_hard(char *why, size_t n);           /* 1 when the committed bytes exceed the hard limit (why: the per-module list), else 0 */
size_t nr_host_membudget_live_avail(void);                      /* a fresh probe (MemAvailable / cgroup now; with NR_HOST_MEM_BUDGET_MB: that minus the committed bytes) */
size_t nr_host_membudget_live_share(nr_host_mem_module_t m);    /* the share of a scalable module computed from the live probe: (calc(live) - pending reservations) * weight */
long nr_host_membudget_size(nr_host_mem_module_t m, int pct, size_t fixed, size_t per_unit, long lo, long hi); /* shrinkable module at start: the largest k in [lo, hi]
                                                                        with fixed + per_unit * k within pct % of the headroom (plus what the module held), committed with
                                                                        scale k / hi; 0 = below the floor (nothing committed) */
void nr_host_membudget_set_unified(size_t (*gpu_committed)(void)); /* the GPU shares this RAM: its committed bytes (callback) are deducted from B */
int nr_host_membudget_format(char *out, size_t n);              /* "avail_mb=.. limited_by=.. budget_mb=.. gw_pool=<mb>/share=<mb>/scale=.." for the SENSING: HOST_MEM line */
void nr_host_membudget_note_failure(nr_host_mem_module_t m);    /* an allocation of the module failed: counted; returns nothing, the caller degrades */
uint64_t nr_host_membudget_failures(nr_host_mem_module_t m);
/* USRP ring plan: the receiver sizes ISAC_USRP_RING_MS from the budget before the radio starts; the USRP driver (a dlopen'ed library) reads the planned value
 * through dlsym(RTLD_DEFAULT, "nr_host_membudget_usrp_ring_ms"). -1 = no plan (the driver keeps ISAC_USRP_RING_MS). */
void nr_host_membudget_set_usrp_ring_ms(int ms);
int nr_host_membudget_usrp_ring_ms(void);
void nr_host_membudget_test_reset(void);
#ifdef __cplusplus
}
#endif
#endif
