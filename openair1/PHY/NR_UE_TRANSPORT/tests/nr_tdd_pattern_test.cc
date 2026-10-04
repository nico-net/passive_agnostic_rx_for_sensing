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

/* BC9 / K40: TS 38.213 11.1 placement. nrofDownlinkSlots are the FIRST slots of the period, nrofUplinkSlots the LAST;
 * nrofDownlinkSymbols start the slot right after the last full DL slot, nrofUplinkSymbols end the slot right before the
 * first full UL slot; everything else is FLEXIBLE. Flexible symbols may carry a DCI-scheduled PDSCH and PDCCH, so they
 * are never excluded and always monitored. Only common UL symbols restrict a PDSCH. */
static nr_tdd_config_t one(uint16_t period, uint8_t dl, uint8_t dsym, uint8_t usym, uint8_t ul)
{
  nr_tdd_pattern_t p{};
  p.period_slots = period; p.dl_slots = dl; p.dl_symbols = dsym; p.ul_symbols = usym; p.ul_slots = ul;
  nr_tdd_config_t c{};
  EXPECT_TRUE(nr_tdd_config_init(&c, &p, nullptr));
  return c;
}
TEST(TddSpec, FlexibleSlotsBetweenDlAndUlAreNeitherExcludedNorSkipped) {
  const nr_tdd_config_t c = one(10, 3, 0, 0, 2); // D D D F F F F F U U
  for (uint32_t s = 0; s < 3; s++) EXPECT_EQ(nr_tdd_slot_direction(&c, s), NR_TDD_SLOT_DL) << s;
  for (uint32_t s = 3; s < 8; s++) {
    EXPECT_EQ(nr_tdd_slot_direction(&c, s), NR_TDD_SLOT_FLEXIBLE) << s;
    EXPECT_TRUE(nr_tdd_slot_has_downlink(&c, s)) << s;   // PDCCH may sit in flexible symbols
    EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, s), 13) << s; // and a PDSCH may fill them
  }
  for (uint32_t s = 8; s < 10; s++) {
    EXPECT_EQ(nr_tdd_slot_direction(&c, s), NR_TDD_SLOT_UL) << s; // the LAST slots of the period
    EXPECT_FALSE(nr_tdd_slot_has_downlink(&c, s)) << s;
    EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, s), -1) << s;
  }
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 13), 13); // periodic
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 18), -1);
}
TEST(TddSpec, Dddsu) {
  const nr_tdd_config_t c = one(5, 3, 6, 4, 1); // mixed slot 3: 6 DL, 4 flexible, 4 UL symbols
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 0), 13);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 3), NR_TDD_SLOT_MIXED);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 3), 9); // flexible 6..9 may carry PDSCH
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 4), -1);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 8), 9);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(nullptr, 4), 13); // unknown pattern never restricts
  nr_tdd_config_t bad{};
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&bad, 4), 13);
}
TEST(TddSpec, SevenD1S2U) {
  const nr_tdd_config_t c = one(10, 7, 6, 4, 2); // the rfsim cell
  for (uint32_t s = 0; s < 7; s++) EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, s), 13) << s;
  EXPECT_EQ(nr_tdd_slot_direction(&c, 7), NR_TDD_SLOT_MIXED);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 7), 9);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 8), -1);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 9), -1);
}
TEST(TddSpec, MixedSymbolsInDifferentSlots) {
  /* D D D [4 DL + flexible] F F [flexible + 3 UL] U U */
  const nr_tdd_config_t c = one(10, 3, 4, 3, 2);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 3), NR_TDD_SLOT_MIXED);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 3), 13); // DL symbols then flexible: no UL symbol here
  EXPECT_EQ(nr_tdd_slot_direction(&c, 4), NR_TDD_SLOT_FLEXIBLE);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 6), NR_TDD_SLOT_FLEXIBLE);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 7), NR_TDD_SLOT_MIXED);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 7), 10); // UL symbols 11..13
  EXPECT_TRUE(nr_tdd_slot_has_downlink(&c, 7));
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 8), -1);
}
TEST(TddSpec, SymbolsMustFitTheirSlots) {
  nr_tdd_pattern_t p{};
  nr_tdd_config_t c{};
  p.period_slots = 5; p.dl_slots = 3; p.dl_symbols = 10; p.ul_symbols = 6; p.ul_slots = 1; // same slot, 16 symbols
  EXPECT_FALSE(nr_tdd_config_init(&c, &p, nullptr));
  p = {}; p.period_slots = 5; p.dl_slots = 5; p.dl_symbols = 2; // no slot left for the DL symbols
  EXPECT_FALSE(nr_tdd_config_init(&c, &p, nullptr));
  p = {}; p.period_slots = 5; p.dl_slots = 2; p.dl_symbols = 2; p.ul_slots = 2; p.ul_symbols = 2; // D D [2D..2U] U U: both symbol groups share slot 2
  ASSERT_TRUE(nr_tdd_config_init(&c, &p, nullptr));
  EXPECT_EQ(nr_tdd_slot_direction(&c, 2), NR_TDD_SLOT_MIXED);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 2), 11);
  p = {}; p.period_slots = 6; p.dl_slots = 2; p.dl_symbols = 2; p.ul_slots = 2; p.ul_symbols = 2; // D D [2D] [2U] U U
  ASSERT_TRUE(nr_tdd_config_init(&c, &p, nullptr));
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 2), 13);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 3), 11);
}
TEST(TddSpec, TwoPatternsWithFlexibleSlots) {
  nr_tdd_pattern_t p1{}, p2{};
  p1.period_slots = 5; p1.dl_slots = 2; p1.ul_slots = 1;                  // D D F F U
  p2.period_slots = 5; p2.dl_slots = 3; p2.ul_slots = 1; p2.ul_symbols = 2; // D D D [F..2U] U
  nr_tdd_config_t c{};
  ASSERT_TRUE(nr_tdd_config_init(&c, &p1, &p2));
  EXPECT_EQ(nr_tdd_slot_direction(&c, 2), NR_TDD_SLOT_FLEXIBLE);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 3), NR_TDD_SLOT_FLEXIBLE);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 4), NR_TDD_SLOT_UL);
  EXPECT_EQ(nr_tdd_slot_direction(&c, 7), NR_TDD_SLOT_DL);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 8), 11);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 9), -1);
  EXPECT_EQ(nr_tdd_pdsch_last_symbol(&c, 12), 13); // period 10: slot 2 of pattern 1 again
}
