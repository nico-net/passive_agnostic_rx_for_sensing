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
static int g_pci1 = 923; /* -1: the second cell is registered but has not synced */
static int g_ncells = 2; /* the multi-cell scheduler is what the older tests exercise; the single-cell tests set 1 */

static bool stub_sampler(nr_sense_sched_sample_t *o, uint64_t)
{
  memset(o, 0, sizeof *o);
  o->n_cells = g_ncells;
  o->pci[0] = 415; o->pci[1] = g_pci1;
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
    g_ncells = 2; g_pci1 = 923;
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

/* The solver DM-RS tap (consumer_dmrs_tap) reaches a disabled unit only through nr_sense_unit_gate: that call alone must give the unit the history the
 * scheduler needs, so a new (RNTI, PDSCH_DMRS) unit is enabled through the cold path, next to the same RNTI's PDSCH_DATA unit (one cold unit per decision). */
TEST_F(Sched, DmrsUnitGainsHistoryFromTheGateAndIsEnabledNextToData)
{
  for (uint64_t k = 0; k < 6 * DEC; k += 2) {
    for (nr_unit_src_t x : {NR_UNIT_PDSCH_DATA, NR_UNIT_PDSCH_DMRS}) nr_sense_unit_skip(x, 0x4660, 200, tb(k)); /* what nr_sense_unit_gate does for a disabled unit */
  }
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DMRS, 0x4661, 106)); /* the gate itself: new RNTI, disabled, counted */
  nr_sense_unit_cell_stats_t cs;
  nr_sense_unit_cell_stats(0, &cs);
  EXPECT_GE(cs.skips, 1u);
  for (int d = 1; d <= 5; d++) nr_sense_sched_step(tb(d * DEC));
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x4660));
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DMRS, 0x4660));
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

TEST_F(Sched, OfferedGrantsWeightTheNextSkip)
{
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4701, 200)); /* nothing offered: one message */
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  const uint64_t now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec + 2 * B; /* one fixed time for both snapshots */
  nr_sense_unit_view_t a[8], b[8];
  ASSERT_EQ(nr_sense_unit_snapshot_at(0, now, a, 8), 1);
  ASSERT_GT(a[0].wb_rate_hz, 0.f);
  nr_sense_unit_offer(NR_UNIT_PDSCH_DATA, 0x4701, 32); /* the policy let one check decode through for 32 grants */
  nr_sense_unit_offer(NR_UNIT_PDSCH_DATA, 0x9999, 32); /* no row: no effect */
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4701, 200));
  ASSERT_EQ(nr_sense_unit_snapshot_at(0, now, b, 8), 1);
  EXPECT_NEAR(b[0].wb_rate_hz / a[0].wb_rate_hz, 33.f, 0.5f); /* 1 + 32 messages */
  EXPECT_EQ(b[0].skips, 2u); /* still one skip per gated message */
}

TEST_F(Sched, WbgRateIsMeasuredAndPublished)
{
  nr_sense_sched_step(tb(DEC)); /* primes the counters */
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_NEAR(s.rate_hz, 0.25f, 1e-3); /* unmeasured: one tap per interval, never unlimited */
  nr_sense_wbg_set_rate(0, 1000.f);
  for (int i = 0; i < 400; i++) { nr_sense_wbg_admit(217, tb(DEC) + 1); nr_sense_wbg_cost(50000); }
  nr_sense_sched_step(tb(2 * DEC)); /* 400 offered over 4 s = 100 Hz, 50 us per tap */
  double off = 0, cost = 0;
  nr_sense_sched_wbg_info(0, &off, &cost);
  EXPECT_NEAR(off, 100.0, 1.0);
  EXPECT_NEAR(cost, 50000.0, 1.0);
  nr_sense_wbg_stats(0, &s);
  EXPECT_NEAR(s.rate_hz, 125.f, 1.5f); /* the pool is idle: the whole offered rate x the 1.25 margin, finite */
  g_cap[NR_RES_POOL] = {100.f, 0.f}; /* a pool far too small for 100 Hz x 50 us per bucket */
  for (int i = 0; i < 400; i++) { nr_sense_wbg_admit(217, tb(2 * DEC) + 2); nr_sense_wbg_cost(50000); }
  nr_sense_sched_step(tb(3 * DEC));
  nr_sense_wbg_stats(0, &s);
  EXPECT_LT(s.rate_hz, 0.1f); /* cut to what the pool leaves (60 of 250000 per bucket) */
  int ok = 0;
  for (int i = 0; i < 100; i++) ok += nr_sense_wbg_admit(150, tb(3 * DEC) + 1) ? 1 : 0;
  EXPECT_LE(ok, 6);
}

TEST_F(Sched, WbgCellResetClearsTheMeasurement)
{
  nr_sense_sched_step(tb(DEC));
  for (int i = 0; i < 400; i++) { nr_sense_wbg_admit(217, tb(DEC) + 1); nr_sense_wbg_cost(50000); }
  nr_sense_sched_step(tb(2 * DEC));
  double off = 0, cost = 0;
  nr_sense_sched_wbg_info(0, &off, &cost);
  ASSERT_GT(cost, 0.0);
  nr_sense_unit_reset(0); /* what a cell resync does: counters back to 0 and the scheduler told */
  nr_sense_sched_step(tb(3 * DEC));
  nr_sense_sched_wbg_info(0, &off, &cost);
  EXPECT_EQ(cost, 0.0); /* the stale cost per tap is gone */
  EXPECT_EQ(off, 0.0);
}

TEST_F(Sched, WbgCounterDecreaseIsNotAHugeCost)
{
  nr_sense_sched_step(tb(DEC));
  for (int i = 0; i < 400; i++) { nr_sense_wbg_admit(217, tb(DEC) + 1); nr_sense_wbg_cost(50000); }
  nr_sense_sched_step(tb(2 * DEC));
  nr_sense_unit_reset(0); /* the counters restart from 0 under the scheduler */
  for (int i = 0; i < 10; i++) { nr_sense_wbg_admit(217, tb(2 * DEC) + 2); nr_sense_wbg_cost(50000); }
  nr_sense_sched_step(tb(3 * DEC));
  double off = 0, cost = 0;
  nr_sense_sched_wbg_info(0, &off, &cost);
  EXPECT_LT(cost, 1e6); /* not ~1e19 from an unsigned underflow */
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_GT(s.rate_hz, 0.1f); /* and the rate did not collapse */
}

TEST_F(Sched, WbgMeasuredCostIsUsedFromTheFirstTapWithNoUnitModel)
{
  /* no pdsch_dmrs unit exists in this cell: the first decision after the first taps must already publish a measured rate, not stay at 0.25 Hz */
  nr_sense_wbg_stats_t s;
  nr_sense_wbg_stats(0, &s);
  EXPECT_NEAR(s.rate_hz, 0.25f, 1e-3);
  int ok = 0;
  for (int i = 0; i < 40; i++) ok += nr_sense_wbg_admit(217, tb(0) + 1) ? 1 : 0; /* 40 offered, 4 admitted */
  for (int i = 0; i < ok; i++) nr_sense_wbg_cost(50000);
  nr_sense_sched_step(tb(DEC)); /* the first decision */
  double off = 0, cost = 0;
  nr_sense_sched_wbg_info(0, &off, &cost);
  EXPECT_NEAR(off, 10.0, 0.5);       /* 40 offered over the first period */
  EXPECT_NEAR(cost, 50000.0, 1.0);   /* one window of 4 taps is enough */
  nr_sense_wbg_stats(0, &s);
  EXPECT_NEAR(s.rate_hz, 12.5f, 0.6f); /* 1.25 x 10 Hz: measured, not the startup rate */
}

/* Single PCI (operator 2026-11-06): exactly one active cell at the decision = no admission; the switch follows the cell count at the next decision, both ways. */
TEST_F(Sched, SingleCellBypassFollowsTheCellCountBothWays)
{
  auto wbg_ok = [](int n) { int ok = 0; for (int i = 0; i < n; i++) ok += nr_sense_wbg_admit(40, tb(1) + i) ? 1 : 0; return ok; };
  EXPECT_FALSE(nr_sense_single());
  EXPECT_FALSE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x4601)); /* no row: disabled */
  g_ncells = 1;
  nr_sense_sched_step(tb(DEC)); /* primes */
  nr_sense_sched_step(tb(2 * DEC));
  EXPECT_TRUE(nr_sense_single());
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x4601)); /* open without a row, no admission */
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PUSCH_DMRS, 0x4601));
  EXPECT_TRUE(nr_sense_unit_gate(NR_UNIT_PDSCH_DATA, 0x4602, 217));
  EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DMRS, 0x4602, 217)); /* DM-RS: the grant-level path takes it */
  EXPECT_EQ(wbg_ok(500), 500); /* any width, no bucket */
  nr_sense_wbg_stats_t s; nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.offered, 500u); EXPECT_EQ(s.admitted, 500u);
  g_ncells = 2; /* a second cell appears: next decision restores the scheduler */
  EXPECT_TRUE(nr_sense_single()); /* not before the decision */
  nr_sense_sched_step(tb(3 * DEC));
  EXPECT_FALSE(nr_sense_single());
  EXPECT_FALSE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x4601));
  int ok = wbg_ok(500);
  EXPECT_LE(ok, 6); /* finite bucket again, and sense_min_prb (128) is back: 40 PRBs refused */
  EXPECT_FALSE(nr_sense_wbg_admit(40, tb(1)));
  g_ncells = 1; /* and removed again */
  nr_sense_sched_step(tb(4 * DEC));
  EXPECT_TRUE(nr_sense_single());
  nr_isac_mode_set(NR_ISAC_MODE_TRAFFIC);
  EXPECT_FALSE(nr_sense_single()); /* traffic mode is never affected */
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DMRS, 1)); /* gates open outside sensing as before */
  EXPECT_FALSE(nr_sense_wbg_admit(217, tb(1)));
}

TEST_F(Sched, RegisteredButUnsyncedSecondCellKeepsSingleModeAndItSwitchesWhenItSyncs)
{
  g_ncells = 2; g_pci1 = -1;
  nr_sense_sched_step(tb(DEC)); nr_sense_sched_step(tb(2 * DEC));
  EXPECT_TRUE(nr_sense_single()); /* two registered cells, one running */
  g_pci1 = 923; /* the second cell syncs */
  EXPECT_TRUE(nr_sense_single()); /* not before the decision */
  nr_sense_sched_step(tb(3 * DEC));
  EXPECT_FALSE(nr_sense_single());
  g_pci1 = -1; /* a cell without a PCI (never synced) */
  nr_sense_sched_step(tb(4 * DEC));
  EXPECT_TRUE(nr_sense_single());
  g_pci1 = 923; g_ncells = 1; /* zero running cells is not single */
  nr_sense_sched_step(tb(5 * DEC));
  EXPECT_TRUE(nr_sense_single());
}

/* The old decide()/apply() assertions with ONE cell: the scheduler still decides and applies (views), while the gates are bypassed. */
TEST_F(Sched, OneCellDecidePathStillEnablesAndRemovesUnitsInTheViews)
{
  g_ncells = 1;
  auto enabled = [&](uint16_t rnti) {
    nr_sense_unit_view_t v[16]; const int n = snap_now(v, 16);
    for (int i = 0; i < n; i++) if (v[i].rnti == rnti && v[i].src == NR_UNIT_PDSCH_DATA) return v[i].enabled;
    return false;
  };
  feed_skips(0x4609, NR_UNIT_PDSCH_DATA, 0, 4 * DEC);
  nr_sense_sched_step(tb(DEC));
  EXPECT_FALSE(enabled(0x4609));
  nr_sense_sched_step(tb(2 * DEC));
  EXPECT_TRUE(enabled(0x4609));
  EXPECT_TRUE(nr_sense_single());
  EXPECT_EQ(stats().enables, 1u);
  nr_sense_unit_violation(NR_UNIT_PDSCH_DATA, 0x4609); /* the gate is open in single mode, the violation books on the enabled unit */
  nr_sense_sched_step(tb(3 * DEC));
  EXPECT_FALSE(enabled(0x4609));
  EXPECT_EQ(stats().disables, 1u);
  EXPECT_TRUE(nr_sense_unit_enabled(NR_UNIT_PDSCH_DATA, 0x4609)); /* ... while the gate itself stays open */
}

TEST_F(Sched, SingleModeDoesNotInflateSkipsAndRefundsKeepTheIdentity)
{
  g_ncells = 1;
  nr_sense_sched_step(tb(DEC)); nr_sense_sched_step(tb(2 * DEC));
  ASSERT_TRUE(nr_sense_single());
  nr_sense_unit_cell_stats_t cs0, cs1;
  nr_sense_unit_cell_stats(0, &cs0);
  for (int i = 0; i < 50; i++) EXPECT_FALSE(nr_sense_unit_gate(NR_UNIT_PDSCH_DMRS, 0x4700, 217));
  nr_sense_unit_cell_stats(0, &cs1);
  EXPECT_EQ(cs1.skips, cs0.skips); /* the closed DM-RS gate books nothing */
  for (int i = 0; i < 10; i++) { EXPECT_TRUE(nr_sense_wbg_admit(217, tb(1))); if (i < 3) nr_sense_wbg_refund(); else nr_sense_wbg_cost(1000); }
  nr_sense_wbg_stats_t s; nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.admitted - s.refunded, s.taps);
  g_ncells = 2; nr_sense_sched_step(tb(3 * DEC)); /* back to the scheduler: the refunds did not stock the bucket */
  int ok = 0; for (int i = 0; i < 100; i++) ok += nr_sense_wbg_admit(217, tb(3 * DEC) + 5) ? 1 : 0;
  EXPECT_LE(ok, 5);
  nr_sense_wbg_stats(0, &s);
  EXPECT_EQ(s.admitted - s.refunded, s.taps + (uint64_t)ok); /* multi mode: the same identity once the tokens are spent */
}
