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

  // CSS0 autoconf is a hard prerequisite of real autodiscover deployments (see this feature's own
  // "bootstrap RNTI" comment in nr_pdcch_blind_monitor.c) and gives us the SAME exclusion mechanism
  // production uses to keep CORESET#0's own known-legitimate footprint out of the dedicated-CORESET
  // recurrence oracle (s_css0_excl_first_w/last_w). Reused here (windows 0..2, i.e. RB 0..17) so the
  // background injected below -- needed to clear ISAC_DISCOVER_MIN_BG, see that comment -- cannot
  // itself accumulate recurrence and get mistaken for a second dedicated CORESET; it is excluded on
  // exactly the same footing CORESET#0 is in production. g_cfg's CSS0 fields are fully overwritten
  // once real dedicated discovery below completes (nr_pdcch_blind_monitor.c's own comment on that
  // overwrite block), so this has no effect on the discovered geometry itself.
  ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(18,1,0,12,40,0,2,0,1,2,12,2));

  std::vector<c16_t> rx(fft),pilot((occupied+6)*3);
  nr_pdcch_dmrs_ref(nr_gold_pdcch(nrb,14,pci,slot,0),pilot.data(),occupied+6);
  auto write_window=[&](int rb0){
    for(int rb=rb0;rb<rb0+6;rb++) for(int p=0;p<3;p++) {
      int k=(offset+rb*12+1+4*p)%fft;
      rx[k]={pilot[rb*3+p].r,static_cast<int16_t>(-pilot[rb*3+p].i)};
    }
  };
  auto clear_window=[&](int rb0){
    for(int rb=rb0;rb<rb0+6;rb++) for(int p=0;p<3;p++) {
      int k=(offset+rb*12+1+4*p)%fft;
      rx[k]={0,0};
    }
  };
  write_window(occupied); // the real, always-present CORESET signal (window occupied/6 == 3)

  // Realistic non-zero background (2026-09-25, R15/fix-link-report.md "Fix round 1"): before this,
  // every OTHER candidate window read a literal, permanent zero, so nr_pdcch_blind_monitor.c's
  // ISAC_DISCOVER_MIN_BG median-hit background gate (default 3, ~line 1576) could never clear and
  // the oracle spun forever without ever completing a dwell. Periodically -- and much more weakly
  // than the real window's every-call presence -- light up three OTHER windows in rotation so their
  // hit counts land a few counts above zero: enough to satisfy the background gate, nowhere near
  // the real window's rate, so it stays the unique "lit" window and the discovered geometry
  // (rb_offset 18 / window 3, which every DlGeometry test below asserts) is unaffected.
  constexpr int kBgWindows[] = {0, 1, 2};
  constexpr int kBgPeriod = 50; // ~20 injections/window per 1000-call dwell; the gate only needs 3

  // Call budget: nr_pdcch_blind_monitor.c's MIN_ORACLE_DWELLS (8) x AUTODISCOVER_OBS_CALLS (1000)
  // -- the oracle commits only after that many independent, well-populated dwells (each dwell
  // resets its own histogram, so this is the real minimum, not a one-off warm-up). A little slack
  // over the exact product avoids off-by-one flakiness against the background gate above.
  constexpr int kMinOracleDwells = 8;
  constexpr int kAutodiscoverObsCalls = 1000;
  constexpr int kDiscoveryCallBudget = kMinOracleDwells * kAutodiscoverObsCalls + 500;

  bool found=false;
  for(int i=0;i<kDiscoveryCallBudget && !found;i++) {
    const int bg_w = kBgWindows[(i / kBgPeriod) % 3];
    const bool inject_bg = (i % kBgPeriod) == 0;
    if (inject_bg) write_window(bg_w * 6);
    found=nr_pdcch_blind_monitor_autodiscover_step(rx.data(),fft,nrb,offset,pci,slot,0,100+i);
    if (inject_bg) clear_window(bg_w * 6);
  }
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
  // Deliberate walk order (51f7d3deac, extent_advance()'s s_ext_phase_idx loop, design comment:
  // "that phase is only a search-order prior... Try it first, then the other five residues"): PER
  // MAPPING, all 6 RB-phase hypotheses are tried before the mapping index advances -- the opposite
  // of the old mapping-first order this test used to assert. For a 6-RB extent (window 3, RB
  // 18..23, well inside the 48-RB carrier so every phase is in-bounds): the initial discovery is
  // already phase 0/mapping 0; each of the 5 remaining phases costs one retry (bundle unchanged,
  // since only the phase index moves this extent's coreset_rb_offset among 18..23); advancing to
  // the next mapping costs one more retry (phase resets to the CSS0-derived hint, i.e. back to
  // offset 18, with a new/interleaved bundle). So walking every one of maps0 mappings this way
  // costs 6*maps0-1 retries that still belong to this extent, and retry #(6*maps0) is the one that
  // finally leaves it.
  auto n_maps=[&](){ nr_pdcch_map_cand_t m[512]; return nr_pdcch_map_candidates(cfg->coreset_freq_domain*6,cfg->coreset_duration,cfg->coreset_pdcch_dmrs_scrambling_id,m,512); };
  const int maps0=n_maps();
  EXPECT_GT(maps0,1);
  for(int mapping=0;mapping<maps0;++mapping){
    for(int phase=1;phase<6;++phase){
      nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
      // Still window 3 (offset/6 is phase-invariant: a phase only shifts within the 6-RB window),
      // same mapping/span -- just a different one of its 6 physical phase residues.
      EXPECT_EQ((cfg->bwp_start+cfg->coreset_rb_offset)/6,3);
      EXPECT_EQ(cfg->coreset_freq_domain,1);
    }
    if(mapping+1<maps0){
      nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
      EXPECT_EQ(cfg->coreset_rb_offset,18); // phase reset to the hint on the new mapping
      EXPECT_NE(cfg->coreset_reg_bundle_size,0); // ...which is one of the interleaved hypotheses
    }
  }
  nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
  // The second extent is the full carrier (see ExtentCandidates.NearestHypothesesComeFirst), back
  // at the non-interleaved mapping. Its span is the WHOLE carrier (48 RB), so every non-zero phase
  // pushes coreset_rb_offset+span past bwp_size and extent_advance()'s own bounds check
  // (off+span>bwp_size) skips it internally without ever returning -- the phase walk is a
  // structural no-op on a full-carrier extent, so its mapping walk below is retry-for-retry
  // identical to the pre-phase-first order (nearest dilation of the observed window comes after).
  EXPECT_EQ(cfg->coreset_rb_offset,0);
  EXPECT_EQ(cfg->coreset_freq_domain,cfg->bwp_size/6);
  EXPECT_EQ(cfg->coreset_reg_bundle_size,0);
  for(int i=1;i<n_maps();i++) nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
  nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
  // The extent CATALOG itself (nr_pdcch_extent_candidates_multi, single seed = window 3, the
  // discovered window) is independent of the phase-first walk order above -- it is generated once,
  // up front, at oracle-commit time -- so it is derived here directly from that same production
  // function rather than hand-computed or copied from an older run: the catalog has grown since
  // this test was last updated (36 entries now, not the 20 "four starts x five ends" the previous
  // version of this test assumed), and re-deriving it is what "not by copying whatever the code
  // outputs blindly" means in practice -- call the exact function under test, not a guess of its
  // output.
  nr_pdcch_extent_cand_t cat[64];
  const int seed_w=3;
  const int n_ext=nr_pdcch_extent_candidates_multi(&seed_w,1,cfg->bwp_size/6,cat,64);
  ASSERT_GT(n_ext,2);
  ASSERT_EQ(cat[0].first_w,3); ASSERT_EQ(cat[0].last_w,3);         // extent 0: the seed's own window
  ASSERT_EQ(cat[1].first_w,0); ASSERT_EQ(cat[1].last_w,7);         // extent 1: the full carrier
  const int third_offset=cat[2].first_w*6;                        // phase 0 (the hint) at entry
  const int third_span=cat[2].last_w-cat[2].first_w+1;
  EXPECT_EQ(cfg->coreset_rb_offset,third_offset);
  EXPECT_EQ(cfg->coreset_freq_domain,third_span);
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_offset_rejected(18));
  // Exhaustive walk over the remaining extents. An extent is identified by (window index, span),
  // recovering the window index as offset/6 -- safe because a phase variant only ever shifts the
  // offset by 0..5 within the SAME 6-RB-aligned window, never across one, so this is unaffected by
  // the phase-first reordering above (raw offset/freq_domain deltas would over-count: they'd also
  // fire on every phase step within one extent's own mapping walk).
  int extents=3, trials=0;
  int last_w=(cfg->bwp_start+cfg->coreset_rb_offset)/6, last_span=cfg->coreset_freq_domain;
  while(nr_pdcch_blind_monitor_autodiscover_done() && trials<400000) {
    nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
    trials++;
    if(nr_pdcch_blind_monitor_autodiscover_done()) {
      const int w=(cfg->bwp_start+cfg->coreset_rb_offset)/6;
      if(w!=last_w || cfg->coreset_freq_domain!=last_span){ extents++; last_w=w; last_span=cfg->coreset_freq_domain; }
    }
    EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_extent_verified());
  }
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_done());
  // The catalog's own size is the ground truth (see above) -- every one of its n_ext entries must
  // be searched exactly once before autodiscover_done() gives up.
  // is taken as the second hypothesis and is one of those 20, so the count is unchanged.
  EXPECT_EQ(extents,n_ext);
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
  // Same phase-first walk as RetrySearchesOtherWidthsAtSameOffsetAndNeverInventsVerification (see
  // its comment for the full derivation): each of the pass0 mappings gets its 5-retry phase walk
  // (bundle fixed, offset cycling through this window's other 5 phase residues) before the mapping
  // index advances; 6*pass0-1 retries stay inside this extent's pass-0 lap, and retry #(6*pass0)
  // is the one that leaves it. For the STAGED default under test here, "leaves it" means the next
  // EXTENT rather than a same-extent mapping advance, because s_map_n is truncated to pass0 (the
  // remaining, non-pass-0 shifts are a second lap over every extent, never tried within this one)
  // -- so the exit retry must NOT land back on offset 18 the way an in-extent mapping advance does.
  for(int mapping=0;mapping<pass0;++mapping){
    for(int phase=1;phase<6;++phase){
      nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
      EXPECT_EQ((cfg->bwp_start+cfg->coreset_rb_offset)/6,3);
    }
    if(mapping+1<pass0){
      nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
      EXPECT_EQ(cfg->coreset_rb_offset,18);
    }
  }
  nr_pdcch_blind_monitor_autodiscover_retry(cfg->bwp_start+cfg->coreset_rb_offset);
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
