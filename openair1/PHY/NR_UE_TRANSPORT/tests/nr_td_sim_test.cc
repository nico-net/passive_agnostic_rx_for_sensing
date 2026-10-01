/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h> /* before log.h: its T() macro breaks gtest templates */
#define NR_TD_SIM_NO_MAIN
#include "nr_td_sim.cc"
TEST(TdSim, DeterministicForSeed)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 20;
  c.seed = 42;
  EXPECT_EQ(run_sim(c).total_grants, run_sim(c).total_grants);
}
TEST(TdSim, BaselineNeverWrong)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 200; /* oracle on (today's runtime): ~1 s per RNTI, cheap */
  c.twins = 2;
  EXPECT_EQ(run_sim(c).wrong, 0);
}
TEST(TdSim, OneRxRank2OnlyReportsUndecidable)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 10;
  c.n_rx = 1;
  c.rank2_frac = 1.0;
  c.gate = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.undecidable, r.acquisitions_rntis);
}
TEST(TdSim, ProbesShareTheGrantState)
{
  /* With probe_inconclusive=0 and K=2, on every grant where the truth fails for SNR, a probe on the truth fails too. */
  SimCfg c = SimCfg::defaults();
  c.acq = 5;
  c.K = 2;
  c.probe_inconclusive = 0;
  c.check_correlation = true;
  EXPECT_EQ(run_sim(c).correlation_violations, 0);
}
TEST(TdSim, OracleOnConvergesInSecondsOracleOffIsMuchSlower)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 20;
  c.rntis_per_acq = 1;
  const SimResult on = run_sim(c);
  c.oracle = 0;
  const SimResult off = run_sim(c);
  EXPECT_EQ(on.undecidable, 0);
  EXPECT_LT(on.mean_s, 10.0);
  EXPECT_GT(off.mean_s, 20 * on.mean_s);
}
TEST(TdSim, WObsOrderingChangesTheBlindSearch)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 5;
  c.rntis_per_acq = 1;
  c.oracle = 0;
  const long base = run_sim(c).total_grants;
  c.w_obs = 1.0f;
  EXPECT_NE(run_sim(c).total_grants, base);
}
/* Pins the copied production DM-RS legality (nr_pdcch_blind_dmrs_mask in nr_pdcch_blind_monitor.c). typeA pos is the ASN.1
 * enum (pos2 = 0). 0x884 is the value the runtime's own test asserts (nr_pdcch_blind_monitor_test.cc
 * TechniqueD.TruthHypothesisRealisesTheGnbsOwnDmrsMask); the rest are evaluated from TS 38.211 Tables 7.4.1.1.2-3/-4. */
TEST(TdSim, DmrsLegalityPinned)
{
  /* (typeA pos, L, S, is_b, add_pos, len) -> mask */
  EXPECT_EQ(sim_legality(0, 13, 1, 0, 2, 1), 0x884);
  EXPECT_EQ(sim_legality(0, 14, 0, 0, 0, 1), 0x4);
  EXPECT_EQ(sim_legality(0, 13, 1, 0, 1, 1), 0x804);
  EXPECT_EQ(sim_legality(0, 7, 0, 0, 0, 1), 0x4);
  EXPECT_EQ(sim_legality(0, 14, 0, 0, 3, 1), 2336 | 4);
  EXPECT_EQ(sim_legality(1, 14, 0, 0, 1, 1), 2048 | 8);
  EXPECT_EQ(sim_legality(1, 14, 0, 0, 3, 1), -1);   /* pos3 forbids add_pos 3 */
  EXPECT_EQ(sim_legality(1, 3, 0, 0, 0, 1), -1);    /* pos3, ld 3 */
  EXPECT_EQ(sim_legality(0, 14, 0, 0, 1, 2), 3072 | 12);
  EXPECT_EQ(sim_legality(0, 7, 4, 0, 0, 1), -1);    /* type A: S > l0 */
  EXPECT_EQ(sim_legality(0, 4, 2, 1, 0, 1), 1 << 2); /* type B, DM-RS on the first PDSCH symbol */
  EXPECT_EQ(sim_legality(0, 7, 0, 1, 1, 1), 17);
  EXPECT_EQ(sim_legality(0, 5, 1, 1, 0, 2), 3 << 1);
  EXPECT_EQ(sim_legality(0, 4, 2, 1, 0, 2), -1);
  EXPECT_EQ(sim_legality(0, 13, 1, 0, 4, 1), -1);
  EXPECT_EQ(sim_legality(0, 13, 1, 0, 2, 3), -1);
}
TEST(TdSim, PriorOnlyWhenFieldbookOffAndSpeedsLaterRntis)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 8;
  c.oracle = 0;
  c.rntis_per_acq = 4;
  const SimResult on = run_sim(c);
  c.prior = 0;
  const SimResult off = run_sim(c);
  EXPECT_LT(on.mean_s, off.mean_s);
  c.prior = 1;
  c.fieldbook = 1; /* prior pruning forced off: identical engine inputs to prior=0 plus field-book ordering with weights 0 */
  EXPECT_EQ(run_sim(c).total_grants, off.total_grants);
}
int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  logInit();
  const int rc = RUN_ALL_TESTS();
  logClean();
  return rc;
}
