#include "nr_traffic_record.h"
#include "nr_traffic_decode.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include "PHY/defs_nr_common.h"
#include "nr_passive_ue_ctx.h"

extern "C" {
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
bool nr_ue_ctx_fast_open=false;
bool nr_cfg_reconf_enabled(void) { return false; }
uint64_t nr_cfg_epoch_cfr_stamp(void) { return 0; }
uint32_t nr_cfg_epoch_dedicated_current(void) { return 0; }
bool nr_ue_ctx_try_get(uint16_t, nr_ue_ctx_t *) { return false; }
}

static nr_sense_dci_rec_t fixture(nr_layout_state_t st)
{
  nr_sense_dci_rec_t r{};
  r.o.abs_slot=42; r.o.t_mono_ns=123; r.o.pci=7; r.o.dir=NR_OBS_DIR_DL; r.o.rnti=0x1234;
  r.o.start_rb=10; r.o.nb_rb=20; r.o.start_sym=2; r.o.nb_sym=12; r.o.mcs=9; r.o.mcs_table=1; r.o.qm=4; r.o.nl=1;
  r.o.tbs=1000; r.o.harq_pid=3; r.o.ndi=1; r.o.rv=0; r.o.crc=-1;
  r.m.epoch=3; r.m.identity_gen=3; r.m.dedicated_epoch=7; r.m.rx_ts=5; r.m.k2=-1; r.m.dci_slot=-1;
  r.m.transform_precoding=-1; r.m.freq_hopping=-1; r.m.dmrs_type=-1; r.m.dmrs_add_pos=-1; r.m.dmrs_n_scid=-1;
  r.m.ta_est_samples=-1; r.m.cfo_est_hz=NAN;
  r.dci_format=1; r.ss_type=1; r.al=4; r.cce=8; r.coreset_id=1; r.dci_len=45;
  const uint8_t b[6]={0xab,0x01,0xff,0x00,0x10,0x80}; memcpy(r.dci_bits,b,6);
  r.rnti_confirmed=true; r.layout_state=st; r.layout_checks_ok=5; r.layout_checks_fail=1; r.cfg_ref_ver=2;
  r.bwp_start=0; r.bwp_size=106; r.start_rb_bwp=10;
  r.vrb_prb=-1; r.tda_index=2; r.k0=0; r.k1=4; r.dai=1; r.tpc=2; r.pri=3; r.srs_req=-1; r.csi_req=-1;
  r.ulsch_ind=-1; r.cbgti=-1; r.retx=0; r.si_ind=-1; r.short_msg_ind=-1; r.short_msg=-1; r.tb_scaling=-1;
  r.pdsch_abs_slot=42; r.harq_fb_abs_slot=46; r.decode=NR_DCI_DECODE_QUEUED;
  return r;
}
static std::string json(const nr_sense_dci_rec_t &r)
{
  char b[4096]; int k=nr_sense_dci_to_json(&r,b,sizeof b); EXPECT_GT(k,0); return std::string(b,k>0?k:0);
}
static bool has(const std::string &s,const char *p){ return s.find(p)!=std::string::npos; }

TEST(TrafficRecord, DciJsonHasEveryGrantKey)
{
  auto r=fixture(NR_LAYOUT_SOLVED);
  char g[2048]; ASSERT_GT(nr_sense_grant_to_json(&r.o,&r.m,g,sizeof g),0);
  std::string d=json(r), gs=g;
  EXPECT_TRUE(has(d,"\"type\":\"dci\""));
  for (size_t p=0;(p=gs.find(",\"",p))!=std::string::npos;) {
    size_t e=gs.find('"',p+2); std::string key=gs.substr(p+1,e-p); /* "key" */
    EXPECT_TRUE(has(d,(key+":").c_str())) << key; p=e;
  }
}
TEST(TrafficRecord, DciJsonNewKeys)
{
  std::string d=json(fixture(NR_LAYOUT_SOLVED));
  for (const char *k : {"\"dci_format\":1","\"al\":4","\"cce\":8","\"dci_len\":45","\"dci_bits\":\"ab01ff001080\"",
       "\"rnti_confirmed\":true","\"layout_state\":\"solved\"","\"cfg_ref\":\"3.7.2\"","\"bwp_start\":0",
       "\"start_rb_bwp\":10","\"k1\":4","\"pri\":3","\"tpc\":2","\"vrb_prb\":null"})
    EXPECT_TRUE(has(d,k)) << k;
}
TEST(TrafficRecord, UnsolvedNullsLayoutFields)
{
  std::string d=json(fixture(NR_LAYOUT_UNSOLVED));
  for (const char *k : {"\"mcs\":null","\"nb_rb\":null","\"k1\":null","\"pdsch_abs_slot\":null","\"dci_bits\":\"ab01ff001080\"",
       "\"rnti\":4660","\"al\":4","\"layout_state\":\"unsolved\""})
    EXPECT_TRUE(has(d,k)) << k;
}
TEST(TrafficRecord, PhysLayoutNullsWhatPhysicsCannotKnow)
{
  auto r = fixture(NR_LAYOUT_SOLVED);
  r.layout_evidence = NR_LAYOUT_EV_PHYS;
  std::string d = json(r);
  for (const char *k : {"\"start_sym\":null", "\"nb_sym\":null", "\"mcs_table\":null", "\"qm\":null", "\"tbs\":null", "\"table_evidence\":null"})
    EXPECT_TRUE(has(d, k)) << k;
  for (const char *k : {"\"mcs\":9", "\"nb_rb\":20", "\"start_rb\":10", "\"k1\":4"}) EXPECT_TRUE(has(d, k)) << k;   // the layout's fields stay
  r.table_evidence = 2;   // Qm consensus with a CRC pass of that table: the table, qm and tbs print, S and L still do not
  d = json(r);
  EXPECT_TRUE(has(d, "\"table_evidence\":\"qm_consensus\""));
  EXPECT_TRUE(has(d, "\"mcs_table\":1"));
  EXPECT_TRUE(has(d, "\"start_sym\":null"));
  r.layout_evidence = NR_LAYOUT_EV_CRC; r.table_evidence = 1;   // CRC: nothing is nulled
  d = json(r);
  EXPECT_TRUE(has(d, "\"table_evidence\":\"crc\""));
  EXPECT_TRUE(has(d, "\"start_sym\":2")); EXPECT_TRUE(has(d, "\"nb_sym\":12")); EXPECT_TRUE(has(d, "\"tbs\":1000"));
}
TEST(TrafficRecord, LayoutEvidenceLabels)
{
  auto r = fixture(NR_LAYOUT_SOLVED);
  EXPECT_TRUE(has(json(r), "\"layout_evidence\":null"));   // unknown by default
  r.layout_evidence = NR_LAYOUT_EV_CRC;  EXPECT_TRUE(has(json(r), "\"layout_evidence\":\"crc\""));
  r.layout_evidence = NR_LAYOUT_EV_BITS; EXPECT_TRUE(has(json(r), "\"layout_evidence\":\"bits\""));
  r.layout_evidence = NR_LAYOUT_EV_PHYS; EXPECT_TRUE(has(json(r), "\"layout_evidence\":\"phys\""));
  EXPECT_TRUE(has(json(r), "\"fields_evidence\":null"));            // separate key
  r.fields_evidence = NR_LAYOUT_EV_BITS; EXPECT_TRUE(has(json(r), "\"fields_evidence\":\"bits\""));
  r.fields_evidence = NR_LAYOUT_EV_CRC;  EXPECT_TRUE(has(json(r), "\"fields_evidence\":\"crc\""));
}
TEST(TrafficRecord, BitsEvidencePrintsHarqAckFieldsUnderCandidate)
{
  auto r = fixture(NR_LAYOUT_CANDIDATE);
  std::string d = json(r);   // no evidence: all null, as before
  for (const char *k : {"\"k1\":null", "\"dai\":null", "\"tpc\":null", "\"pri\":null", "\"mcs\":null"}) EXPECT_TRUE(has(d, k)) << k;
  r.layout_evidence = NR_LAYOUT_EV_BITS; r.fields_evidence = NR_LAYOUT_EV_BITS;   // proven split: the HARQ-ACK fields print, the grant fields still do not
  d = json(r);
  for (const char *k : {"\"k1\":4", "\"dai\":1", "\"tpc\":2", "\"pri\":3", "\"mcs\":null", "\"nb_rb\":null", "\"retx\":null", "\"layout_state\":\"candidate\""})
    EXPECT_TRUE(has(d, k)) << k;
  r.layout_evidence = NR_LAYOUT_EV_CRC; r.fields_evidence = NR_LAYOUT_EV_CRC;    // other evidence does not lift the candidate suppression
  d = json(r);
  EXPECT_TRUE(has(d, "\"k1\":null"));
}
TEST(TrafficRecord, LegacyRecordsCarryNoEvidenceKeys)
{
  // dci records exist only in traffic mode (nr_sense_emit_dci has two callers, traffic_emit_dl/ul, both behind nr_isac_mode() == TRAFFIC); the legacy
  // grant record formatter is untouched by the new keys
  auto r = fixture(NR_LAYOUT_SOLVED);
  char g[2048]; ASSERT_GT(nr_sense_grant_to_json(&r.o, &r.m, g, sizeof g), 0);
  EXPECT_FALSE(has(g, "evidence"));
  EXPECT_FALSE(has(g, "\"type\":\"dci\""));
}
TEST(TrafficRecord, DecodeRecordShape)
{
  struct Reset { ~Reset() { nr_sense_close(); } } reset;
  char path[]="/tmp/nr_traffic_XXXXXX"; int fd=mkstemp(path); ASSERT_GE(fd,0); close(fd);
  nr_sense_cfg_t cfg{}; cfg.records_path=path; cfg.rec_ring_slots=4; cfg.cfr_ring_frames=2; cfg.cfr_max_re=12;
  ASSERT_EQ(nr_sense_open(&cfg),0);
  nr_sense_emit_decode(77,75,0x4567,0,true,1,12.5f,"dci_len");
  nr_sense_emit_decode(78,-1,0x4567,0,false,0,12.5f,NULL);
  nr_sense_close();
  std::ifstream f(path); std::string line; ASSERT_TRUE(std::getline(f,line));
  EXPECT_TRUE(has(line,"\"type\":\"decode\"")); EXPECT_TRUE(has(line,"\"abs_slot\":77")); EXPECT_TRUE(has(line,"\"dci_abs_slot\":75"));
  std::string l2; ASSERT_TRUE(std::getline(f,l2)); EXPECT_TRUE(has(l2,"\"dci_abs_slot\":null"));
  EXPECT_TRUE(has(line,"\"rnti\":17767")); EXPECT_TRUE(has(line,"\"crc\":1")); EXPECT_TRUE(has(line,"\"learned\":\"dci_len\""));
  unlink(path);
}
// E4: a probe the triage skipped changes nothing in the DCI record (the record is written at accept, with the decode verdict "none")
TEST(TrafficRecord, SkippedProbeStillHasItsDciRecord)
{
  auto r=fixture(NR_LAYOUT_UNSOLVED); r.decode=NR_DCI_DECODE_NONE;
  const std::string d=json(r);
  EXPECT_TRUE(has(d,"\"type\":\"dci\"")); EXPECT_TRUE(has(d,"\"rnti\":4660")); EXPECT_TRUE(has(d,"\"dci_bits\":\"ab01ff001080\""));
  EXPECT_FALSE(nr_traffic_probe_wanted(0, 0x1234, 0, 1, 1, false, 40.f, 9, 1));   // the very case: first DCI, no family, no probe
}
TEST(TrafficRecord, CountersSplitByDirection)
{
  nr_traffic_stats_t a, b;
  nr_traffic_stats_get(&a);
  nr_traffic_decode_outcome(1, -1, 0x300, 0, true, 0, 0, 10.f);
  nr_traffic_decode_outcome(1, -1, 0x300, 1, true, 0, 0, 10.f);
  nr_traffic_decode_outcome(1, -1, 0x300, 1, true, 0, 1, 10.f);
  nr_traffic_decode_outcome(1, -1, 0x300, 0, false, 0, 1, 10.f);
  nr_traffic_decode_outcome(1, -1, 0x300, 1, false, 0, 1, 10.f);
  nr_traffic_decode_outcome(1, -1, 0x300, 1, false, 0, 0, 10.f);
  nr_traffic_count(NR_TRAFFIC_CNT_PROBE_SKIP); nr_traffic_count(NR_TRAFFIC_CNT_UL_SKIP);
  nr_traffic_stats_get(&b);
  EXPECT_EQ(b.probe_dl - a.probe_dl, 1u); EXPECT_EQ(b.probe_ul - a.probe_ul, 2u);
  EXPECT_EQ(b.check_dl - a.check_dl, 1u); EXPECT_EQ(b.check_ul - a.check_ul, 2u);
  EXPECT_EQ(b.probe_decodes - a.probe_decodes, 3u); EXPECT_EQ(b.check_decodes - a.check_decodes, 3u);
  EXPECT_EQ(b.probe_skip - a.probe_skip, 1u); EXPECT_EQ(b.ul_decode_skip - a.ul_decode_skip, 1u);
}
TEST(TrafficRecord, BitsUnavailablePrintsNull)
{
  auto r=fixture(NR_LAYOUT_SOLVED); r.dci_len=0;
  std::string d=json(r);
  EXPECT_TRUE(has(d,"\"dci_len\":null")); EXPECT_TRUE(has(d,"\"dci_bits\":null"));
}
TEST(TrafficRecord, DciJsonFitsRecordBytes)
{
  auto r=fixture(NR_LAYOUT_SOLVED);
  memset(r.dci_bits,0xff,sizeof r.dci_bits); r.dci_len=140;
  r.o.abs_slot=INT64_MAX; r.pdsch_abs_slot=INT64_MAX; r.harq_fb_abs_slot=INT64_MAX; r.o.t_mono_ns=UINT64_MAX;
  r.m.verif="SUSPECT"; r.m.ta_applied_valid=true; r.m.ta_applied_samples=-123456; r.m.cfo_est_hz=-12345.678f;
  r.m.ul_timing_ref="dl_frame"; r.o.snr_db=-12.345f; r.o.nvar=1e-9f; r.o.fo_comp_hz=-1234.5f; r.o.delay_samples=-1.25f;
  r.o.carrier_hz=3700000000LL; r.short_msg=255; r.short_msg_ind=3; r.tb_scaling=2; r.si_ind=1;
  EXPECT_LT(json(r).size(), 4096u-1);
}
