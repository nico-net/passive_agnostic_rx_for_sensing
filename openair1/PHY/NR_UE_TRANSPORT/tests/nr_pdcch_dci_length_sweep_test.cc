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
#include <cstdlib>
#include <cstdio>
#include <gtest/gtest.h>

extern "C" {
#include "nr_pdcch_dci_length_sweep.h"
#include "common/config/config_userapi.h"
}

// Standalone LOG/CONFIG_LIB linkage, matching the other PHY decoder tests.
extern "C" {
configmodule_interface_t *uniqCfg=nullptr;
void exit_function(const char *file,const char *fn,int line,const char *message,int fatal) {
  if(message) fprintf(stderr,"%s:%d %s: %s\n",file,line,fn,message);
  if(fatal) abort();
  exit(EXIT_SUCCESS);
}
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

TEST(DciLengthBank, LockedToSuspectAfterNMisses) {
  unsetenv("ISAC_RECONF_N_SUSPECT");
  EXPECT_EQ(nr_pdcch_dci_length_n_suspect_from_env(),200u);
  nr_pdcch_dci_length_context_t c{};
  c.rnti=0x1234;
  EXPECT_EQ(nr_pdcch_dci_length_context_lock(&c,47),0);
  EXPECT_EQ(c.len_state,NR_LEN_LOCKED);
  for(unsigned i=0;i<199;++i)
    nr_pdcch_dci_length_context_note_occasion(&c,false,true,0);
  EXPECT_EQ(c.len_state,NR_LEN_LOCKED);
  nr_pdcch_dci_length_context_note_occasion(&c,false,true,0);
  EXPECT_EQ(c.len_state,NR_LEN_SUSPECT);
  EXPECT_EQ(c.found,47);
  EXPECT_EQ(c.state.preferred_len,47);
  setenv("ISAC_RECONF_N_SUSPECT","3",1);
  EXPECT_EQ(nr_pdcch_dci_length_n_suspect_from_env(),3u);
  unsetenv("ISAC_RECONF_N_SUSPECT");
}

TEST(DciLengthBank, InactiveRntiNeverSuspect) {
  nr_pdcch_dci_length_context_t c{};
  nr_pdcch_dci_length_context_lock(&c,47);
  for(unsigned i=0;i<400;++i)
    nr_pdcch_dci_length_context_note_occasion(&c,false,false,200);
  EXPECT_EQ(c.len_state,NR_LEN_LOCKED);
  EXPECT_EQ(c.miss_occasions,400u);
}

TEST(DciLengthBank, SuspectWithoutCoreset0Evidence) {
  nr_pdcch_dci_length_context_t c{};
  nr_pdcch_dci_length_context_lock(&c,47);
  for(unsigned i=0;i<3;++i)
    nr_pdcch_dci_length_context_note_occasion(&c,false,true,3);
  EXPECT_EQ(c.len_state,NR_LEN_SUSPECT);
}

TEST(DciLengthBank, RelockSameLengthReturnsLocked) {
  nr_pdcch_dci_length_context_t c{};
  nr_pdcch_dci_length_context_lock(&c,47);
  nr_pdcch_dci_length_context_note_occasion(&c,false,true,1);
  int actual=47;
  auto score=[](int len,int trial,uint16_t *rnti,uint32_t *hash,void *ctx)->bool {
    if(len!=*static_cast<int*>(ctx)) return false;
    *rnti=0x1234; *hash=static_cast<uint32_t>(trial+1); return true;
  };
  const int winner=nr_pdcch_dci_length_sweep_feed(&c.state,score,&actual,20,30,140,0x1234);
  ASSERT_EQ(winner,47);
  EXPECT_EQ(nr_pdcch_dci_length_context_lock(&c,winner),47);
  EXPECT_EQ(c.len_state,NR_LEN_LOCKED);
  EXPECT_EQ(c.miss_occasions,0u);
}

TEST(DciLengthBank, RelockDifferentLengthReplaces) {
  nr_pdcch_dci_length_context_t c{};
  c.rnti=0x1234;
  nr_pdcch_dci_length_context_lock(&c,47);
  nr_pdcch_dci_length_context_note_occasion(&c,false,true,1);
  int actual=52;
  auto score=[](int len,int trial,uint16_t *rnti,uint32_t *hash,void *ctx)->bool {
    if(len!=*static_cast<int*>(ctx)) return false;
    *rnti=0x1234; *hash=static_cast<uint32_t>(trial+1); return true;
  };
  int winner=-1;
  for(int occasion=0;occasion<20 && winner<0;++occasion)
    winner=nr_pdcch_dci_length_sweep_feed(&c.state,score,&actual,20,30,140,0x1234);
  ASSERT_EQ(winner,52);
  EXPECT_EQ(nr_pdcch_dci_length_context_lock(&c,winner),47);
  EXPECT_EQ(c.found,52);
  EXPECT_EQ(c.len_state,NR_LEN_LOCKED);
}

TEST(DciLengthBank, RelockOrderOldFirst) {
  nr_pdcch_dci_length_context_t c{};
  nr_pdcch_dci_length_context_lock(&c,47);
  nr_pdcch_dci_length_context_note_occasion(&c,false,true,1);
  const int seen[]={52,41,47,52};
  int order[NR_DCI_MAX_PAYLOAD]{};
  const int n=nr_pdcch_dci_length_context_relock_order(&c,seen,4,order,NR_DCI_MAX_PAYLOAD);
  ASSERT_EQ(n,NR_DCI_MAX_PAYLOAD-29);
  EXPECT_EQ(order[0],47);
  EXPECT_EQ(order[1],52);
  EXPECT_EQ(order[2],41);
  EXPECT_EQ(order[3],30);
}
TEST(DciLengthBank, FirstConvergenceSeedsExistingAndNewPeersWithoutPublishing) {
  nr_pdcch_dci_length_bank_t bank{};
  auto *first=nr_pdcch_dci_length_context(&bank,8,0x1001);
  auto *existing=nr_pdcch_dci_length_context(&bank,8,0x1002);
  ASSERT_NE(first,nullptr);
  ASSERT_NE(existing,nullptr);
  existing->state.resume_len=44;
  existing->state.resume_trial=3;
  nr_pdcch_dci_length_bank_converged(&bank,0x1001,39);
  EXPECT_EQ(bank.cell_len,0);
  EXPECT_EQ(existing->state.preferred_len,39);
  EXPECT_EQ(existing->state.resume_len,0);
  EXPECT_EQ(existing->state.resume_trial,0);
  auto *new_peer=nr_pdcch_dci_length_context(&bank,8,0x1003);
  ASSERT_NE(new_peer,nullptr);
  EXPECT_EQ(new_peer->state.preferred_len,39);
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


namespace {
struct RecurrentFixture {
  int occasion;
  int target_len;
  int distinct_limit;
  bool varying_rnti;

  static bool decode(int len, int trial, uint16_t *rnti, uint32_t *hash, void *opaque)
  {
    auto *f = static_cast<RecurrentFixture *>(opaque);
    if (len != f->target_len)
      return false;
    *rnti = f->varying_rnti
                ? static_cast<uint16_t>(1 + ((len * 257 + f->occasion * 17 + trial) % 65534))
                : 0xac78;
    const int payload_id = f->occasion < f->distinct_limit ? f->occasion : f->distinct_limit - 1;
    *hash = 0x90000000u + static_cast<uint32_t>(payload_id);
    return true;
  }
};
} // namespace

TEST(DciLengthSweepRntiRecurrence, FiveDistinctPayloadsLockWithoutBootstrapOrStatisticalFloor)
{
  nr_pdcch_dci_length_sweep_state_t state{};
  RecurrentFixture f{0, 46, 100, false};
  int found = -1;
  for (; f.occasion < 4; ++f.occasion)
    EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&state, RecurrentFixture::decode, &f, 1, 30, 63, 0), -1);
  found = nr_pdcch_dci_length_sweep_feed(&state, RecurrentFixture::decode, &f, 1, 30, 63, 0);
  EXPECT_EQ(found, 46);
  EXPECT_LT(state.trials[46], 256);
  EXPECT_EQ(state.max_rnti_distinct[46], 5);
}

TEST(DciLengthSweepRntiRecurrence, InvariantPayloadNeverLocks)
{
  nr_pdcch_dci_length_sweep_state_t state{};
  RecurrentFixture f{0, 46, 1, false};
  for (; f.occasion < 20; ++f.occasion)
    EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&state, RecurrentFixture::decode, &f, 1, 30, 63, 0), -1);
  EXPECT_EQ(state.max_rnti_distinct[46], 1);
}

TEST(DciLengthSweepRntiRecurrence, FourDistinctPayloadsAreInsufficient)
{
  nr_pdcch_dci_length_sweep_state_t state{};
  RecurrentFixture f{0, 46, 4, false};
  for (; f.occasion < 20; ++f.occasion)
    EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&state, RecurrentFixture::decode, &f, 1, 30, 63, 0), -1);
  EXPECT_EQ(state.max_rnti_distinct[46], 4);
}

TEST(DciLengthSweepRntiRecurrence, ManyOneOffRntisDoNotLock)
{
  nr_pdcch_dci_length_sweep_state_t state{};
  RecurrentFixture f{0, 46, 100, true};
  for (; f.occasion < 100; ++f.occasion)
    EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(&state, RecurrentFixture::decode, &f, 1, 30, 63, 0), -1);
  EXPECT_LT(state.trials[46], 256);
  EXPECT_LT(state.max_rnti_distinct[46], NR_PDCCH_DCI_LENGTH_SWEEP_RNTI_LOCK);
}


TEST(DciLengthStore, InterleavedCoresetsAndRntisKeepIndependentEvidence)
{
  nr_pdcch_dci_length_store_t store{};
  auto *a = nr_pdcch_dci_length_store_get(&store, 0x1111, nullptr);
  auto *b = nr_pdcch_dci_length_store_get(&store, 0x2222, nullptr);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(a, b);
  auto *a1 = nr_pdcch_dci_length_context(a, 0x1111, 0x1001);
  auto *a2 = nr_pdcch_dci_length_context(a, 0x1111, 0x1002);
  auto *b1 = nr_pdcch_dci_length_context(b, 0x2222, 0x1001);
  ASSERT_NE(a1, a2);
  ASSERT_NE(a1, b1);
  a1->state.trials[47] = 11;
  a2->state.trials[48] = 22;
  b1->state.trials[49] = 33;

  EXPECT_EQ(nr_pdcch_dci_length_store_get(&store, 0x2222, nullptr), b);
  EXPECT_EQ(nr_pdcch_dci_length_store_get(&store, 0x1111, nullptr), a);
  EXPECT_EQ(nr_pdcch_dci_length_context(a, 0x1111, 0x1001)->state.trials[47], 11);
  EXPECT_EQ(nr_pdcch_dci_length_context(a, 0x1111, 0x1002)->state.trials[48], 22);
  EXPECT_EQ(nr_pdcch_dci_length_context(b, 0x2222, 0x1001)->state.trials[49], 33);
}


TEST(DciLengthStore, PrimaryPlusSixteenLookaheadGeometriesRetainEvidence)
{
  nr_pdcch_dci_length_store_t store{};
  constexpr uint64_t active = 17;
  for (uint64_t key = 1; key <= active; ++key) {
    auto *bank = nr_pdcch_dci_length_store_get(&store, key, nullptr);
    ASSERT_NE(bank, nullptr);
    nr_pdcch_dci_length_context(bank, key, 0xac78)->state.bootstrap_hits[46] = key;
  }

  /* A second discovery pass visits the same primary + K=16 lanes. None of the
   * accumulated evidence may have been evicted between passes. */
  for (uint64_t key = 1; key <= active; ++key) {
    auto *bank = nr_pdcch_dci_length_store_get(&store, key, nullptr);
    ASSERT_NE(bank, nullptr);
    EXPECT_EQ(nr_pdcch_dci_length_context(bank, key, 0xac78)->state.bootstrap_hits[46], key);
  }
}

TEST(DciLengthStore, LruEvictionClearsRatherThanAliasesEvidence)
{
  nr_pdcch_dci_length_store_t store{};
  for (uint64_t i = 1; i <= NR_PDCCH_LENGTH_CORESETS; ++i) {
    auto *bank = nr_pdcch_dci_length_store_get(&store, i, nullptr);
    nr_pdcch_dci_length_context(bank, i, 0x2345)->state.trials[47] = static_cast<uint32_t>(i);
  }
  /* Refresh key 1, so key 2 is the oldest entry. */
  ASSERT_NE(nr_pdcch_dci_length_store_get(&store, 1, nullptr), nullptr);
  uint64_t evicted = 0;
  auto *fresh = nr_pdcch_dci_length_store_get(&store, 999, &evicted);
  EXPECT_EQ(evicted, 2u);
  ASSERT_NE(fresh, nullptr);
  auto *ctx = nr_pdcch_dci_length_context(fresh, 999, 0x2345);
  EXPECT_EQ(ctx->state.trials[47], 0u);
  EXPECT_EQ(nr_pdcch_dci_length_context(
                nr_pdcch_dci_length_store_get(&store, 1, nullptr), 1, 0x2345)->state.trials[47],
            1u);
}

TEST(DciLengthSweep, LocksLength100) {
  nr_pdcch_dci_length_sweep_state_t state{};
  auto scorer = [](int len, int trial, uint16_t *rnti, uint32_t *hash, void *ctx) -> bool {
    if (len != 100) return false;
    *rnti = 0x4b31;
    *hash = ++*static_cast<unsigned *>(ctx);
    return true;
  };
  unsigned serial = 0;
  int found = -1;
  for (int i = 0; i < 8; ++i)
    found = nr_pdcch_dci_length_sweep_feed(&state, scorer, &serial, 2, 30, 140, 0x4b31);
  EXPECT_EQ(found, 100);
}

TEST(DciLengthSweep, SeenLengthFirstOutwardOrder) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(([]{
  setenv("ISAC_RECONF", "1", 1);
  nr_pdcch_dci_length_seen_reset();
  nr_pdcch_dci_length_note_seen(47);
  int order[NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN];
  const int n = nr_pdcch_dci_length_order(30, 140, order);
  EXPECT_EQ(n, 111);
  const int expected[] = {47, 46, 48, 45, 49, 44, 50, 43, 51, 42};
  for (int i = 0; i < 10; ++i) EXPECT_EQ(order[i], expected[i]);
  bool visited[141] = {};
  for (int i = 0; i < n; ++i) {
    ASSERT_GE(order[i], 30); ASSERT_LE(order[i], 140);
    EXPECT_FALSE(visited[order[i]]);
    visited[order[i]] = true;
  }
  nr_pdcch_dci_length_seen_reset();
  _exit(::testing::Test::HasFailure() ? 1 : 0);
  }()), ::testing::ExitedWithCode(0), "");
}

TEST(DciLengthSweep, SeenOrderSurvivesBudgetResumeAndNewHints) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(([]{
  setenv("ISAC_RECONF", "1", 1);
  nr_pdcch_dci_length_seen_reset();
  nr_pdcch_dci_length_note_seen(47);
  nr_pdcch_dci_length_sweep_state_t state{};
  struct Visits { int lengths[141]; int count = 0; } visits;
  auto scorer = [](int len, int, uint16_t *, uint32_t *, void *ctx) -> bool {
    auto *v = static_cast<Visits *>(ctx);
    v->lengths[v->count++] = len;
    return false;
  };
  // Interrupt in the middle of the outward walk, then discover a distant length.
  EXPECT_EQ(nr_pdcch_dci_length_sweep_feed_budget(
      &state, scorer, &visits, 1, 30, 140, 0, 0, 10), -1);
  EXPECT_EQ(visits.count, 10);
  const int expected[] = {47, 46, 48, 45, 49, 44, 50, 43, 51, 42};
  for (int i = 0; i < 10; ++i) EXPECT_EQ(visits.lengths[i], expected[i]);
  nr_pdcch_dci_length_note_seen(100);
  EXPECT_EQ(nr_pdcch_dci_length_sweep_feed(
      &state, scorer, &visits, 1, 30, 140, 0), -1);
  EXPECT_EQ(visits.count, 34);
  EXPECT_EQ(visits.lengths[10], 52);
  bool seen[141] = {};
  for (int i = 0; i < visits.count; ++i) {
    EXPECT_FALSE(seen[visits.lengths[i]]);
    seen[visits.lengths[i]] = true;
  }
  EXPECT_EQ(state.occasions_fed, 1);
  // All RNTI/geometry contexts share the next round's hints.
  nr_pdcch_dci_length_sweep_state_t other{};
  visits.count = 0;
  nr_pdcch_dci_length_sweep_feed_budget(&other, scorer, &visits, 1, 30, 140, 0, 0, 4);
  EXPECT_EQ(visits.lengths[0], 47);
  EXPECT_EQ(visits.lengths[1], 100);
  EXPECT_EQ(visits.lengths[2], 46);
  EXPECT_EQ(visits.lengths[3], 48);
  nr_pdcch_dci_length_seen_reset();
  _exit(::testing::Test::HasFailure() ? 1 : 0);
  }()), ::testing::ExitedWithCode(0), "");
}

TEST(DciLengthSweep, DefaultOrderIsAscending) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(([]{
    unsetenv("ISAC_RECONF");
    nr_pdcch_dci_length_note_seen(47);
    int order[141];
    EXPECT_EQ(nr_pdcch_dci_length_order(30, 140, order), 111);
    for (int i = 0; i < 111; ++i) EXPECT_EQ(order[i], 30 + i);
    _exit(::testing::Test::HasFailure() ? 1 : 0);
  }()), ::testing::ExitedWithCode(0), "");
}

TEST(DciLengthSweep, ColdRoundStaysNarrowAcrossStrideAndBudget) {
  nr_pdcch_dci_length_seen_reset();
  nr_pdcch_dci_length_sweep_state_t state{};
  state.stride = 3;
  auto scorer = [](int, int, uint16_t *, uint32_t *, void *) -> bool { return false; };
  while (!state.occasions_fed) {
    nr_pdcch_dci_length_sweep_feed_budget(&state, scorer, nullptr, 2, 30, 140, 0, 0, 7);
    for (int len = 64; len <= 140; ++len) EXPECT_EQ(state.trials[len], 0);
  }
  EXPECT_EQ(state.decodes, 34u * 2);
  for (int len = 30; len <= 63; ++len) EXPECT_EQ(state.trials[len], 2);
  while (state.occasions_fed < 2)
    nr_pdcch_dci_length_sweep_feed(&state, scorer, nullptr, 2, 30, 140, 0);
  for (int len = 64; len <= 140; ++len) EXPECT_EQ(state.trials[len], 2);
  nr_pdcch_dci_length_seen_reset();
}

TEST(DciLengthSweep, WideSeenLengthEnablesColdWideRound) {
  nr_pdcch_dci_length_seen_reset();
  nr_pdcch_dci_length_note_seen(100);
  nr_pdcch_dci_length_sweep_state_t state{};
  auto scorer = [](int, int, uint16_t *, uint32_t *, void *) -> bool { return false; };
  nr_pdcch_dci_length_sweep_feed(&state, scorer, nullptr, 1, 30, 140, 0);
  EXPECT_EQ(state.trials[100], 1);
  nr_pdcch_dci_length_seen_reset();
}

TEST(DciLengthSweep, ExplicitMaximumEnablesColdWideRound) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(([]{
    setenv("ISAC_DCI_LEN_MAX", "100", 1);
    nr_pdcch_dci_length_seen_reset();
    nr_pdcch_dci_length_sweep_state_t state{};
    auto scorer = [](int, int, uint16_t *, uint32_t *, void *) -> bool { return false; };
    nr_pdcch_dci_length_sweep_feed(&state, scorer, nullptr, 1, 30, 100, 0);
    EXPECT_EQ(state.trials[100], 1);
    _exit(::testing::Test::HasFailure() ? 1 : 0);
  }()), ::testing::ExitedWithCode(0), "");
}
