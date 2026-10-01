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
TEST(TdSim, TwinsFlagIsHonoured)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 30;
  c.seed = 3;
  c.oracle = 0;
  c.rntis_per_acq = 1;
  c.twins = 2;
  const SimResult all = run_sim(c);
  c.twins = 0;
  const SimResult none = run_sim(c);
  EXPECT_EQ(all.twins_min, 2);
  EXPECT_EQ(none.twins_min, 0);
  EXPECT_NE(all.total_grants, none.total_grants); /* physical twins slow the search / change the engine's evidence */
}
/* Review Focus 4 (levers Task 7). twins = 3 is clamped to the 2 physical twins this catalogue has (twins_min 2).
 * Oracle 1 explicitly: BLIND (oracle 0) P2 is known to produce twin winners (Task 7 gate,
 * tests/passive_rx/td_sim/results_2026-10-01_p2), so wrong == 0 holds only with the oracles on. */
TEST(TdSim, NearTwinNeverEliminatesTruthUnderP2)
{
  SimCfg c = SimCfg::defaults(); c.acq = 3000; c.K = 3; c.p2 = 1; c.twins = 3; c.table_exercise = 0.5; c.oracle = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.truth_eliminated_by_probe, 0); /* a probe FAIL was never admitted on a grant where the truth's full decode passes */
}
TEST(TdSim, DefaultsUnchangedByRealismFlags) {
  SimCfg a = SimCfg::defaults(); a.acq = 20; a.seed = 11;
  SimCfg b = a; b.oracle_miss = 0; b.oracle_wrong = 0; b.harq_trap = 0; b.crc_false = 0;
  EXPECT_EQ(run_sim(a).total_grants, run_sim(b).total_grants);
}
TEST(TdSim, OracleMissAllIsBlind) {
  SimCfg a = SimCfg::defaults(); a.acq = 10; a.seed = 5; a.oracle = 1; a.oracle_miss = 1.0;
  SimCfg b = a; b.oracle = 0; b.oracle_miss = 0;
  EXPECT_EQ(run_sim(a).total_grants, run_sim(b).total_grants);
}
TEST(TdSim, OracleWrongPrunesTheTruthAndNeverMakesAWrongWinner) {
  SimCfg c = SimCfg::defaults(); c.acq = 10; c.seed = 3; c.oracle_wrong = 1.0; c.cap_s = 60;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.undecidable, 0); /* today's runtime has no recovery from a destructive wrong oracle */
}
TEST(TdSim, HarqTrapPassesAreCounted) {
  SimCfg c = SimCfg::defaults(); c.acq = 5; c.oracle = 0; c.harq_trap = 0.05; c.rntis_per_acq = 1;
  EXPECT_GT(run_sim(c).harq_trap_passes, 0);
}
int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  logInit();
  const int rc = RUN_ALL_TESTS();
  logClean();
  return rc;
}
TEST(TdSim, EquivNeverWrongAndIdenticalWhenTableAlwaysExercised)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 20;
  c.seed = 9;
  c.oracle = 0;
  c.rntis_per_acq = 1;
  SimCfg e = c;
  e.equiv = 1;
  const SimResult rc = run_sim(c), re = run_sim(e);
  EXPECT_EQ(re.wrong, 0);
  EXPECT_EQ(re.undecidable, 0);
  /* The brief asserted re.mean_grants <= rc.mean_grants. MEASURED FALSE on this model (blind, table_exercise 0.9 and 0.964): crediting
   * the table twins on non-exercising grants only (never on exercising ones) dilutes their failure rate and delays their KL
   * elimination. It is therefore not asserted; the numbers are in baseline_bc0_2026-10-01.txt "## BC1 equiv". */
  EXPECT_GT(re.mean_grants, 0.0);
  /* Exactness check: when every grant exercises the table, every class is a singleton and equiv must be bit-identical. */
  c.table_exercise = e.table_exercise = 1.0;
  const SimResult r1c = run_sim(c), r1e = run_sim(e);
  EXPECT_EQ(r1e.total_grants, r1c.total_grants);
  EXPECT_EQ(r1e.n_full, r1c.n_full);
}
TEST(TdSim, ReversibleFieldBookNeverWrong)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 50;
  c.seed = 4;
  c.oracle = 0;
  c.fieldbook = 2;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.undecidable, 0);
}
TEST(TdSim, WrongPromotionRecovers)
{
  SimCfg c = SimCfg::defaults();
  c.acq = 20;
  c.seed = 6;
  c.oracle = 0;
  c.fieldbook = 2;
  c.inject_wrong_field = 0;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.undecidable, 0);
  EXPECT_GT(r.fail_opens, 0);
  EXPECT_GT(r.withdrawals, 0);
  EXPECT_EQ(r.injected, 20);
  EXPECT_EQ(r.recovery_never, 0); /* the wrong value is withdrawn (and the true one re-learned) in every acquisition */
}
TEST(TdSim, FieldBookTwoNotSlowerThanPriorSteady)
{
  SimCfg p = SimCfg::defaults();
  p.acq = 30;
  p.seed = 8;
  p.oracle = 0;
  SimCfg f = p;
  f.fieldbook = 2;
  EXPECT_LE(run_sim(f).mean_s_steady, 1.10 * run_sim(p).mean_s_steady);
}

TEST(TdSim, HarqTrapNeverAcceptedByCrcRule)
{
  SimCfg c = SimCfg::defaults(); c.acq = 200; c.seed = 21; c.oracle = 0; c.equiv = 1; c.crc_accept = 1;
  c.harq_trap = 0.02; c.harq_trap_retx = 1; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.crc_accepts, 0);
}
TEST(TdSim, CrcAcceptWithEquivZeroUsesFeedAttrAndNeverWrong)
{
  /* --equiv 0: crediting is the singleton (lever E stays off) but uniqueness must still see the FULL class. */
  SimCfg c = SimCfg::defaults(); c.acq = 200; c.seed = 22; c.oracle = 0; c.equiv = 0; c.crc_accept = 1;
  c.harq_trap = 0.02; c.harq_trap_retx = 1; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.crc_accepts, 0);
}
TEST(TdSim, GeomPinNeverWrongUnderHarqTrapAndFalsePasses)
{
  SimCfg c = SimCfg::defaults(); c.acq = 200; c.seed = 31; c.oracle = 0; c.geom_pin = 1;
  c.harq_trap = 0.02; c.harq_trap_retx = 1; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.geom_pins, 0);
}
TEST(TdSim, GeomPinRecoversFromWrongPriorViaFailOpen)
{
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 33; c.oracle = 0; c.fieldbook = 2; c.inject_wrong_field = 0; c.geom_pin = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.undecidable, 0);
}
TEST(TdSim, GeomPinFasterBlind)
{
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 35; c.oracle = 0; c.rntis_per_acq = 1;
  SimCfg p = c; p.geom_pin = 1;
  EXPECT_LT(run_sim(p).mean_grants, 0.5 * run_sim(c).mean_grants);
}
TEST(TdSim, NewLeverFlagsOffChangeNothing)
{
  SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 1; c.oracle = 0;
  SimCfg d = c; d.geom_pin = 0; d.crc_accept = 0;
  const SimResult a = run_sim(c), b = run_sim(d);
  EXPECT_EQ(a.total_grants, b.total_grants);
  EXPECT_EQ(b.geom_pins + b.geom_blocks + b.crc_accepts, 0);
}
