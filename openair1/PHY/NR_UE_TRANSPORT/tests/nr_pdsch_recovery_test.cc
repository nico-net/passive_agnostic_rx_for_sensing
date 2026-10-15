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
static std::atomic<int> g_ran{0};
static bool g_expired = false;
static int run_hold(void *, const nr_recov_key_t *)
{
  g_ran++;
  while (g_hold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return 1;
}
static bool exp_hook(const nr_recov_key_t *) { return g_expired; }
static std::atomic<int> g_fb[3];
static void fb_hook(void *, const nr_recov_key_t *, int outcome) { g_fb[outcome]++; }
static int run_publish(void *, const nr_recov_key_t *k)
{
  uint8_t tb[4] = {0};
  nr_pdsch_recovery_publish(k, tb, 32);
  return 1;
}
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

struct Recovery : ::testing::Test {
  void SetUp() override
  {
    g_expired = false; g_hold = false; g_ran = 0; for (auto &f : g_fb) f = 0;
    nr_pdsch_recovery_set_hooks(run_hold, exp_hook, fb_hook);
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
  /* a queued job whose IQ is gone is dropped before its arms run */
  ASSERT_EQ(0, nr_pdsch_recovery_submit(&k, malloc(8)));
  for (int i = 0; i < 200 && field("expired") < e0 + 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(e0 + 2, field("expired"));
  EXPECT_EQ(0, g_ran.load());
}

TEST_F(Recovery, QueueFullDropsNotBlocks)
{
  nr_pdsch_recovery_start(4, "0");
  g_hold = true; /* the worker sticks in the first job */
  nr_recov_key_t k{0x4603, 2, 0, 3000, 2};
  long worst_ns = 0;
  for (int i = 0; i < 10; i++) {
    auto t0 = std::chrono::steady_clock::now();
    void *j = malloc(8);
    if (nr_pdsch_recovery_submit(&k, j)) free(j);
    worst_ns = std::max<long>(worst_ns, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  /* depth 4 plus the one the worker already took: 5 accepted, 5 dropped (6 if the worker had not taken any yet) */
  EXPECT_GE(field("dropped_full"), 5ul);
  EXPECT_LE(field("dropped_full"), 6ul);
  EXPECT_LT(worst_ns, 50000);
  g_hold = false;
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

static void wait_fb(int total)
{
  for (int i = 0; i < 400 && g_fb[0] + g_fb[1] + g_fb[2] < total; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
TEST_F(Recovery, RescueFeedsHooksStaleDoesNot)
{
  nr_pdsch_recovery_set_hooks(run_publish, exp_hook, fb_hook);
  nr_pdsch_recovery_start(4, "0");
  nr_recov_key_t k{0x4610, 5, 0, 4000, 2};
  ASSERT_EQ(0, nr_pdsch_recovery_submit(&k, malloc(8)));
  wait_fb(1);
  EXPECT_EQ(1, g_fb[0].load()); /* rescued -> the feedback hooks get CRC-OK */
  nr_pdsch_recovery_note_newer(0x4610, 5, 1, 4010);
  ASSERT_EQ(0, nr_pdsch_recovery_submit(&k, malloc(8)));
  wait_fb(2);
  EXPECT_EQ(1, g_fb[0].load()); /* stale -> no CRC-OK feedback, the failure verdict is delivered once */
  EXPECT_EQ(1, g_fb[1].load());
  g_expired = true;
  nr_recov_key_t k2{0x4611, 5, 0, 4100, 2};
  ASSERT_EQ(0, nr_pdsch_recovery_submit(&k2, malloc(8)));
  wait_fb(3);
  EXPECT_EQ(1, g_fb[2].load()); /* expired IQ -> failure without credit */
}
