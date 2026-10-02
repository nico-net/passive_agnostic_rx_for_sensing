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
/* ---- Simulator v2 (BC8): slot model, physical k0 trap, DCI observation, TDD, certified flag ---- */
static std::string td_run_cmd(const std::string &cmd)
{
  std::string out;
  FILE *f = popen(cmd.c_str(), "r");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  pclose(f);
  return out;
}
/* The v1 reference binary comes from NR_TD_SIM_REF (built at 77775006f2); the tested one is ./nr_td_sim next to this test (ctest cwd = build dir). */
TEST(TdSimV2, SlotModelOffIsByteIdentical)
{
  const char *ref = getenv("NR_TD_SIM_REF");
  if (!ref) GTEST_SKIP() << "NR_TD_SIM_REF (v1 reference binary) not set";
  const char *arms[] = {"--oracle 0 --fieldbook 0", "--oracle 1 --fieldbook 0", "--oracle 0 --fieldbook 2", "--oracle 1 --fieldbook 2",
                        "--oracle 0 --crc-accept 1 --geom-pin 1 --k0-trap-adj 0.3 --crc-false 1e-3 --equiv 0",
                        "--oracle 1 --fieldbook 2 --inject-wrong-field 0"};
  for (const char *a : arms) {
    const std::string tail = std::string(" --acq 20 --seed 1 ") + a;
    const std::string r = td_run_cmd(std::string(ref) + tail);
    ASSERT_FALSE(r.empty()) << a;
    EXPECT_EQ(r, td_run_cmd("./nr_td_sim" + tail)) << a;
    /* the new flags at their explicit defaults change nothing */
    EXPECT_EQ(r, td_run_cmd("./nr_td_sim" + tail + " --slot-model 0 --k0-oracle-legacy 1 --fo-always 0 --truth-k0 -1 --dci-miss 0 --dci-false 0")) << a;
  }
}
TEST(TdSimV2, SlotKnobsAreInertWithTheSlotModelOff)
{
  SimCfg a = SimCfg::defaults(); a.acq = 20; a.seed = 9;
  SimCfg b = a; b.grant_prob = 0.1; b.persist = 0.3; b.adjacency = 1.0; b.dci_miss = 0.5; b.dci_false = 0.5; b.tdd = "DDDSU";
  const SimResult x = run_sim(a), y = run_sim(b);
  EXPECT_EQ(x.total_grants, y.total_grants);
  EXPECT_EQ(y.dci_missed + y.dci_false + y.proc_grants + y.certified_grants, 0);
}
TEST(TdSimV2, PersistentAllocationProducesK0Trap)
{
  /* oracle off: both k0 siblings alive. Full adjacency + persistence: the neighbour slot carries an identical allocation, the physical trap fires. */
  SimCfg c = SimCfg::defaults(); c.acq = 6; c.seed = 3; c.oracle = 0; c.rntis_per_acq = 1; c.slot_model = 1; c.persist = 1.0; c.adjacency = 1.0; c.cap_s = 100;
  EXPECT_GT(run_sim(c).k0_trap_passes, 0);
  /* adjacency 0: never two consecutive grants, so the shifted slot is always empty and the trap can never fire (no random trap probability in v2) */
  c.adjacency = 0.0;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.k0_trap_passes, 0);
  EXPECT_EQ(r.adj_grants, 0);
}
TEST(TdSimV2, TrapNeedsAnIdenticalAllocation)
{
  SimCfg c = SimCfg::defaults(); c.acq = 6; c.seed = 3; c.oracle = 0; c.rntis_per_acq = 1; c.slot_model = 1; c.persist = 0.0; c.adjacency = 1.0; c.cap_s = 100;
  const SimResult r = run_sim(c); /* persist 0: the neighbour's (mcs, rank, PRBs, rv) equal this grant's only by accident (~1e-3) */
  SimCfg d = c; d.persist = 1.0;
  EXPECT_LT(r.k0_trap_passes * 20, run_sim(d).k0_trap_passes + 1);
}
TEST(TdSimV2, TddWrongDirectionNeverCarriesPdsch)
{
  for (int k0 = 0; k0 <= 1; k0++) {
    SimCfg c = SimCfg::defaults(); c.slot_model = 1; c.tdd = "DDDSU"; c.adjacency = 1.0; c.dci_false = 1.0; c.persist = 0.5;
    SlotTimeline tl(c, k0, 11, 12);
    long n_ul = 0, n_pdsch = 0, n_spur = 0;
    for (long s = 0; s < 4000; s++) {
      const SimSlot sl = tl.slot(s);
      if (!tl.dl(s)) {
        n_ul++;
        EXPECT_FALSE(sl.occ.present) << "PDSCH in a UL slot " << s;
        EXPECT_FALSE(sl.spur) << "PDCCH (spurious DCI) in a UL slot " << s;
        EXPECT_TRUE(tl.observed_dci(s).empty());
      }
      if (sl.occ.present) {
        n_pdsch++;
        EXPECT_TRUE(tl.dl(s - k0)) << "DCI of slot " << s << " would sit in a UL slot";
      }
      n_spur += sl.spur;
    }
    EXPECT_EQ(n_ul, 800); /* one U per 5 slots */
    EXPECT_GT(n_pdsch, 0);
    EXPECT_GT(n_spur, 0);
  }
  /* the pattern also holds end to end: grants/s fall with UL slots, nothing is generated for them */
  SimCfg c = SimCfg::defaults(); c.slot_model = 1; c.tdd = "DDDSU"; c.adjacency = 1.0; c.acq = 3; c.rntis_per_acq = 1; c.k0_oracle_legacy = 0;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.proc_grants, 0);
  EXPECT_EQ(r.wrong, 0);
  /* "UUUUD" has no DL slot whose previous slot is DL: a k0 = 1 truth can never be scheduled -> terminates undecidable (no hang) */
  c.tdd = "UUUUD"; c.truth_k0 = 1; c.cap_s = 10;
  const SimResult u = run_sim(c);
  EXPECT_EQ(u.proc_grants, 0);
  EXPECT_EQ(u.undecidable, u.acquisitions_rntis);
}
TEST(TdSimV2, UnseenNeighbourIsNeverCertified)
{
  SimCfg c = SimCfg::defaults(); c.slot_model = 1; c.persist = 0.0; c.adjacency = 1.0; c.dci_miss = 0.5;
  SlotTimeline tl(c, 0, 21, 22);
  long certified = 0, unseen = 0, seen = 0;
  for (long t = 4; t < 4000; t++) {
    const SimSlot sl = tl.slot(t);
    if (!sl.occ.present) continue;
    for (int L = 0; L <= 1; L++) {
      const long x = t + (L == 0 ? -1 : +1); /* the neighbour slot of the only sibling offset (k0 catalogue {0,1}) */
      const bool nb_seen = !tl.observed_dci(x).empty();
      const bool cert = sim_certified(tl, t, sl.occ.key, L, 0x3);
      nb_seen ? seen++ : unseen++;
      if (!nb_seen) EXPECT_FALSE(cert) << "t=" << t << " L=" << L;
      certified += cert;
    }
  }
  EXPECT_GT(unseen, 100);
  EXPECT_GT(seen, 100);
  EXPECT_GT(certified, 0); /* a SEEN, incompatible neighbour certifies */
  EXPECT_LE(certified, seen);
  /* every DCI missed: nothing is ever certified */
  c.dci_miss = 1.0;
  SlotTimeline none(c, 0, 21, 22);
  for (long t = 4; t < 500; t++)
    if (none.slot(t).occ.present) EXPECT_FALSE(sim_certified(none, t, none.slot(t).occ.key, 0, 0x3));
}
TEST(TdSimV2, CompatibleNeighbourGivesNoCertification)
{
  SimCfg c = SimCfg::defaults(); c.slot_model = 1; c.persist = 1.0; c.adjacency = 1.0; c.dci_miss = 0.0;
  SlotTimeline tl(c, 0, 5, 6);
  long compat = 0, incompat = 0, cert_inc = 0;
  for (long t = 4; t < 4000; t++) {
    const SimSlot sl = tl.slot(t);
    const SimSlot nb = tl.slot(t - 1); /* leader 0, sibling 1: the world "truth is 1" puts the occupant at t-1 */
    ASSERT_TRUE(sl.occ.present && nb.occ.present);
    const bool cert = sim_certified(tl, t, sl.occ.key, 0, 0x3);
    if (sim_compat(nb.occ.key, sl.occ.key)) {
      compat++;
      EXPECT_FALSE(cert) << "compatible observed neighbour must give no certification, t=" << t;
    } else {
      incompat++;
      cert_inc += cert; /* persist 1: only the rv differs; an incompatible (different rv) observed neighbour certifies */
    }
  }
  EXPECT_GT(compat, 100);
  EXPECT_GT(incompat, 100);
  EXPECT_EQ(cert_inc, incompat);
}
TEST(TdSimV2, SpuriousDciCanCertifyWrongly)
{
  SimCfg c = SimCfg::defaults(); c.acq = 8; c.seed = 4; c.oracle = 0; c.rntis_per_acq = 1; c.slot_model = 1; c.persist = 1.0; c.adjacency = 1.0;
  c.dci_miss = 0.5; c.cap_s = 100;
  EXPECT_EQ(run_sim(c).certified_wrong, 0); /* soundness without false accepts: a miss only ever makes a grant ambiguous */
  c.dci_false = 1.0;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.certified_wrong, 0); /* real DCI missed + incompatible spurious DCI stands in for it */
  EXPECT_GT(r.dci_false, 0);
  EXPECT_GT(r.dci_missed, 0);
}
TEST(TdSimV2, CertifiedGrantsAreCountedAndGrowWithObservation)
{
  SimCfg c = SimCfg::defaults(); c.acq = 6; c.seed = 4; c.oracle = 0; c.rntis_per_acq = 1; c.slot_model = 1; c.persist = 0.5; c.adjacency = 1.0; c.cap_s = 100;
  const SimResult a = run_sim(c);
  EXPECT_GT(a.certified_grants, 0);
  EXPECT_EQ(a.certified_wrong, 0);
  EXPECT_EQ(a.dci_missed, 0);
  SimCfg m = c; m.dci_miss = 0.5;
  const SimResult b = run_sim(m);
  EXPECT_GT(b.dci_missed, 0);
  EXPECT_LT((double)b.certified_grants / (double)b.proc_grants, (double)a.certified_grants / (double)a.proc_grants);
}
/* K39 in the simulator's own oracle model (BC7 -> BC8 ruling 3) */
TEST(TdSimV2, AdjacentTrafficTrueK0OneSurvivesOracle)
{
  SimCfg c = SimCfg::defaults(); c.acq = 12; c.seed = 2; c.oracle = 1; c.truth_k0 = 1; c.slot_model = 1; c.adjacency = 1.0; c.k0_oracle_legacy = 0; c.cap_s = 60;
  const SimResult r = run_sim(c);
  ASSERT_GT(r.recs.size(), 0u);
  for (const RntiRec &x : r.recs) {
    EXPECT_EQ(x.truth_k0, 1);
    EXPECT_GE(x.truth_kl_trials, 0) << "the true k0 = 1 hypothesis was pruned by the oracle (acq " << x.acq << " rank " << x.rnti_rank << ")";
  }
  EXPECT_EQ(r.wrong, 0);
}
TEST(TdSimV2, LegacyOracleWithAdjacentTrafficPrunesTrueK0One)
{
  SimCfg c = SimCfg::defaults(); c.acq = 12; c.seed = 2; c.oracle = 1; c.truth_k0 = 1; c.slot_model = 1; c.adjacency = 1.0; c.k0_oracle_legacy = 1; c.cap_s = 60;
  const SimResult r = run_sim(c);
  long pruned = 0;
  for (const RntiRec &x : r.recs) pruned += x.truth_kl_trials < 0;
  EXPECT_GT(pruned, 0);
  EXPECT_GT(r.undecidable + r.wrong, 0); /* the bug: the true k0 is gone; the run either never decides or the trapped k0 = 0 sibling wins */
  /* control: no adjacency => the DCI's own slot never carries a PDSCH for a k0 = 1 truth, the legacy oracle makes no k0 claim */
  c.adjacency = 0.0;
  const SimResult z = run_sim(c);
  for (const RntiRec &x : z.recs) EXPECT_GE(x.truth_kl_trials, 0);
  /* k0 = 0 truths: the legacy claim is correct, with or without adjacency */
  c.truth_k0 = 0; c.adjacency = 1.0;
  const SimResult o = run_sim(c);
  for (const RntiRec &x : o.recs) EXPECT_GE(x.truth_kl_trials, 0);
}
TEST(TdSimV2, TruthK0FlagForcesTheTruth)
{
  for (int k0 = 0; k0 <= 1; k0++) {
    SimCfg c = SimCfg::defaults(); c.acq = 10; c.truth_k0 = k0; c.slot_model = 1; c.rntis_per_acq = 1;
    for (const RntiRec &x : run_sim(c).recs) EXPECT_EQ(x.truth_k0, k0);
  }
}
TEST(TdSimV2, InjectWrongFieldThreeDiffersOnlyInK0AndRecovers)
{
  SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 6; c.oracle = 0; c.fieldbook = 2; c.inject_wrong_field = 3; c.rntis_per_acq = 3; c.cap_s = 600;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.injected, c.acq);
  EXPECT_EQ(r.inject_skipped, 0);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.fail_opens, 0); /* the dormant true sibling can only be recovered by fail-open */
}
TEST(TdSimV2, FailOpenAlwaysFiresOutsideFieldbookTwoWhenPPinsAreActive)
{
  /* lever P active, fieldbook 0, sibling guard off + physical k0 trap: P pins wrong geometries; without --fo-always nothing reopens them */
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 8; c.oracle = 0; c.rntis_per_acq = 1; c.geom_pin = 1; c.cap_s = 300; c.slot_model = 1;
  c.adjacency = 1.0; c.persist = 1.0; c.sib_pmin = 0; c.fo_alpha = 0.3; c.fo_pmin = 0.5;
  const SimResult off = run_sim(c);
  EXPECT_EQ(off.fail_opens, 0);
  EXPECT_GT(off.wrong_pins, 0);
  c.fo_always = 1;
  const SimResult on = run_sim(c);
  EXPECT_GT(on.fail_opens, 0);
  EXPECT_LT(on.wrong, off.wrong); /* a wrong pin can recover */
}
TEST(TdSimV2, DciObservationCounters)
{
  SimCfg c = SimCfg::defaults(); c.acq = 5; c.oracle = 1; c.slot_model = 1; c.rntis_per_acq = 2;
  const SimResult z = run_sim(c);
  EXPECT_EQ(z.dci_missed, 0);
  EXPECT_EQ(z.dci_false, 0);
  c.dci_miss = 0.3; c.dci_false = 0.3;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.dci_missed, 0);
  EXPECT_GT(r.dci_false, 0);
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

/* Fix round 1: three separate trap categories (--crc-false, --retx-trap, --k0-trap-adj); the legacy --harq-trap flags stay for byte identity. */
TEST(TdSim, HarqTrapNeverAcceptedByCrcRule)
{
  SimCfg c = SimCfg::defaults(); c.acq = 15; c.seed = 21; c.oracle = 0; c.equiv = 1; c.crc_accept = 1; /* the k0 trap slows even the KL rule: ~25k grants per RNTI */
  c.retx_trap = 0.02; c.k0_trap_adj = 0.5; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_GT(r.k0_trap_passes, 0); /* the trap really fires */
}
TEST(TdSim, CrcAcceptWithEquivZeroUsesFeedAttrAndNeverWrong)
{
  /* --equiv 0: crediting is the singleton (lever E stays off) but uniqueness must still see the FULL class. */
  SimCfg c = SimCfg::defaults(); c.acq = 15; c.seed = 22; c.oracle = 0; c.equiv = 0; c.crc_accept = 1;
  c.retx_trap = 0.02; c.k0_trap_adj = 0.5; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
}
TEST(TdSim, CrcAcceptDecidesWithoutTraps)
{
  SimCfg c = SimCfg::defaults(); c.acq = 100; c.seed = 23; c.oracle = 0; c.crc_accept = 1; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.crc_accepts, 0);
  EXPECT_GT(r.sib_trials, 0); /* the sibling guard actually scheduled tests */
}
TEST(TdSim, GeomPinNeverWrongUnderTrapsAndFalsePasses)
{
  SimCfg c = SimCfg::defaults(); c.acq = 15; c.seed = 31; c.oracle = 0; c.geom_pin = 1;
  c.retx_trap = 0.02; c.k0_trap_adj = 0.5; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
}
TEST(TdSim, GeomPinUnderALightK0TrapPinsAndBlocksAndIsNeverWrong)
{
  /* A = 0.5 blocks every pin (vacuous), so this is the non-vacuous trap case: the guard sometimes sees the trap sibling pass (block) and
   * sometimes passes clean (pin); either way 0 wrong and 0 wrong pins. */
  SimCfg c = SimCfg::defaults(); c.acq = 60; c.seed = 41; c.oracle = 0; c.geom_pin = 1;
  c.retx_trap = 0; c.k0_trap_adj = 0.005; c.crc_false = 1e-4; c.rntis_per_acq = 1; /* 0.02 blocks every pin: 300 sibling trials see ~4 trap passes */
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_GT(r.geom_pins, 0);
  EXPECT_GT(r.sib_blocks, 0);
}
TEST(TdSim, GeomPinPinsWithoutTraps)
{
  SimCfg c = SimCfg::defaults(); c.acq = 100; c.seed = 32; c.oracle = 0; c.geom_pin = 1; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
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
  /* sibling guard on (default p_min 0.05) and no traps: still clearly faster than the KL rule alone */
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 35; c.oracle = 0; c.rntis_per_acq = 1;
  SimCfg p = c; p.geom_pin = 1;
  EXPECT_LT(run_sim(p).mean_grants, 0.5 * run_sim(c).mean_grants);
}
TEST(TdSim, ExploitHotPassesDoNotBuildFastPathEvidence)
{
  /* Stress: p_f 1e-3. Under the old (hot-inclusive) evidence the stress arm produced 47/8000 wrong; the explore-only stream must stay
   * within a small multiple of its bound (the strict check is the Step 4b campaign, this is the cheap regression guard). */
  SimCfg c = SimCfg::defaults(); c.acq = 300; c.seed = 5; c.oracle = 0; c.crc_accept = 1; c.crc_false = 1e-3; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_LE(r.wrong, 3 + 3 * r.crc_bound);
}
TEST(TdSim, NewLeverFlagsOffChangeNothing)
{
  SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 1; c.oracle = 0;
  SimCfg d = c; d.geom_pin = 0; d.crc_accept = 0; d.retx_trap = 0; d.k0_trap_adj = 0;
  const SimResult a = run_sim(c), b = run_sim(d);
  EXPECT_EQ(a.total_grants, b.total_grants);
  EXPECT_EQ(b.geom_pins + b.geom_blocks + b.crc_accepts + b.sib_trials + b.k0_trap_passes + b.retx_trap_passes, 0);
}
TEST(TdSim, K0TrapAdjIsCountedSeparately)
{
  SimCfg c = SimCfg::defaults(); c.acq = 10; c.seed = 7; c.oracle = 0; c.k0_trap_adj = 0.5; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.k0_trap_passes, 0);
  EXPECT_EQ(r.retx_trap_passes, 0);
}
