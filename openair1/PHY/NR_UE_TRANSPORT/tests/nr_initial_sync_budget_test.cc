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
