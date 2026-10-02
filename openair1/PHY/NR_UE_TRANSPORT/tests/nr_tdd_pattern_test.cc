#include <gtest/gtest.h>
extern "C" {
#include "nr_tdd_pattern.h"
}

TEST(TddPeriod, ConvertsTheLegalPeriodicitiesAtMu1) {
  // 30 kHz SCS: 2 slots per ms. The sub-millisecond entries are why the API takes tenths.
  EXPECT_EQ(nr_tdd_period_slots(5, 1), 1);      // 0.5 ms
  EXPECT_EQ(nr_tdd_period_slots(10, 1), 2);     // 1 ms
  EXPECT_EQ(nr_tdd_period_slots(25, 1), 5);     // 2.5 ms
  EXPECT_EQ(nr_tdd_period_slots(50, 1), 10);    // 5 ms
  EXPECT_EQ(nr_tdd_period_slots(100, 1), 20);   // 10 ms
}

TEST(TddPeriod, RefusesCombinationsThatAreNotAWholeNumberOfSlots) {
  // 0.5 ms at 15 kHz is half a slot. Rounding it would produce a pattern that never matches the
  // cell, and the receiver would then mark real downlink slots as uplink and stop monitoring them.
  EXPECT_EQ(nr_tdd_period_slots(5, 0), 0);
  EXPECT_EQ(nr_tdd_period_slots(25, 0), 0);     // 2.5 ms at 15 kHz
  EXPECT_EQ(nr_tdd_period_slots(0, 1), 0);
  EXPECT_EQ(nr_tdd_period_slots(10, 9), 0);     // nonsense mu
}

// The common DDDSU pattern: 2.5 ms at 30 kHz = 5 slots, 3 DL, 1 mixed, 1 UL.
static nr_tdd_config_t dddsu()
{
  nr_tdd_pattern_t p1{};
  p1.period_slots = 5; p1.dl_slots = 3; p1.dl_symbols = 6; p1.ul_symbols = 4; p1.ul_slots = 1;
  nr_tdd_config_t c{};
  EXPECT_TRUE(nr_tdd_config_init(&c, &p1, nullptr));
  return c;
}

TEST(TddSlot, DddsuHasTheExpectedShape) {
  const nr_tdd_config_t c = dddsu();
  EXPECT_EQ(nr_tdd_slot_direction(&c, 0), NR_TDD_SLOT_DL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 1), NR_TDD_SLOT_DL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 2), NR_TDD_SLOT_DL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 3), NR_TDD_SLOT_MIXED);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 4), NR_TDD_SLOT_UL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 5), NR_TDD_SLOT_DL) << "the pattern must repeat";
  EXPECT_EQ(nr_tdd_slot_direction(&c, 9), NR_TDD_SLOT_UL);
}

TEST(TddSlot, OnlyDownlinkAndMixedSlotsCanCarryPdcch) {
  const nr_tdd_config_t c = dddsu();
  // A mixed slot counts: the CORESET sits at the start of the slot, which is its downlink part.
  EXPECT_TRUE(nr_tdd_slot_has_downlink(&c, 3));
  EXPECT_FALSE(nr_tdd_slot_has_downlink(&c, 4));
  int monitorable = 0;
  for (uint32_t s = 0; s < 100; s++) if (nr_tdd_slot_has_downlink(&c, s)) monitorable++;
  EXPECT_EQ(monitorable, 80) << "DDDSU should leave 1 slot in 5 unmonitorable";
}

TEST(TddSlot, AnUnknownConfigurationMonitorsEverything) {
  // Failing safe matters more than saving CPU: a missed downlink slot loses real grants, while
  // scanning a slot that turns out to be uplink costs only the work this was meant to save.
  nr_tdd_config_t c{};
  EXPECT_TRUE(nr_tdd_slot_has_downlink(&c, 0));
  EXPECT_TRUE(nr_tdd_slot_has_downlink(&c, 7));
  EXPECT_TRUE(nr_tdd_slot_has_downlink(nullptr, 3));
}

TEST(TddConfig, RejectsAPatternLongerThanItsOwnPeriod) {
  // Such a pattern would alias and mark uplink slots as downlink -- worse than not having it.
  nr_tdd_pattern_t bad{};
  bad.period_slots = 4; bad.dl_slots = 3; bad.dl_symbols = 6; bad.ul_symbols = 4; bad.ul_slots = 2;
  nr_tdd_config_t c{};
  EXPECT_FALSE(nr_tdd_config_init(&c, &bad, nullptr));
  EXPECT_FALSE(c.valid);
}

TEST(TddConfig, APurePatternWithNoMixedSlotUsesTheWholePeriod) {
  // With no mixed symbols there is no mixed slot, so dl_slots + ul_slots may fill the period
  // exactly. Counting a mixed slot unconditionally would reject this legal configuration.
  nr_tdd_pattern_t p{};
  p.period_slots = 5; p.dl_slots = 4; p.ul_slots = 1; p.dl_symbols = 0; p.ul_symbols = 0;
  nr_tdd_config_t c{};
  ASSERT_TRUE(nr_tdd_config_init(&c, &p, nullptr));
  EXPECT_EQ(nr_tdd_slot_direction(&c, 3), NR_TDD_SLOT_DL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 4), NR_TDD_SLOT_UL);
}

TEST(TddConfig, TwoPatternsConcatenateAndRepeatTogether) {
  nr_tdd_pattern_t p1{}, p2{};
  p1.period_slots = 5; p1.dl_slots = 3; p1.dl_symbols = 6; p1.ul_symbols = 4; p1.ul_slots = 1;
  p2.period_slots = 3; p2.dl_slots = 1; p2.ul_slots = 2;
  nr_tdd_config_t c{};
  ASSERT_TRUE(nr_tdd_config_init(&c, &p1, &p2));
  EXPECT_EQ(nr_tdd_slot_direction(&c, 4), NR_TDD_SLOT_UL);   // end of pattern 1
  EXPECT_EQ(nr_tdd_slot_direction(&c, 5), NR_TDD_SLOT_DL);   // pattern 2 begins
  EXPECT_EQ(nr_tdd_slot_direction(&c, 6), NR_TDD_SLOT_UL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 7), NR_TDD_SLOT_UL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 8), NR_TDD_SLOT_DL);   // back to pattern 1, period 8
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/* BC9: the last symbol a PDSCH may END on in a slot, from the COMMON pattern only. Common UL symbols can never carry a
 * PDSCH; flexible symbols can (a DCI may schedule a PDSCH there, TS 38.213 11.1), so only UL symbols restrict. */
TEST(TddSlot, PdschLastSymbolUsesCommonUplinkSymbolsOnly) {
  const nr_tdd_config_t c = dddsu(); // mixed slot: 6 DL, 4 flexible, 4 UL symbols
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 0), 13);  // DL slot
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 3), 9);   // mixed: symbols 10..13 are UL; flexible 6..9 may carry PDSCH
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 4), -1);  // UL slot
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 8), 9);   // periodic
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(nullptr, 4), 13); // unknown pattern never restricts
  nr_tdd_config_t bad{};
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&bad, 4), 13);
  // Flexible slots past the configured ones restrict nothing.
  nr_tdd_pattern_t p1{};
  p1.period_slots = 10; p1.dl_slots = 3; p1.ul_slots = 2;
  nr_tdd_config_t f{};
  ASSERT_TRUE(nr_tdd_config_init(&f, &p1, nullptr));
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&f, 3), -1);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&f, 5), 13);
}
