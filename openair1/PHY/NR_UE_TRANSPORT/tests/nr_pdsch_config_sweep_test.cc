#include <cstdlib>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_config_sweep.h"
}

// Feed `n` grants round-robin, letting hypothesis `truth` decode at `p_true` and every other
// hypothesis at `p_false`. Mirrors the live loop: next() then feed() with that index.
static void drive(nr_pdsch_config_sweep_state_t &st, int truth, double p_true, double p_false, int n)
{
  unsigned seed = 12345;
  for (int i = 0; i < n; i++) {
    nr_pdsch_cfg_hypothesis_t h;
    const int idx = nr_pdsch_config_sweep_next(&st, &h);
    const double p = (idx == truth) ? p_true : p_false;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    nr_pdsch_config_sweep_feed(&st, idx, u < p);
  }
}

// The 2026-09-08 live failure: a 200s capture (195,003 decoded, 20.6% overall TB-CRC, Technique D
// armed the whole run) never converged. Root cause -- nr_pdsch_config_sweep_feed() requires EVERY
// hypothesis to reach SWEEP_MIN_TRIALS before deciding, but a hypothesis whose DM-RS mask is
// illegal per TS 38.211 never gets fed at all downstream (the caller resets sweep_idx to -1 for
// it), so its trial count is stuck at 0 forever -- deadlock, on any cell, for as long as even one
// enumerated combination is unrealisable.

static int32_t always_legal(int, int, int, int, int, int) { return 1; }
static int32_t reject_every_third(int, int tda_length, int, int, int, int)
{
  // Synthetic stand-in for "some real (S,L,add_pos,max_len) combos are TS-38.211-illegal": reject
  // roughly a third of them by an arbitrary deterministic rule, so init() has something to filter.
  return (tda_length % 3 == 0) ? -1 : 1;
}

TEST(PdschConfigSweep, ALegalityFilterCannotDeadlockConvergence) {
  // This is the exact bug: without filtering at enumeration time, a permanently-unfeedable
  // hypothesis blocks the "wait for all" gate forever, however many trials the FEEDABLE
  // hypotheses accumulate.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2, 0, reject_every_third);
  ASSERT_GT(n, 0);
  const int truth = n / 2;
  drive(st, truth, 0.76, 0.0, 2000 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), truth)
      << "convergence deadlocked -- an illegal hypothesis was enumerated and never filtered";
}

TEST(PdschConfigSweep, FilteringExcludesEveryRejectedCombination) {
  // Sanity on the filter itself. NOT a count comparison: the real table has 192 total
  // (S,L,add_pos,max_len,mcs_table) combinations against a 64-entry cap, so a filter that removes
  // some combos does NOT shrink the enumerated COUNT -- init() keeps iterating and backfills from
  // later in the table until it fills 64 (or exhausts the legal supply). That is exactly the
  // mistake an earlier version of this test made (asserted n_some < n_all, which is false whenever
  // enough legal combos exist to backfill the cap -- verified true here too). The property that
  // actually matters, and that this checks instead, is that no REJECTED combination survives into
  // the live set.
  nr_pdsch_config_sweep_state_t st_all, st_some;
  const int n_all  = nr_pdsch_config_sweep_init(&st_all,  2, 0, always_legal);
  const int n_some = nr_pdsch_config_sweep_init(&st_some, 2, 0, reject_every_third);
  EXPECT_EQ(n_some, n_all) << "documents the backfill behaviour above -- if this ever fails, the "
                              "legal supply dropped below the cap, which is fine, just re-read why";
  for (int i = 0; i < n_some; i++) {
    EXPECT_GT(reject_every_third(0, st_some.hyp[i].tda_length, 0, 0, 0, 0), 0)
        << "a combination reject_every_third() would reject survived filtering, at index " << i;
  }
}

TEST(PdschConfigSweep, EnumeratesAndStartsUndecided) {
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  EXPECT_GT(n, 8);
  EXPECT_LE(n, NR_PDSCH_SWEEP_MAX_HYP);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, FindsTheTruthAtTheMeasuredWorkingRate) {
  // 76 % is what this rig actually decodes at when the config is right; a wrong hypothesis
  // decodes essentially nothing.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  const int truth = n / 3;
  drive(st, truth, 0.76, 0.0, 400 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), truth);
}

TEST(PdschConfigSweep, FindsTheTruthOnAMarginalLink) {
  // The link itself may be poor; the sweep must still separate 5 % from 0 %.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  const int truth = 3;
  drive(st, truth, 0.05, 0.0, 800 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), truth);
}

TEST(PdschConfigSweep, StaysUndecidedWhenNothingDecodes) {
  // Geometry wrong upstream (bad dci_length/CORESET) -> every hypothesis scores zero. Reporting a
  // winner here would hand the receiver a confident wrong config, which is worse than no answer.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  drive(st, -1, 0.0, 0.0, 600 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, StaysUndecidedWhenTwoHypothesesAreIndistinguishable) {
  // Two hypotheses that the traffic cannot separate must NOT be resolved by a coin flip.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  unsigned seed = 999;
  for (int i = 0; i < 600 * n; i++) {
    nr_pdsch_cfg_hypothesis_t h;
    const int idx = nr_pdsch_config_sweep_next(&st, &h);
    const double p = (idx == 2 || idx == 5) ? 0.60 : 0.0;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    nr_pdsch_config_sweep_feed(&st, idx, u < p);
  }
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, DoesNotDecideBeforeEveryHypothesisHasBeenTried) {
  // Otherwise the first hypothesis in the rotation wins by being early, not by being right.
  nr_pdsch_config_sweep_state_t st;
  nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  for (int i = 0; i < 200; i++) {
    nr_pdsch_cfg_hypothesis_t h;
    const int idx = nr_pdsch_config_sweep_next(&st, &h);
    nr_pdsch_config_sweep_feed(&st, idx, idx == 0);
  }
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, PinsTheWinnerOnceDecided) {
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  const int truth = 1;
  drive(st, truth, 0.76, 0.0, 400 * n);
  ASSERT_EQ(nr_pdsch_config_sweep_winner(&st), truth);
  nr_pdsch_cfg_hypothesis_t h;
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(nr_pdsch_config_sweep_next(&st, &h), truth);  // stops rotating
  }
}

TEST(PdschConfigSweep, RejectsBadArguments) {
  EXPECT_EQ(nr_pdsch_config_sweep_init(NULL, 2, 0, NULL), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(NULL), -1);
  nr_pdsch_config_sweep_state_t st;
  nr_pdsch_config_sweep_init(&st, 2, 0 /* pos2 */, NULL /* no legality filter: pure algorithm test */);
  EXPECT_EQ(nr_pdsch_config_sweep_feed(&st, -1, true), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_feed(&st, 9999, true), -1);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
