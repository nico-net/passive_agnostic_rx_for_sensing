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
int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  logInit();
  const int rc = RUN_ALL_TESTS();
  logClean();
  return rc;
}
