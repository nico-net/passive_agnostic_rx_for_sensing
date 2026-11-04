// Traffic-mode decode policy: which grants are decoded, and when a solved RNTI is reopened.
#include <gtest/gtest.h>
extern "C" {
#include "nr_traffic_decode.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
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
  for (int i = 0; i < 31; i++) EXPECT_FALSE(nr_traffic_decode_wanted(1, 0x109, C, NR_LAYOUT_SOLVED, true, S, false));
  EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x109, C, NR_LAYOUT_SOLVED, true, S, false));
}
TEST(TrafficDecode, DirectionsAreIndependent)
{
  for (int i = 0; i < 3; i++) EXPECT_FALSE(nr_traffic_decode_result(0, 0x10a, 0, 15.f, false));
  EXPECT_FALSE(nr_traffic_decode_result(1, 0x10a, 0, 15.f, false));
  EXPECT_TRUE(nr_traffic_decode_result(0, 0x10a, 0, 15.f, false));
}
TEST(TrafficDecode, SensingModeDecodesEveryGrant)
{
  /* plan S7: in SENSING every grant is decoded to the end (the unit gate acts on the CFR tap, not on the decode) */
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  for (nr_layout_state_t ls : {NR_LAYOUT_FIXED, NR_LAYOUT_CANDIDATE, NR_LAYOUT_UNSOLVED, NR_LAYOUT_SOLVED})
    for (int i = 0; i < 40; i++) {
      EXPECT_TRUE(nr_traffic_decode_wanted(0, 0x10b, C, ls, false, S + i, false)) << ls << " " << i;
      EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10b, C, ls, true, S + i, false)) << ls << " " << i;
    }
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  EXPECT_FALSE(nr_traffic_decode_wanted(0, 0x10c, C, NR_LAYOUT_FIXED, false, S, false)); /* the traffic policy is unchanged outside the sensing mode */
}
