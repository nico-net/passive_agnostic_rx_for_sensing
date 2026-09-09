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
#include "nr_pdcch_ul_field_sweep.h"
#include "nr_pdcch_ul_discovery.h"
}
static nr_pdcch_blind_ul_opts_t facts() {
  nr_pdcch_blind_ul_opts_t o{};
  o.bwp_size=273; o.tda_count=2; o.tda_length[0]=14; o.tda_length[1]=12;
  o.tda_k2[0]=o.tda_k2[1]=4; o.dmrs_add_pos=2; o.dmrs_max_length=1;
  return o;
}
TEST(UlFieldSweep, KnownWidthArithmeticAndCompleteAdmissibleSet) {
  auto fixed=facts();
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  auto n=nr_pdcch_ul_field_sweep_generate(&fixed,40,raw.data(),raw.size());
  ASSERT_EQ(n,87); // independently counted coefficient of the 16-axis generating polynomial
  bool found=false;
  for(int i=0;i<n;++i) {
    auto o=fixed; nr_pdcch_ul_field_sweep_apply(&raw[i],&o);
    EXPECT_EQ(nr_pdcch_blind_dci01_size(&o),40);
    EXPECT_EQ(o.tda_k2[1],4); EXPECT_EQ(o.bwp_size,273);
    nr_pdcch_ul_field_widths_t w; memcpy(&w,raw[i].bytes,sizeof(w));
    nr_pdcch_ul_field_widths_t known{};
    known.harq_pid_bits=4; known.dai1_bits=2; known.antenna_ports_bits=2;
    known.srs_request_bits=2; known.dmrs_seq_init_bits=1;
    if(!memcmp(&w,&known,sizeof(w))) found=true;
  }
  EXPECT_TRUE(found); // the plan incorrectly called this exact vector 47 bits
  EXPECT_EQ(nr_pdcch_ul_field_sweep_generate(&fixed,43,raw.data(),raw.size()),4145);
}
TEST(UlFieldSweep, OversizedSearchRefusesInsteadOfTruncating) {
  auto fixed=facts(); std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  EXPECT_EQ(nr_pdcch_ul_field_sweep_generate(&fixed,47,raw.data(),raw.size()),NR_HYP_SWEEP_RAW_OVERFLOW);
  EXPECT_EQ(nr_pdcch_ul_field_sweep_generate(&fixed,40,raw.data(),86),NR_HYP_SWEEP_RAW_OVERFLOW);
  fixed.tda_count=17;
  EXPECT_EQ(nr_pdcch_ul_field_sweep_generate(&fixed,40,raw.data(),raw.size()),NR_HYP_SWEEP_INVALID);
}
TEST(UlFieldSweep, SyntheticOracleConvergesOnBoundedGeneratedSet) {
  auto fixed=facts(); std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int n=nr_pdcch_ul_field_sweep_generate(&fixed,39,raw.data(),raw.size());
  ASSERT_EQ(n,13);
  nr_hyp_sweep_state_t st; nr_hyp_t chosen;
  ASSERT_EQ(nr_hyp_sweep_init(&st,raw.data(),n,nullptr,nullptr,nullptr,nullptr,0,nullptr),n);
  /* Algorithm-only oracle, not a claim of actual PUSCH decoding. Check each possible winner
   * independently so selection cannot depend on enumeration order. */
  for(int truth=0;truth<n;++truth) {
    ASSERT_EQ(nr_hyp_sweep_init(&st,raw.data(),n,nullptr,nullptr,nullptr,nullptr,0,nullptr),n);
    for(int t=0;t<n*NR_HYP_SWEEP_MIN_TRIALS && nr_hyp_sweep_winner(&st)<0;++t) {
      int idx=nr_hyp_sweep_next(&st,nullptr,nullptr,nullptr,&chosen);
      nr_hyp_sweep_feed(&st,idx,idx==truth && st.classes[idx].trials%4!=0);
    }
    EXPECT_EQ(nr_hyp_sweep_winner(&st),truth);
  }
}
/* The prerequisite that is genuinely unknown is the UL BWP: it is the RIV reference AND it sets
 * the frequency-domain field's width, so nothing can be interpreted without it.
 *
 * This test used to use tda_count == 0 as its "unknown" case. That was never right --
 * nr_pdcch_blind_dci01_size() reads 0 as the TS 38.214 Table 6.1.2.1.1-2 default 16-entry table,
 * i.e. a 4-bit field and a COMPLETE interpretation -- and it only kept passing because the search
 * hit NR_HYP_SWEEP_CLASS_OVERFLOW at the old 64-class cap and refused for an unrelated reason.
 * Raising that cap unmasked it. Both halves are asserted below so neither can hide the other
 * again. */
TEST(UlFieldSweep, UnknownPrerequisitesDoNotEmitGuessedGrants) {
  nr_pdcch_ul_discovery_reset();
  auto unknown=facts(); unknown.bwp_size=0;
  nr_pdcch_blind_ul_result_t out{};
  for(int p=1;p<12;++p) EXPECT_FALSE(nr_pdcch_ul_discovery_grant(&unknown,43,0x1234,p,&out));
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_classes,0);
  nr_pdcch_ul_discovery_feedback(&out,true);
  nr_pdcch_ul_discovery_reset();
}
TEST(UlFieldSweep, DefaultTdaTableIsResolvedAndArmsTheSearch) {
  nr_pdcch_ul_discovery_reset();
  auto resolved=facts(); resolved.tda_count=0;   // default 16-entry table => 4-bit TDA field
  nr_pdcch_blind_ul_result_t out{};
  for(int p=1;p<12;++p) nr_pdcch_ul_discovery_grant(&resolved,43,0x1234,p,&out);
  EXPECT_GT(nr_pdcch_ul_discovery_snapshot().width_classes,0)
      << "the default TDRA table is a complete interpretation, not an unknown";
  nr_pdcch_ul_discovery_reset();
}
