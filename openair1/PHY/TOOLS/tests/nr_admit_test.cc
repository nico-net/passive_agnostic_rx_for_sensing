#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "nr_admit.h"
}
// Strong overrides of nr_admit.c's weak core-pool hooks: the test drives the shed level.
static int g_tiers = 0;
static uint64_t g_note[3];
extern "C" int nr_pool_site_shed_tiers(void) { return g_tiers; }
extern "C" void nr_pool_site_shed_note(int tier) { g_note[tier]++; }
static int g_cells = 0, g_cell = 0, g_mode = 0;
extern "C" int nr_pool_site_active_cells(void) { return g_cells; }
extern "C" int nr_cell_idx_fn(void) { return g_cell; }
extern "C" int nr_isac_mode(void) { return g_mode; }

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
  g_cells = 2;
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
  g_cells = 0;
}

// I2: below 2 active cells nothing is counted, for any class
TEST(Admit, NoCountsWithOneCell)
{
  nr_admit_reset_for_test();
  g_cells = 1;
  for (int c = 0; c < NR_ADMIT_N; c++) nr_admit_try((nr_admit_class_t)c, 0, 0, 0);
  uint64_t a, s, r, e;
  for (int c = 0; c < NR_ADMIT_N; c++) {
    nr_admit_class_counts((nr_admit_class_t)c, &a, &s, &r, &e);
    EXPECT_EQ(0u, a + s + r + e) << c;
  }
  char b[256];
  nr_admit_stats(b, sizeof b);
  EXPECT_STREQ("", b);
  g_cells = 0;
}

// I1: each cell reads and reports its own counts
TEST(Admit, CountersPerCell)
{
  nr_admit_reset_for_test();
  g_cells = 2;
  g_mode = 2;
  g_tiers = 3;
  g_cell = 0;
  nr_admit_try(NR_ADMIT_JANON, 0, 0, 0);
  g_cell = 1;
  nr_admit_try(NR_ADMIT_JANON, 0, 0, 0);
  nr_admit_try(NR_ADMIT_JANON, 0, 0, 0);
  uint64_t a, s, r, e;
  nr_admit_class_counts(NR_ADMIT_JANON, &a, &s, &r, &e);
  EXPECT_EQ(2u, a);
  char b[512];
  nr_admit_stats(b, sizeof b);
  EXPECT_NE(nullptr, strstr(b, "janon[attempts=2 skipped=2")) << b;
  g_cell = 0;
  nr_admit_class_counts(NR_ADMIT_JANON, &a, &s, &r, &e);
  EXPECT_EQ(1u, a);
  nr_admit_stats(b, sizeof b);
  EXPECT_NE(nullptr, strstr(b, "janon[attempts=1 skipped=1")) << b;
  g_mode = 0; // legacy: the line does not carry the class segments
  nr_admit_stats(b, sizeof b);
  EXPECT_EQ(nullptr, strstr(b, "attempts=")) << b;
  g_cells = 0; g_tiers = 0;
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

// an expired item is purged even while the pool sheds (freed, counted), a live one stays
TEST(Admit, PurgeWhileShedding)
{
  nr_admit_reset_for_test();
  nr_admit_skipq_t q = {};
  g_freed = 0;
  nr_admit_skipq_push(NR_ADMIT_JANON, &q, mk(1), 1000, free_cb);
  nr_admit_skipq_push(NR_ADMIT_JANON, &q, mk(2), 5000, free_cb);
  g_tiers = 2;
  EXPECT_EQ(nullptr, nr_admit_skipq_pop(NR_ADMIT_JANON, &q, 2000, free_cb));
  EXPECT_EQ(1, g_freed);
  EXPECT_EQ(1, nr_admit_skipq_len(&q));
  uint64_t a, s, r, e;
  nr_admit_class_counts(NR_ADMIT_JANON, &a, &s, &r, &e);
  EXPECT_EQ(1u, e);
  EXPECT_EQ(0u, r);
  g_tiers = 0;
  void *p = nr_admit_skipq_pop(NR_ADMIT_JANON, &q, 2000, free_cb);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ(2, *(int *)p);
  free(p);
}

// shutdown drain frees everything and counts it expired
TEST(Admit, DrainFreesAndCounts)
{
  nr_admit_reset_for_test();
  nr_admit_skipq_t q = {};
  g_freed = 0;
  nr_admit_skipq_push(NR_ADMIT_DMRS_ID, &q, mk(1), 1 << 30, free_cb);
  nr_admit_skipq_push(NR_ADMIT_DMRS_ID, &q, mk(2), 1 << 30, free_cb);
  nr_admit_skipq_drain(NR_ADMIT_DMRS_ID, &q, free_cb);
  EXPECT_EQ(2, g_freed);
  EXPECT_EQ(0, nr_admit_skipq_len(&q));
  uint64_t a, s, r, e;
  nr_admit_class_counts(NR_ADMIT_DMRS_ID, &a, &s, &r, &e);
  EXPECT_EQ(2u, e);
}
