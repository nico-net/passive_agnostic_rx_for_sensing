/* ISAC_UL_NID_BG=1 (default off): per-UE background search of the UL data-scrambling id (n_ID, 0..1023) and of the layout hypothesis it
 * depends on (twin BWP ranges of the RIV, MCS table, DM-RS id). Plan: docs/superpowers/plans/2026-10-17-u4-ul-dataid-search.md (adapted).
 *
 * Stage A (cheap, LLR only): a real or CRC-trial UL grant that failed its CRC with a high DM-RS SNR, QPSK/low MCS first, hands a COPY of its LLRs
 *   (descrambled with the tried id) to one low-priority worker. Scrambling is a pure sign flip, so the worker re-signs the first code block with every
 *   candidate id and runs the CPU LDPC with CRC early stop (same decoder and all-zero guard as the DL CB0 probe).
 * Stage B (ISAC_UL_NID_BG_IQ=1): the job also carries a private copy of the slot's IQ; the worker re-runs the receive chain (CPU only, its own
 *   decode context) for further layout hypotheses of the same grant -- twin ranges of the RIV under other BWP sizes, the other MCS tables, other
 *   DM-RS ids -- and sweeps the ids on each one's LLRs.
 * Confirmation (never a bare CRC16): a hit confirms its (hypothesis, id) if the code block carries CRC24 (TB CRC24A, or CB CRC24B when C > 1), or
 *   if it repeats with the same hypothesis key in >= 2 different slots (CRC16 false alarm ~3e-4 per candidate).
 * A confirmed entry is per RNTI, is applied by the real-time decode (ids, MCS table, range under the confirmed BWP size, timing advance) and
 *   is dropped when it keeps failing. The decode path never waits for this module. */
#ifndef NR_UL_NID_BG_H
#define NR_UL_NID_BG_H
#include <stdbool.h>
#include <stdint.h>
#include "PHY/TOOLS/tools_defs.h"
#include "nr_pdcch_blind_monitor.h"

typedef struct {
  uint16_t rnti, pci, tried_id, dmrs_id, dmrs_mask;
  uint8_t mapping;
  uint16_t bwp_size, bwp_start, start_rb, num_rb;
  uint8_t qm, nl, rv, bg, mcs, mcs_table, nscid, start_sym, num_sym, tp, dci_format, trial, k2;
  bool crc_ok; /* the decode's own verdict with tried_id (may be the all-zero TB: the worker re-checks) */
  uint32_t G, tbs_bits;
  float snr_db;
  int ta, ta_nom; /* applied / nominal (before the timing search) advance */
  int64_t abs_slot;
} nr_ul_nid_rec_t;

typedef struct {
  uint16_t data_id, dmrs_id, bwp_size, bwp_start, dmrs_mask;
  uint8_t mcs_table, start_sym, num_sym, mapping; /* dmrs_mask applies to grants of this symbol allocation only */
  bool have_ta;
  int ta; /* timing advance of the decode that confirmed it */
} nr_ul_nid_conf_t;

typedef struct nr_ul_nid_job {
  int st; /* internal: 0 free, 1 filling, 2 filled, 3 working */
  nr_ul_nid_rec_t rec;
  nr_pdcch_blind_ul_result_t grant;
  int16_t *llr;
  uint32_t llr_cap;
  bool have_iq, have_iq2; /* IQ of the grant's target slot / of the slot after it (k2 + 1 hypotheses) */
  c16_t *iq[4], *iq2[4];
  uint32_t iq_cap, origin, length, origin2, length2;
  uint8_t nant, slot, spf; /* spf: slots per frame */
  uint32_t frame;
  double fo_hz;
  void *ue;
} nr_ul_nid_job_t;

/* what a hypothesis decode hands back (filled by the decoder's capture hook) */
typedef struct {
  int16_t *llr; /* in: worker buffer */
  uint32_t llr_cap;
  nr_ul_nid_rec_t rec; /* out */
  bool got;
  int ta_applied;
} nr_ul_nid_cap_t;
/* Stage B runner (provided by nr_pusch_passive_decode.c): decode `hg` (slot_delta 0 or 1 slots after the grant's) on the job's private IQ in the background context. true = rec/llr captured. */
typedef bool (*nr_ul_nid_hyp_fn)(const nr_ul_nid_job_t *j, const nr_pdcch_blind_ul_result_t *hg, int32_t ta, int slot_delta, nr_ul_nid_cap_t *cap);

bool nr_ul_nid_bg_enabled(void); /* ISAC_UL_NID_BG=1 */
bool nr_ul_nid_bg_iq_enabled(void); /* ISAC_UL_NID_BG_IQ=1 (stage B) */
/* Gate + per-RNTI rate limit + free slot. Returns the slot index (fill llr/grant/iq via nr_ul_nid_bg_slot, then publish) or -1. */
int nr_ul_nid_bg_admit(const nr_ul_nid_rec_t *r);
nr_ul_nid_job_t *nr_ul_nid_bg_slot(int idx);
bool nr_ul_nid_bg_slot_reserve(int idx, uint32_t llr_n, uint32_t iq_n); /* allocate buffers (iq_n = 0: stage A only) */
void nr_ul_nid_bg_publish(int idx);
void nr_ul_nid_bg_abort(int idx); /* give a claimed slot back unpublished */
/* starts the worker once (fn may be NULL: stage A only) */
/* prep (may be NULL) runs once per job before stage B: the background DM-RS-id / timing discovery of the job's RNTI when none is cached */
typedef void (*nr_ul_nid_prep_fn)(const nr_ul_nid_job_t *j);
void nr_ul_nid_bg_start(nr_ul_nid_hyp_fn fn, nr_ul_nid_prep_fn prep);
/* the real-time decode's lookup: true and *c filled when the RNTI has a confirmed entry */
bool nr_ul_nid_bg_lookup(uint16_t rnti, nr_ul_nid_conf_t *c);
/* the real-time decode's verdict under a confirmed entry (drops the entry when it never decodes) */
void nr_ul_nid_bg_feedback(uint16_t rnti, bool ok, float snr_db);
typedef struct {
  uint64_t submitted, dropped_gate, dropped_busy, jobs_done, hyps, sweeps, cands, hits_weak, hits_strong, confirmed, dropped_entries, gate_snr, gate_qm, gate_rate, gate_trial;
  uint64_t cpu_us, rt_ok, rt_fail, iq2_ok, iq2_miss;
} nr_ul_nid_stats_t;
void nr_ul_nid_bg_stats(nr_ul_nid_stats_t *s);
/* the capture thread asks before it copies the slot after a booked slot: could a grant like this one become a stage-B job (MCS / size gate, RNTI not confirmed)? */
bool nr_ul_nid_bg_wants_next(const nr_pdcch_blind_ul_result_t *g);
void nr_ul_nid_bg_note_iq2(bool ok); /* the k2 + 1 snapshot was / was not obtained */
void nr_ul_nid_bg_stop(void);

#include "nr_ul_nid_rule.h"
#endif
