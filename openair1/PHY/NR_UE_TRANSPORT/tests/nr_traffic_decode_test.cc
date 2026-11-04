// Traffic-mode decode policy: which grants are decoded, and when a solved RNTI is reopened.
#include <gtest/gtest.h>
extern "C" {
#include "nr_traffic_decode.h"
#include "nr_pdcch_blind_monitor.h"
}
namespace {
constexpr uint64_t S = 1000000000ull;
constexpr int C = NR_BLIND_RNTI_CLASS_C;
}
TEST(TrafficDecode, NonCRntiClassesAlwaysDecoded)
{
  for (int c : {NR_BLIND_RNTI_CLASS_SI, NR_BLIND_RNTI_CLASS_RA, NR_BLIND_RNTI_CLASS_P, NR_BLIND_RNTI_CLASS_TC})
    for (nr_layout_state_t ls : {NR_LAYOUT_FIXED, NR_LAYOUT_UNSOLVED})
      EXPECT_TRUE(nr_traffic_decode_wanted(0, 0x100, c, ls, true, S, false));
}
TEST(TrafficDecode, FixedLayoutNotDecoded)
{
  for (int i = 0; i < 100; i++) EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x101, C, NR_LAYOUT_FIXED, false, S + i, true));
}
TEST(TrafficDecode, CandidateUnsolvedNotDecoded)
{
  for (int i = 0; i < 40; i++) {
    EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x102, C, NR_LAYOUT_CANDIDATE, false, S, true));
    EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x102, C, NR_LAYOUT_UNSOLVED, false, S, true));
  }
}
TEST(TrafficDecode, SolvedChecksEvery32Grants)
{
  for (int round = 0; round < 2; round++) {
    for (int i = 0; i < 31; i++) EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x103, C, NR_LAYOUT_SOLVED, false, S, false)) << i;
    EXPECT_TRUE(nr_traffic_decode_wanted(0, 0x103, C, NR_LAYOUT_SOLVED, false, S, false));
    nr_traffic_decode_result(0, 0x103, 0, 20.f, true);
  }
}
TEST(TrafficDecode, SolvedChecksAfter2s)
{
  EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x104, C, NR_LAYOUT_SOLVED, false, S, false));
  EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x104, C, NR_LAYOUT_SOLVED, false, S + 1999999999ull, false));
  EXPECT_TRUE(nr_traffic_decode_wanted(0, 0x104, C, NR_LAYOUT_SOLVED, false, S + 2 * S, false));
}
TEST(TrafficDecode, HrvAlertForcesCheck)
{
  EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x105, C, NR_LAYOUT_SOLVED, false, S, false));
  EXPECT_TRUE(nr_traffic_decode_wanted(0, 0x105, C, NR_LAYOUT_SOLVED, false, S, true));
}
TEST(TrafficDecode, ReopenAfter4CountedFails)
{
  for (int i = 0; i < 3; i++) EXPECT_FALSE(nr_traffic_decode_result(0, 0x106, 0, 15.f, false));
  EXPECT_TRUE(nr_traffic_decode_result(0, 0x106, 0, 15.f, false));
}
TEST(TrafficDecode, LowSnrFailDoesNotCountTowardReopen)
{
  for (int i = 0; i < 10; i++) EXPECT_FALSE(nr_traffic_decode_result(0, 0x107, 0, 5.f, false));
  for (int i = 0; i < 10; i++) EXPECT_FALSE(nr_traffic_decode_result(0, 0x107, 1, 20.f, false)); // rv != 0 neither
}
TEST(TrafficDecode, PassResetsFailCount)
{
  for (int i = 0; i < 3; i++) EXPECT_FALSE(nr_traffic_decode_result(0, 0x108, 0, 15.f, false));
  EXPECT_FALSE(nr_traffic_decode_result(0, 0x108, 0, 15.f, true));
  for (int i = 0; i < 3; i++) EXPECT_FALSE(nr_traffic_decode_result(0, 0x108, 0, 15.f, false));
}
TEST(TrafficDecode, UlDecodesUntilTableLearned)
{
  for (int i = 0; i < 5; i++) EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x109, C, NR_LAYOUT_SOLVED, false, S, false));
}
// E4 rule 1: PUSCH is decoded until the UL layout is locked AND the MCS table is known; then never (lock lost: again).
TEST(TrafficDecode, UlLockedAndTableKnownNoDecode)
{
  for (int i = 0; i < 100; i++) EXPECT_FALSE(nr_traffic_decode_wanted(1, 0x10b, C, NR_LAYOUT_SOLVED, true, S + i, false)) << i;
  for (int i = 0; i < 100; i++) EXPECT_FALSE(nr_traffic_decode_wanted(1, 0x10c, C, NR_LAYOUT_FIXED, true, S + i, true)) << i;   // 0_0: the layout needs no lock
}
TEST(TrafficDecode, UlUnlockedDecodesEvenWithTable)
{
  for (int i = 0; i < 100; i++) EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10d, C, NR_LAYOUT_UNSOLVED, true, S, false)) << i;
}
TEST(TrafficDecode, UlLockLostResumes)
{
  EXPECT_FALSE(nr_traffic_decode_wanted(1, 0x10e, C, NR_LAYOUT_SOLVED, true, S, false));
  EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10e, C, NR_LAYOUT_UNSOLVED, true, S, false));
  EXPECT_FALSE(nr_traffic_decode_wanted(1, 0x10e, C, NR_LAYOUT_SOLVED, true, S, false));   // relocked
  EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10e, C, NR_LAYOUT_SOLVED, false, S, false));   // table forgotten (K failures / identity change)
}
TEST(TrafficDecode, UlOtherClassesStillDecoded)
{
  EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10f, NR_BLIND_RNTI_CLASS_RA, NR_LAYOUT_SOLVED, true, S, false));
}
TEST(TrafficDecode, DirectionsAreIndependent)
{
  for (int i = 0; i < 3; i++) EXPECT_FALSE(nr_traffic_decode_result(0, 0x10a, 0, 15.f, false));
  EXPECT_FALSE(nr_traffic_decode_result(1, 0x10a, 0, 15.f, false));
  EXPECT_TRUE(nr_traffic_decode_result(0, 0x10a, 0, 15.f, false));
}

// ---- E4 probe triage ----
namespace {
constexpr float OK_SNR = 40.f;   // supports any MCS of table 0
bool probe(uint16_t r, int seen, int alive = 1, bool fam = false, float snr = OK_SNR, int mcs = 10, int tbl = 0, int dir = 0)
{
  return nr_traffic_probe_wanted(0, r, dir, seen, alive, fam, snr, mcs, tbl);
}
// the grants between two probes of one RNTI, every probe failing (or passing when pass)
int next_gap(uint16_t r, bool pass)
{
  for (int g = 1; g < 100; g++)
    if (probe(r, 5)) { nr_traffic_probe_result(0, r, pass); return g; }
  return -1;
}
}
TEST(TrafficProbe, SingleDciNoProbe)
{
  EXPECT_FALSE(probe(0x200, 1));
  EXPECT_FALSE(probe(0x200, 0));
}
TEST(TrafficProbe, SecondDciProbes)
{
  EXPECT_TRUE(probe(0x201, 2));
  EXPECT_TRUE(probe(0x202, 7));
}
TEST(TrafficProbe, FamilyMatchProbesOnFirstDci)
{
  EXPECT_TRUE(probe(0x203, 1, 1, true));
}
TEST(TrafficProbe, NoAliveSplitNoProbe)
{
  EXPECT_FALSE(probe(0x204, 9, 0));
  EXPECT_FALSE(probe(0x204, 9, 0, true));
}
TEST(TrafficProbe, LowSnrNoProbe)
{
  EXPECT_FALSE(probe(0x205, 9, 1, false, 0.f, 27));      // 64QAM at high rate needs ~20 dB: more than the margin below
  EXPECT_TRUE(probe(0x205, 9, 1, false, 0.f, 0));        // QPSK 120/1024 is decodable at 0 dB
  EXPECT_TRUE(probe(0x205, 9, 1, false, -100.f, 27));    // no SNR estimate: never gated
}
TEST(TrafficProbe, BackoffDoublesAndResets)
{
  const uint16_t r = 0x206;
  EXPECT_TRUE(probe(r, 5));                       // first probe: no history
  nr_traffic_probe_result(0, r, false);
  const int want[] = {2, 4, 8, 16, 16};
  for (int w : want) EXPECT_EQ(next_gap(r, false), w);
  EXPECT_EQ(next_gap(r, true), 16);               // a pass at the 16-gap
  EXPECT_EQ(next_gap(r, false), 1);               // reset: every grant again
  EXPECT_EQ(next_gap(r, false), 2);
}
TEST(TrafficProbe, OneFailurePerGrantNotPerVariant)
{
  const uint16_t r = 0x207;
  EXPECT_TRUE(probe(r, 5));
  for (int v = 0; v < 24; v++) nr_traffic_probe_result(0, r, false);   // 24 variants of ONE grant fail
  EXPECT_EQ(next_gap(r, false), 2);
}
TEST(TrafficProbe, PassBeforeSiblingFailuresStaysReset)
{
  const uint16_t r = 0x208;
  EXPECT_TRUE(probe(r, 5));
  nr_traffic_probe_result(0, r, false);
  EXPECT_EQ(next_gap(r, true), 2);
  nr_traffic_probe_result(0, r, false);   // late failing sibling of the passing grant
  EXPECT_EQ(next_gap(r, false), 1);
}
TEST(TrafficProbe, RntisAndDirectionsIndependent)
{
  EXPECT_TRUE(probe(0x209, 5));
  nr_traffic_probe_result(0, 0x209, false);
  EXPECT_TRUE(probe(0x20a, 5));
  EXPECT_TRUE(probe(0x209, 5, 1, false, OK_SNR, 10, 0, 1));   // UL entry of the same RNTI
}
TEST(TrafficProbe, ResetForgetsBackoff)
{
  EXPECT_TRUE(probe(0x20b, 5));
  nr_traffic_probe_result(0, 0x20b, false);
  EXPECT_FALSE(probe(0x20b, 5));
  nr_traffic_decode_reset();
  EXPECT_TRUE(probe(0x20b, 5));
}
TEST(TrafficProbe, DciCountPerRntiAndDirection)
{
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20c, true), 1);
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20c, true), 2);
  EXPECT_EQ(nr_traffic_note_dci(1, 0x20c, true), 1);
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20d, true), 1);
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20c, false), 2);   // a duplicate is not counted
  nr_traffic_decode_reset();
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20c, true), 1);
}
// UL decodes that rule 1 still wants follow the same backoff; a CRC pass resets it
TEST(TrafficProbe, UlDecodeBackoff)
{
  const uint16_t r = 0x20e;
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_SOLVED, false, S, false));
  nr_traffic_probe_result(1, r, false);
  EXPECT_FALSE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_SOLVED, false, S, false));
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_SOLVED, false, S, false));
  nr_traffic_probe_result(1, r, true);
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_SOLVED, false, S, false));
}
