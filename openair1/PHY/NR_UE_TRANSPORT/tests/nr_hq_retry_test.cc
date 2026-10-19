// Unit test for nr_hq_retry.h: DL HARQ soft combining coexisting with the async retry arms. A mock entry holds a soft sum
// (one int per LLR slot, a tx adds +1 per slot, so the sum counts transmissions). No PHY dependencies, no replays.
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
extern "C" {
#include "nr_hq_retry.h"
}

struct Entry {
  std::atomic<int> busy{0};
  bool soft_valid = false;
  uint8_t ndi = 0;
  int64_t first_slot = 0;
  int sum[8] = {};
};

// What the decoder does for one decode: a quiet decode (a retry arm) never touches the entry; the first pass stores, a retransmission adds.
static bool decode(Entry &e, bool quiet, bool retx, uint8_t ndi, int64_t slot, bool crc_ok)
{
  if (!nr_hq_may_write_sum(quiet))
    return crc_ok;
  if (!retx) {
    for (int &x : e.sum) x = 1;
    e.first_slot = slot;
  } else {
    for (int &x : e.sum) x += 1;
  }
  e.ndi = ndi;
  e.soft_valid = !crc_ok;
  return crc_ok;
}

static void rescue(Entry &e, uint8_t ndi, int64_t slot)
{
  ASSERT_TRUE(nr_hq_lock_wait(&e.busy, [] {}, 3));
  if (nr_hq_rescue_match(e.soft_valid, e.ndi, e.first_slot, ndi, slot))
    e.soft_valid = false;
  e.busy = 0;
}

TEST(HqRetry, ArmsOnlyInAsyncLane)
{
  EXPECT_FALSE(nr_hq_retry_lane(true, 0));  // combining on, no lane: never inline on the first pass
  EXPECT_TRUE(nr_hq_retry_lane(true, 1));   // consumer: defers
  EXPECT_TRUE(nr_hq_retry_lane(true, 2));   // worker: runs
  EXPECT_TRUE(nr_hq_retry_lane(false, 0));  // no combining: unchanged behaviour
}

TEST(HqRetry, RetryNeverCountsATransmissionTwice)
{
  Entry e;
  decode(e, false, false, 1, 100, false);          // first pass fails, sum = 1 tx
  for (int arm = 0; arm < 5; arm++) decode(e, true, false, 1, 100, false); // five quiet arms on the same samples
  for (int x : e.sum) EXPECT_EQ(x, 1);
  EXPECT_TRUE(e.soft_valid);
  decode(e, false, true, 1, 104, false);           // the retransmission combines once
  for (int x : e.sum) EXPECT_EQ(x, 2);
}

TEST(HqRetry, RescueClearsEntry)
{
  Entry e;
  decode(e, false, false, 1, 100, false);
  ASSERT_TRUE(decode(e, true, false, 1, 100, true));  // an arm passes CRC
  rescue(e, 1, 100);
  EXPECT_FALSE(e.soft_valid);
}

TEST(HqRetry, RetransmissionRacingPendingRetry)
{
  // (a) the retransmission combined first and failed: the later rescue of tx1 still retires the TB
  Entry a;
  decode(a, false, false, 1, 100, false);
  decode(a, false, true, 1, 104, false);
  rescue(a, 1, 100);
  EXPECT_FALSE(a.soft_valid);
  // (b) the retransmission combined and passed: the rescue is a no-op, the entry is already dead
  Entry b;
  decode(b, false, false, 1, 100, false);
  decode(b, false, true, 1, 104, true);
  rescue(b, 1, 100);
  EXPECT_FALSE(b.soft_valid);
  // (c) a NEW TB (ndi toggled) failed first: the pending rescue of the old TB must not retire it
  Entry c;
  decode(c, false, false, 1, 100, false);
  decode(c, false, false, 0, 120, false);
  rescue(c, 1, 100);
  EXPECT_TRUE(c.soft_valid);
  // (d) the 1-bit ndi wrapped (TB3 has ndi 1 again): the slot still tells the TBs apart
  Entry d;
  decode(d, false, false, 1, 300, false);
  rescue(d, 1, 100);
  EXPECT_TRUE(d.soft_valid);
  // (e) the entry lock is held by a consumer: the worker waits, and gives up bounded when it never frees
  Entry f;
  f.busy = 1;
  int naps = 0;
  EXPECT_FALSE(nr_hq_lock_wait(&f.busy, [] {}, 5));
  std::thread t([&] { f.busy = 0; });
  t.join();
  EXPECT_TRUE(nr_hq_lock_wait(&f.busy, [] {}, 0));
  (void)naps;
}

// CRC-BUGS 2026-10-19: a same-NDI grant combines only with the stored TB of its own TBS; another TBS is decoded alone and leaves the entry (-1).
TEST(HqRetxRule, SameNdiAndTbsCombineAnotherTbsDecodesAlone)
{
  EXPECT_EQ(nr_hq_retx_rule(false, 1, 1128, 1, 2, 1128, false), 0); // nothing stored: first transmission
  EXPECT_EQ(nr_hq_retx_rule(true, 0, 1128, 1, 2, 1128, false), 0);  // NDI toggled: a new TB
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1128, 1, 2, 1128, false), 1);  // retransmission of the stored TB
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1128, 1, 3, 984, false), -1);  // same NDI, another TBS: not this TB's retransmission
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1128, 1, 0, 984, true), -1);
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 0, 1, 1, 984, false), 1);      // no TBS recorded: unchanged behaviour
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1128, 1, 0, 1128, false), 0);  // rv 0 on the same NDI: combined only once admitted
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1128, 1, 0, 1128, true), 1);
}
