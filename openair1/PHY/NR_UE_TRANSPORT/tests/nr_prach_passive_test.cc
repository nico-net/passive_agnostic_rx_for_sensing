#include <gtest/gtest.h>
#include <unistd.h>
#include <string>
#include <string.h>
#include <thread>
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

TEST_F(PassivePrach, DiagExplainsMissingRecords)
{
  // idle cell: occasions are correlated but the best margin stays far below the 8 dB a Msg1 needs
  test_prach_floor_reset();
  for (int i = 0; i < 40; ++i) test_prach_floor(0, 40, 350 + i % 5);
  unsigned long det = 0;
  int margin = 0;
  test_prach_diag(&det, &margin);
  EXPECT_EQ(det, 0ul);
  EXPECT_GT(margin, 0);
  EXPECT_LT(margin, 80);
  EXPECT_EQ(test_prach_floor(0, 40, 470), 1);
  test_prach_diag(&det, &margin);
  EXPECT_EQ(det, 1ul);
  EXPECT_GE(margin, 80);
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
  char line[2048]{};
  test_prach_json(1, line, sizeof(line));
  const std::string matched(line);
  EXPECT_NE(matched.find("\"source\":\"prach\""), std::string::npos);
  EXPECT_NE(matched.find("\"abs_slot\":1234"), std::string::npos);
  EXPECT_NE(matched.find("\"epoch\":4,\"identity_gen\":2,\"dedicated_epoch\":3"), std::string::npos);
  EXPECT_NE(matched.find("\"delay_samples\":32"), std::string::npos);
  EXPECT_NE(matched.find("\"occasion\":{\"frame\":61,\"slot\":14,\"fdm_index\":1,\"symbol\":2}"), std::string::npos);
  EXPECT_NE(matched.find("\"rar_match\":true,\"rar_rapid\":17,\"rar_ta\":20"), std::string::npos);
  test_prach_json(0, line, sizeof(line));
  EXPECT_NE(std::string(line).find("\"rar_match\":false"), std::string::npos);
  test_prach_json(-1, line, sizeof(line));
  EXPECT_NE(std::string(line).find("\"rar_match\":null,\"rar_rapid\":null,\"rar_ta\":null"), std::string::npos);
}

/* ---- mpci CELLFEAT: every cell has its own PRACH state and worker (cell A's output does not change when cell B runs) ---- */
namespace {
void seed_floor(int cell)
{
  test_prach_bind(cell);
  for (int i = 0; i < 8; ++i) test_prach_floor(0, 0, 0);
}
/* the run time is the only field that differs between two runs of the same input */
std::string stable(std::string r)
{
  const auto p = r.find("\"t_mono_ns\":");
  if (p != std::string::npos) r.erase(p, r.find(',', p) - p + 1);
  return r;
}
std::string solo_record(int cell, int delay)
{
  test_prach_bind(cell);
  test_prach_reset();
  seed_floor(cell);
  test_prach_run_slot(delay, 1, 0);
  test_prach_drain();
  test_prach_finish();
  return stable(test_prach_record());
}
}

TEST(PassivePrachCells, InterleavedOnOneThreadEqualSoloRuns)
{
  test_prach_init();
  const std::string a = solo_record(0, 32), b = solo_record(1, 8);
  ASSERT_NE(a.find("\"source\":\"prach\""), std::string::npos) << a;
  ASSERT_NE(b.find("\"source\":\"prach\""), std::string::npos) << b;
  ASSERT_NE(a, b) << "the two cells must see different input";
  for (int c = 0; c < 2; ++c) { test_prach_bind(c); test_prach_reset(); seed_floor(c); }
  test_prach_bind(0);
  test_prach_run_slot(32, 1, 0);
  test_prach_bind(1);
  test_prach_run_slot(8, 1, 0);
  test_prach_drain();
  test_prach_finish();
  const std::string b2 = stable(test_prach_record());
  test_prach_bind(0);
  test_prach_drain();
  test_prach_finish();
  const std::string a2 = stable(test_prach_record());
  EXPECT_EQ(a2, a);
  EXPECT_EQ(b2, b);
  test_prach_bind(0);
}

TEST(PassivePrachCells, TwoBoundThreadsEqualSoloRuns)
{
  test_prach_init();
  const std::string a = solo_record(0, 32), b = solo_record(1, 8);
  for (int c = 0; c < 2; ++c) { test_prach_bind(c); test_prach_reset(); seed_floor(c); }
  test_prach_bind(0);
  std::string a2, b2;
  auto run = [](int cell, int delay, std::string *out) {
    test_prach_bind(cell);
    test_prach_run_slot(delay, 1, 0);
    test_prach_drain();
    test_prach_finish();
    *out = stable(test_prach_record());
  };
  std::thread t0(run, 0, 32, &a2), t1(run, 1, 8, &b2);
  t0.join();
  t1.join();
  EXPECT_EQ(a2, a);
  EXPECT_EQ(b2, b);
}

TEST(PassivePrachCells, SkippedOccasionIsCountedOnItsCellOnly)
{
  test_prach_init();
  for (int c = 0; c < 2; ++c) { test_prach_bind(c); test_prach_reset(); }
  test_prach_bind(1);
  const uint64_t before0 = (test_prach_bind(0), test_prach_skipped());
  test_prach_bind(1);
  test_prach_run_slot(-1, 1, 2); /* mixed slot with DL symbols: skipped */
  EXPECT_EQ(test_prach_skipped(), 1u);
  test_prach_bind(0);
  EXPECT_EQ(test_prach_skipped(), before0) << "the skip of cell 1 must not show on cell 0";
  test_prach_finish();
  test_prach_bind(1);
  test_prach_finish();
  test_prach_bind(0);
}

/* MPCI-INT: a paused (retired) cell's PRACH pipeline does nothing and counts nothing; the other cell is unaffected; resume feeds again. */
TEST(PassivePrachCells, PausedCellIgnoresSlotsAndResumes)
{
  test_prach_init();
  for (int c = 0; c < 2; ++c) { test_prach_bind(c); test_prach_reset(); }
  test_prach_bind(1);
  test_prach_pause(1, 1);
  test_prach_run_slot(-1, 1, 2); /* would be a skipped occasion */
  EXPECT_EQ(test_prach_skipped(), 0u) << "paused cell: slot ignored";
  test_prach_bind(0);
  test_prach_run_slot(-1, 1, 2);
  EXPECT_EQ(test_prach_skipped(), 1u) << "cell 0 keeps working while cell 1 is paused";
  test_prach_bind(1);
  test_prach_pause(1, 0);
  test_prach_run_slot(-1, 1, 2);
  EXPECT_EQ(test_prach_skipped(), 1u) << "resumed";
  test_prach_finish();
  test_prach_bind(0);
  test_prach_finish();
}

/* ---- real captured IQ + a synthetic preamble through the production slot hook ---- */
namespace {
const char *kCapture = "/home/nicola/NICOLA/ota_runs/raw_pci415_day_4s_1024_1001";
/* pci 415 SIB1 (rx.log): DDDDDDDSUU, prach-ConfigurationIndex 3 (format 0, even frames, subframe 9), msg1-FDM 1, msg1-FrequencyStart 8;
 * 217 PRB at 30 kHz, 122.88 Ms/s, 4 RX. Root / zeroCorrelationZoneConfig / UL BWP start are not in the logs: any valid values test the chain. */
test_prach_real_cfg_t pci415(int preamble, int delay, double snr)
{
  test_prach_real_cfg_t c{};
  c.capture_dir = kCapture;
  c.t0 = 122160;       /* DL start of a 5 ms TDD period in the capture (power edge) */
  c.n_noise = 10;
  c.config_index = 3;
  c.root = 700; c.zczc = 8; c.restricted = 0;
  c.msg1_fstart = 8; c.msg1_fdm = 0; c.ul_bwp_start = 81;
  c.preamble = preamble; c.delay = delay; c.snr_db = snr;
  return c;
}
bool have_capture() { return access((std::string(kCapture) + "/rx0.sc16").c_str(), R_OK) == 0; }
}

/* The record's integer field after `key`, -1 when absent. */
int field(const char *record, const char *key)
{
  const char *p = strstr(record, key);
  return p ? atoi(p + strlen(key)) : -1;
}

/* A format-0 preamble at the cell's exact parameters, 4 RX, added to the real capture at 0..-10 dB per antenna sample (the window is 122.88 Ms/s wide
 * and the preamble 1.05 MHz, so these are +20.7 dB higher in-band): the production slot hook must report the right preamble index and delay. */
TEST_F(PassivePrach, RealIqDetectsInjectedPreamble)
{
  if (!have_capture()) GTEST_SKIP() << "capture not on this host";
  const int preambles[] = {0, 17, 40, 63};
  const int delays[] = {0, 300, 1500};
  for (double snr : {-10.0, -5.0, 0.0})
    for (int v : preambles)
      for (int d : delays) {
        test_prach_real_out_t o{};
        auto c = pci415(v, d, snr);
        ASSERT_TRUE(test_prach_real_iq(&c, &o));
        EXPECT_EQ(o.records, 1) << "preamble " << v << " delay " << d << " snr " << snr;
        EXPECT_EQ(field(o.record, "\"preamble\":"), v) << "snr " << snr << " delay " << d;
        EXPECT_NEAR(field(o.record, "\"delay_samples\":"), d, 100) << "preamble " << v << " snr " << snr; /* one correlator bin = 96 samples */
        EXPECT_GE(o.peak_last - o.floor_last, 80);
        test_prach_reset();
      }
}

/* The capture is not carrier-offset corrected (capture_info.json receiver_corrections_applied=false; the cell shows -4827 Hz). The receiver
 * removes it per consumer; the PRACH snapshot has to as well, or the peak lands on another preamble index and delay. */
TEST_F(PassivePrach, RealIqDetectsPreambleUnderCarrierOffset)
{
  if (!have_capture()) GTEST_SKIP() << "capture not on this host";
  for (double cfo : {-4827.0, 700.0, 2500.0})
    for (int v : {17, 40}) {
      test_prach_real_out_t o{};
      auto c = pci415(v, 300, -5.0);
      c.cfo_hz = cfo;
      c.rx_fo_hz = cfo;
      ASSERT_TRUE(test_prach_real_iq(&c, &o));
      printf("PRACH_COST_FO us_per_occasion %lu cfo %.0f peak %d..%d last %d floor %d occ %lu det %lu rec %d\n", o.us_per_occasion, cfo, o.peak_min, o.peak_max, o.peak_last, o.floor_last, o.occasions, o.detections, o.records);
      EXPECT_EQ(o.records, 1) << cfo;
      EXPECT_EQ(field(o.record, "\"preamble\":"), v) << "cfo " << cfo;
      EXPECT_NEAR(field(o.record, "\"delay_samples\":"), 300, 150) << "cfo " << cfo;
      test_prach_reset();
    }
}

/* Noise only: every occasion reports its peak and the floor; an idle cell neither detects nor produces zero/constant peaks. */
TEST_F(PassivePrach, RealIqIdleCellStatistics)
{
  if (!have_capture()) GTEST_SKIP() << "capture not on this host";
  test_prach_real_out_t o{};
  auto c = pci415(-1, 0, 0);
  c.n_noise = 60;
  ASSERT_TRUE(test_prach_real_iq(&c, &o));
  EXPECT_EQ(o.records, 0);
  printf("PRACH_COST us_per_occasion %lu\n", (unsigned long)o.us_per_occasion);
  EXPECT_EQ(o.detections, 0ul);
  EXPECT_EQ(o.peak_zero, 0ul);
  EXPECT_EQ(o.snap_zero, 0ul) << "the snapshot of the UL window must hold the capture's samples";
  EXPECT_GT(o.snap_db, 250) << "UL window noise is ~21 LSB rms = 26 dB";
  EXPECT_GT(o.peak_max, o.peak_min) << "constant peaks mean a dead correlator";
  EXPECT_GT(o.floor_last, 0);
  EXPECT_LT(o.peak_max - o.floor_last, 80);
}

/* RAR look-back: below the blind threshold (8 dB over the idle floor of the best of 64 preambles x delays) but above the single-hypothesis threshold, the
 * preamble is found only when a RAR names its RAPID on that occasion (TS 38.321 5.1.3). Per-antenna sample SNR: the level sits between the two thresholds. */
TEST_F(PassivePrach, RealIqLookbackFindsWhatBlindMisses)
{
  if (!have_capture()) GTEST_SKIP() << "capture not on this host";
  /* SNR search is not part of the product: the two levels bracket the thresholds measured on this capture */
  bool found_window = false;
  for (double snr : {-42.0, -40.0, -38.0, -36.0, -34.0, -33.0, -32.0, -31.0}) {
    test_prach_real_out_t blind{}, look{};
    auto c = pci415(17, 300, snr);
    ASSERT_TRUE(test_prach_real_iq(&c, &blind));
    test_prach_reset();
    c.rar_on = 1; c.rar_rapid = 17; c.rar_ta = 20; c.rar_tc = 0x4a5b;
    ASSERT_TRUE(test_prach_real_iq(&c, &look));
    test_prach_reset();
    printf("LB snr %.0f blind_records %d lookback_records %d tried %lu found %lu noring %lu peak %d floor %d\n", snr, blind.records, look.records, look.lb_tried, look.lb_found, look.lb_noring, look.peak_last, look.floor_last);
    if (blind.records == 0 && look.records == 1) {
      found_window = true;
      EXPECT_EQ(field(look.record, "\"preamble\":"), 17);
      EXPECT_EQ(field(look.record, "\"rar_ta\":"), 20);
      EXPECT_EQ(field(look.record, "\"rnti\":"), 0x4a5b);
      EXPECT_NE(std::string(look.record).find("\"rar_match\":true"), std::string::npos) << look.record;
    }
  }
  EXPECT_TRUE(found_window) << "no SNR between the blind and the look-back threshold";
}

/* A RAR for a preamble that is NOT on the air must not be invented by the look-back (1e-3 false alarm per attempt, 4 RX, noise only). */
TEST_F(PassivePrach, RealIqLookbackAbsentPreambleStaysAbsent)
{
  if (!have_capture()) GTEST_SKIP() << "capture not on this host";
  int found = 0, tried = 0;
  for (int v = 0; v < 12; ++v) {
    test_prach_real_out_t o{};
    auto c = pci415(-1, 0, 0);
    c.n_noise = 8;
    c.rar_on = 1; c.rar_rapid = v * 5; c.rar_ta = 10; c.rar_tc = 1;
    ASSERT_TRUE(test_prach_real_iq(&c, &o)); /* nothing injected, a RAR names a preamble anyway */
    found += o.lb_found; tried += o.lb_tried;
    test_prach_reset();
  }
  EXPECT_EQ(found, 0);
  EXPECT_EQ(tried, 12) << "every RAR must have been tested on its retained occasion";
}

/* A receiver chain that delivers the UL window 12-20 dB weaker than the raw capture (the replay does: snapshot power 17.5 dB vs 29.5 dB in the capture) must still
 * correlate: the fixed-point energy used to collapse to 0 for every occasion (peak == floor == 0, nothing ever detected). */
TEST_F(PassivePrach, RealIqWeakInputStillCorrelates)
{
  if (!have_capture()) GTEST_SKIP() << "capture not on this host";
  for (double scale : {0.25, 0.1}) {
    test_prach_real_out_t idle{};
    auto c = pci415(-1, 0, 0);
    c.n_noise = 40;
    c.input_scale = scale;
    ASSERT_TRUE(test_prach_real_iq(&c, &idle));
    EXPECT_EQ(idle.peak_zero, 0ul) << "scale " << scale;
    EXPECT_GT(idle.peak_max, idle.peak_min) << "scale " << scale;
    EXPECT_EQ(idle.records, 0);
    test_prach_reset();
    for (int v : {0, 17, 63}) {
      test_prach_real_out_t o{};
      auto d = pci415(v, 300, -10.0);
      d.input_scale = scale;
      ASSERT_TRUE(test_prach_real_iq(&d, &o));
      EXPECT_EQ(o.records, 1) << "scale " << scale << " preamble " << v;
      EXPECT_EQ(field(o.record, "\"preamble\":"), v) << "scale " << scale;
      EXPECT_GE(o.peak_last - o.floor_last, 80) << "input-referred energy keeps the margin comparable";
      test_prach_reset();
    }
  }
}
