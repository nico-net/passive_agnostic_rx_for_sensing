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
    nr_pdsch_recovery_set_busy_hook(nullptr);
    nr_pdsch_recovery_test_idle_wait_ns(2000000);
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

static void wait_ran(int n)
{
  for (int i = 0; i < 400 && g_ran < n; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

/* TAIL-FIX: a full lane drops the OLDEST queued job (never blocks, never the running one); the drop is counted and the job's failure feedback is delivered once */
TEST_F(Recovery, QueueFullDropsOldestAndCountsIt)
{
  nr_pdsch_recovery_start(4, "0");
  const unsigned long f0 = field("dropped_full"), o0 = field("dropped_old");
  g_hold = true;
  nr_recov_key_t k{0x4603, 2, 0, 3000, 2};
  ASSERT_EQ(0, submit(k));
  wait_ran(1); /* the worker sticks in the first job and keeps its slot */
  long worst_ns = 0;
  int ok = 1;
  for (int i = 0; i < 9; i++) {
    nr_recov_key_t ki{0x4603, 2, 0, (uint32_t)(3001 + i), 2};
    auto t0 = std::chrono::steady_clock::now();
    ok += submit(ki) == 0;
    worst_ns = std::max<long>(worst_ns, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  /* 4 slots: 1 running + 3 queued; the 6 later submits each evicted the oldest queued job, so every submit was accepted */
  EXPECT_EQ(10, ok);
  EXPECT_EQ(o0 + 6, field("dropped_old"));
  EXPECT_EQ(f0, field("dropped_full"));
  EXPECT_EQ(6, g_fb[NR_RECOV_FB_FAIL].load()); /* each evicted job got its failure feedback, once, at eviction */
  EXPECT_EQ(6, g_done.load());
  EXPECT_LT(worst_ns, 50000);
  g_hold = false;
  wait_fb(10);
  EXPECT_EQ(10, g_fb[0] + g_fb[1] + g_fb[2]); /* one feedback per submitted job: no solver / HARQ update is lost */
}

/* the lane starts a job only when no first-pass decode waits, and not before the grant's verdict was emitted */
TEST_F(Recovery, ArmsRunOnlyWhenFirstPassQueuesAreEmpty)
{
  static std::atomic<bool> busy{true};
  static std::atomic<int> seq{0}, verdict_seq{0}, run_seq{0};
  busy = true; seq = 0; verdict_seq = 0; run_seq = 0;
  nr_pdsch_recovery_set_busy_hook([] { return busy.load(); });
  nr_pdsch_recovery_set_hooks([](void *, const nr_recov_key_t *) { run_seq = ++seq; return 0; }, exp_hook, fb_hook, done_hook);
  nr_pdsch_recovery_test_idle_wait_ns(500000000ull);
  nr_pdsch_recovery_start(4, "0");
  verdict_seq = ++seq; /* the consumer logs EVMGRANT / UL_PUSCH_CRC (first-decode time), then hands the grant over */
  nr_recov_key_t k{0x4604, 1, 0, 4000, 2};
  ASSERT_EQ(0, submit(k));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(0, run_seq.load()) << "an arm started while a first-pass decode was waiting";
  busy = false;
  wait_fb(1);
  EXPECT_GT(run_seq.load(), verdict_seq.load());
  EXPECT_EQ(1, g_fb[NR_RECOV_FB_FAIL].load());
  EXPECT_EQ(0ul, field("yielded"));
}

/* a lane that stays blocked past the bounded wait gives the job up: no arm, one failure feedback, counted */
TEST_F(Recovery, BlockedLaneYieldsAfterBoundedWait)
{
  nr_pdsch_recovery_set_busy_hook([] { return true; });
  nr_pdsch_recovery_test_idle_wait_ns(3000000ull);
  nr_pdsch_recovery_start(4, "0");
  nr_recov_key_t k{0x4605, 1, 0, 5000, 2};
  ASSERT_EQ(0, submit(k));
  wait_fb(1);
  EXPECT_EQ(0, g_ran.load());
  EXPECT_EQ(1, g_fb[NR_RECOV_FB_FAIL].load());
  EXPECT_EQ(1, g_done.load());
  EXPECT_EQ(1ul, field("yielded"));
}

/* every submitted job gets exactly one feedback and one done, whatever happens to it (rescued, failed, evicted, yielded) */
TEST_F(Recovery, ExactlyOneFeedbackPerJobAcrossOutcomes)
{
  nr_pdsch_recovery_set_hooks(run_publish, exp_hook, fb_hook, done_hook);
  nr_pdsch_recovery_start(3, "0");
  const unsigned long sub0 = field("submitted");
  int n = 0;
  for (int i = 0; i < 40; i++) {
    nr_recov_key_t k{0x4606, (uint8_t)(i % 4), 0, (uint32_t)(6000 + i), 2};
    n += submit(k) == 0;
  }
  ASSERT_GT(n, 0); /* a submit can lose the lock race with the worker (dropped_full, caller keeps the job): only accepted jobs count */
  nr_pdsch_recovery_stop(); /* the worker drains; pending jobs get their failure verdict */
  EXPECT_EQ(n, g_fb[0] + g_fb[1] + g_fb[2]);
  EXPECT_EQ(n, g_done.load());
  EXPECT_EQ(sub0 + n, field("submitted"));
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

TEST_F(Recovery, StopWhileSlotsAreHeldIsSafe)
{
  nr_pdsch_recovery_start(4, "0");
  void *a = nr_pdsch_recovery_acquire(), *b = nr_pdsch_recovery_acquire();
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  nr_pdsch_recovery_stop(); /* a consumer is still between acquire and submit */
  EXPECT_EQ(nullptr, nr_pdsch_recovery_acquire());
  nr_recov_key_t k{0x4640, 1, 0, 8000, 2};
  EXPECT_EQ(1, nr_pdsch_recovery_submit(&k, a)); /* refused, no crash */
  nr_pdsch_recovery_release(a);
  nr_pdsch_recovery_release(b);
  nr_pdsch_recovery_start(4, "0"); /* and the module restarts cleanly */
  EXPECT_NE(nullptr, nr_pdsch_recovery_acquire());
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

/* A11: every cell has its own recovery worker (the module's state is per cell). The workers of two cells run side by side; each runs its own hooks
 * only; starting cell 1 late (after cell 0 is running) works; a restart after stop works. [OFFLINE VERIFIED, unit] */
extern "C" {
#include "common/utils/nr_cell.h"
}
static std::atomic<int> g_cell_ran[2];
static int run_cell(void *, const nr_recov_key_t *)
{
  g_cell_ran[nr_cell_idx()]++; /* the worker is bound to its cell: nr_cell_idx() is the cell that started it */
  return 1;
}
static int start_cell(int cell)
{
  nr_cell_bind(cell);
  nr_pdsch_recovery_set_job_bytes(256);
  nr_pdsch_recovery_set_hooks(run_cell, exp_hook, fb_hook, done_hook);
  nr_pdsch_recovery_set_busy_hook(nullptr);
  return nr_pdsch_recovery_start(0, nullptr);
}
TEST(RecoveryCells, EveryCellStartsItsOwnWorker)
{
  g_expired = false;
  for (auto &f : g_fb) f = 0;
  g_cell_ran[0] = g_cell_ran[1] = 0;
  int rc0 = -9, rc1 = -9, sub1 = -9;
  unsigned long base0 = 0;
  std::thread t0([&] { rc0 = start_cell(0); base0 = field("submitted"); });
  t0.join();
  /* cell 1 starts late, from another thread, while cell 0's worker is up */
  std::thread t1([&] {
    rc1 = start_cell(1);
    nr_recov_key_t k = {};
    k.rnti = 0x4601;
    sub1 = submit(k);
  });
  t1.join();
  EXPECT_EQ(rc0, 0);
  EXPECT_EQ(rc1, 0);
  EXPECT_EQ(sub1, 0);
  for (int i = 0; i < 400 && g_cell_ran[1] == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(g_cell_ran[1].load(), 1);
  EXPECT_EQ(g_cell_ran[0].load(), 0) << "cell 0's worker must not see cell 1's job";
  std::thread t2([&] {
    nr_cell_bind(1);
    EXPECT_EQ(field("submitted"), 1ul);
    nr_pdsch_recovery_stop();
    EXPECT_EQ(start_cell(1), 0) << "restart after stop";
    nr_pdsch_recovery_stop();
  });
  t2.join();
  std::thread t3([&] {
    nr_cell_bind(0);
    EXPECT_EQ(field("submitted"), base0) << "cell 0 got no job of cell 1";
    nr_pdsch_recovery_stop();
  });
  t3.join();
}

/* A11: a start that cannot create the thread fails with the failing step and errno (not a bare -1), leaves the module restartable, and the caller can run the
 * arms inline. The failure is a real one: RLIMIT_NPROC at 1 makes pthread_create return EAGAIN (an unprivileged process; the test skips otherwise). */
#include <sys/resource.h>
static std::thread::id g_inline_tid;
static int run_inline_hook(void *, const nr_recov_key_t *)
{
  g_inline_tid = std::this_thread::get_id();
  g_ran++;
  return 0;
}
TEST_F(Recovery, ThreadCreateFailureNamesTheStepAndInlineRunsTheArms)
{
  struct rlimit old, lim;
  ASSERT_EQ(getrlimit(RLIMIT_NPROC, &old), 0);
  lim = old;
  lim.rlim_cur = 1;
  if (setrlimit(RLIMIT_NPROC, &lim) != 0) GTEST_SKIP() << "cannot lower RLIMIT_NPROC";
  const int rc = nr_pdsch_recovery_start(4, "0");
  const std::string err = nr_pdsch_recovery_last_error();
  setrlimit(RLIMIT_NPROC, &old);
  if (rc == 0) GTEST_SKIP() << "privileged: pthread_create is not limited";
  EXPECT_EQ(rc, -1);
  EXPECT_NE(err.find("pthread_create"), std::string::npos) << err;
  EXPECT_NE(err.find("errno=11"), std::string::npos) << err; /* EAGAIN */
  EXPECT_EQ(nr_pdsch_recovery_acquire(), nullptr) << "no worker: nothing is queued";
  /* the caller's fallback: the arms of the failed grant run on the calling (consumer) thread, feedback and done once, never queued */
  nr_pdsch_recovery_set_hooks(run_inline_hook, exp_hook, fb_hook, done_hook);
  g_ran = 0; g_done = 0;
  for (auto &f : g_fb) f = 0;
  alignas(64) char job[64] = {0};
  nr_recov_key_t k{0x4601, 1, 0, 9000, 0x1f};
  nr_pdsch_recovery_run_inline(job, &k);
  EXPECT_EQ(g_ran.load(), 1);
  EXPECT_EQ(g_inline_tid, std::this_thread::get_id());
  EXPECT_EQ(g_fb[0] + g_fb[1] + g_fb[2], 1);
  EXPECT_EQ(g_done.load(), 1);
  EXPECT_EQ(nr_pdsch_recovery_inline_runs(), 1ul);
  /* first-pass work waiting: the inline lane yields (no arm), failure verdict delivered once */
  const unsigned long y0 = field("yielded");
  nr_pdsch_recovery_set_busy_hook([] { return true; });
  nr_recov_key_t k2{0x4601, 1, 0, 9001, 0x1f};
  g_ran = 0;
  nr_pdsch_recovery_run_inline(job, &k2);
  EXPECT_EQ(g_ran.load(), 0);
  EXPECT_EQ(field("yielded"), y0 + 1);
  EXPECT_EQ(g_fb[0] + g_fb[1] + g_fb[2], 2);
  nr_pdsch_recovery_set_busy_hook(nullptr);
  /* and the module still starts once threads can be created again */
  EXPECT_EQ(nr_pdsch_recovery_start(4, "0"), 0);
}
