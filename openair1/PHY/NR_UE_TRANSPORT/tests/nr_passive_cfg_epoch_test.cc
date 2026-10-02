#include <gtest/gtest.h>
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_cfg_sources.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include <atomic>
#include <thread>

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
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_RESET);
  EXPECT_EQ(last.last_cause, NR_CAUSE_CELL_IDENTITY_CHANGE);
  nr_cfg_epoch_note_identity(11, 100, 200);
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
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
  EXPECT_EQ(last.last_cause, NR_CAUSE_MIB_CHANGE);
  EXPECT_EQ(nr_cfg_epoch_identity_gen(), 0u);
}
TEST_F(CfgEpoch, Sib1SemanticChangeBumps) {
  nr_cfg_epoch_note_sib1(42);
  nr_cfg_epoch_note_sib1(43);
  EXPECT_EQ(last.last_cause, NR_CAUSE_SIB1_CHANGE);
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
}
TEST_F(CfgEpoch, Sib1SameSemanticNoBump) {
  nr_cfg_epoch_note_sib1(42);
  nr_cfg_epoch_note_sib1(42);
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, SiModAnnouncedBumpsOnlyAtBoundaryIfChanged) {
  nr_cfg_epoch_note_sib1(42);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_note_sib1(43);
  EXPECT_EQ(calls, 0);
  nr_cfg_epoch_tick(99);
  EXPECT_EQ(calls, 0);
  nr_cfg_epoch_tick(100);
  EXPECT_EQ(last.last_cause, NR_CAUSE_SIB1_CHANGE);
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, SiModAnnouncedButUnchangedNoBump) {
  nr_cfg_epoch_note_sib1(42);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_note_sib1(42);
  nr_cfg_epoch_tick(100);
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, SiModBoundaryWithoutSib1ReacquiredBumps) {
  nr_cfg_epoch_note_sib1(42);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_tick(100);
  EXPECT_EQ(last.last_cause, NR_CAUSE_SI_MODIFICATION_ANNOUNCED);
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, SiModNoReacquireFallbackCanBeDisabled) {
  setenv("ISAC_RECONF_SI_BUMP_WITHOUT_SIB1", "0", 1);
  nr_cfg_epoch_note_sib1(42);
  nr_cfg_epoch_note_si_modification(51, 100);
  nr_cfg_epoch_tick(100);
  EXPECT_EQ(calls, 0);
  unsetenv("ISAC_RECONF_SI_BUMP_WITHOUT_SIB1");
}
TEST_F(CfgEpoch, ContinuityLossLoggedAsContinuity) {
  nr_cfg_epoch_note_continuity_loss();
  EXPECT_EQ(last.last_cause, NR_CAUSE_CONTINUITY_LOSS);
  EXPECT_EQ(last.last_class, NR_EPOCH_HARD_REVERIFY);
}
TEST_F(CfgEpoch, TwoConvergedRntisWithin2sIsSoft) {
  nr_cfg_epoch_note_rnti_reopened(1, true, 100);
  nr_cfg_epoch_note_rnti_reopened(2, true, 200);
  EXPECT_EQ(last.last_cause, NR_CAUSE_DEDICATED_CHANGE_SUSPECTED);
  EXPECT_EQ(last.last_class, NR_EPOCH_SOFT);
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, VanishedRntisDoNotTriggerSoft) {
  nr_cfg_epoch_note_rnti_reopened(1, false, 100);
  nr_cfg_epoch_note_rnti_reopened(2, false, 200);
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, OneRntiReopenNoBump) {
  nr_cfg_epoch_note_rnti_reopened(1, true, 100);
  nr_cfg_epoch_note_rnti_reopened(1, true, 200);
  EXPECT_EQ(calls, 0);
}
TEST_F(CfgEpoch, ListenersCalledOncePerBump) {
  nr_cfg_epoch_subscribe(listener);
  nr_cfg_epoch_note_bwp_change();
  EXPECT_EQ(calls, 1);
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
  EXPECT_EQ(last.last_cause, NR_CAUSE_CSIRS_MAP_CHANGE);
  EXPECT_EQ(last.last_class, NR_EPOCH_SOFT);
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
  EXPECT_EQ(calls, 1);
  nr_cfg_epoch_note_identity(11, 100, 0);
  EXPECT_EQ(calls, 1);
}
TEST_F(CfgEpoch, DecodedBwpAndContinuitySources) {
  nr_cfg_epoch_note_bwp_change();
  EXPECT_EQ(last.last_cause, NR_CAUSE_BWP_CHANGE);
  nr_cfg_epoch_note_continuity_loss();
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
