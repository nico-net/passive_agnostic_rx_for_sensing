#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
extern "C" {
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_gating.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_api.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
void __wrap_logRecord_mt(const char *, const char *, int, int, int, const char *, ...) {}
}
namespace {
std::string read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
uint32_t u32(const std::string &b, size_t at) {
  uint32_t v=0;
  for (int j=0;j<4;j++) v |= uint32_t(uint8_t(b[at+j])) << (8*j);
  return v;
}
struct Gating : testing::Test {
  std::string dir, path, config, a0="sense_gating", a1="-O";
  char *argv[3]{};
  void SetUp() override {
    char tmp[]="/tmp/sense_gating_XXXXXX";
    ASSERT_NE(mkdtemp(tmp),nullptr);
    dir=tmp; path=dir+"/frames"; config=dir+"/test.cfg";
  }
  void TearDown() override {
    nr_isac_stop();
    if (uniqCfg) { end_configmodule(uniqCfg); uniqCfg=nullptr; }
    std::filesystem::remove_all(dir);
  }
  void start(int enabled, const char *sources, bool sink=true, int antennas=0) {
    std::ofstream(config) << "sensing={enable=" << enabled << ";sources=\"" << sources << "\";"
      << (sink ? "cfr_path=\""+path+"\";" : "") << "cfr_antennas=" << antennas
      << ";cfr_ring_frames=16;cfr_max_re=64;};";
    argv[0]=a0.data(); argv[1]=a1.data(); argv[2]=config.data();
    uniqCfg=load_configmodule(3,argv,0);
    ASSERT_NE(uniqCfg,nullptr);
    nr_isac_init(); nr_isac_set_rx_antennas(4); nr_isac_start();
  }
  void submit(int source) {
    float iq[4*8*2]; uint32_t k[4], l[4];
    for (int a=0;a<4;a++) for (int i=0;i<8;i++) {
      iq[2*(a*8+i)]=100*a+i; iq[2*(a*8+i)+1]=-100*a-i;
    }
    for (int i=0;i<4;i++) { k[i]=i+12; l[i]=2; }
    nr_isac_submit_cfr_multi(33,0,source,nullptr,iq,4,8,k,l,4,1);
  }
};
}
TEST(SenseCost, MonitorChestAttributionUsesSourceAndSnrGate) {
  EXPECT_FALSE(nr_sense_blind_extra_chest(-1,0));
  EXPECT_FALSE(nr_sense_blind_extra_chest(-1,1));
  EXPECT_TRUE(nr_sense_blind_extra_chest(NR_ISAC_SRC_PDSCH_DMRS,0));
  EXPECT_FALSE(nr_sense_blind_extra_chest(NR_ISAC_SRC_PDSCH_DMRS,1));
  EXPECT_TRUE(nr_sense_blind_extra_chest(NR_ISAC_SRC_PDSCH_DMRS_BLIND,0));
}

TEST_F(Gating, ApiOffNoExtraChannelEstimation) {
  start(0,"pdsch_dmrs,pdsch_data");
  EXPECT_EQ(nr_sense_blind_dmrs_source(),-1);
  EXPECT_FALSE(nr_sense_blind_need_chest(-1,0));
  EXPECT_TRUE(nr_sense_blind_need_chest(-1,1)); // existing receiver SNR gate
  EXPECT_TRUE(nr_sense_blind_skip_capped(-1,1,1));
  EXPECT_FALSE(std::filesystem::exists(path));
}
TEST_F(Gating, PdschDmrsSourceDrivesChest) {
  start(1,"pdsch_dmrs");
#ifdef SENSE_GATING_ON
  EXPECT_EQ(nr_sense_blind_dmrs_source(),NR_ISAC_SRC_PDSCH_DMRS);
  EXPECT_TRUE(nr_sense_blind_need_chest(nr_sense_blind_dmrs_source(),0));
  EXPECT_FALSE(nr_sense_blind_skip_capped(nr_sense_blind_dmrs_source(),1,1));
  submit(nr_sense_blind_dmrs_source());
  nr_isac_stop();
  EXPECT_EQ(u32(read_file(path),20),4u);
#else
  EXPECT_EQ(nr_sense_blind_dmrs_source(),-1);
#endif
}
TEST_F(Gating, DmrsSourceTokenProducesFramesAfterMerge) {
  start(1,"pdsch_dmrs");
#ifdef SENSE_GATING_ON
  const int source=nr_sense_blind_dmrs_source();
  ASSERT_EQ(source,NR_ISAC_SRC_PDSCH_DMRS);
  float iq[2]={1,0}; uint32_t k[1]={12}, l[1]={2};
  nr_sense_rs_meta_t meta{}; meta.rnti=0x1234; meta.start_rb=1; meta.nb_rb=1;
  meta.ports=-1; meta.ssb_index=-1; meta.k_ssb=-1; meta.ta_est_samples=-1;
  nr_isac_submit_cfr_multi_meta(33,0,source,nullptr,iq,1,1,k,l,1,1,&meta);
  nr_isac_stop();
  const auto b=read_file(path);
  ASSERT_GE(b.size(),48u);
  EXPECT_EQ(uint8_t(b[16]),NR_SENSE_SRC_PDSCH_DMRS);
#else
  EXPECT_EQ(nr_sense_blind_dmrs_source(),-1);
#endif
}
TEST_F(Gating, LegacyBlindDmrsSourceStillDrivesChest) {
  start(1,"pdsch_dmrs_blind");
#ifdef SENSE_GATING_ON
  EXPECT_EQ(nr_sense_blind_dmrs_source(),NR_ISAC_SRC_PDSCH_DMRS_BLIND);
  EXPECT_TRUE(nr_sense_blind_need_chest(nr_sense_blind_dmrs_source(),0));
  submit(nr_sense_blind_dmrs_source()); nr_isac_stop();
  const auto b=read_file(path);
  ASSERT_GE(b.size(),48u);
  EXPECT_EQ(uint8_t(b[16]),NR_SENSE_SRC_PDSCH_DMRS);
#else
  EXPECT_EQ(nr_sense_blind_dmrs_source(),-1);
#endif
}
TEST_F(Gating, FourAntennaFrameLayout) {
  start(1,"pdsch_dmrs");
  submit(NR_ISAC_SRC_PDSCH_DMRS); nr_isac_stop();
#ifdef SENSE_GATING_ON
  const auto b=read_file(path);
  ASSERT_EQ(b.size(),48u+12u+4u*4u*8u);
  ASSERT_EQ(uint8_t(b[17]),4);
  for (int a=0;a<4;a++) for (int i=0;i<4;i++) {
    float real, imag;
    const size_t offset=60+4*(a*8+i*2);
    uint32_t bits=u32(b,offset); memcpy(&real,&bits,4);
    bits=u32(b,offset+4); memcpy(&imag,&bits,4);
    EXPECT_FLOAT_EQ(real,100*a+i);
    EXPECT_FLOAT_EQ(imag,-100*a-i);
  }
#else
  EXPECT_FALSE(std::filesystem::exists(path));
#endif
}
TEST_F(Gating, EverySourceReachable) {
  const int src[]={NR_ISAC_SRC_SSB,NR_ISAC_SRC_CSI_RS,NR_ISAC_SRC_PDSCH_DMRS,
    NR_ISAC_SRC_PDSCH_DATA,NR_ISAC_SRC_PUSCH_DMRS,NR_ISAC_SRC_PUSCH_DATA};
  start(1,"ssb,csi_rs,pdsch_dmrs,pdsch_data,pusch_dmrs,pusch_data");
  for (int s:src) {
#ifdef SENSE_GATING_ON
    EXPECT_TRUE(nr_isac_source_enabled(s));
#else
    EXPECT_FALSE(nr_isac_source_enabled(s));
#endif
    submit(s);
  }
  nr_isac_stop();
#ifdef SENSE_GATING_ON
  const auto b=read_file(path);
  ASSERT_EQ(b.size(),6u*(48u+12u+4u*4u*8u));
  const int mapped[]={NR_SENSE_SRC_SSB,NR_SENSE_SRC_CSIRS,NR_SENSE_SRC_PDSCH_DMRS,
    NR_SENSE_SRC_PDSCH_DATA,NR_SENSE_SRC_PUSCH_DMRS,NR_SENSE_SRC_PUSCH_DATA};
  for (int i=0;i<6;i++) {
    EXPECT_EQ(u32(b,i*188+8),unsigned(i+1));
    EXPECT_EQ(uint8_t(b[i*188+16]),mapped[i]);
  }
#else
  EXPECT_FALSE(std::filesystem::exists(path));
#endif
}
TEST_F(Gating, SinklessAndAntennaLimit) {
  start(1,"pdsch_dmrs",false);
#ifdef SENSE_GATING_ON
  EXPECT_TRUE(nr_sense_blind_need_chest(nr_sense_blind_dmrs_source(),0));
#else
  EXPECT_FALSE(nr_sense_blind_need_chest(nr_sense_blind_dmrs_source(),0));
#endif
  submit(NR_ISAC_SRC_PDSCH_DMRS);
  nr_sense_stats_t stats{}; nr_sense_stats(&stats);
#ifdef SENSE_GATING_ON
  EXPECT_EQ(stats.cfr_no_sink,1u);
#endif
}
TEST_F(Gating, ConfiguredSingleAntennaFrame) {
  start(1,"pdsch_dmrs",true,1);
  submit(NR_ISAC_SRC_PDSCH_DMRS); nr_isac_stop();
#ifdef SENSE_GATING_ON
  const auto b=read_file(path);
  ASSERT_EQ(b.size(),48u+12u+4u*8u);
  EXPECT_EQ(uint8_t(b[17]),1);
#else
  EXPECT_FALSE(std::filesystem::exists(path));
#endif
}
int main(int argc, char **argv) {
  unsetenv("ISAC_RECONF"); logInit();
#if T_TRACER
  T_stdout=1;
#endif
  g_log->log_component[PHY].level=OAILOG_WARNING;
  testing::InitGoogleTest(&argc,argv);
  return RUN_ALL_TESTS();
}
