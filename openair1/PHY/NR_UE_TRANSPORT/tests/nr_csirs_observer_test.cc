#include <gtest/gtest.h>
extern "C" {
#include "nr_csirs_observer.h"
#include "nr_passive_metrics.h"
}

static nr_csirs_resource_t resource(unsigned period = 20) {
  nr_csirs_resource_t r{};
  r.row = 2; r.ports = 1; r.density = 2; r.period = period; r.offset = 3;
  return r;
}

TEST(CsirsObserver, CounterPlumbing) {
  nr_csirs_observer_t o{};
  auto r = resource(); r.symb_l0 = 7;
  nr_csirs_observer_candidate(&o, &r, 3);
  nr_csirs_observer_candidate(&o, &r, 23);
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &r, 43));
  nr_csirs_observer_add_time(&o, NR_CSIRS_TIME_SEARCH, 17);
  nr_csirs_observer_add_time(&o, NR_CSIRS_TIME_IDSWEEP, 5);
  nr_csirs_observer_add_time(&o, NR_CSIRS_TIME_CONFIRM, 7);
  nr_csirs_observer_add_time(&o, NR_CSIRS_TIME_CFR, 11);
  nr_passive_metrics_t m{};
  nr_csirs_observer_metrics(&o, &m);
  EXPECT_EQ(m.csirs_candidates, 2u);
  EXPECT_EQ(m.csirs_confirmed, 1u);
  EXPECT_EQ(m.csirs_search_us, 17u);
  EXPECT_EQ(m.csirs_idsweep_us, 5u);
  EXPECT_EQ(m.csirs_confirm_us, 7u);
  EXPECT_EQ(m.csirs_cfr_us, 11u);
  EXPECT_EQ(m.csirs_time_to_confirm_slots_last, 40u);
  char json[8192]; /* Combined levers + reconfiguration metrics, as in the emitter. */
  ASSERT_GT(nr_passive_metrics_to_json(&m, json, sizeof(json)), 0);
  EXPECT_NE(std::string(json).find("\"csirs_confirmed\":1"), std::string::npos);
  EXPECT_NE(std::string(json).find("\"csirs_time_to_confirm_slots_last\":40"), std::string::npos);
  EXPECT_NE(std::string(json).find("\"period\":20,\"offset\":3"), std::string::npos);
  EXPECT_NE(std::string(json).find("\"symb_l0\":7"), std::string::npos);
  EXPECT_NE(std::string(json).find("\"slots\":40"), std::string::npos);
}

TEST(CsirsObserver, ZpCounterPlumbing) {
  nr_csirs_observer_t o{};
  auto r = resource(); r.zp = 1;
  nr_csirs_observer_confirm(&o, &r, 43);
  nr_csirs_observer_export_zp(&o);
  nr_csirs_observer_export_zp(&o);
  nr_csirs_observer_revoke_zp(&o, &r);
  nr_passive_metrics_t m{};
  nr_csirs_observer_metrics(&o, &m);
  EXPECT_EQ(m.csirs_confirmed, 1u);
  EXPECT_EQ(m.csirs_revoked, 1u);
  EXPECT_EQ(m.zp_exported, 2u);
  EXPECT_EQ(m.zp_revoked, 1u);
}

TEST(CsirsObserver, CsirsResourceGoneIsSoft) {
  nr_csirs_observer_t o{};
  auto r = resource();
  nr_csirs_observer_confirm(&o, &r, 43);
  for (int i = 1; i < 4; ++i)
    EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 43 + 20 * i, false));
  EXPECT_TRUE(nr_csirs_observer_due(&o, &r, 123, false));
  EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 143, false));
}

TEST(CsirsObserver, SingleMissedConfirmationNoBump) {
  nr_csirs_observer_t o{};
  auto r = resource();
  nr_csirs_observer_confirm(&o, &r, 43);
  EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 63, false));
  EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 83, true));
  for (int i = 1; i < 4; ++i)
    EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 83 + 20 * i, false));
  EXPECT_TRUE(nr_csirs_observer_due(&o, &r, 163, false));
}

TEST(CsirsObserver, NewResourceDifferentPeriodicityIsSoft) {
  nr_csirs_observer_t o{};
  auto old = resource(), changed = resource(40);
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &old, 43));
  for (int i = 1; i <= 4; ++i)
    nr_csirs_observer_due(&o, &old, 43 + 20 * i, false);
  EXPECT_TRUE(nr_csirs_observer_confirm(&o, &changed, 163));
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &changed, 163));
}

TEST(CsirsObserver, TwoCoexistingResourcesNoBump) {
  nr_csirs_observer_t o{};
  auto a = resource(), b = resource(320);
  b.offset = 17;
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &a, 43));
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &b, 6400));
  EXPECT_TRUE(o.entry[0].active);
  EXPECT_TRUE(o.entry[1].active);
  EXPECT_EQ(o.csirs_revoked, 0u);
}

TEST(CsirsObserver, TrsPairDifferentOffsetsBothTracked) {
  nr_csirs_observer_t o{};
  auto a = resource(), b = a;
  a.row = b.row = 1;
  a.symb_l0 = b.symb_l0 = 4;
  b.offset = 8;
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &a, 4));
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &b, 8));
  EXPECT_TRUE(o.entry[0].active);
  EXPECT_TRUE(o.entry[1].active);
  EXPECT_EQ(o.csirs_revoked, 0u);
}

TEST(CsirsObserver, SameOffsetDifferentSymbolsAreDistinct) {
  nr_csirs_observer_t o{};
  auto a = resource(), b = a;
  a.symb_l0 = 4;
  b.symb_l0 = 8;
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &a, 43));
  EXPECT_FALSE(nr_csirs_observer_confirm(&o, &b, 43));
  EXPECT_TRUE(o.entry[0].active);
  EXPECT_TRUE(o.entry[1].active);
  EXPECT_EQ(o.csirs_confirmed, 2u);
}

TEST(CsirsObserver, RevokingOneZpKeepsCoexistingResource) {
  nr_csirs_observer_t o{};
  auto a = resource(), b = a;
  a.zp = b.zp = 1;
  b.offset = 8;
  nr_csirs_observer_confirm(&o, &a, 43);
  nr_csirs_observer_confirm(&o, &b, 48);
  nr_csirs_observer_revoke_zp(&o, &b);
  EXPECT_TRUE(o.entry[0].active);
  EXPECT_FALSE(o.entry[1].active);
  EXPECT_EQ(o.zp_revoked, 1u);
}

TEST(CsirsObserver, NearThresholdNoFlap) {
  nr_csirs_observer_t o{};
  auto r = resource();
  nr_csirs_observer_confirm(&o, &r, 43);
  for (int i = 1; i <= 8; ++i)
    EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 43 + i * 20, i % 2 != 0));
  EXPECT_TRUE(o.entry[0].active);
}

/* Periodic tracker wiring (NR_PTRACK_CSIRS): the observer feeds every scored occasion of a confirmed resource. */
static nr_ptrack_counters_t periodic(const nr_csirs_observer_t &o) {
  nr_passive_metrics_t m{};
  nr_csirs_observer_metrics(&o, &m);
  return m.periodic[NR_PTRACK_CSIRS];
}
TEST(CsirsObserverPtrack, SteadyPeriodConfirmsAndCountsPredictedHits) {
  nr_csirs_observer_t o{};
  auto r = resource(20);
  nr_csirs_observer_confirm(&o, &r, 3);
  EXPECT_EQ(nr_csirs_observer_predicted_next(&o, &r, 3), 0u); // nothing learned yet
  for (int i = 1; i <= 10; ++i) EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 3 + 20 * i, true));
  auto c = periodic(o);
  EXPECT_EQ(c.confirmed, 1u);
  EXPECT_EQ(c.predicted, 6u);   // 4 detections confirm; occasions 5..10 are predicted
  EXPECT_EQ(c.hit, c.predicted);
  EXPECT_EQ(c.miss, 0u);
  EXPECT_EQ(nr_csirs_observer_predicted_next(&o, &r, 3 + 20 * 10), 3u + 20 * 11);
  EXPECT_TRUE(nr_csirs_observer_predicted_due(&o, &r, 3 + 20 * 11));
  EXPECT_FALSE(nr_csirs_observer_predicted_due(&o, &r, 3 + 20 * 11 + 1));
}
TEST(CsirsObserverPtrack, MissedOccasionsAreCountedAndDemoteAfterFour) {
  nr_csirs_observer_t o{};
  auto r = resource(40);
  nr_csirs_observer_confirm(&o, &r, 3);
  for (int i = 1; i <= 6; ++i) nr_csirs_observer_due(&o, &r, 3 + 40 * i, true);
  ASSERT_EQ(periodic(o).confirmed, 1u);
  nr_csirs_observer_due(&o, &r, 3 + 40 * 7, false);
  nr_csirs_observer_due(&o, &r, 3 + 40 * 8, true);
  EXPECT_EQ(periodic(o).miss, 1u);
  EXPECT_EQ(periodic(o).demoted, 0u);
  for (int i = 9; i <= 12; ++i) nr_csirs_observer_due(&o, &r, 3 + 40 * i, false);
  auto c = periodic(o);
  EXPECT_EQ(c.miss, 5u);
  EXPECT_EQ(c.demoted, 1u);
  EXPECT_EQ(nr_csirs_observer_predicted_next(&o, &r, 3 + 40 * 12), 0u);
  EXPECT_EQ(o.csirs_revoked, 1u); // the observer's own four-miss rule is unchanged
}
TEST(CsirsObserverPtrack, TrsBurstTwoSlotsPerPeriod) {
  nr_csirs_observer_t o{};
  auto a = resource(80), b = a;
  a.n_offsets = b.n_offsets = 2; a.offset = 7; a.offset2 = 8; b.offset = 8; b.offset2 = 7; // both slots of one TRS burst
  nr_csirs_observer_confirm(&o, &a, 7);
  for (int i = 1; i <= 8; ++i) {
    nr_csirs_observer_due(&o, &a, 7 + 80 * i, true);
    nr_csirs_observer_due(&o, &a, 8 + 80 * i, true);
  }
  auto c = periodic(o);
  EXPECT_EQ(c.confirmed, 1u);
  EXPECT_EQ(c.demoted, 0u);
  EXPECT_EQ(c.miss, 0u);
  EXPECT_GE(c.predicted, 4u);
  EXPECT_EQ(nr_csirs_observer_predicted_next(&o, &a, 7 + 80 * 8 + 1), 7u + 80 * 9);
}
TEST(CsirsObserverPtrack, StrayDetectionOffGridIsAuditNotPrediction) {
  nr_csirs_observer_t o{};
  auto r = resource(20);
  nr_csirs_observer_confirm(&o, &r, 3);
  for (int i = 1; i <= 8; ++i) nr_csirs_observer_due(&o, &r, 3 + 20 * i, true);
  const auto before = periodic(o);
  nr_csirs_observer_due(&o, &r, 3 + 20 * 8 + 7, true); // scored at an off-grid slot (stray)
  const auto after = periodic(o);
  EXPECT_EQ(after.predicted, before.predicted);
  EXPECT_EQ(after.audit_hit, before.audit_hit + 1);
  EXPECT_EQ(nr_csirs_observer_predicted_next(&o, &r, 3 + 20 * 8 + 7), 3u + 20 * 9);
}
TEST(CsirsObserverPtrack, PredictionNeverSuppressesDetection) {
  /* A resource whose tracker is demoted (or never confirmed) is still scored and counted by the observer: the tracker is advisory. */
  nr_csirs_observer_t o{};
  auto r = resource(20);
  nr_csirs_observer_confirm(&o, &r, 3);
  EXPECT_FALSE(nr_csirs_observer_predicted_due(&o, &r, 23));
  EXPECT_FALSE(nr_csirs_observer_due(&o, &r, 23, true));
  EXPECT_EQ(o.entry[0].misses, 0u);
  EXPECT_EQ(periodic(o).predicted, 0u);
  nr_csirs_resource_t unknown = resource(40);
  unknown.start_rb = 99;
  EXPECT_FALSE(nr_csirs_observer_predicted_due(&o, &unknown, 23));
  EXPECT_EQ(nr_csirs_observer_predicted_next(&o, &unknown, 23), 0u);
}
