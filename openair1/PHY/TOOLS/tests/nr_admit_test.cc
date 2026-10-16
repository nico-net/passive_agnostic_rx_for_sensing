#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "nr_admit.h"
}
static int g_backlog = 0;
static int fake_backlog(void) { return g_backlog; }
class Admit : public ::testing::Test { protected: void SetUp() override {
  setenv("ISAC_BG_LANE", "1", 1); setenv("ISAC_BG_MIN_DUTY_PCT", "5", 1);
  nr_admit_reset_for_test(); nr_admit_register_backlog(fake_backlog); g_backlog = 0; } };
TEST_F(Admit, IdleRuns)      { EXPECT_EQ(1, nr_admit_try(NR_ADMIT_USS, 1000, 2000, 0)); }
TEST_F(Admit, BusySkips)     { g_backlog = 3; nr_admit_note_duty_for_test(NR_ADMIT_USS, 10); EXPECT_EQ(0, nr_admit_try(NR_ADMIT_USS, 1000, 2000, 0)); }
TEST_F(Admit, ExpiredDropped){ EXPECT_EQ(0, nr_admit_try(NR_ADMIT_NID, 3000, 2000, 1)); }
TEST_F(Admit, TokenBypasses) { g_backlog = 3; EXPECT_EQ(1, nr_admit_try(NR_ADMIT_JANON, 1000, 2000, 1)); }
TEST_F(Admit, FloorPreventsStarvation) {
  g_backlog = 5; int ran = 0;
  for (uint64_t t = 0; t < 100000000ull; t += 100000) {          // 100 ms of 100 us steps, always busy
    if (nr_admit_try(NR_ADMIT_CSIRS, t, t + 1000000, 0)) { ran++; nr_admit_done(NR_ADMIT_CSIRS, 100000); }
  }
  EXPECT_GE(ran, 40); EXPECT_LE(ran, 60);                         // ~5 % of 1000 steps
}
TEST_F(Admit, OffMeansAlwaysRun) { setenv("ISAC_BG_LANE", "0", 1); nr_admit_reset_for_test(); g_backlog = 9; EXPECT_EQ(1, nr_admit_try(NR_ADMIT_PROBE, 0, 1, 0)); }
TEST_F(Admit, StatsCountExpired) { nr_admit_try(NR_ADMIT_NID, 3000, 2000, 0); char b[512]; nr_admit_stats(b, sizeof b); EXPECT_NE(nullptr, strstr(b, "nid[run=0 skip_busy=0 expired=1")); }
// Task 2: the anonymous-solver gate is nr_admit_try(JANON, now, enq + 50 ms, suspect); PDSCH backlog from a fake getter.
TEST_F(Admit, JanonSkipsWhenPdschBacklog) {
  g_backlog = 4; nr_admit_note_duty_for_test(NR_ADMIT_JANON, 10);
  EXPECT_EQ(0, nr_admit_try(NR_ADMIT_JANON, 1000, 50000000, 0));
  EXPECT_EQ(1, nr_admit_try(NR_ADMIT_JANON, 1000, 50000000, 1));   // reconfiguration (SUSPECT) bypass
  EXPECT_EQ(0, nr_admit_try(NR_ADMIT_JANON, 60000000, 50000000, 1)); // stale item dropped even with the token
}
