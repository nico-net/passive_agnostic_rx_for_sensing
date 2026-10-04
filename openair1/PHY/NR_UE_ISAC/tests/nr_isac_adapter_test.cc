#include <gtest/gtest.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sstream>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_isac.h"
#include "nr_sense_api.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
}
namespace {
std::vector<std::string> warnings;
std::string read(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
uint32_t u32(const std::string &b, size_t p) {
  uint32_t v=0;
  for (int i=0;i<4;i++) v |= uint32_t(uint8_t(b[p+i])) << (8*i);
  return v;
}
struct Adapter : testing::Test {
  std::string dir, cfr, records, config;
  std::string arg0="adapter_test", arg1="-O";
  char *argv[3];
  void SetUp() override {
    char path[]="/tmp/sense_adapter_XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    dir=path; cfr=dir+"/cfr"; records=dir+"/records"; config=dir+"/test.cfg";
    warnings.clear();
  }
  void TearDown() override {
    nr_isac_stop();
    if (uniqCfg) { end_configmodule(uniqCfg); uniqCfg=nullptr; }
    std::filesystem::remove_all(dir);
  }
  void init(const std::string &body) {
    std::ofstream(config) << body;
    argv[0]=arg0.data(); argv[1]=arg1.data(); argv[2]=config.data();
    uniqCfg=load_configmodule(3,argv,0);
    ASSERT_NE(uniqCfg,nullptr);
    nr_isac_init(); nr_isac_start();
  }
  std::string cfg(int enable, const std::string &sources) {
    return "sensing={enable="+std::to_string(enable)+";sources=\""+sources+
      "\";cfr_path=\""+cfr+"\";records_path=\""+records+"\";cfr_ring_frames=16;cfr_max_re=64;};";
  }
  void submit(int source) {
    float iq[4*16*2]; uint32_t k[12], l[12];
    for(int i=0;i<128;i++) iq[i]=i+0.25f;
    for(int i=0;i<12;i++) { k[i]=100+i; l[i]=i%4; }
    nr_isac_carrier_t carrier{106,30000,3500000000,42,20};
    nr_isac_submit_cfr_multi(1234,0.5f,source,&carrier,iq,4,16,k,l,12,2.25f);
  }
};
}
// Link-time hook captures the real LOG_W call, without changing production logging.
extern "C" void __wrap_logRecord_mt(const char *, const char *, int, int, int level, const char *fmt, ...) {
  if(level!=OAILOG_WARNING) return;
  char b[16384]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof(b),fmt,ap); va_end(ap);
  warnings.emplace_back(b);
}
TEST_F(Adapter, SubmitMultiProducesOneFramePerCall) {
  init(cfg(1,"all"));
  const int sources[]={NR_ISAC_SRC_CSI_RS, NR_ISAC_SRC_PDSCH_DATA, NR_ISAC_SRC_PDSCH_DMRS_BLIND,
    NR_ISAC_SRC_PUSCH_DMRS, NR_ISAC_SRC_PUSCH_DATA, NR_ISAC_SRC_SSB, NR_ISAC_SRC_PDSCH_DMRS};
  const int mapped[]={NR_SENSE_SRC_CSIRS,NR_SENSE_SRC_PDSCH_DATA,NR_SENSE_SRC_PDSCH_DMRS,
    NR_SENSE_SRC_PUSCH_DMRS,NR_SENSE_SRC_PUSCH_DATA,NR_SENSE_SRC_SSB,NR_SENSE_SRC_PDSCH_DMRS};
  for(int src:sources) submit(src);
  nr_isac_stop();
#ifdef SENSE_ADAPTER_ON
  auto b=read(cfr);
  constexpr size_t frame_bytes=48+36+4*12*8;
  ASSERT_EQ(b.size(),7*frame_bytes);
  for(int f=0;f<7;f++) {
    size_t p=f*frame_bytes;
    EXPECT_EQ(b.substr(p,4),"NRCF"); EXPECT_EQ(u32(b,p+8),unsigned(f+1));
    EXPECT_EQ(uint8_t(b[p+16]),mapped[f]); EXPECT_EQ(uint8_t(b[p+17]),4);
    EXPECT_EQ(u32(b,p+20),12u); EXPECT_EQ(u32(b,p+24),1234u);
    EXPECT_EQ(u32(b,p+40),0u); EXPECT_EQ(u32(b,p+44),0u);
    for(int a=0;a<4;a++) for(int i=0;i<24;i++) {
      uint32_t bits=u32(b,p+84+4*(a*24+i)); float value; memcpy(&value,&bits,4);
      EXPECT_FLOAT_EQ(value,a*32+i+0.25f);
    }
  }
#else
  EXPECT_FALSE(nr_isac_enabled()); EXPECT_FALSE(std::filesystem::exists(cfr));
#endif
}
TEST_F(Adapter, SourceFilterRespected) {
  init(cfg(1,"ssb,csirs"));
  EXPECT_FALSE(nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA));
  submit(NR_ISAC_SRC_PDSCH_DATA); submit(NR_ISAC_SRC_PDSCH_DMRS_BLIND); submit(NR_ISAC_SRC_SSB);
  nr_isac_stop();
#ifdef SENSE_ADAPTER_ON
  auto b=read(cfr); ASSERT_EQ(b.size(),48u+36u+4*12*8);
  EXPECT_EQ(uint8_t(b[16]),NR_SENSE_SRC_SSB);
#else
  EXPECT_FALSE(std::filesystem::exists(cfr));
#endif
}
TEST_F(Adapter, LegacySensingKeysAcceptedAndIgnored) {
  // Copy first: all edits and parsing are confined to /tmp, never the frozen fixture.
  std::filesystem::copy_file(SENSE_SOURCE_DIR "/tests/passive_rx/adaptive_dl_ul_sensing.conf",config);
  auto body=read(config);
  size_t end=body.rfind("};"); ASSERT_NE(end,std::string::npos);
  body.insert(end,"cfr_path=\""+cfr+"\";records_path=\""+records+"\";cfr_ring_frames=16;cfr_max_re=64;\n");
  // Even the red engine must not create its old outputs outside this test's directory.
  for(const std::string old:{"/tmp/passive_rx/x410_manual_reports.jsonl","/tmp/passive_rx/x410_manual_sensing","tcp://*:5556"}) {
    auto pos=body.find(old); ASSERT_NE(pos,std::string::npos);
    body.replace(pos,old.size(),old=="tcp://*:5556" ? "" : dir+"/legacy");
  }
  init(body);
#ifdef SENSE_ADAPTER_ON
  ASSERT_TRUE(nr_isac_enabled());
  nr_isac_init(); // repeated init must not repeat the compatibility warning
  ASSERT_EQ(warnings.size(),1u);
  for(const char *key:{"cpi_slots","interpolate","capture","sync_correction","nominal_los_range_m",
                      "report_path","out_path","report_endpoint"})
    EXPECT_NE(warnings[0].find(key),std::string::npos) << key;
  EXPECT_EQ(warnings[0].find("pdcch_blind_monitor"),std::string::npos);
  submit(NR_ISAC_SRC_SSB);
  nr_isac_stop(); EXPECT_FALSE(read(cfr).empty());
  EXPECT_NE(read(records).find("\"rx_id\":\"rx_x410\""),std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir+"/legacy"));
#else
  EXPECT_FALSE(nr_isac_enabled()); EXPECT_TRUE(warnings.empty());
#endif
}
TEST_F(Adapter, DisabledApiIsNoop) {
  init(cfg(0,"all"));
  EXPECT_FALSE(nr_isac_enabled());
  for(int src=-1;src<=NR_ISAC_SRC_COUNT;src++) {
    EXPECT_FALSE(nr_isac_source_enabled(src)); submit(src);
    nr_isac_submit_cfr(0,src,nullptr,nullptr,nullptr,nullptr,0,0);
    nr_isac_submit_cfr_at(0,0,src,nullptr,nullptr,nullptr,nullptr,0,0);
  }
  uint32_t re=1; float snr=1;
  EXPECT_EQ(nr_isac_subslot_config(&re,&snr),0u); EXPECT_EQ(re,0u); EXPECT_EQ(snr,0);
  nr_isac_stop(); nr_isac_stop();
  EXPECT_FALSE(std::filesystem::exists(cfr)); EXPECT_FALSE(std::filesystem::exists(records));
}
TEST_F(Adapter, EntireLegacyVocabularyIsInertAndMetadataSurvives) {
  const auto legacy=read(SENSE_SOURCE_DIR "/openair1/PHY/NR_UE_ISAC/tests/legacy_algorithm_keys.cfg");
  ASSERT_FALSE(legacy.empty());
  auto body=cfg(1,"csi_rs");
  body.insert(body.rfind("};"),legacy+"rx_id=\"receiver\";rx_array=\"0,0;0.5,0\";\n");
  init(body);
#ifdef SENSE_ADAPTER_ON
  ASSERT_TRUE(nr_isac_enabled());
  ASSERT_EQ(warnings.size(),1u);
  std::istringstream lines(legacy); std::string line;
  while (std::getline(lines,line)) {
    auto pos=line.find(" = ");
    if(pos!=std::string::npos) { EXPECT_NE(warnings[0].find(line.substr(0,pos)),std::string::npos); }
  }
  EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_CSI_RS));
  EXPECT_FALSE(nr_isac_source_enabled(NR_ISAC_SRC_SSB));
  uint32_t re=1; float snr=1;
  EXPECT_EQ(nr_isac_subslot_config(&re,&snr),0u); EXPECT_EQ(re,0u); EXPECT_EQ(snr,0);
  submit(NR_ISAC_SRC_CSI_RS); nr_isac_stop();
  EXPECT_FALSE(read(cfr).empty());
  EXPECT_NE(read(records).find("\"rx_array\":\"0,0;0.5,0\""),std::string::npos);
  EXPECT_NE(read(records).find("\"reconf\":false"),std::string::npos);
#else
  EXPECT_FALSE(nr_isac_enabled()); EXPECT_FALSE(std::filesystem::exists(cfr));
#endif
}

TEST_F(Adapter, SingleAntennaWrappersAndTruncation) {
  auto body=cfg(1,"pdsch_dmrs");
  body.replace(body.find("cfr_max_re=64"),std::strlen("cfr_max_re=64"),"cfr_max_re=4");
  const auto rec_pos=body.find("records_path=");
  body.erase(rec_pos,body.find(';',rec_pos)-rec_pos+1); // CFR-only export must also open.
  init(body);
#ifdef SENSE_ADAPTER_ON
  EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS));
  EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND));
#endif
  float iq[24]; uint32_t k[12], l[12];
  for(int i=0;i<24;i++) iq[i]=i+0.25f;
  for(int i=0;i<12;i++) { k[i]=100+i; l[i]=i%4; }
  nr_isac_submit_cfr(8,NR_ISAC_SRC_PDSCH_DMRS,nullptr,iq,k,l,12,2);
  nr_isac_submit_cfr_at(9,0.75f,NR_ISAC_SRC_PDSCH_DMRS_BLIND,nullptr,iq,k,l,12,2);
  nr_isac_stop();
#ifdef SENSE_ADAPTER_ON
  auto b=read(cfr);
  constexpr size_t size=48+12+32;
  ASSERT_EQ(b.size(),2*size);
  EXPECT_EQ(u32(b,20),4u); EXPECT_EQ(u32(b,size+20),4u);
  EXPECT_EQ(uint8_t(b[17]),1); EXPECT_EQ(uint8_t(b[size+17]),1);
  EXPECT_EQ(u32(b,32),0u);
  float fraction=0.75f; uint32_t bits; memcpy(&bits,&fraction,4);
  EXPECT_EQ(u32(b,size+32),bits);
  nr_sense_stats_t stats{}; nr_sense_stats(&stats); EXPECT_EQ(stats.truncated,2u);
#else
  EXPECT_FALSE(std::filesystem::exists(cfr));
#endif
}

int main(int argc,char **argv) {
  unsetenv("ISAC_RECONF"); logInit();
#if T_TRACER
  T_stdout = 1;
#endif
  g_log->log_component[PHY].level = OAILOG_WARNING;
  testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS();
}
