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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c
 * \brief See nr_pdcch_dci_length_sweep.h for the full design rationale (the two false-accept
 * traps this sweep must reject).
 */
#include <stdlib.h>
#include "nr_pdcch_dci_length_sweep.h"

// Trials per length hypothesis. Enough that a real length (which should land the bootstrap RNTI
// or vary richly) separates clearly from a length producing only 1/256 chance passes.
#define TRIALS_PER_LENGTH 64
#define MAX_HASHES_TRACKED 32  // cap on distinct-payload bookkeeping per length; degenerate
                                // detection only needs to distinguish "1 distinct value" from
                                // "more than 1", so this never needs to be large.
// Absolute floor on passes before a length is even considered "maybe real" (absent a bootstrap
// hit). At 1/256 per-trial chance and TRIALS_PER_LENGTH=64, the EXPECTED chance-pass count is
// only ~0.25 -- scaling that expectation by a constant factor stays well under 1 and is not a
// usable gate (any single lucky pass would then "clear" it). 3 is a real absolute floor: per-length
// P(>=3 chance passes) is ~0.2% at this trial count, i.e. this project's own documented false-accept
// rate (42k/10.9M candidates =~ 1/256) does NOT survive a same-length repeat-and-persist requirement.
#define MIN_SIGNIFICANT_PASSES 3

int nr_pdcch_dci_length_sweep(nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                              int min_len, int max_len, uint16_t bootstrap_rnti)
{
  int best_len = -1;
  double best_score = 0.0;
  int trial_idx = 0;

  for (int len = min_len; len <= max_len; len++) {
    int      passes = 0;
    int      bootstrap_hits = 0;
    uint32_t hashes[MAX_HASHES_TRACKED];
    int      n_distinct = 0;

    for (int t = 0; t < TRIALS_PER_LENGTH; t++, trial_idx++) {
      uint16_t rnti = 0;
      uint32_t payload_hash = 0;
      if (!decode_one_candidate(len, trial_idx, &rnti, &payload_hash, user_ctx)) {
        continue;
      }
      passes++;
      if (bootstrap_rnti != 0 && rnti == bootstrap_rnti) {
        bootstrap_hits++;
      }
      // Track distinct payload hashes seen at this length, capped -- rejects the degenerate
      // fixed-point trap (n_distinct stays at 1 despite many passes).
      bool seen = false;
      for (int h = 0; h < n_distinct; h++) {
        if (hashes[h] == payload_hash) {
          seen = true;
          break;
        }
      }
      if (!seen && n_distinct < MAX_HASHES_TRACKED) {
        hashes[n_distinct++] = payload_hash;
      }
    }

    if (passes == 0) {
      continue;
    }
    // Degenerate fixed point: many passes, but every one decodes to the SAME payload. Reject
    // outright regardless of how many bootstrap hits it racked up (Technique C's own header
    // comment: trap #2).
    if (n_distinct <= 1 && passes >= 3) {
      continue;
    }
    // Significance: either it lands the KNOWN bootstrap RNTI at least once (matching a specific
    // 16-bit value by chance is ~1/65536 per trial -- far below anything this sweep needs to
    // distinguish), or -- with no bootstrap available -- it must clear the absolute pass-count
    // floor on its own (payload-variance-only fallback).
    if (bootstrap_hits == 0 && passes < MIN_SIGNIFICANT_PASSES) {
      continue;  // not clearly above chance, and nothing tying it to a known-real RNTI
    }
    // Score: bootstrap hits dominate (a real length landing the KNOWN rnti is near-certain
    // evidence); payload variance is the tiebreak/floor signal when no bootstrap is available.
    const double score = (double)bootstrap_hits * 100.0 + (double)n_distinct;
    if (score > best_score) {
      best_score = score;
      best_len   = len;
    }
  }
  return best_len;
}
