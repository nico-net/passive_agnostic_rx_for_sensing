// Traffic-mode decode policy: which grants are decoded, and when a solved RNTI is reopened.
#include <gtest/gtest.h>
extern "C" {
#include "nr_traffic_decode.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
#include "nr_sense_unit.h"
#include "common/utils/nr_cell.h"
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
  nr_traffic_decode_reset();
  int n = 0, m = 0;   // only the cell's 1-in-32 check (UlLockedChecksOneIn32OfTheCell)
  for (int i = 0; i < 64; i++) n += nr_traffic_decode_wanted(1, 0x10b, C, NR_LAYOUT_SOLVED, true, S + i, false);
  for (int i = 0; i < 64; i++) m += nr_traffic_decode_wanted(1, 0x10c, C, NR_LAYOUT_FIXED, true, S + i, true);   // 0_0: the layout needs no lock
  EXPECT_EQ(n, 2);
  EXPECT_EQ(m, 2);
}
TEST(TrafficDecode, UlUnlockedDecodesEvenWithTable)
{
  for (int i = 0; i < 100; i++) EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10d, C, NR_LAYOUT_UNSOLVED, true, S, false)) << i;
}
TEST(TrafficDecode, UlLockLostResumes)
{
  nr_traffic_decode_reset();
  EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10e, C, NR_LAYOUT_SOLVED, true, S, false));    // the cell's first grant is a check (counter 0)
  EXPECT_FALSE(nr_traffic_decode_wanted(1, 0x10e, C, NR_LAYOUT_SOLVED, true, S, false));
  EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x10e, C, NR_LAYOUT_UNSOLVED, true, S, false));  // lock lost
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
// grants until the next wanted UL decode of an unlocked RNTI (the backoff), the decode then failing (or passing)
int next_gap(uint16_t r, bool pass)
{
  for (int g = 1; g < 100; g++)
    if (nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_UNSOLVED, false, S, false)) { nr_traffic_probe_result(1, r, pass); return g; }
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
TEST(TrafficProbe, NoDlBackoff)
{
  for (int i = 0; i < 50; i++) EXPECT_TRUE(probe(0x206, 5)) << i;   // the solver's own search back-off rate-limits; E4 adds none
}
// UL decodes wanted before the lock follow an exponential backoff: 1 grant in 2^k, k <= 4; a pass resets it
TEST(TrafficProbe, UlBackoffDoublesAndResets)
{
  const uint16_t r = 0x207;
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_UNSOLVED, false, S, false));
  nr_traffic_probe_result(1, r, false);
  const int want[] = {2, 4, 8, 16, 16};
  for (int w : want) EXPECT_EQ(next_gap(r, false), w);
  EXPECT_EQ(next_gap(r, true), 16);
  EXPECT_EQ(next_gap(r, false), 1);
  EXPECT_EQ(next_gap(r, false), 2);
}
TEST(TrafficProbe, UlOneFailurePerGrantNotPerVariant)
{
  const uint16_t r = 0x208;
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_UNSOLVED, false, S, false));
  for (int v = 0; v < 24; v++) nr_traffic_probe_result(1, r, false);
  EXPECT_EQ(next_gap(r, false), 2);
}
TEST(TrafficProbe, UlPassBeforeSiblingFailuresStaysReset)
{
  const uint16_t r = 0x209;
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_UNSOLVED, false, S, false));
  nr_traffic_probe_result(1, r, false);
  EXPECT_EQ(next_gap(r, true), 2);
  nr_traffic_probe_result(1, r, false);
  EXPECT_EQ(next_gap(r, false), 1);
}
TEST(TrafficProbe, ResetForgetsBackoffAndDciCounts)
{
  const uint16_t r = 0x20b;
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_UNSOLVED, false, S, false));
  nr_traffic_probe_result(1, r, false);
  EXPECT_FALSE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_UNSOLVED, false, S, false));
  EXPECT_EQ(nr_traffic_note_dci(0, r, true), 1);
  nr_traffic_decode_reset();
  EXPECT_TRUE(nr_traffic_decode_wanted(1, r, C, NR_LAYOUT_UNSOLVED, false, S, false));
  EXPECT_EQ(nr_traffic_note_dci(0, r, true), 1);
}
TEST(TrafficProbe, DciCountPerRntiAndDirection)
{
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20c, true), 1);
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20c, true), 2);
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20c, false), 2);   // a duplicate is not counted
  EXPECT_EQ(nr_traffic_note_dci(1, 0x20c, true), 1);
  EXPECT_EQ(nr_traffic_note_dci(0, 0x20d, true), 1);
}
// I1: the busy cell's other RNTIs do not evict a frequent RNTI's count
TEST(TrafficProbe, FrequentRntiNeverEvictedAmong1500)
{
  nr_traffic_decode_reset();
  const uint16_t hot = 0x3000;
  int expect = 0;
  for (int i = 0; i < 1500; i++) {
    nr_traffic_note_dci(0, (uint16_t)(0x4000 + i), true);
    if (i % 3 == 0) expect = nr_traffic_note_dci(0, hot, true);
  }
  EXPECT_EQ(nr_traffic_note_dci(0, hot, false), expect);
  EXPECT_EQ(expect, 500);
  // an RNTI not seen for more than 1024 others is evicted (LRU): starts again
  nr_traffic_note_dci(0, 0x2000, true);
  for (int i = 0; i < 1100; i++) nr_traffic_note_dci(0, (uint16_t)(0x5000 + i), true);
  EXPECT_EQ(nr_traffic_note_dci(0, 0x2000, true), 1);
}
// C1: after lock + table, 1 UL grant in 32 of the CELL is still decoded (its CRC verdicts reopen a stale lock/table)
TEST(TrafficProbe, UlLockedChecksOneIn32OfTheCell)
{
  nr_traffic_decode_reset();
  int n = 0;
  for (int i = 0; i < 32 * 40; i++) n += nr_traffic_decode_wanted(1, (uint16_t)(0x600 + i % 7), C, NR_LAYOUT_SOLVED, true, S, false);
  EXPECT_EQ(n, 40);
  // 8 consecutive check failures (RELOCK_FAILS of the lock) are reached within 8 * 32 cell grants
  int checks = 0, grants = 0;
  while (checks < 8 && grants < 1000) { grants++; checks += nr_traffic_decode_wanted(1, 0x610, C, NR_LAYOUT_FIXED, true, S, false); }
  EXPECT_LE(grants, 8 * 32);
}
TEST(TrafficProbe, UlBackoffDoesNotThrottleTheLockedCheck)
{
  nr_traffic_decode_reset();
  nr_traffic_probe_result(1, 0x620, false);
  int n = 0;
  for (int i = 0; i < 64; i++) n += nr_traffic_decode_wanted(1, 0x620, C, NR_LAYOUT_SOLVED, true, S, false);
  EXPECT_EQ(n, 2);
}

TEST(TrafficDecode, SensingEnabledUnitDecodesEveryGrantDisabledFollowsPolicyPlusSample)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_unit_reset(0);
  nr_cell_bind(0);
  /* a disabled unit (no row): FIXED layout is not decoded by the traffic policy; one grant in 32 is a sampled check decode */
  int n = 0;
  for (int i = 0; i < 64; i++) n += nr_traffic_decode_wanted(0, 0x2001, C, NR_LAYOUT_FIXED, false, S + i, false);
  EXPECT_EQ(n, 2);
  /* enabled: every grant, whatever the layout state */
  nr_sense_unit_set_enabled(0, 0x2001, NR_UNIT_PDSCH_DATA, true);
  for (nr_layout_state_t ls : {NR_LAYOUT_FIXED, NR_LAYOUT_CANDIDATE, NR_LAYOUT_UNSOLVED})
    for (int i = 0; i < 40; i++) EXPECT_TRUE(nr_traffic_decode_wanted(0, 0x2001, C, ls, false, S + i, false));
  /* UL follows PUSCH_DMRS of the RNTI: still disabled there, the policy decodes while the UL table is not learned */
  EXPECT_TRUE(nr_traffic_decode_wanted(1, 0x2001, C, NR_LAYOUT_SOLVED, false, S, false));
  int u = 0;
  for (int i = 0; i < 64; i++) {
    const bool w = nr_traffic_decode_wanted(1, 0x2001, C, NR_LAYOUT_SOLVED, true, S + i, false);
    u += w;
    if (w) nr_traffic_decode_result(1, 0x2001, 0, 20.f, true); /* the decode's verdict resets the policy's counter */
  }
  EXPECT_LE(u, 3); /* 1 policy check per 32 grants and 1 sample per 32 (they may coincide) */
  EXPECT_GE(u, 2);
  nr_sense_unit_reset(0);
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
}
TEST(TrafficDecode, SensingWidebandGrantsAreDecodedWhateverTheUnitSaysOnlyTheExportIsGated)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_isac_sense_min_prb_set(128);
  nr_sense_unit_reset(0);
  nr_cell_bind(0);
  /* a disabled unit: every wideband grant is decoded (FIXED layout, no traffic policy would), the export gate stays closed */
  for (int i = 0; i < 100; i++) EXPECT_TRUE(nr_traffic_decode_wanted_prbs(0, 0x3001, C, NR_LAYOUT_FIXED, false, S + i, false, 200)) << i;
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x3001, 200));
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.skips, 1u);
  /* UL too */
  for (int i = 0; i < 40; i++) EXPECT_TRUE(nr_traffic_decode_wanted_prbs(1, 0x3001, C, NR_LAYOUT_SOLVED, true, S + i, false, 200)) << i;
  /* narrow grants of the same disabled unit follow the traffic policy + the 1-in-32 sample */
  int n = 0;
  for (int i = 0; i < 64; i++) n += nr_traffic_decode_wanted_prbs(0, 0x3002, C, NR_LAYOUT_FIXED, false, S + i, false, 50);
  EXPECT_EQ(n, 2);
  /* an enabled unit decodes narrow grants too and exports */
  nr_sense_unit_set_enabled(0, 0x3002, NR_UNIT_PDSCH_DATA, true);
  for (int i = 0; i < 40; i++) EXPECT_TRUE(nr_traffic_decode_wanted_prbs(0, 0x3002, C, NR_LAYOUT_FIXED, false, S + i, false, 50));
  EXPECT_TRUE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x3002, 50));
  nr_sense_unit_reset(0);
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
}
TEST(TrafficDecode, DecodeStageIsBaselineNotUnitCost)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_unit_reset(0);
  nr_cell_bind(0);
  nr_sense_unit_skip(NR_UNIT_PDSCH_DATA, 0x3003, 200, 1000000000ull);
  nr_sense_unit_decode_job(7000000, 15000000, 1000000000ull);
  EXPECT_EQ(nr_sense_unit_unattributed_ns(0), 7000000u); /* the decode CPU joins the baseline */
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(nr_sense_unit_snapshot_at(0, 1000000000ull + 200000000ull, v, 8), 1);
  EXPECT_FLOAT_EQ(v[0].cpu_p99_ns, 0.f);     /* ... and not the unit's */
  EXPECT_FLOAT_EQ(v[0].wait_p99_ns, 0.f);
  EXPECT_EQ(nr_sense_unit_take_decode_wait_max(0), 15000000u);
  EXPECT_EQ(nr_sense_unit_take_decode_wait_max(0), 0u);
  nr_sense_unit_reset(0);
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
}
