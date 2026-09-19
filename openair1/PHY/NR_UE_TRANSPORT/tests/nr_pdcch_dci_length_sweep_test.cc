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
#include <cstring>
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

TEST(DciLengthSweep, RejectsTheMeasuredSingleOccasionFalseLock)
{
  // MEASURED 2026-09-19, Salt macro: length 42 locked from ONE occasion (~56 trials) with no
  // bootstrap RNTI. A wrong length decodes a REAL DCI more often than noise does, so the assumed
  // 1/256 chance rate under-models it; the null is now measured from the length population and a
  // statistical lock needs MIN_TRIALS_FOR_STATISTICAL_LOCK trials. Verified against the OLD code,
  // which locks 42 here.
  auto structured = [](int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out,
                       void*) -> bool {
    *rnti_out = (uint16_t)(0x1000 + trial_idx + dci_length);
    *payload_hash_out = 7000u + (uint32_t)(trial_idx * 31 + dci_length);
    return dci_length == 42 && (trial_idx % 11) == 0; // 6 of 56
  };
  nr_pdcch_dci_length_sweep_state_t state;
  nr_pdcch_dci_length_sweep_reset(&state);
  EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&state, structured, nullptr, 56, 30, 63, 0), -1);
}

TEST(DciLengthSweep, ASingleBootstrapHitIsNotEnough)
{
  // A chance match on a known 16-bit value is 2^-24 per trial, but a CORESET walk runs ~29k
  // length-hypotheses x ~28k trials, so single hits are expected several times per walk.
  auto one_hit = [](int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out,
                    void*) -> bool {
    *rnti_out = (trial_idx == 0 && dci_length == 55) ? 0x4601 : (uint16_t)(0x2000 + trial_idx);
    *payload_hash_out = 5000u + (uint32_t)(trial_idx + dci_length);
    return (trial_idx == 0 && dci_length == 55);
  };
  nr_pdcch_dci_length_sweep_state_t state;
  nr_pdcch_dci_length_sweep_reset(&state);
  EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&state, one_hit, nullptr, 56, 30, 63, 0x4601), -1);
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

static bool independent_length(int len,int trial,uint16_t *rnti,uint32_t *hash,void *ctx) {
  if(len!=*static_cast<int*>(ctx)) return false;
  *rnti=0x1234; *hash=trial; return true;
}
TEST(NrPdcchDciLengthSweep, IndependentStatesAndReset) {
  nr_pdcch_dci_length_sweep_state_t dl{},ul{};
  int dl_length=47,ul_length=43;
  nr_pdcch_dci_length_sweep_feed(&dl,independent_length,&dl_length,20,30,63,0x1234);
  EXPECT_EQ(ul.occasions_fed,0);
  EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&ul,independent_length,&ul_length,20,30,63,0x1234),43);
  const auto saved=dl;
  nr_pdcch_dci_length_sweep_reset(&ul);
  EXPECT_EQ(ul.occasions_fed,0);
  EXPECT_EQ(dl.occasions_fed,saved.occasions_fed);
  EXPECT_EQ(dl.passes[47],saved.passes[47]);
  EXPECT_EQ(dl.passes[43],0);
}

TEST(DciLengthBank, InterleavedUesKeepDifferentLengthsAndBudgets) {
  nr_pdcch_dci_length_bank_t bank{};
  for(int occasion=0;occasion<20;++occasion)
    for(int u=0;u<3;++u) {
      auto *c=nr_pdcch_dci_length_context(&bank,101,0x3001+u);
      ASSERT_NE(c,nullptr);
      EXPECT_EQ(c->state.occasions_fed,occasion);
      ++c->state.occasions_fed;
      c->found=41+2*u;
    }
  for(int u=0;u<3;++u) {
    auto *c=nr_pdcch_dci_length_context(&bank,101,0x3001+u);
    EXPECT_EQ(c->found,41+2*u);
    EXPECT_EQ(c->state.occasions_fed,20);
  }
  auto *fresh=nr_pdcch_dci_length_context(&bank,102,0x3001);
  EXPECT_EQ(fresh->found,0);
  EXPECT_EQ(fresh->state.occasions_fed,0);
  EXPECT_EQ(nr_pdcch_dci_length_context(&bank,102,0x3002)->found,0);
}
TEST(DciLengthBank, EvictionAndInvalidKeysDoNotInventEvidence) {
  nr_pdcch_dci_length_bank_t bank{};
  for(int u=1;u<=NR_PDCCH_LENGTH_CONTEXTS+1;++u)
    nr_pdcch_dci_length_context(&bank,8,u)->found=40+u;
  EXPECT_EQ(nr_pdcch_dci_length_context(&bank,8,2)->found,42);
  EXPECT_EQ(nr_pdcch_dci_length_context(&bank,8,1)->found,0);
  EXPECT_EQ(nr_pdcch_dci_length_context(nullptr,8,1),nullptr);
  EXPECT_EQ(nr_pdcch_dci_length_context(&bank,8,0),nullptr);
}

// ---- Rotation (2026-09-17) -------------------------------------------------------------------
// The sweep's cost was the receiver's dominant per-occasion expense (34 lengths x every candidate,
// Polar+CRC each, ~1.75ms against a ~667us occasion interval -> 68.5% of occasions dropped). The
// `stride` field spreads the lengths over successive calls. These two tests pin the properties that
// make that safe: every length is still visited equally often, and a ROUND -- not a call -- is what
// the caller's give-up budget counts, so no length loses trials.
namespace {
struct RotProbe {
  static int visits[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
  static bool decode(int dci_length, int, uint16_t *rnti_out, uint32_t *hash_out, void *)
  {
    visits[dci_length]++;
    *rnti_out = 0;
    *hash_out = 0;
    return false; // never passes: this test is about WHICH lengths get tried, not about scoring
  }
};
int RotProbe::visits[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
} // namespace

TEST(DciLengthSweepRotation, EveryLengthIsVisitedOncePerRoundAndNoneIsStarved)
{
  for (int stride : {1, 8, 34}) {
    memset(RotProbe::visits, 0, sizeof(RotProbe::visits));
    nr_pdcch_dci_length_sweep_state_t st{};
    st.stride = stride;
    // Three full rounds: stride calls per round.
    for (int call = 0; call < 3 * stride; call++) {
      nr_pdcch_dci_length_sweep_feed(&st, RotProbe::decode, nullptr, /*n_trials=*/2, 30, 63, 0);
    }
    for (int len = 30; len <= 63; len++) {
      EXPECT_EQ(RotProbe::visits[len], 3 * 2) << "stride " << stride << ", length " << len;
      EXPECT_EQ(st.trials[len], 3 * 2) << "stride " << stride << ", length " << len;
    }
    // A round is stride calls, and only a completed round counts against the give-up budget.
    EXPECT_EQ(st.occasions_fed, 3) << "stride " << stride;
    EXPECT_EQ(st.rot_phase, 0) << "stride " << stride;
  }
}

TEST(DciLengthSweepRotation, PerLengthTrialBudgetIsIdenticalToTheUnrotatedSweep)
{
  // The invariant the give-up cap depends on: at the moment occasions_fed hits the caller's cap,
  // every length has accumulated exactly as many trials as it would have without rotation.
  constexpr int kCap = 25; // stands in for AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS
  int trials_at_cap[2][NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN] = {};
  int calls[2] = {0, 0};
  const int strides[2] = {1, 8};
  for (int arm = 0; arm < 2; arm++) {
    nr_pdcch_dci_length_sweep_state_t st{};
    st.stride = strides[arm];
    while (st.occasions_fed < kCap) {
      nr_pdcch_dci_length_sweep_feed(&st, RotProbe::decode, nullptr, /*n_trials=*/3, 30, 63, 0);
      calls[arm]++;
    }
    memcpy(trials_at_cap[arm], st.trials, sizeof(st.trials));
  }
  for (int len = 30; len <= 63; len++) {
    EXPECT_EQ(trials_at_cap[0][len], trials_at_cap[1][len]) << "length " << len;
    EXPECT_EQ(trials_at_cap[1][len], kCap * 3) << "length " << len;
  }
  // Same budget, spread over 8x the calls -- that is the whole point: 8x less work per occasion.
  EXPECT_EQ(calls[0], kCap);
  EXPECT_EQ(calls[1], kCap * 8);
}
