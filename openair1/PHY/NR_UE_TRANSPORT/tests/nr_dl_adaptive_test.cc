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
 */

#include <gtest/gtest.h>
#include <set>
#include <tuple>
#include <vector>
extern "C" {
#include "nr_pdcch_blind_monitor.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_pdsch_adaptive_config.h"
#include "PHY/impl_defs_top.h"
#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
uint32_t *nr_gold_pdcch(int, int, unsigned short, int, int);
void nr_pdcch_dmrs_ref(const uint32_t *, c16_t *, unsigned short);
}

TEST(DlAdaptive, CompleteLegalCatalogAndNoPermanentlyUnfeedableHypotheses) {
  const int sl[][2]={{1,13},{0,14},{2,12},{1,12},{0,13},{2,10},{1,7},{0,7}};
  using Key=std::tuple<int,int,int,int>;
  for(int typeA : {0,1}) {
    std::set<Key> expected, actual;
    for(auto &p:sl) for(int add=0;add<4;add++) for(int len=1;len<=2;len++) for(int mcs=0;mcs<3;mcs++) {
      int mask=nr_pdcch_blind_dmrs_mask(typeA,p[1],p[0],0,add,len);
      if(mask>0) expected.emplace(p[0],p[1],mask,mcs);
    }
    nr_pdsch_config_sweep_state_t state{};
    nr_pdsch_config_sweep_init_legal(&state,2,typeA,nr_pdcch_blind_dmrs_mask);
    for(int i=0;i<state.n_hyp;i++) {
      auto h=state.hyp[i];
      EXPECT_GT(h.dmrs_mask,0);
      actual.emplace(h.tda_start,h.tda_length,h.dmrs_mask,h.mcs_table);
    }
    EXPECT_EQ(actual,expected);
    EXPECT_EQ(actual.size(),static_cast<size_t>(state.n_hyp));
    ASSERT_GT(state.n_hyp,64); // regression: the old cap hid legal late entries
    // The last catalog entry was previously unreachable. Perfect oracle must now find it.
    const int truth=state.n_hyp-1;
    for(int i=0;i<400*state.n_hyp;i++) {
      nr_pdsch_cfg_hypothesis_t h{};
      const int index=nr_pdsch_config_sweep_next(&state,&h);
      nr_pdsch_config_sweep_feed(&state,index,index==truth);
    }
    EXPECT_EQ(state.winner,truth);
  }
}

TEST(DlAdaptive, SweptMcsReachesPduDecoderAndRateMatching) {
  for(int table=0;table<3;table++) {
    nr_pdsch_cfg_hypothesis_t h{};
    h.tda_start=1; h.tda_length=7;
    h.dmrs_mask=nr_pdcch_blind_dmrs_mask(0,7,1,0,0,1);
    h.mcs_table=table;
    fapi_nr_dl_config_dlsch_pdu_rel15_t pdu{};
    uint8_t grant_table=static_cast<uint8_t>((table+1)%3);
    int8_t lbrm=grant_table;
    nr_pdsch_adaptive_apply(&h,&pdu,&grant_table,&lbrm);
    EXPECT_EQ(pdu.mcs_table,table);
    EXPECT_EQ(grant_table,table);
    EXPECT_EQ(lbrm,table);
    EXPECT_EQ(pdu.dlDmrsSymbPos,h.dmrs_mask);
    EXPECT_EQ(pdu.start_symbol,1);
    EXPECT_EQ(pdu.number_symbols,7);
    // Exactly the two table lookups consumed by nr_pdsch_passive_decode().
    EXPECT_EQ(nr_get_Qm_dl(24,grant_table),nr_get_Qm_dl(24,table));
    EXPECT_EQ(nr_get_code_rate_dl(24,grant_table),nr_get_code_rate_dl(24,table));
  }
  EXPECT_NE(nr_get_Qm_dl(24,0),nr_get_Qm_dl(24,1));
}

static void discover_single_window() {
  constexpr int nrb=48, fft=1024, offset=10, occupied=18, pci=2, slot=3;
  std::vector<c16_t> rx(fft),pilot((occupied+6)*3);
  nr_pdcch_dmrs_ref(nr_gold_pdcch(nrb,14,pci,slot,0),pilot.data(),occupied+6);
  for(int rb=occupied;rb<occupied+6;rb++) for(int p=0;p<3;p++) {
    int k=(offset+rb*12+1+4*p)%fft;
    rx[k]={pilot[rb*3+p].r,static_cast<int16_t>(-pilot[rb*3+p].i)};
  }
  bool found=false;
  for(int i=0;i<1000 && !found;i++)
    found=nr_pdcch_blind_monitor_autodiscover_step(rx.data(),fft,nrb,offset,pci,slot,0,100+i);
  ASSERT_TRUE(found); // failed fixture is not evidence about verification
}
class DlGeometry : public testing::Test {
  void SetUp() override {
    nr_pdcch_blind_monitor_autodiscover_reset();
    nr_pdcch_blind_rnti_bootstrap_reset_for_test();
  }
  void TearDown() override { nr_pdcch_blind_monitor_autodiscover_reset(); }
};
TEST_F(DlGeometry, HistoricalRntiCannotVerifyNewGeometry) {
  nr_pdcch_blind_rnti_bootstrap_record(0x4601,NR_BLIND_RNTI_CLASS_C,1);
  nr_pdcch_blind_rnti_bootstrap_record(0x4601,NR_BLIND_RNTI_CLASS_C,2);
  ASSERT_NO_FATAL_FAILURE(discover_single_window());
  ASSERT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
  bool rotated=false;
  for(int i=0;i<4000;i++) rotated=nr_pdcch_blind_monitor_autodiscover_extent_step(1100+i)||rotated;
  EXPECT_TRUE(rotated); // no DCI-length result was supplied: geometry must still advance
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
}
TEST_F(DlGeometry, FreshDistinctDedicatedGrantsVerifyOnlyTheirOwnEpoch) {
  ASSERT_NO_FATAL_FAILURE(discover_single_window());
  nr_pdcch_blind_monitor_autodiscover_observe(0x4601,1100,11);
  nr_pdcch_blind_monitor_autodiscover_observe(0x4601,1100,12);
  nr_pdcch_blind_monitor_autodiscover_observe(0x4601,1101,11);
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
  const auto generation=nr_pdcch_blind_monitor_autodiscover_generation();
  nr_pdcch_blind_monitor_autodiscover_retry(18);
  EXPECT_GT(nr_pdcch_blind_monitor_autodiscover_generation(),generation);
  nr_pdcch_blind_monitor_autodiscover_observe(0x4601,1102,12);
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
  nr_pdcch_blind_monitor_autodiscover_observe(0x4601,1103,13);
  EXPECT_TRUE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
  for(int i=0;i<5000;i++) EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_step(1200+i));
}
TEST_F(DlGeometry, RetrySearchesOtherWidthsAtSameOffsetAndNeverInventsVerification) {
  ASSERT_NO_FATAL_FAILURE(discover_single_window());
  const auto *cfg=nr_pdcch_blind_monitor_get_cfg();
  EXPECT_EQ(cfg->coreset_rb_offset,18);
  EXPECT_EQ(cfg->coreset_freq_domain,1);
  nr_pdcch_blind_monitor_autodiscover_retry(18);
  EXPECT_EQ(cfg->coreset_rb_offset,18);
  EXPECT_EQ(cfg->coreset_freq_domain,2);
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_offset_rejected(18));
  int trials=1;
  while(nr_pdcch_blind_monitor_autodiscover_done() && trials<40) {
    nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
    trials++;
    EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
  }
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_done());
  EXPECT_EQ(trials,20); // four admissible starts times five admissible ends, all searched
  ASSERT_NO_FATAL_FAILURE(discover_single_window()); // quiet intervals do not permanently blacklist
}
TEST(DlAdaptive, ExtentCatalogContainsEveryAdmissibleContiguousGeometry) {
  nr_pdcch_extent_cand_t c[45*46/2];
  const int n=nr_pdcch_extent_candidates(20,20,45,c,45*46/2);
  EXPECT_EQ(n,21*25);
  std::set<std::pair<int,int>> got;
  for(int i=0;i<n;i++) got.emplace(c[i].first_w,c[i].last_w);
  EXPECT_EQ(got.size(),static_cast<size_t>(n));
  for(int f=0;f<=20;f++) for(int l=20;l<45;l++) EXPECT_EQ(got.count({f,l}),1u);
}
