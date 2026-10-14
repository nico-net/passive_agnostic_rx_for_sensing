/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDCCH_UL_LAYOUT_LOCK_H
#define NR_PDCCH_UL_LAYOUT_LOCK_H
/* UL-LAYOUT (ISAC_UL_LAYOUT_LOCK=1, default off): blind, CRC-free lock of the UL BWP and of the DCI 0_1 field
 * layout of one cell.
 *
 *  stage 1 (bits, per DCI length): the raw payload bank of CRC-verified C-RNTI DCI 0_1 is fitted.
 *    - MCS/NDI/RV/HARQ block offset: the HARQ pid of consecutive grants of one RNTI advances by 1..3 and RV is
 *      mostly 0 only at the true offset;
 *    - prefix split (BWP indicator | RIV | TDRA | FH): constant leading bits, high-entropy RIV LSB, RIV validity
 *      for a candidate N_BWP; every split that survives is kept as a hypothesis;
 *    - tail split (DAI | TPC | SRI | antenna ports | SRS | CSI | beta | DM-RS init | UL-SCH): every spec-legal
 *      split of the remaining width that reads >= 97 % of the bank as plausible, ranked by a configuration prior;
 *    - k2 per TDRA index: smallest k2 that moves the DCI slots onto slots that never carry a PDCCH.
 *  stage 2 (physics, cell-wide): for every bank-fitted length, each grant is booked as a PROBE at its PUSCH slot;
 *    the PUSCH worker correlates the CP-OFDM DM-RS (PCI scrambling, port 0) on the PRBs that every BWP hypothesis
 *    (N_BWP, start) predicts. Strong detections vote; the BWP locks when one hypothesis leads with a clear margin.
 *    The same probes measure the energy of the second comb of the DM-RS symbols against the data symbols (data
 *    there = 1 CDM group without data; empty = 2), which settles the equal-length tail readings that differ in
 *    their CDM-group count, and the DM-RS additional-position set.
 * Nothing needs a PUSCH TB CRC. The DL BWP is never assumed for the UL. Grants of a locked length are read with
 * the locked layout and tagged NR_UL_LAYOUT_TAG; runner-up tails are logged (ISAC_UL_RETRY covers the CDM count). */
#include <stdbool.h>
#include <stdint.h>
#include "nr_pdcch_blind_monitor.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_UL_LAYOUT_TAG   (UINT64_C(1) << 62) /* hyp_generation of a grant read with the locked layout */
#define NR_UL_LAYOUT_PROBE (UINT64_C(1) << 61) /* ... of a DM-RS probe (never decoded) */
#define NR_UL_LAYOUT_TRIAL (UINT64_C(1) << 60) /* ... of a CRC-first trial: a real PUSCH decode under one layout hypothesis;
                                                  bits 40..51 = hypothesis (bwp hyp | prefix<<6 | tail<<9), low 40 = gen */
#define NR_UL_LAYOUT_GEN_MASK ((UINT64_C(1) << 40) - 1)
#define NR_UL_LAYOUT_MAX_TRIALS 4
#define NR_UL_LAYOUT_MAX_HYP 192 /* 64 until BLINDFIX F6: the hypothesis index is 8 bits in a trial tag */
#define NR_UL_LAYOUT_MAX_LEN 4
#define NR_UL_LAYOUT_MAX_PRE 8
#define NR_UL_LAYOUT_MAX_TAIL 4
#define NR_UL_LAYOUT_BANK 1024

typedef struct {
  uint16_t carrier_rb;  /* N_RB of the carrier: every BWP hypothesis fits inside it */
  uint16_t dl_bwp_size; /* only widens the N candidate set; never ranked first */
  uint16_t pci;
  uint8_t mu;
  uint8_t typeA_pos; /* MIB enum: 0 = pos2, 1 = pos3 */
  uint8_t slots_per_frame;
  uint16_t sib1_ul_start, sib1_ul_size; /* SIB1 initialUplinkBWP (0 size = unknown); a hypothesis, ISAC_UL_LOCK_SIB1_FIRST */
  uint16_t dl_bwp_start; /* solved DL BWP start (valid with dl_bwp_size > 0): ISAC_UL_LOCK_PRIOR_SEARCH prior, never fixed truth */
} nr_ul_layout_cell_t;

typedef struct {
  uint8_t bwp_ind, riv, tda, fh; /* widths after the 1-bit identifier (no carrier / UL-SUL indicator) */
  uint16_t n_max;                /* largest candidate N_BWP with this RIV width */
} nr_ul_layout_prefix_t;

typedef struct {
  int8_t dai1, dai2, sri, ant, csi, beta, init;
  uint8_t tp;   /* 1 = transform precoding (DFT-s-OFDM) */
  uint8_t cdm;  /* majority CDM-groups-without-data this tail decodes to */
  int16_t cost; /* configuration prior: 0 = the plainest legal configuration */
  float plaus;  /* fraction of the bank read as a plausible grant */
} nr_ul_layout_tail_t;

typedef struct {
  uint16_t rnti;
  long slot; /* source absolute slot of the DCI */
  nr_dci_bits_t p;
} nr_ul_layout_dci_t;

/* One probe's measurements, per BWP hypothesis (same order as nr_ul_layout_probe_plan()). */
typedef struct {
  int n;
  float snr_db[NR_UL_LAYOUT_MAX_HYP]; /* DM-RS MRC SNR (dB), -99 when the hypothesis has no PRBs for this grant */
  int8_t set[NR_UL_LAYOUT_MAX_HYP];   /* best DM-RS symbol set: 0 = {l0}, 1 = {l0,11}, 2 = {l0,7,11} */
  float ctl_db;                       /* argmax hypothesis, DM-RS of the wrong slot number (false-detection control) */
  float comb1_db;                     /* argmax: comb-1 RE energy of the DM-RS symbols / data-symbol RE energy (dB);
                                         NAN when not measured (argmax SNR < 8 dB) */
  /* ISAC_UL_LOCK_SPRT=1 (ULLOCKSPEED): zn_valid = the worker filled zn[]. zn[h] = (SNR_est(true DM-RS) - SNR_est(wrong-slot
   * control DM-RS)) / sigma0 of the same PRB range, linear and unclamped: symmetric about 0 when the range holds no DM-RS. */
  bool zn_valid;
  float zn[NR_UL_LAYOUT_MAX_HYP];
  /* ISAC_UL_LOCK_EDGE=1 (ULTWINS): ze[h][0/1] = edge statistic of the left / right edge of hypothesis h's range: DM-RS excess of the
   * PRB just inside minus just outside, true minus control sequence, / its null sigma (NAN = outside PRB beyond the carrier).
   * ISAC_UL_LOCK_COHERENT=1: zc[h] = coherent DM-RS sum across the range, true minus control, / null sigma. */
  bool ze_valid, zc_valid;
  float ze[NR_UL_LAYOUT_MAX_HYP][2];
  float zc[NR_UL_LAYOUT_MAX_HYP];
} nr_ul_layout_probe_res_t;

bool nr_ul_layout_lock_enabled(void); /* ISAC_UL_LAYOUT_LOCK=1 */
/* ISAC_UL_LOCK_SPRT=1 (default off, needs the lock): the BWP lock is a sequential test on the accumulated DM-RS evidence of
 * EVERY probe (any grant width) instead of 6-dB votes. See docs/superpowers/reports/csd/ULLOCKSPEED-20261012.md. */
bool nr_ul_layout_sprt_enabled(void); /* also true when EDGE or COHERENT is on (they use the same sequential machinery) */
/* ULTWINS (docs/superpowers/reports/csd/ULTWINS-20261012.md), all default off, need the lock: EDGE = allocation-edge test (the evidence is the
 * DM-RS step at the predicted edges of each grant, outside PRBs covered by another decoded grant of the slot excluded); COHERENT =
 * coherent DM-RS combining across a range; NO_TRIAL_UNLOCKED = no CRC-first PUSCH trials while the BWP is unlocked. With EDGE or COHERENT
 * set the incoherent zn evidence is left out unless ISAC_UL_LOCK_SPRT=1 is set as well. */
bool nr_ul_layout_edge_enabled(void);
bool nr_ul_layout_coherent_enabled(void);
/* BLINDFIX F6 (ISAC_UL_LOCK_BWP_ALL=1, default off): every legal (N_BWP, start) of the carrier whose RIV needs `w` bits and for
 * which N(N+1)/2 > maxriv (the largest RIV observed), N descending, start ascending. count = size; at(idx) = the idx-th. */
uint32_t nr_ul_layout_enum_count(int carrier_rb, int w, uint32_t maxriv);
bool nr_ul_layout_enum_at(int carrier_rb, int w, uint32_t maxriv, uint32_t idx, uint16_t *n, uint16_t *start);
/* ISAC_UL_LOCK_LLR=1 (default off, needs the lock): log-odds lock rule. votes/prior(0|1)/nrnti per BWP hypothesis; true and *lead = the
 * leader when it beats every other hypothesis by the margin (see nr_pdcch_ul_layout_lock.c). The prior (solved DL BWP, SIB1 UL BWP) shifts
 * the margin by one vote and lowers the leader's own minimum from 6 to 4 votes; it can never lock without votes. Also makes the probe booking
 * skip grants that do not discriminate the current contenders. */
bool nr_ul_layout_llr_decide(const uint32_t *votes, const uint8_t *prior, const uint8_t *nrnti, int n, int *lead);
/* lock enabled, cell known and no BWP locked yet: the layout search phase (used for ISAC_PUSCH_SEARCH_PRIO) */
bool nr_ul_layout_searching(void);
void nr_ul_layout_lock_reset(void);
/* Any accepted PDCCH (DL or UL format) in this slot: the slot has DL symbols, so no full-slot PUSCH lands there. */
void nr_ul_layout_note_pdcch_slot(long abs_slot);
/* Producer: one CRC-verified C-RNTI DCI 0_1. Returns
 *   1: the length is locked; *out = the grant read with the locked layout (hyp_generation = TAG|gen)
 *   2: probe; *out = a DM-RS probe grant at its PUSCH slot (hyp_generation = TAG|PROBE|gen), book it
 *   0: banked only. */
int nr_ul_layout_on_dci(uint16_t rnti, uint16_t len, long abs_slot, nr_dci_bits_t payload,
                        const nr_ul_layout_cell_t *cell, nr_pdcch_blind_ul_result_t *out);
static inline bool nr_ul_layout_is_probe(const nr_pdcch_blind_ul_result_t *g)
{
  return g && (g->hyp_generation & NR_UL_LAYOUT_TAG) && (g->hyp_generation & NR_UL_LAYOUT_PROBE);
}
static inline bool nr_ul_layout_is_tagged(const nr_pdcch_blind_ul_result_t *g)
{
  return g && (g->hyp_generation & NR_UL_LAYOUT_TAG);
}
/* Worker: absolute PRB range per BWP hypothesis for this probe (nprb 0 = not testable). Returns the hypothesis
 * count, 0 when the probe is stale (reset since it was booked). */
int nr_ul_layout_probe_plan(const nr_pdcch_blind_ul_result_t *g, uint16_t *prb0, uint16_t *nprb, int cap,
                            uint16_t *nid, uint8_t *l0);
void nr_ul_layout_probe_feed(const nr_pdcch_blind_ul_result_t *g, long target_slot, const nr_ul_layout_probe_res_t *r);
void nr_ul_layout_stats_dump(void);
/* ISAC_UL_LOCK_SIB1_FIRST=1: the SIB1 UL BWP is hypothesis 0. ISAC_UL_LOCK_CRC_FIRST=1: real PUSCH decode jobs under the
 * top-K (ISAC_UL_LOCK_CRC_K, default 3) layout hypotheses before the lock (and, once locked, after a run of CRC failures);
 * a PUSCH TB CRC pass locks/re-locks that hypothesis at once. Both default off. */
bool nr_ul_layout_sib1_first_enabled(void);
bool nr_ul_layout_crc_first_enabled(void);
/* Call after nr_ul_layout_on_dci for the same DCI. Fills out[0..n) with CRC trial grants (hyp_generation TAG|TRIAL|..) to
 * book as ordinary PUSCH jobs; returns n (0 = none; at most one DCI per slot, small MCS only). */
int nr_ul_layout_crc_trials(uint16_t rnti, uint16_t len, long abs_slot, nr_dci_bits_t payload,
                            const nr_ul_layout_cell_t *cell, nr_pdcch_blind_ul_result_t *out, int cap);
/* PUSCH worker: TB CRC verdict of a tagged (locked or trial) grant. */
void nr_ul_layout_crc_result(const nr_pdcch_blind_ul_result_t *g, bool ok);
/* ISAC_UL_LOCK_CRC_GUARD=1 (default off, needs CRC_FIRST): a CRC16 pass (TBS <= 3824 bits) is a false pass with p = 2^-16, so it only
 * counts as evidence (see crc_guard_accept_locked); CRC24A / all-CB-CRC passes lock at once. crc_result() = verdict with unknown TB
 * (treated as CRC16 under the guard). */
bool nr_ul_layout_crc_guard_enabled(void);
void nr_ul_layout_crc_verdict(const nr_pdcch_blind_ul_result_t *g, bool ok, uint32_t tbs_bits, int nseg, int seg_ok, long slot);
static inline bool nr_ul_layout_is_trial(const nr_pdcch_blind_ul_result_t *g)
{
  return g && (g->hyp_generation & NR_UL_LAYOUT_TAG) && (g->hyp_generation & NR_UL_LAYOUT_TRIAL);
}
/* ISAC_UL_LOCK_PRIOR_SEARCH=1 (default off, needs the lock; ULPRIOR report): provisional adoption of the top (SIB1 start, DL-consistent
 * start, neighbours) hypothesis at the fit, demoted by a run of CRC-EXPECTED failures, confirmed by a CRC pass or by DM-RS votes.
 * prior_note: verdict of a grant read with the locked layout + decoder SNR and TB bits per coded RE. */
bool nr_ul_layout_prior_enabled(void);
bool nr_ul_layout_prior_expected(float snr_db, float se); /* pure: snr_db >= Shannon(se) + 2 dB */
/* returns true while the adoption is provisional */
bool nr_ul_layout_prior_state(uint16_t *start, uint16_t *size, uint32_t *crc_expected_n, uint32_t *crc_ok, uint32_t *demotions);
void nr_ul_layout_prior_note(const nr_pdcch_blind_ul_result_t *g, bool ok, uint32_t tbs_bits, int nseg, int seg_ok, long slot,
                             float snr_db, float se);
/* One fit pass over every length's bank: the eval thread runs it every 100 ms; unit tests call it directly. */
void nr_ul_layout_eval_once(void);

/* ---- pure helpers (unit-tested) ---- */
int nr_ul_layout_riv_bits(int n_bwp);
bool nr_ul_layout_riv(uint32_t riv, int n_bwp, uint16_t *start, uint16_t *len);
/* MSB offset of the MCS field by the HARQ/RV statistics. Returns the offset or -1; score = best, *second = runner-up. */
int nr_ul_layout_fit_mcs(const nr_ul_layout_dci_t *b, int n, uint16_t len, float *score, float *second);
/* Prefix splits consistent with mcs_off. cand_n: candidate N_BWP values (fitting the carrier). Returns count. */
int nr_ul_layout_fit_prefix(const nr_ul_layout_dci_t *b, int n, uint16_t len, int mcs_off, const uint16_t *cand_n,
                            int n_cand, nr_ul_layout_prefix_t *out, int cap);
/* Spec-legal tails for this length, best first. Returns count. */
int nr_ul_layout_fit_tail(const nr_ul_layout_dci_t *b, int n, uint16_t len, const nr_ul_layout_prefix_t *pre,
                          nr_ul_layout_tail_t *out, int cap);
/* Smallest k2 in 1..16 that moves >= 95 % of dci_hist onto slots with (almost) no PDCCH; 0 = undecided. */
int nr_ul_layout_infer_k2(const uint32_t *pdcch_occ, const uint32_t *dci_hist, int spf);
/* The opts that read `len` with prefix/tail/BWP and the inferred TDRA rows. */
void nr_ul_layout_build_opts(const nr_ul_layout_prefix_t *pre, const nr_ul_layout_tail_t *t, uint16_t bwp_start,
                             uint16_t bwp_size, const uint8_t *k2_per_row, int add_pos, uint16_t pci, uint8_t mu,
                             uint8_t typeA_pos, nr_pdcch_blind_ul_opts_t *o);
#ifdef __cplusplus
}
#endif
#endif
