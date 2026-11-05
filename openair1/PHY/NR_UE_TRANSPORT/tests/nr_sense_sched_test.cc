#include "nr_sense_sched.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

static const uint64_t S = 1000000000ull;
static const uint64_t NOW = 1000 * S;

/* a synthetic one-or-more-cell scheduler harness; apply() plays the role of S7 (copies the decision into the views) */
struct H {
  std::vector<nr_sense_unit_view_t> v[3];
  nr_sense_sched_in_t in;
  nr_sense_sched_state_t st;
  nr_sense_sched_out_t out;
  int ncell;
  explicit H(int n = 1, float pool = 100) : ncell(n)
  {
    memset(&in, 0, sizeof in);
    memset(&st, 0, sizeof st);
    in.n_cells = n;
    in.min_prb = 128;
    in.now_ns = NOW;
    in.res[NR_RES_POOL].cap_ns = pool;
    in.res[NR_RES_QUEUE_DL].cap_ns = in.res[NR_RES_QUEUE_UL].cap_ns = in.res[NR_RES_SENSE_RING].cap_ns = 1e9f; /* GPU absent (0) unless a test sets it */
  }
  /* defaults: disabled, with cost history cpu, productive (wb 10 Hz, ratio 0.8, 200 PRBs), valid just now */
  nr_sense_unit_view_t &add(int c, uint16_t rnti, float cpu = 25, nr_unit_src_t s = NR_UNIT_PDSCH_DMRS)
  {
    nr_sense_unit_view_t u;
    memset(&u, 0, sizeof u);
    u.rnti = rnti; u.src = s; u.cpu_p99_ns = cpu; u.cpu_p95_ns = cpu; u.cpu_max_ns = cpu;
    u.n_cfr = 20; u.last_cfr_ns = NOW - 1; u.rate_hz = 10; u.wb_rate_hz = 10; u.valid_ratio = 0.8f; u.snr_db = 10; u.est_usable_prbs = 200;
    u.last_valid_wb_ns = NOW - 1; u.last_msg_ns = NOW - 1; u.first_ns = NOW - 100 * S; u.gen = 1;
    v[c].push_back(u);
    return v[c].back();
  }
  static float val(const nr_sense_unit_view_t &u) { return u.wb_rate_hz * u.valid_ratio * u.est_usable_prbs; }
  nr_sense_unit_view_t *find(int c, uint16_t rnti, nr_unit_src_t s = NR_UNIT_PDSCH_DMRS)
  {
    for (auto &u : v[c]) if (u.rnti == rnti && u.src == s) return &u;
    return nullptr;
  }
  void step()
  {
    for (int c = 0; c < ncell; c++) { in.units[c] = v[c].data(); in.n_units[c] = (int)v[c].size(); in.pci[c] = 100 + c; }
    nr_sense_sched_decide(&in, &st, &out);
    for (int c = 0; c < ncell; c++)
      for (auto &u : v[c])
        for (int sl = 0; sl < NR_SENSE_SLOTS; sl++)
          if ((u.rnti == 0 ? sl == 0 : st.rnti[c][sl] == u.rnti)) u.enabled = st.enabled[c][sl][u.src];
  }
  void steps(int n) { for (int i = 0; i < n; i++) step(); }
  int nen(int c) { int n = 0; for (auto &u : v[c]) n += u.enabled; return n; }
  bool en(int c, uint16_t r, nr_unit_src_t s = NR_UNIT_PDSCH_DMRS) { return find(c, r, s)->enabled; }
};

TEST(Sched, BandwidthFirst)
{
  H h; h.add(0, 1, 40).wb_rate_hz = 10; h.add(0, 2, 40).est_usable_prbs = 260; /* equal cost, 2 has the higher wb_rate x prbs */
  h.steps(3);
  EXPECT_TRUE(h.en(0, 2)); EXPECT_FALSE(h.en(0, 1));
}

TEST(Sched, QualityTieBreak)
{
  H h; h.add(0, 1, 40).snr_db = 5; h.add(0, 2, 40).snr_db = 15; /* equal value, 2 has the better SNR */
  h.steps(3);
  EXPECT_TRUE(h.en(0, 2)); EXPECT_FALSE(h.en(0, 1));
}

TEST(Sched, AgeBoostBoundedAt2x)
{
  EXPECT_DOUBLE_EQ(nr_sense_age_boost(true, NOW - 30 * S, NOW), 2.0);
  EXPECT_DOUBLE_EQ(nr_sense_age_boost(true, NOW - 10 * S, NOW), 2.0);
  EXPECT_DOUBLE_EQ(nr_sense_age_boost(true, NOW - 5 * S, NOW), 1.5);
  EXPECT_DOUBLE_EQ(nr_sense_age_boost(true, NOW, NOW), 1.0);
}

TEST(Sched, IdleUnitGetsNoBoostAndAges0)
{
  EXPECT_DOUBLE_EQ(nr_sense_age_boost(false, NOW - 30 * S, NOW), 1.0);
  H h; auto &u = h.add(0, 1, 10); u.wb_rate_hz = 0; u.rate_hz = 0; u.last_valid_wb_ns = NOW - 60 * S;
  EXPECT_FALSE(nr_sense_unit_eligible(&u, NOW));
  h.steps(4);
  EXPECT_FALSE(h.en(0, 1)); /* plenty of capacity, still not admitted */
}

TEST(Sched, IdleEnabledUnitIsDisabledAndTakesNoCapacity)
{
  H h; auto &a = h.add(0, 1, 40); a.enabled = true; a.wb_rate_hz = 0; /* UE gone idle */
  h.add(0, 2, 40);
  h.steps(3);
  EXPECT_FALSE(h.en(0, 1)); EXPECT_TRUE(h.en(0, 2));
}

TEST(Sched, IneligiblePciTakesNoCapacity)
{
  H h(2);
  h.add(0, 1, 25).wb_rate_hz = 0; h.find(0, 1)->est_usable_prbs = 50; /* only 50-PRB traffic */
  h.add(0, 2, 25).wb_rate_hz = 0; h.find(0, 2)->est_usable_prbs = 50;
  for (uint16_t r = 11; r < 14; r++) h.add(1, r, 25);
  h.steps(3);
  EXPECT_EQ(h.nen(0), 0);
  EXPECT_EQ(h.nen(1), 2); /* budget 60 / 25: the other PCI gets all the capacity */
}

TEST(Sched, NoOneUnitPerPciRule)
{
  H h(2, 100);
  h.add(0, 1, 50).valid_ratio = 0.3f;  h.find(0, 1)->last_valid_wb_ns = NOW - 1;
  h.add(1, 2, 50).valid_ratio = 0.9f;
  h.steps(3);
  EXPECT_TRUE(h.en(1, 2)); EXPECT_FALSE(h.en(0, 1)); /* B much better, capacity for one: B only */
  /* A's age (30 s) outweighs a 1.8x better B: the combined boost is x2 */
  H g(2, 100);
  auto &a = g.add(0, 1, 50); a.valid_ratio = 0.5f; a.last_valid_wb_ns = NOW - 30 * S;
  g.st.last_pci_valid_wb_ns[0] = NOW - 30 * S;
  g.add(1, 2, 50).valid_ratio = 0.9f;
  g.steps(3);
  EXPECT_TRUE(g.en(0, 1)); EXPECT_FALSE(g.en(1, 2));
}

TEST(Sched, AdmissionStopsAtHeadroom)
{
  H h(1, 100);
  for (uint16_t r = 1; r <= 4; r++) h.add(0, r, 25);
  h.steps(4);
  EXPECT_EQ(h.nen(0), 2); /* 2 x 25 <= 0.6 x 100 < 3 x 25 */
  EXPECT_EQ(h.out.binding, NR_RES_POOL);
}

TEST(Sched, PerResourceBinding)
{
  H h(1, 1000);
  h.in.res[NR_RES_GPU].cap_ns = 100;
  for (uint16_t r = 1; r <= 4; r++) h.add(0, r, 25, NR_UNIT_PDSCH_DATA);
  h.steps(4);
  EXPECT_EQ(h.nen(0), 2);
  EXPECT_EQ(h.out.binding, NR_RES_GPU);
  H g(1, 100); /* the same GPU, but DM-RS units do not use it: the pool binds */
  g.in.res[NR_RES_GPU].cap_ns = 1;
  for (uint16_t r = 1; r <= 4; r++) g.add(0, r, 25, NR_UNIT_PDSCH_DMRS);
  g.steps(4);
  EXPECT_EQ(g.nen(0), 2);
  EXPECT_EQ(g.out.binding, NR_RES_POOL);
}

TEST(Sched, QueueWaitIsMaxNotSum)
{
  H h(1, 1000);
  h.in.res[NR_RES_QUEUE_DL].cap_ns = 40; /* budget 24 */
  for (uint16_t r = 1; r <= 3; r++) h.add(0, r, 5, NR_UNIT_PDSCH_DATA).wait_p99_ns = 10;
  h.add(0, 4, 5, NR_UNIT_PDSCH_DATA).wait_p99_ns = 30; /* alone above the budget */
  h.steps(3);
  EXPECT_EQ(h.nen(0), 3); EXPECT_FALSE(h.en(0, 4, NR_UNIT_PDSCH_DATA));
  EXPECT_EQ(h.out.binding, NR_RES_QUEUE_DL);
}

TEST(Sched, HysteresisHoldsWithin20Pct)
{
  H h(1, 100);
  auto &a = h.add(0, 1, 40); a.enabled = true;       /* incumbent, value V */
  auto &b = h.add(0, 2, 40); b.est_usable_prbs = 220; /* challenger 10 % better */
  h.steps(4);
  EXPECT_TRUE(h.en(0, 1)); EXPECT_FALSE(h.en(0, 2)); /* within the margin: no swap */
  h.find(0, 2)->est_usable_prbs = 300;               /* 50 % better */
  h.steps(1);
  EXPECT_TRUE(h.en(0, 1)); /* streak */
  h.steps(1);
  EXPECT_FALSE(h.en(0, 1)); EXPECT_TRUE(h.en(0, 2));
}

TEST(Sched, StreakNeedsTwoDecisions)
{
  H h; h.add(0, 1, 10);
  h.step(); EXPECT_FALSE(h.en(0, 1)); EXPECT_EQ(h.out.enables, 0);
  h.step(); EXPECT_TRUE(h.en(0, 1)); EXPECT_EQ(h.out.enables, 1);
  /* disable also needs two */
  h.find(0, 1)->wb_rate_hz = 0;
  h.step(); EXPECT_TRUE(h.en(0, 1));
  h.step(); EXPECT_FALSE(h.en(0, 1)); EXPECT_EQ(h.out.disables, 1);
  /* an interrupted streak restarts */
  h.find(0, 1)->wb_rate_hz = 10;
  h.step(); h.find(0, 1)->wb_rate_hz = 0; h.step(); h.find(0, 1)->wb_rate_hz = 10; h.step();
  EXPECT_FALSE(h.en(0, 1));
}

TEST(Sched, MaxTwoEnables)
{
  H h(1, 1000);
  for (uint16_t r = 1; r <= 5; r++) h.add(0, r, 5);
  h.step();
  h.step(); EXPECT_EQ(h.out.enables, 2); EXPECT_EQ(h.nen(0), 2);
  h.step(); EXPECT_LE(h.out.enables, 2); EXPECT_EQ(h.nen(0), 4);
  h.step(); EXPECT_EQ(h.nen(0), 5);
}

TEST(Sched, ViolationRemovesLowestImmediately)
{
  H h(1, 1000);
  for (uint16_t r = 1; r <= 3; r++) { auto &u = h.add(0, r, 5); u.enabled = true; u.est_usable_prbs = (uint16_t)(100 + 100 * r); }
  h.find(0, 2)->violations = 1; /* a drop in the middle unit */
  h.step();                      /* no streak */
  EXPECT_FALSE(h.en(0, 1)); EXPECT_TRUE(h.en(0, 2)); EXPECT_TRUE(h.en(0, 3));
  EXPECT_EQ(h.out.disables, 1);
  h.step(); /* the same cumulative counter is not a new violation */
  EXPECT_TRUE(h.en(0, 2)); EXPECT_TRUE(h.en(0, 3));
  h.find(0, 3)->violations = 2; h.find(0, 3)->gen = 2; /* row recreated: counters restart, 2 new violations */
  h.step();
  EXPECT_FALSE(h.en(0, 2)); EXPECT_TRUE(h.en(0, 3));
}

TEST(Sched, ViolationGroupIsTheQueue)
{
  H h(1, 1000);
  auto &d = h.add(0, 1, 5, NR_UNIT_PDSCH_DATA); d.enabled = true; d.est_usable_prbs = 500; d.violations = 1;
  auto &m = h.add(0, 2, 5, NR_UNIT_PDSCH_DMRS); m.enabled = true; m.est_usable_prbs = 129; /* lowest value, other group */
  auto &e = h.add(0, 3, 5, NR_UNIT_PDSCH_DATA); e.enabled = true; e.est_usable_prbs = 300;
  h.step();
  EXPECT_TRUE(h.en(0, 2)); EXPECT_FALSE(h.en(0, 3, NR_UNIT_PDSCH_DATA)); EXPECT_TRUE(h.en(0, 1, NR_UNIT_PDSCH_DATA));
}

TEST(Sched, ColdStartOneUnitPerDecision)
{
  H h(1, 1000);
  for (uint16_t r = 1; r <= 3; r++) h.add(0, r, 0); /* no history */
  int total = 0;
  for (int i = 0; i < 6; i++) { h.step(); EXPECT_LE(h.out.enables, 1); total += h.out.enables; }
  EXPECT_EQ(total, 3);
  EXPECT_EQ(h.nen(0), 3);
  /* the source's per-cell p99 is the estimate: 30 (warm, enabled) + 30 (cold) > 0.6 x 50 */
  H g(1, 50);
  g.add(0, 1, 30).enabled = true; g.add(0, 2, 0);
  g.steps(4);
  EXPECT_FALSE(g.en(0, 2)); EXPECT_TRUE(g.en(0, 1));
}

TEST(Sched, SinglePciWorks)
{
  H h(1, 100);
  h.add(0, 0, 25, NR_UNIT_SRS); h.add(0, 7, 25);
  h.steps(3);
  EXPECT_EQ(h.nen(0), 2);
}

TEST(Sched, NarrowCarrierEnablesNothing)
{
  H h(2, 1000);
  for (int c = 0; c < 2; c++)
    for (uint16_t r = 1; r <= 3; r++) { auto &u = h.add(c, r, 5); u.est_usable_prbs = 100; u.wb_rate_hz = 0; }
  h.steps(5);
  EXPECT_EQ(h.nen(0) + h.nen(1), 0); /* narrow traffic: S5 gives wb_rate_hz 0 */
}

TEST(Sched, StateSlotsAreReleased)
{
  H h(1, 1000); h.add(0, 5, 5);
  h.steps(2); EXPECT_TRUE(h.en(0, 5));
  h.v[0].clear(); h.step(); /* row evicted */
  h.add(0, 5, 5); h.step(); /* comes back disabled-looking: no stale streak or enabled bit */
  EXPECT_FALSE(h.en(0, 5)); EXPECT_EQ(h.out.enables, 0);
}

TEST(Sched, CombinedBoostCappedAt2x)
{
  H g(2, 100); /* unit x PCI would be x4: a 3x better B must still win */
  auto &a = g.add(0, 1, 50); a.valid_ratio = 0.3f; a.last_valid_wb_ns = NOW - 30 * S;
  g.st.last_pci_valid_wb_ns[0] = NOW - 30 * S;
  g.add(1, 2, 50).valid_ratio = 0.9f;
  g.steps(3);
  EXPECT_TRUE(g.en(1, 2)); EXPECT_FALSE(g.en(0, 1));
}

TEST(Sched, NewUnitWithoutCfrIsNotDisabled)
{
  H h(1, 1000); auto &u = h.add(0, 1, 5); u.enabled = true; u.n_cfr = 0; u.valid_ratio = 0;
  EXPECT_TRUE(nr_sense_unit_eligible(&u, NOW));
  h.steps(6);
  EXPECT_TRUE(h.en(0, 1)); /* unknown ratio is not a bad ratio */
  h.find(0, 1)->n_cfr = 5; h.find(0, 1)->valid_ratio = 0.1f; /* CFRs below the floor */
  h.step(); EXPECT_TRUE(h.en(0, 1));
  h.step(); EXPECT_FALSE(h.en(0, 1));
}

TEST(Sched, AlternatingGrantSizesDoNotFlap)
{
  H h(1, 1000); auto &u = h.add(0, 1, 5); u.enabled = true;
  for (int i = 0; i < 8; i++) {
    h.find(0, 1)->est_usable_prbs = (i & 1) ? 50 : 200; /* the latest grant is narrow every other decision; the window wb rate stays > 0 */
    h.step();
    EXPECT_TRUE(h.en(0, 1)) << i;
  }
}

TEST(Sched, AgeRestartsAfterIneligible)
{
  H h(2, 100);
  auto &a = h.add(0, 1, 50); a.valid_ratio = 0.5f; a.last_valid_wb_ns = NOW - 60 * S;
  auto &b = h.add(1, 2, 50); b.valid_ratio = 0.9f;
  h.find(0, 1)->wb_rate_hz = 0; h.step(); /* A idle: not eligible */
  h.find(0, 1)->wb_rate_hz = 10;
  h.steps(3); /* back after a minute: its age starts now (x1), B at 1.8x wins; with the old stamp A would be x2 (> 1.8x) */
  EXPECT_TRUE(h.en(1, 2)); EXPECT_FALSE(h.en(0, 1));
}

TEST(Sched, ViolationHoldoffTwoDecisions)
{
  H h(1, 1000);
  auto &u = h.add(0, 1, 5); u.enabled = true; u.violations = 1;
  h.step(); EXPECT_FALSE(h.en(0, 1)); /* D: removed */
  h.step(); EXPECT_FALSE(h.en(0, 1)); /* D+1 */
  h.step(); EXPECT_FALSE(h.en(0, 1)); /* D+2: streak 2 reached but held off */
  h.step(); EXPECT_TRUE(h.en(0, 1));  /* D+3 */
}

TEST(Sched, BadCapIsInvalidExceptGpu)
{
  H h(1, 0); /* POOL cap 0: invalid */
  h.add(0, 1, 0.f + 1); h.add(0, 2, 0);
  h.steps(4);
  EXPECT_EQ(h.nen(0), 0); EXPECT_EQ(h.out.bad_cap, 1);
  H g(1, 100); /* GPU cap 0 is "no GPU": data units are still admitted */
  g.add(0, 1, 10, NR_UNIT_PDSCH_DATA);
  g.steps(3);
  EXPECT_EQ(g.nen(0), 1); EXPECT_EQ(g.out.bad_cap, 0);
}

TEST(Sched, StaleRatioBecomesUnknownAndUnitIsRetried)
{
  H h(1, 1000); auto &u = h.add(0, 1, 5); u.enabled = true; u.valid_ratio = 0.05f; u.last_cfr_ns = NOW - 1; /* fresh, bad CFRs */
  h.steps(2);
  EXPECT_FALSE(h.en(0, 1)); /* disabled for the low ratio; no CFR can arrive now */
  h.steps(3);
  EXPECT_FALSE(h.en(0, 1)); /* the ratio is 16 s old only after 16 s */
  h.in.now_ns = NOW + 17 * S; /* beyond one cost window without a CFR */
  EXPECT_TRUE(nr_sense_unit_eligible(h.find(0, 1), h.in.now_ns));
  h.step(); EXPECT_FALSE(h.en(0, 1)); /* streak */
  h.step(); EXPECT_TRUE(h.en(0, 1));
  /* it produces bad CFRs again: judged again, off after a streak, and not back before another window */
  h.find(0, 1)->last_cfr_ns = h.in.now_ns - 1;
  h.steps(2); EXPECT_FALSE(h.en(0, 1));
  h.steps(3); EXPECT_FALSE(h.en(0, 1));
}

TEST(Sched, UnitProducingInvalidCfrsStaysOff)
{
  H h(1, 1000); auto &u = h.add(0, 1, 5); u.enabled = true; u.valid_ratio = 0.05f;
  for (int i = 0; i < 8; i++) {
    h.in.now_ns = NOW + (uint64_t)i * 4 * S;
    h.find(0, 1)->last_cfr_ns = h.in.now_ns - 1; /* CFRs keep arriving (e.g. an external re-enable) */
    h.step();
    if (i >= 1) EXPECT_FALSE(h.en(0, 1)) << i;
  }
}

TEST(Sched, PerCellRingAndQueuesAreNotSharedAcrossCells)
{
  /* a ring without headroom in cell 0 must not stop an enable in cell 1; the pool stays shared */
  H h(2, 1000);
  h.in.cell_res_set[0] = h.in.cell_res_set[1] = true;
  for (int c = 0; c < 2; c++) {
    memcpy(h.in.cell_res[c], h.in.res, sizeof h.in.res);
    h.in.cell_res[c][NR_RES_SENSE_RING] = {1000.f, 0.f};
  }
  h.in.cell_res[0][NR_RES_SENSE_RING].base_p99_ns = 1000.f; /* cell 0's ring dropped records: no headroom */
  h.add(0, 1, 5); h.add(1, 2, 5);
  h.steps(3);
  EXPECT_FALSE(h.en(0, 1));
  EXPECT_TRUE(h.en(1, 2));
  /* the per-cell queue budget is per cell: the same waits in two cells both fit a budget one cell alone would exhaust */
  H g(2, 1e6f);
  g.in.cell_res_set[0] = g.in.cell_res_set[1] = true;
  for (int c = 0; c < 2; c++) { memcpy(g.in.cell_res[c], g.in.res, sizeof g.in.res); g.in.cell_res[c][NR_RES_QUEUE_DL] = {40.f, 0.f}; } /* budget 24 each */
  g.add(0, 1, 5, NR_UNIT_PDSCH_DATA).wait_p99_ns = 20; g.add(1, 2, 5, NR_UNIT_PDSCH_DATA).wait_p99_ns = 20;
  g.steps(3);
  EXPECT_EQ(g.nen(0) + g.nen(1), 2);
}

TEST(Sched, CostMemoryStopsAnEnableDisableCycleOnASteadyLoad)
{
  H h(1, 100); /* pool budget 60 */
  auto &u = h.add(0, 1, 80); /* costs 80 while it runs: more than the budget */
  u.enabled = true;
  h.steps(3);
  EXPECT_FALSE(h.en(0, 1)); /* shed after the streak */
  auto *p = h.find(0, 1);
  p->cpu_p99_ns = p->cpu_p95_ns = p->cpu_max_ns = 0; /* its cost has left the 16 s window */
  h.steps(2);
  EXPECT_FALSE(h.en(0, 1)); /* without the memory this cold unit (demand 0) would be enabled after the streak */
  h.steps(12);
  EXPECT_TRUE(h.en(0, 1)); /* the memory decays: it is probed again */
}

TEST(Sched, UnitThatCannotStayOnIsProbedWithBackoffNotEveryFewDecisions)
{
  H h(1, 100); /* pool budget 60; the unit costs 80 whenever it runs and nothing while off (its cost leaves the window) */
  h.add(0, 1, 0);
  int enables = 0;
  bool was = false;
  for (int d = 0; d < 80; d++) {
    auto *u = h.find(0, 1);
    u->cpu_p99_ns = u->cpu_p95_ns = u->cpu_max_ns = u->enabled ? 80.f : 0.f;
    h.step();
    const bool now = h.en(0, 1);
    if (now && !was) enables++;
    was = now;
  }
  EXPECT_GE(enables, 2);   /* it is probed again */
  EXPECT_LE(enables, 5);   /* ... with a growing gap: a fixed ~7-decision cycle would give 11 */
}
