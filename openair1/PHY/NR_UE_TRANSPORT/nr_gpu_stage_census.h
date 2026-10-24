/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Q4: census of the staged DISCRETE variants of the standalone (non-pass) GPU entries of libpdsch_gpu.so.
 * On the DISCRETE tier every standalone entry copies exactly the input span its kernels read to the device (explicit async H2D),
 * runs on its own stream and copies exactly the outputs the host consumes back (D2H); the DIRECT tier (GB10) never touches these
 * counters. Per entry: runs on the staged path, copies and bytes of both directions, runs refused because the staging did not
 * fit or was not set up (the caller then runs the CPU). The host reads them with nr_gpu_stage_stats() (one dlsym per module)
 * and adds them to its existing census lines. */
#ifndef NR_GPU_STAGE_CENSUS_H
#define NR_GPU_STAGE_CENSUS_H
#include <stdint.h>
#include <stddef.h>
#include "PHY/TOOLS/nr_gpu_membudget.h"
/* Device bytes one standalone entry may spend on its staging: 2 % of the shared device-memory budget B (T6b), unlimited when the device memory is unknown.
 * Each entry sizes its contexts / rows with nr_gpu_membudget_fit() against this share and turns itself off (the caller runs the CPU) when even the floor does not fit. */
static inline size_t nr_gpu_stage_share(void)
{
  const size_t b = nr_gpu_membudget_total();
  return b == SIZE_MAX ? SIZE_MAX : b / 100 * 2;
}
typedef enum {
  NR_GPU_STG_FEP,      /* exact FEP (nr_pdsch_gpu_exact_fep2) */
  NR_GPU_STG_DELAY,    /* delay estimate of LS rows (nr_pdsch_gpu_exact_delay) */
  NR_GPU_STG_CHEST,    /* bit-exact chest (nr_pdsch_gpu_exact_chest) */
  NR_GPU_STG_DEMOD,    /* demod (nr_pdsch_gpu_demod_run) */
  NR_GPU_STG_ULFEP,    /* UL FEP (nr_pusch_gpu_fep_run) */
  NR_GPU_STG_LDPC_DL,  /* standalone PDSCH LDPC (nr_pdsch_gpu_ldpc_run) */
  NR_GPU_STG_LDPC_UL,  /* standalone PUSCH LDPC (nr_pusch_gpu_ldpc_run) */
  NR_GPU_STG_N
} nr_gpu_stage_entry_t;
/* o[0] runs, o[1] h2d bytes, o[2] d2h bytes, o[3] h2d copies, o[4] d2h copies, o[5] refused (no staging); entry < NR_GPU_STG_N */
typedef void (*nr_gpu_stage_stats_fn)(int entry, uint64_t *o);
#ifdef __cplusplus
#include <atomic>
namespace nr_stage {
inline std::atomic<uint64_t> g_c[NR_GPU_STG_N][6];
inline void note(int e, uint64_t h2d_bytes, int h2d_n, uint64_t d2h_bytes, int d2h_n)
{
  g_c[e][0].fetch_add(1, std::memory_order_relaxed);
  g_c[e][1].fetch_add(h2d_bytes, std::memory_order_relaxed);
  g_c[e][2].fetch_add(d2h_bytes, std::memory_order_relaxed);
  g_c[e][3].fetch_add((uint64_t)h2d_n, std::memory_order_relaxed);
  g_c[e][4].fetch_add((uint64_t)d2h_n, std::memory_order_relaxed);
}
inline void refused(int e) { g_c[e][5].fetch_add(1, std::memory_order_relaxed); }
} // namespace nr_stage
#endif
#endif
