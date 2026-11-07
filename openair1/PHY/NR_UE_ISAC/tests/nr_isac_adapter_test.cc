#include <gtest/gtest.h>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sstream>
#include <tuple>
#include <thread>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_isac.h"
#include "nr_sense_api.h"
#include "nr_sense_quality.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
}
namespace {
std::vector<std::string> warnings;
std::string read(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void redirect_legacy_outputs(std::string &body, const std::string &dir) {
  for (const std::string key : {"report_path", "out_path", "report_endpoint"}) {
    size_t pos=body.find(key); ASSERT_NE(pos,std::string::npos);
    size_t first=body.find('"',body.find('=',pos)); ASSERT_NE(first,std::string::npos);
    size_t last=body.find('"',first+1); ASSERT_NE(last,std::string::npos);
    body.replace(first+1,last-first-1,key=="report_endpoint" ? "" : dir+"/legacy");
  }
}
void redirect_path(std::string &body, const std::string &key, const std::string &path) {
  size_t pos=body.find(key+" ="); ASSERT_NE(pos,std::string::npos);
  size_t first=body.find('"',pos); ASSERT_NE(first,std::string::npos);
  size_t last=body.find('"',first+1); ASSERT_NE(last,std::string::npos);
  body.replace(first+1,last-first-1,path);
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
    nr_isac_init(); nr_isac_set_rx_antennas(4); nr_isac_start();
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
  init(cfg(1,"csi_rs,pdsch_dmrs,pdsch_data,pdsch_dmrs_blind,pusch_dmrs,pusch_data,ssb"));
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
  init(cfg(1,"ssb,csi_rs"));
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
TEST_F(Adapter, ExampleConfigEnablesExplicitSources) {
  auto body=read(SENSE_SOURCE_DIR "/tests/passive_rx/sense_api.example.cfg");
  redirect_path(body,"records_path",records);
  redirect_path(body,"cfr_path",cfr);
  init(body);
#ifdef SENSE_ADAPTER_ON
  for (int source=0; source<NR_ISAC_SRC_COUNT; ++source)
    EXPECT_TRUE(nr_isac_source_enabled(source)) << source;
  EXPECT_TRUE(nr_isac_prach_enabled());
  EXPECT_TRUE(warnings.empty());
#else
  EXPECT_FALSE(nr_isac_enabled());
#endif
}
TEST_F(Adapter, PrachSourceFilter) {
  init(cfg(1,"prach"));
#ifdef SENSE_ADAPTER_ON
  EXPECT_TRUE(nr_isac_prach_enabled());
  EXPECT_FALSE(nr_isac_source_enabled(NR_ISAC_SRC_SSB));
  EXPECT_FALSE(nr_isac_source_enabled(NR_ISAC_SRC_PUSCH_DMRS));
#else
  EXPECT_FALSE(nr_isac_prach_enabled());
#endif
}
TEST_F(Adapter, LegacySensingKeysAcceptedAndIgnored) {
  // Copy first: all edits and parsing are confined to /tmp, never the frozen fixture.
  std::filesystem::copy_file(SENSE_SOURCE_DIR "/tests/passive_rx/adaptive_dl_ul_sensing.conf",config);
  auto body=read(config);
  size_t end=body.rfind("};"); ASSERT_NE(end,std::string::npos);
  body.insert(end,"cfr_path=\""+cfr+"\";records_path=\""+records+"\";cfr_ring_frames=16;cfr_max_re=64;\n");
  // Even the red engine must not create its old outputs outside this test's directory.
  redirect_legacy_outputs(body,dir);
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
    if(pos!=std::string::npos && line.find("source = ")!=0 && line.find("subslot_")!=0) {
      EXPECT_NE(warnings[0].find(line.substr(0,pos)),std::string::npos);
    }
  }
  EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_CSI_RS));
  EXPECT_FALSE(nr_isac_source_enabled(NR_ISAC_SRC_SSB));
  uint32_t re=1; float snr=1;
  EXPECT_EQ(nr_isac_subslot_config(&re,&snr),0u); EXPECT_EQ(re,0u); EXPECT_EQ(snr,-999);
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
  EXPECT_FALSE(nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND));
#endif
  float iq[24]; uint32_t k[12], l[12];
  for(int i=0;i<24;i++) iq[i]=i+0.25f;
  for(int i=0;i<12;i++) { k[i]=100+i; l[i]=i%4; }
  nr_isac_submit_cfr(8,NR_ISAC_SRC_PDSCH_DMRS,nullptr,iq,k,l,12,2);
  nr_isac_submit_cfr_at(9,0.75f,NR_ISAC_SRC_PDSCH_DMRS,nullptr,iq,k,l,12,2);
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

TEST_F(Adapter, EnableWithoutSinkKeepsSourcesActive) {
  std::filesystem::copy_file(SENSE_SOURCE_DIR "/tests/passive_rx/adaptive_dl_ul_sensing.conf",config);
  auto body=read(config);
  redirect_legacy_outputs(body,dir);
  init(body);
#ifdef SENSE_ADAPTER_ON
  EXPECT_TRUE(nr_isac_enabled());
  EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_SSB));
  EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DMRS_BLIND));
  submit(NR_ISAC_SRC_SSB);
  nr_sense_stats_t stats{}; nr_sense_stats(&stats);
  EXPECT_EQ(stats.cfr_no_sink,1u);
  EXPECT_EQ(std::count_if(warnings.begin(),warnings.end(),[](const auto &w) { return w.find("no records_path/cfr_path")!=std::string::npos; }),1);
  EXPECT_FALSE(std::filesystem::exists(dir+"/legacy"));
#else
  EXPECT_FALSE(nr_isac_enabled());
#endif
}

TEST_F(Adapter, SourceTokensMatchLegacy) {
  const std::pair<const char *, int> tokens[]={{"csi_rs",NR_ISAC_SRC_CSI_RS},{"pdsch_dmrs",NR_ISAC_SRC_PDSCH_DMRS},
    {"pdsch_data",NR_ISAC_SRC_PDSCH_DATA},{"pusch_dmrs",NR_ISAC_SRC_PUSCH_DMRS},
    {"pusch_data",NR_ISAC_SRC_PUSCH_DATA},{"pdsch_dmrs_blind",NR_ISAC_SRC_PDSCH_DMRS_BLIND},
    {"ssb",NR_ISAC_SRC_SSB}};
  for (auto [token, expected]:tokens) {
    init(cfg(1,token));
#ifdef SENSE_ADAPTER_ON
    for(int src=0;src<NR_ISAC_SRC_COUNT;src++) EXPECT_EQ(nr_isac_source_enabled(src),src==expected) << token;
#endif
    nr_isac_stop(); end_configmodule(uniqCfg); uniqCfg=nullptr;
  }
  for (auto [sources, legacy, expected]: {std::tuple<const char *,const char *,int>{"", "pdsch_data",NR_ISAC_SRC_PDSCH_DATA},
        {"unknown", "pdsch_dmrs",NR_ISAC_SRC_PDSCH_DMRS},{"", "unknown",NR_ISAC_SRC_CSI_RS},
        {"csirs", "pdsch_data",NR_ISAC_SRC_PDSCH_DATA},
        {"all", "pdsch_data",NR_ISAC_SRC_PDSCH_DATA}}) {
    auto body=cfg(1,sources); body.insert(body.rfind("};"),std::string("source=\"")+legacy+"\";");
    init(body);
#ifdef SENSE_ADAPTER_ON
    for(int src=0;src<NR_ISAC_SRC_COUNT;src++) EXPECT_EQ(nr_isac_source_enabled(src),src==expected);
#endif
    nr_isac_stop(); end_configmodule(uniqCfg); uniqCfg=nullptr;
  }
  auto body=cfg(1,"");
  body.erase(body.find("sources="),std::strlen("sources=\"\";"));
  init(body);
#ifdef SENSE_ADAPTER_ON
  EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_CSI_RS));
#endif
}

TEST_F(Adapter, AntennaLimitAndRejectedIndices) {
  auto body=cfg(1,"ssb"); body.insert(body.rfind("};"),"cfr_antennas=1;");
  init(body);
#ifdef SENSE_ADAPTER_ON
  submit(NR_ISAC_SRC_SSB);
  float iq[18]={}; uint32_t good_k[1]={100}, bad_k[1]={65536}, l[1]={0};
  nr_isac_submit_cfr_multi(1,0,NR_ISAC_SRC_SSB,nullptr,iq,9,1,good_k,l,1,0);
  nr_isac_submit_cfr_multi(1,0,NR_ISAC_SRC_SSB,nullptr,iq,1,1,bad_k,l,1,0);
  nr_sense_stats_t stats{}; nr_sense_stats(&stats);
  EXPECT_EQ(stats.cfr_rejected,2u);
  nr_isac_stop(); auto b=read(cfr);
  ASSERT_EQ(b.size(),48u+36u+12u*8u);
  EXPECT_EQ(uint8_t(b[17]),1);
#endif
}

TEST_F(Adapter, SubslotConfigPassesThrough) {
  auto body=cfg(1,"pdsch_data"); body.insert(body.rfind("};"),
    "subslot_symbols=3;subslot_min_re=72;subslot_min_snr_db=4.5;");
  init(body);
#ifdef SENSE_ADAPTER_ON
  uint32_t re=0; float snr=0;
  EXPECT_EQ(nr_isac_subslot_config(&re,&snr),3u);
  EXPECT_EQ(re,72u); EXPECT_FLOAT_EQ(snr,4.5f);
#endif
}

int main(int argc,char **argv) {
  setenv("ISAC_RECONF", "0", 1); logInit();
#if T_TRACER
  T_stdout = 1;
#endif
  g_log->log_component[PHY].level = OAILOG_WARNING;
  testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS();
}

TEST_F(Adapter, NewUlSourcesRequireExplicitTokens) {
  for(const char *tokens:{"", "all", "allsrs", "prachish", "srs", "prach", "all,srs,prach"}) {
    init(cfg(1,tokens));
#ifdef SENSE_ADAPTER_ON
    const std::string t=tokens;
    EXPECT_EQ(bool(nr_isac_source_enabled(NR_ISAC_SRC_SRS)),t=="srs" || t=="all,srs,prach");
    EXPECT_EQ(bool(nr_isac_source_enabled(NR_ISAC_SRC_PRACH)),t=="prach" || t=="all,srs,prach");
    EXPECT_EQ(nr_isac_prach_enabled(),nr_isac_source_enabled(NR_ISAC_SRC_PRACH));
#else
    EXPECT_FALSE(nr_isac_source_enabled(NR_ISAC_SRC_SRS));
    EXPECT_FALSE(nr_isac_prach_enabled());
#endif
    nr_isac_stop();
    end_configmodule(uniqCfg); uniqCfg=nullptr;
  }
}

/* mpci CELLFEAT: one export session per cell, cell 0 on the configured names, cell k on its own files. */
TEST_F(Adapter, SessionsArePerCellWithOwnFiles) {
  init(cfg(1,"ssb,csi_rs"));
#ifdef SENSE_ADAPTER_ON
  constexpr size_t frame_bytes=48+36+4*12*8;
  submit(NR_ISAC_SRC_SSB);
  std::thread cell1([&] {
    nr_cell_bind(1);
    EXPECT_FALSE(nr_isac_enabled()) << "cell 1 has not started its session";
    nr_isac_set_rx_antennas(4); nr_isac_start();
    EXPECT_TRUE(nr_isac_enabled());
    EXPECT_TRUE(nr_isac_source_enabled(NR_ISAC_SRC_SSB));
    submit(NR_ISAC_SRC_SSB); submit(NR_ISAC_SRC_CSI_RS);
    nr_isac_stop();
    EXPECT_FALSE(nr_isac_enabled());
  });
  submit(NR_ISAC_SRC_CSI_RS);
  cell1.join();
  EXPECT_TRUE(nr_isac_enabled()) << "stopping cell 1 must not stop cell 0";
  EXPECT_TRUE(nr_isac_source_configured(NR_ISAC_SRC_SSB)) << "the parsed configuration survives a non-primary stop";
  submit(NR_ISAC_SRC_SSB);
  nr_isac_stop();
  EXPECT_EQ(read(cfr).size(),3*frame_bytes);
  EXPECT_EQ(read(dir+"/cfr.cell1").size(),2*frame_bytes);
  EXPECT_TRUE(std::filesystem::exists(dir+"/records.cell1"));
  EXPECT_NE(read(dir+"/records").find("\"type\":\"session\""),std::string::npos);
  EXPECT_NE(read(dir+"/records.cell1").find("\"type\":\"session\""),std::string::npos);
  EXPECT_FALSE(nr_isac_source_configured(NR_ISAC_SRC_SSB)) << "the primary's stop drops the configuration";
#else
  EXPECT_FALSE(nr_isac_enabled());
#endif
}

/* Production export path (adapter -> records sink): the measured quality reaches the schema-2 rs line. */
TEST_F(Adapter, RsRecordCarriesSnrAndNoiseVar) {
  init(cfg(1,"pusch_dmrs,pdsch_dmrs"));
  float iq[4*16*2]; uint32_t k[12], l[12];
  for(int i=0;i<128;i++) iq[i]=i+0.25f;
  for(int i=0;i<12;i++) { k[i]=100+i; l[i]=2; }
  nr_isac_carrier_t carrier{106,30000,3500000000,42,20};
  nr_sense_rs_meta_t meta{}; meta.rnti=0x4aE8; meta.start_rb=8; meta.nb_rb=1; meta.ports=1; meta.ssb_index=-1; meta.k_ssb=-1;
  meta.ta_est_samples=-1; meta.transform_precoding=0; meta.dmrs_type=1; meta.dmrs_add_pos=-1; meta.dmrs_n_scid=0;
  nr_sense_meta_set_snr(&meta,7.75f);
  nr_isac_submit_cfr_multi_meta(1234,0,NR_ISAC_SRC_PUSCH_DMRS,&carrier,iq,4,16,k,l,12,0.625f,&meta);
  nr_sense_meta_set_snr(&meta,-30.0f);   /* floored estimate: no snr, but the real noise variance stays */
  nr_isac_submit_cfr_multi_meta(1235,0,NR_ISAC_SRC_PDSCH_DMRS,&carrier,iq,4,16,k,l,12,3.5f,&meta);
  nr_isac_stop();
#ifdef SENSE_ADAPTER_ON
  const auto r=read(records);
  const auto a=r.find("\"source\":\"pusch_dmrs\""), b=r.find("\"source\":\"pdsch_dmrs\"");
  ASSERT_NE(a,std::string::npos); ASSERT_NE(b,std::string::npos);
  const auto la=r.substr(a,r.find('\n',a)-a), lb=r.substr(b,r.find('\n',b)-b);
  EXPECT_NE(la.find("\"noise_var\":0.625,"),std::string::npos) << la;
  EXPECT_NE(la.find("\"snr_db\":7.75,"),std::string::npos) << la;
  EXPECT_NE(lb.find("\"noise_var\":3.5,"),std::string::npos) << lb;
  EXPECT_NE(lb.find("\"snr_db\":null"),std::string::npos) << lb;
#else
  EXPECT_FALSE(nr_isac_enabled());
#endif
}

/* Round 2 (I1): through the production source tokens, the DM-RS type reaches the writer's quality. The decoder / tap export submits as PDSCH_DMRS_BLIND when that token is enabled. With the chest's
 * nvar as the SNR reference, the reported SNR is (P - nvar/rho)/(nvar/rho) with rho = 3.29 (type 1) or 1.75 (type 2): the record shows which operator the writer used. */
TEST_F(Adapter, DmrsTypeAndWindowHintsReachTheWriterThroughProductionTokens) {
  init(cfg(1,"pdsch_dmrs,pdsch_dmrs_blind,pdsch_data"));
#ifdef SENSE_ADAPTER_ON
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  float iq[4*24*2]; uint32_t k[24], l[24];
  for(int p=0;p<4;p++) for(int i=0;i<6;i++) { k[6*p+i]=12*p+2*i; l[6*p+i]=2; }
  for(int a=0;a<4;a++) for(int i=0;i<24;i++) { iq[2*(a*24+i)]=std::sqrt(1000.f); iq[2*(a*24+i)+1]=0.f; }
  nr_isac_carrier_t carrier{106,30000,3500000000,42,20};
  nr_sense_rs_meta_t meta{}; meta.rnti=0x4601; meta.start_rb=0; meta.nb_rb=4; meta.ports=1; meta.ssb_index=-1; meta.k_ssb=-1;
  meta.ta_est_samples=-1; meta.transform_precoding=-1; meta.dmrs_add_pos=-1; meta.dmrs_n_scid=0;
  const double rho1=nr_sense_quality_dmrs_rho(1), rho2=nr_sense_quality_dmrs_rho(2);
  const float nvar=float(100.0*rho1);   /* the chest's figure for an H noise variance of 100 (type 1) */
  meta.dmrs_type=2; nr_isac_submit_cfr_multi_meta(2000,0,NR_ISAC_SRC_PDSCH_DMRS_BLIND,&carrier,iq,4,24,k,l,24,nvar,&meta);
  meta.dmrs_type=1; nr_isac_submit_cfr_multi_meta(2001,0,NR_ISAC_SRC_PDSCH_DMRS_BLIND,&carrier,iq,4,24,k,l,24,nvar,&meta);
  meta.dmrs_type=2; nr_isac_submit_cfr_multi_meta(2002,0,NR_ISAC_SRC_PDSCH_DMRS,&carrier,iq,4,24,k,l,24,nvar,&meta);
  /* window rows of a 2-layer grant: one row per PRB (k = 12p+5, one symbol), strong SNR, header noise = the layer's mean row variance */
  float wiq[4*4*2]; uint32_t wk[4], wl[4];
  for(int p=0;p<4;p++) { wk[p]=12*p+5; wl[p]=4; }
  for(int a=0;a<4;a++) for(int p=0;p<4;p++) { wiq[2*(a*4+p)]=30.f; wiq[2*(a*4+p)+1]=0.f; }
  nr_sense_rs_meta_t wm=meta; wm.dmrs_type=1; wm.layer_idx1=1; wm.n_layers=2; wm.qm=4; wm.win_het=0.7f;
  nr_isac_submit_cfr_multi_meta(2003,0,NR_ISAC_SRC_PDSCH_DATA,&carrier,wiq,4,4,wk,wl,4,1.0f,&wm);
  nr_isac_stop();
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  const auto r=read(records);
  std::vector<std::string> dm; std::string data;
  for(size_t p=0;(p=r.find("{\"schema\"",p))!=std::string::npos;) {
    const size_t e=r.find('\n',p); const std::string line=r.substr(p,e-p); p=e;
    if(line.find("\"source\":\"pdsch_dmrs\"")!=std::string::npos) dm.push_back(line);
    if(line.find("\"source\":\"pdsch_data\"")!=std::string::npos) data=line;
  }
  ASSERT_EQ(dm.size(),3u);
  auto snr=[](const std::string &s){ const auto q=s.find("\"snr_db_p50\":"); return q==std::string::npos?NAN:atof(s.c_str()+q+13); };
  const double e1=10*std::log10((1000-nvar/rho1)/(nvar/rho1)), e2=10*std::log10((1000-nvar/rho2)/(nvar/rho2));
  EXPECT_NEAR(snr(dm[0]),e2,0.05) << dm[0];   /* type 2 through the BLIND token: rho(2) */
  EXPECT_NEAR(snr(dm[1]),e1,0.05) << dm[1];   /* type 1 */
  EXPECT_NEAR(snr(dm[2]),e2,0.05) << dm[2];   /* type 2 through the plain token */
  EXPECT_GT(std::fabs(e1-e2),2.0);
  printf("ADAPTER DM-RS snr_db_p50 via BLIND/type2 %.2f (expect %.2f), BLIND/type1 %.2f (%.2f), DMRS/type2 %.2f\n",snr(dm[0]),e2,snr(dm[1]),e1,snr(dm[2]));
  ASSERT_FALSE(data.empty());
  EXPECT_NE(data.find("\"valid_cfr_prbs\":4,"),std::string::npos) << data;    /* window rows: one row per PRB is enough */
  EXPECT_NE(data.find("\"resid_db\":null"),std::string::npos) << data;
#else
  EXPECT_FALSE(nr_isac_enabled());
#endif
}
