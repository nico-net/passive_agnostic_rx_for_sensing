/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU-DEMOD: bit-exact GPU port of nr_rx_pdsch()'s demodulation for the passive PDSCH decoder
 * (ISAC_PDSCH_GPU_EXACT_DEMOD=1, default off). The CPU side computes the RE geometry with the real
 * nr_dlsch_extract_rbs() (nr_dlsch_extract_geometry), so any rate-matching change carries over; the GPU then
 * gathers, decides the receive branches and the output shift, phase-aligns the MRC branches (ISAC_RX_MRC_ALIGN),
 * compensates, and writes the layer-demapped LLRs straight into the decoder's buffer.
 * Module side: nr_pdsch_gpu_demod.cu (in libpdsch_gpu.so). Host side: nr_pdsch_gpu_demod_host.c. */
#ifndef NR_PDSCH_GPU_DEMOD_H
#define NR_PDSCH_GPU_DEMOD_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NR_GPU_DEMOD_NSYM 14
#define NR_GPU_DEMOD_MAX_RX 4
#define NR_GPU_DEMOD_MAX_NL 4
#define NR_GPU_DEMOD_MAX_EXT 3296 /* (275 * 12 + 15) & ~15 */

/* GPU EVM partial sums of one symbol (nr_rx_pdsch()'s ISAC_PDSCH_EVM_GRANT block), folded by the host */
typedef struct {
  int valid, nb, rbmap, row;
  uint32_t nv, nterm;
  double ideal_pow;
  double err_l[4], qr[4], qi[4], qd[4], qx[4]; /* per layer (Nl <= 4) */
  double cr[4], ci[4], P[4], Y[4];
  double Cr[4][4], Ci[4][4];
  float err_k[12];
  uint32_t n_k[12];
  float err_rb[276];
  uint16_t n_rb[276];
} nr_gpu_evm_sym_t;

typedef struct {
  /* grant shape */
  int nbRx, nl, qm, N;
  int start_symbol, nb_symbols, first_symbol; /* first_symbol: the symbol nr_rx_pdsch() gets first_symbol_flag on */
  uint16_t sym_mask;                          /* symbols nr_rx_pdsch() would be called on */
  uint32_t rx_stride, est_stride, buf, ext;   /* rxdataF row, estimate row, comp/mag row, extraction row */
  uint16_t nre[NR_GPU_DEMOD_NSYM];            /* packed REs per symbol (nr_dlsch_extract_rbs return value) */
  uint32_t llr_off[NR_GPU_DEMOD_NSYM];        /* first LLR of each symbol in the decoder buffer */
  const int16_t *rxdataF;                     /* c16 [nbRx][rx_stride] */
  const int16_t *est;                         /* c16 [nl * nbRx][est_stride] */
  const int16_t *geo_y;                       /* c16 [14][ext]: {k, 1} = rxdataF symbol index k, {0, 0} = erased */
  const int16_t *geo_h;                       /* c16 [14][ext]: {j, s + 1} = estimate row symbol s, index j */
  int16_t *comp, *mag, *magb, *magr;          /* c16 [14][NR_MAX_NB_LAYERS][buf], the caller's buffers */
  int16_t *llr;                               /* decoder LLRs */
  /* decision inputs (resolved by the host exactly as nr_rx_pdsch() resolves them) */
  int mode;        /* ISAC_RX_MRC_MODE or the per-grant override, 0..3 */
  double thr_coef; /* pow(10, -ISAC_RX_BRANCH_MIN_DB / 10) */
  int l2_delta;    /* ISAC_L2MAXH_DELTA */
  int align;       /* ISAC_RX_MRC_ALIGN */
  int shift_sum;   /* ISAC_RX_MRC_SHIFT_SUM (MRCBUG): mode-3 headroom from the combined energy, not the branch count */
  int mag_per_layer;
  int wide_mrc;    /* nr_pdsch_decode_wide_mrc (1-layer QPSK recovery arm) */
  int want_kappa;  /* EVM armed: per-symbol kappa sums */
  int want_evm;    /* EVM armed: GPU EVM partial sums (else the host runs its CPU copy of the EVM block) */
  int evm_erase;   /* nr_dlsch_csi_erase_n > 0 */
  int evm_fp64;    /* GPU EVM per-RE math in double (else float; sums always double) */
  int qam_mode;    /* nl >= 2: nr_rx_pdsch()'s resolved ISAC_PDSCH_MMSE_MODE, 2 (float ZF) or 0 (stock fixed-point MMSE, nl >= 3) */
  uint32_t nvar;   /* nr_rx_pdsch()'s nvar (stock MMSE diagonal loading nvar >> 3) */
  int evm_defer;   /* DECGPU: return once the LLRs are done; the EVM chain completes later (nr_gpu_demod_evm_wait_fn) */
  int evm_pending; /* out: 1 = the EVM partial sums are not in evm[] yet, the context stays held until the wait */
  /* outputs */
  int32_t log2_maxh;
  int used_branch_set, used_branch, mrc_rx_index, mrc_nb_rx;
  int64_t kappa_sz[NR_GPU_DEMOD_NSYM][NR_GPU_DEMOD_MAX_NL], kappa_sm[NR_GPU_DEMOD_NSYM][NR_GPU_DEMOD_MAX_NL];
  /* extraction buffers after the branch mask and the MRC alignment (host-readable while the job's context is held) */
  const int16_t *y_ext; /* c16 [14][NR_GPU_DEMOD_MAX_RX][NR_GPU_DEMOD_MAX_EXT] */
  const int16_t *h_ext; /* c16 [14][16][NR_GPU_DEMOD_MAX_EXT], row l * nbRx + a */
  nr_gpu_evm_sym_t evm[NR_GPU_DEMOD_NSYM];
  int ctx;              /* context held by nr_pdsch_gpu_demod_run until nr_pdsch_gpu_demod_release */
} nr_gpu_demod_job_t;

/* Module entry points (dlsym'd from libpdsch_gpu.so). */
typedef int (*nr_gpu_demod_init_fn)(int nant);
/* 0: done, outputs written, context held (call release); < 0: nothing written, the caller runs the CPU path. */
typedef int (*nr_gpu_demod_run_fn)(nr_gpu_demod_job_t *job);
typedef void (*nr_gpu_demod_release_fn)(int ctx);
/* evm_pending jobs: waits for the context's EVM chain and fills job->evm (the context stays held). 0 = ok. */
typedef int (*nr_gpu_demod_evm_wait_fn)(nr_gpu_demod_job_t *job);

#ifdef __cplusplus
}
#endif
#endif
