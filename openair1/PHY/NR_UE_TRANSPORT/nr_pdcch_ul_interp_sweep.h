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
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#ifndef NR_PDCCH_UL_INTERP_SWEEP_H
#define NR_PDCCH_UL_INTERP_SWEEP_H
#include "nr_hyp_sweep.h"
#include "nr_pdcch_blind_monitor.h"
typedef struct {
  uint8_t tda_start, tda_length, tda_mapping, tda_k2;
  uint8_t dmrs_config_type, dmrs_add_pos, dmrs_max_length, transform_precoding, mcs_table;
} nr_pdcch_ul_interp_hyp_t;
/* TS 38.214 Table 6.1.2.1-1, normal CP: mapping type A (0) S = 0, L 4..14; type B (1) S 0..13,
 * L 1..14, S+L <= 14. Any other mapping_type is not legal. */
bool nr_pusch_tda_legal(int mapping_type, int S, int L);
/* Initial candidate catalogue from the design; not exhaustive NR configuration recovery. Every TDA row
 * (both mapping types) satisfies nr_pusch_tda_legal() -- enforced by the unit test.
 * Unsupported receiver modes remain unresolved hypotheses, never scored as CRC failures. */
int nr_pdcch_ul_interp_sweep_generate(nr_hyp_t *, int);

/* ---- Gap item 2: PUSCH TDRA type B, full (S,L) space once the DM-RS energy oracle has pinned one --
 * see nr_pusch_ul_energy_span()/nr_pusch_ul_dmrs_pin_set() below. The curated catalogue above tries
 * only (0,14),(0,7) type A and (2,12),(0,4) type B because the FULL legal set (11 type A + 105 type
 * B) x k2{1..4} x the 96 field combinations overflows NR_HYP_SWEEP_MAX_RAW with no oracle to prune
 * it first; once (S,L,mapping) is pinned that product collapses to k2{1..4} x 96 = 384, so the WHOLE
 * type-B (S,L) plane becomes reachable. ---- */
/** mapping_type value for nr_pdcch_ul_interp_sweep_generate_pinned()/the pin: S=0 does NOT
 *  determine mapping type by itself -- type A requires S=0 AND L>=4, but type B is ALSO legal at
 *  S=0 for any L with S+L<=14 (TS 38.214 Table 6.1.2.1-1), so an (S,L) with S=0 and L>=4 is legal
 *  under EITHER mapping type and energy occupancy alone cannot tell them apart (both durations
 *  produce the identical observed span). Review-caught: an earlier version of this file guessed
 *  type A whenever S==0 && L>=4 and latched that guess forever, silently excluding every legal
 *  type-B row at that same (S,L) -- e.g. (S=0,L=14) is a real, common type-B row. */
#define NR_PUSCH_MAPPING_EITHER (-1)
/** Every field combination at ONE pinned (S, L), k2 in {1..4}, for every mapping_type that
 *  satisfies nr_pusch_tda_legal(mapping_type, S, L): mapping_type may be 0 (type A only), 1 (type B
 *  only) or NR_PUSCH_MAPPING_EITHER (try whichever of the two nr_pusch_tda_legal() actually admits
 *  -- both, when the (S,L) span is ambiguous per the note above, so a guess is never latched as a
 *  false certainty). Returns NR_HYP_SWEEP_INVALID if no mapping type admits (S,L) at all. */
int nr_pdcch_ul_interp_sweep_generate_pinned(nr_hyp_t *out, int cap, int S, int L, int mapping_type);

/** Occupied-symbol span (S = first, L = length) from a per-symbol RX energy profile covering one
 *  slot (index 0..13). The DM-RS ENERGY oracle itself -- there is no DM-RS-SEQUENCE oracle here
 *  (unlike the DL side's nr_dmrs_prb_coherence()): dmrs-Type/maxLength/AdditionalPosition are
 *  exactly what this receiver does not know yet, so it cannot correlate against a specific
 *  sequence, only detect which symbols carry the grant's signal at all. A symbol counts as occupied
 *  when its energy clears `rel_thresh` of the profile's own peak (the same physical justification
 *  dmrs_oracle_measure() uses in nr_pdsch_passive_queue.c: an unallocated symbol reads near-zero, an
 *  allocated one is within a few dB of the strongest). False if nothing clears the threshold (an
 *  all-zero / noise-only profile, or a bad threshold). */
bool nr_pusch_ul_energy_span(const double energy[14], double rel_thresh, int *S, int *L);

/** One cell-wide pin (this file already assumes a single UL BWP/TDRA table -- same simplification
 *  as tda_count/tda_start elsewhere in this module). Set once a caller has established (S,L) with
 *  confidence and never overwritten after that -- "the oracle prunes on the first observation", the
 *  same rule the DL DM-RS oracle already uses (see nr_pdsch_passive_queue.c's dmrs_oracle_measure()
 *  callers). mapping_type is derived by the setter from (S,L) via nr_pusch_tda_legal() against BOTH
 *  mapping types, not guessed from S alone -- see NR_PUSCH_MAPPING_EITHER above -- so it can never
 *  disagree with, or be more certain than, the (S,L) it was pinned from.
 *  Thread-safe: multiple UL consumer threads may call this concurrently (claimed with a single CAS
 *  on the validity flag before any field is written; a losing caller's own (S,L) is simply
 *  discarded, which is correct under "first observation wins"). */
void nr_pusch_ul_dmrs_pin_set(int S, int L);
/** True and fills S/L/mapping_type (any pointer may be NULL) iff a pin exists. Thread-safe. */
bool nr_pusch_ul_dmrs_pin_get(int *S, int *L, int *mapping_type);
/** Test/reset hook: clears the pin (as if nothing had ever been measured). NOT thread-safe against
 *  a concurrent set()/get() -- call only from a single-threaded test harness between cases. */
void nr_pusch_ul_dmrs_pin_reset(void);

/* Apply only to the actually observed TDA index, never silently to entry zero. */
bool nr_pdcch_ul_interp_sweep_apply(const nr_hyp_t *, int tda_index, nr_pdcch_blind_ul_opts_t *);
#endif
