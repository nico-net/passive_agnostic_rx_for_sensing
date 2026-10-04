#include <gtest/gtest.h>
#include <string>
#include "nr_prach_passive_test_bridge.h"

class PassivePrach : public ::testing::Test {
 protected:
  void SetUp() override { test_prach_init(); test_prach_reset(); }
  void TearDown() override { test_prach_finish(); }
};

TEST_F(PassivePrach, PrachOccasionTable)
{
  test_prach_occasion_t o{};
  // TS 38.211 v17.4.0 Table 6.3.3.2-3, FR1 unpaired; mu=1 slot 19 is subframe 9.
  struct Row { int index, frame, slot, format, symbol, occasions, duration; };
  const Row rows[] = {
      {0, 1, 18, 0, 0, 1, 0},    // long format 0, SFN mod 16 = 1; first 30 kHz slot
      {67, 1, 19, 4, 0, 6, 2},  // A1, two PRACH slots per subframe
      {77, 0, 19, 4, 0, 6, 2},  // A1, SFN mod 1 = 0
      {145, 1, 19, 8, 0, 1, 12},// B4
      {189, 1, 19, 10, 2, 2, 6}// C2
  };
  for (const auto &r : rows) {
    ASSERT_TRUE(test_prach_occasion(r.index, r.frame, r.slot, 1, &o)) << r.index;
    EXPECT_EQ(o.format, r.format) << r.index;
    EXPECT_EQ(o.start_symbol, r.symbol) << r.index;
    EXPECT_EQ(o.num_occasions, r.occasions) << r.index;
    EXPECT_EQ(o.duration, r.duration) << r.index;
  }
  EXPECT_FALSE(test_prach_occasion(0, 0, 18, 1, &o)); // x/y SFN condition
  EXPECT_FALSE(test_prach_occasion(0, 1, 16, 1, &o)); // subframe 9 only
  EXPECT_FALSE(test_prach_occasion(0, 1, 19, 1, &o)); // long starts in first slot
  EXPECT_TRUE(test_prach_occasion(67, 1, 18, 1, &o)); // two PRACH slots in subframe
  EXPECT_FALSE(test_prach_occasion(145, 1, 17, 1, &o));
  EXPECT_FALSE(test_prach_occasion(256, 1, 19, 1, &o));
  // msg1-FDM is broadcast beside the time table: two configured FD entries
  // must both be searched for the same table-derived time occasion.
  test_prach_run_slot(-1, 2, 0);
  test_prach_drain();
  unsigned ru = 0, correlator = 0, exported = 0;
  test_prach_heavy_calls(&ru, &correlator, &exported);
  EXPECT_EQ(ru, 2u);
  EXPECT_EQ(correlator, 2u);
  test_prach_finish();
}

TEST_F(PassivePrach, IdleCellNeverDetects)
{
  test_prach_floor_reset();
  for (int gain : {35, 55, 25})
    for (int i = 0; i < 40; ++i)
      EXPECT_EQ(test_prach_floor(0, gain, 310 + gain * 2 + i % 7), 0);
}

TEST_F(PassivePrach, PreambleAboveFloorDetected)
{
  test_prach_floor_reset();
  for (int i = 0; i < 8; ++i) EXPECT_EQ(test_prach_floor(0, 40, 350 + i % 3), 0);
  EXPECT_EQ(test_prach_floor(0, 40, 470), 1);
  EXPECT_EQ(test_prach_floor(0, 40, 354), 0);
}

TEST_F(PassivePrach, SlotThreadOnlyCopies)
{
  ASSERT_EQ(test_prach_start_calls(), 1u);
  test_prach_run_slot(-1, 1, 0);
  test_prach_drain();
  unsigned slot_calls = 0, worker_calls = 0;
  int same_thread = 1;
  test_prach_threads(&slot_calls, &worker_calls, &same_thread);
  EXPECT_EQ(slot_calls, 1u);
  EXPECT_EQ(worker_calls, 1u);
  EXPECT_EQ(same_thread, 0);
  EXPECT_EQ(test_prach_start_calls(), 1u); // no worker creation in the slot hook
  unsigned ru = 0, correlator = 0, exported = 0;
  test_prach_heavy_calls(&ru, &correlator, &exported);
  EXPECT_EQ(ru, 1u);
  EXPECT_EQ(correlator, 1u);
  EXPECT_EQ(exported, 0u);
  EXPECT_STREQ(test_prach_record(), "");
  test_prach_finish();
}

TEST_F(PassivePrach, WorkerDrainsQueuedOccasions)
{
  test_prach_run_slot(-1, 2, 0); // broadcast msg1-FDM=2: both FD entries run on worker
  test_prach_run_slot(-1, 2, 0);
  test_prach_drain();
  unsigned slot_calls = 0, worker_calls = 0;
  int same_thread = 1;
  test_prach_threads(&slot_calls, &worker_calls, &same_thread);
  EXPECT_EQ(slot_calls, 2u);
  EXPECT_EQ(worker_calls, 2u);
  EXPECT_EQ(same_thread, 0);
  unsigned ru = 0, correlator = 0, exported = 0;
  test_prach_heavy_calls(&ru, &correlator, &exported);
  EXPECT_EQ(ru, 4u); // two msg1-FDM entries in each queued slot
  EXPECT_EQ(correlator, 4u);
  EXPECT_EQ(exported, 0u);
  test_prach_finish();
}

TEST_F(PassivePrach, IgnoreSib1LeavesPrachInert)
{
  test_prach_run_slot(-1, 1, 1);
  unsigned slot_calls = 0, worker_calls = 0;
  int same_thread = 0;
  test_prach_threads(&slot_calls, &worker_calls, &same_thread);
  EXPECT_EQ(worker_calls, 0u);
  EXPECT_EQ(test_prach_skipped(), 0u);
}

TEST_F(PassivePrach, MixedSlotDlSymbolsAreSkipped)
{
  test_prach_run_slot(-1, 1, 2);
  unsigned slot_calls = 0, worker_calls = 0;
  int same_thread = 0;
  test_prach_threads(&slot_calls, &worker_calls, &same_thread);
  EXPECT_EQ(worker_calls, 0u);
  EXPECT_EQ(test_prach_skipped(), 1u);
}

TEST_F(PassivePrach, FddCellIsInactive)
{
  test_prach_run_slot(-1, 1, 4);
  unsigned slot_calls = 0, worker_calls = 0;
  int same_thread = 0;
  test_prach_threads(&slot_calls, &worker_calls, &same_thread);
  EXPECT_EQ(worker_calls, 0u);
}

TEST_F(PassivePrach, ProcessSlotSynthPreamble)
{
  for (int i = 0; i < 8; ++i) EXPECT_EQ(test_prach_floor(0, 0, 0), 0);
  test_prach_run_slot(32, 1, 0);
  test_prach_drain();
  test_prach_finish();
  const std::string record(test_prach_record());
  EXPECT_NE(record.find("\"preamble\":0"), std::string::npos) << record;
  EXPECT_NE(record.find("\"delay_samples\":32"), std::string::npos) << record;
}

TEST_F(PassivePrach, ProcessSlotStitchesPreviousFrame)
{
  for (int i = 0; i < 8; ++i) EXPECT_EQ(test_prach_floor(0, 0, 0), 0);
  test_prach_run_stitched_slot(8);
  test_prach_drain();
  test_prach_finish();
  const std::string record(test_prach_record());
  EXPECT_NE(record.find("\"preamble\":0"), std::string::npos) << record;
  const auto delay = record.find("\"delay_samples\":");
  ASSERT_NE(delay, std::string::npos) << record;
  EXPECT_NEAR(std::stoi(record.substr(delay + 16)), 16, 8); // one TA unit at 30.72 Ms/s, mu=1
}

TEST_F(PassivePrach, PrachMatchesRarRapid)
{
  const uint16_t ra_rnti = 1 + 2 + 14 * 5 + 1120;
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti, 17), 1);
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti, 18), 0);
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti + 1, 17), -1);
  const uint8_t two_rars[] = {
      0xc1, 0, 0, 0, 0, 0, 0, 1,
      0x51, 0, 0, 0, 0, 0, 0, 2};
  test_prach_rar_entry_t entries[2]{};
  ASSERT_EQ(test_prach_rar_entries(two_rars, sizeof(two_rars), entries, 2), 2);
  EXPECT_EQ(entries[0].rapid, 1);
  EXPECT_EQ(entries[1].rapid, 17);
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti, entries[1].rapid), 1);
  EXPECT_EQ(test_prach_rar_age(20479, 0, 20), 1);
  EXPECT_EQ(test_prach_rar_age(20479 + 20480, 1, 20), 2);
  EXPECT_EQ(test_prach_rar_age(200, 199, 20), 20479);
  EXPECT_EQ(test_prach_ra_slot(19, 1, 4), 19);
  EXPECT_EQ(test_prach_ra_slot(19, 1, 0), 9);
}

TEST_F(PassivePrach, PrachDetectsSynthPreamble)
{
  const auto zero = test_prach_synth(7, 0);
  EXPECT_EQ(zero.delay, 0);
  const auto found = test_prach_synth(7, 8);
  EXPECT_EQ(found.preamble, 0);
  EXPECT_NEAR(found.delay, 4, 1);
  EXPECT_GT(found.energy, 0);
}

TEST_F(PassivePrach, PrachRsRecordFields)
{
  char line[768]{};
  test_prach_json(1, line, sizeof(line));
  const std::string matched(line);
  EXPECT_NE(matched.find("\"schema\":2,\"type\":\"rs\",\"source\":\"prach\""), std::string::npos);
  EXPECT_NE(matched.find("\"abs_slot\":1234"), std::string::npos);
  EXPECT_NE(matched.find("\"epoch\":4,\"identity_gen\":2,\"dedicated_epoch\":3"), std::string::npos);
  EXPECT_NE(matched.find("\"preamble\":17,\"energy\":420,\"delay_samples\":32"), std::string::npos);
  EXPECT_NE(matched.find("\"occasion\":{\"frame\":61,\"slot\":14,\"fdm_index\":1,\"symbol\":2}"), std::string::npos);
  EXPECT_NE(matched.find("\"rar_match\":true,\"rar_rapid\":17,\"rar_ta\":20"), std::string::npos);
  test_prach_json(0, line, sizeof(line));
  EXPECT_NE(std::string(line).find("\"rar_match\":false"), std::string::npos);
  test_prach_json(-1, line, sizeof(line));
  EXPECT_NE(std::string(line).find("\"rar_match\":null,\"rar_rapid\":null,\"rar_ta\":null"), std::string::npos);
}
