/*
 * SIB1-derived PDCCH prior for the passive blind monitor.
 *
 * WHY THIS EXISTS (measured 2026-09-20, Swisscom n78 macro):
 * The blind CORESET search walks 133 extents x up to 271 (bundle, interleaver, shift) mappings --
 * ~36,000 hypotheses -- and after four captures totalling ~50 minutes it had verified nothing.
 * Meanwhile SIB1 decodes on every single run, and `ServingCellConfigCommonSIB` carries
 * `pdcch-ConfigCommon.commonControlResourceSet`: a FULL ControlResourceSet IE, i.e. exactly the
 * frequency-domain bitmap, duration, REG bundle size, interleaver size, shift index and DM-RS
 * scrambling id that the search is brute-forcing. The passive path had that struct in hand
 * (`nr_rrc_mac_config_req_sib1()` -> `passive_acquisition_sib1()`) and returned early, after which
 * the ASN.1 struct is freed by its caller -- so the answer was decoded, looked at, and thrown away
 * once per run.
 *
 * SCOPE, deliberately: this is the COMMON CORESET, not the dedicated one. The dedicated CORESET
 * lives in RRCSetup/RRCReconfiguration and is never broadcast. So this is a PRIORITISED HYPOTHESIS,
 * not an answer -- exactly the convention the UL side already uses ("UL discovery seeded from SIB1
 * ... HYPOTHESIS for the dedicated config, not a fact"). Consumers must put it FIRST and still fall
 * back to the blind walk, so a cell that does not reuse its common CORESET cannot regress.
 *
 * Plain C by design: no ASN.1 types, so PHY-side code can include this without dragging in the
 * generated RRC headers.
 */
#ifndef NR_PDCCH_SIB1_PRIOR_H
#define NR_PDCCH_SIB1_PRIOR_H

#include <stdbool.h>
#include <stdint.h>

/** Aggregation levels are indexed 0..4 = AL 1, 2, 4, 8, 16 throughout. */
#define NR_SIB1_PRIOR_NUM_AL 5

typedef struct {
  bool valid; /**< false until MAC has seen a SIB1; every field below is then meaningless. */

  /* ---- initial DL BWP (genericParameters.locationAndBandwidth) -------------------------------
   * The blind DCI length sweep currently guesses n_rb_riv from the SIB1 CARRIER width, which is
   * only equal to the BWP when the operator has not narrowed it. DCI 1_0's FDRA is
   * ceil(log2(N(N+1)/2)) over the ACTIVE BWP, so a wrong N shifts the whole payload. */
  uint16_t dl_bwp_start;
  uint16_t dl_bwp_size;
  bool     dl_bwp_valid;

  /* ---- pdcch-ConfigCommon.commonControlResourceSet -------------------------------------------
   * Field semantics match nr_ue_dci_configuration.c's own conversion (the canonical one), so a
   * consumer can use these directly as fapi_nr_coreset_t values with no further decoding. */
  bool     coreset_valid;
  uint8_t  coreset_id;
  uint8_t  frequency_domain_resource[6]; /**< the 45-bit bitmap, raw bytes, as FAPI wants it. */
  uint8_t  duration;                     /**< 1..3 symbols. */
  bool     interleaved;                  /**< false = non-interleaved (bundle/interleaver/shift 0) */
  uint8_t  reg_bundle_size;              /**< already decoded to 2/3/6, not the ASN.1 enum. */
  uint8_t  interleaver_size;             /**< already decoded to 2/3/6. */
  uint16_t shift_index;                  /**< defaults to physCellId when absent, per spec. */
  uint16_t pdcch_dmrs_scrambling_id;     /**< defaults to physCellId when absent, per spec. */

  /* ---- pdcch-ConfigCommon.commonSearchSpaceList ----------------------------------------------
   * `nrofCandidates` per AL is the cell's OWN answer to the question the adaptive AL ladder has
   * been inferring. A zero means the cell does not monitor that level at all, which is strictly
   * more information than the ladder can derive from accept counts. */
  bool     ss_valid;
  uint8_t  al_candidates[NR_SIB1_PRIOR_NUM_AL];
  uint16_t ss_period_slots;
  uint16_t ss_offset_slots;
  uint8_t  ss_duration;

  /* ---- search space ids, for the RA/Msg4 path -------------------------------------------------
   * RA-RNTI is computable from the PRACH occasion, so a DCI recovered in ra-SearchSpace is
   * SELF-VERIFYING in a way a C-RNTI accept is not. That is the intended escape from the
   * bootstrap circularity, where the anchor is currently always a chance CRC hit. */
  bool     ra_ss_valid;
  uint8_t  ra_ss_id;
  /* The RA search space's OWN monitoring occasions. SIB1 and RA are different search spaces (here
   * sib1_ss=0, ra_ss=1) with independent monitoringSlotPeriodicityAndOffset, and the monitor was
   * configured from SS#0 alone -- gate `slot % 40 in [11,13)`. If SS#1's occasions fall outside
   * those 2 slots in 40, RAR and Msg4 are never even looked at, which reads as an idle cell. */
  uint16_t ra_ss_period;
  uint16_t ra_ss_offset;
  uint8_t  ra_ss_duration;
  /* monitoringSymbolsWithinSlot of the RA search space, bit 13 = symbol 0 (ASN.1 order). The scan
   * runs ONE symbol per slot (SS#0's); a Msg4 at any other monitored symbol is invisible to it, so
   * this is logged to settle "no Msg4 on the air" vs "Msg4 at a symbol we never look at". */
  uint16_t ra_ss_symbol_mask;
  bool     sib1_ss_valid;
  uint8_t  sib1_ss_id;
  bool     paging_ss_valid;
  uint8_t  paging_ss_id;

  /* ---- uplinkConfigCommon.initialUplinkBWP.rach-ConfigCommon --------------------------------
   * Kept for ONE purpose: making an RA-RNTI accept self-verifying. RA-RNTI is
   *     1 + s_id + 14*t_id + 1120*f_id + 8960*ul_carrier_id      (nr_mac_common.c:5118)
   * so a candidate value DECOMPOSES, and the cell's own PRACH config says which decompositions
   * are legal. The blind monitor currently accepts any crc <= NR_PDCCH_BLIND_RA_RNTI_MAX (17920),
   * which passes 17920/65536 = 27 % of random CRCs -- that is why RA accepts have been noise. */
  bool     rach_valid;
  uint8_t  prach_config_index;
  uint8_t  msg1_fdm;            /**< DECODED occasion count 1/2/4/8, not the ASN.1 enum 0..3. */
  uint16_t msg1_frequency_start;
  bool     sul_present;         /**< false => ul_carrier_id must be 0. */
} nr_pdcch_sib1_prior_t;

/** Publish the prior. Called once per SIB1 from the MAC config path; last write wins.
 *  Safe to call with p == NULL (ignored). */
void nr_pdcch_sib1_prior_set(const nr_pdcch_sib1_prior_t *p);

/** Read the prior. Returns NULL until a SIB1 has been seen, so every consumer degrades to the
 *  previous blind behaviour by checking for NULL -- no other guard needed. */
const nr_pdcch_sib1_prior_t *nr_pdcch_sib1_prior_get(void);

/** Derive the 6-RB-group span [first_w, last_w] covered by the prior's frequency-domain bitmap.
 *  Returns false when the prior is absent or its bitmap is empty. The blind extent search speaks
 *  in these window indices, so this is what lets the prior be injected as an extent candidate. */
bool nr_pdcch_sib1_prior_window(int *first_w, int *last_w);

/** True when `rnti` is a legal RA-RNTI for THIS cell's PRACH configuration.
 *
 *  This is the only RNTI class a passive receiver can verify WITHOUT already knowing the answer:
 *  SI-RNTI is the fixed constant 0xFFFF, and RA-RNTI is a structured value whose fields are bounded
 *  by broadcast config. A C-RNTI, by contrast, is an arbitrary 16-bit number assigned in dedicated
 *  signalling, so a C-RNTI accept can only be believed after it REPEATS -- which is the circularity
 *  that has kept the bootstrap table full of n=1 chance hits.
 *
 *  Returns false when no SIB1 prior has been published yet, so callers must keep their existing
 *  range check as the fallback rather than treating false as "reject". */
bool nr_pdcch_sib1_prior_ra_rnti_valid(uint16_t rnti);

#endif /* NR_PDCCH_SIB1_PRIOR_H */
