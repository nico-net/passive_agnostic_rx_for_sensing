#include <gtest/gtest.h>
#include "nr_passive_ue_ctx.h"
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_reconf_rules.h"
#include <sys/stat.h>
#define NR_BLIND_RNTI_CLASS_TC_FOR_TEST 1
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

static std::atomic<int> reopened_count{0};
extern "C" void nr_cfg_epoch_note_rnti_reopened(uint16_t, bool, uint64_t) { ++reopened_count; }
extern "C" bool nr_cfg_reconf_enabled(void) { return false; }
extern "C" uint32_t nr_cfg_epoch_work_stamp(void) { return 0; }
extern "C" int nr_ue_ctx_test_writer_pause_ms;
extern "C" int64_t nr_ue_ctx_test_file_cap;
static uint64_t nowns() { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }

class UeContext : public ::testing::Test {
 protected:
  std::string path;
  void SetUp() override {
    char name[]="/tmp/rr-uectx-XXXXXX";
    int fd=mkstemp(name); ASSERT_GE(fd,0); close(fd); path=name;
    reopened_count=0;
    ASSERT_TRUE(nr_ue_ctx_open(path.c_str(),128,0));
  }
  void TearDown() override {nr_ue_ctx_close(); unlink(path.c_str());}
  std::string contents() {
    nr_ue_ctx_close();
    std::ifstream f(path); std::ostringstream s; s<<f.rdbuf(); return s.str();
  }
  static int count(const std::string &s,const std::string &pattern) {
    int n=0; size_t pos=0;
    while((pos=s.find(pattern,pos))!=std::string::npos) {++n; pos+=pattern.size();}
    return n;
  }
  static nr_passive_obs_t grant(uint16_t rnti,uint64_t ns,int64_t slot) {
    nr_passive_obs_t o{};
    o.rnti=rnti; o.dir=NR_OBS_DIR_DL; o.abs_slot=slot; o.t_mono_ns=ns;
    o.crc=NR_OBS_CRC_OK; o.mcs=4; o.nl=1; o.nb_rb=20; o.start_rb=10;
    o.tbs=800; o.rnti_class=0; o.snr_db=NAN; o.nvar=NAN;
    o.fo_comp_hz=NAN; o.delay_samples=NAN; return o;
  }
};
TEST_F(UeContext, FirstGrantCreatesFirstSeenThenActive) {
  auto o=grant(0x1234,1000000000,100); nr_ue_ctx_on_obs(&o);
  auto s=contents(); nr_ue_ctx_t c{};
  ASSERT_TRUE(nr_ue_ctx_get(o.rnti,&c)); EXPECT_EQ(c.state,NR_UE_ACTIVE);
  EXPECT_EQ(c.st.grants_dl,1);
  EXPECT_NE(s.find("\"ul\":"),std::string::npos);
  EXPECT_NE(s.find("\"state\":\"FIRST_SEEN\""),std::string::npos);
  EXPECT_NE(s.find("\"state\":\"ACTIVE\""),std::string::npos);
}
TEST_F(UeContext, UlStateUsesDlLockAndRejectsOldIncarnationFeedback) {
  nr_ue_ctx_close();
  ASSERT_TRUE(nr_ue_ctx_open(path.c_str(),128,0));
  nr_ue_ctx_on_param(0x101,NR_UEP_DCI_LEN_DL,52,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_on_param(0x101,NR_UEP_BWP,100,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_on_param(0x102,NR_UEP_DCI_LEN_DL,54,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ul_ticket_t a{},b{};
  for(int i=0;i<200 && (!nr_ue_ctx_ul_ticket(0x101,&a) || !nr_ue_ctx_ul_ticket(0x102,&b));i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(nr_ue_ctx_ul_ticket(0x101,&a));
  ASSERT_TRUE(nr_ue_ctx_ul_ticket(0x102,&b));
  nr_ue_ctx_t prior{};
  for(int i=0;i<200;i++) {
    if(nr_ue_ctx_try_get(0x101,&prior) && prior.ul.field[NR_UL_F_BWP].evidence==NR_UL_EVID_DL_PRIOR)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(prior.ul.field[NR_UL_F_DCI_LEN].value,52);
  EXPECT_EQ(prior.ul.field[NR_UL_F_DCI_LEN].evidence,NR_UL_EVID_DL_PRIOR);
  EXPECT_EQ(prior.ul.field[NR_UL_F_BWP].value,100);
  EXPECT_EQ(prior.ul.field[NR_UL_F_BWP].evidence,NR_UL_EVID_DL_PRIOR);
  nr_ue_ctx_ul_ta_command(0x101,7,1,true);
  nr_ue_ctx_ul_ta_command(0x101,3,1,false);
  nr_ue_ul_update_t ua{}; ua.dci_len=49; ua.layout=3; ua.bwp_start=1; ua.bwp_size=100;
  ua.dmrs_id=71; ua.data_id=81; ua.tda=2; ua.k2=4; ua.pci=415;
  nr_ue_ul_update_t ub=ua; ub.dmrs_id=72; ub.data_id=82;
  nr_ue_ctx_ul_grant(&a,&ua); nr_ue_ctx_ul_grant(&b,&ub);
  nr_ue_ctx_ul_feedback(&a,&ua,true);
  nr_ue_ctx_t before{};
  for(int i=0;i<200;i++) {
    if(nr_ue_ctx_try_get(0x102,&before) && before.ul.field[NR_UL_F_DATA_ID].evidence==NR_UL_EVID_PROVISIONAL)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(before.ul.field[NR_UL_F_DATA_ID].evidence,NR_UL_EVID_PROVISIONAL);
  nr_ue_ctx_t before_a{};
  ASSERT_TRUE(nr_ue_ctx_try_get(0x101,&before_a));
  EXPECT_TRUE(before_a.ul.ta_command_valid);
  EXPECT_EQ(before_a.ul.ta_command_units,10);
  nr_cfg_epoch_snapshot_t bump{3,1,NR_EPOCH_HARD_RESET,NR_CAUSE_SIB1_CHANGE};
  nr_ue_ctx_on_epoch(&bump);
  nr_ue_ctx_on_param(0x101,NR_UEP_DCI_LEN_DL,56,NR_UEV_TRUSTED,NR_UEC_CONVERGED,100);
  nr_ue_ctx_ul_feedback(&a,&ua,true); // delayed verdict must not confirm the reused RNTI
  nr_ue_ctx_close();
  nr_ue_ctx_t reused{},other{};
  ASSERT_TRUE(nr_ue_ctx_get(0x101,&reused));
  ASSERT_TRUE(nr_ue_ctx_get(0x102,&other));
  EXPECT_EQ(reused.identity_gen,1u);
  EXPECT_EQ(reused.ul.field[NR_UL_F_DATA_ID].evidence,NR_UL_EVID_UNKNOWN);
  EXPECT_EQ(other.ul.field[NR_UL_F_DATA_ID].evidence,NR_UL_EVID_UNKNOWN);
  std::ifstream json(path);
  std::ostringstream records; records<<json.rdbuf();
  EXPECT_NE(records.str().find("\"ul\":"),std::string::npos);
}
TEST_F(UeContext, UlStateClearsOnDedicatedBankReverify) {
  nr_ue_ctx_close();
  ASSERT_TRUE(nr_ue_ctx_open(path.c_str(),128,0));
  nr_ue_ctx_on_param(0x103,NR_UEP_DCI_LEN_DL,52,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ul_ticket_t ticket{};
  for(int i=0;i<200 && !nr_ue_ctx_ul_ticket(0x103,&ticket);i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(nr_ue_ctx_ul_ticket(0x103,&ticket));
  nr_ue_ul_update_t update{}; update.dci_len=49; update.data_id=721;
  nr_ue_ctx_ul_feedback(&ticket,&update,true);
  nr_ue_ctx_t before{};
  for(int i=0;i<200;i++) {
    if(nr_ue_ctx_try_get(0x103,&before) && before.ul.field[NR_UL_F_DATA_ID].evidence==NR_UL_EVID_TB_CONFIRMED)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(before.ul.field[NR_UL_F_DATA_ID].evidence,NR_UL_EVID_TB_CONFIRMED);
  nr_cfg_epoch_snapshot_t reverify{2,0,NR_EPOCH_HARD_REVERIFY,NR_CAUSE_DEDICATED_CHANGE_SUSPECTED};
  nr_ue_ctx_on_epoch(&reverify);
  nr_ue_ctx_ul_feedback(&ticket,&update,true);
  nr_ue_ctx_close();
  nr_ue_ctx_t state{};
  ASSERT_TRUE(nr_ue_ctx_get(0x103,&state));
  EXPECT_EQ(state.ul.field[NR_UL_F_DATA_ID].evidence,NR_UL_EVID_UNKNOWN);
  EXPECT_EQ(state.cfg[NR_UEP_DCI_LEN_DL].verif,NR_UEV_HINT);
}
TEST_F(UeContext, UlStateRunsWithoutJsonDestination) {
  nr_ue_ctx_close();
  ASSERT_TRUE(nr_ue_ctx_open(nullptr,128,0));
  nr_ue_ctx_on_param(0x104,NR_UEP_DCI_LEN_DL,51,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_close();
  nr_ue_ctx_t state{};
  ASSERT_TRUE(nr_ue_ctx_get(0x104,&state));
  EXPECT_EQ(state.ul.field[NR_UL_F_DCI_LEN].evidence,NR_UL_EVID_DL_PRIOR);
}
TEST_F(UeContext, IdleAfterTidleGoneAfterTgone) {
  auto o=grant(1,1000000000,1); nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_tick(11000,12000000000ull); nr_ue_ctx_tick(62000,62000000000ull);
  auto s=contents();
  EXPECT_NE(s.find("\"state\":\"IDLE\""),std::string::npos);
  EXPECT_NE(s.find("\"state\":\"GONE\""),std::string::npos);
}
TEST_F(UeContext, RarAnchorOnGoneRntiStartsNewIncarnation) {
  auto o=grant(2,1000000000,1); nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_tick(62000,62000000000ull); nr_ue_ctx_on_anchor(2,1,62001);
  EXPECT_NE(contents().find("\"incarnation\":1"),std::string::npos);
}
TEST_F(UeContext, NsaChurnIsLifecycleNotReconfig) {
  auto o=grant(3,1000000000,1); nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_tick(62000,62000000000ull);
  o.t_mono_ns=63000000000ull; o.abs_slot=63000; nr_ue_ctx_on_obs(&o);
  EXPECT_EQ(count(contents(),"\"type\":\"ue_reconfig\""),0);
}
TEST_F(UeContext, TrustedValueChangeEmitsChangeAndReconfig) {
  nr_ue_ctx_on_param(4,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_on_param(4,NR_UEP_DCI_LEN_DL,53,NR_UEV_TRUSTED,NR_UEC_RELOCK,20);
  auto s=contents();
  EXPECT_NE(s.find("\"old\":47,\"new\":53,\"cause\":\"RELOCK\""),std::string::npos);
  EXPECT_NE(s.find("\"class\":\"DCI_SIZE\""),std::string::npos);
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),1);
}
TEST_F(UeContext, ChangesWithinWindowGroupedAsMixed) {
  nr_ue_ctx_on_param(5,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_on_param(5,NR_UEP_BWP,100,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_on_param(5,NR_UEP_DCI_LEN_DL,53,NR_UEV_TRUSTED,NR_UEC_RELOCK,20);
  nr_ue_ctx_on_param(5,NR_UEP_BWP,200,NR_UEV_TRUSTED,NR_UEC_BWP_CHANGE,20);
  auto s=contents(); EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),1);
  EXPECT_NE(s.find("\"class\":\"MIXED\""),std::string::npos);
}
TEST_F(UeContext, StatisticsNeverEmitReconfig) {
  auto o=grant(6,1000000000,1); nr_ue_ctx_on_obs(&o);
  o.mcs=15; o.nb_rb=40; o.nl=2; o.t_mono_ns+=1000000; o.abs_slot++;
  nr_ue_ctx_on_obs(&o); auto s=contents();
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(6,&c));
  EXPECT_EQ(c.st.mcs_hist[4],1); EXPECT_EQ(c.st.mcs_hist[15],1);
}
TEST_F(UeContext, EpochBumpMakesHintThenReverified) {
  nr_ue_ctx_on_param(7,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_cfg_epoch_snapshot_t bump{1,0,NR_EPOCH_HARD_REVERIFY,NR_CAUSE_SIB1_CHANGE};
  nr_ue_ctx_on_epoch(&bump);
  nr_ue_ctx_on_param(7,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,20);
  auto s=contents(); EXPECT_NE(s.find("\"cause\":\"EPOCH_REVERIFIED\""),std::string::npos);
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
}
TEST_F(UeContext, EpochBumpContradictedEmitsDiscardedAndChange) {
  nr_ue_ctx_on_param(8,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_cfg_epoch_snapshot_t bump{1,0,NR_EPOCH_SOFT,NR_CAUSE_DEDICATED_CHANGE_SUSPECTED};
  nr_ue_ctx_on_epoch(&bump);
  nr_ue_ctx_on_param(8,NR_UEP_DCI_LEN_DL,53,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,20);
  auto s=contents(); EXPECT_NE(s.find("\"cause\":\"EPOCH_DISCARDED\""),std::string::npos);
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
}
TEST_F(UeContext, NarrowSoftKeepsTrustedConfiguration) {
  nr_ue_ctx_on_param(7,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_cfg_epoch_snapshot_t bump{1,0,NR_EPOCH_SOFT,NR_CAUSE_BWP_CHANGE};
  nr_ue_ctx_on_epoch(&bump);
  nr_ue_ctx_on_param(7,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,20);
  const auto s=contents();
  EXPECT_EQ(count(s,"\"cause\":\"EPOCH_REVERIFIED\""),0);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(7,&c));
  EXPECT_EQ(c.cfg[NR_UEP_DCI_LEN_DL].verif,NR_UEV_TRUSTED);
}
TEST_F(UeContext, StateFlapIsNotReconfig) {
  for (auto p : {NR_UEP_TD_STATE, NR_UEP_DCI_LEN_STATE}) {
    nr_ue_ctx_on_param(18,p,1,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
    nr_ue_ctx_on_param(18,p,2,NR_UEV_SUSPECT,NR_UEC_REOPENED_NEW_WINNER,20);
    nr_ue_ctx_on_param(18,p,1,NR_UEV_TRUSTED,NR_UEC_CONVERGED,30);
  }
  auto s=contents();
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
  EXPECT_EQ(reopened_count.load(),0);
}
TEST_F(UeContext, Mixed00And01NoFlapping) {
  nr_ue_ctx_on_dci_accept(21,true,true,true,52,0x100020003,55,10);
  nr_ue_ctx_on_dci_accept(21,false,true,false,34,0x200020003,1,11);
  nr_ue_ctx_on_dci_accept(21,true,true,true,52,0x100020003,55,12);
  auto s=contents(); nr_ue_ctx_t c{};
  ASSERT_TRUE(nr_ue_ctx_get(21,&c));
  EXPECT_EQ(c.cfg[NR_UEP_DCI_LEN_UL].value,52);
  EXPECT_EQ(c.cfg[NR_UEP_CORESET].value,0x100020003);
  EXPECT_EQ(c.cfg[NR_UEP_PDCCH_SCR_ID].value,55);
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
}
TEST_F(UeContext, DciOnAnotherCoresetIsNotAReconfiguration) {
  // TS 38.213 10.1: up to 3 CORESETs per BWP, each with its own PDCCH DM-RS identity. OTA TIM: two alternated 8839 times in 30 min.
  for(uint16_t rnti: {0x2100,0x2101})
    for(int k=0;k<6;k++)
      nr_ue_ctx_on_dci_accept(rnti,true,false,true,47,k&1?0x100d80000:0x100b40024,k&1?1931:1000,100+k);
  auto s=contents();
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
  EXPECT_EQ(reopened_count.load(),0);
  EXPECT_LT(nr_reconf_rrc_effective_slot(0x2100),0); // no inferred RRC window either
}
TEST_F(UeContext, NoEpochNoteWithoutSlot) {
  nr_ue_ctx_on_param(19,NR_UEP_TD_WINNER,1,NR_UEV_TRUSTED,NR_UEC_CONVERGED,-1);
  nr_ue_ctx_on_param(19,NR_UEP_TD_WINNER,2,NR_UEV_TRUSTED,NR_UEC_REOPENED_NEW_WINNER,-1);
  EXPECT_EQ(count(contents(),"\"type\":\"ue_reconfig\""),1);
  EXPECT_EQ(reopened_count.load(),0);
}
TEST_F(UeContext, DciOnlyActivityPreventsGone) {
  const auto start=std::chrono::steady_clock::now();
  const auto ns=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
  auto o=grant(20,ns-59000000000ull,1); nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_on_param(20,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,59001);
  nr_ue_ctx_tick(61000,ns+2000000000ull);
  contents(); nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(20,&c));
  EXPECT_NE(c.state,NR_UE_GONE);
}
TEST_F(UeContext, CsiActivityPreventsGone) {
  const auto ns=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  auto o=grant(22,ns-59000000000ull,1); nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_on_aperiodic_csi(22,1,59001,-1);
  nr_ue_ctx_tick(61000,ns+2000000000ull);
  contents(); nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(22,&c));
  EXPECT_NE(c.state,NR_UE_GONE);
}
TEST_F(UeContext, HardResetClosesOldIdentity) {
  nr_ue_ctx_on_param(9,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_cfg_epoch_snapshot_t bump{1,1,NR_EPOCH_HARD_RESET,NR_CAUSE_CELL_IDENTITY_CHANGE};
  nr_ue_ctx_on_epoch(&bump);
  nr_ue_ctx_on_param(9,NR_UEP_DCI_LEN_DL,53,NR_UEV_TRUSTED,NR_UEC_CONVERGED,20);
  auto s=contents();
  EXPECT_NE(s.find("\"identity_gen\":0,\"rnti\":9,\"incarnation\":0,\"state\":\"GONE\""),std::string::npos);
  EXPECT_NE(s.find("\"identity_gen\":1,\"rnti\":9"),std::string::npos);
}
TEST_F(UeContext, DedicatedChangeSuspectedFromTwoConvergedUes) {
  for(uint16_t rnti: {10,11}) {
    nr_ue_ctx_on_param(rnti,NR_UEP_BWP,100,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
    nr_ue_ctx_on_param(rnti,NR_UEP_BWP,200,NR_UEV_TRUSTED,NR_UEC_BWP_CHANGE,20);
  }
  contents(); EXPECT_EQ(reopened_count.load(),2);
}
TEST_F(UeContext, DirectlyNotifiedReopensAreOnlyRecorded) {
  for(uint16_t rnti: {10,11}) {
    nr_ue_ctx_on_param(rnti,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
    nr_ue_ctx_on_param(rnti,NR_UEP_DCI_LEN_DL,53,NR_UEV_TRUSTED,NR_UEC_RELOCK,20);
  }
  const auto s=contents();
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),2);
  EXPECT_EQ(reopened_count.load(),0);
}
TEST_F(UeContext, JsonRoundTripSchemaV1) {
  nr_ue_ctx_on_param(12,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  auto s=contents();
  EXPECT_NE(s.find("\"schema\":\"uectx/1\""),std::string::npos);
  EXPECT_NE(s.find("\"SIB1_HASH\":null"),std::string::npos);
  EXPECT_NE(s.find("\"SIB1_BWP\":null"),std::string::npos);
  EXPECT_NE(s.find("\"SIB1_TDRA_HASH\":null"),std::string::npos);
  EXPECT_NE(s.find("\"APERIODIC_CSI\":null"),std::string::npos);
  EXPECT_EQ(count(s,"\"type\":\"ue_change\""),1);
  const std::string parse="python3 -c 'import json,sys; [json.loads(line) for line in open(sys.argv[1])]' "+path;
  EXPECT_EQ(std::system(parse.c_str()),0);
}
TEST_F(UeContext, WriterFixtureForOfflineTool) {
  nr_ue_ctx_on_param(0x1234,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_on_param(0x1234,NR_UEP_DCI_LEN_DL,53,NR_UEV_TRUSTED,NR_UEC_RELOCK,20);
  auto s=contents();
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),1);
  if (const char *fixture=std::getenv("ISAC_UECTX_FIXTURE_PATH")) {
    std::ofstream out(fixture);
    out << s;
    ASSERT_TRUE(out.good());
  }
}
TEST_F(UeContext, Sib1ParametersRecordedWhenDecoded) {
  nr_ue_ctx_on_sib1(0x1234,10);
  nr_ue_ctx_on_sib1_param(NR_UEP_SIB1_BWP,100,10);
  nr_ue_ctx_on_sib1_param(NR_UEP_SIB1_TDRA_HASH,0x5678,10);
  auto o=grant(15,1000000000,11); nr_ue_ctx_on_obs(&o);
  auto s=contents();
  EXPECT_NE(s.find("\"SIB1_HASH\":{\"value\":4660"),std::string::npos);
  EXPECT_NE(s.find("\"SIB1_BWP\":{\"value\":100"),std::string::npos);
  EXPECT_NE(s.find("\"SIB1_TDRA_HASH\":{\"value\":22136"),std::string::npos);
}
TEST_F(UeContext, CoresetRemovalMarksSuspectWithoutReconfig) {
  nr_ue_ctx_on_param(16,NR_UEP_CORESET,123,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  nr_ue_ctx_on_param(16,NR_UEP_CORESET,123,NR_UEV_SUSPECT,NR_UEC_CORESET_CHANGE,20);
  auto s=contents(); nr_ue_ctx_t c{};
  ASSERT_TRUE(nr_ue_ctx_get(16,&c));
  EXPECT_EQ(c.cfg[NR_UEP_CORESET].verif,NR_UEV_SUSPECT);
  EXPECT_EQ(count(s,"\"param\":\"CORESET\""),2);
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
}
TEST_F(UeContext, AperiodicCsiTriggerLogged) {
  const nr_ue_csi_resource_t observed{4, 7, 0x25, 24, 48, 501};
  nr_ue_ctx_on_dci01_csi(0x1234, 0, 99, &observed);
  nr_ue_ctx_on_dci01_csi(0x1234, 2, 100, &observed);
  nr_ue_ctx_on_dci01_csi(0x1234, 1, 101, nullptr);
  auto s=contents();
  EXPECT_EQ(count(s,"\"param\":\"APERIODIC_CSI\""),2);
  EXPECT_NE(s.find("\"abs_slot\":100,\"epoch\":0,\"identity_gen\":0,\"rnti\":4660,\"incarnation\":0,\"param\":\"APERIODIC_CSI\",\"old\":null,\"new\":2"),std::string::npos);
  EXPECT_NE(s.find("\"evidence\":{\"row\":4,\"freq_domain\":37,\"start_rb\":24,\"nr_of_rbs\":48,\"symb_l0\":7,\"scramb_id\":501}"),std::string::npos);
  EXPECT_NE(s.find("\"abs_slot\":101,\"epoch\":0,\"identity_gen\":0,\"rnti\":4660,\"incarnation\":0,\"param\":\"APERIODIC_CSI\",\"old\":null,\"new\":1,\"cause\":\"FIRST_LEARNED\",\"evidence\":null"),std::string::npos);
  EXPECT_EQ(count(s,"\"type\":\"ue_reconfig\""),0);
}
TEST_F(UeContext, CloseWritesFinalSnapshotForEveryUe) {
  for(uint16_t rnti: {13,14})
    nr_ue_ctx_on_param(rnti,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  EXPECT_GE(count(contents(),"\"type\":\"ue_snapshot\""),4);
}
TEST(UeContextRing, FullRingDropsAndCounts) {
  char name[]="/tmp/rr-uectx-full-XXXXXX"; int fd=mkstemp(name); ASSERT_GE(fd,0); close(fd);
  nr_ue_ctx_test_writer_pause_ms=200;
  ASSERT_TRUE(nr_ue_ctx_open(name,1,0));
  nr_passive_obs_t o{}; o.rnti=1; o.t_mono_ns=1000000000;
  for(int i=0;i<1000;i++) nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_close(); nr_ue_ctx_test_writer_pause_ms=0; unlink(name);
  uint64_t events=0,written=0,dropped=0; nr_ue_ctx_stats(&events,&written,&dropped);
  EXPECT_GT(events,0); EXPECT_GT(dropped,0); EXPECT_GT(written,0);
}
TEST(UeContextRing, CloseQuiescesConcurrentProducers) {
  char name[]="/tmp/rr-uectx-close-XXXXXX"; int fd=mkstemp(name); ASSERT_GE(fd,0); close(fd);
  ASSERT_TRUE(nr_ue_ctx_open(name,64,0));
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int i=0;i<4;i++) threads.emplace_back([&,i] {
    while (!stop.load(std::memory_order_relaxed))
      nr_ue_ctx_on_param(100+i,NR_UEP_DCI_LEN_DL,47,NR_UEV_TRUSTED,NR_UEC_CONVERGED,10);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  nr_ue_ctx_close();
  stop.store(true,std::memory_order_relaxed);
  for (auto &thread:threads) thread.join();
  unlink(name);
  uint64_t events=0,lines=0,drops=0; nr_ue_ctx_stats(&events,&lines,&drops);
  EXPECT_GT(events,0);
  EXPECT_GE(lines,1);
}

// ---- re-attachment, RRC window, INACTIVE (nr_passive_reconf_rules.h) ----
static void put_bits(uint8_t *b,unsigned pos,unsigned n,uint64_t v) {
  for(unsigned i=0;i<n;i++) { unsigned bit=(v>>(n-1-i))&1u,p=pos+i; b[p>>3]=(uint8_t)((b[p>>3]&~(0x80u>>(p&7)))|(bit?(0x80u>>(p&7)):0)); }
}
static uint64_t resume_ccch(uint32_t id) { uint8_t b[6]={0}; put_bits(b,1,2,1); put_bits(b,3,24,id); return nr_reconf_pack48(b); }
static uint64_t stmsi_ccch(uint64_t id) { uint8_t b[6]={0}; put_bits(b,3,1,0); put_bits(b,4,39,id); return nr_reconf_pack48(b); }
static nr_passive_obs_t harq_grant(uint16_t rnti,int pid,int ndi,int tbs,int64_t slot) {
  nr_passive_obs_t o={}; o.rnti=rnti; o.dir=NR_OBS_DIR_DL; o.abs_slot=slot; o.t_mono_ns=nowns(); o.crc=NR_OBS_CRC_OK; o.mcs=4; o.nl=1;
  o.nb_rb=20; o.start_rb=10; o.tbs=tbs; o.harq_pid=pid; o.ndi=ndi; o.rv=0; o.rnti_class=0;
  o.snr_db=NAN; o.nvar=NAN; o.fo_comp_hz=NAN; o.delay_samples=NAN; return o;
}
TEST_F(UeContext, RarOnARecentlyActiveRntiIsTheSameUesContentionFreeRa) {
  auto o=grant(0x2000,nowns(),100); nr_ue_ctx_on_obs(&o); // a live C-RNTI; the gNB answers its PDCCH-order / BFR preamble with its own C-RNTI
  nr_ue_ctx_on_anchor(0x2000,NR_BLIND_RNTI_CLASS_TC_FOR_TEST,110);
  auto s=contents();
  EXPECT_EQ(s.find("\"cause\":\"REATTACH_"),std::string::npos);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2000,&c));
  EXPECT_EQ(c.incarnation,0);
  EXPECT_EQ(c.cfg[NR_UEP_RNTI_CLASS].value,0); // still a C-RNTI, not demoted to TC
}
TEST_F(UeContext, RarAfterTheSilenceHorizonOpensANewIncarnationOnce) {
  auto o=grant(0x2001,nowns()-11000000000ull,100); nr_ue_ctx_on_obs(&o); // last DCI 11 s ago: the value is free again
  nr_ue_ctx_on_anchor(0x2001,NR_BLIND_RNTI_CLASS_TC_FOR_TEST,110);
  nr_ue_ctx_on_anchor(0x2001,NR_BLIND_RNTI_CLASS_TC_FOR_TEST,111); // the same RAR decoded again is the same attachment
  auto s=contents();
  EXPECT_NE(s.find("\"param\":\"REATTACH\""),std::string::npos);
  EXPECT_EQ(count(s,"\"cause\":\"REATTACH_RAR\""),1);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2001,&c)); EXPECT_EQ(c.incarnation,1);
}
TEST_F(UeContext, RarOnAnUnseenRntiIsTheFirstIncarnation) {
  nr_ue_ctx_on_anchor(0x2002,NR_BLIND_RNTI_CLASS_TC_FOR_TEST,110);
  auto s=contents();
  EXPECT_EQ(s.find("\"cause\":\"REATTACH_"),std::string::npos);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2002,&c)); EXPECT_EQ(c.incarnation,0);
}
TEST_F(UeContext, Msg4ContentionResolutionPromotesTcToCAndRecordsTheIdentity) {
  nr_ue_ctx_on_anchor(0x2003,NR_BLIND_RNTI_CLASS_TC_FOR_TEST,100);
  const uint64_t id=resume_ccch(0x123456);
  nr_ue_ctx_on_msg3(0x2003,id,101);
  nr_ue_ctx_on_msg4(0x2003,id,105);
  auto s=contents();
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2003,&c));
  EXPECT_EQ(c.incarnation,0); // the RAR opened it, the Msg4 completes it
  EXPECT_EQ(c.cfg[NR_UEP_UE_ID].value,((int64_t)NR_RECONF_ID_RESUME<<48)|0x123456);
  EXPECT_EQ(c.cfg[NR_UEP_RNTI_CLASS].value,0);
  EXPECT_NE(s.find("\"param\":\"RNTI_CLASS\""),std::string::npos);
}
TEST_F(UeContext, Msg4OnALiveRntiWithoutRarIsAReattach) {
  auto o=grant(0x2004,nowns(),100); nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_on_msg4(0x2004,stmsi_ccch(77),110);
  auto s=contents();
  EXPECT_EQ(count(s,"\"cause\":\"REATTACH_MSG4\""),1);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2004,&c)); EXPECT_EQ(c.incarnation,1);
}
TEST_F(UeContext, ResumeIdentityLinksTheReturningUeToItsEarlierContext) {
  nr_ue_ctx_on_msg4(0x2005,resume_ccch(0xABC),100); // first attach, RNTI 0x2005
  nr_ue_ctx_on_msg4(0x2006,resume_ccch(0xABC),900); // the same UE resumes on another RNTI
  nr_ue_ctx_on_msg4(0x2007,resume_ccch(0xDEF),901); // a different UE: no link
  nr_ue_ctx_on_msg4(0x2008,stmsi_ccch(5),902);      // S-TMSI part 1 is linkable too ...
  nr_ue_ctx_on_msg4(0x2009,stmsi_ccch(5),903);
  auto s=contents();
  nr_ue_ctx_t a{},b{},d{},e{}; ASSERT_TRUE(nr_ue_ctx_get(0x2006,&a)); ASSERT_TRUE(nr_ue_ctx_get(0x2007,&b));
  ASSERT_TRUE(nr_ue_ctx_get(0x2009,&d)); ASSERT_TRUE(nr_ue_ctx_get(0x2008,&e));
  EXPECT_EQ(a.cfg[NR_UEP_RESUME_OF].value,((int64_t)0x2005<<16)|0);
  EXPECT_LT(b.cfg[NR_UEP_RESUME_OF].value,0);
  EXPECT_EQ(d.cfg[NR_UEP_RESUME_OF].value,((int64_t)0x2008<<16)|0);
  EXPECT_LT(e.cfg[NR_UEP_RESUME_OF].value,0);
  EXPECT_EQ(count(s,"\"cause\":\"RESUME_LINK\""),2);
}
TEST_F(UeContext, RandomIdentityIsNeverLinked) {
  uint8_t b[6]={0}; put_bits(b,3,1,1); put_bits(b,4,39,9); const uint64_t rnd=nr_reconf_pack48(b);
  nr_ue_ctx_on_msg4(0x2010,rnd,100); nr_ue_ctx_on_msg4(0x2011,rnd,101);
  contents();
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2011,&c)); EXPECT_LT(c.cfg[NR_UEP_RESUME_OF].value,0);
}
TEST_F(UeContext, UntoggledNdiWithAnotherTbsOnTwoHarqProcessesIsANewUe) {
  auto a=harq_grant(0x2020,0,1,8000,100), b=harq_grant(0x2020,1,0,8000,101); nr_ue_ctx_on_obs(&a); nr_ue_ctx_on_obs(&b);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));           // the old UE went silent; a new one uses the RNTI
  auto a2=harq_grant(0x2020,0,1,12000,102); nr_ue_ctx_on_obs(&a2);        // one violation: not enough
  auto b2=harq_grant(0x2020,1,0,9000,103); nr_ue_ctx_on_obs(&b2);         // second process: HARQ entity was reset
  auto s=contents();
  EXPECT_EQ(count(s,"\"cause\":\"REATTACH_HARQ\""),1);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2020,&c)); EXPECT_EQ(c.incarnation,1);
}
TEST_F(UeContext, RetransmissionsAndToggledNdiAreNotAReattach) {
  auto a=harq_grant(0x2021,0,1,8000,100), b=harq_grant(0x2021,0,1,8000,101), c2=harq_grant(0x2021,0,0,12000,102);
  nr_ue_ctx_on_obs(&a); nr_ue_ctx_on_obs(&b); nr_ue_ctx_on_obs(&c2);
  EXPECT_EQ(contents().find("\"cause\":\"REATTACH_"),std::string::npos);
}
TEST_F(UeContext, DciSizeChangeAfterSilenceIsAReattachButNotInTraffic) {
  nr_ue_ctx_on_param(0x2030,NR_UEP_DCI_LEN_DL,44,NR_UEV_TRUSTED,NR_UEC_CONVERGED,100);
  nr_ue_ctx_on_param(0x2030,NR_UEP_DCI_LEN_DL,46,NR_UEV_TRUSTED,NR_UEC_RELOCK,101); // in traffic: a reconfiguration, same UE
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  nr_ue_ctx_on_param(0x2030,NR_UEP_DCI_LEN_DL,52,NR_UEV_TRUSTED,NR_UEC_RELOCK,102); // after 300 ms of silence
  auto s=contents();
  EXPECT_EQ(count(s,"\"cause\":\"REATTACH_DCI_SIZE\""),1);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2030,&c)); EXPECT_EQ(c.incarnation,1); EXPECT_EQ(c.cfg[NR_UEP_DCI_LEN_DL].value,52);
}
TEST_F(UeContext, RrcWindowReverifiesDedicatedParametersButIsNoEpochEvidence) {
  nr_ue_ctx_on_param(0x2040,NR_UEP_DCI_LEN_DL,44,NR_UEV_TRUSTED,NR_UEC_CONVERGED,100);
  nr_ue_ctx_on_rrc(0x2040,NR_RECONF_RRC_RECONF,1000,1); // effective at slot 1032
  EXPECT_EQ(nr_reconf_rrc_phase(0x2040,1031,1),NR_RECONF_PHASE_HOLD);
  auto o=grant(0x2040,nowns(),1020); nr_ue_ctx_on_obs(&o);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto o2=grant(0x2040,nowns(),1040); nr_ue_ctx_on_obs(&o2);
  auto s=contents();
  EXPECT_EQ(reopened_count.load(),0); // an RRC message without an observed change is not a reopened UE (OTA TIM: 7661 such windows)
  EXPECT_NE(s.find("RRC_WINDOW_CLOSED"),std::string::npos);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2040,&c)); EXPECT_EQ(c.cfg[NR_UEP_DCI_LEN_DL].verif,NR_UEV_HINT);
}
TEST_F(UeContext, SilentUeBecomesIdleOrInactiveByItsIdentity) {
  nr_ue_ctx_on_msg4(0x2050,resume_ccch(0x77),100); // resume identity: has used RRC_INACTIVE
  auto a=grant(0x2050,nowns(),100); nr_ue_ctx_on_obs(&a);
  auto b=grant(0x2051,nowns(),100); nr_ue_ctx_on_obs(&b);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto late=grant(0x2052,nowns()+11000000000ull,200); nr_ue_ctx_on_obs(&late); // the clock moves 11 s on
  contents();
  nr_ue_ctx_t x{},y{}; ASSERT_TRUE(nr_ue_ctx_get(0x2050,&x)); ASSERT_TRUE(nr_ue_ctx_get(0x2051,&y));
  EXPECT_EQ(x.state,NR_UE_INACTIVE);
  EXPECT_EQ(y.state,NR_UE_IDLE);
}
static nr_passive_obs_t grant_for_cap(uint16_t rnti) {
  nr_passive_obs_t o={}; o.rnti=rnti; o.dir=NR_OBS_DIR_DL; o.abs_slot=1; o.t_mono_ns=nowns(); o.crc=NR_OBS_CRC_OK; o.mcs=4; o.nl=1; o.tbs=800;
  o.harq_pid=-1; o.ndi=-1; o.snr_db=NAN; o.nvar=NAN; o.fo_comp_hz=NAN; o.delay_samples=NAN; return o;
}
TEST(UeContextFile, JsonlIsBoundedAndTheOverflowIsCountedAsDropped) {
  char name[]="/tmp/rr-uectx-cap-XXXXXX"; int fd=mkstemp(name); ASSERT_GE(fd,0); close(fd);
  nr_ue_ctx_test_file_cap=2000;
  ASSERT_TRUE(nr_ue_ctx_open(name,128,0));
  for(int i=0;i<40;i++) {auto o=grant_for_cap(0x3000+i); nr_ue_ctx_on_obs(&o);}
  nr_ue_ctx_close();
  uint64_t ev,w,d; nr_ue_ctx_stats(&ev,&w,&d);
  struct stat st; ASSERT_EQ(stat(name,&st),0);
  EXPECT_LT(st.st_size,20000); // 40 UEs would write far more than that unbounded
  EXPECT_GT(d,0u);
  nr_ue_ctx_test_file_cap=512ll<<20;
  unlink(name);
}

// ---- NSA: no RRCSetup, no RRCReconfiguration, no Msg4 is ever decoded. Everything below uses DCI / HARQ / RAR / silence only. ----
TEST_F(UeContext, NoRrcNoMsg4_TcBecomesCFromADedicatedDciAlone) {
  nr_ue_ctx_on_anchor(0x2060,NR_BLIND_RNTI_CLASS_TC_FOR_TEST,100);
  nr_ue_ctx_on_param(0x2060,NR_UEP_DCI_LEN_DL,44,NR_UEV_TRUSTED,NR_UEC_CONVERGED,120); // UE-specific 1_1 accepted under that RNTI
  contents();
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2060,&c));
  EXPECT_EQ(c.cfg[NR_UEP_RNTI_CLASS].value,0);
  EXPECT_EQ(c.incarnation,0);
  EXPECT_LT(c.cfg[NR_UEP_UE_ID].value,0); // no identity, and nothing is "unknown" about the class
}
TEST_F(UeContext, NoRrcNoMsg4_TcBecomesCFromACRntiGrant) {
  nr_ue_ctx_on_anchor(0x2061,NR_BLIND_RNTI_CLASS_TC_FOR_TEST,100);
  auto o=grant(0x2061,nowns(),130); o.rnti_class=1; nr_ue_ctx_on_obs(&o); // first sighting as TC
  auto p=grant(0x2061,nowns(),131); p.rnti_class=0; nr_ue_ctx_on_obs(&p); // then scheduled as C-RNTI
  contents();
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2061,&c)); EXPECT_EQ(c.cfg[NR_UEP_RNTI_CLASS].value,0);
}
TEST_F(UeContext, NoRrcNoMsg4_ReconfigurationIsInferredFromTheDciSizeChangeAndTimesTheRelock) {
  nr_ue_ctx_on_param(0x2062,NR_UEP_DCI_LEN_DL,44,NR_UEV_TRUSTED,NR_UEC_CONVERGED,100);
  nr_ue_ctx_on_param(0x2062,NR_UEP_TD_WINNER,7,NR_UEV_TRUSTED,NR_UEC_CONVERGED,101);
  nr_ue_ctx_on_param(0x2062,NR_UEP_DCI_LEN_DL,46,NR_UEV_TRUSTED,NR_UEC_RELOCK,2000); // size changed in traffic at slot 2000
  contents();
  // TS 38.331 12: the change is the moment the RRCReconfiguration took effect: window ends there, the accelerated re-lock starts there
  EXPECT_EQ(nr_reconf_rrc_effective_slot(0x2062),2000);
  EXPECT_EQ(nr_reconf_rrc_phase(0x2062,1999,1),NR_RECONF_PHASE_HOLD);
  EXPECT_EQ(nr_reconf_rrc_n_suspect(0x2062,2010,1,200),(uint32_t)NR_RECONF_ARM_N_SUSPECT);
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2062,&c));
  EXPECT_EQ(c.cfg[NR_UEP_TD_WINNER].verif,NR_UEV_HINT);        // the same message changed the rest: re-verify
  EXPECT_EQ(c.cfg[NR_UEP_DCI_LEN_DL].value,46);
}
TEST_F(UeContext, NoRrcNoMsg4_ReturningRntiAfterGoneIsANewIncarnationAndNeverLinked) {
  auto a=grant(0x2063,nowns(),100); nr_ue_ctx_on_obs(&a);
  auto late=grant(0x2063,nowns()+61000000000ull,900); nr_ue_ctx_on_obs(&late); // 61 s of DCI silence, then the value is sighted again
  contents();
  nr_ue_ctx_t c{}; ASSERT_TRUE(nr_ue_ctx_get(0x2063,&c));
  EXPECT_EQ(c.incarnation,1);
  EXPECT_LT(c.cfg[NR_UEP_RESUME_OF].value,0);
}
TEST_F(UeContext, HarqViolationsInsideContinuousTrafficAreMissedGrantsNotAReattach) {
  auto a=harq_grant(0x2070,0,1,8000,100), b=harq_grant(0x2070,1,0,8000,101); nr_ue_ctx_on_obs(&a); nr_ue_ctx_on_obs(&b);
  auto a2=harq_grant(0x2070,0,1,12000,102), b2=harq_grant(0x2070,1,0,9000,103); nr_ue_ctx_on_obs(&a2); nr_ue_ctx_on_obs(&b2); // no silence before
  EXPECT_EQ(contents().find("\"cause\":\"REATTACH_"),std::string::npos);
}
