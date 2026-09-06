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

/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc
 * \brief Phase 3 Technique C (rewritten 2026-09-06): the ACCUMULATING dci_length sweep's SELECTION
 * logic, tested against a synthetic scorer stub so the test needs no real candidate stream.
 * Exercises all three traps this project has already paid for (see
 * nr_pdcch_dci_length_sweep.c's header): the classic 1-in-256 single-pass trap, the degenerate
 * polar fixed point, and the NEW one this rewrite introduces protection against -- a fixed
 * absolute pass floor becoming trivially clearable by chance once accumulated trials grow large.
 */
#include <cstdint>
#include <gtest/gtest.h>

extern "C" {
#include "nr_pdcch_dci_length_sweep.h"
}

namespace {
// Deterministic, reproducible "1-in-256 chance pass" -- same formula the original test used, now
// applied to a GLOBAL index (call count folded in) so repeated feed() calls don't replay the
// identical trial_idx pattern and silently fail to accumulate anything new.
bool chance_pass(uint32_t global_idx)
{
  return (global_idx * 2654435761u) % 256 == 0;
}
} // namespace

TEST(DciLengthSweep, PicksTheLengthWithVaryingPayloadsAndBootstrapHits)
{
  // Single occasion, generous trial count: length 47 varies and hits the bootstrap RNTI on 1/5
  // trials (near-certain evidence, doesn't need accumulation); length 39 is the degenerate trap
  // (see next test); everything else is pure chance noise.
  struct Fixture {
    static bool decode(int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out, void*)
    {
      if (dci_length == 47) {
        *rnti_out = (trial_idx % 5 == 0) ? 0x4601 : (uint16_t)(0x1000 + trial_idx);
        *payload_hash_out = 1000u + (uint32_t)trial_idx;
        return (trial_idx % 5 == 0) || chance_pass((uint32_t)trial_idx);
      }
      if (dci_length == 39) {
        *rnti_out = 0x4601;
        *payload_hash_out = 42u; // INVARIANT -- degenerate fixed point
        return (trial_idx % 3 == 0);
      }
      *rnti_out = (uint16_t)(0x2000 + trial_idx);
      *payload_hash_out = 5000u + (uint32_t)trial_idx;
      return chance_pass((uint32_t)(dci_length * 1000 + trial_idx));
    }
  };
  nr_pdcch_dci_length_sweep_state_t state;
  nr_pdcch_dci_length_sweep_reset(&state);
  const int best = nr_pdcch_dci_length_sweep_feed(&state, &Fixture::decode, nullptr,
                                                  /*n_trials_this_call=*/64, /*min_len=*/30,
                                                  /*max_len=*/70, /*bootstrap_rnti=*/0x4601);
  EXPECT_EQ(best, 47);
}

TEST(DciLengthSweep, RejectsAnInvariantPayloadDespiteMatchingTheBootstrapRnti)
{
  // Regression guard for the exact trap this project has already hit: length 39 matches the
  // bootstrap RNTI on every qualifying trial, which a naive "does it hit the known RNTI" scorer
  // would love -- but every trial decodes to the IDENTICAL payload_hash, the degenerate-fixed-point
  // signature this function must reject regardless of bootstrap hits.
  struct Fixture {
    static bool decode(int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out, void*)
    {
      if (dci_length == 39) {
        *rnti_out = 0x4601;
        *payload_hash_out = 42u;
        return (trial_idx % 3 == 0);
      }
      *rnti_out = (uint16_t)(0x2000 + trial_idx);
      *payload_hash_out = 5000u + (uint32_t)trial_idx;
      return chance_pass((uint32_t)(dci_length * 1000 + trial_idx));
    }
  };
  nr_pdcch_dci_length_sweep_state_t state;
  nr_pdcch_dci_length_sweep_reset(&state);
  const int best =
      nr_pdcch_dci_length_sweep_feed(&state, &Fixture::decode, nullptr, 64, 30, 70, 0x4601);
  EXPECT_NE(best, 39);
}

TEST(DciLengthSweep, ReturnsMinusOneWhenNoLengthClearsSignificanceYet)
{
  // A scorer where NOTHING is ever real -- every length is pure chance noise, and only a handful
  // of trials have accumulated so far. Must NOT pick a length yet.
  auto pure_noise = [](int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out,
                       void*) -> bool {
    *rnti_out = (uint16_t)(0x3000 + trial_idx);
    *payload_hash_out = 9000u + (uint32_t)trial_idx;
    return chance_pass((uint32_t)(dci_length * 1000 + trial_idx));
  };
  nr_pdcch_dci_length_sweep_state_t state;
  nr_pdcch_dci_length_sweep_reset(&state);
  const int best =
      nr_pdcch_dci_length_sweep_feed(&state, pure_noise, nullptr, 20, 30, 70, 0x9999 /* never seen */);
  EXPECT_EQ(best, -1);
}

TEST(DciLengthSweep, AccumulatesAcrossManyOccasionsToFindARealButSparseLength)
{
  // The scenario this rewrite exists for: length 47 is genuinely real but only decodes on ~8% of
  // its own trials (far below what one occasion's ~20 candidates could establish on their own, and
  // deliberately with NO bootstrap RNTI available -- pure variance-only accumulation), while every
  // other length is pure 1/256 chance noise. One occasion (20 trials) must NOT be enough; many
  // occasions accumulated must eventually converge on 47 and nothing else.
  struct Fixture {
    static bool decode(int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out,
                       void* ctx)
    {
      int* calls = static_cast<int*>(ctx);
      const uint32_t global_idx = (uint32_t)(*calls * 1000 + trial_idx);
      if (dci_length == 47) {
        *rnti_out = (uint16_t)(0x5000 + global_idx); // varies -- not a degenerate fixed point
        *payload_hash_out = global_idx;
        return (global_idx % 12 == 0); // ~8% real hit rate, no bootstrap tie-in
      }
      *rnti_out = (uint16_t)(0x2000 + global_idx);
      *payload_hash_out = 6000u + global_idx;
      return chance_pass((uint32_t)(dci_length * 100000 + global_idx));
    }
  };
  nr_pdcch_dci_length_sweep_state_t state;
  nr_pdcch_dci_length_sweep_reset(&state);
  int    calls = 0;
  int    best  = -1;
  for (int occasion = 0; occasion < 200 && best < 0; occasion++) {
    calls = occasion;
    best  = nr_pdcch_dci_length_sweep_feed(&state, &Fixture::decode, &calls, /*n_trials_this_call=*/20,
                                          30, 70, /*bootstrap_rnti=*/0 /* none available */);
  }
  EXPECT_EQ(best, 47);
  // And it must not have been reachable from a single occasion's worth of exposure alone.
  nr_pdcch_dci_length_sweep_state_t one_shot;
  nr_pdcch_dci_length_sweep_reset(&one_shot);
  int one_call = 0;
  EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&one_shot, &Fixture::decode, &one_call, 20, 30, 70, 0), -1);
}

TEST(DciLengthSweep, FixedFloorWouldFalseTriggerButScaledTestDoesNot)
{
  // The regression this rewrite specifically guards: at this project's own measured ~1/256
  // chance-pass rate, accumulating ~700+ trials per length gives an expected ~2.7 chance passes --
  // enough to have cleared the OLD fixed floor of 3 purely by chance for at least one of 41 length
  // hypotheses. Every length here is PURE chance noise, accumulated over enough occasions that a
  // fixed floor would very likely have false-triggered on one of them; the scaled test must not.
  auto pure_noise = [](int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out,
                       void* ctx) -> bool {
    int* calls = static_cast<int*>(ctx);
    const uint32_t global_idx = (uint32_t)(*calls * 1000 + trial_idx);
    *rnti_out = (uint16_t)(0x3000 + global_idx);
    *payload_hash_out = 9000u + global_idx;
    return chance_pass((uint32_t)(dci_length * 100000 + global_idx));
  };
  nr_pdcch_dci_length_sweep_state_t state;
  nr_pdcch_dci_length_sweep_reset(&state);
  int calls = 0;
  int best  = -1;
  for (int occasion = 0; occasion < 40; occasion++) { // 40 * 20 = 800 trials/length accumulated
    calls = occasion;
    const int r = nr_pdcch_dci_length_sweep_feed(&state, pure_noise, &calls, 20, 30, 70, 0);
    if (r >= 0) {
      best = r; // capture the first (should never happen) false positive, don't stop early
    }
  }
  EXPECT_EQ(best, -1);
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
