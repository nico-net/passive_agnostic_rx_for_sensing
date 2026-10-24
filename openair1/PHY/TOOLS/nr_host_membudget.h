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
typedef enum { NR_HOST_MEM_GW_POOL, NR_HOST_MEM_QUEUE, NR_HOST_MEM_CSIRS, NR_HOST_MEM_TD, NR_HOST_MEM_PINNED, NR_HOST_MEM_OTHER, NR_HOST_MEM_N } nr_host_mem_module_t;
#define NR_HOST_MB_RESERVE ((size_t)1536 << 20)
static const char *const nr_host_mem_name[NR_HOST_MEM_N] = {"gw_pool", "queue", "csirs", "td", "pinned", "other"};

/* ---- pure functions ---- */
static inline size_t nr_host_membudget_calc(size_t avail)
{
  if (avail == SIZE_MAX) return SIZE_MAX;
  const size_t usable = avail / 5 * 4; /* 0.80 */
  return usable > NR_HOST_MB_RESERVE ? usable - NR_HOST_MB_RESERVE : 0;
}
/* Largest k in [lo, hi] with fixed + per_unit * k <= share; 0 when even lo does not fit; hi unchanged whenever it fits (a large budget never changes a size). */
static inline long nr_host_membudget_fit(size_t share, size_t fixed, size_t per_unit, long lo, long hi)
{
  if (share == SIZE_MAX || fixed + per_unit * (size_t)hi <= share) return hi;
  if (share <= fixed || !per_unit) return 0;
  const long k = (long)((share - fixed) / per_unit);
  return k < lo ? 0 : k;
}
/* Parsers (the probe is a thin reader on top; fixtures feed them text). Return 0 on success. "max" / absent limit -> *out = SIZE_MAX. */
int nr_host_parse_meminfo(const char *text, size_t *mem_available);                  /* "MemAvailable:   123 kB" -> bytes */
int nr_host_parse_limit(const char *text, size_t *out);                               /* "max\n" or a byte count (v1: values above 2^62 mean none) */
int nr_host_parse_cgroup_path(const char *text, int v1, char *path, size_t n);        /* /proc/self/cgroup: v2 "0::/p", v1 "N:memory:/p" (also "N:cpu,memory:/p") */
/* Probe rooted at proc_root ("/proc") and cg_root ("/sys/fs/cgroup"): the available bytes (SIZE_MAX = unknown) and which source limited it. */
typedef struct { size_t mem_available, cgroup_headroom, avail; char limited_by[24]; } nr_host_probe_t;
void nr_host_probe_at(const char *proc_root, const char *cg_root, nr_host_probe_t *out);

/* ---- process state ---- */
size_t nr_host_membudget_total(void);                           /* B, bytes; SIZE_MAX = unlimited. The probe runs once (NR_HOST_MEM_BUDGET_MB emulates a host with that much available memory) */
size_t nr_host_membudget_share(nr_host_mem_module_t m);         /* scalable modules: (B - committed fixed) * weight / 100; others: B. SIZE_MAX = unlimited */
void nr_host_membudget_commit(nr_host_mem_module_t m, size_t bytes, double scale); /* the bytes the module took (0 with scale 0 = not ready) */
size_t nr_host_membudget_committed(void);                       /* sum of the committed bytes */
void nr_host_membudget_set_unified(size_t (*gpu_committed)(void)); /* the GPU shares this RAM: its committed bytes (callback) are deducted from B */
int nr_host_membudget_format(char *out, size_t n);              /* "avail_mb=.. limited_by=.. budget_mb=.. gw_pool=<mb>/share=<mb>/scale=.." for the SENSING: HOST_MEM line */
void nr_host_membudget_note_failure(nr_host_mem_module_t m);    /* an allocation of the module failed: counted; returns nothing, the caller degrades */
uint64_t nr_host_membudget_failures(nr_host_mem_module_t m);
void nr_host_membudget_test_reset(void);
#ifdef __cplusplus
}
#endif
#endif
