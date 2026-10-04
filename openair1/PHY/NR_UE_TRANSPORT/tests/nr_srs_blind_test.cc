/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <vector>
#include <cmath>
#include "nr_srs_blind.h"
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
  nr_srs_observe(s,&c,67); EXPECT_EQ(c.period,20); EXPECT_EQ(c.slot_offset,7);
  EXPECT_EQ(c.track_id,id);
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
    truth.comb=comb; generate();
    for(int a=0;a<4;a++) for(int i=0;i<14*512*2;i++) iq[a*14*512*2+i]=lrint(iq[a*14*512*2+i]*(0.25+0.125*a));
    auto c=truth;
    std::vector<float> out(4*NR_SRS_MAX_RE*2);
    std::vector<uint16_t> k(NR_SRS_MAX_RE); std::vector<uint8_t> l(NR_SRS_MAX_RE);
    int n=nr_srs_estimate(s,iq.data(),14*512,&c,out.data(),k.data(),l.data());
    ASSERT_EQ(n,8*12*2);
    for(int a=0;a<4;a++) for(int i=8;i<8*12-8;i++) {
      float amplitude=std::hypot(out[2*(a*n+i)],out[2*(a*n+i)+1]);
      EXPECT_NEAR(20*std::log10(amplitude/(0.25+0.125*a)),0,0.5);
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
  m.reconf=true;m.epoch=7;m.identity_gen=3;m.cfr_max_re=48;
  auto c=truth;c.track_id=1;
  EXPECT_EQ(nr_srs_export(&c,&m,96,h.data(),k.data(),l.data()),1);
  m.epoch=8;
  EXPECT_EQ(nr_srs_export(&c,&m,96,h.data(),k.data(),l.data()),0);
  nr_sense_test_pause_writer(false);nr_sense_close();
  std::ifstream rec(path);std::string first,second;std::getline(rec,first);std::getline(rec,second);
  EXPECT_NE(first.find("\"source\":\"srs\""),std::string::npos);
  EXPECT_NE(first.find("\"epoch\":7"),std::string::npos);
  EXPECT_NE(first.find("\"n_ant\":4"),std::string::npos);
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
