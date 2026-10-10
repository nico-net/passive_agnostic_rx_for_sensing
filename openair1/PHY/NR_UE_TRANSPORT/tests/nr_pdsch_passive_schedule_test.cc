/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <vector>
extern "C" {
#include "nr_pdsch_passive_schedule.h"
}

struct Clock {
  long before, after;
  int calls = 0;
  static long read(void *p) {
    auto &c = *static_cast<Clock *>(p);
    return c.calls++ ? c.after : c.before;
  }
};

/* TEST: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:38, common/config/tests/test_config_cmdline.cpp:47, common/config/tests/test_config_cmdline.cpp:56, common/config/tests/test_config_cmdline.cpp:65 (+1255 more).
 */
TEST(PdschSnapshot, FourAntennasWrapAndSurviveOverwrite) {
  std::array<std::array<uint32_t, 80>, 4> ring{}, copy{};
  const void *src[4]; void *dst[4];
  for (unsigned a = 0; a < 4; ++a) {
    for (unsigned i = 0; i < 80; ++i) ring[a][i] = a * 1000 + i + 1;
    src[a] = ring[a].data(); dst[a] = copy[a].data();
  }
  Clock clk{109, 110};
  ASSERT_TRUE(nr_pdsch_copy_iq(dst, src, 4, 320, 300, 40, 100, 20, Clock::read, &clk));
  for (auto &r : ring) r.fill(0xdeadbeef);
  for (unsigned a = 0; a < 4; ++a)
    for (unsigned i = 0; i < 80; ++i)
      EXPECT_EQ(copy[a][i], i < 5 || i >= 75 ? a * 1000 + i + 1 : 0u);
}

/* TEST: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:38, common/config/tests/test_config_cmdline.cpp:47, common/config/tests/test_config_cmdline.cpp:56, common/config/tests/test_config_cmdline.cpp:65 (+1254 more).
 */
TEST(PdschSnapshot, RejectsFutureExpiredAndOverwriteDuringCopy) {
  uint32_t ring[80] = {}, copy[80] = {};
  const void *src[] = {ring}; void *dst[] = {copy};
  for (const auto &times : {std::pair<long, long>{99, 100}, {118, 118}, {117, 118}}) {
    Clock clk{times.first, times.second};
    EXPECT_FALSE(nr_pdsch_copy_iq(dst, src, 1, sizeof(ring), 0, 40, 100, 20, Clock::read, &clk));
  }
  Clock clk{117, 117};
  EXPECT_TRUE(nr_pdsch_copy_iq(dst, src, 1, sizeof(ring), 0, 40, 100, 20, Clock::read, &clk));
}

/* TEST: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:38, common/config/tests/test_config_cmdline.cpp:47, common/config/tests/test_config_cmdline.cpp:56, common/config/tests/test_config_cmdline.cpp:65 (+1253 more).
 */
TEST(PdschLatency, InclusiveBoundsAndOverflow) {
  EXPECT_EQ(nr_pdsch_latency_bin(0), 0u);
  EXPECT_EQ(nr_pdsch_latency_bin(100000), 0u);
  EXPECT_EQ(nr_pdsch_latency_bin(100001), 1u);
  EXPECT_EQ(nr_pdsch_latency_bin(16000000), 7u);
  EXPECT_EQ(nr_pdsch_latency_bin(UINT64_MAX), 8u);
}

/* Discrete-event queue model, not a device benchmark. Single RNTI => production's
 * fair eviction is drop-oldest. Three consumers, depth 20, 500 us slots, source
 * already nine slots old on arrival, one serial GPU lane. Snapshot is the real
 * production copy/guard helper above. Injected service times are independent of
 * wall clock, so load assertions do not depend on the host scheduler. */
struct LoadResult { int offered = 0, decoded = 0, stale = 0, full = 0, peak = 0; };
/* load: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_passive_schedule_test.cc:102, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_passive_schedule_test.cc:109, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_passive_schedule_test.cc:110, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_passive_schedule_test.cc:116 (+1 more).
 */
static LoadResult load(int rate, int gpu_us, int cpu_us, bool owned) {
  constexpr int64_t duration = 2000000, base = 100000;
  constexpr int depth = 20;
  struct Job { long source; };
  std::deque<Job> q;
  std::array<int64_t, 3> available{base, base, base};
  int64_t gpu_free = base, next = base, now = base;
  LoadResult out;
  uint32_t ring[80] = {}, copy[80] = {};
  const void *src[] = {ring}; void *dst[] = {copy};
  while (next < base + duration || !q.empty()) {
    const auto consumer = std::min_element(available.begin(), available.end());
    const auto take = q.empty() ? INT64_MAX : std::max(now, *consumer);
    if (next < base + duration && next <= take) {
      now = next;
      if (q.size() == depth) { q.pop_front(); ++out.full; }
      q.push_back({long(now / 500 - 9)});
      ++out.offered;
      out.peak = std::max(out.peak, int(q.size()));
      next += 1000000 / rate;
      continue;
    }
    now = take;
    const auto job = q.front(); q.pop_front();
    Clock clk{long(now / 500), long((now + 100) / 500)};
    if (!nr_pdsch_copy_iq(dst, src, 1, sizeof(ring), 0, 40, job.source, 20, Clock::read, &clk)) {
      ++out.stale;
      continue;
    }
    // 100 us CPU copy; GPU injection; CPU LDPC tail. Completion releases just this request.
    const auto done = std::max(now + 100, gpu_free) + gpu_us;
    gpu_free = done;
    *consumer = done + cpu_us;
    if (owned || nr_passive_samples_valid(long(*consumer / 500), job.source, 20)) ++out.decoded;
    else ++out.stale;
  }
  return out;
}

/* TEST: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:38, common/config/tests/test_config_cmdline.cpp:47, common/config/tests/test_config_cmdline.cpp:56, common/config/tests/test_config_cmdline.cpp:65 (+1252 more).
 */
TEST(PdschLoad, NoStaleThrough200GrantsPerSecond) {
  for (int rate : {20, 50, 100, 200}) {
    const auto r = load(rate, 4000, 1000, true);
    EXPECT_EQ(r.decoded, r.offered) << rate;
    EXPECT_EQ(r.stale, 0) << rate;
    EXPECT_EQ(r.full, 0) << rate;
  }
}
/* TEST: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:38, common/config/tests/test_config_cmdline.cpp:47, common/config/tests/test_config_cmdline.cpp:56, common/config/tests/test_config_cmdline.cpp:65 (+1251 more).
 */
TEST(PdschLoad, LowLagGpuRoundTripRegression) {
  const auto old = load(50, 4920, 1000, false);
  const auto fixed = load(50, 4920, 1000, true);
  EXPECT_EQ(old.stale, old.offered);
  EXPECT_EQ(fixed.decoded, fixed.offered);
  EXPECT_EQ(fixed.stale, 0);
}
/* TEST: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:38, common/config/tests/test_config_cmdline.cpp:47, common/config/tests/test_config_cmdline.cpp:56, common/config/tests/test_config_cmdline.cpp:65 (+1250 more).
 */
TEST(PdschLoad, LongGpuInjectionDoesNotExpireOwnedSamples) {
  const auto r = load(20, 25000, 2000, true);
  EXPECT_EQ(r.decoded, r.offered);
  EXPECT_EQ(r.stale, 0);
}
/* TEST: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/config/tests/test_config_cmdline.cpp:38, common/config/tests/test_config_cmdline.cpp:47, common/config/tests/test_config_cmdline.cpp:56, common/config/tests/test_config_cmdline.cpp:65 (+1249 more).
 */
TEST(PdschLoad, OverloadIsBoundedAndAccounted) {
  for (int rate : {500, 2000, 10000}) {
    const auto r = load(rate, 4000, 1000, true);
    EXPECT_GT(r.decoded, 0);
    EXPECT_GT(r.stale + r.full, 0);
    EXPECT_LE(r.peak, 20);
    EXPECT_EQ(r.decoded + r.stale + r.full, r.offered);
    if (rate == 10000) { EXPECT_GT(r.full, 0); }
  }
}
