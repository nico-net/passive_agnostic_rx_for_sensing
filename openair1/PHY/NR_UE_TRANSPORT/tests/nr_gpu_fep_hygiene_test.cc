#include "nr_gpu_fep_hygiene.h"
#include <gtest/gtest.h>
#include <condition_variable>
#include <mutex>
#include <thread>

struct FakeGpu {
  long producer = 100;
  uint64_t pre = 0, post = 0;
  int copies = 0, decodes = 0;
};

TEST(GpuFepHygiene, ExpiresWhileWaitingForWorker)
{
  FakeGpu g;
  // The request is queued at source 100, then the producer reaches the first invalid slot
  // while its worker thread is blocked behind a simulated predecessor.
  const auto producer = [](void *p) { return static_cast<FakeGpu *>(p)->producer; };
  const auto stale = [](void *p, int stage) { auto *g = static_cast<FakeGpu *>(p); stage == 0 ? g->pre++ : g->post++; };
  const auto fep = [](void *p) { static_cast<FakeGpu *>(p)->copies++; return 0; };
  const auto decode = [](void *p) -> int64_t { static_cast<FakeGpu *>(p)->decodes++; return 1; };
  std::mutex mu;
  std::condition_variable cv;
  bool waiting = false, released = false;
  int64_t rc = 0;
  std::thread worker([&] {
    std::unique_lock<std::mutex> lock(mu);
    waiting = true;
    cv.notify_one();
    cv.wait(lock, [&] { return released; });
    lock.unlock();
    rc = nr_gpu_fep_run_checked(100, 10, producer, stale, fep, decode, &g);
  });
  {
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&] { return waiting; });
    g.producer = 108; // spf - 2: the first invalid producer position
    released = true;
  }
  cv.notify_one();
  worker.join();
  EXPECT_EQ(-1, rc);
  EXPECT_EQ(1u, g.pre);
  EXPECT_EQ(0, g.copies);
  EXPECT_EQ(0, g.decodes);
}

TEST(GpuFepHygiene, ExpiresDuringSynchronousCopy)
{
  FakeGpu g;
  const auto producer = [](void *p) { return static_cast<FakeGpu *>(p)->producer; };
  const auto stale = [](void *p, int stage) { auto *g = static_cast<FakeGpu *>(p); stage == 0 ? g->pre++ : g->post++; };
  const auto fep = [](void *p) { auto *g = static_cast<FakeGpu *>(p); g->copies++; g->producer = 108; return 0; };
  const auto decode = [](void *p) -> int64_t { static_cast<FakeGpu *>(p)->decodes++; return 1; };
  EXPECT_EQ(-1, nr_gpu_fep_run_checked(100, 10, producer, stale, fep, decode, &g));
  EXPECT_EQ(1u, g.post);
  EXPECT_EQ(1, g.copies);
  EXPECT_EQ(0, g.decodes);
}

TEST(GpuFepHygiene, ToleranceGate)
{
  EXPECT_TRUE(nr_gpu_fep_health_pass(0.9999, 0.99, 0.999, true));
  EXPECT_TRUE(nr_gpu_fep_health_pass(1.0, 1.01, 1.0, true));
  EXPECT_FALSE(nr_gpu_fep_health_pass(0.9998, 1.0, 1.0, true));
  EXPECT_FALSE(nr_gpu_fep_health_pass(1.0, 1.011, 1.0, true));
  EXPECT_FALSE(nr_gpu_fep_health_pass(1.0, 1.0, 0.998, true));
  EXPECT_FALSE(nr_gpu_fep_health_pass(1.0, 1.0, 1.0, false));
  EXPECT_FALSE(nr_gpu_fep_failure_disables(0));
  EXPECT_TRUE(nr_gpu_fep_failure_disables(1));
}

TEST(GpuFepHygiene, NotComparableIsSkipped)
{
  for (bool llr : {false, true})
    for (bool snapshot : {false, true})
      if (!llr || !snapshot) {
        EXPECT_EQ(NR_GPU_FEP_SKIPPED, nr_gpu_fep_check_result(llr && snapshot, 1, 1, 1, true));
      }
  EXPECT_EQ(NR_GPU_FEP_SKIPPED, nr_gpu_fep_check_result(false, -1, 1, 0, false));
  EXPECT_EQ(NR_GPU_FEP_SKIPPED, nr_gpu_fep_check_result(true, -1, 1, 1, true));
  const int16_t weak[] = {-8, 0, 8};
  const int16_t gpu[] = {8, 0, -8};
  EXPECT_FALSE(isfinite(nr_gpu_fep_sign_agreement(weak, gpu, 3)));
  const int16_t cpu2[] = {-9, 9, 1};
  EXPECT_DOUBLE_EQ(1.0, nr_gpu_fep_sign_agreement(cpu2, weak, 3));
}

TEST(GpuFepHygiene, ExpiredReferenceIsSkipped)
{
  EXPECT_EQ(NR_GPU_FEP_SKIPPED, nr_gpu_fep_reference_result(108, 100, 10, true, 1, 1, 1, true));
  EXPECT_EQ(NR_GPU_FEP_FAIL, nr_gpu_fep_reference_result(107, 100, 10, true, 0, 1, 1, true));
}

TEST(GpuFepHygiene, ObserveModeNeverDisables)
{
  for (uint64_t failures = 0; failures < 10; ++failures)
    EXPECT_FALSE(nr_gpu_fep_should_disable(false, failures));
}

TEST(GpuFepHygiene, EnforceModeDisablesAfterTwo)
{
  EXPECT_FALSE(nr_gpu_fep_should_disable(true, 0));
  EXPECT_TRUE(nr_gpu_fep_should_disable(true, 1));
}
