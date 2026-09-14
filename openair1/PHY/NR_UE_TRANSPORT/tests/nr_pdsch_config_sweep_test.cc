#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
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

TEST(PdschConfigSweep, EnumeratesAndStartsUndecided) {
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  EXPECT_GT(n, 8);
  EXPECT_LE(n, NR_PDSCH_SWEEP_MAX_HYP);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, FindsTheTruthAtTheMeasuredWorkingRate) {
  // 76 % is what this rig actually decodes at when the config is right; a wrong hypothesis
  // decodes essentially nothing.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  const int truth = n / 3;
  drive(st, truth, 0.76, 0.0, 400 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), truth);
}

TEST(PdschConfigSweep, FindsTheTruthOnAMarginalLink) {
  // The link itself may be poor; the sweep must still separate 5 % from 0 %.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  const int truth = 3;
  drive(st, truth, 0.05, 0.0, 800 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), truth);
}

TEST(PdschConfigSweep, StaysUndecidedWhenNothingDecodes) {
  // Geometry wrong upstream (bad dci_length/CORESET) -> every hypothesis scores zero. Reporting a
  // winner here would hand the receiver a confident wrong config, which is worse than no answer.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  drive(st, -1, 0.0, 0.0, 600 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, StaysUndecidedWhenTwoHypothesesAreIndistinguishable) {
  // Two hypotheses that the traffic cannot separate must NOT be resolved by a coin flip.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
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
  nr_pdsch_config_sweep_init(&st, 2);
  for (int i = 0; i < 200; i++) {
    nr_pdsch_cfg_hypothesis_t h;
    const int idx = nr_pdsch_config_sweep_next(&st, &h);
    nr_pdsch_config_sweep_feed(&st, idx, idx == 0);
  }
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, PinsTheWinnerOnceDecided) {
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  const int truth = 1;
  drive(st, truth, 0.76, 0.0, 400 * n);
  ASSERT_EQ(nr_pdsch_config_sweep_winner(&st), truth);
  nr_pdsch_cfg_hypothesis_t h;
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(nr_pdsch_config_sweep_next(&st, &h), truth);  // stops rotating
  }
}

TEST(PdschConfigSweep, RejectsBadArguments) {
  EXPECT_EQ(nr_pdsch_config_sweep_init(NULL, 2), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(NULL), -1);
  nr_pdsch_config_sweep_state_t st;
  nr_pdsch_config_sweep_init(&st, 2);
  EXPECT_EQ(nr_pdsch_config_sweep_feed(&st, -1, true), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_feed(&st, 9999, true), -1);
}


static int32_t test_legal(int, int length, int start, int, int add, int maxlen)
{
  return 1 + start * 1000 + length * 40 + add * 3 + maxlen;
}
static nr_pdsch_sweep_ticket_t select_context(uint64_t config, uint16_t rnti, uint8_t tda)
{
  nr_pdsch_sweep_ticket_t ticket{};
  nr_pdsch_cfg_hypothesis_t h{};
  EXPECT_TRUE(nr_pdsch_config_sweep_select(config,rnti,tda,2,0,test_legal,&ticket,&h));
  return ticket;
}
TEST(PdschConfigSweep, PoolsRntiButSeparatesTdaAndConfiguration) {
  nr_pdsch_config_sweep_reset_all();
  const auto a=select_context(1,0x4601,0), b=select_context(1,0x4602,0),
             c=select_context(1,0x4601,1), d=select_context(2,0x4601,0);
  // Same configuration+tda, different RNTI -> ONE shared context (evidence pools across UEs).
  EXPECT_EQ(b.context_slot, a.context_slot);
  EXPECT_EQ(b.generation, a.generation);
  // Different tda, or different configuration -> distinct contexts.
  EXPECT_NE(c.context_slot, a.context_slot);
  EXPECT_NE(d.context_slot, a.context_slot);
}
TEST(PdschConfigSweep, StaleQueuedFeedbackCannotScoreAfterResetOrEviction) {
  nr_pdsch_config_sweep_reset_all();
  const auto old=select_context(1,0x4601,0);
  nr_pdsch_config_sweep_reset_all();
  auto current=select_context(1,0x4601,0);
  nr_pdsch_config_sweep_feedback(&old,true,nullptr);
  nr_pdsch_config_sweep_state_t state{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&current,&state));
  EXPECT_EQ(state.trials[current.hypothesis],0u);
  for(int i=0;i<NR_PDSCH_SWEEP_MAX_CONTEXTS;i++) select_context(100+i,0x4602,0);
  EXPECT_FALSE(nr_pdsch_config_sweep_snapshot(&current,&state));
  EXPECT_FALSE(nr_pdsch_config_sweep_feedback(&current,true,nullptr));
}
TEST(PdschConfigSweep, ConcurrentConsumerFeedbackLosesNoTrials) {
  nr_pdsch_config_sweep_reset_all();
  const auto ticket=select_context(1,0x4601,0);
  std::vector<std::thread> workers;
  for(int i=0;i<6;i++) workers.emplace_back([ticket]{
    for(int j=0;j<10000;j++) nr_pdsch_config_sweep_feedback(&ticket,j%2==0,nullptr);
  });
  for(auto &worker:workers) worker.join();
  nr_pdsch_config_sweep_state_t state{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&ticket,&state));
  EXPECT_EQ(state.trials[ticket.hypothesis],60000u);
  EXPECT_EQ(state.ok[ticket.hypothesis],30000u);
  EXPECT_EQ(state.winner,-1); // other hypotheses received no trials
}
TEST(PdschConfigSweep, IndependentConfigsAndTdasConvergeSeparately) {
  nr_pdsch_config_sweep_reset_all();
  for(int i=0;i<400*NR_PDSCH_SWEEP_MAX_HYP;i++) {
    for(int ctx=0;ctx<3;ctx++) {
      auto ticket=select_context(ctx==1 ? 2 : 1,0x4601,ctx==2 ? 1 : 0);
      nr_pdsch_config_sweep_feedback(&ticket,ticket.hypothesis==ctx+2,nullptr);
    }
  }
  for(int ctx=0;ctx<3;ctx++) {
    auto ticket=select_context(ctx==1 ? 2 : 1,0x4601,ctx==2 ? 1 : 0);
    nr_pdsch_config_sweep_state_t state{};
    ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&ticket,&state));
    EXPECT_EQ(state.winner,ctx+2);
  }
}
TEST(PdschConfigSweep, InvalidTicketAndUnavailableContextCannotScore) {
  nr_pdsch_sweep_ticket_t ticket{};
  EXPECT_FALSE(nr_pdsch_config_sweep_feedback(&ticket,true,nullptr));
  nr_pdsch_cfg_hypothesis_t h{};
  EXPECT_FALSE(nr_pdsch_config_sweep_select(1,0x4601,2,2,0,test_legal,&ticket,&h));
  EXPECT_EQ(ticket.generation,0u);
  EXPECT_FALSE(nr_pdsch_config_sweep_select(1,0x4601,0,2,0,nullptr,&ticket,&h));
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/* Recovery fixtures use three independently identifiable MCS-table hypotheses.
 * CRC outcomes model a stationary link or an explicit change, not a layout hint. */
static int32_t recovery_legal(int, int length, int start, int, int add, int maxlen)
{
  return start==1 && length==13 && add==0 && maxlen==1 ? 4 : 0;
}
static nr_pdsch_sweep_ticket_t recovery_select(uint64_t config=800, uint16_t rnti=0x4601)
{
  nr_pdsch_sweep_ticket_t ticket{};
  nr_pdsch_cfg_hypothesis_t hypothesis{};
  EXPECT_TRUE(nr_pdsch_config_sweep_select(config,rnti,0,0,0,recovery_legal,&ticket,&hypothesis));
  return ticket;
}
static nr_pdsch_sweep_ticket_t recovery_settle(int truth, int success_period=1,
                                               uint64_t config=800, uint16_t rnti=0x4601)
{
  unsigned truth_trials=0;
  for (int i=0;i<12000;++i) {
    auto ticket=recovery_select(config,rnti);
    if (ticket.settled) return ticket;
    const bool pass=ticket.hypothesis==truth && truth_trials++%success_period==0;
    nr_pdsch_config_sweep_feedback(&ticket,pass,nullptr);
  }
  ADD_FAILURE() << "fixture did not converge";
  return {};
}
static nr_pdsch_sweep_report_t last_recovery_report{};
static void recovery_report(const nr_pdsch_sweep_report_t *r)
{
  if (r->invalidated) last_recovery_report=*r;
}
struct PdschRecovery : testing::Test {
  void SetUp() override {
    nr_pdsch_config_sweep_reset_all();
    ASSERT_TRUE(nr_pdsch_config_sweep_set_recovery_policy(32,1e-6));
    last_recovery_report={};
    nr_pdsch_config_sweep_set_reporter(recovery_report);
  }
  void TearDown() override {
    nr_pdsch_config_sweep_set_reporter(nullptr);
    nr_pdsch_config_sweep_set_recovery_policy(32,1e-6);
    nr_pdsch_config_sweep_reset_all();
  }
};
TEST_F(PdschRecovery, SustainedLossReopensOnlyAffectedContextAndRejectsStaleFeedback) {
  const auto old=recovery_settle(0), other=recovery_settle(1,1,801,0x4602);
  ASSERT_TRUE(old.settled && other.settled);
  unsigned failures=0;
  for (;failures<5000 && nr_pdsch_config_sweep_is_settled(800,0x4601,0,0);++failures) {
    auto t=recovery_select();
    nr_pdsch_config_sweep_feedback(&t,false,nullptr);
  }
  ASSERT_LT(failures,5000u);
  EXPECT_GE(failures,32u);
  EXPECT_TRUE(last_recovery_report.invalidated);
  EXPECT_EQ(last_recovery_report.previous_generation,old.generation);
  EXPECT_EQ(last_recovery_report.failure_streak,failures);
  EXPECT_EQ(last_recovery_report.reacquisitions,1u);
  EXPECT_GT(last_recovery_report.reference_crc_lower,0);
  EXPECT_FALSE(nr_pdsch_config_sweep_is_settled(800,0x4601,0,0));
  EXPECT_TRUE(nr_pdsch_config_sweep_is_settled(801,0x4602,0,0));
  const auto fresh=recovery_select();
  EXPECT_NE(fresh.generation,old.generation);
  EXPECT_FALSE(fresh.settled);
  EXPECT_FALSE(nr_pdsch_config_sweep_feedback(&old,true,nullptr));
  nr_pdsch_config_sweep_state_t state{};
  EXPECT_FALSE(nr_pdsch_config_sweep_snapshot(&old,&state));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&fresh,&state));
  for(int i=0;i<state.n_hyp;++i) { EXPECT_EQ(state.trials[i],0u); EXPECT_EQ(state.ok[i],0u); }
  const auto changed=recovery_settle(2);
  EXPECT_TRUE(changed.settled);
  EXPECT_EQ(changed.hypothesis,2);
  EXPECT_EQ(changed.generation,fresh.generation);
}
TEST_F(PdschRecovery, IntermittentFailuresDoNotResetAWorkingContext) {
  const auto initial=recovery_settle(0);
  for(int i=0;i<300;++i) {
    auto t=recovery_select();
    nr_pdsch_config_sweep_feedback(&t,i%3!=0,nullptr);
  }
  auto final=recovery_select();
  EXPECT_TRUE(final.settled);
  EXPECT_EQ(final.generation,initial.generation);
  EXPECT_FALSE(last_recovery_report.invalidated);
}
TEST_F(PdschRecovery, MarginalLinkIsNotJudgedAgainstAHighRateAssumption) {
  const auto initial=recovery_settle(0,20);
  ASSERT_TRUE(initial.settled);
  for(int i=0;i<64;++i) {
    auto t=recovery_select();
    nr_pdsch_config_sweep_feedback(&t,false,nullptr);
  }
  auto final=recovery_select();
  EXPECT_TRUE(final.settled);
  EXPECT_EQ(final.generation,initial.generation);
  EXPECT_FALSE(last_recovery_report.invalidated);
}
TEST_F(PdschRecovery, QueuedExplorationFailuresAreNotOperationalLossEvidence) {
  std::vector<nr_pdsch_sweep_ticket_t> probes;
  for(int i=0;i<100;++i) probes.push_back(recovery_select());
  const auto initial=recovery_settle(0);
  ASSERT_TRUE(initial.settled);
  for(const auto &t:probes) nr_pdsch_config_sweep_feedback(&t,false,nullptr);
  auto final=recovery_select();
  EXPECT_TRUE(final.settled);
  EXPECT_EQ(final.generation,initial.generation);
  EXPECT_FALSE(last_recovery_report.invalidated);
}
TEST_F(PdschRecovery, PolicyRejectsInvalidValuesWithoutDisablingRecovery) {
  EXPECT_FALSE(nr_pdsch_config_sweep_set_recovery_policy(0,1e-6));
  EXPECT_FALSE(nr_pdsch_config_sweep_set_recovery_policy(32,0));
  EXPECT_FALSE(nr_pdsch_config_sweep_set_recovery_policy(32,1));
  EXPECT_FALSE(nr_pdsch_config_sweep_set_recovery_policy(32,-0.1));
  EXPECT_TRUE(nr_pdsch_config_sweep_set_recovery_policy(64,1e-7));
}


// ---- Cell-wide prior -------------------------------------------------------------------------
// Only (S,L) belongs to a TDRA entry; dmrs_add_pos, dmrs_max_len and mcs_table are cell-wide.
// These pin the behaviour measured OTA 2026-09-13, where tda=0 converged on S=1 L=13 while tda=1
// of the SAME cell still had 61 trials on its leader after 7805 outcomes.

static bool same_hyp(const nr_pdsch_cfg_hypothesis_t &a, const nr_pdsch_cfg_hypothesis_t &b)
{
  return a.tda_start == b.tda_start && a.tda_length == b.tda_length
         && a.dmrs_add_pos == b.dmrs_add_pos && a.dmrs_max_len == b.dmrs_max_len
         && a.mcs_table == b.mcs_table;
}

// Drive one context through the real select/feedback path. Returns the outcome count at which it
// announced a winner, or -1 if it never did.
static int drive_context(uint64_t config, uint16_t rnti, uint8_t tda,
                         const nr_pdsch_cfg_hypothesis_t &truth, double p_true,
                         int max_outcomes, nr_pdsch_cfg_hypothesis_t *won)
{
  unsigned seed = 4242;
  for (int i = 1; i <= max_outcomes; i++) {
    nr_pdsch_sweep_ticket_t ticket{};
    nr_pdsch_cfg_hypothesis_t h{};
    if (!nr_pdsch_config_sweep_select(config, rnti, tda, 2, 0, test_legal, &ticket, &h))
      return -1;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    if (nr_pdsch_config_sweep_feedback(&ticket, same_hyp(h, truth) && u < p_true, won))
      return i;
  }
  return -1;
}

TEST(PdschConfigSweepPrior, PruneKeepsOnlyMatchingAndClearsEvidence) {
  nr_pdsch_config_sweep_state_t st;
  const int full = nr_pdsch_config_sweep_init_legal(&st, 2, 0, test_legal);
  ASSERT_GT(full, 8);
  drive(st, 0, 0.8, 0.0, 500);  // lay down evidence that must NOT survive re-indexing
  const int n = nr_pdsch_config_sweep_prune_to(&st, 1, 1, 1);
  ASSERT_GT(n, 0);
  EXPECT_LT(n, full);
  EXPECT_EQ(st.n_hyp, n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
  for (int i = 0; i < st.n_hyp; i++) {
    EXPECT_EQ(st.hyp[i].mcs_table, 1);
    EXPECT_EQ(st.hyp[i].dmrs_add_pos, 1);
    EXPECT_EQ(st.hyp[i].dmrs_max_len, 1);
    EXPECT_EQ(st.trials[i], 0u);   // stale evidence would be attributed to the wrong hypothesis
    EXPECT_EQ(st.ok[i], 0u);
  }
}

TEST(PdschConfigSweepPrior, PruneMatchingNothingLeavesTheCatalogUsable) {
  nr_pdsch_config_sweep_state_t st;
  const int full = nr_pdsch_config_sweep_init_legal(&st, 2, 0, test_legal);
  // max_len 7 exists in no catalog entry. An emptied context could never converge, so the
  // catalog must be left alone instead.
  EXPECT_EQ(nr_pdsch_config_sweep_prune_to(&st, 1, 1, 7), 0);
  EXPECT_EQ(st.n_hyp, full);
}

TEST(PdschConfigSweepPrior, SiblingTdaContextConvergesFarSooner) {
  const nr_pdsch_cfg_hypothesis_t truth0{1, 13, 1, 1, 0, 1};  // S=1 L=13, the OTA-measured winner
  const nr_pdsch_cfg_hypothesis_t truth1{1,  7, 1, 1, 0, 1};  // same cell fields, other TDRA entry

  // Baseline: no prior available to the second context.
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_cfg_hypothesis_t w{};
  const int alone = drive_context(7, 0x4601, 1, truth1, 0.54, 400000, &w);
  ASSERT_GT(alone, 0);

  // With a sibling context having already converged and published the cell-wide fields.
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  ASSERT_GT(drive_context(7, 0x4601, 0, truth0, 0.54, 400000, &w), 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_prior_get(nullptr, nullptr, nullptr, nullptr));
  nr_pdsch_cfg_hypothesis_t w1{};
  const int primed = drive_context(7, 0x4602, 1, truth1, 0.54, 400000, &w1);
  ASSERT_GT(primed, 0);
  EXPECT_TRUE(same_hyp(w1, truth1));      // still the RIGHT answer, not merely a faster one
  EXPECT_LT(primed * 4, alone);           // and materially cheaper (catalog ~24x narrower)
  std::cerr << "[ MEASURED ] outcomes to converge: alone=" << alone
            << "  with_prior=" << primed
            << "  speedup=" << ((double)alone / (double)primed) << "x" << std::endl;
}

TEST(PdschConfigSweepPrior, AWrongPriorIsAbandonedAndTheTruthIsStillFound) {
  // Publish a prior from a cell whose mcs_table is 1 ...
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  const nr_pdsch_cfg_hypothesis_t truth0{1, 13, 1, 1, 0, 1};
  nr_pdsch_cfg_hypothesis_t w{};
  ASSERT_GT(drive_context(9, 0x4601, 0, truth0, 0.54, 400000, &w), 0);
  uint8_t tbl = 255;
  ASSERT_TRUE(nr_pdsch_config_sweep_prior_get(nullptr, &tbl, nullptr, nullptr));
  ASSERT_EQ(tbl, 1);

  // ... then give a sibling context a truth the prior EXCLUDES (mcs_table 0). The pruned catalog
  // cannot contain it, so the context must detect that and restore the full search.
  const nr_pdsch_cfg_hypothesis_t truth1{2, 12, 1, 1, 0, 0};
  nr_pdsch_cfg_hypothesis_t w1{};
  const int n = drive_context(9, 0x4602, 1, truth1, 0.54, 400000, &w1);
  ASSERT_GT(n, 0) << "a wrong prior trapped the context: it never converged";
  EXPECT_TRUE(same_hyp(w1, truth1));
}
