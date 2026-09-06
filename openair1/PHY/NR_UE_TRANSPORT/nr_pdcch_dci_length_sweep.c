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
 * \brief See nr_pdcch_dci_length_sweep.h for the full design rationale.
 *
 * REWRITTEN 2026-09-06. The original one-shot design (all TRIALS_PER_LENGTH=64 trials drawn from a
 * SINGLE occasion's candidates, decided once, never retried) could not work on live air: this
 * cell's real accept rate is ~2% of occasions (measured on the proven-working manual-conf path,
 * 2801/138000), so a single occasion's ~20-60 candidates essentially never contains the >=3 real
 * decodable candidates the significance test needed. Live-measured consequence: across three 200s
 * captures with confirmed continuous real DL traffic (Technique A's own correlation confirms this),
 * the sweep found "nothing significant" at every length in [30,63], including 47 (this deployment's
 * own known-correct value, confirmed via the proven-working manual conf).
 *
 * Fixed by making the sweep STATEFUL and accumulating across many occasions -- feed() is called
 * once per candidate-bearing occasion instead of once ever. This introduces a new trap (this
 * header's #3): a FIXED absolute pass floor, calibrated for ~64 total trials, is trivially
 * clearable by chance alone once accumulated trials per length reach the hundreds. Fixed by scoring
 * against a floor that scales with accumulated trial count (a z-score above the expected chance-
 * pass rate), not a constant.
 */
#include "nr_pdcch_dci_length_sweep.h"
#include <string.h>
#include <math.h>

// Empirically measured false-accept rate of nr_pdcch_blind_decode_and_extract_ex()'s "plausible"
// gate on this project's own prior data (42k/10.9M candidates -- see this file's header and
// TOTAL_PASSIVE_UE_HANDOVER.md). NOT a theoretical CRC-24 rate: "plausible" is a field-sanity
// heuristic weaker than a genuine CRC match, which is exactly why a significance test is needed at
// all rather than trusting any single pass.
#define CHANCE_PASS_RATE (1.0 / 256.0)

// One-sided significance margin, in chance-rate standard deviations, required before ACCUMULATED
// passes at some length are trusted. With up to 34 simultaneous length hypotheses tested every
// call (this project's own [30,63] range), Bonferroni-correcting for that many simultaneous tests
// still leaves 34 * P(Z > 6) far below any usable false-positive budget -- and because this scales
// with sqrt(trials) rather than being a constant, it stays conservative however long the
// observation window runs (unlike the fixed floor it replaces -- see this file's header).
#define Z_SIGMA 6.0

void nr_pdcch_dci_length_sweep_reset(nr_pdcch_dci_length_sweep_state_t* state)
{
  memset(state, 0, sizeof(*state));
}

// Returns true iff h is new (and records it, space permitting) -- false if already seen.
static bool add_distinct_hash(uint32_t* hashes, int* n_distinct, uint32_t h)
{
  for (int i = 0; i < *n_distinct; i++) {
    if (hashes[i] == h) {
      return false;
    }
  }
  if (*n_distinct < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_HASHES) {
    hashes[(*n_distinct)++] = h;
  }
  return true;
}

// Does this length's ACCUMULATED (trials, passes) clear the chance floor by Z_SIGMA standard
// deviations? Scales with trials -- see this file's header comment on why a fixed floor cannot be
// reused here.
static bool clears_chance_floor(int trials, int passes)
{
  if (trials <= 0) {
    return false;
  }
  const double mean = (double)trials * CHANCE_PASS_RATE;
  const double var  = (double)trials * CHANCE_PASS_RATE * (1.0 - CHANCE_PASS_RATE);
  const double sd   = sqrt(var);
  return (double)passes >= mean + Z_SIGMA * sd + 1.0; // +1: never accept on a razor-thin margin
}

int nr_pdcch_dci_length_sweep_feed(nr_pdcch_dci_length_sweep_state_t* state,
                                   nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                                   int n_trials_this_call, int min_len, int max_len,
                                   uint16_t bootstrap_rnti)
{
  if (state == NULL || decode_one_candidate == NULL || n_trials_this_call <= 0) {
    return -1;
  }
  for (int len = min_len; len <= max_len && len < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN; len++) {
    for (int t = 0; t < n_trials_this_call; t++) {
      uint16_t rnti = 0;
      uint32_t payload_hash = 0;
      state->trials[len]++;
      if (!decode_one_candidate(len, t, &rnti, &payload_hash, user_ctx)) {
        continue;
      }
      state->passes[len]++;
      if (bootstrap_rnti != 0 && rnti == bootstrap_rnti) {
        state->bootstrap_hits[len]++;
      }
      add_distinct_hash(state->hashes[len], &state->n_distinct[len], payload_hash);
    }
  }
  state->occasions_fed++;

  int    best_len   = -1;
  double best_score = 0.0;
  for (int len = min_len; len <= max_len && len < NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN; len++) {
    if (state->passes[len] == 0) {
      continue;
    }
    // Degenerate fixed point: repeated passes, but every one decodes to the SAME payload. Reject
    // outright regardless of bootstrap hits (this file's header: trap #2).
    if (state->n_distinct[len] <= 1 && state->passes[len] >= 3) {
      continue;
    }
    // Significance: either it has landed the KNOWN bootstrap RNTI at least once (matching a
    // specific 16-bit value by chance is ~1/65536 per trial -- far below anything needed here,
    // and unaffected by accumulated sample size), or -- with no bootstrap available, or not yet on
    // this length -- its accumulated passes must clear the sample-size-scaled chance floor.
    const bool significant = (state->bootstrap_hits[len] > 0)
                           || clears_chance_floor(state->trials[len], state->passes[len]);
    if (!significant) {
      continue;
    }
    const double score = (double)state->bootstrap_hits[len] * 100.0 + (double)state->n_distinct[len];
    if (score > best_score) {
      best_score = score;
      best_len   = len;
    }
  }
  return best_len;
}
