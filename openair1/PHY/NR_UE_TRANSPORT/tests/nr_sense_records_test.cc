#include "nr_sense_records.h"
#include <gtest/gtest.h>
#include <cstring>
#include <cmath>
#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>
#include "PHY/defs_nr_common.h"
#include "nr_passive_ue_ctx.h"

static std::atomic<bool> test_reconf{false};
static std::atomic<uint32_t> test_epoch{0};
static bool test_ctx_found=false;
static nr_ue_ctx_t test_ctx{};
extern "C" {
bool nr_ue_ctx_fast_open=false;
bool nr_cfg_reconf_enabled(void) { return test_reconf.load(); }
uint64_t nr_cfg_epoch_cfr_stamp(void) { return uint64_t(test_epoch.load())<<32; }
uint32_t nr_cfg_epoch_dedicated_current(void) { return 0; }
bool nr_ue_ctx_try_get(uint16_t, nr_ue_ctx_t *out) { if (!test_ctx_found) return false; *out=test_ctx; return true; }
}

TEST(SenseRecords, GrantV2IsV1Superset)
{
  nr_passive_obs_t o{};
  o.abs_slot = 42; o.t_mono_ns = 123; o.frame = 4; o.slot = 2; o.pci = 7;
  o.dir = NR_OBS_DIR_DL; o.rnti = 0x1234; o.start_rb = 10; o.nb_rb = 20;
  char v1[1024], v2[2048];
  ASSERT_GT(nr_passive_obs_to_json(&o, v1, sizeof v1), 0);
  nr_sense_grant_meta_t m{};
  m.epoch = 3; m.identity_gen = 2; m.dedicated_epoch = 1;
  m.dl_inline = 1;
  ASSERT_GT(nr_sense_grant_to_json(&o, &m, v2, sizeof v2), 0);
  EXPECT_NE(strstr(v2, "\"schema\":2"), nullptr);
  EXPECT_NE(strstr(v2, "\"type\":\"grant\""), nullptr);
  EXPECT_NE(strstr(v2, "\"abs_slot\":42"), nullptr);
  EXPECT_NE(strstr(v2, "\"dl_coverage\":\"inline\""), nullptr);
  EXPECT_NE(strstr(v2, "\"epoch\":3"), nullptr);
  std::string v1_fields = strchr(v1, ',');
  v1_fields.pop_back();
  EXPECT_NE(std::string(v2).find(v1_fields), std::string::npos);
}

TEST(SenseRecords, UlGrantCarriesUlFields)
{
  nr_passive_obs_t o{};
  o.dir = NR_OBS_DIR_UL; o.rnti = 0x4567;
  nr_sense_grant_meta_t m{};
  m.k2 = 3; m.dci_slot = 7; m.transform_precoding = 1;
  m.freq_hopping = 0; m.dmrs_type = 2; m.dmrs_add_pos = 1; m.dmrs_n_scid = 0;
  m.ta_est_samples = 17; m.cfo_est_hz = 38;
  char line[2048];
  ASSERT_GT(nr_sense_grant_to_json(&o, &m, line, sizeof line), 0);
  EXPECT_NE(strstr(line, "\"k2\":3"), nullptr);
  EXPECT_NE(strstr(line, "\"dci_slot\":7"), nullptr);
  EXPECT_NE(strstr(line, "\"ta_est_samples\":17"), nullptr);
  EXPECT_NE(strstr(line, "\"ul_timing_ref\":\"dl_frame\""), nullptr);
}

TEST(SenseRecords, UlTaSignedAndSweptValue)
{
  nr_passive_obs_t o{}; o.dir=NR_OBS_DIR_UL;
  nr_sense_grant_meta_t m{};
  m.ta_est_samples=-1; /* no per-UE estimate */
  m.ta_applied_valid=true;
  m.ta_applied_samples=nr_sense_ul_ta_applied(1600,-8,4,1);
  char line[2048];
  ASSERT_GT(nr_sense_grant_to_json(&o,&m,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"ta_est_samples\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"ta_applied_samples\":-4"),nullptr);
  EXPECT_EQ(nr_sense_ul_ta_refined(m.ta_applied_samples,3),-7);
  m.ta_applied_samples=0;
  ASSERT_GT(nr_sense_grant_to_json(&o,&m,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"ta_applied_samples\":0"),nullptr);
  nr_sense_rs_meta_t rs{};
  rs.ta_est_samples=-1; rs.ta_applied_samples=-4; rs.ta_applied_valid=true;
  rs.ports=2; rs.port_mask=0x9;
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PUSCH_DMRS,&rs,1,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"ta_applied_samples\":-4"),nullptr);
  EXPECT_NE(strstr(line,"\"ports\":2"),nullptr);
  EXPECT_NE(strstr(line,"\"port_mask\":9"),nullptr);
}

TEST(SenseRecords, UlGrantEpochAtCapture)
{
  test_reconf=true; test_epoch=7;
  nr_sense_grant_meta_t m{};
  nr_sense_capture_grant_stamp(&m);
  test_epoch=8;
  nr_passive_obs_t o{}; o.dir=NR_OBS_DIR_UL;
  char line[2048];
  ASSERT_GT(nr_sense_grant_to_json(&o,&m,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"epoch\":7"),nullptr);
  test_reconf=false; test_epoch=0;
}

TEST(SenseRecords, RsLinksCfrFrame)
{
  char records[]="/tmp/nr_sense_links_rec_XXXXXX";
  char frames[]="/tmp/nr_sense_links_cfr_XXXXXX";
  int rfd=mkstemp(records), cfd=mkstemp(frames);
  ASSERT_GE(rfd,0); ASSERT_GE(cfd,0); close(rfd); close(cfd);
  struct Reset { ~Reset() { nr_sense_test_pause_writer(false); nr_sense_close(); } } reset;
  nr_sense_cfg_t cfg{}; cfg.records_path=records; cfg.cfr_path=frames;
  cfg.rec_ring_slots=4; cfg.cfr_ring_frames=1; cfg.cfr_max_re=1;
  nr_sense_test_pause_writer(true);
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={1,0}; uint16_t k=4; uint8_t l=1;
  const uint64_t first=nr_sense_push_cfr(NR_SENSE_SRC_PUSCH_DMRS,1,iq,1,1,&k,&l,42,0,1,0,0,nullptr);
  const uint64_t dropped=nr_sense_push_cfr(NR_SENSE_SRC_PUSCH_DMRS,1,iq,1,1,&k,&l,42,0,1,0,0,nullptr);
  ASSERT_EQ(first,1u); ASSERT_EQ(dropped,0u);
  nr_sense_rs_meta_t m{};
  m.rnti = 0x1234; m.abs_slot = 42; m.frame = 4; m.slot = 2;
  m.start_rb = 10; m.nb_rb = 20; m.n_re = 1; m.n_ant = 1;
  nr_sense_emit_rs(NR_SENSE_SRC_PUSCH_DMRS,&m,first);
  nr_sense_emit_rs(NR_SENSE_SRC_PUSCH_DMRS,&m,dropped);
  nr_sense_test_pause_writer(false); nr_sense_close();
  std::ifstream f(records); std::string a,b;
  ASSERT_TRUE(std::getline(f,a)); ASSERT_TRUE(std::getline(f,b));
  EXPECT_NE(a.find("\"dir\":\"UL\""),std::string::npos);
  EXPECT_NE(a.find("\"cfr_frame_id\":1"),std::string::npos);
  EXPECT_NE(b.find("\"cfr_frame_id\":null"),std::string::npos);
  unlink(records); unlink(frames);
}

TEST(SenseRecords, DeferredRsFormattingPreservesBytesAndStrings)
{
  char records[]="/tmp/nr_sense_deferred_XXXXXX";
  int fd=mkstemp(records); ASSERT_GE(fd,0); close(fd);
  nr_sense_cfg_t cfg{}; cfg.records_path=records;
  cfg.rec_ring_slots=4; cfg.cfr_ring_frames=1; cfg.cfr_max_re=1;
  nr_sense_test_pause_writer(true);
  ASSERT_EQ(nr_sense_open(&cfg),0);
  char resource[]="row=3,fd=12,l0=2,density=2,period=unknown";
  nr_sense_rs_meta_t m{};
  m.t_mono_ns=123456; m.stamp_valid=true; m.epoch=-1; m.identity_gen=-1;
  m.dedicated_epoch=-1; m.abs_slot=42; m.frame=2; m.slot=2; m.pci=7;
  m.start_rb=10; m.nb_rb=20; m.n_re=4; m.n_ant=4; m.ports=-1;
  m.ssb_index=-1; m.k_ssb=-1; m.csirs_resource=resource;
  m.ta_est_samples=-1; m.transform_precoding=-1; m.dmrs_type=-1;
  m.dmrs_add_pos=-1; m.dmrs_n_scid=-1;
  char expected[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_CSIRS,&m,9,expected,sizeof expected),0);
  nr_sense_emit_rs(NR_SENSE_SRC_CSIRS,&m,9);
  memset(resource,'X',strlen(resource));
  nr_sense_test_pause_writer(false);
  nr_sense_close();
  std::ifstream f(records); std::string line;
  ASSERT_TRUE(std::getline(f,line));
  EXPECT_EQ(line,expected);
  nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_EQ(s.rec_count,1u);
  unlink(records);
}

TEST(SenseRecords, UlRsRecordSelfContained)
{
  nr_sense_rs_meta_t m{};
  m.rnti=77; m.ta_est_samples=63; m.transform_precoding=1;
  m.dmrs_type=2; m.dmrs_add_pos=3; m.dmrs_n_scid=1;
  char line[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PUSCH_DMRS,&m,1,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"ta_est_samples\":63"),nullptr);
  EXPECT_NE(strstr(line,"\"transform_precoding\":1"),nullptr);
  EXPECT_NE(strstr(line,"\"dmrs_type\":2"),nullptr);
  EXPECT_NE(strstr(line,"\"dmrs_n_scid\":1"),nullptr);
}

TEST(SenseRecords, NullsForUnknown)
{
  nr_passive_obs_t o{};
  nr_sense_grant_meta_t m{};
  m.epoch=m.identity_gen=m.dedicated_epoch=-1;
  m.rx_ts=-1; m.k2=m.dci_slot=m.ta_est_samples=-1;
  m.cfo_est_hz=NAN;
  char line[2048];
  ASSERT_GT(nr_sense_grant_to_json(&o,&m,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"epoch\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"identity_gen\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"dedicated_epoch\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"verif\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"rx_ts\":null"),nullptr);
}

TEST(SenseRecords, GrantVerifFromUeContext)
{
  struct Reset { ~Reset() { nr_ue_ctx_fast_open=false; test_ctx_found=false; nr_sense_close(); } } reset;
  char path[]="/tmp/nr_sense_verif_XXXXXX";
  int fd=mkstemp(path); ASSERT_GE(fd,0); close(fd);
  nr_sense_cfg_t cfg{}; cfg.records_path=path; cfg.rec_ring_slots=4;
  cfg.cfr_ring_frames=2; cfg.cfr_max_re=12;
  ASSERT_EQ(nr_sense_open(&cfg),0);
  test_ctx_found=true; test_ctx.cfg[NR_UEP_DCI_LEN_DL].value=47;
  test_ctx.cfg[NR_UEP_DCI_LEN_DL].verif=NR_UEV_HINT;
  nr_ue_ctx_fast_open=true;
  nr_passive_obs_t o{}; o.dir=NR_OBS_DIR_DL; o.rnti=123;
  nr_sense_emit_grant(&o,0);
  nr_sense_close();
  std::ifstream f(path); std::string line;
  ASSERT_TRUE(std::getline(f,line));
  EXPECT_NE(line.find("\"verif\":\"HINT\""),std::string::npos);
  unlink(path);
}

TEST(SenseRecords, EpochStampedAtCapture)
{
  nr_passive_obs_t o{};
  nr_sense_grant_meta_t captured{};
  captured.stamp_valid=true; captured.epoch=5; captured.identity_gen=2; captured.dedicated_epoch=4;
  struct Reset { ~Reset() { test_reconf=false; test_epoch=0; nr_sense_close(); } } reset;
  char path[]="/tmp/nr_sense_epoch_XXXXXX";
  int fd=mkstemp(path); ASSERT_GE(fd,0); close(fd);
  nr_sense_cfg_t cfg{}; cfg.records_path=path; cfg.rec_ring_slots=16;
  cfg.cfr_ring_frames=2; cfg.cfr_max_re=12;
  ASSERT_EQ(nr_sense_open(&cfg),0);
  test_reconf=true; test_epoch=6; /* capture happened at epoch 5 */
  nr_sense_emit_grant_meta(&o,&captured);
  nr_sense_close();
  std::ifstream f(path); std::string line;
  ASSERT_TRUE(std::getline(f,line));
  EXPECT_NE(line.find("\"epoch\":5"),std::string::npos);
  EXPECT_NE(line.find("\"identity_gen\":2"),std::string::npos);
  unlink(path);
}

TEST(SenseRecords, SessionOnGainChange)
{
  char path[]="/tmp/nr_sense_records_XXXXXX";
  int fd=mkstemp(path); ASSERT_GE(fd,0); close(fd);
  nr_sense_cfg_t cfg{}; cfg.records_path=path; cfg.rec_ring_slots=16;
  cfg.cfr_ring_frames=2; cfg.cfr_max_re=12;
  ASSERT_EQ(nr_sense_open(&cfg),0);
  NR_DL_FRAME_PARMS fp{}; fp.N_RB_DL=106; fp.subcarrier_spacing=30000;
  fp.samples_per_subframe=61440; fp.nb_antennas_rx=4;
  nr_sense_emit_session(&fp,30.0);
  nr_sense_emit_session(&fp,33.0);
  nr_sense_close();
  std::ifstream f(path); std::string a,b,c;
  ASSERT_TRUE(std::getline(f,a)); ASSERT_TRUE(std::getline(f,b));
  EXPECT_FALSE(std::getline(f,c));
  EXPECT_NE(a.find("\"rx_gain_db\":30"),std::string::npos);
  EXPECT_NE(b.find("\"rx_gain_db\":33"),std::string::npos);
  unlink(path);
}

TEST(SenseRecords, InlineDlRecorded)
{
  nr_passive_obs_t o{}; o.dir=NR_OBS_DIR_DL;
  nr_sense_grant_meta_t m{}; m.dl_inline=1;
  char line[2048];
  ASSERT_GT(nr_sense_grant_to_json(&o,&m,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"dl_coverage\":\"inline\""),nullptr);
}

TEST(SenseRecords, PrachAndSrsOptionalFields)
{
  nr_sense_rs_meta_t m{};
  m.prach_preamble=12; m.prach_energy=9.5f; m.prach_delay=4.0f;
  char line[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PRACH,&m,0,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"preamble\":12"),nullptr);
  EXPECT_NE(strstr(line,"\"energy\":9.5"),nullptr);
  m.srs_comb=4; m.srs_offset=2; m.srs_cs=3; m.srs_group=11;
  m.srs_period=20; m.srs_track_id=6; m.srs_attribution="delay_match";
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_SRS,&m,0,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"srs_track_id\":6"),nullptr);
  EXPECT_NE(strstr(line,"\"attribution\":\"delay_match\""),nullptr);
}

TEST(SenseRecords, SrsSignedTimingAndCrbOffset)
{
  nr_sense_rs_meta_t m{};
  m.ta_est_samples=-7; m.srs_offset=3;
  char line[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_SRS,&m,0,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"ta_est_samples\":-7"),nullptr);
  EXPECT_NE(strstr(line,"\"comb_offset\":3"),nullptr);
}

TEST(SenseRecords, PrachSrsEpochStampedAtCapture)
{
  struct Reset { ~Reset() { test_reconf=false; test_epoch=0; nr_sense_close(); } } reset;
  char path[]="/tmp/nr_sense_new_ul_epoch_XXXXXX";
  int fd=mkstemp(path); ASSERT_GE(fd,0); close(fd);
  nr_sense_cfg_t cfg{}; cfg.records_path=path; cfg.rec_ring_slots=4;
  cfg.cfr_ring_frames=1; cfg.cfr_max_re=1;
  ASSERT_EQ(nr_sense_open(&cfg),0);
  nr_sense_rs_meta_t m{};
  m.stamp_valid=true; m.epoch=5; m.identity_gen=2; m.dedicated_epoch=4; m.t_mono_ns=123;
  test_reconf=true; test_epoch=6;
  nr_sense_emit_rs(NR_SENSE_SRC_PRACH,&m,0);
  nr_sense_emit_rs(NR_SENSE_SRC_SRS,&m,0);
  nr_sense_close();
  std::ifstream f(path); std::string line;
  for(int i=0;i<2;i++) {
    ASSERT_TRUE(std::getline(f,line));
    EXPECT_NE(line.find("\"epoch\":5"),std::string::npos);
    EXPECT_NE(line.find("\"dedicated_epoch\":4"),std::string::npos);
    EXPECT_NE(line.find("\"t_mono_ns\":123"),std::string::npos);
  }
  unlink(path);
}
