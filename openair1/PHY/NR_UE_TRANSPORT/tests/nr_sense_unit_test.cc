#include "nr_sense_unit.h"
#include "nr_sense_sched.h"
#include "common/utils/nr_cell.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

static const uint64_t B = SENSE_BUCKET_NS;
static const uint64_t T0 = 100000ull * B; /* arbitrary monotonic origin */
static uint64_t tb(uint64_t k) { return (T0 + k) * B + 1000; }
static const uint64_t MS = 1000000ull;

class Unit : public ::testing::Test {
 protected:
  void SetUp() override
  {
    nr_isac_mode_set(NR_ISAC_MODE_SENSING);
    nr_cell_bind(0);
    nr_sense_unit_test_seam(nullptr);
    for (int c = 0; c < 3; c++) nr_sense_unit_reset(c);
  }
  void TearDown() override { nr_isac_mode_set(NR_ISAC_MODE_LEGACY); nr_cell_bind(0); }
  static int snap(int cell, uint64_t now, nr_sense_unit_view_t *v) { return nr_sense_unit_snapshot_at(cell, now, v, 256); }
};

TEST_F(Unit, BucketsAccumulate)
{
  for (int i = 0; i < 3; i++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4601, 1 * MS, 0, false, 200, tb(5) + i);
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4601, 1 * MS, 0, false, 200, tb(6));
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 0x4601, NR_UNIT_PDSCH_DATA, tb(5)), 3 * MS);
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 0x4601, NR_UNIT_PDSCH_DATA, tb(6)), 1 * MS);
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 0x4601, NR_UNIT_PDSCH_DMRS, tb(5)), 0u); /* the source is part of the key */
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 0x4602, NR_UNIT_PDSCH_DATA, tb(5)), 0u); /* so is the RNTI */
}

TEST_F(Unit, P99OverWindow)
{
  /* nearest rank over the 320 buckets: p95 = 304th, p99 = 317th smallest. 4 buckets of 50 ms (the top 1.25 %) put the 317th at 50 ms. */
  ASSERT_EQ(SENSE_WIN_BUCKETS, 320u);
  for (int k = 0; k < 320; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 7, (k >= 316 ? 50 : 1) * MS, 0, false, 200, tb(k));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(320), v), 1);
  EXPECT_FLOAT_EQ(v[0].cpu_p99_ns, 50.0f * MS);
  EXPECT_FLOAT_EQ(v[0].cpu_p95_ns, 1.0f * MS);
  EXPECT_FLOAT_EQ(v[0].cpu_max_ns, 50.0f * MS);
}

TEST_F(Unit, NearestRankSingleOutlierIsNotP99)
{
  /* 1 bucket of 320 is 0.31 %: under nearest rank the p99 (317th) stays at 1 ms and only the burst max would show it */
  for (int k = 0; k < 320; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 7, (k == 319 ? 50 : 1) * MS, 0, false, 200, tb(k));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(320), v), 1);
  EXPECT_FLOAT_EQ(v[0].cpu_p99_ns, 1.0f * MS);
  EXPECT_FLOAT_EQ(v[0].cpu_p95_ns, 1.0f * MS);
  EXPECT_FLOAT_EQ(v[0].cpu_max_ns, 50.0f * MS); /* the burst the percentiles ignore */
}

TEST_F(Unit, IdleBucketsCountAsZeroAndOldBucketsLeaveTheWindow)
{
  for (int k = 0; k < 10; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 7, 9 * MS, 0, false, 200, tb(k));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(10), v), 1);
  EXPECT_FLOAT_EQ(v[0].cpu_p99_ns, 9.0f * MS); /* 10 busy buckets of 320 (3.1 %): the 317th smallest is busy, the 304th is an idle bucket */
  EXPECT_FLOAT_EQ(v[0].cpu_p95_ns, 0.0f);
  for (int k = 10; k < 330; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 7, 2 * MS, 0, false, 200, tb(k));
  ASSERT_EQ(snap(0, tb(330), v), 1);
  EXPECT_FLOAT_EQ(v[0].cpu_p99_ns, 2.0f * MS); /* the 9 ms buckets are 320+ buckets old: the ring slot was reused, nothing stale remains */
}

TEST_F(Unit, LruEvictsIdle)
{
  for (int r = 1; r <= 64; r++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, (uint16_t)r, MS, 0, false, 200, tb(r));
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 1, MS, 0, false, 200, tb(70)); /* rnti 1 is the oldest, touch it: rnti 2 becomes the oldest */
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 65, MS, 0, false, 200, tb(71)); /* the 65th evicts rnti 2 */
  nr_sense_unit_view_t v[256];
  const int n = snap(0, tb(72), v);
  EXPECT_EQ(n, 64);
  bool has1 = false, has2 = false, has65 = false;
  for (int i = 0; i < n; i++) { has1 |= v[i].rnti == 1; has2 |= v[i].rnti == 2; has65 |= v[i].rnti == 65; }
  EXPECT_TRUE(has1);
  EXPECT_FALSE(has2);
  EXPECT_TRUE(has65);
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 2, NR_UNIT_PDSCH_DATA, tb(2)), 0u); /* evicted history is gone, not inherited by the new RNTI */
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 65, NR_UNIT_PDSCH_DATA, tb(2)), 0u);
}

TEST_F(Unit, ViolationAndSkipCounted)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 9, MS, 0, false, 200, tb(1));
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 9);
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 9);
  nr_sense_unit_skip(NR_UNIT_PDSCH_DATA, 9, 200, tb(1));
  nr_sense_unit_skip(NR_UNIT_PDSCH_DATA, 9, 200, tb(1));
  nr_sense_unit_skip(NR_UNIT_PDSCH_DATA, 9, 200, tb(1));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(2), v), 1);
  EXPECT_EQ(v[0].violations, 2u);
  EXPECT_EQ(v[0].skips, 3u);
  EXPECT_EQ(v[0].late, 0u);
  EXPECT_TRUE(v[0].enabled);
  nr_sense_unit_set_enabled(0, 9, NR_UNIT_PDSCH_DATA, false);
  ASSERT_EQ(snap(0, tb(2), v), 1);
  EXPECT_FALSE(v[0].enabled);
  nr_sense_unit_set_enabled(0, 9, NR_UNIT_PDSCH_DATA, true);
  ASSERT_EQ(snap(0, tb(2), v), 1);
  EXPECT_TRUE(v[0].enabled);
}

TEST_F(Unit, SnapshotPerCell)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 5, 4 * MS, 0, false, 200, tb(1)); /* bound to cell 0 */
  std::thread t([] {
    nr_cell_bind(1);
    nr_sense_unit_job(NR_UNIT_PDSCH_DMRS, 6, 8 * MS, 0, true, 200, tb(1));
    nr_sense_unit_violation(NR_UNIT_PDSCH_DMRS, 6);
  });
  t.join();
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(2), v), 1);
  EXPECT_EQ(v[0].rnti, 5);
  EXPECT_EQ(v[0].src, NR_UNIT_PDSCH_DATA);
  EXPECT_EQ(v[0].violations, 0u);
  ASSERT_EQ(snap(1, tb(2), v), 1);
  EXPECT_EQ(v[0].rnti, 6);
  EXPECT_EQ(v[0].src, NR_UNIT_PDSCH_DMRS);
  EXPECT_EQ(v[0].late, 1u);
  EXPECT_EQ(v[0].violations, 1u);
  EXPECT_EQ(snap(2, tb(2), v), 0);
  EXPECT_EQ(nr_sense_unit_bucket_cpu(1, 5, NR_UNIT_PDSCH_DATA, tb(1)), 0u);
}

TEST_F(Unit, WaitRateAndWideband)
{
  /* 160 messages over 160 buckets (8 s): 20 Hz; the even ones are 200 PRB (wideband, so the first opens the row), the odd ones 50 */
  for (int k = 0; k < 160; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 3, MS, (k == 7 ? 3 * MS : 100000), false, (k & 1) ? 50 : 200, tb(k));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(320), v), 1);
  EXPECT_FLOAT_EQ(v[0].rate_hz, 160.0f / 16.0f); /* over the 16 s window */
  EXPECT_FLOAT_EQ(v[0].wb_rate_hz, 80.0f / 16.0f);
  EXPECT_FLOAT_EQ(v[0].wait_p99_ns, 100000.0f); /* nearest rank over the 160 buckets that saw a job: the 159th is 100 us, the one 3 ms bucket is the max */
  EXPECT_EQ(v[0].last_msg_ns, tb(159));
}

TEST_F(Unit, WaitOutlierBecomesP99WhenFrequent)
{
  for (int k = 0; k < 100; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 3, MS, k < 98 ? 100000 : 3 * MS, true, 200, tb(k));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(100), v), 1);
  EXPECT_FLOAT_EQ(v[0].wait_p99_ns, 3.0f * MS); /* 100 buckets: 99th smallest is a 3 ms bucket */
  EXPECT_EQ(v[0].late, 100u);
}

TEST_F(Unit, CfrQuality)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 4, MS, 0, false, 200, tb(1));
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 4, 100, 10.0f, tb(1) + 5);
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(2), v), 1);
  EXPECT_FLOAT_EQ(v[0].valid_ratio, 0.5f);
  EXPECT_FLOAT_EQ(v[0].snr_db, 10.0f);
  EXPECT_EQ(v[0].last_valid_wb_ns, 0u); /* 100 < sense_min_prb (128) */
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 4, 200, 20.0f, tb(3));
  ASSERT_EQ(snap(0, tb(4), v), 1);
  EXPECT_EQ(v[0].last_valid_wb_ns, tb(3));
  EXPECT_GT(v[0].valid_ratio, 0.5f);
  EXPECT_GT(v[0].snr_db, 10.0f);
  EXPECT_LT(v[0].snr_db, 20.0f);
}

TEST_F(Unit, OnlyInSensingMode)
{
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  EXPECT_EQ(nr_sense_unit_cpu_ns(), 0u);
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 1, MS, 0, true, 200, tb(1));
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 1, 200, 5.f, tb(1));
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 1);
  nr_sense_unit_skip(NR_UNIT_PDSCH_DATA, 1, 200, tb(1));
  nr_isac_mode_set(NR_ISAC_MODE_TRAFFIC);
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 1, MS, 0, true, 200, tb(1));
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_unit_view_t v[8];
  EXPECT_EQ(snap(0, tb(2), v), 0);
  EXPECT_NE(nr_sense_unit_cpu_ns(), 0u);
}

TEST_F(Unit, ConcurrentWritersLoseNothing)
{
  /* 4 threads, same unit, same buckets: the sums are exact */
  const int N = 20000;
  std::vector<std::thread> th;
  for (int w = 0; w < 4; w++)
    th.emplace_back([N] {
      nr_cell_bind(0);
      for (int i = 0; i < N; i++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 11, 1000, 10, i % 2, 200, tb(i % 8));
    });
  for (auto &t : th) t.join();
  uint64_t sum = 0;
  for (int k = 0; k < 8; k++) sum += nr_sense_unit_bucket_cpu(0, 11, NR_UNIT_PDSCH_DATA, tb(k));
  EXPECT_EQ(sum, 4ull * N * 1000);
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(8), v), 1);
  EXPECT_EQ(v[0].late, 4u * N / 2);
  /* messages saturate per bucket (10 bits, 20 kHz) but never wrap */
  EXPECT_GT(v[0].rate_hz, 0.0f);
}

TEST_F(Unit, ConcurrentCreateOfDistinctRntis)
{
  std::vector<std::thread> th;
  for (int w = 0; w < 4; w++)
    th.emplace_back([w] {
      nr_cell_bind(0);
      for (int r = 0; r < 200; r++) nr_sense_unit_job((nr_unit_src_t)(w & 3), (uint16_t)(1 + r % 40), 100, 0, false, 200, tb(r % 4));
    });
  for (auto &t : th) t.join();
  nr_sense_unit_view_t v[256];
  const int n = snap(0, tb(5), v);
  EXPECT_LE(n, 4 * 40);
  EXPECT_GE(n, 1);
  for (int i = 0; i < n; i++) EXPECT_TRUE(v[i].rnti >= 1 || v[i].src == NR_UNIT_SRS); /* SRS is the per-cell rnti-0 unit */
}

TEST_F(Unit, NarrowRntisNeverEvictAWidebandUnit)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 7, MS, 0, false, 200, tb(1)); /* the wideband unit, oldest row */
  for (int r = 0; r < 500; r++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, (uint16_t)(1000 + r), 3, 0, false, 20, tb(2 + r % 50));
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 4242, 9, 0, false, 0, tb(60)); /* cost-only, no row: no row is opened either */
  nr_sense_unit_view_t v[256];
  ASSERT_EQ(snap(0, tb(100), v), 1);
  EXPECT_EQ(v[0].rnti, 7);
  EXPECT_EQ(nr_sense_unit_unattributed_ns(0), 500u * 3 + 9); /* nothing lost from the baseline */
  EXPECT_EQ(nr_sense_unit_unattributed_ns(1), 0u);
  /* a cost-only call for the existing unit is attributed to it */
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 7, 5 * MS, 0, false, 0, tb(61));
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 7, NR_UNIT_PDSCH_DATA, tb(61)), 5 * MS);
  EXPECT_EQ(nr_sense_unit_unattributed_ns(0), 500u * 3 + 9);
}

TEST_F(Unit, SkipKeepsAWidebandRate)
{
  for (int i = 0; i < 40; i++) nr_sense_unit_skip(NR_UNIT_PDSCH_DATA, 21, i % 2 ? 50 : 200, tb(i)); /* never ran, 20 wideband of 40 */
  nr_sense_unit_skip(NR_UNIT_PDSCH_DATA, 22, 20, tb(1));                                            /* a narrow skip opens no row */
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(40), v), 1);
  EXPECT_EQ(v[0].rnti, 21);
  EXPECT_EQ(v[0].skips, 40u);
  EXPECT_FLOAT_EQ(v[0].rate_hz, 40.0f / 2.0f); /* 40 buckets = 2 s old */
  EXPECT_FLOAT_EQ(v[0].wb_rate_hz, 20.0f / 2.0f);
}

TEST_F(Unit, SrsIsOnePerCellUnit)
{
  nr_sense_unit_job(NR_UNIT_SRS, 99, 4 * MS, 0, false, 0, tb(1));  /* the blind search: cost on the cell's SRS unit */
  nr_sense_unit_cfr(NR_UNIT_SRS, 1234, 150, 10.f, tb(1));         /* records with an attributed RNTI land on the same unit */
  nr_sense_unit_cfr(NR_UNIT_SRS, 5, 10, 8.f, tb(2));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(3), v), 1);
  EXPECT_EQ(v[0].src, NR_UNIT_SRS);
  EXPECT_EQ(v[0].rnti, 0);
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 0, NR_UNIT_SRS, tb(1)), 4 * MS);
  EXPECT_NEAR(v[0].rate_hz, 2.0f / 0.1f, 1e-2); /* 2 records over the unit's age (2 buckets) */
  EXPECT_NEAR(v[0].wb_rate_hz, 1.0f / 0.1f, 1e-2);
  EXPECT_FLOAT_EQ(v[0].valid_ratio, 0.75f);      /* records 1 (wideband-valid) then 0: EWMA 1 + 0.25 * (0 - 1) */
  EXPECT_EQ(v[0].last_valid_wb_ns, tb(1));
}

TEST_F(Unit, YoungUnitRateUsesItsAge)
{
  for (int k = 0; k < 20; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 31, MS, 0, false, 200, tb(k));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(20), v), 1);
  EXPECT_EQ(v[0].first_ns, tb(0));
  EXPECT_FLOAT_EQ(v[0].rate_hz, 20.0f / 1.0f); /* 20 messages in 1 s, not in 16 s */
  ASSERT_EQ(snap(0, tb(330), v), 1);
  EXPECT_FLOAT_EQ(v[0].rate_hz, 10.0f / 16.0f); /* once older than the window: the window (buckets 10..329 hold 10 messages) */
}

static void evict_row_77()
{
  nr_sense_unit_test_seam(nullptr);
  for (int r = 0; r < 64; r++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, (uint16_t)(1000 + r), MS, 0, false, 200, tb(10 + r));
}
TEST_F(Unit, LateWriterCannotTouchTheSuccessor)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 77, MS, 0, false, 200, tb(1)); /* oldest row */
  nr_sense_unit_view_t v[256];
  ASSERT_EQ(snap(0, tb(2), v), 1);
  const uint32_t g_before = v[0].gen;
  /* the writer has located row 77, the seam evicts it before the update: the violation, the late flag and the cost must go nowhere */
  nr_sense_unit_test_seam(evict_row_77);
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 77);
  nr_sense_unit_test_seam(nullptr);
  const int n = snap(0, tb(80), v);
  ASSERT_EQ(n, 64);
  for (int i = 0; i < n; i++) {
    EXPECT_NE(v[i].rnti, 77);
    EXPECT_EQ(v[i].violations, 0u) << "rnti " << v[i].rnti;
  }
  /* the evicted RNTI comes back on a new generation with counters from 0 */
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 77, MS, 0, true, 200, tb(90));
  int found = 0;
  for (int i = 0, m = snap(0, tb(91), v); i < m; i++)
    if (v[i].rnti == 77) { found++; EXPECT_NE(v[i].gen, g_before); EXPECT_EQ(v[i].late, 1u); }
  EXPECT_EQ(found, 1);
}

TEST_F(Unit, CurrentBucketDoesNotEraseTheOldestOfTheWindow)
{
  for (int k = 0; k < 320; k++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 8, (k == 0 ? 50 : 1) * MS, 0, false, 200, tb(k));
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 8, MS, 0, false, 200, tb(320)); /* first sample of the bucket being written */
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(320), v), 1);
  EXPECT_FLOAT_EQ(v[0].cpu_max_ns, 50.0f * MS);
}

TEST_F(Unit, OlderWriterNeverOverwritesANewerBucket)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 8, 5 * MS, 0, false, 200, tb(321));
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 8, 1 * MS, 0, false, 200, tb(0)); /* a thread a whole ring late shares the slot */
  EXPECT_EQ(nr_sense_unit_bucket_cpu(0, 8, NR_UNIT_PDSCH_DATA, tb(321)), 5 * MS);
}

/* cost of the accounting calls (printed, not asserted: host-dependent). Run: --gtest_also_run_disabled_tests --gtest_filter=*Cost* */
static double per_call_ns(int n, void (*f)(int))
{
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < n; i++) f(i);
  clock_gettime(CLOCK_MONOTONIC, &b);
  return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / n;
}
static volatile uint64_t sink;
TEST_F(Unit, DISABLED_Cost)
{
  const int N = 2000000;
  printf("cpu_ns() sensing      : %.1f ns\n", per_call_ns(N, [](int) { sink += nr_sense_unit_cpu_ns(); }));
  printf("job  (row hit, 1 unit): %.1f ns\n", per_call_ns(N, [](int i) { nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4601, 1000, 50, false, 200, tb(i >> 12)); }));
  for (int r = 1; r <= 63; r++) nr_sense_unit_job(NR_UNIT_PDSCH_DATA, (uint16_t)(0x5000 + r), 1, 0, false, 200, tb(0));
  printf("job  (row 64th of 64) : %.1f ns\n", per_call_ns(N, [](int i) { nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x5000 + 63, 1000, 50, false, 200, tb(i >> 12)); }));
  printf("cfr                   : %.1f ns\n", per_call_ns(N, [](int i) { nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x4601, 150, 12.f, tb(i >> 12)); }));
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  printf("cpu_ns() legacy       : %.1f ns\n", per_call_ns(N, [](int) { sink += nr_sense_unit_cpu_ns(); }));
  printf("job  legacy           : %.1f ns\n", per_call_ns(N, [](int i) { nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4601, 1000, 50, false, 200, tb(i >> 12)); }));
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  printf("snapshot of 64 units  : ");
  nr_sense_unit_view_t v[256];
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  int n = 0;
  for (int i = 0; i < 100; i++) n = snap(0, tb(2000), v);
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("%d units, %.1f us per snapshot\n", n, ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 100 / 1e3);
}
