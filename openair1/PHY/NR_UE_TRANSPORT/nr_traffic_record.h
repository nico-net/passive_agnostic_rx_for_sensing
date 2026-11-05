/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic-mode DCI record: one struct per accepted DCI, copied under the sense-ring mutex and formatted on the writer thread. */
#ifndef NR_TRAFFIC_RECORD_H
#define NR_TRAFFIC_RECORD_H
#include "nr_passive_obs.h"
#include "nr_sense_records.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NR_LAYOUT_FIXED = 0, NR_LAYOUT_SOLVED, NR_LAYOUT_CANDIDATE, NR_LAYOUT_UNSOLVED } nr_layout_state_t;
/* What proved the layout behind a record's fields: NONE (null), BITS (CRC-free proof from DCI bits and spec constraints, E1), PHYS (DM-RS physics, E3),
 * CRC (the TB CRC solved the layout). Only a fields_evidence of BITS lets the HARQ-ACK fields (DAI, TPC, PRI, K1) print under a CANDIDATE layout state. */
enum { NR_LAYOUT_EV_NONE = 0, NR_LAYOUT_EV_BITS, NR_LAYOUT_EV_PHYS, NR_LAYOUT_EV_CRC };
/* record dci_format codes: DL 1_1 = 0 and 1_0 = 1 (nr_blind_dci_format_t), UL 0_1 = 2 and 0_0 = 3 (nr_blind_ul_dci_format_t + 2) */
enum { NR_DCI_REC_FMT_11 = 0, NR_DCI_REC_FMT_10 = 1, NR_DCI_REC_FMT_01 = 2, NR_DCI_REC_FMT_00 = 3 };
typedef enum { NR_DCI_DECODE_NONE = 0, NR_DCI_DECODE_QUEUED = 1 } nr_dci_decode_t;
typedef struct {
  nr_passive_obs_t o;            /* existing grant keys, filled as the post-decode callers do (crc = -1 here) */
  nr_sense_grant_meta_t m;       /* existing meta keys */
  uint8_t dci_format;            /* nr_blind_dci_format_t / UL 0_0, 0_1 */
  int8_t ss_type, al; int16_t cce, coreset_id; uint16_t dci_len;
  uint8_t dci_bits[18];          /* raw payload, MSB-first, dci_len bits (<= 140) */
  bool rnti_confirmed;           /* nr_pdcch_blind_monitor_rnti_confirmed() at accept */
  nr_layout_state_t layout_state; uint32_t layout_checks_ok, layout_checks_fail;
  uint8_t layout_evidence;       /* NR_LAYOUT_EV_*: what proved the layout (0 = unknown, the memset default) */
  uint8_t fields_evidence;       /* NR_LAYOUT_EV_*: what proved the HARQ-ACK fields (k1, dai, tpc, pri) printed here: BITS = the E1 split proof, CRC = the CRC-solved layout */
  uint8_t grant_unproven;        /* E3: 1 = the layout is PHYS and neither hq_verified nor CRC-confirmed: MCS, NDI, RV, HARQ pid, retx and the HARQ-ACK fields (unless E1 proved them) print null */
  uint8_t table_evidence;        /* E3: what fixed the MCS table behind mcs_table / qm / tbs: 0 unknown (null), 1 = a CRC pass, 2 = Qm consensus with a CRC pass of that table. A PHYS layout prints S, L, table, qm and tbs only with a table evidence of 2 (S and L: never, they are CRC-learnt) */
  uint32_t cfg_ref_ver;          /* with m.identity_gen + m.dedicated_epoch = cfg_ref */
  int16_t bwp_start, bwp_size, start_rb_bwp;  /* -1 unknown */
  int8_t vrb_prb, tda_index, k0, k1, dai, tpc, pri, srs_req, csi_req, ulsch_ind, cbgti, retx; /* -1 unknown */
  int8_t si_ind, short_msg_ind; int16_t short_msg; int8_t tb_scaling;   /* P/SI/RA only, -1 otherwise */
  int64_t pdsch_abs_slot, harq_fb_abs_slot;  /* -1 unknown */
  nr_dci_decode_t decode;
} nr_sense_dci_rec_t;
int nr_sense_dci_to_json(const nr_sense_dci_rec_t *r, char *buf, size_t n);   /* writer-thread formatter, pure */
void nr_sense_emit_dci(nr_sense_dci_rec_t *r);   /* stamps epoch/identity/t_mono like nr_sense_emit_grant_meta, then nr_sense_push_dci */
/* outcome of a probe/check decode: {"type":"decode","abs_slot" (= PDSCH/PUSCH slot),"dci_abs_slot" (= the DCI record's abs_slot, the join key; null if unknown),"rnti","dir","probe","crc","snr_db","learned":...} */
void nr_sense_emit_decode(uint64_t abs_slot, int64_t dci_abs_slot, uint16_t rnti, int dir, bool probe, int crc, float snr_db, const char *learned);
/* Traffic-mode counters of this cell (ISAC_METRICS). accepted[] is indexed by NR_DCI_REC_FMT_*, state[] by nr_layout_state_t. */
typedef struct {
  uint64_t accepted[4], state[4], dup, traffic_skip, check_decodes, check_fail, probe_decodes, reopens, reopens_ul;
  uint64_t check_dl, check_ul, probe_dl, probe_ul, probe_skip, ul_decode_skip;   /* E4: check/probe_decodes are the DL+UL sums */
  uint64_t e1_changes, e1_skip_classes, e1_unfed;
  uint64_t e1_deferred_scoped;   /* the same, of a solved RNTI's own-class (scoped) looks */
  uint64_t e1_deferred;   /* E1 looks held back by the per-cell prove budget (20 ms per s), once per RNTI episode */
  uint64_t e1_cls[5];   /* histogram of alive layout classes per prove attempt of an UNSOLVED RNTI: 1, 2-4, 5-16, 17-64, >64 */   /* E1 prover: proven-state changes; rounds skipped (more than NR_DCI11_CLASS_MAX layout classes, or an incomplete resolver set); C-RNTI 1_1 accepts of tasks that keep no payload (never fed) */
  uint64_t e3_scored, e3_hyps, e3_ns, e3_phys_solved, e3_fam_accept, e3_fam_tried, e3_reopen_score, e3_reopen_e1, e3_deferred, e3_skip;   /* E3: grants scored on the scan consumer, hypotheses scored, their CPU (ns), RNTIs solved by physics, by a family (accepted / tried), reopened by the score / by E1, grants held back by the per-cell budget, skipped (slot not ready, TDD mixed slot, no grid) */
  uint64_t e3_trunc;   /* sign-test grants whose hypothesis list was truncated (E3_HYPS / E3_READINGS): counted, never decided */
  uint64_t e3_qm_calls, e3_qm_deferred, e3_qm_ns;   /* Qm classifier looks (each ~ms), held back by the Qm budget, their CPU (ns) */
  uint64_t emit_us_p50, emit_us_p99;
} nr_traffic_stats_t;
void nr_traffic_count_dci(int fmt, nr_layout_state_t ls, uint64_t emit_ns);   /* one emitted DCI record + its fill+emit time */
void nr_traffic_count(int what);   /* NR_TRAFFIC_CNT_* */
void nr_traffic_add(int what, uint64_t n);   /* the same, by n */
enum { NR_TRAFFIC_CNT_DUP, NR_TRAFFIC_CNT_SKIP, NR_TRAFFIC_CNT_CHECK, NR_TRAFFIC_CNT_CHECK_FAIL, NR_TRAFFIC_CNT_PROBE, NR_TRAFFIC_CNT_REOPEN, NR_TRAFFIC_CNT_REOPEN_UL, NR_TRAFFIC_CNT_E1_CHANGE, NR_TRAFFIC_CNT_E1_SKIP, NR_TRAFFIC_CNT_E1_UNFED,
       NR_TRAFFIC_CNT_E1_CLS_1, NR_TRAFFIC_CNT_E1_CLS_2_4, NR_TRAFFIC_CNT_E1_CLS_5_16, NR_TRAFFIC_CNT_E1_CLS_17_64, NR_TRAFFIC_CNT_E1_CLS_GT64, NR_TRAFFIC_CNT_E1_DEFERRED, NR_TRAFFIC_CNT_E1_DEFERRED_SCOPED,
       NR_TRAFFIC_CNT_CHECK_UL, NR_TRAFFIC_CNT_PROBE_UL, NR_TRAFFIC_CNT_PROBE_SKIP, NR_TRAFFIC_CNT_UL_SKIP,
       NR_TRAFFIC_CNT_E3_SCORED, NR_TRAFFIC_CNT_E3_HYPS, NR_TRAFFIC_CNT_E3_NS, NR_TRAFFIC_CNT_E3_PHYS_SOLVED, NR_TRAFFIC_CNT_E3_FAM_ACCEPT, NR_TRAFFIC_CNT_E3_FAM_TRIED,
       NR_TRAFFIC_CNT_E3_REOPEN_SCORE, NR_TRAFFIC_CNT_E3_REOPEN_E1, NR_TRAFFIC_CNT_E3_DEFERRED, NR_TRAFFIC_CNT_E3_SKIP, NR_TRAFFIC_CNT_E3_QM_CALLS,
       NR_TRAFFIC_CNT_E3_QM_DEFERRED, NR_TRAFFIC_CNT_E3_QM_NS, NR_TRAFFIC_CNT_E3_TRUNC, NR_TRAFFIC_CNT_N };   /* CHECK/PROBE = DL */
void nr_traffic_stats_get(nr_traffic_stats_t *s);
/* Traffic mode: one decode outcome (probe or full decode, crc < 0 = no verdict) -> the "decode" record, the check/probe counters and the
 * reopen decision. Returns true after K consecutive counted failures. dir 0: the caller then calls nr_pdsch_solver_reopen (reopens). dir 1: the
 * caller forgets that RNTI's learned UL MCS table (reopens_ul); UL never touches DL state. */
bool nr_traffic_decode_outcome(uint64_t abs_slot, int64_t dci_abs_slot, uint16_t rnti, int dir, bool probe, int rv, int crc, float snr_db);
/* Sensing: the consumer DM-RS tap of a solver job runs after a CRC pass, and also after a CRC failure when the layout is SOLVED (the DM-RS channel estimate needs no CRC once the
 * layout is known); unsolved/probe grants stay CRC-gated. */
static inline bool nr_dmrs_tap_wanted(bool job_tap, bool layout_probe, bool crc_ok, bool crc_fail, nr_layout_state_t ls)
{
  return job_tap && !layout_probe && (crc_ok || (crc_fail && ls == NR_LAYOUT_SOLVED));
}
/* Per RNTI+HARQ pid last NDI (dir 0 DL, 1 UL): -1 first sighting (or ndi < 0), 0 new data (NDI toggled), 1 retransmission (same NDI). Updates the table. */
int nr_traffic_retx(int dir, uint16_t rnti, int pid, int ndi);
/* E7b: PDSCH slot offset k0 per (DCI length, TDA index) of this cell, proven by a decode whose FULL transport block passed CRC24 (TBS > 3824 bits; a CRC16 TB,
 * a first-code-block probe or a failed decode proves nothing). Two different proven k0 for one key mark it unknown for good (until the reset), counted. */
void nr_traffic_k0_note(int dci_len, int tda, int k0, int tbs_bits, bool tb_crc_ok);
int nr_traffic_k0_get(int dci_len, int tda);   /* -1 unknown, absent or conflicting */
uint64_t nr_traffic_k0_conflicts(void);
void nr_traffic_k0_reset(void);   /* identity change: with nr_traffic_retx_reset */
void nr_traffic_retx_reset(void);   /* HARD_RESET epochs: every RNTI is new again */
/* 1 queued, 0 dropped (ring full / no sink: counted in dci_lost), -1 API not open. Never blocks beyond the per-cell mu. */
int nr_sense_push_dci(const nr_sense_dci_rec_t *r);
#ifdef __cplusplus
}
#endif
#endif
