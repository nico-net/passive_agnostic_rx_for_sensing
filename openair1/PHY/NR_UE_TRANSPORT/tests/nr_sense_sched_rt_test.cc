// Plan S7: the unit gates, violation routing and the scheduler wiring (real unit table, stubbed resource sampler).
#include "nr_sense_sched_rt.h"
#include "nr_sense_unit.h"
#include "common/utils/nr_cell.h"
#include <gtest/gtest.h>
#include <unistd.h>
extern "C" {
void *uniqCfg = nullptr; /* the LOG library of the scheduler's log line needs these two */
void exit_function(const char *, const char *, int, const char *s, int) { fprintf(stderr, "%s\n", s ? s : ""); abort(); }
}

static const uint64_t B = SENSE_BUCKET_NS;
static const uint64_t T0 = 200000ull * B;
static uint64_t tb(uint64_t k) { return (T0 + k) * B + 1000; }
static const uint64_t DEC = SENSE_DECIDE_NS / B; /* buckets per decision */
static nr_sense_res_t g_cap[NR_RES_N];

static bool stub_sampler(nr_sense_sched_sample_t *o, uint64_t)
{
  memset(o, 0, sizeof *o);
  o->n_cells = 1;
  o->pci[0] = 415;
  memcpy(o->res, g_cap, sizeof g_cap);
  return true;
}

class Sched : public ::testing::Test {
 protected:
  void SetUp() override
  {
    nr_isac_mode_set(NR_ISAC_MODE_SENSING);
    nr_isac_sense_min_prb_set(128);
    nr_cell_bind(0);
    for (int c = 0; c < 2; c++) nr_sense_unit_reset(c);
    nr_sense_sched_reset();
    for (int r = 0; r < NR_RES_N; r++) g_cap[r] = {200e6f, 0.f};
    g_cap[NR_RES_GPU] = {0.f, 0.f}; /* no GPU on this host: absent, not invalid */
    nr_sense_sched_set_sampler(stub_sampler);
  }
  void TearDown() override { nr_sense_sched_reset(); nr_isac_mode_set(NR_ISAC_MODE_LEGACY); nr_cell_bind(0); }
  /* messages of a unit that keeps arriving while it is off: one per bucket pair over [from, to) */
  static void feed_skips(uint16_t rnti, nr_unit_src_t s, uint64_t from, uint64_t to, uint16_t est = 200)
  {
    for (uint64_t k = from; k < to; k += 2) nr_sense_unit_skip(s, rnti, est, tb(k));
  }
  static int snap_now(nr_sense_unit_view_t *v, int n)
  {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return nr_sense_unit_snapshot_at(0, (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec + 2 * B, v, n); /* the bucket holding "now" is not in the window */
  }
  static nr_sense_sched_stats_t stats() { nr_sense_sched_stats_t s; nr_sense_sched_stats(&s); return s; }
};

TEST_F(Sched, DisabledUnitSkippedBeforeWork)
{
  int work = 0;
  auto run = [&](uint16_t est) { if (nr_sense_unit_gate(NR_UNIT_PDSCH_DMRS, 0x4601, est)) work++; };
  run(200); /* a new RNTI: no row yet, so disabled */
  EXPECT_EQ(work, 0);
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.skips, 1u);
  nr_sense_unit_set_enabled(0, 0x4601, NR_UNIT_PDSCH_DMRS, true);
  run(200);
  EXPECT_EQ(work, 1);
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.skips, 1u); /* an enabled unit is never counted as a skip */
}

TEST_F(Sched, SkipCarriesTheUsablePrbs)
{
  for (int i = 0; i < 5; i++) EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4602, 200));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap_now(v, 8), 1);
  EXPECT_EQ(v[0].skips, 5u);
  EXPECT_EQ(v[0].est_usable_prbs, 200); /* without the estimate the row would never be eligible */
  EXPECT_GT(v[0].wb_rate_hz, 0.f);
  EXPECT_FALSE(v[0].enabled);
  /* a narrow message opens no row */
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4603, 20));
  ASSERT_EQ(snap_now(v, 8), 1);
}

TEST_F(Sched, SrsGateCountsOneWidebandMessage)
{
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_SRS, 0, 0)); /* the per-cell unit, no RNTI, no PRB count: sense_min_prb is used */
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(snap_now(v, 8), 1);
  EXPECT_EQ(v[0].rnti, 0);
  EXPECT_GT(v[0].wb_rate_hz, 0.f);
  EXPECT_FALSE(v[0].enabled);
}

TEST_F(Sched, NewRowsStartDisabledInEverySource)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4604, 1000000, 0, false, 200, tb(1));
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DMRS, 0x4604, 200, 10.f, tb(1));
  nr_sense_unit_view_t v[8];
  const int n = nr_sense_unit_snapshot_at(0, tb(2), v, 8);
  ASSERT_EQ(n, 2);
  for (int i = 0; i < n; i++) EXPECT_FALSE(v[i].enabled) << i;
  EXPECT_FALSE(nr_sense_unit_enabled(NR_UNIT_PUSCH_DMRS, 0x4604)); /* a source never touched is disabled too */
}

TEST_F(Sched, InFlightFinishesAfterDisable)
{
  nr_sense_unit_set_enabled(0, 0x4605, NR_UNIT_PDSCH_DATA, true);
  ASSERT_TRUE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4605, 200)); /* admitted */
  nr_sense_unit_set_enabled(0, 0x4605, NR_UNIT_PDSCH_DATA, false);  /* the scheduler disables while it is in flight */
  /* the work is not re-gated: it finishes and its cost and record are accounted */
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4605, 3000000, 0, false, 200, tb(3));
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x4605, 200, 12.f, tb(3));
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(nr_sense_unit_snapshot_at(0, tb(4), v, 8), 1);
  EXPECT_EQ(v[0].n_cfr, 1u);
  EXPECT_GT(v[0].cpu_p95_ns + v[0].cpu_max_ns, 0.f);
  EXPECT_EQ(v[0].skips, 0u);
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4605, 200)); /* the next message is skipped */
}

TEST_F(Sched, DropInEnabledUnitIsViolation)
{
  nr_sense_unit_set_enabled(0, 0x4606, NR_UNIT_PDSCH_DATA, true);
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 0x4606);
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 0x4606);
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(nr_sense_unit_snapshot_at(0, tb(1), v, 8), 1);
  EXPECT_EQ(v[0].violations, 2u);
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.violations, 2u);
}

TEST_F(Sched, DropInDisabledUnitIsNotViolation)
{
  nr_sense_unit_job(NR_UNIT_PDSCH_DATA, 0x4607, 1000, 0, false, 200, tb(1)); /* the row exists, disabled */
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 0x4607);
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 0x9999); /* no row at all */
  nr_sense_unit_view_t v[8];
  ASSERT_EQ(nr_sense_unit_snapshot_at(0, tb(2), v, 8), 1);
  EXPECT_EQ(v[0].violations, 0u);
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.violations, 0u);
}

TEST_F(Sched, SsbCsirsNeverGated)
{
  /* SSB and CSI-RS are not units: a source outside nr_unit_src_t is never gated, in any mode, and nothing is counted */
  const nr_unit_src_t none = (nr_unit_src_t)(NR_UNIT_SRC_N + 3);
  EXPECT_TRUE(nr_sense_unit_enabled(none, 5));
  EXPECT_TRUE(nr_sense_unit_gate(none, 5, 200));
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.skips, 0u);
  nr_sense_unit_view_t v[8];
  EXPECT_EQ(snap_now(v, 8), 0);
}

TEST_F(Sched, GatesOpenOutsideSensingMode)
{
  for (nr_isac_mode_t m : {NR_ISAC_MODE_LEGACY, NR_ISAC_MODE_TRAFFIC}) {
    nr_isac_mode_set(m);
    for (int s = 0; s < NR_UNIT_SRC_N; s++) EXPECT_TRUE(nr_sense_unit_gate((nr_unit_src_t)s, 0x4608, 200));
  }
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_unit_view_t v[8];
  EXPECT_EQ(snap_now(v, 8), 0);
}

TEST_F(Sched, ColdUnitIsEnabledAfterTwoDecisions)
{
  feed_skips(0x4609, NR_UNIT_PDSCH_DATA, 0, 3 * DEC);
  nr_sense_sched_step(tb(DEC));
  EXPECT_FALSE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x4609)); /* streak 1 of SENSE_CONFIRM */
  nr_sense_sched_step(tb(2 * DEC));
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x4609));
  const auto st = stats();
  EXPECT_EQ(st.decisions, 2u);
  EXPECT_EQ(st.enables, 1u);
  EXPECT_EQ(st.bad_cap, 0u);
  EXPECT_EQ(st.deferrals, 0u);
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.units_enabled, 1u);
}

TEST_F(Sched, ViolationRemovesTheUnitAtOnce)
{
  feed_skips(0x460a, NR_UNIT_PDSCH_DATA, 0, 4 * DEC);
  nr_sense_sched_step(tb(DEC));
  nr_sense_sched_step(tb(2 * DEC));
  ASSERT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x460a));
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 0x460a); /* e.g. a queue-full drop of its message */
  nr_sense_sched_step(tb(3 * DEC));
  EXPECT_FALSE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x460a)); /* no streak needed */
  EXPECT_EQ(stats().disables, 1u);
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.violations, 1u);
}

TEST_F(Sched, ZeroCapOnAQueueAdmitsNothingButAbsentGpuIsFine)
{
  g_cap[NR_RES_QUEUE_DL] = {0.f, 0.f}; /* a cap left 0 is invalid input */
  feed_skips(0x460b, NR_UNIT_PDSCH_DATA, 0, 5 * DEC);
  for (int d = 1; d <= 4; d++) nr_sense_sched_step(tb(d * DEC));
  EXPECT_FALSE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x460b));
  EXPECT_GE(stats().bad_cap, 4u);
  EXPECT_EQ(stats().enables, 0u);
  /* the GPU cap is 0 in this fixture (absent): the queue fixed, the unit is enabled all the same */
  g_cap[NR_RES_QUEUE_DL] = {200e6f, 0.f};
  feed_skips(0x460b, NR_UNIT_PDSCH_DATA, 5 * DEC, 8 * DEC);
  nr_sense_sched_step(tb(5 * DEC));
  nr_sense_sched_step(tb(6 * DEC));
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x460b));
}

TEST_F(Sched, UnusableSampleIsADeferral)
{
  nr_sense_sched_set_sampler([](nr_sense_sched_sample_t *, uint64_t) { return false; });
  nr_sense_sched_step(tb(DEC));
  EXPECT_EQ(stats().deferrals, 1u);
  EXPECT_EQ(stats().decisions, 0u);
}

TEST_F(Sched, WidebandCountersAndGap)
{
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x460c, 200, 10.f, tb(10));
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x460c, 150, 10.f, tb(30)); /* 20 buckets = 1 s later */
  nr_sense_unit_cfr(NR_UNIT_PDSCH_DATA, 0x460c, 20, 10.f, tb(31)); /* narrow: not counted */
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_EQ(cs.wb_valid, 2u);
  EXPECT_EQ(cs.wb_valid_prbs, 350u);
  EXPECT_EQ(cs.gap_max_ns, 20 * B);
}

TEST_F(Sched, StartIsANoopOutsideSensingAndStopIsSafe)
{
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  nr_sense_sched_start();
  nr_sense_sched_stop();
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_sched_start(); /* the stub sampler: the thread runs one silent step at start */
  nr_sense_sched_start(); /* idempotent */
  for (int i = 0; i < 200 && stats().decisions == 0; i++) usleep(10000);
  nr_sense_sched_stop();
  nr_sense_sched_stop();
  EXPECT_EQ(stats().decisions, 1u); /* exactly the first step: the next one is SENSE_DECIDE_NS later */
}
