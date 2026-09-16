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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h
 * \brief Phase 3 Technique C: lock dci_length by histogram, ACCUMULATED ACROSS OCCASIONS (rewritten
 * 2026-09-06 -- see nr_pdcch_dci_length_sweep.c's header for why the original one-shot,
 * single-occasion design could not work on live air). DCI 1_1 lengths land in roughly a 30-70 bit
 * range (roadmap artifact), so a linear sweep is ~40 hypotheses.
 *
 * THREE TRAPS ALREADY PAID FOR IN THIS PROJECT, all of which this module's scoring must reject
 * (see PHASE1_CSS0_AUTOCONF_HANDOVER.md, TOTAL_PASSIVE_UE_HANDOVER.md and
 * PHASE3_DEDICATED_CONFIG_RECOVERY_HANDOVER.md for the history):
 *  1. `upper 8 bits == 0` alone is a 1-in-256 test -- produced 42k chance passes on 10.9M
 *     candidates. Never score a length by pass-count alone.
 *  2. A spiked histogram at some length is NOT sufficient if the decoded payload is INVARIANT
 *     across instances -- that is a degenerate polar fixed point, not traffic (3744 byte-identical
 *     "decodes" were once mistaken for a working length).
 *  3. (NEW, 2026-09-06) A FIXED absolute pass-count floor only holds while total trials per length
 *     stays small (~64, the original one-shot design's whole budget). Accumulated over many
 *     occasions to get enough REAL exposure, total trials per length can reach the hundreds or
 *     thousands, at which point a fixed floor is trivially cleared by CHANCE ALONE (at this
 *     project's own measured ~1/256 chance-pass rate, ~700 trials already has an expected ~2.7
 *     chance passes). The significance test must scale with accumulated trial count.
 *
 * This module owns only the SWEEP + SCORING logic; it does not decode anything itself and does not
 * duplicate the RT monitor's own candidate-iteration loop. It takes a caller-supplied
 * nr_pdcch_dci_length_scorer_fn so it stays testable in isolation (see
 * tests/nr_pdcch_dci_length_sweep_test.cc, which drives it with a synthetic stub) and decoupled
 * from the real candidate stream.
 */
#ifndef NR_PDCCH_DCI_LENGTH_SWEEP_H
#define NR_PDCCH_DCI_LENGTH_SWEEP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attempt one candidate decode at the given dci_length.
 *
 * @param dci_length       Hypothesis under test.
 * @param trial_idx        CALL-LOCAL trial counter (0..n_trials_this_call-1) -- accumulation
 *                          across calls is the sweep state's job, not this callback's.
 * @param rnti_out         CRC-recovered RNTI, filled only if the caller returns true.
 * @param payload_hash_out A cheap hash/fingerprint of the decoded payload bits, filled only if the
 *                          caller returns true -- used to detect an invariant (degenerate) payload.
 * @param user_ctx          Opaque, passed through unchanged.
 * @return true iff this trial's CRC-adjacent check passed (e.g. upper 8 bits zero) -- the SWEEP,
 *         not this callback, decides whether the length is real.
 */
typedef bool (*nr_pdcch_dci_length_scorer_fn)(int dci_length, int trial_idx, uint16_t* rnti_out,
                                              uint32_t* payload_hash_out, void* user_ctx);

#define NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN    64 // dci_length is capped at 63 elsewhere in this
                                                 // project (values 0-63 fit); indexed directly by
                                                 // length, no offset arithmetic to get wrong.
#define NR_PDCCH_DCI_LENGTH_SWEEP_MAX_HASHES 32 // cap on distinct-payload bookkeeping per length;
                                                 // degenerate detection only needs to distinguish
                                                 // "1 distinct value" from "more than 1".

/** Persistent state, accumulated across many nr_pdcch_dci_length_sweep_feed() calls (one call per
 *  candidate-bearing occasion). Plain struct, no hidden allocation -- zero-initialize (static
 *  storage, or nr_pdcch_dci_length_sweep_reset()) before the first feed. */
typedef struct {
  int      trials[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];         // total attempts at this length
  int      passes[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];         // of which, CRC-adjacent passes
  int      bootstrap_hits[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN]; // of which, hit the known RNTI
  uint32_t hashes[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN][NR_PDCCH_DCI_LENGTH_SWEEP_MAX_HASHES];
  int      n_distinct[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
  int      occasions_fed;
  /* A length known to belong to ANOTHER format pair on the same search space (the derived 1_0/0_0
   * size): its CRC passes are real but say nothing about the 1_1/0_1 size the sweep is after. The
   * SA rfsim cell locked 44 = its 1_0 size on 58 format-1_0 accepts (2026-09-16). 0 = none. */
  int      excluded_len;
} nr_pdcch_dci_length_sweep_state_t;

/* A caller-serialized bank. Interleaved UEs never reset one another; geometry
 * epoch changes invalidate all entries. Eviction discards evidence, never reuses it. */
#define NR_PDCCH_LENGTH_CONTEXTS 16
typedef struct {
  nr_pdcch_dci_length_sweep_state_t state;
  uint16_t rnti;
  int found;
  bool exhausted;
  uint64_t touched;
} nr_pdcch_dci_length_context_t;
typedef struct {
  nr_pdcch_dci_length_context_t ue[NR_PDCCH_LENGTH_CONTEXTS];
  uint64_t epoch, clock;
} nr_pdcch_dci_length_bank_t;
nr_pdcch_dci_length_context_t *nr_pdcch_dci_length_context(
    nr_pdcch_dci_length_bank_t *bank, uint64_t epoch, uint16_t rnti);

void nr_pdcch_dci_length_sweep_reset(nr_pdcch_dci_length_sweep_state_t* state);

/**
 * @brief Feed ONE occasion's worth of real candidates into the ongoing sweep and re-check
 *        significance against the ACCUMULATED history (see this header's trap #3 and
 *        nr_pdcch_dci_length_sweep.c's header for why one occasion's candidate volume alone is not
 *        enough). Call this once per candidate-bearing occasion, not once ever.
 *
 * @param state               Persistent state; reset before the first call.
 * @param decode_one_candidate/user_ctx  Per-trial callback, same contract as before.
 * @param n_trials_this_call  This occasion's own candidate count -- every length in
 *                            [min_len,max_len] is tried against all of them.
 * @param min_len/max_len     Hypothesis range.
 * @param bootstrap_rnti      0 = none yet; a hit is near-certain evidence regardless of
 *                            accumulated sample size and short-circuits the chance-floor test.
 * @return the winning dci_length once one clears (sample-size-scaled) significance, or -1 to keep
 *         observing. The caller decides when to give up (a bounded occasion cap of its own).
 */
int nr_pdcch_dci_length_sweep_feed(nr_pdcch_dci_length_sweep_state_t* state,
                                   nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                                   int n_trials_this_call, int min_len, int max_len,
                                   uint16_t bootstrap_rnti);

#ifdef __cplusplus
}
#endif

#endif
