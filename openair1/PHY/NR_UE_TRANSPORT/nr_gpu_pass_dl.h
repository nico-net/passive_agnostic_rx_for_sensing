/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 4, host side (nr_gpu_pass_dl.c, in the PHY UE library): loads libpdsch_gpu.so's graph pass (nr_gpu_pass.h), warms the shape classes before the RX
 * fence and turns the state of passive_decode_body_core at the point just before its FEP into an nr_gpu_pass_job_t. The body calls nr_gpu_pass_dl_run() with the
 * facts only it knows (the statics behind its gates) and commits the result itself. ISAC_GPU_PASS=1 enables it (default off: nothing here runs). */
#ifndef NR_GPU_PASS_DL_H
#define NR_GPU_PASS_DL_H
#include "PHY/defs_nr_UE.h"
#include "nr_gpu_pass.h"
#include "nr_ssb_rate_match.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h"

typedef struct {
  PHY_VARS_NR_UE *ue;
  const UE_nr_rxtx_proc_t *proc;
  fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config;
  const freq_alloc_bitmap_t *freq_alloc;
  const fapi_nr_dl_cw_info_t *cw;   /* TBS, base graph, rv, Qm, Nl of the grant */
  const nr_ssb_rm_mask_t *ssb_rm;   /* what the demod gets as ssb_unav ? &ssb.dem : &no_ssb */
  c16_t *rxdataF;                   /* the caller's rxdataF[nbRx][rx_stride] */
  uint32_t rx_stride;
  uint16_t rnti;
  uint32_t G, tbslbrm, est_size, rx_size_symbol;
  uint16_t dmrs_full;               /* every DM-RS symbol of the slot (the estimate always covers them all) */
  int first_symbol;                 /* first_symbol_with_data */
  int nvfix, branch;                /* ISAC_RX_NVAR_FIX, nr_dlsch_planned_branch() when ISAC_RX_NVAR_PERBRANCH else -1 */
  int norm;                         /* ISAC_LLR_NORM on */
  int it_max;                       /* max_ldpc_iterations of passive_ldpc_decode_core */
  int chest_diag;                   /* chest_pdp_diag_on() */
  double fep_fo;                    /* the FEP frequency offset the old path would use (must be 0) */
  uint8_t *tb;                      /* receives the transport block (g_harq.b) */
} nr_gpu_pass_dl_in_t;

typedef struct {
  nr_gpu_pass_result_t r;
  nr_gpu_demod_job_t demod;         /* decisions, kappa and EVM sums of the demod (r.demod_out) */
  int16_t brfo_rows[4 * 2 * 4096 * 2];
  int16_t qm_row[4096 * 2];         /* the equalised layer-0 symbols of the symbol with the most data REs (the QM oracle's input), DIRECT only */
  uint32_t qm_n, dl_valid_re[14];
  int qm_sym, ndmrs, dmrs_first, dmrs_last, C;
  uint32_t nvar_ant[4];             /* the chest's per-antenna noise of the last DM-RS symbol and layer (nr_dl_chest_nvar_ant) */
  uint32_t K, F, Z, E, R;           /* TB parameters of the pass (the body's t_seg_* statics) */
  int BG;
  int16_t *llr;                     /* heap [G]: the final LLRs of a pass whose TB failed its CRC (the retry arms re-decode them) */
} nr_gpu_pass_dl_out_t;

/* 1 = ISAC_GPU_PASS=1 and the module is up (set by nr_gpu_pass_warmup) */
int nr_gpu_pass_active(void);
/* before the RX fence, after nr_pdsch_gpu_exact_warmup returned 1 */
void nr_gpu_pass_warmup(PHY_VARS_NR_UE *ue);
/* 0 = the pass ran (out filled; the TB may still have failed CRC or need the CPU decoder: out->r.tb_ok / need_cpu), 1 = not handled (counted), the old path decodes */
int nr_gpu_pass_dl_run(const nr_gpu_pass_dl_in_t *in, nr_gpu_pass_dl_out_t *out);
/* body-side events for the census */
enum { NR_GPU_PASS_EV_COMMIT, NR_GPU_PASS_EV_RETRY_OLD /* pass said CRC fail / CPU: the old path decodes */, NR_GPU_PASS_EV_ZERO_TB, NR_GPU_PASS_EV_LIFETIME, NR_GPU_PASS_EV_SHADOW,
       NR_GPU_PASS_EV_SHADOW_BAD, NR_GPU_PASS_EV_CONT_FAIL /* CRC fail of the pass continued into the retry arms, no old first body */,
       NR_GPU_PASS_EV_OLD_FIRST /* old first body executed although a pass was attempted */, NR_GPU_PASS_EV_N };
void nr_gpu_pass_note(int ev);
/* shadow check cadence: 1 = this pass is also decoded by the old path and the verdicts compared (ISAC_GPU_PASS_CHECK=N, default 64, 0 = off) */
int nr_gpu_pass_shadow_due(void);
/* a shadow mismatch: the pass is switched off for the rest of the run */
void nr_gpu_pass_disable(const char *why);
#endif
