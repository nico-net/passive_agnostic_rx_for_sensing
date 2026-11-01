// Unit test for nr_hq_retry.h: DL HARQ soft combining coexisting with the async retry arms. A mock entry holds a soft sum
// (one int per LLR slot, a tx adds +1 per slot, so the sum counts transmissions). No PHY dependencies, no replays.
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
extern "C" {
#include "nr_hq_retry.h"
#include "nr_pdsch_ldpc_cap.h"
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

// RANK2-ITER: the one LDPC iteration cap and the per-TB iteration summary (nr_pdsch_ldpc_cap.h)
TEST(PdschLdpcCap, ParseEnv)
{
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse(nullptr), 0);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse(""), 0);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("abc"), 0);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("12x"), 0);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("0"), 0);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("-3"), 0);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("31"), 0);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("1"), 1);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("16"), 16);
  EXPECT_EQ(nr_pdsch_ldpc_cap_parse("30"), 30);
}

TEST(PdschLdpcCap, ProductDefaultWithoutEnv)
{
  unsetenv("NR_TEST_PDSCH_LDPC_ITERS");
  EXPECT_EQ(nr_pdsch_ldpc_cap(), NR_PDSCH_LDPC_CAP_DEFAULT);
  EXPECT_EQ(NR_PDSCH_LDPC_CAP_DEFAULT, 16);
  EXPECT_LE(NR_PDSCH_LDPC_CAP_DEFAULT, NR_PDSCH_LDPC_GPU_MAX); // the product cap must run on the GPU LDPC
}

TEST(PdschLdpcCap, IterSummary)
{
  const uint8_t its[5] = {3, 5, 8, 0, 0}; // block 2 hit the cap (8), blocks 3 and 4 were aborted (not reported)
  const bool ok[5] = {true, true, false, false, false};
  uint8_t mx = 99;
  uint16_t sum = 99;
  EXPECT_EQ(nr_pdsch_ldpc_iters_summary(its, ok, 5, &mx, &sum), 3);
  EXPECT_EQ(mx, 8);
  EXPECT_EQ(sum, 8); // 3 + 5: only the blocks that decoded
  const uint8_t none[2] = {0, 0};
  const bool nok[2] = {false, false};
  EXPECT_EQ(nr_pdsch_ldpc_iters_summary(none, nok, 2, &mx, &sum), 0);
  EXPECT_EQ(mx, 0);
  EXPECT_EQ(sum, 0);
}

// ---- pred1101/harq: retransmission anticipation, eviction and the no-history gate ----

TEST(HqAnticipation, NdiToggleEvictsAndRestarts)
{
  // a failed TB (ndi 1) is stored; the next grant of the pid toggled the NDI: a new TB, the stored sum is dead
  EXPECT_TRUE(nr_hq_ndi_evicts(true, 1, 0));
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1000, 0, 0, 1000, false), 0);   // restart, never combined
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1000, 0, 2, 1000, false), 0);   // even at rv 2: another TB's parity must not be summed in
  EXPECT_FALSE(nr_hq_ndi_evicts(true, 1, 1));                        // same NDI: a retransmission, kept
  EXPECT_FALSE(nr_hq_ndi_evicts(false, 1, 0));                       // nothing live to drop
  EXPECT_EQ(nr_hq_retx_rule(true, 1, 1000, 1, 2, 1000, false), 1);   // rv 2, same NDI, same TBS: combined
}

TEST(HqAnticipation, WindowIsAirTimeNotSlots)
{
  EXPECT_EQ(nr_hq_window_slots(10), 150);  // mu 0: 1 slot/ms
  EXPECT_EQ(nr_hq_window_slots(20), 300);  // mu 1
  EXPECT_EQ(nr_hq_window_slots(40), 600);  // mu 2
  EXPECT_EQ(nr_hq_window_slots(0), 300);   // unknown frame: mu 1
  const int64_t w = nr_hq_window_slots(20);
  EXPECT_FALSE(nr_hq_expired(true, 1000, 1000 + w, w));      // exactly the window: kept
  EXPECT_TRUE(nr_hq_expired(true, 1000, 1000 + w + 1, w));   // older: dropped
  EXPECT_FALSE(nr_hq_expired(false, 1000, 100000, w));       // no live sum, nothing to drop
  EXPECT_FALSE(nr_hq_expired(true, -1, 100000, w));          // unknown age (never stamped): kept, the LRU bounds it
  EXPECT_FALSE(nr_hq_expired(true, 1000, -1, w));            // unknown now: no sweep
}

TEST(HqAnticipation, StoreLifecycle)
{
  // the per-pid sequence the decoder runs: first fails -> stored; retx combines; decoded -> retired; NDI toggle -> restart; expiry -> alone
  struct Hq { bool valid = false; uint8_t ndi = 0; uint32_t tbs = 0; int64_t slot = -1; } e;
  const int64_t w = nr_hq_window_slots(20);
  int evicted = 0, combined = 0, alone = 0;
  auto grant = [&](uint8_t ndi, uint8_t rv, uint32_t tbs, int64_t now, bool crc_ok) {
    if (nr_hq_expired(e.valid, e.slot, now, w)) { e.valid = false; evicted++; }
    const int d = nr_hq_retx_rule(e.valid, e.ndi, e.tbs, ndi, rv, tbs, false);
    if (d == 0 && nr_hq_ndi_evicts(e.valid, e.ndi, ndi)) evicted++;
    if (d > 0) combined++;
    if (rv != 0 && d <= 0) alone++;
    e.valid = !crc_ok; e.ndi = ndi; e.tbs = tbs; e.slot = now;
  };
  grant(1, 0, 4000, 100, false);   // first, fails: stored
  grant(1, 2, 4000, 108, false);   // rv 2 combines (still failing)
  grant(1, 3, 4000, 116, true);    // rv 3 combines and passes: retired
  EXPECT_EQ(combined, 2);
  EXPECT_FALSE(e.valid);
  grant(0, 0, 4000, 130, false);   // new TB (NDI toggled), fails
  grant(1, 0, 4000, 140, false);   // another new TB: the failed one is evicted by the toggle
  EXPECT_EQ(evicted, 1);
  grant(1, 3, 4000, 140 + w + 5, true); // its retransmission arrives after the window: no history, decoded alone
  EXPECT_EQ(evicted, 2);
  EXPECT_EQ(alone, 1);
  EXPECT_EQ(combined, 2);
}

TEST(HqAnticipation, NoHistoryGate)
{
  // a retransmission with no stored sum is decoded alone iff its own bits hold the information
  EXPECT_TRUE(nr_hq_alone_decodable(2, 3, 120.0));
  EXPECT_TRUE(nr_hq_alone_decodable(2, 2, 120.0));    // BG2 low rate: rv 2 wraps over the systematic columns
  EXPECT_FALSE(nr_hq_alone_decodable(1, 2, 700.0));   // BG1 high rate rv 2: parity only
  EXPECT_FALSE(nr_hq_alone_decodable(0, 1, 500.0));
  EXPECT_FALSE(nr_hq_alone_decodable(0, 4, 100.0));   // not an rv
  // monotone in the rate: a lower rate never loses self-decodability
  for (int bg = 0; bg <= 2; bg++)
    for (int rv = 1; rv <= 3; rv++) {
      bool seen_false = false;
      for (double r = 30.0; r < 1000.0; r += 10.0) {
        const bool ok = nr_hq_alone_decodable(bg, rv, r);
        if (!ok) seen_false = true;
        else EXPECT_FALSE(seen_false) << "bg " << bg << " rv " << rv << " r1024 " << r;
      }
    }
}
