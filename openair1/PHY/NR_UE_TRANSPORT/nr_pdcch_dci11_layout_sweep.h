/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.h
 * \brief Derive the DCI 1_1 FIELD LAYOUT from the air, instead of assuming it.
 *
 * THE GAP THIS CLOSES. Technique A-C recover where the PDCCH is, how long the DCI is and which
 * RNTI it carries; Technique D recovers three payload-interpretation fields (TDRA entry, DM-RS
 * additional position, MCS table). Everything else about the DCI 1_1 layout is a FIXED ASSUMPTION
 * SET in nr_pdcch_blind_dci_size() -- ~15 field widths, each annotated as traceable to one specific
 * fact about the lab cell. On any other gNB those facts differ.
 *
 * AND A CORRECT TOTAL LENGTH DOES NOT IMPLY A CORRECT LAYOUT. This has already cost a campaign:
 * two widths were wrong (bwp_indicator 1->0, time_domain_assignment 4->2) while the total was
 * right, so every field after the frequency-domain assignment was read at the wrong bit offset.
 * The RNTI still matched (it comes from the CRC, not the payload) and the allocations still looked
 * plausible (RIV precedes both errors), so nothing looked wrong except 0/83 transport blocks.
 *
 * WHY THE SEARCH IS SMALL. Two structural facts collapse it:
 *
 *  1. The ~15 widths are not 15 free parameters. They are decided by a handful of RRC switches
 *     (pdsch_Config present, tci_PresentInDCI, supplementaryUplink, CBG transmission, n_dl_bwp,
 *     DM-RS type/maxLength, dl_DataToUL_ACK size, DAI codebook). Sweeping switches rather than
 *     widths is the difference between an intractable product and a list.
 *
 *  2. Extraction only cares about the OFFSETS of the fields actually read -- RIV, TDA, MCS/RV,
 *     antenna ports, DM-RS sequence init. Fields between two of those contribute only through
 *     their SUM. So the searchable state is four group sums plus the antenna-port width, not the
 *     individual switches. Two layouts with the same sums are indistinguishable to the receiver
 *     AND decode identically, so collapsing them loses nothing.
 *
 * Then the already-derived TOTAL LENGTH pins the last group exactly, removing a whole dimension:
 * a candidate whose widths do not sum to the observed payload size is rejected before a single
 * grant is spent on it.
 *
 * TWO-STAGE SCORING, because the TB CRC is expensive. A wrong layout usually reads a nonsensical
 * MCS or RV, which costs nothing to check -- no channel estimate, no equaliser, no LDPC. Stage 1
 * scores plausibility over accepted DCIs and prunes hard; stage 2 hands the survivors to the
 * TB-CRC oracle, exactly as Technique D does.
 *
 * Field order is TS 38.212 7.3.1.2.2. Carrier indicator is fixed at 0 (no cross-carrier
 * scheduling is representable here, and modelling it needs a second serving cell anyway).
 */

#ifndef __NR_PDCCH_DCI11_LAYOUT_SWEEP_H__
#define __NR_PDCCH_DCI11_LAYOUT_SWEEP_H__

#include <stdbool.h>
#include <stdint.h>

#include "nr_pdsch_prb_set.h"   /* NR_FDRA_* modes, N_RBG, FDRA width */

/// One layout hypothesis, in the only terms extraction can distinguish.
typedef struct {
  uint8_t bwp_ind;      ///< BWP indicator width: 0, 1 or 2 (n_dl_bwp)
  uint8_t pre_mcs;      ///< vrb_to_prb + prb_bundling + rate_matching + zp_csirs  (0..6)
  uint8_t pre_ant;      ///< tb2 + harq_pid + dai + tpc + pucch_ri + pdsch_to_harq (harq/tpc/ri fixed)
  uint8_t ant_ports;    ///< 4, 5 or 6 (DM-RS type x maxLength)
  uint8_t post_ant;     ///< tci + srs + cbg + cbg_flush (0..14)
  uint8_t dmrs_type;    ///< 0 = type 1, 1 = type 2: at 5 bits both are layouts (Tables -2 vs -3)
  /* Frequency-domain assignment. resourceAllocation (type 0 / type 1 / dynamicSwitch) and rbg-Size are
   * RRC switches too, and they set the FDRA WIDTH -- get it wrong and every later field shifts, the
   * same failure as the old bwp_indicator/TDA gap. NR_FDRA_* (0 = type 1/RIV, all-zero = old layout). */
  uint8_t fdra_mode;
  uint8_t n_rbg;        ///< N_RBG of this mode's RBG configuration; 0 for type 1
} nr_dci11_layout_t;

/* Searched TDA width x DM-RS type x HARQ width. Measured over BWP 6..273 and a 28..62-bit DCI: one FDRA
 * stage (one mode, TDA 0..4) peaks at 5458 and every mode together at 26,992, so 32768 holds the whole
 * staged set without a cut. Resolver memory ~0.52 kB/entry (hist dominates): ~17.2 MB each. */
#define NR_DCI11_LAYOUT_MAX 32768
#define NR_DCI11_HIST_BINS 116   /* mcs 32 | rv 4 | tda 16 | ant ports 64 */

/// Bit offsets (MSB-first, as read_field() counts) of every field the extraction consumes.
typedef struct {
  uint16_t riv, tda, mcs, rv, ant_ports, dmrs_init, total;
  /* The antenna-ports WIDTH, carried explicitly. It cannot be recovered from the offsets:
   * dmrs_init - ant_ports is width + post_ant, and using that as the width made the
   * antenna-ports plausibility check silently never fire for any layout with TCI/SRS/CBG bits. */
  uint8_t ant_ports_bits;
  /* How many codepoints the antenna-ports table actually defines, or 0 for "do not test".
   * FORMAT-SPECIFIC and not optional: DCI 1_1 indexes TS 38.212 Table 7.3.1.2.2-1, where 12 of 16
   * rows exist at width 4, but DCI 0_1 indexes a completely different family of tables
   * (7.3.1.1.2-6..23, varying with transform precoding / DM-RS type / maxLength). Applying the
   * downlink rule to an uplink payload REJECTS VALID GRANTS -- measured: it deleted the true DCI
   * 0_1 layout during stage 1. Uplink therefore sets 0 until those row counts are verified
   * in-tree, exactly as widths 5/6 already are on the downlink side. */
  uint8_t ap_valid_rows;
  /* TDA index width and how many entries the list really has (0 = unknown, do not test). An index
   * at or beyond the list length is impossible for the true layout and common for a shifted one. */
  uint8_t tda_bits;
  uint8_t tda_valid;
  /* How the field at `riv` (width tda - riv) is read: NR_FDRA_* mode, N_RBG and the RIV width
   * (the dynamicSwitch split needs both). The plausibility test follows the mode: RIV inside the BWP,
   * or a non-empty RBG bitmap. */
  uint8_t fdra_mode;
  uint8_t n_rbg;
  uint8_t riv_bits;
} nr_dci11_offsets_t;

/** Offsets implied by a layout. `tda_bits` comes from the TDRA list Technique D already recovers,
 * `riv_bits` from the BWP size. Returns false if the layout is self-inconsistent. Pure. */
bool nr_dci11_layout_offsets(const nr_dci11_layout_t *l, uint16_t riv_bits, uint8_t tda_bits,
                             nr_dci11_offsets_t *out);

/** Every layout whose total width equals `observed_len`, written to `out` (up to `max`). Type-1 (RIV)
 * frequency-domain assignment only -- nr_dci11_layout_enumerate_fdra() searches every mode.
 * Returns the count, or -1 on bad arguments. This is the whole point: the derived DCI length is a
 * hard constraint that most of the switch space fails. Pure. */
int nr_dci11_layout_enumerate(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                              nr_dci11_layout_t *out, int max);

/** The layouts of ONE FDRA mode: the same switch space with that mode's FDRA width, the length
 * constraint pruning the impossible ones. N_RBG needs the BWP's CRB start (RBGs align to the common
 * grid). Returns 0 for rbg-Size config 2 where it gives config 1's RBG size (> 144 PRB): identical
 * reads, a duplicate. -1 on bad arguments. */
int nr_dci11_layout_enumerate_mode(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len, uint16_t bwp_start,
                                   uint16_t bwp_size, uint8_t fdra_mode, nr_dci11_layout_t *out, int max);
/** Every FDRA mode in NR_FDRA_* order (type 1 first). */
int nr_dci11_layout_enumerate_fdra(uint16_t riv_bits, uint8_t tda_bits, uint16_t observed_len,
                                   uint16_t bwp_start, uint16_t bwp_size, nr_dci11_layout_t *out, int max);

/** Stage-1 oracle: is this payload PLAUSIBLE under this layout? Checks the fields a wrong offset
 * corrupts first -- MCS not in the reserved rows, RV in range, and a RIV inside the BWP. Costs no
 * decode. A right layout passes nearly always; a wrong one fails most of the time. Pure. */
bool nr_dci11_layout_plausible(const nr_dci11_offsets_t *off, uint64_t payload, uint16_t bwp_size);

/* ---- STATEFUL RESOLVER ------------------------------------------------------------------------
 * Holds the candidate set for one cell configuration and narrows it in the two stages the header
 * describes. Deliberately shaped like nr_pdsch_config_sweep so the two behave the same way under
 * the same conditions: hypotheses are INTERLEAVED per grant rather than tested in blocks, because
 * this receiver's TB-CRC drifts on a timescale of minutes and testing A now against B later
 * compares them across that drift -- the confound that produced several wrong conclusions before.
 */

typedef struct {
  nr_dci11_layout_t hyp[NR_DCI11_LAYOUT_MAX];
  nr_dci11_offsets_t off[NR_DCI11_LAYOUT_MAX];
  uint32_t seen[NR_DCI11_LAYOUT_MAX];   ///< stage-1 payloads examined
  uint32_t pass[NR_DCI11_LAYOUT_MAX];   ///< stage-1 payloads found plausible
  uint32_t trials[NR_DCI11_LAYOUT_MAX]; ///< stage-2 decodes attempted
  uint32_t ok[NR_DCI11_LAYOUT_MAX];     ///< stage-2 decodes that passed the TB CRC
  bool     alive[NR_DCI11_LAYOUT_MAX];  ///< still a candidate
  int      n_hyp;
  int      n_alive;
  int      cursor;                      ///< round-robin position for stage 2
  int      winner;                      ///< -1 until decided
  uint32_t probe_ok[NR_DCI11_LAYOUT_MAX];  ///< code-block-0 probe passes per layout (survives sweep-context eviction)
  uint32_t probe_tr[NR_DCI11_LAYOUT_MAX];  ///< probe trials per layout
  /* INTERPRETATION FAMILIES. On one DCI many layouts read identical fields (start/len, TDA, MCS,
   * RV, NDI, HARQ, ports, nscid): their probes are the same decode and their evidence adds. Per
   * layout the family of its LAST read; per family the probe tallies. Rank-4 bed 2026-09-16:
   * 12 % marginal CRC yet no layout alone ever reached 8 passes. */
#define NR_DCI11_FAM_N 4096
  uint16_t layout_fam[NR_DCI11_LAYOUT_MAX];
  uint32_t fam_ok[NR_DCI11_FAM_N];
  uint32_t fam_tr[NR_DCI11_FAM_N];
  uint16_t riv_bits;
  uint8_t  tda_bits;
  uint16_t observed_len;
  uint16_t bwp_size;
  uint16_t bwp_start;
  uint8_t  tda_count;      ///< last nr_dci11_resolver_set_tda_count(), applied to modes armed later
  uint8_t  fdra_next;      ///< next FDRA STAGE index to arm (1..4; NR_DCI11_FDRA_STAGES = all armed / disarmed)
  /* DISTRIBUTIONAL EVIDENCE (stage 1). A correctly aligned field has structure on a live cell --
   * MCS sits on one or two values under load, RV is overwhelmingly 0, the TDA index uses one to three
   * entries, the antenna-ports codepoint is constant for a single-layer UE -- while a misaligned read
   * mixes in HARQ PID, DAI, NDI or RIV bits and looks close to uniform. Per hypothesis, one histogram
   * per read field, laid out [mcs 32 | rv 4 | tda 16 | ant ports 64]. */
  uint32_t hist[NR_DCI11_LAYOUT_MAX][NR_DCI11_HIST_BINS];
  uint32_t n_obs;          ///< payloads offered to observe(), all hypotheses alike
  uint32_t dropped_dist;   ///< hypotheses removed by the distributional test (diagnostic)
} nr_dci11_resolver_t;

/** Build the candidate set for a cell (type 1 first -- see nr_dci11_resolver_init_fdra()). Returns the
 * number of candidates, 0 if none of ANY FDRA mode fits the observed length (which means one of
 * riv_bits/tda_bits/observed_len is wrong -- a real signal, not a resolver failure). */
#define NR_DCI11_TDA_UNKNOWN 0xFF /* tda_bits: enumerate every width 0..4 (the list size is an RRC switch) */
int nr_dci11_resolver_init(nr_dci11_resolver_t *r, uint16_t bwp_size, uint16_t riv_bits,
                           uint8_t tda_bits, uint16_t observed_len);
/** As nr_dci11_resolver_init(), remembering the BWP CRB start so the RA type 0 / dynamicSwitch modes can
 * be ARMED later. FDRA MODE STAGING: only type 1 is enumerated here, so a type-1 cell behaves exactly as
 * before the other modes existed. Stage 1 cannot refute the other modes' aliases on a type-1 cell (same
 * MCS/RV/TDA/antenna-port fields, and a RIV read as a bitmap is almost never empty: survivors x2.6-6.7),
 * nor refute type 1 on a type-0 cell (a type-1 window starting on constant leading bits always reads an
 * in-range RIV). Only the TB CRC can, so the next mode is armed once stage 2 has refuted every live
 * layout -- nr_dci11_resolver_all_refuted() -- and the caller then calls nr_dci11_resolver_arm_next_mode(). */
int nr_dci11_resolver_init_fdra(nr_dci11_resolver_t *r, uint16_t bwp_start, uint16_t bwp_size, uint16_t riv_bits,
                                uint8_t tda_bits, uint16_t observed_len);

#define NR_DCI11_FDRA_STAGES 5
/** Stage index of an FDRA mode in the arming order: type 1 = 0, type 0 cfg1 = 1, dynamicSwitch cfg1 = 2,
 * dynamicSwitch cfg2 = 3, type 0 cfg2 = 4 (the bulk, last). -1 for a bad mode. */
int nr_dci11_fdra_stage(uint8_t fdra_mode);

/** A type-1 layout passed a TB / code-block CRC: type 1 is proven. Kill every non-type-1 layout, revive
 * `type1_idx` if its stage was retired, and never arm again. Returns the number killed. */
int nr_dci11_resolver_disarm(nr_dci11_resolver_t *r, int type1_idx);

/* Trials with zero TB-CRC / code-block passes before a layout counts as refuted by stage 2. 64 = the
 * stage-2 decision floor (DCI11_S2_MIN_TRIALS). Against the lowest true-layout rate measured on these
 * rigs (12 % marginal CRC, rank-4 bed 2026-09-16) the truth reads 0/64 with probability 0.88^64 =
 * 2.8e-4 -- and a false refutation is not destructive: arming only ADDS the next mode's hypotheses
 * (nothing already live is dropped), i.e. it costs the pre-staging dilution, not the answer. */
#define NR_DCI11_FDRA_ARM_MIN_TRIALS 64

/** True when no live layout has a pass (own feed(), code-block probe, or interpretation family) and the live
 * set has taken >= min_trials x n_alive stage-2 trials IN TOTAL (aggregate, so a layout that can never be
 * trialled cannot freeze staging). False with a winner or an empty set. The caller must also gate on link
 * health: on a dead link every layout reads 0 passes. */
bool nr_dci11_resolver_all_refuted(const nr_dci11_resolver_t *r, uint32_t min_trials);

/** Arm the next FDRA stage (type 0 cfg1, dynamicSwitch cfg1, dynamicSwitch cfg2, type 0 cfg2 -- skipping any
 * with no layout at this length). The refuted live set (every live layout WITHOUT a pass) is retired first,
 * so n_alive stays one stage; the new layouts are APPENDED alive with fresh counters. Call only after
 * nr_dci11_resolver_all_refuted(). Returns the mode armed and *added (may be NULL) its count, or -1 once
 * every stage is armed. Caller-serialised (the stage-1 observer). */
int nr_dci11_resolver_arm_next_mode(nr_dci11_resolver_t *r, int *added);

/** Append offsets to a resolver built by nr_dci_resolver_init_from_offsets() (DCI 0_1 FDRA staging), alive,
 * fresh counters, published with a release store. Entries whose total differs from the resolver's stop
 * the append. Returns the number appended (short at the cap). */
int nr_dci_resolver_append_offsets(nr_dci11_resolver_t *r, const nr_dci11_offsets_t *offsets, int n);

/** Build a resolver directly from a caller-supplied offsets list, for a DCI format other than 1_1.
 * The resolver only ever reads offsets, so it is format-agnostic: DCI 0_1 exposes the same fields
 * to extraction (RIV, TDA, MCS, RV, antenna ports, DM-RS init) and reuses this rather than
 * duplicating two hundred lines of pruning and Wilson scoring. `hyp[]` is left zeroed -- a caller
 * using this owns its own layout descriptors and must not read back nr_dci11_layout_t. */
int nr_dci_resolver_init_from_offsets(nr_dci11_resolver_t *r, uint16_t bwp_size,
                                      const nr_dci11_offsets_t *offsets, int n);

/** STAGE 1. Offer one accepted DCI payload. Every live candidate is scored for plausibility; one
 * that has been implausible too often is dropped. Costs no decode. Returns the number still alive.
 * Never drops the last candidate: an empty set can never converge, and a run of unlucky payloads
 * must not be able to erase the answer. */
int nr_dci11_resolver_observe(nr_dci11_resolver_t *r, uint64_t payload);

/** Tell every hypothesis how many entries the TDA list has, enabling the impossible-index test.
 * 0 = unknown (the test stays off). */
void nr_dci11_resolver_set_tda_count(nr_dci11_resolver_t *r, uint8_t tda_count);

/** Stage-1 distributional score of hypothesis i, in BITS: how much more compressible the fields it
 * reads are than uniform, summed over MCS, RV, TDA and antenna ports (n * (log2 K - H), with the
 * Miller-Madow small-sample correction). Higher = more structure = more likely aligned. Pure. */
double nr_dci11_resolver_score(const nr_dci11_resolver_t *r, int i);

/** Thompson sampling over n arms: draw p_i ~ Beta(1 + ok_i + prior_i, 1 + trials_i - ok_i) and
 * return the argmax. Trials go to each arm in proportion to the posterior probability that it is
 * the best one, so a leading hypothesis gets most grants and settles in tens of trials instead of
 * the thousands round-robin needs (the 1/N dilution measured at N=14: 0.3 % DL CRC). Nothing is ever
 * deleted -- a starved arm still gets sampled -- so a 20 % retransmission rate cannot lose the truth.
 * `prior` may be NULL (all zero). `rng` is caller-held xorshift state, must be non-zero. */
int nr_dci11_thompson_pick(const uint32_t *ok, const uint32_t *trials, const double *prior, int n,
                           uint64_t *rng);

/** STAGE 2. Pick the next live candidate to decode under, round-robin. Returns its index and fills
 * *out, or -1 when the set is empty. Once a winner exists it is returned every time. */
int nr_dci11_resolver_next(nr_dci11_resolver_t *r, nr_dci11_offsets_t *out);

/** STAGE 2 feedback: the TB CRC outcome of a grant decoded under candidate `idx`.
 * Returns the winning index once one separates, else -1. */
int nr_dci11_resolver_feed(nr_dci11_resolver_t *r, int idx, bool tb_crc_ok);

/** Winner index, or -1. */
int nr_dci11_resolver_winner(const nr_dci11_resolver_t *r);

/** Per-field bit widths that reproduce a layout, for handing back to
 * nr_pdcch_blind_extract_opts_t. Only the SUMS matter to extraction, so a group's total is carried
 * on ONE member of that group and the rest are zero -- these are therefore NOT the gNB's real
 * per-field widths and must not be logged as if they were. What they are guaranteed to be is
 * offset-identical to the layout, which is the only property the extractor depends on and which
 * nr_dci11_layout_apply_roundtrip() checks directly. */
typedef struct {
  int bwp_indicator_bits;
  int vrb_to_prb_bits;      ///< carries pre_mcs (vrb + prb bundling + rate match + zp csirs)
  int prb_bundling_bits, rate_matching_bits, zp_csirs_bits;   ///< always 0, see above
  int tb2_bits;             ///< carries pre_ant minus the constant TPC(2) + PUCCH-RI(3)
  int harq_pid_bits, dai_bits, pdsch_to_harq_bits;            ///< always 0, see above
  int antenna_ports_bits;
  int dmrs_config_type;     ///< 0 = type 1, 1 = type 2 (which antenna-ports table the width means)
  int tci_bits;             ///< carries post_ant (tci + srs + cbg + flush)
  int srs_request_bits, cbg_bits;                             ///< always 0, see above
  int fdra_mode;            ///< NR_FDRA_*: the extractor derives the FDRA width (and N_RBG) from it
} nr_dci11_field_bits_t;

/** Translate a layout into those widths. Returns false if the layout cannot be represented
 * (pre_ant below the constant TPC+PUCCH-RI floor, which no legal layout produces). Pure. */
bool nr_dci11_layout_to_field_bits(const nr_dci11_layout_t *l, nr_dci11_field_bits_t *out);

/** Self-check: do those widths rebuild exactly the layout's offsets? Pure, and the reason the
 * mapping above can be trusted without reading it twice. */
bool nr_dci11_layout_apply_roundtrip(const nr_dci11_layout_t *l, uint16_t riv_bits, uint8_t tda_bits);

#endif /* __NR_PDCCH_DCI11_LAYOUT_SWEEP_H__ */
