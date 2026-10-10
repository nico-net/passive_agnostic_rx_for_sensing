/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU-DEMOD stage 2 (ISAC_PDSCH_GPU_EXACT_CHEST=1, default off): the passive PDSCH channel estimator of one grant on
 * the GPU, bit-exact with nr_pdsch_channel_estimation()'s DMRS_TYPE1/TYPE2_linear_interp path: LS (chest_ls_type1/2),
 * the GPU6 bit-exact delay transform, the 16-tap FIR with the delay table, the inverse delay and the per-antenna noise.
 * One row = (DM-RS symbol, layer, antenna). Host side: nr_dl_channel_estimation.c (nr_pdsch_gpu_chest_grant). */
#ifndef NR_PDSCH_GPU_CHEST_H
#define NR_PDSCH_GPU_CHEST_H
#include <stdint.h>
#define NR_GPU_CHEST_MAX_ROWS 256
typedef struct {
  const int16_t *rxF;  /* c16: &rxdataF[a][symbol * N + delta] */
  const int16_t *pil;  /* c16: &pilot[(6 or 4) * rb_offset] of this (symbol, layer) */
  int16_t *out;        /* c16: N entries rewritten (the estimate row, or a scratch row for a PRG segment) */
  int k0, nb_rb;       /* first LS subcarrier (bwp_start_subcarrier % N) and contiguous RBs of this row */
} nr_gpu_chest_row_t;
typedef struct {
  int n_rows, type, N;
  const int16_t *delay_table; /* c16 [2 * MAX_DELAY_COMP + 1][stride] */
  int dt_stride, max_delay_comp;
  int16_t filt[4][16];        /* p0, p1p2, middle, last */
  nr_gpu_chest_row_t rows[NR_GPU_CHEST_MAX_ROWS];
  /* outputs */
  uint32_t nvar[NR_GPU_CHEST_MAX_ROWS];
  int delay[NR_GPU_CHEST_MAX_ROWS][3];
} nr_gpu_chest_job_t;
typedef int (*nr_gpu_chest_fn)(nr_gpu_chest_job_t *job);
#endif
