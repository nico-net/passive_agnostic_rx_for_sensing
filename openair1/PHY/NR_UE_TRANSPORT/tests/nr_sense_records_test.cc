#include "nr_sense_records.h"
#include "nr_sense_noise_est.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include "PHY/defs_nr_common.h"
#include "nr_passive_ue_ctx.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"

static std::atomic<bool> test_reconf{false};
static std::atomic<uint32_t> test_epoch{0};
static bool test_ctx_found=false;
static nr_ue_ctx_t test_ctx{};
extern "C" {
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
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

TEST(SenseRecords, PdschLayerFrameKeys)
{
  nr_sense_rs_meta_t m{};
  m.rnti=77; m.ports=1; m.port_mask=2; m.dmrs_type=1; m.dmrs_add_pos=-1; m.dmrs_n_scid=0;
  char single[2048], layer[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DATA,&m,1,single,sizeof single),0);
  EXPECT_EQ(strstr(single,"\"layer\""),nullptr);      // single-layer records are unchanged
  EXPECT_EQ(strstr(single,"\"n_layers\""),nullptr);
  m.layer_idx1=2; m.n_layers=3;
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DATA,&m,1,layer,sizeof layer),0);
  EXPECT_NE(strstr(layer,"\"layer\":1"),nullptr);
  EXPECT_NE(strstr(layer,"\"n_layers\":3"),nullptr);
  EXPECT_NE(strstr(layer,"\"port_mask\":2"),nullptr);
  EXPECT_NE(strstr(layer,"\"source\":\"pdsch_data\""),nullptr);
}

TEST(SenseRecords, PartialTbKeysAreAppendedAndOnlyForPartialRecords)
{
  nr_sense_rs_meta_t m{};
  m.rnti=77; m.ports=1; m.port_mask=1; m.dmrs_type=1; m.dmrs_add_pos=-1; m.dmrs_n_scid=0;
  char whole[2048], part[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DATA,&m,1,whole,sizeof whole),0);
  EXPECT_EQ(strstr(whole,"partial_tb"),nullptr); // a whole-TB record is byte-identical to before
  m.tb_cb_ok=3; m.tb_cb_total=90;
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DATA,&m,1,part,sizeof part),0);
  auto strip=[](std::string x){ size_t a=x.find("\"t_mono_ns\":"); size_t b=x.find(',',a+1); return x.erase(a,b-a); }; // the timestamp differs between two calls
  std::string w=strip(whole), p=strip(part);
  EXPECT_EQ(p.substr(0,w.size()-1),w.substr(0,w.size()-1)); // every old key unchanged, same order; the new ones sit before the closing brace
  EXPECT_NE(strstr(part,",\"partial_tb\":1,\"cb_ok\":3,\"cb_total\":90}"),nullptr);
}

TEST(SenseRecords, DmrsRecordsSayWhichEstimateTheyAre)
{
  nr_sense_rs_meta_t m{};
  m.rnti=77; m.ports=1; m.port_mask=1; m.dmrs_type=1; m.dmrs_add_pos=-1; m.dmrs_n_scid=0;
  char a[2048], b[2048], c[2048], d[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,1,a,sizeof a),0);
  EXPECT_EQ(strstr(a,"cfr_est"),nullptr); // outside sensing mode nothing is added
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,1,b,sizeof b),0);
  EXPECT_NE(strstr(b,",\"cfr_est\":\"own\""),nullptr);
  m.est_dec=true; m.est_sym=2; m.est_layers=2; m.tb_cb_ok=1; m.tb_cb_total=3;
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,1,c,sizeof c),0);
  EXPECT_NE(strstr(c,",\"cfr_est\":\"decoder\",\"est_dmrs_sym\":2,\"est_layers\":2,\"partial_tb\":1,\"cb_ok\":1,\"cb_total\":3}"),nullptr);
  m.est_dec=2; // the first run of a segmented grant / a wider estimate band are labelled, never passed off as the exact decoder estimate
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,1,c,sizeof c),0);
  EXPECT_NE(strstr(c,",\"cfr_est\":\"decoder_seg\",\"est_dmrs_sym\":2"),nullptr);
  m.est_dec=3;
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,1,c,sizeof c),0);
  EXPECT_NE(strstr(c,",\"cfr_est\":\"decoder_band\",\"est_dmrs_sym\":2"),nullptr);
  nr_sense_rs_meta_t n{}; n.rnti=77; n.ports=1; n.port_mask=1; n.dmrs_type=1; n.dmrs_add_pos=-1; n.dmrs_n_scid=0;
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DATA,&n,1,d,sizeof d),0);
  EXPECT_EQ(strstr(d,"cfr_est"),nullptr); // other sources are untouched
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
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

TEST(SenseRecords, AbsSlotHyperFrame)
{
  /* mu=1: 20 slots per frame; frame 0 of hyper-frame 1 follows frame 1023 of hyper-frame 0 */
  EXPECT_EQ(nr_sense_abs_slot_hfn(0,1023,19,20), 1023u*20+19);
  EXPECT_EQ(nr_sense_abs_slot_hfn(1,0,0,20), 1024u*20);
  EXPECT_EQ(nr_sense_abs_slot_hfn(1,0,0,20), nr_sense_abs_slot_hfn(0,1023,19,20)+1);
  /* a deferred caller with the producer counter a few slots ahead: lands on the slot's own un-wrapped value, across the wrap too */
  const int64_t prod=1024*20+3;
  EXPECT_EQ(nr_sense_abs_slot_align(prod,1023,18,20), 1023u*20+18);
  EXPECT_EQ(nr_sense_abs_slot_align(prod,0,2,20), 1024u*20+2);
  EXPECT_EQ(nr_sense_abs_slot_align(5*1024*20+7,17,4,20), 4u*1024*20+17*20+4);
  EXPECT_EQ(nr_sense_abs_slot_align(-1,17,4,20), 17u*20+4);
}

TEST(SenseRecords, RxStateRingAndShifts)
{
  nr_sense_rx_reset();
  nr_sense_rx_note_slot(100,1000000,12.5);
  nr_sense_rx_note_shift(0);                 /* ignored */
  nr_sense_rx_note_shift(-3);
  nr_sense_rx_note_slot(101,1015000,12.5);
  nr_sense_rx_note_shift(5);
  nr_sense_rx_note_slot(102,1030000,-4.0);
  nr_sense_rx_state_t s;
  ASSERT_TRUE(nr_sense_rx_state_lookup(100,&s));
  EXPECT_EQ(s.rx_ts,1000000); EXPECT_EQ(s.shift_count,0u); EXPECT_EQ(s.shift_cum,0); EXPECT_DOUBLE_EQ(s.cfo_hz,12.5);
  ASSERT_TRUE(nr_sense_rx_state_lookup(101,&s));
  EXPECT_EQ(s.shift_count,1u); EXPECT_EQ(s.shift_cum,-3);
  ASSERT_TRUE(nr_sense_rx_state_lookup(102,&s));
  EXPECT_EQ(s.shift_count,2u); EXPECT_EQ(s.shift_cum,2); EXPECT_DOUBLE_EQ(s.cfo_hz,-4.0);
  /* an unknown slot falls back to the latest state, without a timestamp */
  EXPECT_FALSE(nr_sense_rx_state_lookup(5000,&s));
  EXPECT_EQ(s.rx_ts,-1); EXPECT_EQ(s.shift_count,2u); EXPECT_DOUBLE_EQ(s.cfo_hz,-4.0);
  /* a slot one ring-turn later overwrites its entry */
  nr_sense_rx_note_slot(100+1024,2000000,0);
  EXPECT_FALSE(nr_sense_rx_state_lookup(100,&s));
  nr_sense_rx_reset();
}

TEST(SenseRecords, RsCarriesReceiverStateAndDmrsFields)
{
  nr_sense_rs_meta_t m{};
  m.abs_slot=1024ull*20+3; m.rx_ts=123456789012ll; m.ports=2; m.port_mask=0x3; m.dmrs_type=2; m.dmrs_n_scid=1;
  m.rx_state_valid=true; m.rx_cfo_hz=-37.25; m.rx_shift_cum_samples=-12; m.rx_shift_count=4; m.rx_gain_db=38;
  char line[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,1,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"abs_slot\":20483"),nullptr);
  EXPECT_NE(strstr(line,"\"rx_ts\":123456789012"),nullptr);
  EXPECT_NE(strstr(line,"\"rx_cfo_applied_hz\":-37.25"),nullptr);
  EXPECT_NE(strstr(line,"\"rx_shift_cum_samples\":-12,\"rx_shift_count\":4"),nullptr);
  EXPECT_NE(strstr(line,"\"rx_gain_db\":38"),nullptr);
  EXPECT_NE(strstr(line,"\"dir\":\"DL\""),nullptr);
  EXPECT_NE(strstr(line,"\"dmrs_type\":2"),nullptr);   /* DL DM-RS records carry them too */
  EXPECT_NE(strstr(line,"\"dmrs_n_scid\":1"),nullptr);
  EXPECT_NE(strstr(line,"\"ports\":2"),nullptr);
  nr_sense_rs_meta_t u{}; u.ports=-1; u.dmrs_type=-1;
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_SSB,&u,0,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"rx_cfo_applied_hz\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"rx_shift_cum_samples\":null,\"rx_shift_count\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"dmrs_type\":null"),nullptr);
}

TEST(SenseRecords, NoiseEstimateFromSecondDifference)
{
  /* smooth channel (constant + linear phase ramp) plus complex noise of known variance */
  const uint32_t n=4000, nant=2;
  std::vector<float> h(2*nant*n);
  std::vector<uint32_t> k(n), l(n);
  uint32_t seed=1;
  auto gauss=[&]() { double s=0; for (int i=0;i<12;i++) { seed=seed*1664525u+1013904223u; s+=(seed>>8)/16777216.0; } return s-6.0; };
  const double sigma2=0.04;
  for (uint32_t i=0;i<n;i++) { k[i]=i%600; l[i]=i/600; }
  for (uint32_t a=0;a<nant;a++) for (uint32_t i=0;i<n;i++) {
    const double ph=0.02*k[i];
    h[2*(a*n+i)]  =(float)(2.0*cos(ph)+sqrt(sigma2/2)*gauss());
    h[2*(a*n+i)+1]=(float)(2.0*sin(ph)+sqrt(sigma2/2)*gauss());
  }
  const float nv=nr_sense_h_noise_var(h.data(),k.data(),l.data(),n,n,nant);
  EXPECT_NEAR(nv,sigma2,0.15*sigma2);
  /* no three adjacent subcarriers -> unknown, not a placeholder */
  for (uint32_t i=0;i<n;i++) k[i]=3*i;
  EXPECT_TRUE(std::isnan(nr_sense_h_noise_var(h.data(),k.data(),l.data(),n,n,nant)));
}

/* Receiver-measured quality rides every RS record: snr_db is the estimator's DM-RS SNR, null when none (never the -30 floor or a
 * -99 sentinel), noise_var is the real estimate or null (never the old placeholder 1). */
TEST(SenseRecords, RsCarriesMeasuredQuality)
{
  char line[2048];
  for (int src=0; src<NR_SENSE_SRC_COUNT; src++) {
    nr_sense_rs_meta_t m{}; m.noise_var=0.4375f; m.ports=1;
    nr_sense_meta_set_snr(&m,12.5f);
    ASSERT_GT(nr_sense_rs_to_json(nr_sense_src_t(src),&m,1,line,sizeof line),0);
    EXPECT_NE(strstr(line,"\"noise_var\":0.4375,"),nullptr) << src;
    EXPECT_NE(strstr(line,"\"snr_db\":12.5,"),nullptr) << src;
  }
  for (float sentinel : {-30.0f,-99.0f,NAN}) {
    nr_sense_rs_meta_t m{}; m.noise_var=2; nr_sense_meta_set_snr(&m,sentinel);
    ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PUSCH_DMRS,&m,1,line,sizeof line),0);
    EXPECT_NE(strstr(line,"\"snr_db\":null"),nullptr) << sentinel;
    EXPECT_EQ(strstr(line,"\"snr_db\":-"),nullptr) << sentinel;
  }
  nr_sense_rs_meta_t z{};   /* nothing measured: both null */
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DATA,&z,1,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"snr_db\":null"),nullptr);
  EXPECT_NE(strstr(line,"\"noise_var\":null"),nullptr);
  nr_sense_rs_meta_t neg{}; nr_sense_meta_set_snr(&neg,-6.25f);   /* a real negative SNR is a measurement */
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PUSCH_DMRS,&neg,1,line,sizeof line),0);
  EXPECT_NE(strstr(line,"\"snr_db\":-6.25,"),nullptr);
}

/* ---- S3: quality + link context ---- */
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
#include "nr_sense_quality.h"
#include <thread>

namespace {
/* deterministic meta (no clock, no stamp lookups) for byte comparisons */
nr_sense_rs_meta_t s3_meta(uint16_t rnti, int pci)
{
  nr_sense_rs_meta_t m{};
  m.rnti=rnti; m.start_rb=0; m.nb_rb=20; m.n_re=240; m.n_ant=4; m.ports=1; m.ssb_index=-1; m.k_ssb=-1;
  m.noise_var=1.f; m.abs_slot=42; m.frame=4; m.slot=2; m.pci=pci; m.rx_ts=1000; m.t_mono_ns=5000;
  m.l_sym_mask=(1u<<2)|(1u<<3); m.layer_idx1=0; m.stamp_valid=true; m.epoch=1; m.identity_gen=2; m.dedicated_epoch=3;
  m.ta_est_samples=-1; m.dmrs_type=1; m.dmrs_add_pos=-1; m.dmrs_n_scid=0;
  nr_sense_meta_set_snr(&m,15.f);
  return m;
}
/* 4-antenna, 20-PRB DM-RS-like CFR (even k, symbols 2 and 3) at ~20 dB: flat per-(ant,k) channel, tiny deterministic noise */
struct S3Cfr { std::vector<float> iq; std::vector<uint16_t> k; std::vector<uint8_t> l; int n_ant=4, n_re=0; };
S3Cfr s3_cfr(int nprb, float amp)
{
  S3Cfr c;
  for (int s=2;s<=3;s++) for (int p=0;p<nprb;p++) for (int i=0;i<6;i++) { c.k.push_back(12*p+2*i); c.l.push_back(s); }
  c.n_re=c.k.size();
  c.iq.resize(2*c.n_ant*c.n_re);
  for (int a=0;a<c.n_ant;a++) for (int i=0;i<c.n_re;i++) {
    const float th=-2.f*float(M_PI)*2.f*c.k[i]/4096.f+0.7f*a;
    c.iq[2*(a*c.n_re+i)]=amp*std::cos(th); c.iq[2*(a*c.n_re+i)+1]=amp*std::sin(th);
  }
  return c;
}
std::vector<std::string> s3_lines(const char *path)
{
  std::ifstream f(path); std::vector<std::string> v; std::string s;
  while (std::getline(f,s)) v.push_back(s);
  return v;
}
struct S3Sink {
  char rec[40]="/tmp/nr_s3_rec_XXXXXX", cfr[40]="/tmp/nr_s3_cfr_XXXXXX";
  S3Sink(uint32_t ring=8) {
    int a=mkstemp(rec), b=mkstemp(cfr); close(a); close(b);
    nr_sense_cfg_t cfg{}; cfg.records_path=rec; cfg.cfr_path=cfr; cfg.rec_ring_slots=16; cfg.cfr_ring_frames=ring; cfg.cfr_max_re=4096;
    EXPECT_EQ(nr_sense_open(&cfg),0);
  }
  ~S3Sink() { nr_sense_test_pause_writer(false); nr_sense_close(); nr_isac_mode_set(NR_ISAC_MODE_LEGACY); nr_isac_sense_min_prb_set(128); unlink(rec); unlink(cfr); }
};
/* captured from nr_sense_rs_to_json BEFORE the S3 change (pre-S3 binary), same meta */
/* UL and SSB goldens: current formatter output with q_valid=false; that path is textually the pre-S3 code (git diff 5f6733d34d shows only the dir ternary, since reverted, and the appended q_valid block) */
const char *kGoldenLegacyUl = "{\"schema\":2,\"type\":\"rs\",\"epoch\":1,\"identity_gen\":2,\"dedicated_epoch\":3,\"abs_slot\":42,\"frame\":4,\"slot\":2,\"t_mono_ns\":5000,\"rx_ts\":1000,\"pci\":7,\"source\":\"pusch_dmrs\",\"dir\":\"UL\",\"rnti\":17767,\"start_rb\":0,\"nb_rb\":20,\"n_re\":240,\"l_syms\":[2,3],\"ports\":1,\"n_ant\":4,\"port_mask\":null,\"noise_var\":1,\"slot_frac\":0,\"snr_db\":15,\"cfr_frame_id\":3,\"truncated\":false,\"ssb_index\":null,\"k_ssb\":null,\"csirs_resource\":null,\"ta_est_samples\":5,\"k_abs_convention\":\"crb0_point_a\",\"freq_hopping\":null,\"cfo_est_hz\":null,\"rx_cfo_applied_hz\":null,\"rx_shift_cum_samples\":null,\"rx_shift_count\":null,\"rx_gain_db\":null,\"ta_applied_samples\":null,\"transform_precoding\":0,\"dmrs_type\":1,\"dmrs_add_pos\":null,\"dmrs_n_scid\":0,\"ul_timing_ref\":\"dl_frame\",\"preamble\":null,\"energy\":null,\"delay\":null,\"comb\":null,\"offset\":null,\"comb_offset\":null,\"cs\":null,\"group\":null,\"period\":null,\"srs_track_id\":null,\"attribution\":null}";
const char *kGoldenLegacySsb = "{\"schema\":2,\"type\":\"rs\",\"epoch\":1,\"identity_gen\":2,\"dedicated_epoch\":3,\"abs_slot\":42,\"frame\":4,\"slot\":2,\"t_mono_ns\":5000,\"rx_ts\":1000,\"pci\":7,\"source\":\"ssb\",\"dir\":\"DL\",\"rnti\":null,\"start_rb\":0,\"nb_rb\":20,\"n_re\":240,\"l_syms\":[2,3],\"ports\":1,\"n_ant\":4,\"port_mask\":null,\"noise_var\":1,\"slot_frac\":0,\"snr_db\":15,\"cfr_frame_id\":null,\"truncated\":false,\"ssb_index\":2,\"k_ssb\":6,\"csirs_resource\":null,\"ta_est_samples\":null,\"k_abs_convention\":\"crb0_point_a\",\"freq_hopping\":null,\"cfo_est_hz\":null,\"rx_cfo_applied_hz\":null,\"rx_shift_cum_samples\":null,\"rx_shift_count\":null,\"rx_gain_db\":null,\"ta_applied_samples\":null,\"transform_precoding\":null,\"dmrs_type\":1,\"dmrs_add_pos\":null,\"dmrs_n_scid\":0,\"ul_timing_ref\":null,\"preamble\":null,\"energy\":null,\"delay\":null,\"comb\":null,\"offset\":null,\"comb_offset\":null,\"cs\":null,\"group\":null,\"period\":null,\"srs_track_id\":null,\"attribution\":null}";
const char *kGoldenLegacyRs = "{\"schema\":2,\"type\":\"rs\",\"epoch\":1,\"identity_gen\":2,\"dedicated_epoch\":3,\"abs_slot\":42,\"frame\":4,\"slot\":2,\"t_mono_ns\":5000,\"rx_ts\":1000,\"pci\":7,\"source\":\"pdsch_dmrs\",\"dir\":\"DL\",\"rnti\":4660,\"start_rb\":0,\"nb_rb\":20,\"n_re\":240,\"l_syms\":[2,3],\"ports\":1,\"n_ant\":4,\"port_mask\":null,\"noise_var\":1,\"slot_frac\":0,\"snr_db\":15,\"cfr_frame_id\":1,\"truncated\":false,\"ssb_index\":null,\"k_ssb\":null,\"csirs_resource\":null,\"ta_est_samples\":null,\"k_abs_convention\":\"crb0_point_a\",\"freq_hopping\":null,\"cfo_est_hz\":null,\"rx_cfo_applied_hz\":null,\"rx_shift_cum_samples\":null,\"rx_shift_count\":null,\"rx_gain_db\":null,\"ta_applied_samples\":null,\"transform_precoding\":null,\"dmrs_type\":1,\"dmrs_add_pos\":null,\"dmrs_n_scid\":0,\"ul_timing_ref\":null,\"preamble\":null,\"energy\":null,\"delay\":null,\"comb\":null,\"offset\":null,\"comb_offset\":null,\"cs\":null,\"group\":null,\"period\":null,\"srs_track_id\":null,\"attribution\":null}";
}

TEST(SenseRecordsS3, LegacyRecordUnchanged)
{
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  const S3Cfr c=s3_cfr(20,4.f);
  nr_sense_rs_meta_t m=s3_meta(0x1234,7);
  char direct[2048];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,1,direct,sizeof direct),0);
  EXPECT_STREQ(direct,kGoldenLegacyRs);
  { /* UL and SSB variants (same formatter, different branches); not through the writer */
    nr_sense_rs_meta_t u=s3_meta(0x4567,7); u.ta_est_samples=5; u.transform_precoding=0;
    char b[2048];
    ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PUSCH_DMRS,&u,3,b,sizeof b),0);
    EXPECT_STREQ(b,kGoldenLegacyUl);
    nr_sense_rs_meta_t sb=s3_meta(0,7); sb.ssb_index=2; sb.k_ssb=6;
    ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_SSB,&sb,0,b,sizeof b),0);
    EXPECT_STREQ(b,kGoldenLegacySsb);
  }
  { S3Sink sink;
    const uint64_t id=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),42,0,1.f,0,0,nullptr);
    ASSERT_EQ(id,1u);
    nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS,&m,id);
    nr_sense_close();
    const auto v=s3_lines(sink.rec);
    std::string line; for (const auto &s : v) if (s.find("\"type\":\"rs\"")!=std::string::npos) line=s;
    EXPECT_EQ(line.find("valid_cfr_prbs"),std::string::npos);
    EXPECT_EQ(line.find("\"prg_size\""),std::string::npos);
    EXPECT_EQ(line.find("\"tx\""),std::string::npos);
    /* the stored line is the golden with only the cfr_frame_id link changed (1 in both) */
    EXPECT_EQ(line,std::string(kGoldenLegacyRs));
  }
}

TEST(SenseRecordsS3, QualityKeysInSensingMode)
{
  const S3Cfr c=s3_cfr(20,4.f);
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_isac_sense_min_prb_set(10);
  nr_sense_quality_t want; /* the S2 function on the same input is the reference for the numbers */
  nr_sense_quality_compute(c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),1.f,&want);
  ASSERT_EQ(want.valid_cfr_prbs,20);
  S3Sink sink;
  nr_sense_rs_meta_t dl=s3_meta(0x1234,7); dl.prg_valid=true; dl.prg_size=2;
  const uint64_t a=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),42,0,1.f,0,0,nullptr);
  nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS,&dl,a);
  nr_sense_rs_meta_t ul=s3_meta(0x1a2b,7);
  const uint64_t b=nr_sense_push_cfr(NR_SENSE_SRC_PUSCH_DMRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),43,0,1.f,0,0,nullptr);
  nr_sense_emit_rs(NR_SENSE_SRC_PUSCH_DMRS,&ul,b);
  nr_sense_rs_meta_t da=s3_meta(0x1234,7); da.recon_valid=true; da.q.recon_conf=0.75f; da.cfg_ref_ver=4; memset(da.recon_prb_ok,0xff,sizeof da.recon_prb_ok);
  const uint64_t d=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DATA,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),44,0,1.f,0,0,nullptr);
  nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DATA,&da,d);
  nr_isac_sense_min_prb_set(128);
  nr_sense_rs_meta_t nw=s3_meta(0x1234,7);
  const uint64_t e=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),45,0,1.f,0,0,nullptr);
  nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS,&nw,e);
  nr_sense_close();
  std::vector<std::string> rs;
  for (const auto &s : s3_lines(sink.rec)) if (s.find("\"type\":\"rs\"")!=std::string::npos) rs.push_back(s);
  ASSERT_EQ(rs.size(),4u);
  auto has=[](const std::string &s,const char *k){ return s.find(k)!=std::string::npos; };
  EXPECT_TRUE(has(rs[0],"\"valid_cfr_prbs\":20,")) << rs[0];
  EXPECT_TRUE(has(rs[0],"\"allocated_prbs\":20,"));
  EXPECT_TRUE(has(rs[0],"\"estimated_usable_prbs\":20,"));
  EXPECT_TRUE(has(rs[0],"\"first_prb\":0,"));
  EXPECT_TRUE(has(rs[0],"\"valid_mask\":\"ffff0f\""));
  EXPECT_TRUE(has(rs[0],"\"wideband\":true"));
  EXPECT_TRUE(has(rs[0],"\"dir\":\"DL\""));
  EXPECT_FALSE(has(rs[0],"\"dir\":\"dl\""));
  EXPECT_TRUE(has(rs[0],"\"tx\":7,"));
  EXPECT_TRUE(has(rs[0],"\"prg_size\":2"));
  EXPECT_TRUE(has(rs[0],"\"cfg_ref\":null"));
  EXPECT_TRUE(has(rs[0],"\"ssb_beam\":null"));
  EXPECT_TRUE(has(rs[0],"\"recon_conf\":null"));
  char key[64]; snprintf(key,sizeof key,"\"snr_db_p50\":%.9g",want.snr_db_p50);
  EXPECT_TRUE(has(rs[0],key)) << key << "\n" << rs[0];
  snprintf(key,sizeof key,"\"resid_db\":%.9g",want.resid_db);
  EXPECT_TRUE(has(rs[0],key)) << key;
  EXPECT_TRUE(has(rs[0],"\"sym_consist\":")); EXPECT_FALSE(has(rs[0],"\"sym_consist\":null"));
  EXPECT_TRUE(has(rs[0],"\"snr_db_p10\":"));
  EXPECT_TRUE(has(rs[0],"\"layer\"") == false);                 /* layer keys stay absent without a layer frame */
  EXPECT_TRUE(has(rs[1],"\"dir\":\"UL\"")); EXPECT_TRUE(has(rs[1],"\"tx\":\"rnti:1a2b\""));
  EXPECT_TRUE(has(rs[1],"\"prg_size\":null"));
  EXPECT_TRUE(has(rs[2],"\"recon_conf\":0.75")); EXPECT_TRUE(has(rs[2],"\"cfg_ref\":\"2.3.4\""));
  EXPECT_TRUE(has(rs[3],"\"wideband\":false")) << rs[3];
  EXPECT_TRUE(has(rs[3],"\"valid_cfr_prbs\":20,"));
}

TEST(SenseRecordsS3, NoQualityWithoutCfrOrInTraffic)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_rs_meta_t m=s3_meta(0x1234,7);
  char line[4096];
  ASSERT_GT(nr_sense_rs_to_json(NR_SENSE_SRC_PDSCH_DMRS,&m,0,line,sizeof line),0);   /* q_valid false: no keys */
  EXPECT_EQ(strstr(line,"valid_cfr_prbs"),nullptr);
  EXPECT_EQ(strstr(line,"\"tx\""),nullptr);
  EXPECT_NE(strstr(line,"\"dir\":\"DL\""),nullptr);
  nr_isac_mode_set(NR_ISAC_MODE_TRAFFIC);
  const S3Cfr c=s3_cfr(20,4.f);
  S3Sink sink;
  const uint64_t a=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),42,0,1.f,0,0,nullptr);
  nr_sense_quality_t q;
  EXPECT_FALSE(nr_sense_quality_for_frame(a,&q));
  nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS,&m,a);
  nr_sense_close();
  for (const auto &s : s3_lines(sink.rec)) { if (s.find("\"type\":\"rs\"")!=std::string::npos) EXPECT_EQ(s.find("valid_cfr_prbs"),std::string::npos); }
}

TEST(SenseRecordsS3, DirIdenticalWithAndWithoutQuality)
{
  const S3Cfr c=s3_cfr(20,4.f);
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  S3Sink sink;
  for (int src : {NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PUSCH_DMRS}) {
    nr_sense_rs_meta_t m=s3_meta(0x1234,7);
    const uint64_t id=nr_sense_push_cfr(nr_sense_src_t(src),c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),42,0,1.f,0,0,nullptr);
    nr_sense_emit_rs(nr_sense_src_t(src),&m,id);   /* ring hit: quality keys */
    nr_sense_emit_rs(nr_sense_src_t(src),&m,0);    /* no CFR (id 0): no keys */
  }
  nr_sense_close();
  std::vector<std::string> rs;
  for (const auto &s : s3_lines(sink.rec)) { if (s.find("\"type\":\"rs\"")!=std::string::npos) rs.push_back(s); }
  ASSERT_EQ(rs.size(),4u);
  EXPECT_NE(rs[0].find("valid_cfr_prbs"),std::string::npos); EXPECT_EQ(rs[1].find("valid_cfr_prbs"),std::string::npos);
  EXPECT_NE(rs[2].find("valid_cfr_prbs"),std::string::npos); EXPECT_EQ(rs[3].find("valid_cfr_prbs"),std::string::npos);
  for (int i : {0,1}) EXPECT_NE(rs[i].find("\"dir\":\"DL\""),std::string::npos) << rs[i];
  for (int i : {2,3}) EXPECT_NE(rs[i].find("\"dir\":\"UL\""),std::string::npos) << rs[i];
}

TEST(SenseRecordsS3, ReconMaskJoinsPrbValidity)
{
  const S3Cfr c=s3_cfr(20,4.f);
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_isac_sense_min_prb_set(11);
  S3Sink sink;
  nr_sense_rs_meta_t half=s3_meta(0x1234,7); half.recon_valid=true; half.q.recon_conf=0.6f; half.recon_prb_ok[0]=0xff; half.recon_prb_ok[1]=0x03; /* PRB 0..9 pass */
  nr_sense_rs_meta_t none=s3_meta(0x1234,7);   /* no data-aided verdict: validity unchanged */
  for (const nr_sense_rs_meta_t *m : {&half,&none}) {
    const uint64_t id=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DATA,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),42,0,1.f,0,0,nullptr);
    nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DATA,m,id);
  }
  nr_sense_close();
  std::vector<std::string> rs;
  for (const auto &s : s3_lines(sink.rec)) { if (s.find("\"type\":\"rs\"")!=std::string::npos) rs.push_back(s); }
  ASSERT_EQ(rs.size(),2u);
  EXPECT_NE(rs[0].find("\"valid_cfr_prbs\":10,"),std::string::npos) << rs[0];
  EXPECT_NE(rs[0].find("\"valid_mask\":\"ff03\""),std::string::npos);
  EXPECT_NE(rs[0].find("\"allocated_prbs\":20,"),std::string::npos);
  EXPECT_NE(rs[0].find("\"wideband\":false"),std::string::npos) << "wideband follows the validity after (d)";
  EXPECT_NE(rs[0].find("\"recon_conf\":0.600000024"),std::string::npos);
  EXPECT_NE(rs[1].find("\"valid_cfr_prbs\":20,"),std::string::npos);
  EXPECT_NE(rs[1].find("\"wideband\":true"),std::string::npos);
  EXPECT_NE(rs[1].find("\"recon_conf\":null"),std::string::npos);
}

TEST(SenseRecordsS3, JobsQueuedBeforeTheirFrameAreProcessedKeepOrderAndGetQuality)
{
  const S3Cfr c=s3_cfr(20,4.f);
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  S3Sink sink(16);
  nr_sense_test_pause_writer(true);   /* every job and frame is queued before the writer starts */
  for (int i=0;i<6;i++) {
    nr_sense_rs_meta_t m=s3_meta(0x1000+i,7); m.abs_slot=100+i;
    const uint64_t id=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),100+i,0,1.f,0,0,nullptr);
    ASSERT_NE(id,0u);
    nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS,&m,id);
  }
  nr_sense_test_pause_writer(false);
  nr_sense_close();
  std::vector<std::string> rs;
  for (const auto &s : s3_lines(sink.rec)) { if (s.find("\"type\":\"rs\"")!=std::string::npos) rs.push_back(s); }
  ASSERT_EQ(rs.size(),6u);
  for (int i=0;i<6;i++) {
    EXPECT_NE(rs[i].find("\"abs_slot\":"+std::to_string(100+i)+","),std::string::npos) << "order " << i;
    EXPECT_NE(rs[i].find("\"valid_cfr_prbs\":20,"),std::string::npos) << i;
  }
}

TEST(SenseRecordsS3, ProducersNeverBlockWhenTheWriterStalls)
{
  const S3Cfr c=s3_cfr(20,4.f);
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  S3Sink sink(2);
  nr_sense_test_pause_writer(true);
  const auto t0=std::chrono::steady_clock::now();
  for (int i=0;i<300;i++) {
    nr_sense_rs_meta_t m=s3_meta(0x1000,7);
    const uint64_t id=nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),i,0,1.f,0,0,nullptr);
    nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS,&m,id);
  }
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
  nr_sense_stats_t st; nr_sense_stats(&st);
  EXPECT_LT(ms,1000.0) << "producers returned without waiting for the stalled writer";
  EXPECT_GT(st.cfr_dropped[NR_SENSE_SRC_PDSCH_DMRS],0u);   /* drops stay counted: ring semantics unchanged */
  EXPECT_GT(st.rec_dropped,0u);
  nr_sense_test_pause_writer(false);
}

TEST(SenseRecordsS3, JsonFallbackRecordCountsAsQualityMiss)
{
  const S3Cfr c=s3_cfr(20,4.f);
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  S3Sink sink;
  std::string longname(300,'x');   /* >= 256: the binary job path is unavailable, the record is formatted on the pushing thread */
  nr_sense_rs_meta_t m=s3_meta(0,7); m.csirs_resource=longname.c_str();
  const uint64_t id=nr_sense_push_cfr(NR_SENSE_SRC_CSIRS,c.n_ant,c.iq.data(),c.n_re,c.n_re,c.k.data(),c.l.data(),42,0,1.f,0,0,nullptr);
  nr_sense_emit_rs(NR_SENSE_SRC_CSIRS,&m,id);
  nr_sense_stats_t st; nr_sense_stats(&st);
  EXPECT_EQ(st.q_miss,1u);
  nr_sense_close();
}
