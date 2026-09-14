#include <cstring>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_ss_registry.h"
}

static nr_pdcch_ss_entry_t mk(uint8_t id, uint8_t ss_type, uint16_t period, uint16_t offset)
{
  nr_pdcch_ss_entry_t e{};
  e.coreset_id = id; e.coreset_duration = 1; e.coreset_n_rbs = 48;
  e.ss_type = ss_type; e.ss_first_symbol = 0;
  e.ss_period_slots = period; e.ss_offset_slots = offset;
  e.bwp_start = 0; e.bwp_size = 273;
  e.al_candidates[1] = 2;   // AL2
  return e;
}

TEST(SsRegistry, HoldsSeveralConfigurations) {
  nr_pdcch_ss_registry_t r{};
  EXPECT_EQ(nr_pdcch_ss_register(&r, nullptr), -1);
  EXPECT_EQ(nr_pdcch_ss_register(nullptr, nullptr), -1);
  const auto a = mk(0, 0, 1, 0), b = mk(1, 1, 2, 1);
  EXPECT_EQ(nr_pdcch_ss_register(&r, &a), 0);
  EXPECT_EQ(nr_pdcch_ss_register(&r, &b), 1);
  EXPECT_EQ(nr_pdcch_ss_live(&r), 2);
}

TEST(SsRegistry, RejectsDuplicateGEOMETRYEvenWithADifferentId) {
  // Two entries with different ids but identical geometry monitor the SAME occasions: keeping both
  // doubles the scan cost and splits one physical search space's evidence between two counters,
  // which can leave each below the retirement bar while the space is plainly alive.
  nr_pdcch_ss_registry_t r{};
  auto a = mk(0, 1, 2, 1);
  auto b = mk(7, 1, 2, 1);          // different coreset_id, same geometry
  EXPECT_EQ(nr_pdcch_ss_register(&r, &a), 0);
  EXPECT_EQ(nr_pdcch_ss_register(&r, &b), -1);
  EXPECT_EQ(r.n, 1);
}

TEST(SsRegistry, RejectsConfigurationsThatCanNeverMonitorAnything) {
  nr_pdcch_ss_registry_t r{};
  auto e = mk(0, 0, 1, 0);
  e.ss_period_slots = 0;                       EXPECT_EQ(nr_pdcch_ss_register(&r, &e), -1);
  e = mk(0, 0, 4, 4);                          // offset == period: never matches a slot
  EXPECT_EQ(nr_pdcch_ss_register(&r, &e), -1);
  e = mk(0, 0, 4, 9);                          // offset beyond period
  EXPECT_EQ(nr_pdcch_ss_register(&r, &e), -1);
  e = mk(0, 0, 4, 1); memset(e.al_candidates, 0, sizeof(e.al_candidates));
  EXPECT_EQ(nr_pdcch_ss_register(&r, &e), -1) << "no candidates at any AL decodes nothing";
  e = mk(0, 0, 4, 1); e.coreset_duration = 4;  EXPECT_EQ(nr_pdcch_ss_register(&r, &e), -1);
  e = mk(0, 0, 4, 1); e.bwp_size = 0;          EXPECT_EQ(nr_pdcch_ss_register(&r, &e), -1);
}

TEST(SsRegistry, MonitoringOccasionsFollowPeriodAndOffset) {
  const auto e = mk(0, 1, 4, 1);
  EXPECT_FALSE(nr_pdcch_ss_monitors_slot(&e, 0));
  EXPECT_TRUE(nr_pdcch_ss_monitors_slot(&e, 1));
  EXPECT_FALSE(nr_pdcch_ss_monitors_slot(&e, 2));
  EXPECT_TRUE(nr_pdcch_ss_monitors_slot(&e, 5));
  EXPECT_TRUE(nr_pdcch_ss_monitors_slot(&e, 9));
  EXPECT_FALSE(nr_pdcch_ss_monitors_slot(nullptr, 1));
}

TEST(SsRegistry, RawAcceptsAreNotEvidence) {
  // A blind search over noise yields an in-range RNTI occasionally -- exactly how a spurious
  // CORESET looks alive. Only a REPEATED RNTI is evidence, and noise does not reproduce one.
  nr_pdcch_ss_registry_t r{};
  const auto real = mk(0, 1, 1, 0), noise = mk(1, 0, 2, 1);
  ASSERT_EQ(nr_pdcch_ss_register(&r, &real), 0);
  ASSERT_EQ(nr_pdcch_ss_register(&r, &noise), 1);
  for (int i = 0; i < 5000; i++) {
    nr_pdcch_ss_observe(&r, 0, i % 10 == 0, i % 10 == 0);          // real: confirmed accepts
    nr_pdcch_ss_observe(&r, 1, i % 500 == 0, false);               // noise: raw accepts only
  }
  EXPECT_GT(r.accepts[1], 0u) << "the test must actually exercise raw accepts";
  EXPECT_EQ(r.confirmed[1], 0u);
  EXPECT_EQ(nr_pdcch_ss_retire_barren(&r, 1000), 1);
  EXPECT_TRUE(r.retired[1]);
  EXPECT_FALSE(r.retired[0]);
  EXPECT_EQ(nr_pdcch_ss_live(&r), 1);
}

TEST(SsRegistry, DoesNotRetireOnThinEvidence) {
  nr_pdcch_ss_registry_t r{};
  const auto a = mk(0, 1, 1, 0), b = mk(1, 0, 2, 1);
  ASSERT_GE(nr_pdcch_ss_register(&r, &a), 0);
  ASSERT_GE(nr_pdcch_ss_register(&r, &b), 0);
  for (int i = 0; i < 100; i++) { nr_pdcch_ss_observe(&r, 0, true, true); nr_pdcch_ss_observe(&r, 1, false, false); }
  EXPECT_EQ(nr_pdcch_ss_retire_barren(&r, 1000), 0) << "retired before the evidence bar was met";
  EXPECT_EQ(nr_pdcch_ss_live(&r), 2);
}

TEST(SsRegistry, NeverRetiresTheLastLiveEntry) {
  // A receiver monitoring nothing cannot recover: it stops producing the very evidence that would
  // bring an entry back. Retiring everything is unrecoverable, so it must be impossible.
  nr_pdcch_ss_registry_t r{};
  const auto a = mk(0, 0, 1, 0), b = mk(1, 1, 2, 1);
  ASSERT_GE(nr_pdcch_ss_register(&r, &a), 0);
  ASSERT_GE(nr_pdcch_ss_register(&r, &b), 0);
  for (int i = 0; i < 5000; i++) { nr_pdcch_ss_observe(&r, 0, false, false); nr_pdcch_ss_observe(&r, 1, false, false); }
  nr_pdcch_ss_retire_barren(&r, 100);
  EXPECT_GE(nr_pdcch_ss_live(&r), 1);
  nr_pdcch_ss_retire_barren(&r, 100);
  EXPECT_GE(nr_pdcch_ss_live(&r), 1);
}

TEST(SsRegistry, AConfirmedEntryIsNeverRetired) {
  nr_pdcch_ss_registry_t r{};
  const auto a = mk(0, 1, 1, 0);
  ASSERT_GE(nr_pdcch_ss_register(&r, &a), 0);
  const auto b = mk(1, 0, 2, 1);
  ASSERT_GE(nr_pdcch_ss_register(&r, &b), 0);
  for (int i = 0; i < 5000; i++) {
    nr_pdcch_ss_observe(&r, 0, i % 1000 == 0, i % 1000 == 0);   // rare but real
    nr_pdcch_ss_observe(&r, 1, false, false);
  }
  nr_pdcch_ss_retire_barren(&r, 1000);
  EXPECT_FALSE(r.retired[0]) << "a rarely-used but REAL search space was retired";
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
