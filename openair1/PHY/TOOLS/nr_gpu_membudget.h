/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_MEMBUDGET_H
#define NR_GPU_MEMBUDGET_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One shared device-memory budget for all GPU modules (T6b).
 *   B = 0.85 * free_at_probe - NR_GPU_MB_RESERVE (the CUDA context remainder, module images, cuFFT plans and the unscaled PDSCH demod/LDPC contexts).
 *   Each scalable module gets share = B * weight / 100 and sizes its main knob so its device need fits that share. When the share covers the module's
 *   full need the scale is exactly 1 (GB10: 128 GB unified, every module at today's size). Unknown memory (no device, free_at_probe == 0): unlimited.
 *   Weights follow the 273 PRB 4 RX census on the 8 GB RTX 4060 Ti [MEASURED, device MB]: csirs ~3460 at 640 slots (fixed 1380 incl. 178 scratch and ~480 local memory of refine_kernel, 3.25 per slot; floor 160 slots = 1900),
 *   ldpc pool 620, ldpc cb0 874, td_cb0 556, pdcch_nid 776, pass 361, graphs up to 2048: about proportional to need, csirs raised so its floor fits an 8 GB card, pass raised to cover its instances + HARQ, graphs take their share plus whatever
 *   the other modules did not use (nr_gpu_membudget_graph_bytes). */
typedef enum { NR_GPU_MEM_CSIRS, NR_GPU_MEM_LDPC_POOL, NR_GPU_MEM_LDPC_CB0, NR_GPU_MEM_TD_CB0, NR_GPU_MEM_PDCCH_NID, NR_GPU_MEM_PASS, NR_GPU_MEM_GRAPH,
               NR_GPU_MEM_N } nr_gpu_mem_module_t;
#define NR_GPU_MB_RESERVE ((size_t)768 << 20)
static const int nr_gpu_mem_weight[NR_GPU_MEM_N] = {36, 8, 11, 7, 10, 8, 20};
static const char *const nr_gpu_mem_name[NR_GPU_MEM_N] = {"csirs", "ldpc_pool", "ldpc_cb0", "td_cb0", "pdcch_nid", "pass", "graph"};
/* Pure functions (no CUDA, no state). */
static inline size_t nr_gpu_membudget_calc(size_t free_at_probe)
{
  if (!free_at_probe) return SIZE_MAX; /* unknown */
  const size_t usable = free_at_probe / 20 * 17; /* 0.85 */
  return usable > NR_GPU_MB_RESERVE ? usable - NR_GPU_MB_RESERVE : 0;
}
static inline size_t nr_gpu_membudget_share_of(size_t budget, nr_gpu_mem_module_t m)
{
  if (budget == SIZE_MAX) return SIZE_MAX;
  return budget / 100 * (size_t)nr_gpu_mem_weight[m];
}
/* min(1, share / need); exactly 1.0 when the share covers the need (or need == 0). */
static inline double nr_gpu_membudget_scale_of(size_t share, size_t need)
{
  if (!need || share >= need) return 1.0;
  return (double)share / (double)need;
}
/* Largest k in [lo, hi] (hi = the full-size knob) with bytes(k) = fixed + per_unit * k <= share; 0 when even lo does not fit (module not ready).
 * hi is returned unchanged whenever it fits, so a large budget never changes a size. */
static inline long nr_gpu_membudget_fit(size_t share, size_t fixed, size_t per_unit, long lo, long hi)
{
  if (share == SIZE_MAX || fixed + per_unit * (size_t)hi <= share) return hi;
  if (share <= fixed || !per_unit) return 0;
  const long k = (long)((share - fixed) / per_unit);
  return k < lo ? 0 : k;
}
/* Process-wide state (nr_gpu_platform.c). */
size_t nr_gpu_membudget_total(void);                          /* B, bytes; SIZE_MAX = unlimited. NR_GPU_MEM_BUDGET_MB overrides free_at_probe (diagnostic, logged) */
size_t nr_gpu_membudget_share(nr_gpu_mem_module_t m);         /* bytes; SIZE_MAX = unlimited */
void nr_gpu_membudget_commit(nr_gpu_mem_module_t m, size_t bytes, double scale); /* the size the module chose (report, graph remainder); bytes 0 = not ready */
size_t nr_gpu_membudget_graph_bytes(void);                    /* graph share + (share - committed) of every module that committed less than its share; floor 0 */
int nr_gpu_membudget_format(char *out, size_t n);             /* "budget_mb=.. csirs=<mb>/<scale> ..." for the SENSING: GPU_MEM line */
void nr_gpu_membudget_test_reset(void);
#ifdef __cplusplus
}
#endif
#endif
