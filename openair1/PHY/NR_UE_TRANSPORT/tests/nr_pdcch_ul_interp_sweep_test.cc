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

#include <cstring>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_ul_interp_sweep.h"
}
TEST(UlInterpSweep, CatalogueAndObservedIndexIsolation) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  // 10 TDA x 4 pos x 2 max x (2 types x 3 non-TP tables + 1 type x 2 TP tables).
  ASSERT_EQ(nr_pdcch_ul_interp_sweep_generate(raw.data(),raw.size()),640);
  int found=-1;
  for(int i=0;i<640;++i) {
    nr_pdcch_ul_interp_hyp_t h; memcpy(&h,raw[i].bytes,sizeof(h));
    if(h.tda_start==0 && h.tda_length==14 && h.tda_mapping==0 && h.tda_k2==4 &&
       h.dmrs_config_type==0 && h.dmrs_add_pos==2 && h.dmrs_max_length==1 &&
       h.transform_precoding==0 && h.mcs_table==0) found=i;
  }
  ASSERT_GE(found,0);
  nr_pdcch_blind_ul_opts_t o{}; o.tda_count=3; o.tda_k2[0]=9; o.harq_pid_bits=5;
  ASSERT_TRUE(nr_pdcch_ul_interp_sweep_apply(&raw[found],2,&o));
  EXPECT_EQ(o.tda_k2[0],9); EXPECT_EQ(o.tda_k2[2],4); EXPECT_EQ(o.tda_length[2],14);
  EXPECT_EQ(o.harq_pid_bits,5);
  EXPECT_FALSE(nr_pdcch_ul_interp_sweep_apply(&raw[found],3,&o));
}
/* CONTRACT CHANGED: NR_HYP_SWEEP_MAX_CLASSES was unified with NR_HYP_SWEEP_MAX_RAW
 * (nr_hyp_sweep.h) so a live UL search stops refusing permanently whenever finite-sample class
 * splitting pushed a real cell's config past the old, separate 512-class cap -- see that header's
 * comment for the measured live failure this fixed. Direct consequence, checked here rather than
 * assumed: a class always holds >=1 of the n raw hypotheses fed to nr_hyp_sweep_init, so
 * n_classes <= n; n itself is already bounded by NR_HYP_SWEEP_MAX_RAW before init ever runs; so
 * n_classes <= n <= MAX_RAW == MAX_CLASSES always, and NR_HYP_SWEEP_CLASS_OVERFLOW can no longer
 * fire from any call that already passed the raw-cap check below. That is not a relaxation of
 * "refuse loudly, never truncate silently": the raw cap is still enforced, and it is now the ONLY
 * limit that can trip, because it is tighter than -- and implies -- the old separate class limit.
 * This test used to assert the opposite: that the full un-collapsed interpretation catalogue (960,
 * no equivalence merging in this generator) overflowed a lower, separate class cap. That premise no
 * longer holds by construction; reintroducing it would mean re-lowering MAX_CLASSES below MAX_RAW,
 * i.e. reintroducing the exact defect nr_hyp_sweep.h's comment documents fixing. */
TEST(UlInterpSweep, FullCatalogueFitsAndRawCapStillRefusesLoudly) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  int n=nr_pdcch_ul_interp_sweep_generate(raw.data(),raw.size());
  nr_hyp_sweep_state_t st;
  const int classes=nr_hyp_sweep_init(&st,raw.data(),n,nullptr,nullptr,nullptr,nullptr,0,nullptr);
  ASSERT_EQ(classes,n) << "no equivalence fn supplied: every raw hypothesis must be its own class";
  EXPECT_LE(classes,NR_HYP_SWEEP_MAX_CLASSES);
  EXPECT_EQ(nr_hyp_sweep_winner(&st),-1);
  EXPECT_EQ(nr_pdcch_ul_interp_sweep_generate(raw.data(),n-1),NR_HYP_SWEEP_RAW_OVERFLOW);
}
TEST(UlInterpSweep, DifferentK2IsNotAnEquivalentGrant) {
  nr_pdcch_blind_ul_opts_t a{},b{}; a.tda_count=b.tda_count=1;
  nr_pdcch_ul_interp_hyp_t h{0,14,0,1,0,2,1,0,0};
  nr_hyp_t raw{}; raw.len=sizeof(h); memcpy(raw.bytes,&h,sizeof(h));
  ASSERT_TRUE(nr_pdcch_ul_interp_sweep_apply(&raw,0,&a));
  h.tda_k2=4; memcpy(raw.bytes,&h,sizeof(h));
  ASSERT_TRUE(nr_pdcch_ul_interp_sweep_apply(&raw,0,&b));
  EXPECT_NE(a.tda_k2[0],b.tda_k2[0]);
  /* Matching only S/L, as the plan's tie-check did, is not grant equivalence. */
}
/* TS 38.214 Table 6.1.2.1-1 (normal CP): type A S = 0, L 4..14; type B S 0..13, L 1..14, S+L <= 14. */
/* TS 38.214 6.1.4.1: transform-precoding-enabled hypotheses must only ever carry mcs_table in
 * {3,4} (Table 6.1.4.1-1/-2), never {0,1,2} (the non-TP tables, including qam256 which TP does not
 * combine with); non-TP hypotheses must only ever carry {0,1,2}. Checked over every generated
 * hypothesis, not just one, since a single off-by-one in the generator's loop bounds would
 * otherwise leak one wrong combination through undetected. */
TEST(UlInterpSweep, TransformPrecodingOnlyGeneratesItsOwnMcsTables) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int n = nr_pdcch_ul_interp_sweep_generate(raw.data(), raw.size());
  ASSERT_GT(n, 0);
  int tp0_count = 0, tp1_count = 0;
  for (int i = 0; i < n; ++i) {
    nr_pdcch_ul_interp_hyp_t h;
    memcpy(&h, raw[i].bytes, sizeof(h));
    if (h.transform_precoding) {
      EXPECT_TRUE(h.mcs_table == 3 || h.mcs_table == 4) << "i=" << i << " mcs_table=" << (int)h.mcs_table;
      ++tp1_count;
    } else {
      EXPECT_TRUE(h.mcs_table <= 2) << "i=" << i << " mcs_table=" << (int)h.mcs_table;
      ++tp0_count;
    }
  }
  EXPECT_EQ(tp0_count, 480);
  EXPECT_EQ(tp1_count, 160);
}
TEST(UlInterpSweep, TransformPrecodingNeverGeneratesType2) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int n = nr_pdcch_ul_interp_sweep_generate(raw.data(), raw.size());
  ASSERT_GT(n, 0);
  for (int i = 0; i < n; ++i) {
    nr_pdcch_ul_interp_hyp_t h;
    memcpy(&h, raw[i].bytes, sizeof(h));
    EXPECT_FALSE(h.transform_precoding && h.dmrs_config_type) << "hypothesis=" << i;
  }
}
TEST(UlInterpSweep, ApplyRejectsIllegalTransformPrecodingType2WithoutMutation) {
  nr_pdcch_ul_interp_hyp_t h{0,14,0,1,1,1,1,1,3};
  nr_hyp_t raw{}; raw.len=sizeof(h); memcpy(raw.bytes,&h,sizeof(h));
  nr_pdcch_blind_ul_opts_t opts{}; opts.tda_count=1; opts.tda_k2[0]=9;
  EXPECT_FALSE(nr_pdcch_ul_interp_sweep_apply(&raw,0,&opts));
  EXPECT_EQ(opts.tda_k2[0],9);
}
TEST(UlInterpSweep, PuschLegalTdaCounts) {
  int a=0,b=0;
  for(int S=0;S<14;++S)
    for(int L=1;L<=14;++L) { a+=nr_pusch_tda_legal(0,S,L); b+=nr_pusch_tda_legal(1,S,L); }
  EXPECT_EQ(a,11);
  EXPECT_EQ(b,105);
  EXPECT_FALSE(nr_pusch_tda_legal(2,0,4));
}
TEST(UlInterpSweep, EveryCatalogueTdaIsLegalAndTypeBIsPresent) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int n=nr_pdcch_ul_interp_sweep_generate(raw.data(),raw.size());
  ASSERT_GT(n,0);
  int type_b=0;
  for(int i=0;i<n;++i) {
    nr_pdcch_ul_interp_hyp_t h; memcpy(&h,raw[i].bytes,sizeof(h));
    EXPECT_TRUE(nr_pusch_tda_legal(h.tda_mapping,h.tda_start,h.tda_length));
    type_b+=h.tda_mapping==1;
  }
  EXPECT_GT(type_b,0);
}

// ---- Gap item 2: PUSCH TDRA type B, full (S,L) space once the DM-RS energy oracle has pinned one --

TEST(UlInterpSweepPinned, ReachesAFullTypeBPointTheCuratedCatalogueNeveromits) {
  // (S,L) = (5,6) type B is legal (S 0..13, L 1..14, S+L<=14) but is NOT one of the curated
  // catalogue's four rows -- exactly the gap this closes.
  ASSERT_TRUE(nr_pusch_tda_legal(1, 5, 6));
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int n = nr_pdcch_ul_interp_sweep_generate_pinned(raw.data(), raw.size(), 5, 6, 1);
  ASSERT_EQ(n, 4 /*k2*/ * (2 * 4 * 2 * 3 /*CP*/ + 4 * 2 * 2 /*TP: type1, tables3/4*/));
  for (int i = 0; i < n; i++) {
    nr_pdcch_ul_interp_hyp_t h; memcpy(&h, raw[i].bytes, sizeof(h));
    EXPECT_EQ(h.tda_start, 5);
    EXPECT_EQ(h.tda_length, 6);
    EXPECT_EQ(h.tda_mapping, 1);
    EXPECT_GE(h.tda_k2, 1);
    EXPECT_LE(h.tda_k2, 4);
  }
  // Confirms the curated catalogue really never reaches this point (the premise above).
  std::vector<nr_hyp_t> curated(NR_HYP_SWEEP_MAX_RAW);
  const int nc = nr_pdcch_ul_interp_sweep_generate(curated.data(), curated.size());
  for (int i = 0; i < nc; i++) {
    nr_pdcch_ul_interp_hyp_t h; memcpy(&h, curated[i].bytes, sizeof(h));
    EXPECT_FALSE(h.tda_start == 5 && h.tda_length == 6 && h.tda_mapping == 1);
  }
}

TEST(UlInterpSweepPinned, RejectsAnIllegalPin) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  // Type A requires S=0; S=3 type A is illegal.
  EXPECT_EQ(nr_pdcch_ul_interp_sweep_generate_pinned(raw.data(), raw.size(), 3, 4, 0), NR_HYP_SWEEP_INVALID);
  // S+L>14 is illegal for either mapping type.
  EXPECT_EQ(nr_pdcch_ul_interp_sweep_generate_pinned(raw.data(), raw.size(), 10, 10, 1), NR_HYP_SWEEP_INVALID);
}

TEST(UlInterpSweepPinned, TransformPrecodingUsesOnlyType1AndTpMcsTables) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int n = nr_pdcch_ul_interp_sweep_generate_pinned(raw.data(), raw.size(), 5, 6, 1);
  ASSERT_GT(n, 0);
  unsigned cp[2][3] = {}, tp[2] = {};
  unsigned illegal_tp_type = 0, illegal_tp_table = 0, illegal_cp_table = 0;
  for (int i = 0; i < n; ++i) {
    nr_pdcch_ul_interp_hyp_t h;
    memcpy(&h, raw[i].bytes, sizeof(h));
    if (h.transform_precoding) {
      illegal_tp_type += h.dmrs_config_type != 0;
      if (h.mcs_table < 3 || h.mcs_table > 4) ++illegal_tp_table;
      else ++tp[h.mcs_table - 3];
    } else {
      if (h.mcs_table > 2 || h.dmrs_config_type > 1) ++illegal_cp_table;
      else ++cp[h.dmrs_config_type][h.mcs_table];
    }
  }
  EXPECT_EQ(illegal_tp_type, 0u);
  EXPECT_EQ(illegal_tp_table, 0u);
  EXPECT_EQ(illegal_cp_table, 0u);
  // Every k2/position/maxLength combination reaches both TP tables and all CP tables.
  for (unsigned count : tp) EXPECT_EQ(count, 4u * 4u * 2u);
  for (const auto &type : cp)
    for (unsigned count : type) EXPECT_EQ(count, 4u * 4u * 2u);
  EXPECT_EQ(n, 256);
  EXPECT_EQ(nr_pdcch_ul_interp_sweep_generate_pinned(raw.data(), 255, 5, 6, 1), NR_HYP_SWEEP_RAW_OVERFLOW);
  EXPECT_EQ(nr_pdcch_ul_interp_sweep_generate_pinned(raw.data(), 256, 5, 6, 1), 256);
}

TEST(UlEnergySpan, RecoversTheOccupiedSpanFromAPeakedProfile) {
  double e[14] = {0};
  for (int i = 2; i < 12; i++) e[i] = 100.0; // S=2, L=10
  int S = -1, L = -1;
  ASSERT_TRUE(nr_pusch_ul_energy_span(e, 0.25, &S, &L));
  EXPECT_EQ(S, 2);
  EXPECT_EQ(L, 10);
}

TEST(UlEnergySpan, IgnoresNoiseBelowTheRelativeThreshold) {
  double e[14] = {0};
  for (int i = 0; i < 14; i++) e[i] = 1.0; // noise floor everywhere
  for (int i = 4; i < 8; i++) e[i] = 100.0; // real signal, S=4, L=4
  int S = -1, L = -1;
  ASSERT_TRUE(nr_pusch_ul_energy_span(e, 0.25, &S, &L));
  EXPECT_EQ(S, 4);
  EXPECT_EQ(L, 4);
}

TEST(UlEnergySpan, AllZeroProfileFails) {
  double e[14] = {0};
  int S, L;
  EXPECT_FALSE(nr_pusch_ul_energy_span(e, 0.25, &S, &L));
}

TEST(UlDmrsPin, GetFailsUntilSetThenLatchesTheFirstValue) {
  nr_pusch_ul_dmrs_pin_reset();
  int S = -1, L = -1, m = -1;
  EXPECT_FALSE(nr_pusch_ul_dmrs_pin_get(&S, &L, &m));
  nr_pusch_ul_dmrs_pin_set(2, 12); // legal type B (S!=0)
  ASSERT_TRUE(nr_pusch_ul_dmrs_pin_get(&S, &L, &m));
  EXPECT_EQ(S, 2);
  EXPECT_EQ(L, 12);
  EXPECT_EQ(m, 1);
  // First observation wins: a second, different measurement must not overwrite it.
  nr_pusch_ul_dmrs_pin_set(0, 14);
  ASSERT_TRUE(nr_pusch_ul_dmrs_pin_get(&S, &L, &m));
  EXPECT_EQ(S, 2);
  EXPECT_EQ(L, 12);
  nr_pusch_ul_dmrs_pin_reset();
  EXPECT_FALSE(nr_pusch_ul_dmrs_pin_get(nullptr, nullptr, nullptr));
}

TEST(UlDmrsPin, RejectsAnIllegalSpanAndStaysUnset) {
  nr_pusch_ul_dmrs_pin_reset();
  nr_pusch_ul_dmrs_pin_set(1, 0); // L=0 is illegal for either mapping type
  EXPECT_FALSE(nr_pusch_ul_dmrs_pin_get(nullptr, nullptr, nullptr));
  nr_pusch_ul_dmrs_pin_reset();
}

// Review-caught: an earlier version of this file guessed mapping type A whenever S==0 && L>=4 and
// pinned that guess forever. That is wrong -- TS 38.214 Table 6.1.2.1-1 makes type B legal at S=0
// too (any L with S+L<=14), so (S=0,L=14) is legal under EITHER mapping and energy occupancy alone
// (which is all this oracle measures) cannot tell them apart. The pin must keep that ambiguity
// rather than manufacture a false certainty that then excludes every legal type-B row at (0,14).
TEST(UlDmrsPin, AmbiguousSpanKeepsBothMappingTypesRatherThanGuessing) {
  nr_pusch_ul_dmrs_pin_reset();
  ASSERT_TRUE(nr_pusch_tda_legal(0, 0, 14)); // legal as type A
  ASSERT_TRUE(nr_pusch_tda_legal(1, 0, 14)); // ALSO legal as type B: genuinely ambiguous
  nr_pusch_ul_dmrs_pin_set(0, 14);
  int m = -1;
  ASSERT_TRUE(nr_pusch_ul_dmrs_pin_get(nullptr, nullptr, &m));
  EXPECT_EQ(m, NR_PUSCH_MAPPING_EITHER);
  // The pinned generator must then produce hypotheses for BOTH mapping types at this (S,L).
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int n = nr_pdcch_ul_interp_sweep_generate_pinned(raw.data(), raw.size(), 0, 14, m);
  ASSERT_EQ(n, 2 * 256); // both mapping types, same legal CP/TP combinations as the single-map case
  bool saw_a = false, saw_b = false;
  for (int i = 0; i < n; i++) {
    nr_pdcch_ul_interp_hyp_t h; memcpy(&h, raw[i].bytes, sizeof(h));
    EXPECT_EQ(h.tda_start, 0);
    EXPECT_EQ(h.tda_length, 14);
    saw_a |= h.tda_mapping == 0;
    saw_b |= h.tda_mapping == 1;
  }
  EXPECT_TRUE(saw_a);
  EXPECT_TRUE(saw_b);
  nr_pusch_ul_dmrs_pin_reset();
}

// S=0 does NOT always mean "candidate type A": type A additionally requires L>=4, so a short span
// starting at 0 is unambiguously type B despite S==0.
TEST(UlDmrsPin, ShortSpanAtSZeroIsUnambiguouslyTypeB) {
  nr_pusch_ul_dmrs_pin_reset();
  ASSERT_FALSE(nr_pusch_tda_legal(0, 0, 2)); // type A needs L>=4
  ASSERT_TRUE(nr_pusch_tda_legal(1, 0, 2));  // type B has no such floor
  nr_pusch_ul_dmrs_pin_set(0, 2);
  int m = -1;
  ASSERT_TRUE(nr_pusch_ul_dmrs_pin_get(nullptr, nullptr, &m));
  EXPECT_EQ(m, 1);
  nr_pusch_ul_dmrs_pin_reset();
}

// Review-caught race: nr_pusch_ul_dmrs_pin_set() used to be a plain load-then-store with no claim
// on the fields it writes, so two UL consumer threads racing into it (a real case: `ul_thread` can
// run more than one consumer) could tear a reader's view of (S,L,mapping). This cannot PROVE the
// absence of a race in one run, but it is exactly the shape a thread sanitizer or a lucky
// interleaving would catch, and it pins the invariant the CAS-claim fix depends on: whichever
// (S,L) wins is entirely self-consistent (never a mix of two callers' values), every run.
TEST(UlDmrsPin, ConcurrentSetIsNeverTornEvenUnderContention) {
  for (int trial = 0; trial < 50; ++trial) {
    nr_pusch_ul_dmrs_pin_reset();
    static const int kCandidates[][2] = {{0, 14}, {2, 12}, {5, 6}, {0, 4}, {3, 8}};
    std::vector<std::thread> threads;
    // Capture s/l BY VALUE: a by-reference capture of the range-for variable would dangle once the
    // loop moves on, since the thread body runs concurrently with (not after) this loop.
    for (const auto &c : kCandidates) {
      const int s = c[0], l = c[1];
      threads.emplace_back([s, l] { nr_pusch_ul_dmrs_pin_set(s, l); });
    }
    for (auto &t : threads) t.join();
    int S = -1, L = -1, m = -1;
    ASSERT_TRUE(nr_pusch_ul_dmrs_pin_get(&S, &L, &m));
    // Whoever won must be one of the offered candidates, in full (never S from one thread paired
    // with L from another), and mapping must be exactly what that (S,L) legally admits.
    bool matched_one = false;
    for (const auto &c : kCandidates)
      matched_one |= (S == c[0] && L == c[1]);
    EXPECT_TRUE(matched_one) << "S=" << S << " L=" << L << " matches no offered candidate -- torn write";
    const bool legal_a = nr_pusch_tda_legal(0, S, L), legal_b = nr_pusch_tda_legal(1, S, L);
    const int expected_m = (legal_a && legal_b) ? NR_PUSCH_MAPPING_EITHER : (legal_a ? 0 : 1);
    EXPECT_EQ(m, expected_m);
  }
  nr_pusch_ul_dmrs_pin_reset();
}
