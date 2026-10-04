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
  if (!ref) GTEST_SKIP() << "SKIPPED (NOT CHECKED): NR_TD_SIM_REF (path of the v1 reference binary built at 77775006f2) is not set; "
                            "TdSimV2.GoldenV1Output below still pins the v1 numbers";
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
/* Always-on v1 pin (M4): numbers produced by the v1 reference binary (77775006f2), `--acq 20 --seed 1`, summed over the per-RNTI records. */
TEST(TdSimV2, GoldenV1Output)
{
  struct G { int oracle, fieldbook, inject; long grants, n_full, n_probe; double mean_grants; };
  const G gold[] = {{0, 0, -1, 3101544, 3101544, 0, 38769.3}, {1, 0, -1, 16379, 16379, 0, 204.7}, {1, 2, 0, 16544, 16544, 0, 206.8}};
  for (const G &g : gold) {
    SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 1; c.oracle = g.oracle; c.fieldbook = g.fieldbook; c.inject_wrong_field = g.inject;
    const SimResult r = run_sim(c);
    EXPECT_EQ(r.total_grants, g.grants) << g.oracle << g.fieldbook;
    EXPECT_EQ(r.n_full, g.n_full);
    EXPECT_EQ(r.n_probe, g.n_probe);
    EXPECT_NEAR(r.mean_grants, g.mean_grants, 0.05);
    EXPECT_EQ(r.wrong + r.undecidable, 0);
  }
}
TEST(TdSimV2, GoldenV1LeverAndTrapArms)
{
  /* P+C lever arm: re-pinned after merging BC9 (fffc8bed3a/95f1b2eeaf/663a9200b8 deliberately change lever semantics:
   * sibling-test trials require new data, the certified table mask covers PRIOR/FIELD-dormant siblings, bounded skips);
   * value from the merged engine @355284580d. Safety is pinned too. The trap-only arm below still matches 77775006f2. */
  SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 1; c.oracle = 0; c.crc_accept = 1; c.geom_pin = 1; c.k0_trap_adj = 0.3; c.crc_false = 1e-3;
  const SimResult pc = run_sim(c);
  EXPECT_EQ(pc.total_grants, 5320506);
  EXPECT_EQ(pc.k0_trap_passes, 3144);
  EXPECT_EQ(pc.wrong, 0);
  EXPECT_EQ(pc.wrong_pins, 0);
  SimCfg d = SimCfg::defaults(); d.acq = 10; d.seed = 7; d.oracle = 0; d.k0_trap_adj = 0.5; d.rntis_per_acq = 1;
  const SimResult t = run_sim(d);
  EXPECT_EQ(t.total_grants, 4158534);
  EXPECT_EQ(t.k0_trap_passes, 2158);
  EXPECT_EQ(t.undecidable, 2);
}
TEST(TdSimV2, SlotKnobsAreInertWithTheSlotModelOff)
{
  SimCfg a = SimCfg::defaults(); a.acq = 20; a.seed = 9;
  SimCfg b = a; b.grant_prob = 0.1; b.persist = 0.3; b.adjacency = 1.0; b.dci_miss = 0.5; b.dci_false = 0.5; b.tdd = "DDDSU"; b.other_ue_occ = 0.5; b.snr_rho = 0.9; b.mcs_change = 0.5; b.tdd_s_dl_symbols = 3;
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
      if (tl.dir(s) == 'S') EXPECT_TRUE(tl.dl(s)); /* PDCCH-capable */
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
  /* M1: with a truth allocation that does not fit the S slot's DL symbols, S slots carry no PDSCH (but still PDCCH) */
  {
    SimCfg c = SimCfg::defaults(); c.slot_model = 1; c.tdd = "DDDSU"; c.adjacency = 1.0; c.tdd_s_dl_symbols = 6;
    SlotTimeline fit(c, 0, 3, 4, true), nofit(c, 0, 3, 4, false);
    long s_fit = 0, s_nofit = 0;
    for (long s = 0; s < 2000; s++) {
      const bool is_s = fit.dir(s) == 'S';
      if (is_s) { s_fit += fit.slot(s).occ.present; s_nofit += nofit.slot(s).occ.present; EXPECT_TRUE(nofit.dl(s)); }
    }
    EXPECT_GT(s_fit, 0);
    EXPECT_EQ(s_nofit, 0);
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
      const bool cert = sim_certified(tl, t, sl.occ.key, L, 0x3, 7);
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
    if (none.slot(t).occ.present) EXPECT_FALSE(sim_certified(none, t, none.slot(t).occ.key, 0, 0x3, 7));
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
    const bool cert = sim_certified(tl, t, sl.occ.key, 0, 0x3, 7);
    if (sim_compat_any(nb.occ.key, sl.occ.key, 7)) {
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
  EXPECT_EQ(r.fail_opens, 0); /* BC6b: the truth is never dormant (pruning ignores k0), so no fail-open is needed; the k0 part is re-learned instead */
  EXPECT_EQ(r.recovery_never, 0);
}
TEST(TdSimV2, FailOpenAlwaysFiresOutsideFieldbookTwoWhenPPinsAreActive)
{
  /* lever P active, fieldbook 0, sibling guard off + physical k0 trap: P pins wrong geometries; without --fo-always nothing reopens them */
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 8; c.oracle = 0; c.rntis_per_acq = 1; c.geom_pin = 1; c.cap_s = 300; c.slot_model = 1;
  c.adjacency = 1.0; c.persist = 1.0; c.sib_pmin = 0; c.fo_alpha = 0.3; c.fo_pmin = 0.5;
  c.cert_evidence = 0; /* the pre-BC9 arm (certified = true): this test needs the uncertified trap passes to count */
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
TEST(TdSimV2, MacMcsTablesPinned)
{
  /* verbatim from nr_mac_common.c Table_51311/12/13 ({Qm, 10 x R}) */
  EXPECT_EQ(sim_cr(0, 0), (2u << 16) | 1200);
  EXPECT_EQ(sim_cr(0, 2), (2u << 16) | 1930);
  EXPECT_EQ(sim_cr(1, 1), (2u << 16) | 1930); /* table 1 mcs 2 and table 2 mcs 1 are both Qm 2, R 193 */
  EXPECT_EQ(sim_cr(2, 8), (2u << 16) | 1930);
  EXPECT_EQ(sim_cr(0, 28), (6u << 16) | 9480);
  EXPECT_EQ(sim_cr(1, 27), (8u << 16) | 9480);
  EXPECT_EQ(sim_cr(1, 20), (8u << 16) | 6825);
  EXPECT_EQ(sim_cr(2, 0), (2u << 16) | 300);
  EXPECT_EQ(sim_cr(2, 28), (6u << 16) | 7720);
  EXPECT_EQ(sim_cr(0, 29), (2u << 16) | 0); /* reserved */
  EXPECT_TRUE(sim_tbl_differs(5, 0, 1));    /* {2,3790} vs {4,3780} */
  EXPECT_FALSE(sim_tbl_differs(0, 0, 1));   /* mcs 0 is {2,1200} in tables 0 and 1: a twin on this grant */
  EXPECT_TRUE(sim_tbl_differs(0, 0, 2));
}
TEST(TdSimV2, CompatibilityIsPerTableComputationNotMcsIndex)
{
  SimKey a, b;
  a.mcs = 2; a.rank = 1; a.prb = 3; a.rv = 0;
  b = a; b.mcs = 1;
  EXPECT_TRUE(sim_compat_tbl(a, 0, b, 1));  /* table 0 mcs 2 == table 1 mcs 1 */
  EXPECT_FALSE(sim_compat_tbl(a, 0, b, 0)); /* same table, different index */
  EXPECT_FALSE(sim_compat_tbl(a, 1, b, 1));
  EXPECT_TRUE(sim_compat_any(a, b, 0x3));   /* tables 0 and 1 alive: some pair coincides */
  EXPECT_FALSE(sim_compat_any(a, b, 0x1));  /* only table 0 alive: incompatible under every alive pair */
  EXPECT_FALSE(sim_compat_any(a, b, 0x4));
  SimKey c = b; c.prb = 4;
  EXPECT_FALSE(sim_compat_any(a, c, 7));
  c = b; c.rv = 2;
  EXPECT_FALSE(sim_compat_any(a, c, 7));
  c = b; c.rank = 2;
  EXPECT_FALSE(sim_compat_any(a, c, 7));
}
TEST(TdSimV2, EmptySiblingSetIsNeverCertified)
{
  SimCfg c = SimCfg::defaults(); c.slot_model = 1; c.persist = 0.0; c.adjacency = 1.0; c.dci_miss = 0.0;
  SlotTimeline tl(c, 0, 5, 6);
  long both = 0;
  for (long t = 4; t < 500; t++) {
    const SimOcc o = tl.slot(t).occ;
    EXPECT_FALSE(sim_certified(tl, t, o.key, 0, 0x1, 7)) << "only k0 = 0 alive: nothing to certify against";
    EXPECT_FALSE(sim_certified(tl, t, o.key, 0, 0x0, 7));
    both += sim_certified(tl, t, o.key, 0, 0x3, 7);
  }
  EXPECT_GT(both, 0); /* control: with a sibling and observed incompatible DCIs it certifies */
}
TEST(TdSimV2, OtherUePdschTriggersTheLegacyOracleAndPrunesTrueK0)
{
  /* no same-RNTI adjacency at all (adjacency 0): the legacy oracle never fires without another UE ... */
  SimCfg c = SimCfg::defaults(); c.acq = 12; c.seed = 2; c.oracle = 1; c.truth_k0 = 1; c.slot_model = 1; c.adjacency = 0.0; c.k0_oracle_legacy = 1; c.cap_s = 30;
  for (const RntiRec &x : run_sim(c).recs) EXPECT_GE(x.truth_kl_trials, 0);
  /* ... another UE overlapping the grant's PRBs does: DM-RS is cell-scrambled, the true k0 is pruned and the wrong mask removes the truth's entries, so
   * the run cannot decide (no same-RNTI neighbour exists to make a trapped sibling win) */
  c.other_ue_occ = 1.0;
  const SimResult r = run_sim(c);
  long pruned = 0;
  for (const RntiRec &x : r.recs) pruned += x.truth_kl_trials < 0;
  EXPECT_GT(pruned, 0);
  EXPECT_GT(r.undecidable, 0);
  EXPECT_EQ(r.wrong, 0);
}
TEST(TdSimV2, MaskObservationNeedsTheOwnSlotOccupied)
{
  /* I1: k0 = 1 truth, adjacency 0 (own slot never carries this RNTI's PDSCH), fixed oracle: no mask observation => blind, much slower than a k0 = 0 truth */
  SimCfg c = SimCfg::defaults(); c.acq = 10; c.seed = 4; c.oracle = 1; c.slot_model = 1; c.adjacency = 0.0; c.k0_oracle_legacy = 0; c.rntis_per_acq = 1; c.cap_s = 600;
  c.truth_k0 = 0;
  const SimResult k0 = run_sim(c);
  c.truth_k0 = 1;
  const SimResult k1 = run_sim(c);
  EXPECT_TRUE(k1.undecidable > k0.undecidable || k1.mean_s > 5 * k0.mean_s); /* blind (plus runtime k0 >= 2 probe layers) vs oracle-pruned */
}
TEST(TdSimV2, K0ProbeLayersAreAddedWhenTheOwnSlotIsEmpty)
{
  SimCfg c = SimCfg::defaults(); c.acq = 10; c.seed = 4; c.oracle = 1; c.slot_model = 1; c.adjacency = 0.0; c.k0_oracle_legacy = 0; c.rntis_per_acq = 1; c.truth_k0 = 1;
  c.other_ue_occ = 0.3; c.cap_s = 600;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.k0_probes, 0);
  EXPECT_GT(r.k0_probe_hyp, 0);
  SimCfg d = c; d.truth_k0 = 0; /* a k0 = 0 truth's own slot is never empty: no probes */
  EXPECT_EQ(run_sim(d).k0_probes, 0);
}
TEST(TdSimV2, McsEvolvesPerSlotIndependentOfAllocationAndSnrIsAr1)
{
  SimCfg c = SimCfg::defaults(); c.slot_model = 1; c.adjacency = 1.0; c.persist = 1.0; c.mcs_change = 0.5; c.snr_rho = 0.0;
  SlotTimeline t0(c, 0, 1, 2);
  long mcs_changes = 0, alloc_changes = 0; SimKey prev; double sx = 0, sxx = 0, sxy = 0; double ps = 0; long n = 0;
  for (long s = 0; s < 6000; s++) {
    const SimOcc o = t0.slot(s).occ;
    if (!o.present) continue;
    if (s > 0) { mcs_changes += o.key.mcs != prev.mcs; alloc_changes += o.key.prb != prev.prb || o.key.rank != prev.rank; sx += ps; sxx += ps * ps; sxy += ps * o.snr; n++; }
    prev = o.key; ps = o.snr;
  }
  EXPECT_GT(mcs_changes, 1000);
  EXPECT_EQ(alloc_changes, 0); /* persist 1: the allocation never changes although the MCS does */
  const double corr0 = (sxy / n - sx / n * sx / n) / (sxx / n - sx / n * sx / n);
  c.snr_rho = 0.9;
  SlotTimeline t1(c, 0, 1, 2);
  sx = sxx = sxy = ps = 0; n = 0;
  for (long s = 0; s < 6000; s++) {
    const SimOcc o = t1.slot(s).occ;
    if (!o.present) continue;
    if (s > 0) { sx += ps; sxx += ps * ps; sxy += ps * o.snr; n++; }
    ps = o.snr;
  }
  const double corr1 = (sxy / n - sx / n * sx / n) / (sxx / n - sx / n * sx / n);
  EXPECT_LT(std::fabs(corr0), 0.1);
  EXPECT_GT(corr1, 0.7);
}
TEST(TdSimV2, TwinTablesAreDistinguishedByTheMcsTableNotADraw)
{
  /* slot model: the same seed with table-exercise 0 or 1 must give identical results (exercised comes from the MCS tables) */
  SimCfg a = SimCfg::defaults(); a.acq = 6; a.seed = 5; a.oracle = 0; a.slot_model = 1; a.rntis_per_acq = 1; a.cap_s = 200;
  SimCfg b = a; b.table_exercise = 0.0;
  EXPECT_EQ(run_sim(a).total_grants, run_sim(b).total_grants);
}
TEST(TdSimV2, ObservedSetMirrorsTheRuntime)
{
  /* the BC8 round-2 model (--obs-lastset 0); the BC7b set model is ObservedLastSymbolSetIsMonotone below */
  SimObs o; o.use_set = false;
  EXPECT_EQ(o.record(0x884, 12, -1), 0);
  EXPECT_EQ(o.record(0x884, 12, 0), 0);
  EXPECT_EQ(o.last[0], 12);
  EXPECT_EQ(o.k0[0], 0);            /* a refinement */
  o.record(0x884, 10, -1);
  EXPECT_EQ(o.last[0], -1);         /* a contradicting last symbol relaxes it to unknown */
  for (int m = 1; m <= 7; m++) EXPECT_GE(o.record((uint16_t)m, 5, -1), 0);
  EXPECT_EQ(o.n, 8);
  EXPECT_EQ(o.record(0x777, 5, -1), -1); /* OBS_MASKS_MAX = 8: a full set drops new masks */
  EXPECT_EQ(o.n, 8);
  nr_pdsch_cfg_hypothesis_t h{};
  h.dmrs_mask = 0x884; h.tda_start = 1; h.tda_length = 11; h.k0 = 1;
  EXPECT_FALSE(o.admits(h, 0));     /* k0 pinned to 0 by the earlier refinement: a k0 = 1 entry is rejected */
  h.k0 = 0;
  EXPECT_TRUE(o.admits(h, 0));      /* last symbol unknown (relaxed), k0 matches */
  h.k0 = 1;
  SimObs p; p.use_set = false; p.record(0x884, 11, 0);
  EXPECT_FALSE(p.admits(h, 0));     /* legacy k0 pin 0 rejects a k0 = 1 entry */
  p.k0[0] = -1;
  EXPECT_TRUE(p.admits(h, 0));      /* the fixed oracle keeps no pin */
  h.tda_length = 10;
  EXPECT_FALSE(p.admits(h, 0));     /* last symbol S+L-1 = 10 != 11 */
  SimObs u; u.use_set = false; u.record(0x1, 3, -1); u.record(0x884, 11, -1);
  h.tda_length = 11;
  EXPECT_TRUE(u.any_admits(h));     /* union semantics */
}
TEST(TdSimV2, ObservedLastSymbolSetIsMonotone)
{
  /* K42 (BC7b, the runtime's obs_record_set / obs_admits): the measured last symbols of a mask accumulate; nothing is re-refined */
  SimObs o;
  ASSERT_TRUE(o.use_set);
  nr_pdsch_cfg_hypothesis_t h{};
  h.dmrs_mask = 0x884; h.tda_start = 2; h.k0 = 1;
  EXPECT_EQ(o.record(0x884, -1, -1), 0);
  h.tda_length = 6;
  EXPECT_TRUE(o.admits(h, 0));      /* nothing measured: every duration */
  o.record(0x884, 13, -1);
  EXPECT_FALSE(o.admits(h, 0));     /* {13}: ends on 7, rejected */
  h.tda_length = 12;
  EXPECT_TRUE(o.admits(h, 0));
  o.record(0x884, 11, -1);
  EXPECT_TRUE(o.admits(h, 0));      /* {11, 13}: the earlier admission survives the contradiction */
  o.record(0x884, 13, -1);
  EXPECT_TRUE(o.admits(h, 0));
  h.tda_length = 10;
  EXPECT_TRUE(o.admits(h, 0));      /* and the new one survives the next observation (no re-refine) */
  EXPECT_EQ(o.lastset[0], (1u << 11) | (1u << 13));
  SimObs g; /* promotion carries the whole set */
  g.record_from(0x884, o, 0);
  EXPECT_EQ(g.lastset[0], o.lastset[0]);
  for (int m = 1; m <= 7; m++) EXPECT_GE(o.record((uint16_t)m, 5, -1), 0);
  EXPECT_EQ(o.record(0x777, 5, -1), -1); /* a full set still drops new masks */
  SimObs p; p.record(0x884, 11, 0);
  h.tda_length = 10;
  EXPECT_FALSE(p.admits(h, 0));     /* the legacy k0 pin is unchanged */
}
TEST(TdSimV2, OtherUeK0OneTruthsConverge)
{
  /* K42 (BC7b): another UE's PDSCH on the grant's PRBs shows the cell's mask with varying last symbols. The round-2 model (relax / re-refine) re-adds and
   * re-prunes on every observation and wipes the evidence each time: every k0 = 1 truth stays undecidable (BC8 R2: 1072/1072). The monotone set does not. */
  SimCfg c = SimCfg::defaults(); c.acq = 10; c.seed = 1; c.oracle = 1; c.slot_model = 1; c.k0_oracle_legacy = 0; c.truth_k0 = 1; c.other_ue_occ = 0.1;
  c.dci_miss = 0.01; c.cap_s = 600;
  c.obs_lastset = 0;
  const SimResult r2 = run_sim(c);
  c.obs_lastset = 1;
  const SimResult r = run_sim(c);
  ASSERT_EQ(r.recs.size(), 40u);
  EXPECT_EQ(r2.undecidable, 40);                 /* the round-2 thrash */
  EXPECT_LE(r.undecidable * 3, r2.undecidable);  /* "well below": measured 9 / 40 at this 600 s cap */
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r2.wrong, 0);
  EXPECT_LT(r.restores * 10, r2.restores);       /* the restore / prune round trips are gone */
  for (const RntiRec &x : r.recs) EXPECT_EQ(x.truth_k0, 1);
}
TEST(TdSimV2, RestoreReAddsTheTruthAfterAForeignMaskPrunedIt)
{
  /* fixed oracle, a k0 = 1 truth with adjacent same-PRB traffic and another UE on the PRBs when the own PDSCH does not overlap: a foreign mask can arrive
   * first and prune the truth; the own mask later triggers restore_observed_typea and the union keeps the truth */
  SimCfg c = SimCfg::defaults(); c.acq = 12; c.seed = 3; c.oracle = 1; c.truth_k0 = 1; c.slot_model = 1; c.adjacency = 1.0; c.persist = 0.5; c.k0_oracle_legacy = 0;
  c.other_ue_occ = 0.5; c.other_ue_same_cfg = 1; c.rntis_per_acq = 1; c.cap_s = 60;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.restores, 0);
  EXPECT_EQ(r.wrong, 0);
  for (const RntiRec &x : r.recs) EXPECT_GE(x.truth_kl_trials, 0) << "the union / restore must keep the truth alive (acq " << x.acq << ")";
}
TEST(TdSimV2, LegacyPinTravelsWithTheRestoredMask)
{
  /* legacy: the pin k0 = 0 is part of the observation, so a restore re-adds only k0 = 0 entries: a k0 = 1 truth cannot come back */
  SimCfg c = SimCfg::defaults(); c.acq = 12; c.seed = 3; c.oracle = 1; c.truth_k0 = 1; c.slot_model = 1; c.adjacency = 1.0; c.persist = 0.5; c.k0_oracle_legacy = 1;
  c.other_ue_occ = 0.5; c.other_ue_same_cfg = 1; c.rntis_per_acq = 1; c.cap_s = 60;
  const SimResult r = run_sim(c);
  long pruned = 0;
  for (const RntiRec &x : r.recs) pruned += x.truth_kl_trials < 0;
  EXPECT_GT(pruned, 0);
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
/* BC9 simulator part: levers C/P count only CERTIFIED explore passes (certified = k0-unambiguous w.r.t. every alive k0 sibling of the decoded
 * hypothesis, from OBSERVED DCIs); TDD exclusion mirrors nr_pdsch_config_sweep_exclude (the engine's own per-hypothesis predicate). */
static SimCfg bc9_cfg()
{
  SimCfg c = SimCfg::defaults();
  c.slot_model = 1; c.oracle = 0; c.crc_accept = 1; c.geom_pin = 1; c.persist = 0.9; c.dci_miss = 0.1; c.dci_false = 1e-3; c.other_ue_occ = 0.1;
  c.acq = 60; c.seed = 1; c.rntis_per_acq = 1; c.cap_s = 3600;
  return c;
}
/* MEASURED finding (BC9 sim): at persistence 0.9 the physical trap makes the sibling-test decode of the neighbouring slot PASS on almost every RNTI, and
 * the engine's sibling guard blocks the fast path on ANY sibling pass (certified or not): levers C/P never fire (sib_blocks == RNTIs, 0 accepts/pins in
 * 480 RNTIs, 2 seeds). Safety then holds because the guard works, not because certification is rare. The firing case needs low persistence. */
TEST(TdSimBc9, CertifiedPCNeverWrongUnderPhysicalTrap)
{
  const SimResult r = run_sim(bc9_cfg()); /* rho 0.9, dci-miss 0.1, dci-false 1e-3, other-ue 0.1 */
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_EQ(r.crc_wrong, 0);
  EXPECT_GT(r.cert_fed, 0);
  EXPECT_LE(r.cert_fed, r.fed_all);
  EXPECT_GE((double)r.sib_blocks, 0.95 * (double)r.acquisitions_rntis); /* the guard caught the trap on (almost) every RNTI (review minor 4) */
  EXPECT_EQ(r.crc_accepts + r.geom_pins, 0);
}
TEST(TdSimBc9, CertifiedPCFiresAndIsNeverWrongWhenNeighboursAreIncompatible)
{
  SimCfg c = bc9_cfg(); c.persist = 0.0; c.acq = 30;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_EQ(r.crc_wrong, 0);
  EXPECT_GT(r.crc_accepts + r.geom_pins, 0); /* the fast path actually fires */
  EXPECT_GT(r.cert_fed, 0);
}
TEST(TdSimBc9, CertifiedPCNeverWrongUnderPhysicalTrapWithTdd)
{
  for (double rho : {0.9, 0.0}) {
    SimCfg c = bc9_cfg(); c.tdd = "DDDSU"; c.persist = rho; c.acq = 30;
    const SimResult r = run_sim(c);
    EXPECT_EQ(r.wrong, 0) << rho;
    EXPECT_EQ(r.wrong_pins, 0) << rho;
    EXPECT_EQ(r.crc_wrong, 0) << rho;
    EXPECT_GT(r.tdd_excl_removed, 0) << rho; /* the exclusion fires */
    if (rho == 0.0) EXPECT_GT(r.crc_accepts + r.geom_pins, 0);
    SimCfg o = c; o.tdd_exclude = 0;
    EXPECT_EQ(run_sim(o).tdd_excl_removed, 0) << rho;
  }
}
TEST(TdSimBc9, ExclusionPredicateIsTheEnginesAndPerHypothesis)
{
  /* UL slot: k0 impossible; S slot (6 DL symbols): only entries ending at symbol <= 5 survive; D slot: unconstrained */
  SimCfg c = SimCfg::defaults(); c.tdd = "DDDSU"; c.tdd_s_dl_symbols = 6;
  SlotTimeline tl(c, 0, 5, 6);
  long s = 0;
  while (tl.dir(s) != 'S') s++;
  nr_td_excl_t e; sim_tdd_excl(tl, s - 1, c, &e); /* DCI slot s-1: k0 = 1 -> the S slot */
  EXPECT_EQ(e.last[1], 5);
  EXPECT_EQ(e.last[0], 13);
  sim_tdd_excl(tl, s, c, &e); /* k0 = 1 -> the U slot */
  EXPECT_EQ(e.last[1], -1);
  nr_pdsch_cfg_hypothesis_t h = {};
  h.k0 = 1; h.tda_start = 2; h.tda_length = 4; /* ends at 5 */
  sim_tdd_excl(tl, s - 1, c, &e);
  EXPECT_TRUE(nr_td_excl_admits(&e, &h));
  h.tda_length = 5; /* ends at 6 > 5 */
  EXPECT_FALSE(nr_td_excl_admits(&e, &h));
}
TEST(TdSimBc9, SlotModelOffKeepsCertifiedTrue)
{
  /* v1: the engine is fed certified = true (byte identity); the counters stay zero */
  SimCfg c = SimCfg::defaults(); c.acq = 3; c.crc_accept = 1; c.geom_pin = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.cert_fed, 0);
  EXPECT_EQ(r.fed_all, 0);
}
TEST(TdSimBc9, CertifiedEvidenceAloneStopsTheWrongPinsOfTheGuardOffArm)
{
  /* same arm as FailOpenAlwaysFires...: lever P, sibling guard OFF, persistence 1, adjacency 1 (the neighbour always carries the compatible TB). With
   * certified = true the trap pins wrong geometries (wrong_pins > 0); with the observed-DCI certified flag the compatible neighbour is never certified, so
   * the shifted-slot trap passes are not counted and no wrong pin occurs. */
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 8; c.oracle = 0; c.rntis_per_acq = 1; c.geom_pin = 1; c.cap_s = 300; c.slot_model = 1;
  c.adjacency = 1.0; c.persist = 1.0; c.sib_pmin = 0; c.fo_alpha = 0.3; c.fo_pmin = 0.5;
  c.cert_evidence = 0;
  EXPECT_GT(run_sim(c).wrong_pins, 0);
  c.cert_evidence = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_EQ(r.wrong, 0);
}

/* ---- BC9d: hard TDD exclusion only from CONFIRMED DCIs (a genuine grant whose main decode passed CRC) ---- */
static SimCfg bc9d_cfg()
{
  SimCfg c = bc9_cfg();
  c.tdd = "DDDSU"; c.dci_false = 1e-2; c.acq = 20; c.cap_s = 300;
  return c;
}
TEST(TdSimBc9d, SpuriousDciHazardWithUnconfirmedExclusion)
{
  /* the pre-BC9d runtime: every accepted DCI 1_1 of the row excluded at accept time, spurious ones included -> the truth is pruned and the RNTI
   * ends wrong or undecidable */
  SimCfg c = bc9d_cfg(); c.excl_unconfirmed = 1;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.spur_excl_dcis, 0);
  EXPECT_GT(r.truth_excluded, 0);
  EXPECT_GT(r.wrong + r.undecidable, 0);
}
TEST(TdSimBc9d, ConfirmedExclusionNeverPrunesTruth)
{
  SimCfg c = bc9d_cfg(); c.cap_s = 3600; /* the default cap: a capped RNTI is slow, not lost (the 300 s cap of the hazard test censors ~1/3) */
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.spur_excl_dcis, 0);
  EXPECT_EQ(r.truth_excluded, 0);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_GT(r.confirmed_dcis, 0);
  EXPECT_GT(r.tdd_excl_removed, 0); /* the confirmed exclusion still fires */
  SimCfg b = c; b.dci_false = 0; /* baseline: no spurious DCI at all */
  const SimResult rb = run_sim(b);
  EXPECT_LE(r.undecidable, rb.undecidable);
  EXPECT_EQ(rb.truth_excluded, 0);
}
TEST(TdSimBc9d, IntermediatePersistenceTrapsAndFastPathFires)
{
  /* BC9 sim review I3: rho 0.5 -- the physical shifted-slot trap occurs (identical neighbour allocations) AND certified grants exist.
   * MEASURED (BC9d, seed 1, 40 RNTIs): with the sibling guard on, guard B still blocks the fast path on (almost) every RNTI down to rho 0.3 (it blocks
   * on ANY sibling pass, certified or not: the BC9c question); with the guard off (--sib-pmin 0) the fast path fires on every RNTI and the certified
   * evidence alone keeps it right (wrong 0, wrong_pins 0). Both arms are asserted. */
  SimCfg c = bc9_cfg(); c.persist = 0.5; c.acq = 20;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.trap_grants, 0);
  EXPECT_GT(r.k0_trap_passes, 0);
  EXPECT_GT(r.cert_fed, 0);
  EXPECT_GE((double)r.sib_blocks, 0.95 * (double)r.acquisitions_rntis);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_EQ(r.crc_wrong, 0);
  c.sib_pmin = 0; /* guard off: certified evidence alone */
  const SimResult o = run_sim(c);
  EXPECT_GT(o.k0_trap_passes, 0);
  EXPECT_GT(o.crc_accepts + o.geom_pins, 0); /* the fast path fires */
  EXPECT_EQ(o.wrong, 0);
  EXPECT_EQ(o.wrong_pins, 0);
  EXPECT_EQ(o.crc_wrong, 0);
}
TEST(TdSimBc9d, CertConfirmedRemovesCertifiedWrong)
{
  /* certified_wrong = a missed real compatible occupant + a spurious incompatible one standing in for it. Spurious DCIs are never confirmed, so with
   * --cert-confirmed 1 (ISAC_TD_CERT_CONFIRMED=1) it cannot occur. */
  SimCfg c = bc9_cfg(); c.dci_miss = 0.3; c.dci_false = 0.1; c.acq = 20;
  const SimResult r0 = run_sim(c);
  EXPECT_GT(r0.certified_wrong, 0);
  c.cert_confirmed = 1;
  const SimResult r1 = run_sim(c);
  EXPECT_EQ(r1.certified_wrong, 0);
  EXPECT_LE(r1.certified_grants, r0.certified_grants);
}

/* ---- BC6b: field-book k0 hole, TDD-exclusion merge, pin counting. Flags = the BC6 gate's common flags (gate_bc.json) ---- */
static SimCfg bc6_cfg()
{
  SimCfg c = SimCfg::defaults();
  c.slot_model = 1; c.persist = 0.5; c.dci_miss = 0.01; c.dci_false = 1e-3; c.other_ue_occ = 0.1; c.obs_lastset = 1; c.k0_oracle_legacy = 0;
  c.gate = 1; c.twins = 2; c.K = 1; c.crc_false = 5.96e-8; c.rntis_per_acq = 4; c.sib1 = 1; c.seed = 1;
  return c;
}
TEST(TdSimBc6b, WrongK0OnlyPromotionNeverWrong)
{
  /* a force-promoted TDRA with the truth's S/L/mapping and ANOTHER k0 used to put the truth to sleep (BC6: 840-1980 wrong). Pruning ignores k0 now. */
  SimCfg c = bc6_cfg(); c.acq = 12; c.oracle = 0; c.fieldbook = 2; c.inject_wrong_field = 3; c.persist = 0.9; c.adjacency = 1.0;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.injected, c.acq);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.recovery_never, 0); /* the wrong k0 part is re-learned from the converged winners */
}
TEST(TdSimBc6b, TddExclusionMergeNoWipeStorm)
{
  /* oracle 1 + DDDSU: every k0 = 1 truth probes k0 >= 2 layers and every confirmed DCI of another phase excludes some of them again. Per-DCI
   * exclusions without the runtime's per-row merge wiped the KL evidence 1000-1600 times per RNTI (BC6: every k0 = 1 truth undecidable). */
  SimCfg c = bc6_cfg(); c.acq = 5; c.rntis_per_acq = 1; c.oracle = 1; c.tdd = "DDDSU"; c.truth_k0 = 1; c.n_rx = 4;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.undecidable, 0);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.truth_excluded, 0);
}
TEST(TdSimBc6b, PinCountedOncePerLeverRun)
{
  /* cp_nog DDDSU: a confirmed-DCI exclusion after a pin compacts the GEOM mask; that is not a second pin and not a wrong one (BC6 anomaly 3,
   * acq 191 rank 2 of the 4 RX cell: one real pin on the truth's geometry, then a fake "pin" from the compaction). */
  SimCfg c = bc6_cfg(); c.acq = 200; c.oracle = 0; c.tdd = "DDDSU"; c.crc_accept = 1; c.geom_pin = 1; c.sib_pmin = 0; c.n_rx = 4;
  const SimResult r = run_sim(c);
  EXPECT_GT(r.geom_pins, 0);
  EXPECT_EQ(r.wrong_pins, 0);
  EXPECT_EQ(r.wrong, 0);
  for (const RntiRec &x : r.recs) EXPECT_LE(x.geom_pins, 1) << "acq " << x.acq << " rank " << x.rnti_rank;
}
/* ---- CB0 elimination channel (--cb0-elim; engine nr_pdsch_config_sweep_feed_cb0) ---- */
static SimCfg cb0_gate_cfg()
{
  /* the BC6 gate cell (gate_bc.json common flags), blind */
  SimCfg c = SimCfg::defaults();
  c.slot_model = 1; c.persist = 0.5; c.dci_miss = 0.01; c.dci_false = 1e-3; c.other_ue_occ = 0.1; c.obs_lastset = 1; c.k0_oracle_legacy = 0;
  c.gate = 1; c.twins = 2; c.K = 1; c.crc_false = 5.96e-8; c.sib1 = 1; c.oracle = 0; c.seed = 1;
  return c;
}
TEST(TdSimCb0, OffKnobsAreInert)
{
  SimCfg a = cb0_gate_cfg(); a.acq = 3; a.oracle = 1;
  SimCfg b = a; b.cb0_margin_db = 5; b.cb0_rank_max = 1; /* cb0_elim 0: knobs inert */
  const SimResult x = run_sim(a), y = run_sim(b);
  EXPECT_EQ(x.total_grants, y.total_grants);
  EXPECT_EQ(x.n_full, y.n_full);
  EXPECT_EQ(y.cb0_decodes, 0);
}
TEST(TdSimCb0, Cb0ElimNeverWrongUnderTrap)
{
  /* Strong physical k0 trap: persistent allocation (persist 0.9) and back-to-back grants (adjacency 0.9): k0 siblings genuinely pass CB0
   * (and TB) on compatible grants; MCS-table twins pass on non-separating grants. margin 0 = the worst case CB0 == TB. */
  long trap = 0, decided = 0;
  for (double margin : {0.0, 1.0}) {
    SimCfg c = cb0_gate_cfg(); c.acq = 15; c.persist = 0.9; c.adjacency = 0.9; c.cb0_elim = 1; c.cb0_margin_db = margin;
    const SimResult r = run_sim(c);
    EXPECT_EQ(r.wrong, 0) << margin;
    EXPECT_EQ(r.truth_cb0_elim, 0) << margin;
    EXPECT_GT(r.cb0_elims, 0) << margin;
    trap += r.k0_trap_passes;
    decided += r.n_decided;
  }
  EXPECT_GT(trap, 0); /* the trap was actually exercised */
  EXPECT_GT(decided, 100);
  /* Discriminating arm: inflated false passes (--crc-false 0.1 on TB and CB0 of wrong hypotheses) make a WRONG hypothesis the full-TB
   * leader with a positive lower bound early on; only then can an unsound channel eliminate the truth (a failure-only CB0 count, the P2
   * flaw, does so here: verified by mutation, see the task-ELIM report). */
  SimCfg c = cb0_gate_cfg(); c.acq = 10; c.persist = 0.9; c.adjacency = 0.9; c.cb0_elim = 1; c.crc_false = 0.1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.truth_cb0_elim, 0);
  EXPECT_GT(r.cb0_false_passes, 0);
}
TEST(TdSimCb0, Cb0ElimFasterBlind)
{
  /* oracle 0, 4 RX, first RNTI of each acquisition: >= 10x faster with the channel on; never wrong. */
  double s_on = 0, s_off = 0;
  for (int on = 0; on < 2; on++) {
    SimCfg c = cb0_gate_cfg(); c.acq = 12; c.rntis_per_acq = 1; c.n_rx = 4; c.cb0_elim = on;
    const SimResult r = run_sim(c);
    ASSERT_EQ(r.wrong, 0);
    ASSERT_EQ(r.undecidable, 0);
    double sum = 0;
    for (const RntiRec &x : r.recs) sum += x.seconds;
    (on ? s_on : s_off) = sum / (double)r.recs.size();
    if (on) { EXPECT_GT(r.cb0_decodes, 0); EXPECT_EQ(r.truth_cb0_elim, 0); }
  }
  EXPECT_GE(s_off, 10.0 * s_on) << "first-RNTI mean off " << s_off << " s, on " << s_on << " s";
}
TEST(TdSimCb0, DecoderFallbackKeepsTheChannel)
{
  /* CB0 batch on CUDA (1 dB more sensitive than the CPU full-TB decoder), circuit-breaker fallback to the CPU after 200 grants: under the
   * per-batch dominance rule a CPU batch is still admissible while every TB is CPU-decoded, so nothing is dropped; never wrong. */
  SimCfg c = cb0_gate_cfg(); c.acq = 10; c.persist = 0.9; c.adjacency = 0.9; c.cb0_elim = 1; c.cb0_decoder = 1; c.cb0_fallback_at = 200;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.undecidable, 0);
  EXPECT_EQ(r.truth_cb0_elim, 0);
  EXPECT_EQ(r.cb0_dec_dropped, 0);
}
/* Premise-violation arm (fix round 1): a near-perfect MCS-table twin (v1 model, table exercise 0.05: identical TB and CB0 outcomes on 95 %
 * of grants), CB0 LESS sensitive than the TB (margin -6 dB), soft-combining gain only in the TB of retransmissions that are (wrongly)
 * admitted, rank-SNR correlation, low SNR; the trap-family exemption is off so that only the premise check stands between the truth
 * and elimination by a twin leader. */
static SimCfg premise_arm()
{
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 1; c.oracle = 0; c.n_rx = 4; c.gate = 1; c.rntis_per_acq = 1; c.cap_s = 600;
  c.cb0_elim = 1; c.cb0_admit_all = 1; c.rank_snr_db = 3; c.cb0_no_family = 1; c.cb0_margin_db = -6; c.harq_gain_db = 6; c.mu = 8;
  c.table_exercise = 0.05;
  return c;
}
TEST(TdSimCb0, PremiseCheckStopsTheViolationArm)
{
  SimCfg off = premise_arm(); off.cb0_no_premise = 1;
  const SimResult w = run_sim(off);
  EXPECT_GT(w.wrong, 0) << "the arm must be discriminating (wrong winners without the check)";
  const SimResult r = run_sim(premise_arm());
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.cb0_alarms, 0);
}
TEST(TdSimCb0, SubsetNeverWrong)
{
  for (int B : {64, 128}) {
    SimCfg c = cb0_gate_cfg(); c.acq = 10; c.persist = 0.9; c.adjacency = 0.9; c.cb0_elim = 1; c.cb0_subset = B;
    const SimResult r = run_sim(c);
    EXPECT_EQ(r.wrong, 0) << B;
    EXPECT_EQ(r.truth_cb0_elim, 0) << B;
    EXPECT_LT((double)r.cb0_decodes / (double)std::max(1L, r.cb0_grants), 2.0 * B + 2) << B;
  }
}
