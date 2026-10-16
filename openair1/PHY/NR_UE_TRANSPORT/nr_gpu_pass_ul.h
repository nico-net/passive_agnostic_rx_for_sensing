/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 7, host side of the PUSCH (UL) graph pass (nr_gpu_pass_ul.c, in the PHY UE library). ISAC_UL_GPU_PASS=1 enables it (default off: nothing here runs).
 *
 *   - ul_hyp_cache: the per-RNTI hypothesis a locked grant decodes with (timing advance, DM-RS identity and layout). A hit lets the decoder skip the timing search and the
 *     DM-RS-id discovery and go straight into the GPU pass; a miss leaves the existing path untouched. Filled by the decoder when a grant of the RNTI decodes (and, in
 *     Stage 3, by the background search). The two functions below are the stable API Stage 3 consumes.
 *   - nr_gpu_pass_ul_run: turns a plain description of one grant (the PDU the old path builds, the cell geometry, the UL snapshot windows) into an nr_gpu_pass_job_t, runs
 *     the pass and folds its outputs into what nr_rx_pusch_group_tp / nr_ulsch_decoding would have left behind (nvar, the DM-RS SNR, the delay of the TA refinement). It takes
 *     no gNB and no UE, so the unit test drives it from synthetic slots. */
#ifndef NR_GPU_PASS_UL_H
#define NR_GPU_PASS_UL_H
#include <stdint.h>
#include "nr_gpu_pass.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int32_t ta;           /* applied timing advance of the FFT window (samples, nr_symbol_fep_ul convention: positive reads earlier) */
  uint16_t dmrs_mask;   /* ul_dmrs_symb_pos the grant decoded with (only when have_dmrs) */
  int32_t dmrs_id;      /* ul_dmrs_scrambling_id */
  uint8_t nscid, dmrs_type, mapping_type, add_pos, front_load, n_cdm_groups, have_dmrs;
  uint8_t start_symbol, num_symbols; /* the grant shape the DM-RS fields were found for: they apply to a grant of the same shape only */
  uint16_t dmrs_ports;
  uint8_t tp;           /* transform precoding of the grant it was found for: an entry applies to grants with the same flag only (ISAC_UL_BGDISC) */
  uint8_t fail_streak;  /* passes that failed the CRC since the last decode: the entry is dropped after three */
  uint32_t epoch;       /* nr_cfg_epoch_dedicated_current() when it was stored: a later epoch invalidates it */
  int64_t abs_slot;     /* slot index it was stored at (age) */
  uint32_t n_ok;        /* decodes that refreshed it */
} ul_hyp_t;

/* 1 and *h filled when the RNTI has an entry, else 0. Thread-safe. */
int ul_hyp_cache_get(uint16_t rnti, ul_hyp_t *h);
/* inserts or replaces the entry of the RNTI (64 entries, least recently written is evicted). Thread-safe. */
void ul_hyp_cache_put(uint16_t rnti, const ul_hyp_t *h);
/* the bg worker's put: skipped (0) when the entry holds a decode-refreshed (n_ok > 0) or newer-slot hypothesis */
int ul_hyp_cache_put_if_new(uint16_t rnti, const ul_hyp_t *h);
void ul_hyp_cache_clear(void);
void ul_hyp_cache_drop(uint16_t rnti);
/* the call site's lookup: counts hit / miss / stale (epoch differs or older than ISAC_UL_HYP_MAX_AGE slots, default 2000); 1 = usable hit */
int nr_gpu_pass_ul_hyp_lookup(uint16_t rnti, uint32_t epoch, int64_t abs_slot, ul_hyp_t *h);
void nr_gpu_pass_ul_cache_counts(uint64_t *hit, uint64_t *miss, uint64_t *stale, uint64_t *put);

#define NR_GPU_PASS_UL_FB_N 12
typedef struct {
  /* ---- the grant, as the PDU of the old path states it */
  uint16_t rnti;
  uint8_t slot;
  int start_symbol, num_symbols, num_rb, bwp_start, rb_start;
  uint16_t ul_dmrs_symb_pos;
  int dmrs_config_type;                  /* 0 = type 1 */
  int n_dmrs_cdm_groups_no_data, nscid, dmrs_scrambling_id;
  uint16_t dmrs_ports;                   /* FAPI bit mask of the ports */
  int qam_mod_order, nr_of_layers;
  int transform_precoding;               /* 1 = DFT-s-OFDM (the grant's own flag, not the FAPI enum) */
  int frequency_hopping, ptrs, rv, bg;
  uint32_t tbs_bits;                     /* A */
  uint32_t tbslbrm;
  int data_scrambling_id;
  int max_ldpc_iterations;
  /* ---- the cell */
  int N, symbols_per_slot, n_rb_ul, first_carrier_offset, ncp_normal, nb_rx;
  int passive;                           /* IS_PASSIVE_RX_MODE */
  int keep;                              /* nr_ulsch_passive_keep_branch(): the receive branch the equaliser uses, -1 = all */
  int timing_keep;                       /* the branch nr_pusch_channel_estimation() prefers for the delay (nr_ulsch_passive_branch_selection), -1 = strongest */
  /* ---- front end: window of symbol m (c16 offset from iq[a], < 0 = not transformed) and the RX symbol rotation of the slot */
  const int16_t *iq[4];
  uint32_t iq_len;
  int32_t win[NR_GPU_PASS_UL_NSYM];
  const int16_t *symbol_rot;             /* c16 [symbols_per_slot] */
  int16_t *rx[4];                        /* the caller's rxdataF row of symbol 0 of the slot per antenna (c16), or NULL */
  int ctx;                               /* decode context (buffers are per context, allocated by nr_gpu_pass_ul_prewarm) */
  /* ---- outputs the caller wants */
  uint8_t *tb;                           /* receives the TB (>= (lenWithCrc(1, A) + 7) / 8 + 8 bytes); the decoder's harq b */
  int16_t *llr_out;                      /* [G]: the final LLRs of a TB that failed its CRC (the UCI search re-decodes them) */
  int llr_out_all;                       /* 1 = also on success (tests) */
  int want_est;                          /* 1 = the chest estimates go to out->est (the CFR / diagnostic consumers) */
  /* ---- test hooks: pilots / descrambling words of the caller instead of the Gold sequence */
  const int16_t *pil_override[NR_GPU_PASS_MAX_DMRS];
  const uint32_t *scr_override;
} nr_gpu_pass_ul_in_t;

typedef struct {
  nr_gpu_pass_result_t r;
  int ndmrs, dmrs_sym[NR_GPU_PASS_MAX_DMRS];
  uint32_t G, C, K, F, Z, E, tbs_bits;
  uint32_t nvar;                         /* nr_pusch_channel_estimation()'s mean noise over the DM-RS symbols (nr_rx_pusch_group_internal's nvar) */
  float dmrs_snr_db;                     /* nr_ulsch_dmrs_snr_db() of the pass (NAN as in the old path when there is no estimate) */
  double dmrs_sig2;                      /* its first argument: the mean |H|^2 of the DM-RS estimates */
  int est_delay, est_delay_max_val;      /* pvp->delay of the last DM-RS symbol, the TA refinement's input */
  int log2_maxh;
  const int16_t *est;                    /* want_est: [ndmrs * nbRx][4096] c16, row = dmrs_index * nbRx + antenna */
} nr_gpu_pass_ul_out_t;

/* 1 when ISAC_UL_GPU_PASS=1 was asked for (before the module is up): the decoder allocates its per-context buffers only then */
int nr_gpu_pass_ul_requested(void);
/* before the RX fence (per decode context): the job, pilot and estimate buffers, allocated and prefaulted */
void nr_gpu_pass_ul_prewarm(int ctx);
/* 1 = ISAC_UL_GPU_PASS=1 and the classes are cached */
int nr_gpu_pass_ul_active(void);
/* 0 = the pass ran (out filled; out->r.tb_ok is the TB CRC verdict, out->r.need_cpu the zero-syndrome case); 1 = not handled (counted), the old path decodes */
int nr_gpu_pass_ul_run(const nr_gpu_pass_ul_in_t *in, nr_gpu_pass_ul_out_t *out);
/* the old path decoded the same grant too (shadow): counts it and, on a mismatch (CRC verdict, TB bytes or LLRs differ), switches the pass off process-wide. 1 = the pass was disabled. */
int nr_gpu_pass_ul_shadow(int mismatch);
int nr_gpu_pass_ul_shadow_due(void);
void nr_gpu_pass_ul_note(int ev);
enum { NR_GPU_PASS_UL_EV_OK, NR_GPU_PASS_UL_EV_FAIL /* pass said CRC fail: the old tail decoded its LLRs */, NR_GPU_PASS_UL_EV_REFINE /* |delay| above the tolerance: the old path redid the grant */,
       NR_GPU_PASS_UL_EV_SHADOW, NR_GPU_PASS_UL_EV_SHADOW_BAD, NR_GPU_PASS_UL_EV_SHADOW_LLR_DIFF /* LLRs not bit-equal (signs agree within the policy) */,
       NR_GPU_PASS_UL_EV_SHADOW_EST_DIFF /* delay / scale / DM-RS SNR differ */, NR_GPU_PASS_UL_EV_NEED_CPU /* zero syndrome with a failing CRC: old path from scratch */,
       NR_GPU_PASS_UL_EV_EST_OK_DISABLING /* CRC-OK shadow, est_delay or log2_maxh differ (disables) */, NR_GPU_PASS_UL_EV_EST_OK_SNR_ONLY /* CRC-OK shadow, only snr_db differs */,
       NR_GPU_PASS_UL_EV_EST_OK_STATE3 /* CRC-OK shadow where the pass could not tell (need_cpu): the old path's values are used */, NR_GPU_PASS_UL_EV_SNR_BIG /* CRC-OK shadow, |snr pass - snr cpu| > 1 dB */, NR_GPU_PASS_UL_EV_SHADOW_EST_DIFF_FAIL /* est diff on a CRC-fail shadow (counted) */, NR_GPU_PASS_UL_EV_N };
/* 1 = the pass result may be used; 0 = old path from scratch (need_cpu: the pass left no LLRs; counted). A shadowed pass is always compared, never used. */
/* rate gate on CRC-OK shadows (nr_fp32_gate: >= 64 shadows, snr_big above 1 %): 1 = tripped and the pass was disabled */
int nr_gpu_pass_ul_snr_note(int big);
int nr_gpu_pass_ul_accept(const nr_gpu_pass_ul_out_t *o, int shadow);
/* atomically: fail_streak++ of the RNTI's entry; the entry is dropped when it reaches max. A concurrent CRC-OK refresh is never rolled back. */
void ul_hyp_cache_note_fail(uint16_t rnti, int max);
void nr_gpu_pass_ul_census(void);
/* per-grant wall time of the whole decode call, by what carried it: 0 old path, 1 pass (TB decoded), 2 pass (CRC failed, old tail), 3 shadowed (both paths ran). Kept for the census percentiles. */
void nr_gpu_pass_ul_grant_us(int kind, uint32_t us);
/* class warm-up (before the RX fence), called by nr_gpu_pass_warmup() after the module is up (h = the dlopen handle); returns the classes captured */
int nr_gpu_pass_ul_warm(void *h, int nbrx, int fp32);

#ifdef __cplusplus
}
#endif
#endif
