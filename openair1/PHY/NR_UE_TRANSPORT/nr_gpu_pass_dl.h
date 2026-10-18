/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 4, host side (nr_gpu_pass_dl.c, in the PHY UE library): loads libpdsch_gpu.so's graph pass (nr_gpu_pass.h), warms the shape classes before the RX
 * fence and turns the state of passive_decode_body_core at the point just before its FEP into an nr_gpu_pass_job_t. The body calls nr_gpu_pass_dl_run() with the
 * facts only it knows (the statics behind its gates) and commits the result itself. ISAC_GPU_PASS=1 enables it (default off: nothing here runs). */
#ifndef NR_GPU_PASS_DL_H
#define NR_GPU_PASS_DL_H
#include "PHY/defs_nr_UE.h"
#include "nr_gpu_pass.h"
#include "nr_gpu_graph_pool.h"
#include "nr_ssb_rate_match.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h"

typedef struct {
  PHY_VARS_NR_UE *ue;
  const UE_nr_rxtx_proc_t *proc;
  fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config;
  const freq_alloc_bitmap_t *freq_alloc;
  const nr_prb_seg_t *seg;          /* the PRG / VRB-bundle segments of a segmented grant (seg_path), data order; NULL / nseg 0 = the contiguous grant */
  int nseg;
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
  /* v2 Task 2 (ISAC_PDSCH_GPU_GRAPH_COVER): a layout probe decodes code block 0 only (with the full-C E / K / Z) and is never a decoded TB (out->r.tb_ok = 0);
   * probe_last_sym: its symbol horizon (-1 = the whole grant; the body's rule), set for every grant */
  int probe, probe_last_sym;
  int gate_on;                      /* Fix A+D: the in-graph no-chance gate (nr_snr_gate.h): required SNR - 5 dB in gate_thr_db; 0 = never skip */
  float gate_thr_db;
  /* the band the channel estimate covers (the slot share widens it upward; nr_pdsch_passive_decode.c share_chest_band); NULL = the grant's. Contiguous grants only */
  const freq_alloc_bitmap_t *chest_fa;
  const fapi_nr_dl_config_dlsch_pdu_rel15_t *chest_cfg;
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
int nr_gpu_pass_fp32_shadow(int flip, int sign_bad); /* FP32 shadow outcome of one grant; 1 = the pass was disabled */
int nr_gpu_pass_fp32_on(void); /* Task 5: the graphs run the FP32 demod; the shadow checks then compare LLR signs and CRC verdicts, not hashes */
/* before the RX fence, after nr_pdsch_gpu_exact_warmup returned 1 */
void nr_gpu_pass_warmup(PHY_VARS_NR_UE *ue);
/* 0 = the pass ran (out filled; the TB may still have failed CRC or need the CPU decoder: out->r.tb_ok / need_cpu), 1 = not handled (counted), the old path decodes */
int nr_gpu_pass_seg_max(void); /* segments of the largest segment-count class the pass covers: 32 in v2 split mode, else 16 */
int nr_gpu_pass_dl_run(const nr_gpu_pass_dl_in_t *in, nr_gpu_pass_dl_out_t *out);
/* v2 Task 4 (ISAC_PDSCH_GPU_GRAPH_RETRY + _SPLIT): around one nr_pdsch_passive_decode call, the pass instance its first body takes stays held for the later bodies, which
 * skip the chest sub-graph when its inputs are unchanged. begin returns 1 when held (then call end). dirty: a body ran the stage path (it may rewrite rxdataF). */
int nr_gpu_graph_call_begin(void);
void nr_gpu_graph_call_end(void);
void nr_gpu_graph_call_dirty(void);
/* v2 Task 1 (ISAC_PDSCH_GPU_GRAPH=1): a set of the pre-fence pool (nr_gpu_graph_pool.h) for a thread that reaches the pass after the RX fence; sets *shadow and the
 * thread's job buffer. NULL = no pool (v2 off) or pool empty (counted). The set returns to the pool when the thread exits. */
nr_gpu_pass_dl_out_t *nr_gpu_graph_pool_claim(uint8_t **shadow);
int nr_gpu_graph_pool_has_free(void); /* v2 and a set is free (the body's nomem gate) */
/* body-side events for the census */
enum { NR_GPU_PASS_EV_COMMIT, NR_GPU_PASS_EV_RETRY_OLD /* pass said CRC fail / CPU: the old path decodes */, NR_GPU_PASS_EV_ZERO_TB, NR_GPU_PASS_EV_LIFETIME, NR_GPU_PASS_EV_SHADOW,
       NR_GPU_PASS_EV_SHADOW_BAD, NR_GPU_PASS_EV_CONT_FAIL /* CRC fail of the pass continued into the retry arms, no old first body */,
       NR_GPU_PASS_EV_OLD_FIRST /* old first body executed although a pass was attempted */, NR_GPU_PASS_EV_SHADOW_FAIL, NR_GPU_PASS_EV_SHADOW_FAIL_BAD,
       NR_GPU_PASS_EV_FP32_FLIP /* FP32 pass: a shadow where pass and old path disagree on the CRC verdict only (tolerated, counted) */,
       NR_GPU_PASS_EV_PROBE /* v2: a layout probe's pass committed (never a decoded TB, so not a cont_fail) */, NR_GPU_PASS_EV_N };
/* gates of passive_decode_body_core that keep a grant off the pass (the first one that refuses, counted) */
enum { NR_GPU_PASS_GATE_SEG /* more segments than the largest class (16; 32 in split mode) */, NR_GPU_PASS_GATE_PROBE, NR_GPU_PASS_GATE_SHARE, NR_GPU_PASS_GATE_GW, NR_GPU_PASS_GATE_PTRS, NR_GPU_PASS_GATE_TINTERP, NR_GPU_PASS_GATE_HARQ,
       NR_GPU_PASS_GATE_LDPC_OFF, NR_GPU_PASS_GATE_CHEST_ONLY, NR_GPU_PASS_GATE_OCC, NR_GPU_PASS_GATE_NOMEM, NR_GPU_PASS_GATE_RETRY /* ISAC_GPU_PASS_RETRY_OLD: a retry body runs the old path */, NR_GPU_PASS_GATE_N };
void nr_gpu_pass_gate(int gate);
/* every ISAC_GPU_PASS_CHECK-th CRC-failing pass: also run the old first body and compare */
int nr_gpu_pass_shadow_due_fail(void);
/* consumer start, before the RX fence: the per-thread job and LLR copy buffers, allocated and prefaulted */
void nr_gpu_pass_dl_prewarm(nr_gpu_pass_dl_out_t *out);
void nr_gpu_pass_note(int ev);
/* shadow check cadence: 1 = this pass is also decoded by the old path and the verdicts compared (ISAC_GPU_PASS_CHECK=N, default 64, 0 = off) */
int nr_gpu_pass_shadow_due(void);
/* a shadow mismatch: the pass is switched off for the rest of the run */
void nr_gpu_pass_disable(const char *why);
#endif
