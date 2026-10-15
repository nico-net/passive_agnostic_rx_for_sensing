#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "nr_pdsch_recovery.h"
}

static std::atomic<bool> g_hold{false};
static std::atomic<int> g_ran{0}, g_done{0};
static bool g_expired = false;
static int g_run_ret = 1;
static std::atomic<int> g_fb[3];
static int run_hold(void *, const nr_recov_key_t *)
{
  g_ran++;
  while (g_hold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return 1;
}
static int run_publish(void *, const nr_recov_key_t *k)
{
  uint8_t tb[4] = {0};
  nr_pdsch_recovery_publish(k, tb, 32);
  return 1;
}
static int run_ret(void *, const nr_recov_key_t *) { return g_run_ret; }
static bool exp_hook(const nr_recov_key_t *) { return g_expired; }
static void fb_hook(void *, const nr_recov_key_t *, int outcome) { g_fb[outcome]++; }
static void done_hook(void *) { g_done++; }
static std::string stat()
{
  char b[256];
  nr_pdsch_recovery_stats(b, sizeof b);
  return b;
}
static unsigned long field(const char *name)
{
  std::string s = stat();
  size_t p = s.find(std::string(name) + "=");
  return p == std::string::npos ? ~0ul : strtoul(s.c_str() + p + strlen(name) + 1, nullptr, 10);
}
static int submit(const nr_recov_key_t &k)
{
  void *s = nr_pdsch_recovery_acquire();
  if (!s) return 1;
  memset(s, 0, 8);
  return nr_pdsch_recovery_submit(&k, s);
}
static void wait_fb(int total)
{
  for (int i = 0; i < 400 && g_fb[0] + g_fb[1] + g_fb[2] < total; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

struct Recovery : ::testing::Test {
  void SetUp() override
  {
    g_expired = false; g_hold = false; g_ran = 0; g_done = 0; g_run_ret = 1;
    for (auto &f : g_fb) f = 0;
    nr_pdsch_recovery_set_job_bytes(256);
    nr_pdsch_recovery_set_hooks(run_hold, exp_hook, fb_hook, done_hook);
  }
  void TearDown() override { g_hold = false; nr_pdsch_recovery_stop(); }
};

TEST_F(Recovery, StaleRescueIsDiscarded)
{
  nr_pdsch_recovery_start(4, "0");
  nr_recov_key_t k{0x4601, 3, 0, 1000, 2};
  nr_pdsch_recovery_note_newer(0x4601, 3, /*ndi=*/1, /*slot=*/1010);
  uint8_t tb[16] = {0};
  const unsigned long st0 = field("stale");
  EXPECT_EQ(1, nr_pdsch_recovery_publish(&k, tb, 128));
  EXPECT_EQ(st0 + 1, field("stale"));
  nr_recov_key_t fresh{0x4601, 3, 1, 1010, 2}; /* the newest TB itself is not stale */
  EXPECT_EQ(0, nr_pdsch_recovery_publish(&fresh, tb, 128));
  nr_recov_key_t other{0x4601, 4, 0, 1000, 2}; /* another HARQ process is untouched */
  EXPECT_EQ(0, nr_pdsch_recovery_publish(&other, tb, 128));
}

TEST_F(Recovery, ExpiredIqIsDropped)
{
  nr_pdsch_recovery_start(4, "0");
  g_expired = true;
  uint8_t tb[16] = {0};
  nr_recov_key_t k{0x4602, 1, 0, 2000, 2};
  const unsigned long e0 = field("expired");
  EXPECT_EQ(2, nr_pdsch_recovery_publish(&k, tb, 128));
  EXPECT_EQ(e0 + 1, field("expired"));
  ASSERT_EQ(0, submit(k)); /* a queued job whose IQ is gone is dropped before its arms run */
  wait_fb(1);
  EXPECT_EQ(e0 + 2, field("expired"));
  EXPECT_EQ(0, g_ran.load());
  EXPECT_EQ(1, g_fb[2].load());
}

TEST_F(Recovery, QueueFullDropsNotBlocks)
{
  nr_pdsch_recovery_start(4, "0");
  const unsigned long f0 = field("dropped_full");
  g_hold = true; /* the worker sticks in the first job */
  nr_recov_key_t k{0x4603, 2, 0, 3000, 2};
  long worst_ns = 0;
  int ok = 0;
  for (int i = 0; i < 10; i++) {
    auto t0 = std::chrono::steady_clock::now();
    ok += submit(k) == 0;
    worst_ns = std::max<long>(worst_ns, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  /* 4 slots; the worker keeps its slot while it runs, so 4 accepted, 6 dropped */
  EXPECT_EQ(4, ok);
  EXPECT_EQ(f0 + 6, field("dropped_full"));
  EXPECT_LT(worst_ns, 50000);
  g_hold = false;
}

TEST_F(Recovery, BusyLockDropsWithin50us)
{
  nr_pdsch_recovery_start(4, "0");
  const unsigned long f0 = field("dropped_full");
  nr_pdsch_recovery_test_lock(1); /* the worker (or another consumer) holds the lock */
  auto t0 = std::chrono::steady_clock::now();
  void *s = nr_pdsch_recovery_acquire();
  const long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
  nr_pdsch_recovery_test_lock(0);
  EXPECT_EQ(nullptr, s);
  EXPECT_LT(ns, 50000);
  EXPECT_EQ(f0 + 1, field("dropped_full"));
}

TEST_F(Recovery, SubmitAllocatesNothing)
{
  nr_pdsch_recovery_start(8, "0");
  const uint64_t a0 = nr_pdsch_recovery_allocs();
  EXPECT_GT(a0, 0u);
  nr_recov_key_t k{0x4604, 1, 0, 5000, 2};
  for (int i = 0; i < 20; i++) { submit(k); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  EXPECT_EQ(a0, nr_pdsch_recovery_allocs());
}

TEST_F(Recovery, RescueFeedsHooksStaleDoesNot)
{
  nr_pdsch_recovery_set_hooks(run_publish, exp_hook, fb_hook, done_hook);
  nr_pdsch_recovery_start(4, "0");
  nr_recov_key_t k{0x4610, 5, 0, 4000, 2};
  ASSERT_EQ(0, submit(k));
  wait_fb(1);
  EXPECT_EQ(1, g_fb[0].load()); /* rescued -> the feedback hooks get CRC-OK */
  nr_pdsch_recovery_note_newer(0x4610, 5, 1, 4010);
  ASSERT_EQ(0, submit(k));
  wait_fb(2);
  EXPECT_EQ(1, g_fb[0].load()); /* stale -> no CRC-OK feedback, the failure verdict is delivered once */
  EXPECT_EQ(1, g_fb[1].load());
  g_expired = true;
  nr_recov_key_t k2{0x4611, 5, 0, 4100, 2};
  ASSERT_EQ(0, submit(k2));
  wait_fb(3);
  EXPECT_EQ(1, g_fb[2].load()); /* expired IQ -> failure without credit */
  EXPECT_EQ(3, g_done.load()) << "done() once per job";
}

TEST_F(Recovery, EpochChangeDeliversOneFailFeedback)
{
  nr_pdsch_recovery_set_hooks(run_ret, exp_hook, fb_hook, done_hook);
  g_run_ret = 3; /* abandoned: config epoch changed */
  nr_pdsch_recovery_start(4, "0");
  ASSERT_EQ(0, submit(nr_recov_key_t{0x4620, 2, 0, 6000, 2}));
  wait_fb(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(0, g_fb[0].load());
  EXPECT_EQ(1, g_fb[1].load());
  EXPECT_EQ(0, g_fb[2].load());
}

TEST_F(Recovery, StopDeliversPendingFailures)
{
  nr_pdsch_recovery_start(4, "0");
  g_hold = true;
  for (int i = 0; i < 3; i++) ASSERT_EQ(0, submit(nr_recov_key_t{0x4630, 1, 0, (uint32_t)(7000 + i), 2}));
  std::thread rel([] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); g_hold = false; });
  nr_pdsch_recovery_stop();
  rel.join();
  EXPECT_EQ(3, g_fb[0] + g_fb[1] + g_fb[2]); /* every submitted job got exactly one verdict */
  EXPECT_EQ(3, g_done.load());
}

TEST(RecoveryOrder, FirstArmIsTdraToggle)
{
  int o[31];
  int n = nr_pdsch_recovery_arm_order(0x1f, o);
  ASSERT_EQ(31, n);
  EXPECT_EQ(2, o[0]);
  EXPECT_EQ(1, o[1]);
  int seen = 0;
  for (int i = 0; i < n; i++) seen |= 1 << (o[i] - 1);
  EXPECT_EQ(0x7fffffff, seen); /* every nonempty combination exactly once */
  n = nr_pdsch_recovery_arm_order(0x0d, o); /* CSI|CDM|wide: no S toggle */
  EXPECT_EQ(1, o[0]);
}
