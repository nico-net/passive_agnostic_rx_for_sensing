/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/* test_nr_lock_verify: verification of a fresh PBCH lock and the per-cell rejected SSB beams (FIX1102 B). */
#include <gtest/gtest.h>
#include "nr_lock_verify.h"

TEST(LockVerify, NeverTrackedLockIsRejectedAfterTheWindows)
{
  nr_lock_verify_t v = {};
  v.lock_ssb = v.lock_pci = -1;
  EXPECT_EQ(nr_lock_verify_window(&v, false, 0, 0, 0, -1, 2), NR_LOCK_KEEP); /* searching: never judged */
  EXPECT_EQ(nr_lock_verify_window(&v, true, 0, 50, 2, 415, 2), NR_LOCK_KEEP);
  EXPECT_EQ(v.lock_ssb, 2);
  EXPECT_EQ(v.lock_pci, 415);
  EXPECT_EQ(nr_lock_verify_window(&v, true, 0, 50, 4, 415, 2), NR_LOCK_REJECT); /* RSRP may have moved the index: the lock's is kept */
  EXPECT_EQ(v.lock_ssb, 2);
}

TEST(LockVerify, OneDecodedPbchVerifiesForTheWholeLock)
{
  nr_lock_verify_t v = {};
  v.lock_ssb = v.lock_pci = -1;
  EXPECT_EQ(nr_lock_verify_window(&v, true, 1, 49, 4, 415, 2), NR_LOCK_VERIFIED);
  /* a verified lock that later loses PBCH is the RFSTALL watchdog's case, never a rejection */
  for (int w = 0; w < 10; w++)
    EXPECT_EQ(nr_lock_verify_window(&v, true, 0, 50, 4, 415, 2), NR_LOCK_KEEP);
}

TEST(LockVerify, AWindowWithoutSsbOccasionsDoesNotCount)
{
  nr_lock_verify_t v = {};
  v.lock_ssb = v.lock_pci = -1;
  EXPECT_EQ(nr_lock_verify_window(&v, true, 0, 0, 0, 923, 2), NR_LOCK_KEEP); /* lock at the window's end: no PBCH tried yet */
  EXPECT_EQ(nr_lock_verify_window(&v, true, 0, 50, 0, 923, 2), NR_LOCK_KEEP);
  EXPECT_EQ(nr_lock_verify_window(&v, true, 3, 47, 0, 923, 2), NR_LOCK_VERIFIED);
}

TEST(LockVerify, ANewLockStartsOver)
{
  nr_lock_verify_t v = {};
  v.lock_ssb = v.lock_pci = -1;
  EXPECT_EQ(nr_lock_verify_window(&v, true, 2, 48, 4, 415, 2), NR_LOCK_VERIFIED);
  EXPECT_EQ(nr_lock_verify_window(&v, false, 0, 0, 4, 415, 2), NR_LOCK_KEEP); /* lost */
  EXPECT_EQ(nr_lock_verify_window(&v, true, 0, 50, 5, 415, 2), NR_LOCK_KEEP); /* relocked, other beam */
  EXPECT_EQ(nr_lock_verify_window(&v, true, 0, 50, 5, 415, 2), NR_LOCK_REJECT);
  EXPECT_EQ(v.lock_ssb, 5);
}

TEST(SsbReject, SkipsOnlyTheRejectedBeamsOfThatPci)
{
  nr_ssb_reject_t r;
  nr_ssb_reject_init(&r);
  EXPECT_FALSE(nr_ssb_reject_skip(&r, 415, 2));
  nr_ssb_reject_add(&r, 415, 2);
  nr_ssb_reject_add(&r, 415, 6);
  EXPECT_TRUE(nr_ssb_reject_skip(&r, 415, 2));
  EXPECT_TRUE(nr_ssb_reject_skip(&r, 415, 6));
  EXPECT_FALSE(nr_ssb_reject_skip(&r, 415, 4));
  EXPECT_FALSE(nr_ssb_reject_skip(&r, 923, 2)); /* another cell behind the same slot is not affected */
  nr_ssb_reject_add(&r, 923, 0);                /* a rejection on another PCI replaces the memory */
  EXPECT_FALSE(nr_ssb_reject_skip(&r, 415, 2));
  EXPECT_TRUE(nr_ssb_reject_skip(&r, 923, 0));
  EXPECT_FALSE(nr_ssb_reject_skip(&r, 923, 64)); /* out of range: never skipped */
  nr_ssb_reject_add(&r, 923, -1);
  EXPECT_TRUE(nr_ssb_reject_skip(&r, 923, 0));
}

TEST(SsbReject, ForgottenAfterMissedSearchesSoACellCannotLockItselfOut)
{
  nr_ssb_reject_t r;
  nr_ssb_reject_init(&r);
  nr_ssb_reject_add(&r, 415, 2);
  for (int i = 0; i < 3; i++) {
    nr_ssb_reject_search_done(&r, false, 4);
    EXPECT_TRUE(nr_ssb_reject_skip(&r, 415, 2));
  }
  nr_ssb_reject_search_done(&r, false, 4);
  EXPECT_FALSE(nr_ssb_reject_skip(&r, 415, 2));
  nr_ssb_reject_add(&r, 415, 2);
  nr_ssb_reject_search_done(&r, false, 4);
  nr_ssb_reject_search_done(&r, true, 4); /* a lock (not yet verified) resets the miss count, keeps the memory */
  for (int i = 0; i < 3; i++)
    nr_ssb_reject_search_done(&r, false, 4);
  EXPECT_TRUE(nr_ssb_reject_skip(&r, 415, 2));
}
