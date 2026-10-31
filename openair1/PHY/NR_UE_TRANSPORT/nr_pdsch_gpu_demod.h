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
#include "PHY/TOOLS/nr_gpu_mem.h"
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

/* XDIAG (EVMGRANT snrx= sdisp=): sums over the equaliser output the LLRs are cut from (comp, in the units of mag), per symbol, accumulated by k_tail (armed grants only).
 * Lattice mode (Qm > 2, or one layer: the QAM unit u per RE is mag / 2^(Qm/2 - 1), for QPSK m / sqrt 2 from |h|^2 as re_comp builds mag): e = sum |z/u - slice|^2.
 * Moment mode (QPSK of a multi-layer solve, no per-RE unit): ll[4..7] = sum |z|^2, ll[8..11] = sum |Re z| + |Im z|. ll[0..3] = REs counted (both modes). */
#define NR_GPU_XD_NG 72 /* groups of 48 packed REs per symbol: (NR_GPU_DEMOD_MAX_EXT + 47) / 48 rounded up */
/* RESID (slot-phase RE-pattern estimator, nr_slotphase_resid.h): the same layer-0 lattice error as eg / ng, keyed by the RE's POSITION instead of its packed index. Position from the
 * extraction index map (geo_y = rxdataF subcarrier k): rel = (k - rs_k0) mod rs_n, crb = rel / 12, sc = rel % 12. Key = sc + 12 * (crb & 3) (0..47), then NR_GPU_RS_BKT buckets of
 * 8 CRBs (crb / 8) in rs_e / rs_n[NR_GPU_RS_KEYS ..]. Written only when the job has want_resid (flagged slot phases and a bounded reference budget; else untouched). */
#define NR_GPU_RS_KEYS 48
#define NR_GPU_RS_BKT 36
#define NR_GPU_RS_N (NR_GPU_RS_KEYS + NR_GPU_RS_BKT)
typedef struct {
  float e[NR_GPU_DEMOD_NSYM][NR_GPU_DEMOD_MAX_NL];
  long long ll[NR_GPU_DEMOD_NSYM][12];
  float eg[NR_GPU_DEMOD_NSYM][NR_GPU_XD_NG], ng[NR_GPU_DEMOD_NSYM][NR_GPU_XD_NG]; /* layer 0, lattice mode: error sum and RE count per group */
  float rs_e[NR_GPU_DEMOD_NSYM][NR_GPU_RS_N];    /* RESID: layer 0, lattice mode: error sum per position key / 8-CRB bucket */
  uint32_t rs_n[NR_GPU_DEMOD_NSYM][NR_GPU_RS_N]; /* RESID: REs per position key / bucket */
} nr_gpu_xdiag_t;

typedef struct nr_gpu_demod_job_s {
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
  int cpe;         /* PHASE-TRACK: the per-symbol CPE tracker of nr_rx_pdsch() (k_cpe); 1 in the passive receiver */
  int tint;        /* PHASE-TRACK, graph pass only: k_tinterp interpolates the (device) estimate into the data-symbol planes first; the CPU does it into the host estimate */
  int tint_cols;   /* tint: allocation-relative subcarriers of the estimate rows */
  uint16_t tint_dm; /* the grant's DM-RS symbol mask (tint: the interpolation pairs; cpe: DM-RS symbols are not tracked) */
  int shift_sum;   /* ISAC_RX_MRC_SHIFT_SUM (MRCBUG): mode-3 headroom from the combined energy, not the branch count */
  int mag_per_layer;
  int wide_mrc;    /* nr_pdsch_decode_wide_mrc (1-layer QPSK recovery arm) */
  int want_kappa;  /* EVM armed: per-symbol kappa sums */
  int want_evm;    /* EVM armed: GPU EVM partial sums (else the host runs its CPU copy of the EVM block) */
  int evm_erase;   /* nr_dlsch_csi_erase_n > 0 */
  int want_resid;  /* RESID: k_tail also accumulates the position-keyed layer-0 error (xd.rs_e / rs_n); needs want_kappa */
  int rs_k0, rs_n; /* RESID: fp->first_carrier_offset and fp->ofdm_symbol_size, to turn geo_y's subcarrier k into (CRB, sc) */
  int evm_fp64;    /* GPU EVM per-RE math in double (else float; sums always double) */
  float m3_nvfac_f; /* demod_fp32: (float)m3_nvfac, set by the launch entry points (no double reaches the float kernels) */
  int demod_fp32;  /* GPUPASS Task 5: the float instantiation of the align / multi-layer MMSE kernels (graph pass on an RTX 4070; not bit-exact, 0 = the bit-exact double one) */
  int qam_mode;    /* nl >= 2: nr_rx_pdsch()'s resolved ISAC_PDSCH_MMSE_MODE, 2 (float ZF) or 0 (stock fixed-point MMSE, nl >= 3),
                      3 (exact double MMSE, ISAC_PDSCH_GPU_EXACT_DEMOD_MMSE3=1) */
  uint32_t nvar;   /* nr_rx_pdsch()'s nvar (stock MMSE diagonal loading nvar >> 3) */
  double m3_nvfac; /* mode 3, nbRx <= nl: s2 = nvar x m3_nvfac (pow(10, ISAC_PDSCH_MMSE_EXACT_NVAR_DB / 10), host libm as the CPU) */
  uint32_t nvar_ant[4];                   /* rank-1 mode 3: the chest's per-RX DM-RS residual noise (nr_dl_chest_nvar_ant) = the branch weights of the noise-weighted MRC; all 0 = no weighting */
  const uint32_t *nvar_ant_dev;           /* GPUPASS: non-null = the per-RX noise is read from here (the chest post's nvar_ant), `nvar_ant` above is ignored (set by the module from nvar_dev) */
  const void *irc_w;                      /* D4 IRC: device-accessible whitening matrices of the DM-RS interference covariance, irc_ng groups of NR_IRC_WSTRIDE values (double,
                                           * or float when demod_fp32), nr_irc_final(); null = scalar noise model. Mode 3 (qam_mode 3, nl >= 2) only. */
  int irc_ng, irc_nre; /* groups, and the REs of a full symbol of the grant (the group of a data RE: nr_irc_group_of) */
  const uint32_t *nvar_dev;               /* GPUPASS: non-null = nvar is read from here when the kernels run (the chest post's output), `nvar` above is ignored */
  const struct nr_gpu_demod_job_s *jref;  /* GPUPASS: non-null = the kernels read the job from here instead of their by-value copy (graph replay: the job is rewritten
                                           * between replays; the by-value copy is that of the capture). Device-accessible, at least offsetof(evm) bytes. */
  int stage_full;  /* Q4, DISCRETE tier only (ignored on DIRECT): 1 = copy every output back to the caller (comp / mag* of all layers: a checked grant is compared with the CPU);
                    * 0 = only what the decoder reads (the LLRs and comp layer 0 of the symbol with the most REs: the Qm oracle) */
  int evm_defer;   /* DECGPU: return once the LLRs are done; the EVM chain completes later (nr_gpu_demod_evm_wait_fn) */
  int evm_pending; /* out: 1 = the EVM partial sums are not in evm[] yet, the context stays held until the wait */
  /* outputs */
  int32_t log2_maxh;
  int used_branch_set, used_branch, mrc_rx_index, mrc_nb_rx;
  float irc_gain_db; /* D4 IRC, graph pass: mean whitening gain estimate over the usable groups (valid when irc_active > 0) */
  int irc_active;    /* groups whose DM-RS covariance was usable */
  int64_t kappa_sz[NR_GPU_DEMOD_NSYM][NR_GPU_DEMOD_MAX_NL], kappa_sm[NR_GPU_DEMOD_NSYM][NR_GPU_DEMOD_MAX_NL];
  /* extraction buffers after the branch mask and the MRC alignment (host-readable while the job's context is held) */
  const int16_t *y_ext; /* c16 [14][NR_GPU_DEMOD_MAX_RX][NR_GPU_DEMOD_MAX_EXT] */
  const int16_t *h_ext; /* c16 [14][16][NR_GPU_DEMOD_MAX_EXT], row l * nbRx + a */
  nr_gpu_evm_sym_t evm[NR_GPU_DEMOD_NSYM];
  nr_gpu_xdiag_t xd;     /* out, after evm: not part of the kernels' by-value job (offsetof(evm)) */
  float cpe_phi[NR_GPU_DEMOD_NSYM][NR_GPU_DEMOD_MAX_NL]; /* out: PHASE-TRACK, the CPE (rad) k_cpe applied per symbol and layer, 0 = none (counters only) */
  int ctx;              /* context held by nr_pdsch_gpu_demod_run until nr_pdsch_gpu_demod_release */
} nr_gpu_demod_job_t;

/* Module entry points (dlsym'd from libpdsch_gpu.so). */
typedef int (*nr_gpu_demod_init_fn)(int nant);
/* 0: done, outputs written, context held (call release); < 0: nothing written, the caller runs the CPU path. */
typedef int (*nr_gpu_demod_run_fn)(nr_gpu_demod_job_t *job);
typedef void (*nr_gpu_demod_release_fn)(int ctx);
/* Q4: DISCRETE staging of nr_pdsch_gpu_demod_run (module export nr_pdsch_gpu_demod_stage_init, called after the init, before RX starts): device buffers for
 * the span of the slot the kernels read; sized from the shared memory budget. 0 = ready (also on DIRECT: nothing to stage), -1 = off (the run refuses, the CPU demodulates).
 * N: ofdm_symbol_size, ext_max: (N_RB * 12 + 15) & ~15. */
typedef int (*nr_gpu_demod_stage_init_fn)(int nbRx, int N, uint32_t ext_max);
/* GPUPASS launch-only variant (nr_pdsch_gpu_demod.cu): enqueues the demod of one grant on `stream`, LLRs into llr_dev; no wait, no
 * host-side results, no EVM. 0 or -1. */
typedef int (*nr_gpu_demod_dev_fn)(const nr_gpu_demod_job_t *job, nr_gpu_buf_t *llr_dev, void *stream);
/* Graph-pass variants (nr_gpu_pass_graph.cu): each pass instance (0 <= inst < NR_GPU_PASS_RING) owns a context, its job block and its decision block. _inst_dev enqueues
 * the demod of instance `inst` reading the job from the instance's job block (jb: nr_gpu_demod_pass_job_block), including the EVM chain on a forked branch when the
 * block's job wants it, and ends with the D2H of the decision/EVM block. _pass_set writes a job into the block (host only). _pass_out copies the decisions and
 * the EVM sums of the instance's last pass into `job` (valid after the pass finished). */
typedef int (*nr_gpu_demod_pass_dev_fn)(int inst, const nr_gpu_demod_job_t *job_capture, nr_gpu_buf_t *llr_dev, const uint32_t *nvar_dev, void *stream, void *stream2);
typedef int (*nr_gpu_demod_pass_set_fn)(int inst, const nr_gpu_demod_job_t *job);
typedef void (*nr_gpu_demod_pass_out_fn)(int inst, nr_gpu_demod_job_t *job);
/* evm_pending jobs: waits for the context's EVM chain and fills job->evm (the context stays held). 0 = ok. */
typedef int (*nr_gpu_demod_evm_wait_fn)(nr_gpu_demod_job_t *job);

#ifdef __cplusplus
}
#endif
#endif
