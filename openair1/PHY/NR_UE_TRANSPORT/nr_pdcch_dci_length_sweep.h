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
 * \brief Phase 3 Technique C: lock dci_length by histogram, once at startup. DCI 1_1 lengths land
 * in roughly a 30-70 bit range (roadmap artifact), so a linear sweep is ~40 passes.
 *
 * TWO TRAPS ALREADY PAID FOR IN THIS PROJECT, both of which this module's scoring must reject
 * (see PHASE1_CSS0_AUTOCONF_HANDOVER.md and TOTAL_PASSIVE_UE_HANDOVER.md for the history):
 *  1. `upper 8 bits == 0` alone is a 1-in-256 test -- produced 42k chance passes on 10.9M
 *     candidates. Never score a length by pass-count alone.
 *  2. A spiked histogram at some length is NOT sufficient if the decoded payload is INVARIANT
 *     across instances -- that is a degenerate polar fixed point, not traffic (3744 byte-identical
 *     "decodes" were once mistaken for a working length).
 *
 * This module owns only the SWEEP + SCORING logic; it does not decode anything itself and does not
 * duplicate the RT monitor's own candidate-iteration loop. It takes a caller-supplied
 * nr_pdcch_dci_length_scorer_fn so it stays testable in isolation (see
 * tests/nr_pdcch_dci_length_sweep_test.cc, which drives it with a synthetic stub) and decoupled
 * from the real candidate stream -- a later task wires in the real decode-and-extract call
 * (nr_pdcch_blind_decode_and_extract_ex()) and the Task 1 bootstrap-RNTI check
 * (nr_pdcch_blind_monitor_confirmed_rnti()) inside its own decode_one_candidate implementation.
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
 * @param trial_idx        Monotonically increasing trial counter, unique across the WHOLE sweep
 *                          (not per-length) -- lets a deterministic test fixture vary its
 *                          synthetic output without depending on real candidate timing.
 * @param rnti_out         CRC-recovered RNTI, filled only if the caller returns true.
 * @param payload_hash_out A cheap hash/fingerprint of the decoded payload bits, filled only if the
 *                          caller returns true -- used to detect an invariant (degenerate) payload.
 * @param user_ctx          Opaque, passed through unchanged.
 * @return true iff this trial's CRC-adjacent check passed (e.g. upper 8 bits zero) -- the SWEEP,
 *         not this callback, decides whether the length is real.
 */
typedef bool (*nr_pdcch_dci_length_scorer_fn)(int dci_length, int trial_idx, uint16_t* rnti_out,
                                              uint32_t* payload_hash_out, void* user_ctx);

/**
 * @brief Sweep [min_len, max_len], TRIALS_PER_LENGTH candidates per hypothesis, and return the
 *        length whose population is BOTH statistically significant above chance AND shows a
 *        varying payload (rejects the degenerate-fixed-point trap) -- optionally weighted toward
 *        hitting bootstrap_rnti when one is available (0 = none, score on variance alone).
 *
 * @return the winning dci_length, or -1 if nothing in range clears significance.
 */
int nr_pdcch_dci_length_sweep(nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                              int min_len, int max_len, uint16_t bootstrap_rnti);

#ifdef __cplusplus
}
#endif

#endif
