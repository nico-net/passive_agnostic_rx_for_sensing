#include <gtest/gtest.h>
#include "nr_passive_ue_ctx.h"
#include "nr_passive_cfg_epoch.h"
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
extern "C" bool nr_cfg_ignore_sib1(void) {
  const char *value=getenv("ISAC_TD_IGNORE_SIB1");
  return value && std::string(value)=="1";
}
extern "C" void nr_cfg_ignore_sib1_reset_for_test(void) {}

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
  EXPECT_NE(s.find("\"state\":\"FIRST_SEEN\""),std::string::npos);
  EXPECT_NE(s.find("\"state\":\"ACTIVE\""),std::string::npos);
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
TEST_F(UeContext, IgnoreSib1KeepsParametersNull) {
  const char *old=getenv("ISAC_TD_IGNORE_SIB1");
  const bool had_old=old!=nullptr;
  const std::string old_value=had_old?old:"";
  setenv("ISAC_TD_IGNORE_SIB1","1",1);
  nr_cfg_ignore_sib1_reset_for_test();
  nr_ue_ctx_on_sib1(0x1234,10);
  nr_ue_ctx_on_sib1_param(NR_UEP_SIB1_BWP,100,10);
  nr_ue_ctx_on_sib1_param(NR_UEP_SIB1_TDRA_HASH,0x5678,10);
  auto o=grant(15,1000000000,11); nr_ue_ctx_on_obs(&o);
  const auto s=contents();
  EXPECT_NE(s.find("\"SIB1_HASH\":null"),std::string::npos);
  EXPECT_NE(s.find("\"SIB1_BWP\":null"),std::string::npos);
  EXPECT_NE(s.find("\"SIB1_TDRA_HASH\":null"),std::string::npos);
  if(had_old) setenv("ISAC_TD_IGNORE_SIB1",old_value.c_str(),1);
  else unsetenv("ISAC_TD_IGNORE_SIB1");
  nr_cfg_ignore_sib1_reset_for_test();
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
  setenv("ISAC_UECTX_TEST_WRITER_PAUSE_MS","200",1);
  ASSERT_TRUE(nr_ue_ctx_open(name,1,0));
  nr_passive_obs_t o{}; o.rnti=1; o.t_mono_ns=1000000000;
  for(int i=0;i<1000;i++) nr_ue_ctx_on_obs(&o);
  nr_ue_ctx_close(); unsetenv("ISAC_UECTX_TEST_WRITER_PAUSE_MS"); unlink(name);
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
