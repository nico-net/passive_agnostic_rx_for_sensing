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
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_ul_interp_sweep.h"
}
TEST(UlInterpSweep, CatalogueAndObservedIndexIsolation) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  // 10 tda x 2 type x 4 pos x 2 max x (3 mcs for tp=0 + 2 mcs for tp=1) = 10*2*4*2*5 = 800: TP
  // enabled has only 2 valid MCS-table choices (qam256 does not combine with transform precoding),
  // not 3, so the raw catalogue shrank from the pre-TP-support 960 -- narrower, not looser, since
  // every one of those 160 removed combinations described a configuration TS 38.214 forbids.
  ASSERT_EQ(nr_pdcch_ul_interp_sweep_generate(raw.data(),raw.size()),800);
  int found=-1;
  for(int i=0;i<800;++i) {
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
  // 3 mcs choices x half the (tda,type,pos,max) space for tp=0; 2 choices for tp=1.
  EXPECT_EQ(tp0_count, n * 3 / 5);
  EXPECT_EQ(tp1_count, n * 2 / 5);
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
