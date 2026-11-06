#include "nr_sense_unit.h"
#include "nr_pdsch_da_blocks.h"
#include "nr_sense_sched.h"
#include "nr_sense_api.h"
#include "common/utils/nr_cell.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>

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
  nr_sense_unit_view_t vw[8];
  ASSERT_GE(snap(0, tb(7), vw), 1);
  EXPECT_EQ(vw[0].n_cfr, 0u);
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x4601, 150, 10.f, tb(6));
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x4601, 150, 10.f, tb(6));
  ASSERT_GE(snap(0, tb(7), vw), 1);
  EXPECT_EQ(vw[0].n_cfr, 2u); /* since row creation */
  EXPECT_EQ(vw[0].last_cfr_ns, tb(6));
  EXPECT_EQ(vw[0].est_usable_prbs, 200); /* the latest usable PRBs are in the view (scheduler value) */
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
  nr_sense_unit_set_enabled(0, 9, NR_UNIT_PDSCH_DATA, true); /* a new row starts disabled (S7); a drop counts only in an enabled unit */
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
    nr_sense_unit_set_enabled(1, 6, NR_UNIT_PDSCH_DMRS, true);
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
  nr_sense_unit_set_enabled(0, 77, NR_UNIT_PDSCH_DATA, true); /* enabled: a violation that reached it would count */
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

TEST_F(Unit, RatioInitialisedByFirstRecord)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4601, MS, 0, false, 200, tb(1));
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x4601, 20, 5.f, tb(2));   /* first record: invalid (20 of 200 PRBs) -> ratio 0.1 */
  nr_sense_unit_view_t vw[8];
  ASSERT_GE(snap(0, tb(3), vw), 1);
  EXPECT_FLOAT_EQ(vw[0].valid_ratio, 0.1f);
  for (int i = 0; i < 12; i++) nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x4601, 200, 5.f, tb(3) + i);
  ASSERT_GE(snap(0, tb(4), vw), 1);
  EXPECT_GT(vw[0].valid_ratio, 0.9f); /* follows the records, not an average from 0 */
}

TEST_F(Unit, RingAndDaCostStreamsAreSeparatePerUnit)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 5, 80 * MS, 0, false, 200, tb(1)); /* pool CPU */
  nr_sense_unit_cost_da(NR_UNIT_PDSCH_DATA, 5, 70 * MS, tb(1));
  nr_sense_unit_cost_ring(NR_UNIT_PDSCH_DATA, 5, 2 * MS, tb(1));
  nr_sense_unit_cost_ring(NR_UNIT_PDSCH_DATA, 99, 5 * MS, tb(1)); /* no row: not attributed, no row opened */
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap(0, tb(100), v), 1);
  EXPECT_FLOAT_EQ(v[0].cpu_max_ns, 80.f * MS);
  EXPECT_FLOAT_EQ(v[0].da_p99_ns, 0.f);   /* one busy bucket of 320 is not the p99 (nearest rank) ... */
  for (int k = 0; k < 320; k++) { nr_sense_unit_cost_da(NR_UNIT_PDSCH_DATA, 5, 3 * MS, tb(10 + k)); nr_sense_unit_cost_ring(NR_UNIT_PDSCH_DATA, 5, MS / 2, tb(10 + k)); }
  ASSERT_EQ(snap(0, tb(331), v), 1);
  EXPECT_FLOAT_EQ(v[0].da_p99_ns, 3.f * MS);    /* ... a steady one is */
  EXPECT_FLOAT_EQ(v[0].ring_p99_ns, 0.5f * MS);
}

TEST_F(Unit, ArrivalGapsPerThreshold)
{
  const uint64_t base = 500ull * 1000000000ull, win = 4000 * MS, now = base + win;
  const int thr[3] = {128, 64, 1};
  nr_sense_gapstat_t g[3];
  nr_sense_unit_arrival_gaps(0, now, win, thr, 3, g); /* nothing yet: one gap = the whole window, no arrivals */
  for (int i = 0; i < 3; i++) { EXPECT_EQ(g[i].n, 0u); EXPECT_EQ(g[i].p95_gap_ns, win); }
  /* a 200 PRB grant every 100 ms and a 70 PRB one halfway between: 40 + 40 arrivals */
  for (int i = 0; i < 40; i++) {
    nr_sense_dci_arrival(200, base + (uint64_t)i * 100 * MS + MS);
    nr_sense_dci_arrival(70, base + (uint64_t)i * 100 * MS + 50 * MS);
  }
  nr_sense_unit_arrival_gaps(0, now, win, thr, 3, g);
  EXPECT_EQ(g[0].n, 40u);
  EXPECT_EQ(g[1].n, 80u);
  EXPECT_EQ(g[2].n, 80u);
  EXPECT_NEAR((double)g[0].p95_gap_ns, 100.0 * MS, 1.0 * MS);  /* the 100 ms spacing; the open ends are shorter */
  EXPECT_NEAR((double)g[1].p95_gap_ns, 50.0 * MS, 1.0 * MS);
  EXPECT_NEAR(g[0].rate_hz, 10.f, 0.5f);
  EXPECT_NEAR(g[1].rate_hz, 20.f, 0.5f);
  /* the window now ends 2 s after the last arrival: a trailing gap of ~2 s is the p95 of the few gaps left */
  nr_sense_unit_arrival_gaps(0, now + 2000 * MS, win, thr, 1, g);
  EXPECT_LT(g[0].n, 40u);
  EXPECT_GE(g[0].p95_gap_ns, 100 * MS);
}

/* S9 fix round 2: a grant between T_cell and sense_min_prb opens its unit, is gated like a wideband one and its CFR is exported; the wideband metric keeps sense_min_prb. */
TEST_F(Unit, NarrowContinuityGrantOpensTheUnitAndExports)
{
  nr_sense_cell_floor_reset();
  nr_sense_cell_floor_publish(0, 64); /* T_cell = 64, sense_min_prb = 128 */
  const uint64_t t = nr_sense_now_ns();
  nr_sense_unit_view_t vw[8];
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4601, 64)); /* disabled: skipped, but the 64 PRB grant is a message of a new row */
  ASSERT_EQ(snap(0, t + 2 * B, vw), 1);
  EXPECT_GT(vw[0].wb_rate_hz, 0.f); /* the scheduler can value it */
  EXPECT_EQ(vw[0].skips, 1u);
  nr_sense_unit_set_enabled(0, 0x4601, NR_UNIT_PDSCH_DATA, true);
  EXPECT_TRUE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4601, 64)); /* exported */
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x4601, 64, 10.f, t + 20 * MS);
  ASSERT_EQ(snap(0, t + 30 * MS, vw), 1);
  EXPECT_EQ(vw[0].n_cfr, 1u);
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.wb_valid, 0u); /* 64 < sense_min_prb: not a wideband record */
  /* below T_cell: no row, nothing gated or booked */
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4602, 48));
  EXPECT_EQ(snap(0, t + 40 * MS, vw), 1);
  /* the floor back at sense_min_prb: a 64 PRB message of a new RNTI opens nothing */
  nr_sense_cell_floor_publish(0, 128);
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4603, 64));
  EXPECT_EQ(snap(0, t + 50 * MS, vw), 1);
}

TEST_F(Unit, CellResyncRestartsTheFloor)
{
  nr_sense_cell_floor_publish(0, 64);
  nr_sense_unit_cell_resync(0);
  EXPECT_EQ(nr_sense_cell_floor(0), nr_isac_sense_min_prb());
  nr_sense_cell_floor_publish(0, 64);
  nr_sense_unit_reset(0);
  EXPECT_EQ(nr_sense_cell_floor(0), nr_isac_sense_min_prb());
}

TEST_F(Unit, ArrivalGapsIgnoreExportedCfrs)
{
  const uint64_t base = 500ull * 1000000000ull, win = 4000 * MS, now = base + win;
  const int thr[1] = {128};
  nr_sense_gapstat_t g[1];
  for (int i = 0; i < 40; i++) nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 4, 200, 10.f, base + (uint64_t)i * 100 * MS + MS); /* exported CFRs are not arrivals */
  nr_sense_unit_arrival_gaps(0, now, win, thr, 1, g);
  EXPECT_EQ(g[0].n, 0u);
  EXPECT_EQ(g[0].p95_gap_ns, win);
}

TEST_F(Unit, ArrivalGapsWindowAndWrap)
{
  const uint64_t base = 900ull * 1000000000ull;
  const int thr[1] = {128};
  nr_sense_gapstat_t g[1];
  for (int i = 0; i < 5000; i++) nr_sense_dci_arrival(200, base + (uint64_t)i * MS); /* more than the 4096-arrival ring, 1 kHz */
  nr_sense_unit_arrival_gaps(0, base + 5000 * MS, 5000 * MS, thr, 1, g);
  EXPECT_EQ(g[0].n, 4095u);                                    /* the oldest arrival only marks where the shortened window starts */
  EXPECT_NEAR((double)g[0].p95_gap_ns, 1.0 * MS, 0.1 * MS);   /* no 5 s hole although the window asked for 5 s */
  EXPECT_NEAR(g[0].rate_hz, 1000.f, 5.f);
}

/* ---- grant-level wideband path (nr_sense_wbg_*) ---- */
TEST_F(Unit, WbgFiniteBeforeADecisionAndNarrowIsRefused)
{
  int ok = 0;
  for (int i = 0; i < 50; i++) ok += nr_sense_wbg_admit(217, tb(1) + i) ? 1 : 0;
  EXPECT_EQ(ok, 4); /* never unlimited: before the first decision only the bucket depth (4) is available, then 1 per decision interval */
  EXPECT_FALSE(nr_sense_wbg_admit(127, tb(1))); /* below the wideband target (sense_min_prb): not this path (not even counted) */
  nr_sense_cell_floor_publish(0, 32);
  EXPECT_FALSE(nr_sense_wbg_admit(100, tb(1))); /* the continuity floor T_cell does not lower it */
  nr_sense_cell_floor_publish(0, 0);
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.offered, 50u);
  EXPECT_EQ(s.admitted, 4u);
  EXPECT_NEAR(s.rate_hz, 0.25f, 1e-3); /* the startup rate: one tap per SENSE_DECIDE_NS */
  EXPECT_FALSE(nr_sense_wbg_admit(217, tb(1) + 1000000)); /* 1 ms later: still nothing */
  EXPECT_TRUE(nr_sense_wbg_admit(217, tb(1) + 4100ull * MS)); /* one decision interval later: one more */
}

TEST_F(Unit, WbgSiteConditions)
{
  /* consumer site: the TB CRC must have passed (a CRC-failed grant on a SOLVED layout has a DCI nobody verified) and the RNTI must be a C-RNTI */
  EXPECT_TRUE(nr_sense_wbg_consumer_ok(true, true));
  EXPECT_FALSE(nr_sense_wbg_consumer_ok(false, true));
  EXPECT_FALSE(nr_sense_wbg_consumer_ok(true, false));
  /* monitor site: C-RNTI, proven fields (DCI 1_0 or a non-dl_auto 1_1), and no unsettled sweep hypothesis */
  EXPECT_TRUE(nr_sense_wbg_monitor_ok(true, true, false, false));
  EXPECT_TRUE(nr_sense_wbg_monitor_ok(true, true, true, true));
  EXPECT_FALSE(nr_sense_wbg_monitor_ok(false, true, false, false)); /* SI/RA/TC/P RNTI */
  EXPECT_FALSE(nr_sense_wbg_monitor_ok(true, false, false, false)); /* dl_auto 1_1: layout not proven */
  EXPECT_FALSE(nr_sense_wbg_monitor_ok(true, true, true, false));   /* sweep hypothesis not settled */
  /* export: a record needs a noise variance, a channel power and at least one RE */
  EXPECT_TRUE(nr_sense_wbg_export_ok(1.f, 2.0, 10));
  EXPECT_FALSE(nr_sense_wbg_export_ok(0.f, 2.0, 10));
  EXPECT_FALSE(nr_sense_wbg_export_ok(1.f, 0.0, 10));
  EXPECT_FALSE(nr_sense_wbg_export_ok(1.f, 2.0, 0));
  EXPECT_FALSE(nr_sense_wbg_export_ok(NAN, 2.0, 10));
}

TEST_F(Unit, WbgNoDoubleExportWhenTheUnitIsOpen)
{
  nr_sense_wbg_set_rate(0, 100.f);
  EXPECT_EQ(nr_sense_wbg_after_gate(true, true, 217, tb(1)), 0);   /* the unit's own tap runs: wbg takes no token and counts nothing */
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.offered, 0u);
  EXPECT_EQ(nr_sense_wbg_after_gate(false, true, 217, tb(1)), 1);  /* disabled unit, site ok: wbg */
  EXPECT_EQ(nr_sense_wbg_after_gate(false, false, 217, tb(1)), -1); /* disabled unit, site refuses (CRC): none */
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.offered, 1u);
}

TEST_F(Unit, WbgCellsBeyondTheSchedulerAreNeverUnlimitedAndCounted)
{
  if (NR_CELL_MAX <= NR_SENSE_CELLS) GTEST_SKIP();
  nr_cell_bind(NR_SENSE_CELLS); /* a cell the scheduler does not decide for */
  EXPECT_FALSE(nr_sense_wbg_admit(217, tb(1)));
  EXPECT_FALSE(nr_sense_wbg_admit(217, tb(1)));
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(NR_SENSE_CELLS, &s);
  EXPECT_EQ(s.overcap, 2u); /* not silent: counted (offered_wbg_overcap) */
  EXPECT_EQ(s.admitted, 0u);
  nr_cell_bind(0);
}

TEST_F(Unit, WbgTapNeedsTheCrcAndTheRingStillHoldingTheSlot)
{
  EXPECT_TRUE(nr_sense_wbg_tap_ok(true, true));
  EXPECT_FALSE(nr_sense_wbg_tap_ok(true, false)); /* credit expired after the CRC pass: the IQ is another slot's */
  EXPECT_FALSE(nr_sense_wbg_tap_ok(false, true));
}

TEST_F(Unit, WbgRefundsBalanceTheTaps)
{
  nr_sense_wbg_set_rate(0, 0.f);
  int adm = 0;
  for (int i = 0; i < 6; i++) adm += nr_sense_wbg_admit(217, tb(1)) ? 1 : 0; /* 4 tokens */
  ASSERT_EQ(adm, 4);
  nr_sense_wbg_refund(); /* enqueue refused */
  nr_sense_wbg_refund(); /* ring credit expired */
  nr_sense_wbg_cost(100000); nr_sense_wbg_cost(100000); /* two taps ran */
  nr_sense_wbg_wasted(50000); /* CPU of a tap that exported nothing */
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.admitted - s.refunded, s.taps);
  EXPECT_EQ(s.cost_ns, 250000u);
  int more = 0; /* the two refunded tokens are back */
  for (int i = 0; i < 6; i++) more += nr_sense_wbg_admit(150, tb(1)) ? 1 : 0;
  EXPECT_EQ(more, 2);
}

TEST_F(Unit, WbgRefundIsCappedAtTheBucketDepth)
{
  nr_sense_wbg_set_rate(0, 0.f);
  EXPECT_TRUE(nr_sense_wbg_admit(217, tb(1)));
  for (int i = 0; i < 20; i++) nr_sense_wbg_refund();
  int ok = 0;
  for (int i = 0; i < 20; i++) ok += nr_sense_wbg_admit(150, tb(1)) ? 1 : 0;
  EXPECT_EQ(ok, 4); /* never above the depth */
}

TEST_F(Unit, WbgConcurrentAdmitsStayWithinTheTokens)
{
  nr_sense_wbg_set_rate(0, 0.f); /* no refill: only the bucket (4) and at most the 2 tokens of borrowing */
  std::atomic<int> ok{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 8; t++) th.emplace_back([&] { nr_cell_bind(0); for (int i = 0; i < 2000; i++) if (nr_sense_wbg_admit(217, tb(1))) ok++; });
  for (auto &x : th) x.join();
  EXPECT_LE(ok.load(), 6);
  EXPECT_GE(ok.load(), 4);
}

TEST_F(Unit, WbgRingCostAndReset)
{
  nr_sense_wbg_ring_cost(70000);
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.ring_ns, 70000u);
  nr_sense_wbg_admit(217, tb(1));
  nr_sense_unit_reset(0); /* a new cell: counters, bucket and rate restart */
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.ring_ns, 0u);
  EXPECT_EQ(s.offered, 0u);
  EXPECT_NEAR(s.rate_hz, 0.25f, 1e-3);
}

TEST_F(Unit, WbgOnlyInSensingMode)
{
  nr_isac_mode_set(NR_ISAC_MODE_TRAFFIC);
  EXPECT_FALSE(nr_sense_wbg_admit(217, tb(1)));
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  EXPECT_FALSE(nr_sense_wbg_admit(217, tb(1)));
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.offered, 0u); /* no state touched outside SENSING */
}

TEST_F(Unit, WbgRateLimitsAndTheGapRefills)
{
  nr_sense_wbg_set_rate(0, 10.f); /* 10 grants/s */
  const uint64_t t0 = 5000ull * 1000000000ull;
  int ok = 0;
  for (int i = 0; i < 400; i++) ok += nr_sense_wbg_admit(217, t0 + (uint64_t)i * 5 * MS) ? 1 : 0; /* 200 grants/s for 2 s, same width */
  EXPECT_GE(ok, 22); /* burst of 4 + 2 s x 10/s */
  EXPECT_LE(ok, 26);
  /* after a 1 s silence 10 more tokens accrued (bucket depth 4): the oldest gap is served first */
  const uint64_t t1 = t0 + 400 * 5 * MS + 1000 * MS;
  int burst = 0;
  for (int i = 0; i < 10; i++) burst += nr_sense_wbg_admit(217, t1 + (uint64_t)i * MS) ? 1 : 0;
  EXPECT_EQ(burst, 4);
}

TEST_F(Unit, WbgZeroRateKeepsOnlyTheWidestAndStopsAtTheDebtBound)
{
  nr_sense_wbg_set_rate(0, 0.f);
  const uint64_t t0 = 6000ull * 1000000000ull;
  int ok = 0;
  for (int i = 0; i < 4; i++) ok += nr_sense_wbg_admit(150, t0 + i) ? 1 : 0; /* the bucket starts full: 4 grants */
  EXPECT_EQ(ok, 4);
  EXPECT_FALSE(nr_sense_wbg_admit(150, t0 + 10)); /* empty, and not wider than the widest admitted */
  EXPECT_TRUE(nr_sense_wbg_admit(217, t0 + 11));  /* wider than any this window: may borrow ... */
  EXPECT_TRUE(nr_sense_wbg_admit(273, t0 + 12));
  EXPECT_FALSE(nr_sense_wbg_admit(273, t0 + 13)); /* ... never below the debt bound */
  EXPECT_FALSE(nr_sense_wbg_admit(200, t0 + 14)); /* narrower than the widest */
}

TEST_F(Unit, WbgCostAndCellBookingNeverTouchAnRntiUnit)
{
  nr_sense_wbg_cost(100000);
  nr_sense_wbg_cost(300000);
  nr_sense_wbg_cfr(217, tb(3));
  nr_sense_wbg_cfr(60, tb(4));
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.taps, 2u);
  EXPECT_EQ(s.cost_ns, 400000u);
  EXPECT_EQ(s.records, 2u);
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.wb_valid, 1u); /* the 217-PRB record is a cell wideband record, the 60-PRB one is not */
  nr_sense_unit_view_t vw[8];
  EXPECT_EQ(snap(0, tb(5), vw), 0); /* no row was opened, no unit cost changed */
  EXPECT_EQ(nr_sense_unit_unattributed_ns(0), 0u);
}

TEST_F(Unit, TbProvenTruthTable)
{
  EXPECT_TRUE(nr_sense_tb_proven(true, 1, 0));   /* C = 1: TB CRC only */
  EXPECT_FALSE(nr_sense_tb_proven(false, 1, 1)); /* C = 1 has no code-block CRC: cb_ok must not prove it */
  EXPECT_FALSE(nr_sense_tb_proven(false, 5, 0));
  EXPECT_TRUE(nr_sense_tb_proven(false, 5, 1));
  EXPECT_TRUE(nr_sense_tb_proven(false, 5, 5));
  EXPECT_TRUE(nr_sense_tb_proven(false, 90, 1)); /* more than 64 blocks: counts, not the 64-bit map */
  EXPECT_TRUE(nr_sense_tb_proven(true, 90, 0));
}

TEST_F(Unit, SingleCellSkipsTheBucketAndTheWidthFloorOnlyInSensing)
{
  nr_sense_single_set(true);
  int ok = 0;
  for (int i = 0; i < 200; i++) ok += nr_sense_wbg_admit((uint16_t)(20 + i % 200), tb(1) + i) ? 1 : 0;
  EXPECT_EQ(ok, 200);
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.offered, 200u); EXPECT_EQ(s.admitted, 200u);
  for (int i = 0; i < 200; i++) nr_sense_wbg_cost(1000);
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x1234));
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_SRS, 0));
  EXPECT_FALSE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DMRS, 0x1234));
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  EXPECT_FALSE(nr_sense_single()); /* the flag means nothing outside sensing */
  EXPECT_FALSE(nr_sense_wbg_admit(217, tb(1)));
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_single_set(false);
  EXPECT_FALSE(nr_sense_wbg_admit(100, tb(1))); /* multi-cell rule back: below sense_min_prb */
}

TEST(DaBlocksZero, AllZeroBlockWithCrcPassDoesNotProve)
{
  /* 3 blocks of 5 payload bytes in a buffer; block 1 is all zero but "passed" its CRC24B */
  uint8_t pay[15] = {1,2,3,4,5, 0,0,0,0,0, 9,8,7,6,5};
  nr_da_cb_pass_t p{}; p.C = 3; p.ok = 1;
  nr_da_cb_set(&p, 1, true);
  nr_da_cb_clear_zero(&p, pay, 5, 5, sizeof pay);
  EXPECT_EQ(p.ok, 0u);
  EXPECT_FALSE(nr_sense_tb_proven(false, p.C, p.ok)); /* the zero block alone proves nothing */
  nr_da_cb_set(&p, 2, true); p.ok = 1;
  nr_da_cb_clear_zero(&p, pay, 5, 5, sizeof pay);
  EXPECT_EQ(p.ok, 1u); /* a real block still proves, the zero one stays out */
  EXPECT_TRUE(nr_sense_tb_proven(false, p.C, p.ok));
  EXPECT_FALSE(nr_da_cb_get(&p, 1));
  const uint8_t shortbuf[6] = {0,0,0,0,0,7}; /* limit stops the read: block 1 starts at 5, only 1 byte (7) is inside */
  nr_da_cb_pass_t q{}; q.C = 2; nr_da_cb_set(&q, 1, true);
  nr_da_cb_clear_zero(&q, shortbuf, 5, 5, sizeof shortbuf);
  EXPECT_EQ(q.ok, 1u);
}

TEST(DaBlocksZero, TakeKeepsValidSymbolsInOrderForEveryNl)
{
  for (uint32_t nl = 1; nl <= 4; nl++) {
    const uint32_t n_re = 40;
    std::vector<uint8_t> valid(n_re);
    for (uint32_t i = 0; i < n_re; i++) valid[i] = (i / 7) % 2 == 0 ? 1 : 0; /* runs of valid / invalid REs */
    std::vector<int16_t> orig(2 * n_re * nl), work;
    for (size_t i = 0; i < orig.size(); i++) orig[i] = (int16_t)(1000 + i);
    work = orig;
    uint32_t kept = 0;
    std::vector<uint32_t> src;
    for (uint32_t i = 0; i < n_re; i++)
      if (nr_da_re_take(valid.data(), true, i * nl, nl, kept, work.data())) { src.push_back(i); kept++; }
    for (uint32_t j = 0; j < kept; j++)
      for (uint32_t l = 0; l < nl && nl > 1; l++) {
        EXPECT_EQ(work[2 * (j * nl + l)], orig[2 * (src[j] * nl + l)]) << "nl " << nl;
        EXPECT_EQ(work[2 * (j * nl + l) + 1], orig[2 * (src[j] * nl + l) + 1]);
      }
    uint32_t want = 0; for (auto v : valid) want += v;
    EXPECT_EQ(kept, want);
    EXPECT_TRUE(nr_da_re_take(valid.data(), false, 0, nl, 0, work.data())); /* whole TB: everything taken, nothing moved */
  }
}

TEST_F(Unit, WbqCountsRulesOfShortWideRecords)
{
  nr_sense_quality_t q{};
  q.valid_cfr_prbs = 100; q.inval[NR_SENSE_INV_SNR] = 20; q.inval[NR_SENSE_INV_RECON] = 5;
  nr_sense_wbq_note(&q);
  q.valid_cfr_prbs = 130; q.inval[NR_SENSE_INV_RECON] = 0; q.inval[NR_SENSE_INV_RESID] = 3; /* wide enough: counted as wide, not as short */
  nr_sense_wbq_note(&q);
  q.valid_cfr_prbs = 120; memset(q.inval, 0, sizeof q.inval); q.inval[NR_SENSE_INV_RECON] = 8;
  nr_sense_wbq_note(&q);
  nr_sense_wbq_stats_t w;
  nr_sense_wbq_stats(0, &w);
  EXPECT_EQ(w.wide, 3u); EXPECT_EQ(w.shorts, 2u);
  EXPECT_EQ(w.hit[NR_SENSE_INV_SNR], 1u); EXPECT_EQ(w.first[NR_SENSE_INV_SNR], 1u); EXPECT_EQ(w.prbs[NR_SENSE_INV_SNR], 20u);
  EXPECT_EQ(w.hit[NR_SENSE_INV_RECON], 2u); EXPECT_EQ(w.first[NR_SENSE_INV_RECON], 1u); EXPECT_EQ(w.prbs[NR_SENSE_INV_RECON], 13u);
  EXPECT_EQ(w.hit[NR_SENSE_INV_RESID], 0u);
  nr_sense_wbq_stats(1, &w); EXPECT_EQ(w.wide, 0u); // per cell
}
