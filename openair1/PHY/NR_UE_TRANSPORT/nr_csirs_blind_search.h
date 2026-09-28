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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h
 * \brief Recover a cell's NZP CSI-RS resource configuration from the air.
 *
 * WHY THIS EXISTS. Every other parameter this passive receiver needs is now derived: the CORESET
 * and search space (CSS0 autoconf), the DCI length (length sweep), the RNTI (from the DCI CRC
 * mask), and the TDRA entry / DM-RS additional position / MCS table (Technique D). CSI-RS was the
 * last one still requiring hand-written `csirs_monitor` values copied out of the DU log, which is
 * exactly what cannot be done against a commercial gNB.
 *
 * WHY IT NEEDS ITS OWN ORACLE. Techniques A-D all score on a CRC. CSI-RS carries no CRC, and its
 * configuration is dedicated RRC, so it is not broadcast anywhere a passive receiver can read.
 * What it does have is a KNOWN sequence: the gold sequence is a function of
 * (N_RB, symbols_per_slot, slot, symbol, scramblingID) alone -- the row, the frequency-domain
 * bitmap and the density decide only WHICH resource elements carry it. So the oracle is
 * correlation: score a candidate by how well the received REs match the sequence that candidate
 * predicts. A right guess correlates near 1, a wrong one near 1/sqrt(N). This is the same test
 * already used for PDCCH DM-RS (`DMRSSTAT ... real DM-RS ~0.8-0.95`).
 *
 * WHY THE REFERENCE COMES FROM THE REAL GENERATOR. The mapping from resource element to sequence
 * index is intricate (row-dependent k-prime/l-prime, CDM groups, density 0.5 parity). Re-deriving
 * it here would risk a subtly wrong reference, and a wrong reference sequence does not look like a
 * weak signal -- it looks like a dead channel, which is the failure this module exists to avoid
 * diagnosing. `nr_csirs_blind_reference()` therefore calls nr_generate_csi_rs() itself and treats
 * whatever it writes as the truth.
 *
 * STRUCTURE. The two load-bearing pieces are pure and unit-tested without any PHY state:
 *   - nr_csirs_blind_correlate(): the oracle.
 *   - nr_csirs_blind_infer_period(): turns a set of hit slots into (period, offset), constrained
 *     to the periodicities 38.331 actually allows.
 * The PHY-dependent wrapper is a thin shim over those two.
 */

#ifndef __NR_CSIRS_BLIND_SEARCH_H__
#define __NR_CSIRS_BLIND_SEARCH_H__

#include <stdbool.h>
#include <stdint.h>

/// One CSI-RS resource hypothesis. Mirrors the fields `csirs_monitor` takes, so a confirmed
/// candidate can be printed straight back out as a config line.
typedef struct {
  uint8_t  row;          ///< TS 38.211 Table 7.4.1.5.3-1 row index
  uint16_t freq_domain;  ///< frequency-domain allocation bitmap (the "b" of get_csi_mapping_parms)
  uint8_t  symb_l0;      ///< first OFDM symbol
  uint8_t  symb_l1;      ///< second OFDM symbol (rows that use one; else 0)
  uint8_t  cdm_type;
  uint8_t  freq_density; ///< 0/1 = dot5 (even/odd RB), 2 = one, 3 = three
  uint16_t scramb_id;    ///< almost always the PCI, but not required to be
  uint16_t start_rb, nr_of_rbs;
} nr_csirs_candidate_t;

/** Normalised correlation between a received slot and a reference grid, over the REs the
 * reference actually occupies. Returns |<rx,ref>| / (||rx|| ||ref||) in [0,1], or -1.0 when the
 * reference is empty (a candidate that maps no RE cannot be scored, and must not read as 0.0 --
 * that would rank it alongside a genuine mismatch).
 * Pure: no PHY state, no allocation. `n` is the number of REs in both buffers. */
double nr_csirs_blind_correlate(const int16_t *rx_re_im, const int16_t *ref_re_im, int n);

/** Same, and reports how many REs the reference occupied. USE THIS: rho alone is not comparable
 * across candidates (|rho| ~ 0.89/sqrt(n_used) for noise), so the scale-free score is
 * rho * sqrt(n_used) -- ~0.89 for noise at any size, sqrt(n_used) for a perfect match. */
double nr_csirs_blind_correlate_n(const int16_t *rx_re_im, const int16_t *ref_re_im, int n, int *n_used);

/** Sequence-free positional evidence: mean power ON the candidate's REs over mean power on the
 * other REs of the RBs it touches. >1 boosted pilot, <1 zero-power, ~1 noise/PDSCH. Needs no
 * scramblingID, so it still speaks when the sequence hypothesis is wrong. Pure. */
double nr_csirs_blind_energy_ratio(const int16_t *rx_re_im, const int16_t *ref_re_im, int n);

/** Channel-robust score: coherent inside sub-bands of `sub_res` occupied REs, magnitudes combined
 * across sub-bands. Noise reads ~1.0 at any candidate size; a correct sequence reads
 * ~sqrt(REs per sub-band). Use this OTA -- the flat whole-band correlation is destroyed by the
 * channel's phase ramp over a wide carrier. Pure. */
double nr_csirs_blind_correlate_blocks(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                       int sub_res, int *n_used);

/** Same statistic, but reading rx at (i + rx_shift) % n -- the FFT-ordered position of the CRB-order
 * reference index i. rx_shift is frame_parms->first_carrier_offset; see the definition for why the
 * receiver grid and the generated reference do not share a convention. Pure. */
double nr_csirs_blind_correlate_blocks_shift(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                             int sub_res, int rx_shift, int *n_used);

/** Sequence-free energy ratio with the same rx index mapping. Pure. */
double nr_csirs_blind_energy_ratio_shift(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                         int rx_shift);

/** Same statistic, but over the best CONTIGUOUS RUN of sub-bands rather than all of them, and it
 * reports which run won. A candidate asserts the resource spans the whole carrier; a real CSI-RS
 * often covers only part of the BWP, and the whole-band mean then reads ~fraction * perfect, which
 * is indistinguishable from a near-miss. Returns -1.0 when fewer than two sub-bands are scorable. */
double nr_csirs_blind_correlate_bestrun(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                        int sub_res, int *first_block_out, int *n_blocks_out,
                                        int *n_used_out);

/// Periodicities TS 38.331 CSI-ResourcePeriodicityAndOffset admits, in slots.
#define NR_CSIRS_BLIND_N_PERIODS 13
extern const uint16_t nr_csirs_blind_periods[NR_CSIRS_BLIND_N_PERIODS];

/** Infer (period, offset) from the absolute slots at which a candidate scored a hit.
 *
 * Deliberately NOT a GCD of the deltas: a missed occurrence (fading, a slot the receiver did not
 * observe) makes one delta a multiple of the true period, and a GCD over a sparse or noisy hit set
 * happily returns 1. Instead it is a hypothesis test over the 13-element legal set: keep the
 * periodicities under which every observed hit falls in the same slot of the cycle, and return the
 * LARGEST of them.
 *
 * Largest, not smallest -- this is the part that is easy to get wrong. Every DIVISOR of the true
 * period also passes the mod test (hits at period 20 are equally consistent with 4, 5 and 10), so
 * a smallest-first rule degenerates to 4 almost always. A period LONGER than the truth is rejected
 * on its own, because its hits land at different phases. The largest survivor is therefore the
 * answer, bounded by the observation span so the result stays falsifiable.
 *
 * Requires at least `min_hits` hits, and rejects the degenerate case where the span of the hits is
 * shorter than the period it would claim (one occurrence proves nothing about periodicity).
 * Returns true and fills the period and offset outputs on success. Pure. */
bool nr_csirs_blind_infer_period(const uint32_t *hit_slots, int n_hits, int min_hits,
                                 uint16_t *period, uint16_t *offset);

/** nr_csirs_blind_infer_period(), plus the one multi-slot shape a resource GEOMETRY really takes: the
 * same (row, bitmap, symbols) sent at TWO phases of one period. A TRS set is exactly that (38.214
 * 5.1.6.1.1: its resources span two consecutive slots), and no single phase explains its hits. A
 * single phase is always tried first; two phases are accepted only for the largest legal period at
 * which the hits take exactly two phases with at least two hits EACH, so one stray hit is never
 * promoted to a phase. Fills off[0] < off[1] (n_off = 2) or off[0] (n_off = 1). Pure. */
bool nr_csirs_blind_infer_period2(const uint32_t *hit_slots, int n_hits, int min_hits,
                                  uint16_t *period, uint16_t off[2], int *n_off);

/** Format a confirmed candidate as a `csirs_monitor` config entry
 * ("row:start_rb:nr_rbs:freq_domain:symb_l0:symb_l1:cdm_type:freq_density:scramb_id:period:offset").
 * Returns the number of characters written, or 0 if it would not fit. Pure. */
int nr_csirs_blind_format(const nr_csirs_candidate_t *c, uint16_t period, uint16_t offset,
                          char *out, int out_len);

/* ---- CANDIDATE ENUMERATION AND SCHEDULING -----------------------------------------------------
 * The primitives above are the oracle and the periodicity test. What follows is the search itself.
 *
 * SCOPE, STATED UP FRONT. The round-robin enumerates rows 1-5 (one set bitmap bit each): a TRS pair
 * and the ordinary CQI resources. Rows 6-18 (8-32 ports, what a massive-MIMO cell uses for CSI
 * acquisition) need 2-6 simultaneous bitmap bits and a second symbol, tens of thousands of
 * hypotheses -- far too many to round-robin. They are reached FOOTPRINT-FIRST instead (see
 * nr_csirs_blind_fp_match below) and appended to the same population once the air shows them.
 *
 * scramblingID is NOT swept by default. It is almost always the PCI, which acquisition already
 * gives us, and sweeping 1024 values would multiply the space by three orders of magnitude for a
 * parameter we can simply try first and fall back on.
 */

/* The extended enumeration (rows 1-5, row-1's full 4-bit bitmap, row-2's dot5 densities, symbols
 * 0-13) produces ~680 candidates; at 256 the list was silently truncated mid-row and everything
 * after row 2 was never tried. Cost of the larger space is CONVERGENCE TIME, not CPU per slot: one
 * candidate is still scored per slot, so a full pass is ~1 s at this slot rate and the per-candidate
 * evidence (32 samples before the sweep pins) takes ~35 s. */
#define NR_CSIRS_BLIND_MAX_CAND 1024
/* Confirmed resources kept per search (NZP and ZP each). A PDSCH carries at most
 * NFAPI_MAX_NUM_CSI_RATEMATCH (4) of them per slot; 8 covers a TRS pair + CQI + CSI-IM with room. */
#define NR_CSIRS_BLIND_MAX_CONF 8
/* Distinct predicted occasions with decoded-grant evidence kept per ZP (nr_csirs_blind_zp_grant_feed);
 * revocation counts the contradictions among them. A false ZP contradicts on EVERY decoded grant that
 * crosses it, so it is revoked at its second one for any window >= 2; the window only bounds how far
 * apart two isolated spurious contradictions (a phantom grant) may be before they add up on a true ZP. */
#define NR_CSIRS_BLIND_ZP_GRANT_WINDOW 8
/* One rejected-phase bit for every offset of every legal period, per ZP evidence epoch.
 * Unlike a slot FIFO, this cannot forget a contradiction under dense observations. */
#define NR_CSIRS_BLIND_ZP_PHASE_BITS (4 + 5 + 8 + 10 + 16 + 20 + 32 + 40 + 64 + 80 + 160 + 320 + 640)
#define NR_CSIRS_BLIND_ZP_PHASE_WORDS ((NR_CSIRS_BLIND_ZP_PHASE_BITS + 63) / 64)

/* The probation horizon is the maximum of nr_csirs_blind_periods, not a deployment
 * duration or false-alarm probability. A unit test checks the lattice identity. */
uint16_t nr_csirs_blind_zp_lattice_horizon(void);

typedef struct {
  int owner; ///< candidate index + 1; zero is a free bank entry
  uint16_t period, off[2];
  uint8_t n_off;
  uint32_t proposed_slot, support_start, admitted_slot;
  uint64_t votes[2], promotion_votes;
  uint32_t occupied[NR_CSIRS_BLIND_ZP_PHASE_BITS]; ///< UINT32_MAX means unobserved
} nr_csirs_zp_probation_t;

typedef struct {
  nr_csirs_candidate_t cand[NR_CSIRS_BLIND_MAX_CAND];
  uint32_t             hits[NR_CSIRS_BLIND_MAX_CAND];
  uint32_t             tried[NR_CSIRS_BLIND_MAX_CAND];
  uint32_t             zp_last_slot[NR_CSIRS_BLIND_MAX_CAND]; ///< last distinct ZP observation
  uint32_t             zp_holes[NR_CSIRS_BLIND_MAX_CAND]; ///< raw holes, before the population test
  uint32_t             zp_epoch[NR_CSIRS_BLIND_MAX_CAND]; ///< monotonically numbered evidence epochs
  uint32_t             zp_revocations[NR_CSIRS_BLIND_MAX_CAND];
  uint8_t              zp_contradictions[NR_CSIRS_BLIND_MAX_CAND]; ///< scorable predicted misses since last qualified hit
  uint16_t             zp_selected_period[NR_CSIRS_BLIND_MAX_CAND]; ///< last proposed phase, for diagnostics
  uint16_t             zp_selected_off[NR_CSIRS_BLIND_MAX_CAND][2];
  uint8_t              zp_selected_n_off[NR_CSIRS_BLIND_MAX_CAND];
  uint64_t             zp_rejected_phase[NR_CSIRS_BLIND_MAX_CAND][NR_CSIRS_BLIND_ZP_PHASE_WORDS];
  uint64_t             zp_failed_run[NR_CSIRS_BLIND_MAX_CAND]; ///< cumulative contradicted support; durable across epochs
  uint32_t             zp_evidence_floor[NR_CSIRS_BLIND_MAX_CAND]; ///< do not reuse pre-withdrawal/eviction hits
  uint32_t             zp_grant_slot[NR_CSIRS_BLIND_MAX_CAND][NR_CSIRS_BLIND_ZP_GRANT_WINDOW]; ///< decoded-grant evidence ring
  uint8_t              zp_grant_n[NR_CSIRS_BLIND_MAX_CAND], zp_grant_w[NR_CSIRS_BLIND_MAX_CAND];
  uint8_t              zp_grant_contra[NR_CSIRS_BLIND_MAX_CAND]; ///< bit j: a grant of ring entry j contradicted
  uint8_t              zp_grant_support[NR_CSIRS_BLIND_MAX_CAND]; ///< bit j: a grant of ring entry j supported
  uint32_t             zp_grant_floor[NR_CSIRS_BLIND_MAX_CAND]; ///< export slot: older evidence is stale
  nr_csirs_zp_probation_t zp_bank[NR_CSIRS_BLIND_MAX_CONF]; ///< shared probation + export capacity
  double               best_rho[NR_CSIRS_BLIND_MAX_CAND];
  uint32_t             hit_slot[NR_CSIRS_BLIND_MAX_CAND][8]; ///< NZP first 8; ZP current evidence epoch
  uint8_t              n_hit_slot[NR_CSIRS_BLIND_MAX_CAND];
  int                  n;
  int                  cursor;
  int                  confirmed;   ///< index of a resolved resource, or -1
  int                  pinned;      ///< candidate served on every next() while pin_left > 0, or -1
  uint32_t             pin_left;    ///< remaining pinned next() calls
  uint16_t             period, offset; ///< of `confirmed` (the FIRST confirmation; legacy single view)
  /* EVERY confirmed resource. A cell carries several and PDSCH is rate-matched around all of them. */
  int                  n_conf;
  int                  conf_idx[NR_CSIRS_BLIND_MAX_CONF];
  uint16_t             conf_period[NR_CSIRS_BLIND_MAX_CONF];
  uint16_t             conf_off[NR_CSIRS_BLIND_MAX_CONF][2];
  uint8_t              conf_n_off[NR_CSIRS_BLIND_MAX_CONF];
  int                  cycle_left, cycle_pad, cycle_last; ///< phase-diverse round-robin cadence
} nr_csirs_blind_state_t;

/** True when candidate idx is one of the confirmed resources. Pure. */
bool nr_csirs_blind_is_confirmed(const nr_csirs_blind_state_t *st, int idx);

/** Confirmed resources that occur in `absolute_slot`, written to idx_out (up to max). Pure. */
int nr_csirs_blind_occurring(const nr_csirs_blind_state_t *st, uint32_t absolute_slot, int *idx_out, int max);

/** Due admitted ZP geometries, once each: exports, probation and unresolved divisors.
 * Never use this list for rate matching. At most MAX_CONF entries. */
int nr_csirs_blind_zp_due(const nr_csirs_blind_state_t *st, uint32_t absolute_slot, int *idx_out, int max);

/** Enumerate candidate resources for a cell. `scramb_id` is normally the PCI.
 * Returns the count, or -1 on bad arguments. */
int nr_csirs_blind_enumerate(nr_csirs_candidate_t *out, int max, uint16_t n_rb, uint16_t scramb_id);

/** Set bits get_csi_mapping_parms() requires for @p row (1-18), or -1 for anything else.
 * Its bitmap walk is UNBOUNDED: hand it a bitmap with fewer set bits than the row needs and it
 * spins forever on the receive thread. */
int nr_csirs_blind_row_needs_bits(uint8_t row);

/** CSI-RS antenna ports of a row (TS 38.211 Table 7.4.1.5.3-1, rows 1-18); 0 for anything else.
 *  Reference generation must provide this many per-port buffers. */
int nr_csirs_blind_row_ports(uint8_t row);

/** True when @p c may safely be handed to get_csi_mapping_parms(). Call this before generating a
 * reference from any candidate that did not come straight out of nr_csirs_blind_enumerate(). */
bool nr_csirs_blind_candidate_safe(const nr_csirs_candidate_t *c);

/** Initialise a search state from an enumeration. Returns the candidate count. */
int nr_csirs_blind_init(nr_csirs_blind_state_t *st, uint16_t n_rb, uint16_t scramb_id);

/** Which candidate to test in this slot. Round-robin, so every candidate sees statistically the
 * same channel -- the same reason Technique D interleaves per grant. Confirmed candidates leave the
 * rotation. A round is padded to a stride coprime to the legal periods, avoiding phase aliasing
 * with regular slot visits. Returns -1 when empty, full, or all candidates confirmed. */
int nr_csirs_blind_next(nr_csirs_blind_state_t *st);

/** Serve candidate idx on EVERY next() call for up to `budget` calls (or until confirmed), then resume
 *  round-robin. Round-robin scores a candidate only when its turn lands on a CSI-RS slot -- about once
 *  per n*period calls -- so a candidate the scramblingID sweep is working on, or has just solved,
 *  would otherwise wait minutes for the hits it needs. */
void nr_csirs_blind_pin(nr_csirs_blind_state_t *st, int idx, uint32_t budget);
#define NR_CSIRS_BLIND_PIN_SWEEP_CALLS   (32 * 640)  /* 1024 ids / 32 per aligned visit x longest period */
#define NR_CSIRS_BLIND_PIN_CONFIRM_CALLS (4 * 640)   /* > CSIRS_MIN_HITS periods at the longest period */

/** Record the correlation a candidate scored in `absolute_slot`.
 * `rho_null` is the median score of the OTHER candidates tested recently: the detection bar is
 * relative to that, never an absolute number, because the right absolute threshold depends on
 * occupancy, gain and bandwidth and would have to be recalibrated per deployment -- exactly the
 * kind of constant this project replaces with a measured one.
 * Returns true once the resource is confirmed (a periodicity has been inferred). */
bool nr_csirs_blind_feed(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                         double rho, double rho_null);

/* ---- ZERO-POWER CSI-RS ---------------------------------------------------------------------------
 * A ZP CSI-RS is a rate-matching pattern only: the same RE pattern as an NZP row, carrying no
 * energy while the PDSCH scheduled around it does. Observable without any reference sequence:
 * the energy on the pattern's REs against the energy on the other REs of the same RBs and symbol. */

/** 1 - min(1, E_on / E_off): E_on = mean |rx|^2 on the REs `ref` occupies. E_off is the
 * weakest off-pattern subcarrier class after averaging each class over touched RBs. A quiet
 * subset of a wider comb is not an identifiable geometry, and isolated/boosted pilots cannot
 * manufacture a data background: REs dark only beside a pilot (lab G4: an 8-port NZP of which one
 * antenna receives port 0 only) read ~0. ~1 for a complete ZP pattern under scheduled PDSCH, also a
 * wide one (8 RE/RB) when the candidate covers every dark class; ~0 for data or an NZP resource.
 * Conservative if other resources add off-pattern holes (two holes in one symbol are refused).
 * Cannot, in one symbol, reject dark REs covering every dark class beside >= 4 bright non-PDSCH
 * classes (a wide NZP in a slot without PDSCH): revocation withdraws such an export once PDSCH lands
 * there -- nr_csirs_blind_zp_grant_feed() after 2 decoded grants crossing it, whatever their band share
 * or the NZP boost; nr_csirs_blind_zp_feed_pair() only for data over > ~half the band at the NZP EPRE.
 * -1 when the reference is empty, <= 3 off-pattern classes remain (too few to tell a sparse pilot
 * from data), or an off-pattern class carries no energy. Rationale: nr_csirs_blind_search.c. Pure. */
double nr_csirs_blind_zero_score(const int16_t *rx_re_im, const int16_t *ref_re_im, int n);

/** ZP energy score with the rx index mapped to FFT order (rx_shift = first_carrier_offset). Pure. */
double nr_csirs_blind_zero_score_shift(const int16_t *rx_re_im, const int16_t *ref_re_im, int n,
                                       int rx_shift);

/** Same, with occupancy equal to the union of generated reference planes. ZP geometry is the
 * full port union: plane 0 alone omits other CDM groups (for example row 4's ports 2/3). */
double nr_csirs_blind_zero_score_ports_shift(const int16_t *rx_re_im, const int16_t *const *refs,
                                             int n_refs, int n, int rx_shift);

/** Same completeness score, optionally reporting the previous per-RB-median score on the
 * same reference occupancy. For row 2 this is the exact old ZP statistic. Diagnostic only:
 * the median score must never feed confirmation, scheduling, or population state. */
double nr_csirs_blind_zero_score_evidence_shift(const int16_t *rx_re_im, const int16_t *const *refs,
                                               int n_refs, int n, int rx_shift, double *median_score);

/// Minimum ZP score counted as a (raw) hole; nr_csirs_blind_zp_score_qualifies() also needs the population bar.
#define NR_CSIRS_BLIND_ZP_MIN_SCORE 0.5

/** The existing ZP hit predicate, shared with diagnostic comparison so its thresholds cannot drift. */
bool nr_csirs_blind_zp_score_qualifies(double score, double score_null);

/** nr_csirs_blind_feed() for the ZP search. Repeated occasions in the same candidate/slot count
 * once. A raw hole on more than half the observed slots is structural and cannot confirm;
 * population-threshold misses do not erase raw-hole evidence. Unscorable samples are ignored.
 * Finite observations lacking a joint raw hole veto claimed phases within the positive-evidence epoch.
 * Population-suppressed raw holes remain unresolved. Discovery creates unexported probation;
 * distinct prospective votes must span the complete legal-period lattice horizon and exceed
 * any previously contradicted held-out run. Both offsets need independent support. A harmonic
 * remains unexported until measured divisor-only observations resolve every viable divisor.
 * Eight admitted/exported geometries share bounded scheduling; phase timestamps and failed-run
 * history survive discovery resets and revocation. New measured evidence may recover a phase.
 * Confirmed resources remain observable: two distinct scorable occupied predicted occasions
 * since the last qualified hole revoke them. Unscorable/population-suppressed holes do not count.
 * Revocation affects only export decisions made AFTER this feed (including future k0 queries).
 * Feed observations in nondecreasing absolute-slot order, under the existing sequential owner.
 * Counter overflow and slot wrap/backwards observations fail closed until cell reset. */
bool nr_csirs_blind_zp_feed(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                            double score, double score_null);

/** Two-symbol ZP evidence: both scores must be scorable and qualify on the SAME slots. The
 * inferred period/phase therefore validates both symbols, not their independently inferred union.
 * Raw holes on either symbol count toward the structural veto; population misses never erase them.
 * The single-symbol entry point above passes the same score twice. */
bool nr_csirs_blind_zp_feed_pair(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot,
                                double score, double other_score, double score_null);

/* ---- DECODED-GRANT EVIDENCE ON AN EXPORTED ZP ------------------------------------------------------
 * The full-band score above cannot revoke a false ZP whose REs carry PDSCH on only part of the band
 * (averaged over all the pattern's RBs the data stays above 0.5), nor beside an NZP boosted over the
 * PDSCH EPRE, and in an idle symbol it reads exactly like data on the pattern -- which is why a
 * qualified hole must clear its contradictions there. A DECODED grant has none of those ambiguities:
 * the receiver knows the PRBs and symbols that carry data, and it rate-matched this ZP out of them.
 * Score only those REs, against the grant's own data REs on a data-only symbol (no DM-RS, no CSI-RS):
 * a true ZP reads ~1 (noise against data), data on the pattern ~0, whatever the band share, the load
 * pattern or the NZP boost. A grant whose reference is not clearly above the noise floor (a false DCI
 * accept, a PRB/symbol/k0 hypothesis that looks where nothing was sent) is no evidence. */

/** Adds |y|^2 of the REs of one FFT'd symbol at the subcarriers-in-RB of @p re_even / @p re_odd
 * (12-bit masks by CRB parity, the extractor's convention) of every RB set in @p rb_bitmap
 * (bit rb = RB rb, n_rb bits; CRB = rb + crb_offset) to *sum, and their count to *n. rx is read at
 * (i + rx_shift) % n_fft like every comparator here. Pure. */
void nr_csirs_blind_re_energy(const int16_t *rx_re_im, int n_fft, int rx_shift, const uint32_t *rb_bitmap, int n_rb,
                              int crb_offset, uint16_t re_even, uint16_t re_odd, double *sum, uint32_t *n);

/** Hypothesis-free noise floor of one FFT'd symbol: adds |y|^2 of the guard bins (the n_fft - n_occupied FFT bins
 * outside the carrier, which starts at rx_shift) nearest the two carrier edges -- 1/8 of the guard on each side,
 * where an analog filter attenuates least; leakage from busy edge subcarriers only raises the estimate, which
 * only makes the gate below stricter. Adds nothing when the guard is shorter than 8 bins. Pure. */
void nr_csirs_blind_guard_energy(const int16_t *rx_re_im, int n_fft, int rx_shift, int n_occupied, double *sum,
                                 uint32_t *n);

/** Data reference over the noise floor that a grant needs to count: a TRUE ZP reads noise on its REs, and it can
 * only be contradicted (score <= 0.5) when the data reference is <= 2x the noise; 4x (6 dB) keeps a further 2x
 * margin for the guard-vs-in-band noise mismatch. */
#define NR_CSIRS_BLIND_ZP_GRANT_MIN_SNR 4.0

/** 1 - min(1, (e_zp / n_zp) / (e_data / n_data)): e_zp on the ZP REs inside the grant's PRBs, e_data on the grant's
 * data-only reference symbol, e_noise from nr_csirs_blind_guard_energy() on that symbol. -1 (no evidence) when the
 * ZP or data set is empty, no noise floor was measured, or the data mean is below NR_CSIRS_BLIND_ZP_GRANT_MIN_SNR
 * times the noise mean. Known limit: interference on a TRUE ZP at or above the grant's own signal (SINR <= 0 dB)
 * reads as a contradiction. Pure. */
double nr_csirs_blind_zp_grant_score(double e_zp, uint32_t n_zp, double e_data, uint32_t n_data, double e_noise,
                                     uint32_t n_noise);

/** Decoded-grant evidence for exported ZP @p idx in @p absolute_slot (a score from the functions above).
 * Ignored unless idx is exported, the slot is one of its predicted occasions and not before the export (evidence
 * queued under an earlier export); unscorable (< 0) is ignored. Grants of one slot count once, and a slot is a
 * contradiction only if NO scorable grant of it supports the ZP (one bad hypothesis cannot decide the slot).
 * Separate from the full-band feed: it may arrive out of slot order (decode consumers run behind the search),
 * and a qualified full-band hole does not clear it. Two contradicted slots among the last
 * NR_CSIRS_BLIND_ZP_GRANT_WINDOW slots with evidence revoke the export, exactly as two full-band contradictions
 * do. Returns true while idx stays exported. */
bool nr_csirs_blind_zp_grant_feed(nr_csirs_blind_state_t *st, int idx, uint32_t absolute_slot, double grant_score);

/* ---- ROWS 6-18: FOOTPRINT-FIRST -------------------------------------------------------------------
 * A candidate's sequence is only worth testing once the air shows its RE PATTERN. The pattern is
 * measured sequence-free, with the EPR statistic evaluated per slot at subcarrier granularity: for the
 * one symbol the RT tap FFTs anyway, mean |y|^2 per subcarrier-in-RB over the whole carrier, split by
 * RB parity (density 0.5 lives on one parity). A resource that is transmitted into an otherwise
 * quiet symbol shows as a clean on/off split of those 12 values; the "on" subcarriers of each slot are
 * recorded per (density, symbol, subcarrier) cell, a cell whose hits are PERIODIC is resource energy,
 * cells sharing a periodicity form one measured footprint, and only (row, bitmap, l0, l1) whose OAI
 * footprint fits it are handed to the existing confirm path.
 *
 * WHY PER SLOT AND NOT THE ACCUMULATED EPR MEAN. The per-candidate mean over visits is diluted by
 * every slot that does not carry the resource (1/period of visits do), and for a wide row the other
 * REs of the symbol are mostly the resource itself: one RE of a 32-port pattern against the other 11
 * of its symbol reads at most 11/7 = 1.57 even in a perfectly quiet slot. Averaged, that is lost in
 * the noise; inside one slot it is an unmistakable 8-on / 4-off split.
 *
 * KNOWN BLIND SPOTS. Row 9 (12 ports) fills all 12 subcarriers of its symbol, so no intra-symbol
 * contrast exists: it is never matched. A symbol that also carries PDSCH shows no contrast either,
 * so only slots where the resource is sent into otherwise empty REs contribute evidence. */

#define NR_CSIRS_BLIND_NSYM 14

/** The REs @p c occupies in ONE RB (density ignored), from OAI's get_csi_mapping_parms(): per symbol
 *  a 12-bit subcarrier mask. Returns the distinct RE count, or -1 if @p c is unsafe or would place an
 *  RE outside the slot. Density 1 rows 6-18 return exactly their port count. */
int nr_csirs_blind_footprint(const nr_csirs_candidate_t *c, uint16_t sym_mask[NR_CSIRS_BLIND_NSYM]);

/** Per-slot evidence from one FFT'd symbol (read at (i + rx_shift) % n_fft like every comparator
 *  here): the subcarriers-in-RB whose mean power over the even (resp. odd) RBs stands above the rest
 *  by at least NR_CSIRS_BLIND_FP_GAP at the largest ratio gap of the sorted 12. 0 when flat. Pure. */
#define NR_CSIRS_BLIND_FP_GAP 2.0
void nr_csirs_blind_symbol_on(const int16_t *rx_re_im, int n_fft, int rx_shift, int n_rb,
                              uint16_t *on_even, uint16_t *on_odd);

/// Per-cell hit history, cell = (density 0 even / 1 odd / 2 one, symbol, subcarrier-in-RB).
typedef struct {
  uint32_t hit_slot[3][NR_CSIRS_BLIND_NSYM][12][8]; ///< ring of the LAST 8 hit slots
  uint8_t  n_hit[3][NR_CSIRS_BLIND_NSYM][12];
  uint8_t  w[3][NR_CSIRS_BLIND_NSYM][12];
} nr_csirs_blind_fp_t;

/** Record one visit's on-masks for @p symbol. A subcarrier lit on both parities is density one; on
 *  one parity only, density 0.5 on that parity. Returns true when a cell with enough hits to be
 *  tested for periodicity gained a hit (the caller's cue to re-run the match). */
bool nr_csirs_blind_fp_record(nr_csirs_blind_fp_t *fp, int symbol, uint16_t on_even, uint16_t on_odd,
                              uint32_t absolute_slot);

/** Candidates (rows 6-18) whose footprint fits a measured one. Periodic cells of one density are
 *  grouped by a jointly consistent periodicity; a candidate is kept when its footprint lies inside a
 *  group and is not strictly inside another kept candidate's (a real cell sends several resources in
 *  one slot, so the group may be a union -- exact equality would then find nothing). Candidates
 *  with the identical footprint (e.g. rows 16 and 17) are all returned: only the sequence stage can
 *  separate them. Returns the count written to @p out. */
int nr_csirs_blind_fp_match(const nr_csirs_blind_fp_t *fp, uint16_t n_rb, uint16_t scramb_id,
                            nr_csirs_candidate_t *out, int max);

/** Append @p c to the search population. Returns its index, or -1 if full or already present (same
 *  row, bitmap, symbols, density -- scramb_id ignored, since IDSWEEP patches it in place). */
int nr_csirs_blind_append(nr_csirs_blind_state_t *st, const nr_csirs_candidate_t *c);

/** The confirmed resource, or NULL. Fills period/offset when non-NULL. */
const nr_csirs_candidate_t *nr_csirs_blind_confirmed(const nr_csirs_blind_state_t *st,
                                                     uint16_t *period, uint16_t *offset);

#endif /* __NR_CSIRS_BLIND_SEARCH_H__ */
