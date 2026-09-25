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

#include <cstdlib>
#include <iostream>
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
  /* Every legal (S,L) of TS 38.214 Table 5.1.2.1-1 for BOTH mapping types (type A S 0..3, L 3..14;
   * type B S 0..12, L 2..13; S+L <= 14) x k0 {0,1}: no curated prefix. The key is the effective PDU, so
   * a type-B entry identical to a type-A one is one hypothesis. */
  using Key=std::tuple<int,int,int,int,int>;
  for(int typeA : {0,1}) {
    std::set<Key> expected, actual;
    for(int mt=0;mt<2;mt++) for(int S=0;S<=12;S++) for(int L=2;S+L<=14;L++) for(int k0=0;k0<2;k0++)
      for(int add=0;add<4;add++) for(int len=1;len<=2;len++) for(int mcs=0;mcs<3;mcs++) {
      if(!nr_pdsch_tda_legal(mt,S,L)) continue;
      int mask=nr_pdcch_blind_dmrs_mask(typeA,L,S,mt,add,len);
      if(mask>0) expected.emplace(S,L,k0,mask,mcs);
    }
    nr_pdsch_config_sweep_state_t state{};
    nr_pdsch_config_sweep_init_legal(&state,2,typeA,nr_pdcch_blind_dmrs_mask);
    for(int i=0;i<state.n_hyp;i++) {
      auto h=state.hyp[i];
      EXPECT_GT(h.dmrs_mask,0);
      actual.emplace(h.tda_start,h.tda_length,h.k0,h.dmrs_mask,h.mcs_table);
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
  // Exhaustive walk (every mapping of an extent before the next extent) is now opt-in: the
  // default is a pass-0 lap first (see StagedDefaultWalksPassZeroPrefixThenMovesOn).
  setenv("ISAC_MAP_PASS0_ONLY","0",1);
  ASSERT_NO_FATAL_FAILURE(discover_single_window());
  const auto *cfg=nr_pdcch_blind_monitor_get_cfg();
  EXPECT_EQ(cfg->coreset_rb_offset,18);
  EXPECT_EQ(cfg->coreset_freq_domain,1);
  EXPECT_EQ(cfg->coreset_reg_bundle_size,0); // hypothesis 0: non-interleaved
  // Every CCE-to-REG mapping of an extent is tried before the next extent: a retry walks the
  // interleaved hypotheses of this 6-RB window first (each with the same dwell as an extent).
  auto n_maps=[&](){ nr_pdcch_map_cand_t m[512]; return nr_pdcch_map_candidates(cfg->coreset_freq_domain*6,cfg->coreset_duration,cfg->coreset_pdcch_dmrs_scrambling_id,m,512); };
  const int maps0=n_maps();
  EXPECT_GT(maps0,1);
  for(int i=1;i<maps0;i++){
    nr_pdcch_blind_monitor_autodiscover_retry(18);
    EXPECT_EQ(cfg->coreset_rb_offset,18);
    EXPECT_EQ(cfg->coreset_freq_domain,1);
    EXPECT_NE(cfg->coreset_reg_bundle_size,0);
  }
  nr_pdcch_blind_monitor_autodiscover_retry(18);
  // The second extent is the full carrier (see ExtentCandidates.NearestHypothesesComeFirst),
  // back at the non-interleaved mapping; the nearest dilation of the observed window comes after it.
  EXPECT_EQ(cfg->coreset_rb_offset,0);
  EXPECT_EQ(cfg->coreset_freq_domain,cfg->bwp_size/6);
  EXPECT_EQ(cfg->coreset_reg_bundle_size,0);
  for(int i=1;i<n_maps();i++) nr_pdcch_blind_monitor_autodiscover_retry(0);
  nr_pdcch_blind_monitor_autodiscover_retry(0);
  EXPECT_EQ(cfg->coreset_rb_offset,18);
  EXPECT_EQ(cfg->coreset_freq_domain,2);
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_offset_rejected(18));
  int extents=3, trials=0;
  while(nr_pdcch_blind_monitor_autodiscover_done() && trials<20000) {
    const int off=cfg->coreset_rb_offset, span=cfg->coreset_freq_domain;
    nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
    trials++;
    if(nr_pdcch_blind_monitor_autodiscover_done() && (cfg->coreset_rb_offset!=off || cfg->coreset_freq_domain!=span)) extents++;
    EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
  }
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_done());
  // Four admissible starts x five admissible ends = 20 geometries, all searched; the full carrier
  // is taken as the second hypothesis and is one of those 20, so the count is unchanged.
  EXPECT_EQ(extents,20);
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

TEST_F(DlGeometry, StagedDefaultWalksPassZeroPrefixThenMovesOn) {
  // Default (env unset): the primary walks only the pass-0 mappings (PCI residue / shift 0 of every
  // legal (L, R)) of an extent, then advances to the next extent; the remaining shifts come in a
  // second lap. The pass-0 count is what the enumerator emits under ISAC_MAP_PASS0_ONLY=1.
  unsetenv("ISAC_MAP_PASS0_ONLY");
  ASSERT_NO_FATAL_FAILURE(discover_single_window());
  const auto *cfg=nr_pdcch_blind_monitor_get_cfg();
  ASSERT_EQ(cfg->coreset_rb_offset,18);
  nr_pdcch_map_cand_t m[512];
  const int full=nr_pdcch_map_candidates(6,cfg->coreset_duration,cfg->coreset_pdcch_dmrs_scrambling_id,m,512);
  int pass0=0;
  for(int i=0;i<full;i++){ // pass-0 prefix: shift is 0 or the PCI residue, and the prefix is contiguous
    const int nb=(6*cfg->coreset_duration)/m[i].bundle;
    const bool p0=(m[i].bundle==0)||(m[i].shift==0)||(m[i].shift==cfg->coreset_pdcch_dmrs_scrambling_id%nb);
    if(!p0) break;
    pass0=i+1;
  }
  ASSERT_GT(pass0,0); ASSERT_LT(pass0,full);
  for(int i=1;i<pass0;i++){ nr_pdcch_blind_monitor_autodiscover_retry(18); EXPECT_EQ(cfg->coreset_rb_offset,18); }
  nr_pdcch_blind_monitor_autodiscover_retry(18);
  EXPECT_NE(cfg->coreset_rb_offset,18); // pass-0 prefix exhausted: next extent, not the remaining shifts
}

TEST_F(DlGeometry, UlScanIntentSurvivesCss0AndRealDedicatedDiscovery) {
  auto *cfg=const_cast<nr_pdcch_blind_monitor_cfg_t *>(nr_pdcch_blind_monitor_get_cfg());
  const auto saved=*cfg;
  for(int requested : {0,1}) {
    cfg->dci01_scan=requested;
    cfg->ul.bwp_size=273;
    for(int cycle=0;cycle<2;cycle++) {
      nr_pdcch_blind_monitor_autodiscover_reset();
      ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(48,1,0,12,40,0,2,0,1,2,12,0));
      EXPECT_EQ(cfg->coreset_type,1);
      EXPECT_FALSE(nr_pdcch_blind_monitor_ul_scan_enabled(cfg));
      ASSERT_NO_FATAL_FAILURE(discover_single_window());
      ASSERT_EQ(cfg->coreset_type,0);
      EXPECT_EQ(cfg->dci01_scan,requested) << "request=" << requested << " cycle=" << cycle;
      EXPECT_EQ(nr_pdcch_blind_monitor_ul_scan_enabled(cfg),requested==1);
    }
  }
  *cfg=saved;
}

TEST(UlScanContext, RejectsCommonDisabledAndMalformedContextsEvenWithAutoEnabled) {
  nr_pdcch_blind_monitor_cfg_t cfg{};
  cfg.dl_full_auto=1;
  cfg.dci01_scan=1;
  cfg.ul.bwp_size=273;
  cfg.dci10_ss_type=NR_BLIND_SS_UE_SPECIFIC;
  EXPECT_TRUE(nr_pdcch_blind_monitor_ul_scan_enabled(&cfg));
  cfg.dci10_ss_type=NR_BLIND_SS_COMMON;
  EXPECT_FALSE(nr_pdcch_blind_monitor_ul_scan_enabled(&cfg));
  cfg.dci10_ss_type=NR_BLIND_SS_UE_SPECIFIC;
  cfg.ul.bwp_size=0;
  EXPECT_TRUE(nr_pdcch_blind_monitor_ul_scan_enabled(&cfg)); // raw auto recovery needs no BWP
  cfg.dl_full_auto=0;
  EXPECT_FALSE(nr_pdcch_blind_monitor_ul_scan_enabled(&cfg));
  cfg.dl_full_auto=1;
  cfg.ul.bwp_size=273;
  for(int disabled : {-1,0,2}) {
    cfg.dci01_scan=disabled;
    EXPECT_FALSE(nr_pdcch_blind_monitor_ul_scan_enabled(&cfg));
  }
  EXPECT_FALSE(nr_pdcch_blind_monitor_ul_scan_enabled(nullptr));
}

#include "../nr_passive_sample_lifetime.h"
TEST(DlAdaptive, DeferredSamplesExpireDuringFepAndMustNotScore) {
  EXPECT_TRUE(nr_passive_samples_valid(1017,1000,20));
  EXPECT_FALSE(nr_passive_samples_valid(1018,1000,20));
  EXPECT_FALSE(nr_passive_samples_valid(1020,1000,20));
  EXPECT_FALSE(nr_passive_samples_valid(999,1000,20)); // resync/rebase
  EXPECT_FALSE(nr_passive_samples_valid(-1,-1,20));
  EXPECT_FALSE(nr_passive_samples_valid(0,0,2));
  EXPECT_TRUE(nr_passive_samples_valid(20481,20479,20)); // SFN wrap, same epoch
}

#include "../nr_passive_ul_grant_book.h"
TEST(UlGrantBook, KeepsThreeUesInOneSlotAndAcceptsLateDci) {
  nr_passive_ul_book_t book{};
  nr_pdcch_blind_ul_result_t grant{};
  grant.plausible=true;grant.k2=4;
  for(int u=0;u<3;u++) {
    grant.rnti=0x200+u;grant.raw_payload=10+u;
    ASSERT_EQ(nr_passive_ul_book_put(&book,&grant,20476),1);
  }
  EXPECT_EQ(nr_passive_ul_book_put(&book,&grant,20476),0);
  nr_passive_ul_book_entry_t entry{};unsigned expired=0;
  EXPECT_FALSE(nr_passive_ul_book_take(&book,20479,20,&entry,&expired));
  std::set<int> rntis;
  for(int i=0;i<3;i++) {
    ASSERT_TRUE(nr_passive_ul_book_take(&book,20483,20,&entry,&expired));
    EXPECT_EQ(entry.target,20480);rntis.insert(entry.grant.rnti);
  }
  EXPECT_EQ(rntis.size(),3u);EXPECT_EQ(expired,0u);
  EXPECT_FALSE(nr_passive_ul_book_take(&book,20483,20,&entry,&expired));
  // A DCI decoded after its target slot still delivers the original IQ, not next frame's.
  EXPECT_EQ(nr_passive_ul_book_put(&book,&grant,20476),1);
  EXPECT_TRUE(nr_passive_ul_book_take(&book,20484,20,&entry,&expired));
  EXPECT_EQ(entry.target,20480);
}
TEST(UlGrantBook, ExpiredAndCapacityDropsNeverOverwriteAnotherUe) {
  nr_passive_ul_book_t book{};nr_pdcch_blind_ul_result_t g{};
  g.plausible=true;g.k2=0;g.rnti=0x345;
  for(int i=0;i<NR_PASSIVE_UL_BOOK_CAPACITY;i++) {
    g.raw_payload=i;
    ASSERT_EQ(nr_passive_ul_book_put(&book,&g,1000),1);
  }
  g.raw_payload=9999;
  EXPECT_EQ(nr_passive_ul_book_put(&book,&g,1000),-1);
  nr_passive_ul_book_entry_t e{};unsigned expired=0;
  EXPECT_FALSE(nr_passive_ul_book_take(&book,1018,20,&e,&expired));
  EXPECT_EQ(expired,NR_PASSIVE_UL_BOOK_CAPACITY);
  EXPECT_EQ(nr_passive_ul_book_put(&book,&g,1020),1);
}

/* Production-shaped type-B acquisition: the real mask generator, a TYPE-B truth (S=5 L=7, add_pos 1,
 * len 1, 256QAM, k0 0 -> DM-RS on symbols 5 and 9 = 0x220, last symbol 11). ONE oracle observation
 * (mask, last symbol, k0 of the DCI's own slot) must prune the live context to a few entries that
 * include the truth, and the context must then converge on it within the pre-Task-14 400k budget. */
TEST(DlAdaptive, TypeBTruthIsPinnedByOneOracleObservationAndConverges) {
  const int S = 5, L = 7;
  const uint16_t mask = (uint16_t)nr_pdcch_blind_dmrs_mask(0, L, S, 1, 1, 1);
  ASSERT_EQ(mask, 0x220);
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x7B, 0x4601, 0, 2, 0, nr_pdcch_blind_dmrs_mask, &t, &h));
  nr_pdsch_config_sweep_state_t st{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  const int full = st.n_hyp;
  const int n = nr_pdsch_config_sweep_observe(&t, mask, S + L - 1, 0);
  /* the prune re-indexed the context: take a fresh ticket before reading it */
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x7B, 0x4601, 0, 2, 0, nr_pdcch_blind_dmrs_mask, &t, &h));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  bool has_truth = false;
  for (int i = 0; i < st.n_hyp; i++)
    has_truth |= st.hyp[i].tda_start == S && st.hyp[i].tda_length == L && st.hyp[i].k0 == 0
                 && st.hyp[i].dmrs_mask == mask && st.hyp[i].mcs_table == 1 && st.hyp[i].mapping_type == 1;
  std::cerr << "[ MEASURED ] type-B truth: catalog " << full << " -> " << n << " after one observation" << std::endl;
  EXPECT_GT(n, 0);
  EXPECT_LE(n, 40);
  EXPECT_TRUE(has_truth);
  unsigned seed = 4242;
  int converged = -1;
  nr_pdsch_cfg_hypothesis_t w{};
  for (int i = 1; i <= 400000 && converged < 0; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x7B, 0x4601, 0, 2, 0, nr_pdcch_blind_dmrs_mask, &t, &h));
    const bool truth = h.tda_start == S && h.tda_length == L && h.k0 == 0 && h.dmrs_mask == mask && h.mcs_table == 1;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    if (nr_pdsch_config_sweep_feedback(&t, truth && u < 0.54, &w))
      converged = i;
  }
  std::cerr << "[ MEASURED ] type-B truth converged after " << converged << " outcomes" << std::endl;
  ASSERT_GT(converged, 0);
  EXPECT_EQ(w.tda_start, S);
  EXPECT_EQ(w.tda_length, L);
  EXPECT_EQ(w.mapping_type, 1);
  EXPECT_EQ(w.mcs_table, 1);
  nr_pdsch_config_sweep_reset_all();
}

TEST(DlAdaptive, MaskOracleCollapsesTheFullCatalogToAFewEntries) {
  // Measured OTA 2026-09-16 (DMRS_ORACLE mask=0x884, symbols 2/7/11). The full (S,L) x k0 catalog is
  // ~1000 entries; joint with ~200 live DCI layouts the OTA search spread 98k probes so thin that no
  // (layout, entry) pair was tried twice. The oracle must bring a context down to the entries that
  // actually carry DM-RS on those symbols, so the joint search is layouts x O(10), not x O(1000).
  nr_pdsch_config_sweep_state_t st{};
  const int full = nr_pdsch_config_sweep_init_legal(&st, 2, 0, nr_pdcch_blind_dmrs_mask);
  const int n = nr_pdsch_config_sweep_prune_mask(&st, 0x884);
  std::cerr << "[ MEASURED ] catalog " << full << " -> " << n << " after mask 0x884:";
  for (int i = 0; i < n; i++)
    std::cerr << " (S" << +st.hyp[i].tda_start << ",L" << +st.hyp[i].tda_length << ",k0=" << +st.hyp[i].k0
              << ",m" << +st.hyp[i].mcs_table << ")";
  std::cerr << "\n";
  EXPECT_GT(n, 0);
  EXPECT_LE(n, 40);
  for (int i = 0; i < n; i++) EXPECT_EQ(st.hyp[i].dmrs_mask, 0x884);
}
