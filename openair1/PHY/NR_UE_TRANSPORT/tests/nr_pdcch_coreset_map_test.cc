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

/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_coreset_map_test.cc
 * \brief Phase 3 Technique A (roadmap artifact): full-carrier DM-RS correlation mapping, tested
 * with a SYNTHETIC symbol containing one genuinely occupied 6-RB window (built by regenerating
 * the exact same DM-RS this function itself uses to detect it -- a closed-loop test, deliberately
 * so: it proves the correlation MATH is self-consistent before any live capture is spent on it,
 * the same discipline nr_pdcch_blind_monitor_test.cc's own RawPayloadRoundTrip group uses).
 */
#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <gtest/gtest.h>

extern "C" {
#include "PHY/impl_defs_top.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "nr_pdcch_coreset_map.h"
// Real current signatures (openair1/PHY/NR_REFSIG/nr_refsig.h) -- nr_pdcch_dmrs_ref()'s third
// argument is an RB COUNT (it writes nb_rb_coreset*3 pilots), not a raw pilot-element count.
uint32_t* nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short scrambling_id, int slot, int symbol);
void nr_pdcch_dmrs_ref(const uint32_t* gold, c16_t* pilot, unsigned short nb_rb_coreset);
}

// LOG/CONFIG_LIB need these outside a full softmodem executable -- same convention as
// nr_pdcch_blind_monitor_test.cc / isac_sync_test.cc.
extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  if (s != nullptr) {
    fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  }
  if (assert) {
    abort();
  } else {
    exit(EXIT_SUCCESS);
  }
}

TEST(CoresetMap, FindsOneSyntheticallyOccupiedWindowAboveNoise) {
  const int n_rb_carrier = 48;       // small carrier for a fast, deterministic test
  const int ofdm_symbol_size = 512;  // must exceed n_rb_carrier*12 with margin
  const int first_carrier_offset = 10;
  const uint16_t scrambling_id = 2;  // this project's PCI on the reference cell
  const int slot = 3, symbol = 0;

  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(42);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto& s : rxdataF) {
    s.r = (int16_t)std::lround(noise(rng));
    s.i = (int16_t)std::lround(noise(rng));
  }

  // Plant a real, correctly-generated PDCCH DM-RS at RB offset 18 (a 6-RB window not aligned to
  // either carrier edge, so the test cannot pass by an off-by-one accident at the boundary).
  const int occupied_rb_offset = 18;
  const int pilot_rb_count = occupied_rb_offset + 6;
  uint32_t* gold = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, slot, symbol);
  std::vector<c16_t> pilot(pilot_rb_count * 3);
  nr_pdcch_dmrs_ref(gold, pilot.data(), (unsigned short)pilot_rb_count);
  for (int rb = occupied_rb_offset; rb < occupied_rb_offset + 6; rb++) {
    for (int p = 0; p < 3; p++) {
      const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
      // pilot[] is already conj(X); the received symbol at a real DM-RS RE is Y = X (unit
      // channel, no noise added on top of the ambient floor above) -> conj(pilot) recovers X.
      rxdataF[k].r = (int16_t)pilot[rb * 3 + p].r;
      rxdataF[k].i = (int16_t)(-pilot[rb * 3 + p].i);
    }
  }

  nr_pdcch_coreset_candidate_t candidates[16];
  const int n = nr_pdcch_coreset_map_scan(rxdataF.data(), ofdm_symbol_size, n_rb_carrier, first_carrier_offset,
                                          scrambling_id, slot, symbol, candidates, 16);
  ASSERT_GT(n, 0);
  EXPECT_EQ(candidates[0].rb_offset, occupied_rb_offset);
  EXPECT_GT(candidates[0].corr, 0.7);  // real DM-RS gives ~0.8-0.95 per this project's own measured range
}

TEST(CoresetMap, ReportsNothingOnPureNoise) {
  const int n_rb_carrier = 48, ofdm_symbol_size = 512, first_carrier_offset = 10;
  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto& s : rxdataF) {
    s.r = (int16_t)std::lround(noise(rng));
    s.i = (int16_t)std::lround(noise(rng));
  }
  nr_pdcch_coreset_candidate_t candidates[16];
  const int n = nr_pdcch_coreset_map_scan(rxdataF.data(), ofdm_symbol_size, n_rb_carrier, first_carrier_offset,
                                          2, 3, 0, candidates, 16);
  // Pure noise gives sqrt(pi)/(2*sqrt(18)) ~= 0.209 per trial (this project's own already-derived
  // figure for an 18-pilot CCE); the significance bar this function uses must sit well above that.
  EXPECT_EQ(n, 0);
}

TEST(UssPrior, ContainsEveryStandardHashHypothesisAndRejectsNoCandidate)
{
  static const uint32_t A[3] = {39827, 39829, 39839};
  static const int Mset[7] = {1, 2, 3, 4, 5, 6, 8};
  std::mt19937 rng(0x3813);
  for (int trial = 0; trial < 200; ++trial) {
    const int n_cces = 6 + (rng() % 40);
    const int ai = rng() % 4;
    const int L = 1 << ai;
    if (n_cces / L < 1)
      continue;
    const int N = n_cces / L;
    const uint16_t rnti = (uint16_t)(1 + rng() % 0xfff0);
    const int slot = rng() % 20;
    const int cid = rng() % 3;
    const int M = Mset[rng() % 7];
    const int m = rng() % M;
    uint32_t Y = rnti;
    for (int s = 0; s <= slot; ++s)
      Y = (A[cid] * Y) % 65537u;
    const uint16_t expected =
        (uint16_t)(L * ((Y + (uint32_t)((m * n_cces) / (L * M))) % (uint32_t)N));

    std::vector<uint16_t> cce(N), support(N);
    std::vector<uint8_t> al(N, (uint8_t)L);
    for (int i = 0; i < N; ++i)
      cce[i] = (uint16_t)(i * L);
    nr_pdcch_uss_candidate_supports(n_cces, slot, &rnti, 1, cce.data(), al.data(), N,
                                    support.data());
    ASSERT_GT(support[expected / L], 0)
        << "trial=" << trial << " L=" << L << " M=" << M << " cid=" << cid;
  }
}

TEST(UssPrior, CoversDurationThreeCcePositionsAboveFortyFive)
{
  constexpr int n_cces=135, L=1, slot=17;
  const uint16_t rnti=0x5a3c;
  std::vector<uint16_t> cce(n_cces), support(n_cces);
  std::vector<uint8_t> al(n_cces, L);
  for (int i=0;i<n_cces;i++) cce[i]=(uint16_t)i;
  nr_pdcch_uss_candidate_supports(n_cces,slot,&rnti,1,cce.data(),al.data(),n_cces,
                                  support.data());
  bool above=false;
  for (int i=46;i<n_cces;i++) above |= support[i] > 0;
  EXPECT_TRUE(above);
}

TEST(UssPrior, EmptyIdentitySetProducesNoPreference)
{
  const uint16_t cce[] = {0, 2, 4, 6};
  const uint8_t al[] = {2, 2, 2, 2};
  uint16_t support[4] = {9, 9, 9, 9};
  nr_pdcch_uss_candidate_supports(12, 7, nullptr, 0, cce, al, 4, support);
  for (const uint16_t v : support)
    EXPECT_EQ(v, 0);
}

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


TEST(DmrsRank, InterleavedPilotsBeatStrongerNoiseAndWrongSlot) {
  constexpr int fft=1024, nrb=60, carrier=800, slot=7, id=382;
  std::vector<c16_t> rx(fft*14);
  std::mt19937 rng(184);
  std::normal_distribution<double> noise(0,3000);
  for (auto &v:rx) v={(int16_t)noise(rng),(int16_t)noise(rng)};
  std::vector<c16_t> pilot(nrb*3);
  nr_pdcch_coreset_pilot(id,slot,0,nrb,pilot.data());
  // Independently worked example: 48 RB, D=1, Lreg=6, R=2, shift=3, CCE0 AL2.
  // Bundles 0,1 map to bundles 3,7 -> RB18..23,42..47, plus a 1-RB CORESET offset.
  for (int rb=0;rb<nrb;rb++) if ((rb>=19&&rb<25)||(rb>=43&&rb<49)) {
    const int phase=rb<25 ? 1 : -1; // independent bundle precoders must not cancel
    for(int q=0;q<3;q++) {
      const auto x=pilot[rb*3+q];
      rx[(carrier+rb*12+1+4*q)%fft]={(int16_t)(phase*x.r/16),(int16_t)(-phase*x.i/16)};
    }
  }
  nr_pdcch_dmrs_rank_grid_t g;
  ASSERT_TRUE(nr_pdcch_dmrs_rank_grid(&g,rx.data(),fft,carrier,nrb,id,slot,0,1,0));
  const double correct=nr_pdcch_dmrs_candidate_score(&g,1,48,6,2,3,0,2);
  EXPECT_NEAR(correct,1.0,1e-9);
  EXPECT_GT(correct,nr_pdcch_dmrs_candidate_score(&g,1,48,6,2,0,0,2)+0.5);
  ASSERT_TRUE(nr_pdcch_dmrs_rank_grid(&g,rx.data(),fft,carrier,nrb,id,slot+1,0,1,0));
  EXPECT_LT(nr_pdcch_dmrs_candidate_score(&g,1,48,6,2,3,0,2),0.5);
}

TEST(DmrsRank, ExplicitReferenceAndSixRbAlOne) {
  constexpr int fft=1024,nrb=60,offset=7;
  std::vector<c16_t> rx(fft*14),pilot(nrb*3);
  nr_pdcch_coreset_pilot(382,2,1,nrb,pilot.data());
  for(int rb=offset;rb<offset+6;rb++) for(int q=0;q<3;q++) {
    const auto x=pilot[(rb-offset)*3+q];
    rx[fft+(rb*12+1+4*q)]={x.r,(int16_t)-x.i};
  }
  nr_pdcch_dmrs_rank_grid_t g;
  ASSERT_TRUE(nr_pdcch_dmrs_rank_grid(&g,rx.data(),fft,0,nrb,382,2,1,1,offset));
  EXPECT_NEAR(nr_pdcch_dmrs_candidate_score(&g,offset,6,0,0,0,0,1),1.0,1e-9);
  EXPECT_FALSE(std::isfinite(nr_pdcch_dmrs_candidate_score(&g,offset,6,0,0,0,0,2)));
  EXPECT_FALSE(nr_pdcch_dmrs_rank_grid(&g,rx.data(),fft,0,nrb,382,2,13,2,offset));
  EXPECT_EQ(g.n_rb,0);
}

TEST(DmrsRank, WeakCandidatesRemainExplorableAtEveryAggregationLevel) {
  double score[20]; uint8_t al[20]; bool seen[20]={};
  for(int i=0;i<20;i++) { score[i]=20-i; al[i]=1<<(i/4); }
  for(uint64_t visit=0;visit<4;visit++) {
    uint8_t order[20];
    ASSERT_EQ(nr_pdcch_dmrs_candidate_order(score,al,20,visit,false,order),10);
    bool once[20]={};
    double prev[5] = {INFINITY, INFINITY, INFINITY, INFINITY, INFINITY};
    for(int j=0;j<10;j++) {
      EXPECT_FALSE(once[order[j]]); once[order[j]]=true; seen[order[j]]=true;
      int ai=0; while ((1<<ai) < al[order[j]]) ++ai;
      EXPECT_GE(prev[ai], score[order[j]]);
      prev[ai]=score[order[j]];
    }
    for(int i=0;i<20;i+=4) EXPECT_TRUE(once[i]); // highest-scoring candidate of each AL
  }
  for(bool v:seen) EXPECT_TRUE(v);
  uint8_t order[20];
  EXPECT_EQ(nr_pdcch_dmrs_candidate_order(score,al,20,4,true,order),20);
  score[0]=std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(nr_pdcch_dmrs_candidate_order(score,al,20,4,true,order),20);
  int pos0=-1;
  for (int j=0;j<20;j++) if (order[j]==0) pos0=j;
  ASSERT_GE(pos0,0);
  for (int i=1;i<4;i++) {
    int pos=-1;
    for (int j=0;j<20;j++) if (order[j]==i) pos=j;
    EXPECT_LT(pos,pos0); // NaN is last within AL1; other ALs are intentionally interleaved.
  }
}
