#include <gtest/gtest.h>
#include <cstdlib>
extern "C" {
#include "nr_admit.h"
}
// Strong overrides of nr_admit.c's weak core-pool hooks: the test drives the shed level.
static int g_tiers = 0;
static uint64_t g_note[3];
extern "C" int nr_pool_site_shed_tiers(void) { return g_tiers; }
extern "C" void nr_pool_site_shed_note(int tier) { g_note[tier]++; }

// Admission control is off (no active-cells hook): every class runs, even with a deadline already passed.
TEST(Admit, AlwaysRuns)
{
  g_tiers = 0;
  EXPECT_EQ(0, nr_admit_on());
  EXPECT_EQ(1, nr_admit_try(NR_ADMIT_PROBE, 3000, 2000, 0));
  EXPECT_EQ(1, nr_admit_try(NR_ADMIT_JANON, 1000, 2000, 1));
}

TEST(Admit, AdmitTiersUnchanged)
{
  const int want[NR_ADMIT_N] = {2 /*JANON*/, -1 /*USS*/, 2 /*NID*/, 1 /*CSIRS*/, -1 /*DMRS_SCAN*/, 1 /*DMRS_ID*/, -1 /*PROBE*/, 0 /*UL_DISC*/};
  for (int c = 0; c < NR_ADMIT_N; c++) EXPECT_EQ(want[c], nr_admit_tier((nr_admit_class_t)c)) << c;
  // cumulative: shed level L skips every class with tier < L, never tier -1
  for (int lvl = 0; lvl <= 3; lvl++) {
    g_tiers = lvl;
    for (int c = 0; c < NR_ADMIT_N; c++) {
      const int t = want[c];
      EXPECT_EQ(t < 0 || lvl <= t ? 1 : 0, nr_admit_try((nr_admit_class_t)c, 0, 0, 0)) << "lvl " << lvl << " class " << c;
    }
  }
  g_tiers = 0;
}

TEST(Admit, AdmitCountsAttempts)
{
  nr_admit_reset_for_test();
  g_tiers = 0;
  nr_admit_try(NR_ADMIT_JANON, 0, 0, 0);
  g_tiers = 3;
  nr_admit_try(NR_ADMIT_JANON, 0, 0, 0);
  nr_admit_try(NR_ADMIT_JANON, 0, 0, 0);
  nr_admit_try(NR_ADMIT_USS, 0, 0, 0); // never shed, still an attempt
  g_tiers = 0;
  uint64_t a, s, r, e;
  nr_admit_class_counts(NR_ADMIT_JANON, &a, &s, &r, &e);
  EXPECT_EQ(3u, a);
  EXPECT_EQ(2u, s);
  nr_admit_class_counts(NR_ADMIT_USS, &a, &s, &r, &e);
  EXPECT_EQ(1u, a);
  EXPECT_EQ(0u, s);
}

static int g_freed;
static void free_cb(void *p) { g_freed++; free(p); }
static void *mk(int v) { int *p = (int *)malloc(sizeof(int)); *p = v; return p; }
static const uint64_t SEC = 1000000000ull;

TEST(Admit, BacklogReplaysWhenIdle)
{
  nr_admit_reset_for_test();
  nr_admit_skipq_t q = {};
  g_freed = 0;
  nr_admit_skipq_push(NR_ADMIT_JANON, &q, mk(7), 100 + SEC, free_cb);
  g_tiers = 1; // still shedding: nothing comes back
  EXPECT_EQ(nullptr, nr_admit_skipq_pop(NR_ADMIT_JANON, &q, 200, free_cb));
  g_tiers = 0;
  void *p = nr_admit_skipq_pop(NR_ADMIT_JANON, &q, 200, free_cb);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ(7, *(int *)p);
  free(p);
  EXPECT_EQ(nullptr, nr_admit_skipq_pop(NR_ADMIT_JANON, &q, 200, free_cb));
  uint64_t a, s, r, e;
  nr_admit_class_counts(NR_ADMIT_JANON, &a, &s, &r, &e);
  EXPECT_EQ(1u, r);
  EXPECT_EQ(0u, e);
  EXPECT_EQ(0, g_freed);
}

TEST(Admit, BacklogExpiresOld)
{
  nr_admit_reset_for_test();
  nr_admit_skipq_t q = {};
  g_tiers = 0;
  g_freed = 0;
  nr_admit_skipq_push(NR_ADMIT_DMRS_ID, &q, mk(1), 1000, free_cb); // deadline 1000
  EXPECT_EQ(nullptr, nr_admit_skipq_pop(NR_ADMIT_DMRS_ID, &q, 2000, free_cb));
  EXPECT_EQ(1, g_freed);
  uint64_t a, s, r, e;
  nr_admit_class_counts(NR_ADMIT_DMRS_ID, &a, &s, &r, &e);
  EXPECT_EQ(1u, e);
  EXPECT_EQ(0u, r);
  // full backlog drops the oldest
  g_freed = 0;
  for (int i = 0; i < NR_ADMIT_SKIPQ_DEPTH + 1; i++) nr_admit_skipq_push(NR_ADMIT_DMRS_ID, &q, mk(i), 1 << 30, free_cb);
  EXPECT_EQ(1, g_freed);
  nr_admit_class_counts(NR_ADMIT_DMRS_ID, &a, &s, &r, &e);
  EXPECT_EQ(2u, e);
  void *p = nr_admit_skipq_pop(NR_ADMIT_DMRS_ID, &q, 0, free_cb);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ(1, *(int *)p); // item 0 was the dropped one; oldest survivor first
  free(p);
  while ((p = nr_admit_skipq_pop(NR_ADMIT_DMRS_ID, &q, 0, free_cb))) free(p);
}
