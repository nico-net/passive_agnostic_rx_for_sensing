/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G5: GPU backend of the in-process channelizer (iq_chan.h), in the receiver's own CUDA context.
 * libshm_iq_chan_gpu.so (shm_iq_chan_gpu.cu, built with ENABLE_LDPC_CUDA) is dlopen'd by the CHAN path selector (iq_chan_gpu_sel.c) so that the receiver and
 * the default build keep no CUDA link dependency. Kernels: the T0 import of the old shm_iq_chan_gpu.cu (MPCIFIX ABI 2: fused mixer + first halfband
 * stage, grid.y = channels; per-element arithmetic in shm_iq_chan_kern.h), driven behind iq_chan_backend_t instead of a separate process.
 *   SHARED tier (integrated GPU with pageable access, GB10): zero copy, the kernels read the wideband ring (or the caller's block) and write the
 *   caller's output buffers in host memory directly;
 *   DISCRETE tier: pinned staging + explicit H2D / D2H copies on the channelizer stream.
 * Placement (plan B.6 item 2): (a) a top-priority stream in the real-time green context (all real-time SMs), (b) a top-priority stream in the
 * one-granule probe group of the derived SM plan (nr_gpu_sm_plan.h; DL never uses it). No green contexts: one top-priority stream, any SM.
 * Every allocation is booked in the GPU ledger (GPUMEM module "chan", SHARED) and taken from the GPU budget headroom (NR_GPU_MEM_CHAN); none after
 * the RX fence. Waits are polled (cudaEventQuery). */
#ifndef SHM_IQ_CHAN_GPU_H
#define SHM_IQ_CHAN_GPU_H
#include "iq_chan.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define IQ_CHAN_GPU_ABI 3

enum { IQ_CHAN_GPU_PLACE_RT = 1, IQ_CHAN_GPU_PLACE_GROUP = 2 }; /* (a), (b) of plan B.6 item 2 */

typedef struct {
  int decim;
  double shift_hz, passband_hz;
} iq_chan_gpu_cell_t;

typedef struct {
  long max_lsb, differing, total; /* |GPU - iq_ddc| over every output component */
  int ok;                         /* max_lsb <= 1 and differing <= total / 1000 */
} iq_chan_gpu_check_t;

typedef struct {
  double blocks, seconds;
  double wall_ms_per_block;  /* run() wall time per block (launch + wait), free running */
  double gpu_ms_per_block;   /* GPU time per block (events around the run) */
  double samples_per_s;      /* input samples per channel per second sustained */
  double rt_factor;          /* samples_per_s / in_rate */
  int sms;                   /* SMs the channelizer stream lands on (probe kernel), 0 = unknown */
} iq_chan_gpu_bench_t;

typedef struct {
  long runs;
  double gpu_ms, wall_ms; /* sums over every run() */
} iq_chan_gpu_stats_t;

typedef struct {
  int abi;
  const iq_chan_backend_t *backend; /* name "gpu" */
  int (*available)(void);           /* 1 = a device of a GPU tier (SHARED / DISCRETE) is usable */
  int (*tier_shared)(void);         /* 1 = zero-copy (SHARED) data path */
  /* GPU vs iq_ddc on caller-given input: nch channels of nblocks * block samples, every cell for every block. 0 = ran (verdict in out->ok). */
  int (*compare)(unsigned nch, double rate, int block, int nblocks, const iq_chan_gpu_cell_t *cells, int ncells, const int16_t *const *in,
                 iq_chan_gpu_check_t *out);
  size_t (*bytes)(const iq_chan_load_t *load, int block); /* device + pinned bytes a channelizer for `load` takes */
  int (*bench)(const iq_chan_load_t *load, int block, double seconds, iq_chan_gpu_bench_t *out);
  void (*set_placement)(int place); /* tests / benches: the placement of instances created afterwards (0 = the default) */
  int (*placement)(void);           /* the placement new instances use */
  void (*stats)(void *ctx, iq_chan_gpu_stats_t *st); /* of a backend instance (iq_chan's ctx) */
} iq_chan_gpu_api_t;

const iq_chan_gpu_api_t *iq_chan_gpu_api(void); /* the module's entry point (dlsym) */

/* ---- CHAN path selector (iq_chan_gpu_sel.c, part of mpci_iq; no CUDA) ---- */
const iq_chan_gpu_api_t *iq_chan_gpu_load(void);                /* the module next to libnr_gpu_platform.so, NULL if absent */
int iq_chan_gpu_selfcheck(const iq_chan_load_t *load, int block, iq_chan_gpu_check_t *res); /* GPU vs iq_ddc on the load's geometry; 0 = ran */
const iq_chan_backend_t *iq_chan_path_select(const iq_chan_load_t *load); /* the per-process decision (NULL = CPU); called by iq_chan_backend_select */
void iq_chan_gpu_sel_test_reset(void);                          /* tests: decide again at the next call (with nr_gpu_path_test_reset) */
#ifdef __cplusplus
}
#endif
#endif
