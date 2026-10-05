/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "nr_srs_blind.h"
#include "nr_sense_ul.h"
#include "PHY/NR_UE_ISAC/nr_isac_ssb_axis.h"
extern "C" void srs_test_init(void);
extern "C" bool srs_test_generate(const nr_srs_grid_t *,const nr_srs_candidate_t *,int16_t *);
class SrsBlind : public testing::Test {
protected:
  nr_srs_grid_t g{512,356,24,4,14,20};
  nr_srs_candidate_t truth{2,0,12,2,4,8,17,3};
  std::vector<int16_t> iq;
  nr_srs_blind_t *s=nullptr;
  static void SetUpTestSuite() { srs_test_init(); }
  void SetUp() override { iq.resize(4*14*512*2); s=nr_srs_blind_create(&g); ASSERT_NE(s,nullptr); }
  void TearDown() override { nr_srs_blind_destroy(s); }
  void generate() {
    std::fill(iq.begin(),iq.end(),0);
    ASSERT_TRUE(srs_test_generate(&g,&truth,iq.data()));
    for(int a=1;a<4;a++) std::copy_n(iq.data(),14*512*2,iq.data()+a*14*512*2);
  }
};
TEST_F(SrsBlind,SrsCombDetectorSynth) {
  for(int comb:{2,4,8}) for(int offset=0;offset<comb;offset++) for(int ns:{1,2,4}) {
    truth.comb=comb; truth.comb_offset=offset; truth.n_symbols=ns; truth.start_symbol=14-ns;
    generate(); nr_srs_candidate_t c[NR_SRS_MAX_CANDIDATES];
    EXPECT_TRUE(nr_srs_pregate(&g,iq.data(),14*512,0x3f00));
    int n=nr_srs_comb_detect(&g,iq.data(),14*512,0x3f00,c);
    ASSERT_GT(n,0); EXPECT_EQ(c[0].comb,comb); EXPECT_EQ(c[0].comb_offset,offset);
    EXPECT_EQ(c[0].n_symbols,ns); EXPECT_EQ(c[0].start_symbol,14-ns);
    EXPECT_EQ(c[0].start_rb,4); EXPECT_EQ(c[0].nb_rb,8);
  }
}
TEST_F(SrsBlind,NoiseAndDownlinkRejected) {
  uint32_t x=9;
  for(auto &v:iq) {x=x*1664525u+1013904223u;v=(x>>20)-2048;}
  nr_srs_candidate_t c[NR_SRS_MAX_CANDIDATES];
  EXPECT_EQ(nr_srs_comb_detect(&g,iq.data(),14*512,0x3f00,c),0);
  generate(); EXPECT_EQ(nr_srs_comb_detect(&g,iq.data(),14*512,0,c),0);
}
TEST_F(SrsBlind,SrsPregateSkipsNoCombEnergy) {
  uint32_t x=9;
  for(auto &v:iq) { x=x*1664525u+1013904223u; v=(x>>20)-2048; }
  EXPECT_FALSE(nr_srs_pregate(&g,iq.data(),14*512,0x3f00));
  generate();
  EXPECT_TRUE(nr_srs_pregate(&g,iq.data(),14*512,0x3f00));
  EXPECT_FALSE(nr_srs_pregate(&g,iq.data(),14*512,0));
}
TEST_F(SrsBlind,SrsBackoffResetsOnCandidate) {
  nr_srs_idle_t idle{};
  for(uint64_t slot=0;slot<64;slot++) {
    if(nr_srs_idle_scan(&idle,slot,false)) nr_srs_idle_result(&idle,false);
  }
  EXPECT_GT(idle.interval,1u);
  EXPECT_GT(idle.backoff_skip,0u);
  nr_srs_idle_result(&idle,true);
  EXPECT_EQ(idle.interval,1u);
  EXPECT_TRUE(nr_srs_idle_scan(&idle,64,false));
}
TEST_F(SrsBlind,SrsBackoffCountsEligibleSlots) {
  nr_srs_idle_t idle{};
  unsigned scans=0;
  for(uint64_t i=0;i<512;i++)
    if(nr_srs_idle_scan(&idle,19+20*i,false)) {scans++;nr_srs_idle_result(&idle,false);}
  EXPECT_EQ(idle.interval,64u);
  EXPECT_GT(scans,8u);
}
TEST_F(SrsBlind,SrsIdleCellCostBounded) {
  nr_srs_idle_t idle{};
  uint64_t work=0;
  uint32_t x=19;
  for(auto &v:iq) {x=x*1664525u+1013904223u;v=(x>>20)-2048;}
  nr_srs_candidate_t candidates[NR_SRS_MAX_CANDIDATES];
  for(uint64_t slot=0;slot<20000;slot++) {
    uint64_t now=slot*500000u;
    if(!nr_srs_cpu_allow(&idle,now,5000000u)) continue;
    if(!nr_srs_idle_scan(&idle,slot,false)) continue;
    if(slot<128) {
      EXPECT_FALSE(nr_srs_pregate(&g,iq.data(),14*512,0x3f00));
      EXPECT_EQ(nr_srs_comb_detect(&g,iq.data(),14*512,0x3f00,candidates),0);
    }
    nr_srs_idle_result(&idle,false);
    nr_srs_cpu_charge(&idle,now,5000000u);
    work+=5000000u;
  }
  EXPECT_LE(work,550000000u); // 5% of ten seconds plus one 50 ms window
  EXPECT_GT(idle.budget_skip,0u);
  EXPECT_GT(idle.backoff_skip,0u);
  EXPECT_EQ(nr_srs_sequence_trials(s),0u);
}
TEST_F(SrsBlind,SrsGroupCsSearchSynth) {
  for(int comb:{2,4,8}) for(int group:{0,17,29}) for(int cs:{0,3,5}) {
    truth.comb=comb; truth.group=group; truth.cs=cs; generate(); auto c=truth;c.group=c.cs=-1;
    ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));
    EXPECT_EQ(c.group,group); EXPECT_EQ(c.cs,cs); EXPECT_GT(c.score,0.95);
  }
}
TEST_F(SrsBlind,SrsPeriodFromRecurrence) {
  auto c=truth;
  nr_srs_observe(s,&c,7); EXPECT_EQ(c.period,0); auto id=c.track_id;
  nr_srs_observe(s,&c,27); EXPECT_EQ(c.period,0);
  nr_srs_observe(s,&c,47); EXPECT_EQ(c.period,0);
  nr_srs_observe(s,&c,67); EXPECT_EQ(c.period,20); EXPECT_EQ(c.slot_offset,7);
  EXPECT_EQ(c.track_id,id);
}
TEST_F(SrsBlind,ConfirmedResourceOnlyOnItsOccasions) {
  auto c=truth;
  for(uint64_t slot:{7,27,47,67}) nr_srs_observe(s,&c,slot);
  ASSERT_TRUE(nr_srs_has_confirmed(s,67));
  nr_srs_candidate_t due[NR_SRS_MAX_TRACKS];
  EXPECT_EQ(nr_srs_confirmed_due(s,68,due),0);
  ASSERT_EQ(nr_srs_confirmed_due(s,87,due),1);
  EXPECT_EQ(due[0].start_rb,truth.start_rb);
  generate();nr_srs_candidate_t detected[NR_SRS_MAX_CANDIDATES];
  ASSERT_GT(nr_srs_comb_detect(&g,iq.data(),14*512,0x3f00,detected),0);
  auto trials=nr_srs_sequence_trials(s);
  EXPECT_TRUE(nr_srs_search_slot(s,iq.data(),14*512,&detected[0],&due[0],87));
  EXPECT_LE(nr_srs_sequence_trials(s)-trials,1u);
  EXPECT_FALSE(nr_srs_has_confirmed(s,10308)); // stale track cannot suppress blind search
  for(uint64_t slot:{87,107,127,147}) nr_srs_confirmed_miss(s,slot,nullptr,0);
  EXPECT_FALSE(nr_srs_has_confirmed(s,147));
}
TEST_F(SrsBlind,IllegalGapNeverSuppressesBlindSearch) {
  auto c=truth;
  for(uint64_t slot:{7,10,13,16}) nr_srs_observe(s,&c,slot);
  EXPECT_FALSE(c.confirmed);
  EXPECT_FALSE(nr_srs_has_confirmed(s,16));
}
TEST_F(SrsBlind,SrsHarvestHintOptional) {
  generate(); auto c=truth;c.group=c.cs=-1;auto hint=truth; hint.group=2;
  ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,&hint)); EXPECT_EQ(c.group,17);
  c.group=c.cs=-1; ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,&truth));
  EXPECT_EQ(c.cs,3);
}
TEST_F(SrsBlind,DisjointResourcesAndInvalidGeometry) {
  generate();
  auto other=truth;other.start_rb=16;other.nb_rb=4;other.comb=4;other.comb_offset=1;
  ASSERT_TRUE(srs_test_generate(&g,&other,iq.data()));
  nr_srs_candidate_t c[NR_SRS_MAX_CANDIDATES];
  int n=nr_srs_comb_detect(&g,iq.data(),14*512,0x3f00,c);
  ASSERT_EQ(n,2);
  EXPECT_EQ(c[0].start_rb,4);EXPECT_EQ(c[1].start_rb,16);
  auto invalid=g;invalid.n_rb=275;
  EXPECT_EQ(nr_srs_comb_detect(&invalid,iq.data(),14*512,0x3f00,c),0);
  EXPECT_EQ(nr_srs_comb_detect(&g,iq.data(),1,0x3f00,c),0);
}
TEST_F(SrsBlind,NonSrsCombRejectedBySequenceSearch) {
  generate();uint32_t x=17;
  for(auto &v:iq) if(v) { x=x*1664525u+1013904223u; v=(x>>18)-8192; }
  auto c=truth;
  EXPECT_FALSE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));
}
TEST_F(SrsBlind,SrsLsEstimateSynth) {
  for(int comb:{2,4,8}) {
    truth.comb=comb; truth.comb_offset=comb-1; generate();
    for(int a=0;a<4;a++) for(int i=0;i<14*512;i++) {
      int j=2*(a*14*512+i);double re=iq[j],im=iq[j+1],gain=0.25+0.125*a;
      iq[j]=lrint(gain*(re*std::cos(0.6*a)-im*std::sin(0.6*a)));
      iq[j+1]=lrint(gain*(re*std::sin(0.6*a)+im*std::cos(0.6*a)));
    }
    auto c=truth;
    std::vector<float> out(4*NR_SRS_MAX_RE*2);
    std::vector<uint16_t> k(NR_SRS_MAX_RE); std::vector<uint8_t> l(NR_SRS_MAX_RE);
    int n=nr_srs_estimate(s,iq.data(),14*512,&c,out.data(),k.data(),l.data());
    ASSERT_EQ(n,8*12*2);
    for(int a=0;a<4;a++) for(int i=0;i<8*12;i++) {
      float amplitude=std::hypot(out[2*(a*n+i)],out[2*(a*n+i)+1]);
      EXPECT_NEAR(20*std::log10(amplitude/(0.25+0.125*a)),0,0.5);
      EXPECT_NEAR(out[2*(a*n+i)],(0.25+0.125*a)*std::cos(0.6*a),0.02);
      EXPECT_NEAR(out[2*(a*n+i)+1],(0.25+0.125*a)*std::sin(0.6*a),0.02);
      EXPECT_EQ(k[i],4*12+i); EXPECT_EQ(l[i],12);
    }
  }
}
#include "nr_srs_export.h"
#include "nr_sense_api.h"
#include <fstream>
#include <unistd.h>
TEST_F(SrsBlind,SrsExportLinksFrameAndDropIsNull) {
  char path[]="/tmp/srs-export-XXXXXX";int fd=mkstemp(path);ASSERT_GE(fd,0);close(fd);
  std::string cfr=std::string(path)+".cfr";
  nr_sense_cfg_t cfg{path,cfr.c_str(),nullptr,1,48,8};
  ASSERT_EQ(nr_sense_open(&cfg),0);nr_sense_test_pause_writer(true);
  std::vector<float> h(4*96*2,0.5);std::vector<uint16_t> k(96);std::vector<uint8_t> l(96,12);
  for(int i=0;i<96;i++) k[i]=48+i;
  nr_srs_capture_t m{};m.abs_slot=100;m.frame=5;m.slot=0;m.pci=42;m.n_ant=4;m.t_mono_ns=123;
  m.reconf=true;m.epoch=7;m.identity_gen=3;m.dedicated_epoch=9;m.cfr_max_re=48;m.ta_est_samples=-7;
  auto c=truth;c.track_id=(1ull<<40)+1;c.comb=8;c.comb_offset=7;c.start_rb=5;
  EXPECT_EQ(nr_srs_export(&c,&m,96,h.data(),k.data(),l.data()),1);
  m.epoch=8;
  EXPECT_EQ(nr_srs_export(&c,&m,96,h.data(),k.data(),l.data()),0);
  nr_sense_test_pause_writer(false);nr_sense_close();
  std::ifstream rec(path);std::string first,second;std::getline(rec,first);std::getline(rec,second);
  EXPECT_NE(first.find("\"source\":\"srs\""),std::string::npos);
  EXPECT_NE(first.find("\"epoch\":7"),std::string::npos);
  EXPECT_NE(first.find("\"dedicated_epoch\":9"),std::string::npos);
  EXPECT_NE(first.find("\"ta_est_samples\":-7"),std::string::npos);
  EXPECT_NE(first.find("\"srs_track_id\":1099511627777"),std::string::npos);
  EXPECT_NE(first.find("\"n_ant\":4"),std::string::npos);
  EXPECT_NE(first.find("\"comb_offset\":3"),std::string::npos);
  EXPECT_NE(first.find("\"n_re\":48"),std::string::npos);
  EXPECT_NE(first.find("\"cfr_frame_id\":1"),std::string::npos);
  EXPECT_NE(first.find("\"rnti\":null"),std::string::npos);
  EXPECT_NE(first.find("\"rx_ts\":null"),std::string::npos);
  EXPECT_NE(second.find("\"cfr_frame_id\":null"),std::string::npos);
  std::ifstream bin(cfr,std::ios::binary);std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(bin),{}};
  ASSERT_GE(bytes.size(),48u);EXPECT_EQ(bytes[16],NR_SENSE_SRC_SRS);EXPECT_EQ(bytes[17],4);
  EXPECT_EQ(bytes[20],48);EXPECT_EQ(bytes[40],7);EXPECT_EQ(bytes[44],3);
  unlink(path);unlink(cfr.c_str());
}
TEST_F(SrsBlind,SrsAperiodicAttribution) {
  auto c=truth;nr_srs_observe(s,&c,102);
  nr_srs_note_trigger(s,0x1234,100,1);
  nr_srs_note_pusch(s,0x1234,100,0,nullptr,nullptr,0);
  nr_srs_attribute(s,&c,102,nullptr,nullptr,0);
  EXPECT_EQ(c.rnti,0x1234); EXPECT_STREQ(c.attribution,"dci_request");
  nr_srs_note_trigger(s,0x4567,101,1);
  nr_srs_note_pusch(s,0x4567,101,0,nullptr,nullptr,0);
  nr_srs_attribute(s,&c,102,nullptr,nullptr,0);
  EXPECT_EQ(c.rnti,0); EXPECT_STREQ(c.attribution,"anonymous");
  nr_srs_attribute(s,&c,400,nullptr,nullptr,0); EXPECT_EQ(c.rnti,0);
}
TEST_F(SrsBlind,SrsDelayAndCfrAttribution) {
  auto c=truth;c.delay_samples=23;
  float h[]={1,0,0,1,-1,0,0,-1};uint16_t k[]={48,49,50,51};
  nr_srs_note_pusch(s,0x1234,100,23,h,k,4);
  nr_srs_attribute(s,&c,102,h,k,4); EXPECT_EQ(c.rnti,0x1234);
  EXPECT_STREQ(c.attribution,"pusch_delay_cfr");
  nr_srs_note_pusch(s,0x4567,100,23,h,k,4);
  nr_srs_attribute(s,&c,102,h,k,4); EXPECT_EQ(c.rnti,0);
}
TEST_F(SrsBlind,HoppingSymbolsDoNotExportWrongReference) {
  generate();auto next=truth;next.n_symbols=1;next.start_symbol=13;next.group=5;
  for(int a=0;a<4;a++) ASSERT_TRUE(srs_test_generate(&g,&next,iq.data()+a*14*512*2));
  auto c=truth;EXPECT_FALSE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));
  c=truth;c.n_symbols=1;ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));EXPECT_EQ(c.group,17);
  c=next;ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));EXPECT_EQ(c.group,5);
}
TEST_F(SrsBlind,IdentityResetClearsAttributionAndTracks) {
  auto c=truth;nr_srs_observe(s,&c,100);auto id=c.track_id;
  nr_srs_note_trigger(s,0x1234,100,1);nr_srs_blind_reset(s);
  nr_srs_attribute(s,&c,101,nullptr,nullptr,0);EXPECT_EQ(c.rnti,0);
  nr_srs_observe(s,&c,101);EXPECT_NE(c.track_id,id);
}
extern "C" bool srs_test_supported(const nr_srs_candidate_t *);
TEST_F(SrsBlind,Comb8UnsupportedLengthsRejected) {
  for(int rb:{28,36,44,52,60,68,76}) {
    auto c=truth;c.comb=8;c.nb_rb=rb;
    EXPECT_FALSE(srs_test_supported(&c)) << rb;
  }
}
TEST_F(SrsBlind,TddGapsNotDeclaredPeriodic) {
  auto c=truth;
  for(uint64_t slot:{7,9,17,19,27,29,37}) {
    nr_srs_observe(s,&c,slot);EXPECT_EQ(c.period,0);
  }
}
TEST_F(SrsBlind,OtherUesPeriodicSrsNotClaimed) {
  auto c=truth;c.delay_samples=23;
  nr_srs_note_trigger(s,0x1234,100,1);
  nr_srs_note_pusch(s,0x1234,100,10,nullptr,nullptr,0);
  nr_srs_attribute(s,&c,102,nullptr,nullptr,0);EXPECT_EQ(c.rnti,0);
}
TEST_F(SrsBlind,SingleSightingNotConfirmed) {
  char path[]="/tmp/srs-single-XXXXXX";int fd=mkstemp(path);ASSERT_GE(fd,0);close(fd);
  nr_sense_cfg_t cfg{path,nullptr,nullptr,2,48,8};ASSERT_EQ(nr_sense_open(&cfg),0);
  auto c=truth;nr_srs_observe(s,&c,100);
  nr_srs_capture_t m{};m.n_ant=4;m.cfr_max_re=48;
  float h[4*8]={};uint16_t k[]={48,49,50,51};uint8_t l[]={12,12,12,12};
  nr_srs_export(&c,&m,4,h,k,l);nr_sense_close();
  std::ifstream rec(path);std::string line;std::getline(rec,line);
  EXPECT_NE(line.find("\"confirmed\":false"),std::string::npos);unlink(path);
}
extern "C" void srs_test_pusch_tp(const nr_srs_grid_t *,const nr_srs_candidate_t *,int16_t *);
TEST_F(SrsBlind,TransformPrecodedPuschDmrsNotSrs) {
  truth.start_symbol=11;truth.n_symbols=1;truth.cs=0;srs_test_pusch_tp(&g,&truth,iq.data());
  // This sequence is shared with transform-precoded PUSCH DM-RS (u=17,v=0).
  auto c=truth;ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));
  nr_srs_note_grant(s,100,(1<<2)|(1<<7)|(1<<11),4,8);
  EXPECT_FALSE(nr_srs_search_slot(s,iq.data(),14*512,&c,nullptr,100));
  EXPECT_TRUE(nr_srs_search_slot(s,iq.data(),14*512,&c,nullptr,101));
}
TEST_F(SrsBlind,ExplicitSourceAndBoundedSearch) {
  EXPECT_FALSE(nr_srs_sources_selected(nullptr));EXPECT_FALSE(nr_srs_sources_selected("all"));
  EXPECT_FALSE(nr_srs_sources_selected("ssb,srs_extra"));EXPECT_TRUE(nr_srs_sources_selected("all, srs"));
  generate();auto c=truth;
  auto before=nr_srs_sequence_trials(s);
  ASSERT_TRUE(nr_srs_search_slot(s,iq.data(),14*512,&c,&truth,100));
  EXPECT_LE(nr_srs_sequence_trials(s)-before,1u);
  EXPECT_FALSE(nr_srs_search_slot(s,iq.data(),14*512,&c,&truth,100));
  uint32_t x=17;for(auto &v:iq) if(v) {x=x*1664525u+1013904223u;v=(x>>18)-8192;}
  EXPECT_FALSE(nr_srs_search_slot(s,iq.data(),14*512,&c,nullptr,101));
  auto failed=nr_srs_sequence_trials(s);EXPECT_LE(failed-before,32u);
  for(int slot=101;slot<109;slot++) EXPECT_FALSE(nr_srs_search_slot(s,iq.data(),14*512,&c,nullptr,slot));
  EXPECT_EQ(nr_srs_sequence_trials(s),failed);
  EXPECT_FALSE(nr_srs_search_slot(s,iq.data(),14*512,&c,nullptr,109));
  EXPECT_GT(nr_srs_sequence_trials(s),failed);
}
extern "C" void srs_test_realistic(const nr_srs_grid_t *,const nr_srs_candidate_t *,int,int,int,int16_t *,float *,uint16_t *,double *);
TEST_F(SrsBlind,SrsDelayMatchesPuschWithin2Samples) {
  truth.cs=0;truth.n_symbols=1;
  float ph[4*48*2];uint16_t pk[48];double pd=0;
  srs_test_realistic(&g,&truth,39,0,0,iq.data(),ph,pk,&pd);
  EXPECT_NEAR(pd,39,1);auto c=truth;
  ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));
  EXPECT_NEAR(c.delay_ambiguity_samples,32,1e-6);
  c.delay_samples=-c.delay_samples;
  nr_srs_note_trigger(s,0x1234,100,1);nr_srs_note_pusch(s,0x1234,100,-lrint(pd),ph,pk,48);
  nr_srs_attribute(s,&c,102,ph,pk,48);
  EXPECT_EQ(c.rnti,0x1234);EXPECT_NEAR(c.delay_samples,-pd,2);EXPECT_EQ(c.cs,0);
}
TEST_F(SrsBlind,SrsPuschCfrAlignOnSameSubcarriers) {
  truth.cs=0;truth.n_symbols=1;
  float ph[4*48*2];uint16_t pk[48];double pd=0;
  srs_test_realistic(&g,&truth,7,3,-7,iq.data(),ph,pk,&pd);
  auto c=truth;ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));
  std::vector<float> h(4*NR_SRS_MAX_RE*2);std::vector<uint16_t> k(NR_SRS_MAX_RE);std::vector<uint8_t> l(NR_SRS_MAX_RE);
  int n=nr_srs_estimate(s,iq.data(),14*512,&c,h.data(),k.data(),l.data());ASSERT_EQ(n,96);
  for(int a=0;a<4;a++) {
    for(int j=0;j<48;j++) nr_sense_ul_to_dl(&ph[2*(a*48+j)],&ph[2*(a*48+j)+1],pk[j],g.first_carrier_offset,512,-7);
    for(int j=0;j<n;j++) nr_sense_ul_to_dl(&h[2*(a*n+j)],&h[2*(a*n+j)+1],k[j],g.first_carrier_offset,512,3);
  }
  for(int a=0;a<4;a++) for(int j=4;j<44;j++) {
    SCOPED_TRACE(testing::Message()<<"ant="<<a<<" pilot="<<j);
    ASSERT_EQ(k[2*j],pk[j]);
    EXPECT_NEAR(h[2*(a*n+2*j)],ph[2*(a*48+j)],.08);
    EXPECT_NEAR(h[2*(a*n+2*j)+1],ph[2*(a*48+j)+1],.08);
  }
}
TEST_F(SrsBlind,CarrierWidthRefreshClearsHistory) {
  auto c=truth;nr_srs_observe(s,&c,100);auto id=c.track_id;
  auto next=g;next.n_rb=20;ASSERT_TRUE(nr_srs_refresh_grid(s,&next));
  nr_srs_observe(s,&c,120);EXPECT_NE(c.track_id,id);
  next.n_rb=275;EXPECT_FALSE(nr_srs_refresh_grid(s,&next));
}
TEST_F(SrsBlind,Comb8RejectedAndCountedWithoutGeneration) {
  nr_srs_grid_t wide{2048,1400,106,4,14,20};auto *search=nr_srs_blind_create(&wide);ASSERT_NE(search,nullptr);
  std::vector<int16_t> data(4*14*2048*2);
  for(int rb:{28,36,44,52,60,68,76}) {
    auto c=truth;c.comb=8;c.nb_rb=rb;
    EXPECT_FALSE(nr_srs_group_cs_search(search,data.data(),14*2048,&c,nullptr));
  }
  EXPECT_EQ(nr_srs_unsupported_lengths(search),7u);EXPECT_EQ(nr_srs_sequence_trials(search),0u);
  nr_srs_blind_destroy(search);
}
TEST_F(SrsBlind,PuschCfrResolvesDelayAliasWithoutTrigger) {
  truth.cs=0;truth.n_symbols=1;
  float ph[4*48*2];uint16_t pk[48];double pd=0;
  srs_test_realistic(&g,&truth,39,0,0,iq.data(),ph,pk,&pd);
  auto c=truth;ASSERT_TRUE(nr_srs_group_cs_search(s,iq.data(),14*512,&c,nullptr));
  std::vector<float> h(4*NR_SRS_MAX_RE*2);std::vector<uint16_t> k(NR_SRS_MAX_RE);std::vector<uint8_t> l(NR_SRS_MAX_RE);
  int n=nr_srs_estimate(s,iq.data(),14*512,&c,h.data(),k.data(),l.data());ASSERT_EQ(n,96);
  c.delay_samples=-c.delay_samples;
  nr_srs_note_pusch(s,0x1234,100,-lrint(pd),ph,pk,48);
  nr_srs_attribute(s,&c,102,h.data(),k.data(),n);
  EXPECT_EQ(c.rnti,0x1234);EXPECT_NEAR(c.delay_samples,-pd,2);EXPECT_EQ(c.cs,0);
}

// Exercise the real SRS estimator and SSB axis against the coordinate helpers
// used by CSI-RS, PDSCH data/DM-RS and PUSCH data/DM-RS producers.
TEST_F(SrsBlind,CfrKabsConventionConsistentAcrossSources) {
  truth.start_rb=12; // the resource crosses FFT bin zero (356 + 144)
  generate();
  auto c=truth;
  std::vector<float> h(4*NR_SRS_MAX_RE*2);
  std::vector<uint16_t> k(NR_SRS_MAX_RE);
  std::vector<uint8_t> l(NR_SRS_MAX_RE);
  const int n=nr_srs_estimate(s,iq.data(),14*512,&c,h.data(),k.data(),l.data());
  ASSERT_EQ(n,192);
  uint32_t ssb[240];
  nr_isac_ssb_k_abs(149,5,g.fft_size,ssb);
  for(int i=0;i<96;i++) {
    const int crb=truth.start_rb+i/12, sc=i%12;
    const auto dl_data=nr_sense_crb_re(crb,sc); // contiguous and sparse RB mapping
    const auto ul_data=nr_sense_crb_re(7+5+i/12,sc); // includes BWP start
    const auto dl_dmrs=nr_sense_crb_re(12,0)+i;
    const int bin=(g.first_carrier_offset+144+i)%g.fft_size;
    const auto ul_dmrs=nr_sense_crb_k(bin,g.first_carrier_offset,g.fft_size);
    EXPECT_EQ(k[i],144+i);
    EXPECT_EQ(k[i],dl_data); EXPECT_EQ(k[i],ul_data);
    EXPECT_EQ(k[i],dl_dmrs); EXPECT_EQ(k[i],ul_dmrs);
    if(sc==0) EXPECT_EQ(k[i],nr_sense_crb_re(crb,0)); // CSI-RS per-RB estimate
    if(i>=5) EXPECT_EQ(k[i],ssb[i-5]); // SSB retains its sub-RB shift
  }
}
