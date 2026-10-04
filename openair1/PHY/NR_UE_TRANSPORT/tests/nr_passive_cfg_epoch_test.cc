#include <gtest/gtest.h>
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_cfg_sources.h"
#include "nr_passive_ue_ctx.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <unistd.h>

extern "C" {
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

namespace {
std::atomic<int> calls{0};
nr_cfg_epoch_snapshot_t last{};
void listener(const nr_cfg_epoch_snapshot_t *s) { last = *s; ++calls; }

class CfgEpoch : public ::testing::Test {
 protected:
  void SetUp() override {
    nr_cfg_epoch_reset();
    calls = 0;
    nr_cfg_epoch_subscribe(listener);
    nr_cfg_epoch_note_identity(10, 100, 200);
  }
};

TEST_F(CfgEpoch, PciChangeIsHardResetAndIdentityGen) {
  nr_cfg_epoch_note_identity(11, 100, 200);
  EXPECT_EQ(nr_cfg_epoch_current(), 1u);
  EXPECT_EQ(nr_cfg_epoch_identity_gen(), 1u);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_RESET);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_CELL_IDENTITY_CHANGE);
  nr_cfg_epoch_note_identity(11, 100, 200);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, CarrierAndPointAChangesAreHardReset) {
  nr_cfg_epoch_note_identity(10, 101, 200);
  nr_cfg_epoch_note_identity(10, 101, 201);
  EXPECT_EQ(nr_cfg_epoch_identity_gen(), 2u);
  EXPECT_EQ(nr_cfg_epoch_current(), 2u);
}
TEST_F(CfgEpoch, MibChangeIsHardReverify) {
  nr_cfg_epoch_note_mib(42);
  nr_cfg_epoch_note_mib(43);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_MIB_CHANGE);
  EXPECT_EQ(nr_cfg_epoch_identity_gen(), 0u);
}
TEST_F(CfgEpoch, Sib1SemanticChangeBumps) {
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_sib1(43, 0);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_SIB1_CHANGE);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
}
TEST_F(CfgEpoch, Sib1SameSemanticNoBump) {
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, SiModAnnouncedBumpsOnlyAtBoundaryIfChanged) {
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_tick(99);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 0);
  nr_cfg_epoch_tick(100);
  nr_cfg_epoch_note_sib1(43, 100);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_SIB1_CHANGE);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, SiModAnnouncedButUnchangedNoBump) {
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_note_sib1(42, 100);
  nr_cfg_epoch_tick(100);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, SiModBoundaryWithoutSib1ReacquiredBumps) {
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_tick(5099);
  EXPECT_EQ(nr_cfg_epoch_current(), 0u);
  nr_cfg_epoch_tick(5100);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_SI_MODIFICATION_ANNOUNCED);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, SiModNoReacquireFallbackCanBeDisabled) {
  setenv("ISAC_RECONF_SI_BUMP_WITHOUT_SIB1", "0", 1);
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_tick(100);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 0);
  unsetenv("ISAC_RECONF_SI_BUMP_WITHOUT_SIB1");
}
TEST_F(CfgEpoch, ContinuityLossLoggedAsContinuity) {
  nr_cfg_epoch_note_continuity_loss_samples(10000, 1000);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_CONTINUITY_LOSS);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
}
TEST_F(CfgEpoch, ShortGapIsSoft) {
  nr_cfg_epoch_note_continuity_loss_samples(9000, 1000);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_SOFT);
  EXPECT_EQ(last.last_cause, NR_CAUSE_CONTINUITY_LOSS);
}
TEST_F(CfgEpoch, LongGapIsHardReverify) {
  for (uint64_t ms : {10u, 50u}) {
    nr_cfg_epoch_note_continuity_loss_samples(ms * 1000, 1000);
    nr_cfg_epoch_drain();
    EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
    EXPECT_EQ(last.last_cause, NR_CAUSE_CONTINUITY_LOSS);
  }
  EXPECT_EQ(calls, 2);
}
TEST_F(CfgEpoch, GapThresholdFromEnv) {
  setenv("ISAC_RECONF_GAP_HARD_MS", "20", 1);
  nr_cfg_epoch_note_continuity_loss_samples(19000, 1000);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_SOFT);
  nr_cfg_epoch_note_continuity_loss_samples(20000, 1000);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
  unsetenv("ISAC_RECONF_GAP_HARD_MS");
}
TEST_F(CfgEpoch, TwoConvergedRntisWithin2sIsSoft) {
  nr_cfg_epoch_note_rnti_reopened(1, true, 100);
  nr_cfg_epoch_note_rnti_reopened(2, true, 200);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_DEDICATED_CHANGE_SUSPECTED);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_SOFT);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, SuspectStillReachesEpochWithUeContextOn) {
  char name[]="/tmp/rr-epoch-uectx-XXXXXX";
  int fd=mkstemp(name); ASSERT_GE(fd,0); close(fd);
  ASSERT_TRUE(nr_ue_ctx_open(name,64,0));
  for (uint16_t rnti : {uint16_t(1),uint16_t(2)}) {
    nr_ue_ctx_on_param(rnti,NR_UEP_DCI_LEN_STATE,2,NR_UEV_SUSPECT,NR_UEC_RELOCK,100);
    nr_cfg_epoch_note_rnti_reopened(rnti,true,100);
  }
  nr_ue_ctx_close(); unlink(name);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class,NR_EPOCH_SOFT);
  EXPECT_EQ(last.last_cause,NR_CAUSE_DEDICATED_CHANGE_SUSPECTED);
  EXPECT_EQ(calls,1);
}
TEST_F(CfgEpoch, VanishedRntisDoNotTriggerSoft) {
  nr_cfg_epoch_note_rnti_reopened(1, false, 100);
  nr_cfg_epoch_note_rnti_reopened(2, false, 200);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, OneRntiReopenNoBump) {
  nr_cfg_epoch_note_rnti_reopened(1, true, 100);
  nr_cfg_epoch_note_rnti_reopened(1, true, 200);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, ListenersCalledOncePerBump) {
  nr_cfg_epoch_subscribe(listener);
  nr_cfg_epoch_note_bwp_change();
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 1);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.epoch, 1u);
}
TEST_F(CfgEpoch, CurrentIsThreadSafe) {
  std::atomic<bool> bad{false};
  std::thread reader([&] {
    uint32_t previous = 0;
    for (int i = 0; i < 10000; ++i) {
      const uint32_t current = nr_cfg_epoch_current();
      if (current < previous || current > 100) bad = true;
      previous = current;
    }
  });
  for (int i = 0; i < 100; ++i) nr_cfg_epoch_note_bwp_change();
  reader.join();
  EXPECT_FALSE(bad);
  EXPECT_EQ(nr_cfg_epoch_current(), 100u);
}
TEST_F(CfgEpoch, CsirsMapChangeIsSoft) {
  nr_cfg_epoch_note_csirs_map_change();
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_CSIRS_MAP_CHANGE);
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_class, NR_EPOCH_SOFT);
}
TEST_F(CfgEpoch, SiModDoesNotBumpAtBoundaryWithoutPostBoundaryDecode) {
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_tick(100);
  EXPECT_EQ(nr_cfg_epoch_current(), 0u);
}
TEST_F(CfgEpoch, AnnouncementDoesNotRequestSib1BeforeBoundary) {
  nr_cfg_epoch_note_si_modification(51, 100);
  EXPECT_FALSE(nr_cfg_epoch_si_redecode_pending(51));
}
static std::mutex length_lock;
static std::vector<uint32_t> bump_order;
static void length_listener(const nr_cfg_epoch_snapshot_t *s) {
  std::lock_guard<std::mutex> guard(length_lock);
  bump_order.push_back(s->epoch);
}
TEST_F(CfgEpoch, ListenerDeferredWhileCallerHoldsLengthLock) {
  bump_order.clear();
  nr_cfg_epoch_subscribe(length_listener);
  {
    std::lock_guard<std::mutex> guard(length_lock);
    nr_cfg_epoch_note_bwp_change();
    nr_cfg_epoch_note_continuity_loss();
    EXPECT_EQ(calls, 0);
  }
  nr_cfg_epoch_drain();
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(bump_order, (std::vector<uint32_t>{1, 2}));
}
TEST_F(CfgEpoch, HardResetClearsSiPeriod) {
  nr_cfg_epoch_set_si_period(20480);
  nr_cfg_epoch_note_identity(11, 100, 200);
  EXPECT_EQ(nr_cfg_epoch_si_period(), 0u);
}
TEST_F(CfgEpoch, PreBoundarySib1IgnoredForComparison) {
  nr_cfg_epoch_note_sib1(42, 0);
  nr_cfg_epoch_note_si_modification(51, 100);
  EXPECT_FALSE(nr_cfg_epoch_note_sib1(99, 99));
  nr_cfg_epoch_tick(100);
  EXPECT_EQ(nr_cfg_epoch_current(), 0u);
  EXPECT_TRUE(nr_cfg_epoch_si_redecode_pending(100));
  nr_cfg_epoch_note_sib1(43, 101);
  EXPECT_EQ(nr_cfg_epoch_current(), 1u);
  nr_cfg_epoch_tick(5100);
  EXPECT_EQ(nr_cfg_epoch_current(), 1u);
}
TEST(CfgSources, IdentityFrequencyRoundingDoesNotChangeCell) {
  EXPECT_EQ(nr_cfg_identity_frequency(3408960000, 30000), nr_cfg_identity_frequency(3408960001, 30000));
  EXPECT_NE(nr_cfg_identity_frequency(3408960000, 30000), nr_cfg_identity_frequency(3408975000, 30000));
}
TEST(CfgSources, PeriodicSib1WindowIsBounded) {
  EXPECT_FALSE(nr_cfg_sib1_window_expired(120, 100, 7, 1000));
  EXPECT_TRUE(nr_cfg_sib1_window_expired(121, 100, 8, 1000));
  EXPECT_TRUE(nr_cfg_sib1_window_expired(1100, 100, 0, 1000));
}
TEST_F(CfgEpoch, InFlightJobAcrossBumpNotCredited) {
  uint64_t dropped = 0;
  nr_cfg_epoch_work_t work;
  nr_cfg_epoch_work_begin(&work, nr_cfg_epoch_current(), &dropped);
  EXPECT_TRUE(nr_cfg_epoch_work_current()); // dequeued
  nr_cfg_epoch_note_bwp_change(); // decode / GPU batch still in flight
  EXPECT_FALSE(nr_cfg_epoch_work_current()); // first evidence consumer
  EXPECT_FALSE(nr_cfg_epoch_work_current()); // another consumer, same job
  EXPECT_EQ(dropped, 1u);
  nr_cfg_epoch_work_end(&work);
  EXPECT_TRUE(nr_cfg_epoch_work_current());
}
TEST_F(CfgEpoch, ModificationPeriodBeyondSfnWrapUsesSfnZeroBoundary) {
  nr_cfg_epoch_set_slots_per_second(2000);
  nr_cfg_epoch_note_si_modification(20000, 40960); // 2048 radio frames, SFN wraps at 1024
  EXPECT_EQ(nr_cfg_epoch_si_boundary(), 20480u);
}
TEST(CfgSources, ShortMessageRequiresReencodeGate) {
  EXPECT_TRUE(nr_cfg_prnti_si_credible(2, 0x80, 0, 0));
  EXPECT_FALSE(nr_cfg_prnti_si_credible(2, 0x80, 31, 0));
  EXPECT_FALSE(nr_cfg_prnti_si_credible(2, 0x00, 0, 0));
}
TEST(CfgSources, MibHashIgnoresFrameAndSsbIndex) {
  const auto a = nr_cfg_mib_hash(1, 12, 2, 4, 1, 1, 0);
  EXPECT_EQ(a, nr_cfg_mib_hash(1, 12, 2, 4, 1, 1, 0));
  EXPECT_NE(a, nr_cfg_mib_hash(1, 12, 3, 4, 1, 1, 0));
}
TEST(CfgSources, Sib1CanonicalHashIgnoresEncodingOnlyDifferences) {
  uint8_t identity_a[] = {0x12, 0x34, 0x50};
  uint8_t identity_b[] = {0x12, 0x34, 0x5f}; // unused low nibble differs
  auto make = [](const uint8_t *identity) {
    uint32_t h = nr_cfg_semantic_start();
    h = nr_cfg_semantic_add(h, 310); // decoded PLMN
    h = nr_cfg_semantic_bits(h, identity, 20); // decoded cell identity bits
    h = nr_cfg_semantic_add(h, 106); // decoded carrier bandwidth
    return h;
  };
  EXPECT_EQ(make(identity_a), make(identity_b));
  EXPECT_NE(make(identity_a), nr_cfg_semantic_add(make(identity_a), 1));
}
TEST(CfgSources, PrntiShortMessageSiModification) {
  EXPECT_TRUE(nr_cfg_prnti_si_modified(2, 0x80));
  EXPECT_TRUE(nr_cfg_prnti_si_modified(3, 0x80));
  EXPECT_FALSE(nr_cfg_prnti_si_modified(1, 0x80));
  EXPECT_FALSE(nr_cfg_prnti_si_modified(2, 0x40));
}
TEST(CfgSources, SiPeriodAndPeriodicRedecode) {
  EXPECT_EQ(nr_cfg_si_period_slots(4, 128, 20), 10240u);
  EXPECT_FALSE(nr_cfg_sib1_redecode_due(9999, 0, 2000));
  EXPECT_TRUE(nr_cfg_sib1_redecode_due(10000, 0, 2000));
}
TEST(CfgSources, Sib1CacheKeyIncludesSemanticHash) {
  char a[100], b[100], legacy[100];
  nr_cfg_sib1_cache_name(a, sizeof(a), "/tmp/fixture", 17, 0x1234, true);
  nr_cfg_sib1_cache_name(b, sizeof(b), "/tmp/fixture", 17, 0x1235, true);
  nr_cfg_sib1_cache_name(legacy, sizeof(legacy), "/tmp/fixture", 17, 0x1235, false);
  EXPECT_STRNE(a, b);
  EXPECT_STREQ(legacy, "/tmp/fixture/sib1_common_pci17.bin");
}
TEST_F(CfgEpoch, IdentityRefinesUnknownPointAWithoutBump) {
  nr_cfg_epoch_note_identity(11, 100, 0);
  nr_cfg_epoch_refine_point_a(201);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 1);
  nr_cfg_epoch_note_identity(11, 100, 0);
  nr_cfg_epoch_drain();
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, DecodedBwpAndContinuitySources) {
  nr_cfg_epoch_note_bwp_change();
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_BWP_CHANGE);
  nr_cfg_epoch_note_continuity_loss();
  nr_cfg_epoch_drain();
  EXPECT_EQ(last.last_cause, NR_CAUSE_CONTINUITY_LOSS);
}
TEST_F(CfgEpoch, SfnWrapKeepsSiBoundaryMonotonic) {
  EXPECT_EQ(nr_cfg_epoch_observe_slot(1023, 19, 20), 20479u);
  EXPECT_EQ(nr_cfg_epoch_observe_slot(0, 0, 20), 20480u);
}
}

int main(int argc, char **argv) {
  setenv("ISAC_RECONF", "1", 1);
  logInit();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
