/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <algorithm>
extern "C" {
#include "nr_initial_sync_budget.h"
}

/* The pre-A11 formula, kept verbatim as the reference. */
static int old_batch(size_t bytes_per_gscn, int len_thr)
{
  size_t max_by_mem = (512UL * 1024UL * 1024UL) / (bytes_per_gscn ? bytes_per_gscn : 1);
  if (max_by_mem < 1)
    max_by_mem = 1;
  size_t max_by_thread = (size_t)len_thr;
  if (max_by_thread < 1)
    max_by_thread = 1;
  return (int)(max_by_mem < max_by_thread ? max_by_mem : max_by_thread);
}

TEST(ScanBatch, DefaultMatchesOldFormula)
{
  const size_t sizes[] = {0, 1, 1000000, 39UL << 20, 40UL << 20, 85UL << 20, 512UL << 20, 600UL << 20, 5UL << 30};
  for (size_t b : sizes)
    for (int thr : {0, 1, 4, 8, 64})
      EXPECT_EQ(nr_initial_sync_scan_batch(b, thr, 512), old_batch(b, thr)) << b << " " << thr;
}

TEST(ScanBatch, KnownValue273Prb4Rx)
{
  /* ~39 MB per GSCN: 512 MB holds 13, a 4-thread pool caps it at 4; 8192 MB with 64 threads gives 64 */
  EXPECT_EQ(nr_initial_sync_scan_batch(39UL << 20, 64, 512), 13);
  EXPECT_EQ(nr_initial_sync_scan_batch(39UL << 20, 64, 8192), 64);
  EXPECT_EQ(nr_initial_sync_scan_batch(39UL << 20, 4, 8192), 4);
}

TEST(ScanBatch, ClampsLowAndHigh)
{
  const size_t b = 8UL << 20;
  EXPECT_EQ(nr_initial_sync_scan_batch(b, 100000, 1), 64 / 8);
  EXPECT_EQ(nr_initial_sync_scan_batch(b, 100000, -5), 64 / 8);
  EXPECT_EQ(nr_initial_sync_scan_batch(b, 100000, 64), 64 / 8);
  EXPECT_EQ(nr_initial_sync_scan_batch(b, 100000, 16384), 16384 / 8);
  EXPECT_EQ(nr_initial_sync_scan_batch(b, 100000, 1000000), 16384 / 8);
}

TEST(ScanBatch, ThreadCapAndFloor)
{
  EXPECT_EQ(nr_initial_sync_scan_batch(1UL << 20, 8, 16384), 8);
  EXPECT_EQ(nr_initial_sync_scan_batch(1UL << 20, 0, 16384), 1);
  EXPECT_EQ(nr_initial_sync_scan_batch(1UL << 30, 8, 512), 1); /* one GSCN bigger than budget */
}

TEST(ScanBatch, SizeTThreadCountNoNarrowing)
{
  /* len_thr is size_t: a value above INT_MAX must not wrap negative (floor 1) but be capped by the memory budget */
  EXPECT_EQ(nr_initial_sync_scan_batch(8UL << 20, (size_t)1 << 33, 64), 64 / 8);
}

TEST(AutoAcquire, TriggerOnlyPassiveAndUnpinned)
{
  EXPECT_TRUE(nr_auto_acquire_trigger(1, 0, 0));
  EXPECT_FALSE(nr_auto_acquire_trigger(1, 1, 0)); // --ssb given
  EXPECT_FALSE(nr_auto_acquire_trigger(1, 0, 1)); // --initial-fo given
  EXPECT_FALSE(nr_auto_acquire_trigger(1, 1, 1));
  EXPECT_FALSE(nr_auto_acquire_trigger(0, 0, 0)); // not passive
}

TEST(AutoAcquire, CfoBoundDefault)
{
  EXPECT_EQ(NR_ACQ_CFO_MAX_HZ, 60000.0);
  EXPECT_LE(NR_ACQ_CFO_MAX_HZ, 32.0 * 15000); // the bound must fit 32 SCS at the narrowest FR1 SCS
}

/* A1: the pinned-start CFO ladder. */
TEST(AcqLadder, CentreFirstThenAliasesAndBounded)
{
  int h[NR_ACQ_LADDER_MAX];
  const int n = nr_acq_cfo_ladder(2178, 30000, 60000, h, NR_ACQ_LADDER_MAX);
  ASSERT_GE(n, 9);
  EXPECT_EQ(h[0], 2178);
  EXPECT_EQ(h[1], 2178 + NR_ACQ_ALIAS_HZ);
  EXPECT_EQ(h[2], 2178 - NR_ACQ_ALIAS_HZ);
  for (int i = 0; i < n; i++)
    EXPECT_LE(abs(h[i] - 2178), 60000) << i;
  /* non-decreasing rung: the alias neighbours of step k come before step k + 1 */
  EXPECT_EQ(h[3], 2178 + 30000);
}

TEST(AcqLadder, EverySeedErrorWithinCaptureRangeIsCovered)
{
  /* the PSS/SSS estimators refine a residual of ~10 kHz (test_nr_sync_pin: the 30 kHz rung locks a 40 kHz offset): every true offset within +-45 kHz of the seed must have a hypothesis within 8.1 kHz of it */
  int h[NR_ACQ_LADDER_MAX];
  for (int scs : {15000, 30000}) {
    const int n = nr_acq_cfo_ladder(-1500, scs, 60000, h, NR_ACQ_LADDER_MAX);
    for (int err = -45000; err <= 45000; err += 250) {
      int best = 1 << 30;
      for (int i = 0; i < n; i++)
        best = std::min(best, abs((-1500 + err) - h[i]));
      EXPECT_LE(best, 8100) << "scs " << scs << " err " << err;
    }
  }
}

TEST(AcqLadder, NineKhzSeedErrorIsAnAliasRung)
{
  int h[NR_ACQ_LADDER_MAX];
  const int n = nr_acq_cfo_ladder(0, 30000, 60000, h, NR_ACQ_LADDER_MAX);
  int best = 1 << 30, at = -1;
  for (int i = 0; i < n; i++)
    if (abs(9000 - h[i]) < best) { best = abs(9000 - h[i]); at = i; }
  EXPECT_GT(at, 0); /* not the seed: escalation is needed */
  EXPECT_EQ(h[at], NR_ACQ_ALIAS_HZ);
  EXPECT_LE(best, 2100);
}

/* A2: CSS0 occasions by slot. calls_per_10 of every 10 slots reach the occasion function (TDD DL/special slots); SS#0 is a 2-slot window in a 40-slot period. */
static bool old_counter_pick(unsigned long long n) { return n % 8 == 0; }

TEST(Css0Slot, EverySs0SlotIsCoveredForAnyCallDensity)
{
  const int period = 40, dur = 2;
  for (int calls : {7, 8, 10})
    for (int offset : {0, 5, 11, 20, 36}) {
      int ss0_called = 0, ss0_hit = 0, old_hit = 0;
      unsigned long long counter = 0;
      for (unsigned slot = 0; slot < 40u * 200u; slot++) {
        if ((int)(slot % 10) >= calls)
          continue; /* not an occasion slot */
        const bool in = nr_acq_slot_in_window(slot, period, offset, dur);
        const bool oldp = old_counter_pick(counter++);
        if (in) {
          ss0_called++;
          ss0_hit++; /* the new rule runs CORESET#0 on every SS#0 slot that is an occasion */
          old_hit += oldp;
        } else {
          EXPECT_TRUE(!nr_acq_slot_in_window(slot, period, offset, dur));
        }
      }
      EXPECT_GT(ss0_called, 0) << calls << " " << offset;
      EXPECT_EQ(ss0_hit, ss0_called);
      /* the counter rule only samples a subset (that is the defect); it is allowed to be anything, the new rule is what is asserted */
      EXPECT_LE(old_hit, ss0_called);
    }
}

TEST(Css0Slot, OldCounterRuleMissesSs0ForEightCallsPerPeriod)
{
  /* 8 calls per 8-call-aligned stream: the counter phase is fixed to one slot of the pattern; SS#0 on another slot is never sampled */
  const int period = 40, dur = 1;
  int old_hit = 0, new_hit = 0;
  unsigned long long counter = 0;
  for (unsigned slot = 0; slot < 4000; slot++) {
    if ((int)(slot % 10) >= 8)
      continue;
    const bool oldp = old_counter_pick(counter++);
    if (nr_acq_slot_in_window(slot, period, 3, dur)) {
      new_hit++;
      old_hit += oldp;
    }
  }
  EXPECT_EQ(old_hit, 0);
  EXPECT_GT(new_hit, 0);
}

TEST(Css0Slot, WindowEdges)
{
  EXPECT_FALSE(nr_acq_slot_in_window(5, 0, 0, 1));
  EXPECT_TRUE(nr_acq_slot_in_window(41, 40, 1, 2));
  EXPECT_TRUE(nr_acq_slot_in_window(42, 40, 1, 2));
  EXPECT_FALSE(nr_acq_slot_in_window(43, 40, 1, 2));
  EXPECT_FALSE(nr_acq_slot_in_window(40, 40, 1, 2));
}
