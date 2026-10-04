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
