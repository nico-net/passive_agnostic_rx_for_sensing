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
  ASSERT_EQ(nr_pdcch_ul_interp_sweep_generate(raw.data(),raw.size()),960);
  int found=-1;
  for(int i=0;i<960;++i) {
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
TEST(UlInterpSweep, FullCatalogueCannotSilentlyBypassClassCap) {
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  int n=nr_pdcch_ul_interp_sweep_generate(raw.data(),raw.size());
  nr_hyp_sweep_state_t st;
  EXPECT_EQ(nr_hyp_sweep_init(&st,raw.data(),n,nullptr,nullptr,nullptr,nullptr,0,nullptr),
            NR_HYP_SWEEP_CLASS_OVERFLOW);
  EXPECT_EQ(nr_hyp_sweep_winner(&st),-1);
  EXPECT_EQ(nr_pdcch_ul_interp_sweep_generate(raw.data(),959),NR_HYP_SWEEP_RAW_OVERFLOW);
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
