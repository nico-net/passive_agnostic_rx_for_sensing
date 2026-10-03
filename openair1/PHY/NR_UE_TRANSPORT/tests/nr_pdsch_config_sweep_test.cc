#include <cstdlib>
#include <cmath>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_config_sweep.h"
#include "nr_td_order.h"
#include "nr_td_legal.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}

extern "C" {
configmodule_interface_t *uniqCfg=nullptr;
void exit_function(const char *file,const char *fn,int line,const char *message,int fatal) {
  if(message) fprintf(stderr,"%s:%d %s: %s\n",file,line,fn,message);
  if(fatal) abort();
  exit(EXIT_SUCCESS);
}
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

/* The two StaysUndecided tests are O(n^2) in the catalog (the post-MIN_TRIALS fallback scans every
 * hypothesis per feed): they run on the type-A-only legal catalog (2016), which is what they were sized
 * for; the 6336-entry pure A+B catalog made each take ~117 s. */
static int32_t test_legal(int, int length, int start, int mapping_b, int add, int maxlen);

TEST(PdschConfigSweep, StaysUndecidedWhenNothingDecodes) {
  // Geometry wrong upstream (bad dci_length/CORESET) -> every hypothesis scores zero. Reporting a
  // winner here would hand the receiver a confident wrong config, which is worse than no answer.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init_legal(&st, 2, 0, test_legal);
  drive(st, -1, 0.0, 0.0, 600 * n);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(&st), -1);
}

TEST(PdschConfigSweep, StaysUndecidedWhenTwoHypothesesAreIndistinguishable) {
  // Two hypotheses that the traffic cannot separate must NOT be resolved by a coin flip.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init_legal(&st, 2, 0, test_legal);
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


/* Models a cell whose legality admits mapping type A only -- the catalog every test below was sized
 * against (mapping type was not an input before Task 14). Admitting type B here as well grows these
 * catalogs 2016 -> 4368 and unaided convergence 223,838 -> 484,911 outcomes (measured), past their
 * 400,000 budgets; type B is exercised by the PdschConfigSweepTypeB tests with their own fixture. */
static int32_t test_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  return mapping_b ? 0 : 1 + start * 1000 + length * 40 + add * 3 + maxlen;
}
static nr_pdsch_sweep_ticket_t select_context(uint64_t config, uint16_t rnti, uint8_t tda)
{
  nr_pdsch_sweep_ticket_t ticket{};
  nr_pdsch_cfg_hypothesis_t h{};
  EXPECT_TRUE(nr_pdsch_config_sweep_select(config,rnti,tda,2,0,test_legal,&ticket,&h));
  return ticket;
}
TEST(PdschConfigSweep, SeparatesRntiTdaAndConfiguration) {
  nr_pdsch_config_sweep_reset_all();
  const auto a=select_context(1,0x4601,0), b=select_context(1,0x4602,0),
             c=select_context(1,0x4601,1), d=select_context(2,0x4601,0), e=select_context(1,0x4601,0);
  // Different RNTI, tda, or configuration -> distinct contexts; the same key -> the same one.
  EXPECT_NE(b.context_slot, a.context_slot);
  EXPECT_NE(c.context_slot, a.context_slot);
  EXPECT_NE(d.context_slot, a.context_slot);
  EXPECT_EQ(e.context_slot, a.context_slot);
  EXPECT_EQ(e.generation, a.generation);
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
  logInit();
  int rc=RUN_ALL_TESTS();
  logClean();
  return rc;
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
    nr_pdsch_config_sweep_k0_legacy_set(-1);
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
  return a.tda_start == b.tda_start && a.tda_length == b.tda_length && a.k0 == b.k0
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
  const nr_pdsch_cfg_hypothesis_t truth0{1, 13, 0, 1, 1, 0, 1};  // S=1 L=13, the OTA-measured winner
  const nr_pdsch_cfg_hypothesis_t truth1{1, 7, 0, 1, 1, 0, 1};  // same cell fields, other TDRA entry

  // Baseline: no prior available to the second context.
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_cfg_hypothesis_t w{};
  const int alone = drive_context(7, 0x4601, 1, truth1, 0.54, 400000, &w);
  ASSERT_GT(alone, 0);

  // With a sibling context OF THE SAME UE having already converged and published its fields.
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  ASSERT_GT(drive_context(7, 0x4601, 0, truth0, 0.54, 400000, &w), 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_rnti_prior_get(0x4601, nullptr, nullptr, nullptr, nullptr));
  ASSERT_FALSE(nr_pdsch_config_sweep_prior_get(nullptr, nullptr, nullptr, nullptr)); // one UE is no cell evidence
  nr_pdsch_cfg_hypothesis_t w1{};
  const int primed = drive_context(7, 0x4601, 1, truth1, 0.54, 400000, &w1);
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
  const nr_pdsch_cfg_hypothesis_t truth0{1, 13, 0, 1, 1, 0, 1};
  nr_pdsch_cfg_hypothesis_t w{};
  ASSERT_GT(drive_context(9, 0x4601, 0, truth0, 0.54, 400000, &w), 0);
  uint8_t tbl = 255;
  ASSERT_TRUE(nr_pdsch_config_sweep_rnti_prior_get(0x4601, nullptr, &tbl, nullptr, nullptr));
  ASSERT_EQ(tbl, 1);

  // ... then give a sibling context of the same UE a truth the prior EXCLUDES (mcs_table 0). The
  // pruned catalog cannot contain it, so the context must detect that and restore the full search.
  const nr_pdsch_cfg_hypothesis_t truth1{2, 12, 0, 1, 1, 0, 0};
  nr_pdsch_cfg_hypothesis_t w1{};
  const int n = drive_context(9, 0x4601, 1, truth1, 0.54, 400000, &w1);
  ASSERT_GT(n, 0) << "a wrong prior trapped the context: it never converged";
  EXPECT_TRUE(same_hyp(w1, truth1));
  // The wrong prior was dropped at probation, and the full-catalog convergence then re-published
  // the UE's prior from what it actually found -- exactly as the cell-wide code did before.
  tbl = 255;
  ASSERT_TRUE(nr_pdsch_config_sweep_rnti_prior_get(0x4601, nullptr, &tbl, nullptr, nullptr));
  EXPECT_EQ(tbl, 0);
}

// MEASURED OTA 2026-09-25 (lab cell): pure LRU-by-touch on the RNTI evidence cache evicted the ONE
// real, continuously-scheduled RNTI 5 times in a 200s run, wiping its prior each time, because a
// flood of one-off noise-floor RNTIs (blind PDCCH false-accepts) touched the table between the real
// RNTI's own grants. The fix protects any evidence-bearing slot from a zero-evidence one regardless
// of recency; this reproduces the failure shape (far more distinct noise RNTIs than the cache has
// slots) and asserts the real RNTI's evidence survives it.
TEST(PdschConfigSweepRntiCache, EvictionProtectsEvidenceFromNoiseChurn) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  const nr_pdsch_cfg_hypothesis_t truth{1, 13, 0, 1, 1, 0, 1};
  nr_pdsch_cfg_hypothesis_t w{};
  ASSERT_GT(drive_context(21, 0x4601, 0, truth, 0.54, 400000, &w), 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_rnti_prior_get(0x4601, nullptr, nullptr, nullptr, nullptr));

  // A flood of distinct one-off RNTIs, each touched exactly once, far exceeding RNTI_CTX_MAX (64).
  for (uint16_t noise = 0; noise < 500; noise++)
    select_context(21, (uint16_t)(0x1000 + noise), 0);

  EXPECT_TRUE(nr_pdsch_config_sweep_rnti_prior_get(0x4601, nullptr, nullptr, nullptr, nullptr))
      << "evidence-bearing RNTI context was evicted by zero-evidence noise churn";
}

/* ---- Per-RNTI contexts ---------------------------------------------------------------------------
 * Priors and DM-RS observations are per-UE in the spec. Each RNTI keeps its own, seeded from the
 * cell-wide ones, which exist only once two distinct RNTIs agree. */

// Drive RNTI a alone, then a interleaved with a second RNTI b that has its own truth on the same
// configuration. Returns a's outcome count to convergence and the first 300 hypothesis indices it was
// offered; b must not perturb either. (bit-identity of the single-RNTI path: b's contexts, prior and
// observations are invisible to a until two RNTIs agree, and a alone can never make two.)
static int drive_a_with_b(uint64_t config, uint16_t a, uint16_t b,
                          const nr_pdsch_cfg_hypothesis_t &truth_a, const nr_pdsch_cfg_hypothesis_t &truth_b,
                          std::vector<int> *seq_a, nr_pdsch_cfg_hypothesis_t *won_a)
{
  unsigned seed_a = 4242, seed_b = 99;
  int converged = -1;
  for (int i = 1; i <= 400000 && converged < 0; i++) {
    nr_pdsch_sweep_ticket_t t{};
    nr_pdsch_cfg_hypothesis_t h{};
    if (!nr_pdsch_config_sweep_select(config, a, 0, 2, 0, test_legal, &t, &h))
      return -1;
    if ((int)seq_a->size() < 300)
      seq_a->push_back(t.hypothesis);
    const double u = (double)rand_r(&seed_a) / (double)RAND_MAX;
    if (nr_pdsch_config_sweep_feedback(&t, same_hyp(h, truth_a) && u < 0.54, won_a))
      converged = i;
    if (b) {
      nr_pdsch_sweep_ticket_t tb{};
      nr_pdsch_cfg_hypothesis_t hb{};
      if (!nr_pdsch_config_sweep_select(config, b, 0, 2, 0, test_legal, &tb, &hb))
        return -1;
      const double ub = (double)rand_r(&seed_b) / (double)RAND_MAX;
      nr_pdsch_config_sweep_feedback(&tb, same_hyp(hb, truth_b) && ub < 0.54, nullptr);
    }
  }
  return converged;
}

TEST(PdschConfigSweepPerRnti, ASecondRntiWithADifferentConfigCannotPerturbTheFirst) {
  const nr_pdsch_cfg_hypothesis_t truth_a{1, 13, 0, 1, 1, 0, 1};
  const nr_pdsch_cfg_hypothesis_t truth_b{2, 12, 0, 0, 1, 0, 0}; // same cell, a UE with other dedicated fields
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  std::vector<int> alone, mixed;
  nr_pdsch_cfg_hypothesis_t w_alone{}, w_mixed{};
  const int n_alone = drive_a_with_b(5, 0x4601, 0, truth_a, truth_b, &alone, &w_alone);
  ASSERT_GT(n_alone, 0);
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  const int n_mixed = drive_a_with_b(5, 0x4601, 0x4602, truth_a, truth_b, &mixed, &w_mixed);
  ASSERT_GT(n_mixed, 0);
  EXPECT_EQ(n_mixed, n_alone);   // same outcome count to convergence
  EXPECT_EQ(mixed, alone);       // same hypothesis sequence offered to a
  EXPECT_TRUE(same_hyp(w_mixed, w_alone));
  EXPECT_TRUE(same_hyp(w_alone, truth_a));
  EXPECT_FALSE(nr_pdsch_config_sweep_prior_get(nullptr, nullptr, nullptr, nullptr)); // they disagree: no cell prior
}

TEST(PdschConfigSweepPerRnti, CellPriorNeedsTwoAgreeingRntisAndThenSeedsAThird) {
  const nr_pdsch_cfg_hypothesis_t truth{1, 13, 0, 1, 1, 0, 1};
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_cfg_hypothesis_t w{};
  const int first = drive_context(11, 0x4601, 0, truth, 0.54, 400000, &w);
  ASSERT_GT(first, 0);
  EXPECT_FALSE(nr_pdsch_config_sweep_prior_get(nullptr, nullptr, nullptr, nullptr));
  // The second RNTI pays the full search too -- one UE's fields were not yet cell evidence ...
  const int second = drive_context(11, 0x4602, 0, truth, 0.54, 400000, &w);
  ASSERT_GT(second, 0);
  EXPECT_GT(second * 4, first);
  uint8_t tbl = 255;
  ASSERT_TRUE(nr_pdsch_config_sweep_prior_get(nullptr, &tbl, nullptr, nullptr)); // ... two agreeing ones are
  EXPECT_EQ(tbl, 1);
  // ... and a third RNTI starts pruned to them.
  const int third = drive_context(11, 0x4603, 0, truth, 0.54, 400000, &w);
  ASSERT_GT(third, 0);
  EXPECT_LT(third * 4, first);
  EXPECT_TRUE(same_hyp(w, truth));
}

TEST(PdschConfigSweepPerRnti, ObservedMaskIsPrivateUntilASecondRntiSeesIt) {
  const uint16_t mask = (uint16_t)test_legal(0, 13, 1, 0, 2, 1);
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  nr_pdsch_config_sweep_state_t st{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(3, 0x4601, 0, 2, 0, test_legal, &t, &h));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  const int full = st.n_hyp;
  ASSERT_GT(nr_pdsch_config_sweep_observe_mask(&t, mask), 0);
  // Another RNTI's new context is untouched by 0x4601's private observation ...
  ASSERT_TRUE(nr_pdsch_config_sweep_select(3, 0x4602, 0, 2, 0, test_legal, &t, &h));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  EXPECT_EQ(st.n_hyp, full);
  // ... until it observes the same mask itself, which promotes it cell-wide ...
  ASSERT_GT(nr_pdsch_config_sweep_observe_mask(&t, mask), 0);
  // ... so a third RNTI starts pruned to it.
  ASSERT_TRUE(nr_pdsch_config_sweep_select(3, 0x4603, 1, 2, 0, test_legal, &t, &h));
  EXPECT_EQ(h.dmrs_mask, mask);
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  EXPECT_LT(st.n_hyp, full);
}

TEST(PdschConfigSweepQm, PruneQmKeepsOnlyTablesPredictingTheMeasuredOrder) {
  nr_pdsch_config_sweep_state_t st;
  ASSERT_GT(nr_pdsch_config_sweep_init(&st, 2), 0);
  // MCS 20: table 0 -> 64QAM, table 1 -> 256QAM, table 2 -> 16QAM. Measured 256QAM => table 1 only.
  const int n = nr_pdsch_config_sweep_prune_qm(&st, 20, 8);
  ASSERT_GT(n, 0);
  for (int i = 0; i < st.n_hyp; i++) EXPECT_EQ(st.hyp[i].mcs_table, 1);
}

TEST(PdschConfigSweepQm, UninformativeMcsLeavesTheCatalogAlone) {
  nr_pdsch_config_sweep_state_t st;
  const int full = nr_pdsch_config_sweep_init(&st, 2);
  EXPECT_EQ(nr_pdsch_config_sweep_prune_qm(&st, 2, 2), full);  // MCS 2 is QPSK in every table
  EXPECT_EQ(st.n_hyp, full);
}

TEST(PdschConfigSweepQm, LiveObservationPrunesOnlyAfterTwoAgreeingSightings) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  auto t = select_context(31, 0x4601, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // first sighting: evidence only
  EXPECT_GT(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // second agreeing: pruned
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // nothing left to remove: no reset
}

TEST(PdschConfigSweepQm, ConflictingObservationsResetInsteadOfPruning) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  auto t = select_context(32, 0x4602, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // implies table 1
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 6), 0);   // implies table 0: conflict -> reset
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 6), 0);   // first sighting after reset
}

/* OTA 2026-09-16: the oracle measured DM-RS at {2,7,11} on every slot, yet every decode used the
 * add_pos-0 mask, because the layout rotation churned contexts faster than any walked past the
 * catalog head. An observed mask must prune every context created AFTER the observation. */
TEST(PdschConfigSweepOracle, ObservedMaskPrunesContextsCreatedLater) {
  const nr_pdsch_cfg_hypothesis_t truth{1, 13, 0, 2, 1, 0, 1};  // S=1 L=13 add_pos 2
  const uint16_t truth_mask = (uint16_t)test_legal(0, 13, 1, 0, 2, 1);
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_cfg_hypothesis_t w0{};
  const int alone = drive_context(1, 0x4601, 1, truth, 0.54, 400000, &w0);  // no observation
  ASSERT_GT(alone, 0);
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t t0{};
  nr_pdsch_cfg_hypothesis_t h0{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(1, 0x4601, 0, 2, 0, test_legal, &t0, &h0));
  ASSERT_GT(nr_pdsch_config_sweep_observe_mask(&t0, truth_mask), 0);
  // A context created later, for another TDA index, starts already pruned to that mask.
  nr_pdsch_sweep_ticket_t t1{};
  nr_pdsch_cfg_hypothesis_t h1{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(1, 0x4601, 1, 2, 0, test_legal, &t1, &h1));
  EXPECT_EQ(h1.dmrs_mask, truth_mask);
  EXPECT_EQ(h1.dmrs_add_pos, 2);
  nr_pdsch_cfg_hypothesis_t w{};
  const int n = drive_context(1, 0x4601, 1, truth, 0.54, 400000, &w);
  ASSERT_GT(n, 0);
  EXPECT_TRUE(same_hyp(w, truth));
  EXPECT_LT(n * 4, alone);  // mcs_table is all that is left to the CRC
}

static int32_t long_short_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  if (mapping_b || start != 2 || add != 2 || maxlen != 1)
    return -1;
  return length == 12 ? 0x884 : length == 6 ? 0x84 : -1;
}

TEST(PdschConfigSweepOracle, LaterShortTdaObservationRestoresCandidatesPrunedByEarlierLongTda) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(79, 0x4601, 0, 2, 0, long_short_legal, &t, &h));
  ASSERT_EQ(nr_pdsch_config_sweep_observe(&t, 0x884, 13, 0), 6); // K39: k0 {0,1} both survive
  ASSERT_TRUE(nr_pdsch_config_sweep_select(79, 0x4601, 1, 2, 0, long_short_legal, &t, &h));
  EXPECT_EQ(h.dmrs_mask, 0x884); // inherited observation is a seed, not proof that TDA1 is long
  nr_pdsch_config_sweep_feedback(&t, false, nullptr);
  const auto outstanding = t;
  ASSERT_EQ(nr_pdsch_config_sweep_observe(&t, 0x84, 7, 0), 12);
  // Appending does not move the old hypothesis or invalidate its pending feedback.
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  uint32_t passes = 0, trials = 0;
  nr_pdsch_config_sweep_context_stats(79, 0x4601, 1, 0, &passes, &trials);
  EXPECT_EQ(trials, 2u);
  EXPECT_EQ(passes, 0u);
  EXPECT_EQ(nr_pdsch_config_sweep_observe(&outstanding, 0x84, 7, 0), 12); // no duplicate append
  bool short_seen = false, long_seen = false;
  for (int i = 0; i < 12; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(79, 0x4601, 1, 2, 0, long_short_legal, &t, &h));
    EXPECT_EQ(t.generation, outstanding.generation);
    short_seen |= h.tda_start == 2 && h.tda_length == 6 && h.dmrs_mask == 0x84;
    long_seen |= h.tda_start == 2 && h.tda_length == 12 && h.dmrs_mask == 0x884;
  }
  EXPECT_TRUE(short_seen);
  EXPECT_TRUE(long_seen);
}

TEST(PdschConfigSweepOracle, MaskLastSymbolAndK0CollapseAContextToTheEndAmbiguity) {
  // A mask alone leaves every (S,L,k0,mcs) that produces it; the allocation END (last symbol with
  // energy on the grant's PRBs) and the job's k0 are observable in the same FEP. With test_legal's
  // synthetic masks (unique per S,L,add,len) the mask already pins (S,L,add,len); the end and k0
  // observation must then remove the k0 dimension and leave only the 3 MCS tables.
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 2, 0, test_legal, &t, &h));
  const uint16_t mask = (uint16_t)test_legal(0, 13, 1, 0, 2, 1); // S=1 L=13 add 2 len 1
  nr_pdsch_config_sweep_k0_legacy_set(1); // this test documents the OLD pinning (A/B flag)
  const int n = nr_pdsch_config_sweep_observe(&t, mask, 13, 0);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
  EXPECT_EQ(n, 3);
  nr_pdsch_sweep_ticket_t t2{};
  for (int i = 0; i < 12; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 2, 0, test_legal, &t2, &h));
    EXPECT_EQ(h.dmrs_mask, mask);
    EXPECT_EQ(h.tda_start + h.tda_length - 1, 13);
    EXPECT_EQ(h.k0, 0);
  }
}

/* K39: DM-RS presence marks k0 plausible, never pins it. */
static int count_snapshot_k0(const nr_pdsch_sweep_ticket_t &t, int k0, int *total = nullptr)
{
  static nr_pdsch_config_sweep_state_t st;
  EXPECT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  int n = 0;
  for (int i = 0; i < st.n_hyp; i++)
    n += st.hyp[i].k0 == k0;
  if (total) *total = st.n_hyp;
  return n;
}
static nr_pdsch_sweep_ticket_t k39_open(uint16_t *mask_out)
{
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_config_sweep_k0_legacy_set(0); // default behaviour regardless of the environment; tests restore -1
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  EXPECT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 2, 0, test_legal, &t, &h));
  *mask_out = (uint16_t)test_legal(0, 13, 1, 0, 2, 1);
  return t;
}
static nr_pdsch_sweep_ticket_t k39_fresh(uint8_t tda = 0, uint64_t cfg = 0x1234, uint16_t rnti = 0x4601)
{ // a prune re-numbers the context and invalidates outstanding tickets: take a live one
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  EXPECT_TRUE(nr_pdsch_config_sweep_select(cfg, rnti, tda, 2, 0, test_legal, &t, &h));
  return t;
}
/* K39 fixture: the legacy-pinning flag is restored to its environment default after every test. */
struct PdschConfigSweepK39 : testing::Test {
  void TearDown() override { nr_pdsch_config_sweep_k0_legacy_set(-1); }
};
TEST_F(PdschConfigSweepK39, DmrsObservationDoesNotPruneOtherK0) {
  uint16_t mask;
  auto t = k39_open(&mask);
  int total = 0;
  EXPECT_EQ(nr_pdsch_config_sweep_observe(&t, mask, 13, 0), 6);
  t = k39_fresh();
  EXPECT_EQ(count_snapshot_k0(t, 0, &total), 3);
  EXPECT_EQ(count_snapshot_k0(t, 1), 3); // same mask and last symbol, k0 = 1 stays
  EXPECT_EQ(total, 6);
  // A later context of the same RNTI is seeded with both k0 as well.
  nr_pdsch_sweep_ticket_t t2{};
  nr_pdsch_cfg_hypothesis_t h{};
  bool k1 = false;
  for (int i = 0; i < 12; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 1, 2, 0, test_legal, &t2, &h));
    k1 |= h.k0 == 1;
  }
  EXPECT_TRUE(k1);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
}
TEST_F(PdschConfigSweepK39, CertifiedK0Prunes) {
  uint16_t mask;
  auto t = k39_open(&mask);
  ASSERT_EQ(nr_pdsch_config_sweep_observe(&t, mask, 13, 0), 6);
  t = k39_fresh();
  EXPECT_EQ(nr_pdsch_config_sweep_certify_k0(&t, 0x3), 6); // {0,1} certified: nothing to prune
  t = k39_fresh();
  EXPECT_EQ(nr_pdsch_config_sweep_certify_k0(&t, 0x2), 3); // k0 = 1 only
  t = k39_fresh();
  EXPECT_EQ(count_snapshot_k0(t, 0), 0);
  EXPECT_EQ(count_snapshot_k0(t, 1), 3);
  // A contradictory certification never empties the context.
  nr_pdsch_config_sweep_certify_k0(&t, 0x1);
  t = k39_fresh();
  EXPECT_EQ(count_snapshot_k0(t, 1), 3);
  EXPECT_EQ(nr_pdsch_config_sweep_certify_k0(&t, 0), 0);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
}
TEST_F(PdschConfigSweepK39, LegacyFlagRestoresPinning) {
  uint16_t mask;
  auto t = k39_open(&mask);
  nr_pdsch_config_sweep_k0_legacy_set(1);
  EXPECT_EQ(nr_pdsch_config_sweep_observe(&t, mask, 13, 0), 3);
  t = k39_fresh();
  EXPECT_EQ(count_snapshot_k0(t, 1), 0);
  EXPECT_EQ(count_snapshot_k0(t, 0), 3);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
}

static int32_t ab_legal(int, int length, int start, int mapping_b, int add, int maxlen);
static int k39_count(const nr_pdsch_sweep_ticket_t &t, int k0) { return count_snapshot_k0(t, k0); }
TEST_F(PdschConfigSweepK39, CertificationDoesNotLeakAcrossTdaRows) {
  uint16_t mask;
  auto t0 = k39_open(&mask);
  ASSERT_EQ(nr_pdsch_config_sweep_observe(&t0, mask, 13, 0), 6);
  t0 = k39_fresh(0);
  ASSERT_EQ(nr_pdsch_config_sweep_certify_k0(&t0, 0x2), 3); // tda0: k0 = 1 only
  const auto t1 = k39_fresh(1);                              // another TDRA row of the same RNTI
  EXPECT_GT(k39_count(t1, 0), 0);
  EXPECT_GT(k39_count(t1, 1), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_observe(&t1, mask, 13, 0), 6); // its own observation keeps both k0
  const auto t1b = k39_fresh(1);
  EXPECT_EQ(k39_count(t1b, 0), 3);
  EXPECT_EQ(k39_count(t1b, 1), 3);
  // A different configuration of the same RNTI/row is not bound either.
  const auto tc = k39_fresh(0, 0x9999);
  EXPECT_GT(k39_count(tc, 0), 0);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
}
/* M1: the certification binds on every path that could bring another k0 back. */
TEST_F(PdschConfigSweepK39, CertificationBindsOverTypeBLayerAndK0LayerAppends) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_config_sweep_k0_legacy_set(0);
  nr_pdsch_sweep_ticket_t a{}, b{};
  nr_pdsch_cfg_hypothesis_t h{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(77, 0x4602, 0, 2, 0, ab_legal, &b, &h));
  nr_pdsch_config_sweep_observe_mask(&b, 0x20); // a second RNTI shows the type-B-only mask: g_obs carries it
  ASSERT_TRUE(nr_pdsch_config_sweep_select(77, 0x4601, 0, 2, 0, ab_legal, &a, &h));
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&a, 0x1), 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_select(77, 0x4601, 0, 2, 0, ab_legal, &a, &h));
  // the k0 layer (probe path) must not bring in a non-certified k0
  EXPECT_EQ(nr_pdsch_config_sweep_add_k0(&a, 3), 0);
  // the observe path adds the type-B layer (k0 {0,1}); the cert must prune k0 = 1 again
  nr_pdsch_config_sweep_observe_mask(&a, 0x20);
  ASSERT_TRUE(nr_pdsch_config_sweep_select(77, 0x4601, 0, 2, 0, ab_legal, &a, &h));
  EXPECT_EQ(count_snapshot_k0(a, 1), 0);
  EXPECT_GT(count_snapshot_k0(a, 0), 0);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
}
TEST_F(PdschConfigSweepK39, CertificationSurvivesContextEvictionForTheSameKey) {
  uint16_t mask;
  auto t = k39_open(&mask);
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&t, 0x2), 0);
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS + 40; i++)
    k39_fresh(0, 0x7000 + i, (uint16_t)(0x5000 + i % 40)); // evict the (cfg, rnti, tda) context
  const auto again = k39_fresh(0); // recreated: inherits its key's certification
  EXPECT_EQ(k39_count(again, 0), 0);
  EXPECT_GT(k39_count(again, 1), 0);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
}
TEST_F(PdschConfigSweepK39, CertificationSurvivesPriorProbationRestore) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_config_sweep_k0_legacy_set(0);
  const nr_pdsch_cfg_hypothesis_t truth0{1, 13, 0, 1, 1, 0, 1};
  nr_pdsch_cfg_hypothesis_t w{};
  ASSERT_GT(drive_context(9, 0x4601, 0, truth0, 0.54, 400000, &w), 0); // publishes a prior (mcs_table 1)
  auto t = k39_fresh(1, 9);                                          // sibling row seeded by it (pruned catalog)
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&t, 0x2), 0);
  for (int i = 0; i < 2600; i++) {                                   // nothing passes: probation fails, full catalog restored
    t = k39_fresh(1, 9);
    nr_pdsch_config_sweep_feedback(&t, false, nullptr);
  }
  t = k39_fresh(1, 9);
  EXPECT_EQ(k39_count(t, 0), 0);
  EXPECT_GT(k39_count(t, 1), 0);
}
TEST_F(PdschConfigSweepK39, CertifiedK0LayerAppendIsBoundByTheCertification) {
  uint16_t mask;
  auto t = k39_open(&mask);
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&t, UINT64_C(1) << 3), 0); // no k0 = 3 entry yet: fails safe, nothing pruned
  t = k39_fresh(0);
  EXPECT_GT(k39_count(t, 0), 0);
  ASSERT_GT(nr_pdsch_config_sweep_add_k0(&t, 3), 0);
  t = k39_fresh(0);
  EXPECT_GT(k39_count(t, 3), 0);
  EXPECT_EQ(k39_count(t, 0), 0); // the certified layer now binds the rest away
  EXPECT_EQ(k39_count(t, 1), 0);
}
TEST_F(PdschConfigSweepK39, CertificationPersistsPerConfigurationOnSameRnti) {
  uint16_t mask;
  auto a = k39_open(&mask); // cfg 0x1234
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&a, 0x2), 0);
  auto b = k39_fresh(0, 0x4321);
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&b, 0x1), 0);
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS + 40; i++)
    k39_fresh(0, 0x7000 + i, (uint16_t)(0x5000 + i % 40)); // evict both contexts
  a = k39_fresh(0, 0x1234);
  b = k39_fresh(0, 0x4321);
  EXPECT_EQ(k39_count(a, 0), 0);
  EXPECT_GT(k39_count(a, 1), 0);
  EXPECT_EQ(k39_count(b, 1), 0);
  EXPECT_GT(k39_count(b, 0), 0);
}
TEST_F(PdschConfigSweepK39, RebuildClearsPriorAndFieldDormantOnlyWhenTheCatalogChanges) {
  static nr_pdsch_config_sweep_state_t st;
  ASSERT_GT(nr_pdsch_config_sweep_init_legal(&st, 2, 0, test_legal), 0);
  const int full = st.n_hyp;
  auto even_table = [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return (h->mcs_table & 1) == 0; };
  auto k0zero = [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->k0 == 0; };
  ASSERT_GE(nr_pdsch_config_sweep_set_dormant(&st, NR_TD_DORMANT_PRIOR, even_table, nullptr), 0);
  ASSERT_GE(nr_pdsch_config_sweep_set_dormant(&st, NR_TD_DORMANT_FIELD_BASE, k0zero, nullptr), 0);
  uint64_t prior[NR_TD_DWORDS], field[NR_TD_DWORDS];
  memcpy(prior, st.dormant[NR_TD_DORMANT_PRIOR], sizeof(prior));
  memcpy(field, st.dormant[NR_TD_DORMANT_FIELD_BASE], sizeof(field));
  nr_pdsch_config_sweep_rebuild(&st, 2, 0, test_legal); // same catalog: bit-identical
  EXPECT_EQ(st.n_hyp, full);
  EXPECT_EQ(memcmp(prior, st.dormant[NR_TD_DORMANT_PRIOR], sizeof(prior)), 0);
  EXPECT_EQ(memcmp(field, st.dormant[NR_TD_DORMANT_FIELD_BASE], sizeof(field)), 0);
  // pruned -> full: the index-keyed bits would be mis-attributed, so they are cleared
  ASSERT_GT(nr_pdsch_config_sweep_prune_keep(&st, k0zero, nullptr), 0);
  ASSERT_LT(st.n_hyp, full);
  nr_pdsch_config_sweep_set_dormant(&st, NR_TD_DORMANT_PRIOR, even_table, nullptr);
  nr_pdsch_config_sweep_rebuild(&st, 2, 0, test_legal);
  EXPECT_EQ(st.n_hyp, full);
  for (int i = 0; i < NR_TD_DWORDS; i++) {
    EXPECT_EQ(st.dormant[NR_TD_DORMANT_PRIOR][i], 0u);
    EXPECT_EQ(st.dormant[NR_TD_DORMANT_FIELD_BASE][i], 0u);
  }
}
TEST_F(PdschConfigSweepK39, ObserveKeepsItsReturnContractWithACertificationActive) {
  uint16_t mask;
  auto t = k39_open(&mask);
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&t, 0x3), 0);
  t = k39_fresh(0);
  // a mask no hypothesis produces admits nothing: the observed prune reports 0 (nothing pruned), certification or not
  EXPECT_EQ(nr_pdsch_config_sweep_observe(&t, 0x1, -1, 0), 0);
}

/* K42 (BC7b): observation thrash. One DM-RS mask (0x884) is produced by two durations at S = 2 (L = 10 ends on 11, L = 12 ends
 * on 13), so the same mask legitimately arrives with two last symbols (another UE's PDSCH, the RNTI's own other TDRA rows,
 * energy-threshold jitter). The observed last symbols of a mask form a monotone SET: an observation only widens it, so an
 * observation-driven prune never removes an entry an earlier observation admitted, and restore round-trips never wipe evidence. */
static int32_t k42_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  if (mapping_b || start != 2 || add != 2 || maxlen != 1)
    return -1;
  return (length == 12 || length == 10) ? 0x884 : length == 6 ? 0x84 : -1;
}
/* The type-B tests use ab_legal (type A S=1 L=13 0x4; type B adds S=5 L=4 0x20): mask_needs_typeb() needs a cached catalog
 * template, and the 4 template slots are taken by the first legality functions of this binary (ab_legal is one of them). */
struct PdschConfigSweepK42 : testing::Test {
  static constexpr uint64_t kCfg = 0x4242;
  nr_pdsch_legality_fn_t legal = k42_legal;
  void SetUp() override
  {
    nr_pdsch_config_sweep_reset_all();
    nr_pdsch_config_sweep_prior_reset();
    nr_pdsch_config_sweep_k0_legacy_set(0);
  }
  void TearDown() override { nr_pdsch_config_sweep_k0_legacy_set(-1); }
  nr_pdsch_sweep_ticket_t sel(uint8_t tda = 0, uint16_t rnti = 0x4601)
  {
    nr_pdsch_sweep_ticket_t t{};
    nr_pdsch_cfg_hypothesis_t h{};
    EXPECT_TRUE(nr_pdsch_config_sweep_select(kCfg, rnti, tda, 2, 0, legal, &t, &h));
    return t;
  }
  int observe(uint16_t mask, int last, uint8_t tda = 0, uint16_t rnti = 0x4601)
  {
    const auto t = sel(tda, rnti); // a live ticket: the observation must reach the context
    return nr_pdsch_config_sweep_observe(&t, mask, last, 0);
  }
  uint32_t trials(uint8_t tda = 0, uint16_t rnti = 0x4601)
  {
    uint32_t p = 0, t = 0;
    nr_pdsch_config_sweep_context_stats(kCfg, rnti, tda, 0, &p, &t);
    return t;
  }
  void fail(int n, uint8_t tda = 0, uint16_t rnti = 0x4601)
  {
    for (int i = 0; i < n; i++) {
      const auto t = sel(tda, rnti);
      nr_pdsch_config_sweep_feedback(&t, false, nullptr);
    }
  }
  template <class F> int count(F pred, uint8_t tda = 0, uint16_t rnti = 0x4601)
  {
    static nr_pdsch_config_sweep_state_t st;
    const auto t = sel(tda, rnti);
    EXPECT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
    int n = 0;
    for (int i = 0; i < st.n_hyp; i++)
      n += pred(st.hyp[i]);
    return n;
  }
  static bool ends(const nr_pdsch_cfg_hypothesis_t &h, int last) { return h.tda_start + h.tda_length - 1 == last; }
};
TEST_F(PdschConfigSweepK42, AlternatingLastSymbolNeverWipesEvidence) {
  ASSERT_EQ(observe(0x884, 13), 6); // L = 12, k0 {0,1} x 3 tables
  fail(30);
  ASSERT_EQ(trials(), 30u);
  const auto outstanding = sel(); // in flight across the observations below
  for (int i = 0; i < 6; i++)
    observe(0x884, (i & 1) ? 13 : 11);
  EXPECT_EQ(trials(), 30u) << "an observation round-trip wiped the CRC evidence";
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  EXPECT_EQ(trials(), 31u) << "the in-flight ticket was staled (context_reindexed)";
}
TEST_F(PdschConfigSweepK42, ForeignLastSymbolOnlyWidens) {
  ASSERT_EQ(observe(0x884, 13), 6);
  observe(0x884, 11); // the other duration of the same mask: restored, nothing removed
  EXPECT_EQ(count([](const auto &h) { return ends(h, 11); }), 6);
  EXPECT_EQ(count([](const auto &h) { return ends(h, 13); }), 6);
  observe(0x884, 13); // seen again: the set {11, 13} is not re-refined
  EXPECT_EQ(count([](const auto &h) { return ends(h, 11); }), 6);
  EXPECT_EQ(count([](const auto &h) { return ends(h, 13); }), 6);
  observe(0x884, -1); // an unmeasured last symbol neither widens nor narrows
  EXPECT_EQ(count([](const auto &) { return true; }), 12);
  // a later context of the same RNTI is seeded with the whole set (never with L = 6, whose mask was never seen)
  EXPECT_EQ(count([](const auto &h) { return ends(h, 11); }, 1), 6);
  EXPECT_EQ(count([](const auto &h) { return ends(h, 13); }, 1), 6);
  EXPECT_EQ(count([](const auto &) { return true; }, 1), 12);
}
TEST_F(PdschConfigSweepK42, PromotedCellMaskCarriesTheLastSymbolSet) {
  observe(0x884, 13, 0, 0x4601);
  observe(0x884, 11, 0, 0x4601); // RNTI A: {11, 13}
  observe(0x884, 13, 0, 0x4602); // RNTI B agrees on the mask: promoted to g_obs with A's set, then B's 13
  const auto c = [&](int last) { return count([last](const auto &h) { return ends(h, last); }, 0, 0x4603); };
  EXPECT_EQ(c(11), 6); // a third RNTI is seeded from g_obs: the promoted set, not B's single last symbol
  EXPECT_EQ(c(13), 6);
  EXPECT_EQ(c(7), 0);
}
TEST_F(PdschConfigSweepK42, OwnObservationAfterForeignKeepsTruth) {
  // truth: S = 2, L = 10 (last 11), k0 = 1. Another UE's PDSCH (same mask, last 13) is measured first and prunes the truth.
  ASSERT_EQ(observe(0x884, 13), 6);
  const auto truth = [](const nr_pdsch_cfg_hypothesis_t &h) { return ends(h, 11) && h.k0 == 1 && h.mcs_table == 1; };
  ASSERT_EQ(count(truth), 0);
  observe(0x884, 11); // own measurement: restore re-adds the truth
  ASSERT_EQ(count(truth), 1);
  fail(24);
  const uint32_t before = trials();
  for (int i = 0; i < 8; i++) { // sequence B: foreign and own alternate
    observe(0x884, (i & 1) ? 11 : 13);
    ASSERT_EQ(count(truth), 1) << "the truth was pruned again at step " << i;
  }
  EXPECT_EQ(trials(), before);
}
TEST_F(PdschConfigSweepK42, FullSetForeignMaskDoesNotWipe) {
  ASSERT_EQ(observe(0x884, 13), 6);
  for (uint16_t m = 1; m <= 7; m++) // 7 more masks no entry produces: r->obs is full (OBS_MASKS_MAX = 8)
    observe(m, 5);
  ASSERT_EQ(count([](const auto &) { return true; }), 6);
  fail(20);
  const auto outstanding = sel();
  observe(0x84, 7); // a 9th mask, not in g_obs: obs_record drops it, so nothing may be restored for it
  EXPECT_EQ(count([](const auto &h) { return h.dmrs_mask == 0x84; }), 0);
  EXPECT_EQ(trials(), 20u);
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  EXPECT_EQ(trials(), 21u);
}
TEST_F(PdschConfigSweepK42, RestoreThenCertDoesNotWipe) {
  ASSERT_EQ(observe(0x884, 13), 6);
  auto t = sel();
  ASSERT_EQ(nr_pdsch_config_sweep_certify_k0(&t, 0x1), 3); // k0 = 0 certified for this row
  fail(20);
  ASSERT_EQ(trials(), 20u);
  const auto outstanding = sel();
  uint64_t lt2 = 0, ge2 = 0, lt2_after = 0;
  nr_pdsch_config_sweep_excl_stats(&lt2, &ge2, nullptr);
  observe(0x884, 11); // restores L = 10: only certified k0 may be appended
  nr_pdsch_config_sweep_excl_stats(&lt2_after, &ge2, nullptr);
  EXPECT_EQ(lt2_after, lt2) << "restored entries were appended and then removed by apply_cert";
  EXPECT_EQ(count([](const auto &h) { return h.k0 == 1; }), 0);
  EXPECT_EQ(count([](const auto &h) { return ends(h, 11); }), 3);
  EXPECT_EQ(trials(), 20u);
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  EXPECT_EQ(trials(), 21u);
}
TEST_F(PdschConfigSweepK42, TypeBLayerRoundTripTruncatesWithoutWipe) {
  legal = ab_legal;
  ASSERT_EQ(observe(0x4, 13), 6);
  fail(20);
  const auto outstanding = sel();
  observe(0x1, -1); // a mask no type-A entry produces: typeb_seen; the type-B layer is appended, and the prune removes exactly it
  EXPECT_EQ(count([](const auto &h) { return h.mapping_type == 1; }), 0);
  observe(0x4, 13); // (latched since BC7b M1: not re-appended while the observed sets are unchanged)
  EXPECT_EQ(count([](const auto &h) { return h.mapping_type == 1; }), 0);
  EXPECT_EQ(count([](const auto &) { return true; }), 6);
  EXPECT_EQ(trials(), 20u);
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  EXPECT_EQ(trials(), 21u);
  observe(0x20, 8); // control: the type-B layer is live for this RNTI (typeb_seen), so the round trips above were real
  EXPECT_EQ(count([](const auto &h) { return h.mapping_type == 1; }), 6);
}
TEST_F(PdschConfigSweepK42, CertOnAppendedTypeBLayerTruncatesWithoutWipe) {
  legal = ab_legal;
  ASSERT_EQ(observe(0x4, 13), 6);
  auto t = sel();
  ASSERT_EQ(nr_pdsch_config_sweep_certify_k0(&t, 0x1), 3); // k0 = 0 certified
  observe(0x1, -1); // typeb_seen; the appended type-B layer is not observed: truncated away
  fail(20);
  const auto outstanding = sel();
  observe(0x20, 8); // re-appends the type-B layer (k0 {0,1}); now observed, so only apply_cert removes its k0 = 1 entries
  EXPECT_EQ(count([](const auto &h) { return h.mapping_type == 1 && h.k0 == 0; }), 3);
  EXPECT_EQ(count([](const auto &h) { return h.k0 == 1; }), 0);
  EXPECT_EQ(trials(), 20u);
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  EXPECT_EQ(trials(), 21u);
}
/* BC7b I1: a k >= 2 layer appended into a context with a BC9 exclusion is bound by apply_cert_from(c, n0): truncation, no wipe. */
TEST_F(PdschConfigSweepK42, CertifiedOrExcludedK0LayerAppendDoesNotWipe) {
  sel(); // full catalogue: L 6 / 10 / 12 (ends 7 / 11 / 13), k0 {0,1}, 3 tables
  nr_td_excl_t e;
  nr_td_excl_none(&e);
  e.last[3] = 11; // k0 = 3 may end at most on symbol 11: a k0 = 3 layer is admitted only partly (L = 12 excluded)
  ASSERT_GE(nr_pdsch_config_sweep_exclude_key(kCfg, 0x4601, 0, &e), 0);
  fail(20);
  ASSERT_EQ(trials(), 20u);
  const auto outstanding = sel();
  auto t = sel();
  ASSERT_GT(nr_pdsch_config_sweep_add_k0(&t, 3), 0);
  EXPECT_EQ(count([](const auto &h) { return h.k0 == 3; }), 6);
  EXPECT_EQ(count([](const auto &h) { return h.k0 == 3 && ends(h, 13); }), 0);
  EXPECT_EQ(trials(), 20u) << "the layer bind wiped the CRC evidence";
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  EXPECT_EQ(trials(), 21u) << "the in-flight ticket was staled";
}
TEST_F(PdschConfigSweepK42, WhollyExcludedK0LayerOnRepeatedProbesDoesNotWipe) {
  sel();
  nr_td_excl_t e;
  nr_td_excl_none(&e);
  e.last[3] = 5; // >= 1, so add_k0 does not refuse k0 = 3, but every entry ends on 7 or later: the whole layer is excluded
  ASSERT_GE(nr_pdsch_config_sweep_exclude_key(kCfg, 0x4601, 0, &e), 0);
  fail(20);
  const auto outstanding = sel();
  for (int i = 0; i < 5; i++) { // every probe hit re-appends the layer (none of it survives)
    auto t = sel();
    nr_pdsch_config_sweep_add_k0(&t, 3);
    EXPECT_EQ(count([](const auto &h) { return h.k0 == 3; }), 0);
  }
  EXPECT_EQ(trials(), 20u) << "a probe hit wiped the CRC evidence";
  nr_pdsch_config_sweep_feedback(&outstanding, false, nullptr);
  EXPECT_EQ(trials(), 21u);
}
/* TD_EXCL census (OTA diagnostic, no behaviour change): wipe-causing exclusions are restarts, tail truncations are counted apart. */
static void excl_census(uint32_t *r, uint32_t *t, uint32_t *p) {
  constexpr uint64_t kCfg = 0x4242;
  ASSERT_TRUE(nr_pdsch_config_sweep_excl_census(kCfg, 0x4601, 0, r, t, p));
}
TEST_F(PdschConfigSweepK42, ExclCensusCountsWipeAsRestartAndPhasesDistinct) {
  sel();
  nr_pdsch_config_sweep_note_dci_phase(kCfg, 0x4601, 3);
  nr_pdsch_config_sweep_note_dci_phase(kCfg, 0x4601, 3); // same phase again
  uint64_t r0, t0, a0;
  nr_pdsch_config_sweep_excl_restart_stats(&r0, &t0, &a0);
  nr_td_excl_t e;
  nr_td_excl_none(&e);
  e.last[0] = 10; // k0 = 0 entries that end on 13 go, from the middle of the catalogue: wipe + reindex
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(kCfg, 0x4601, 0, &e), 0);
  uint32_t r = 9, t = 9, p = 9;
  excl_census(&r, &t, &p);
  EXPECT_EQ(r, 1u);
  EXPECT_EQ(t, 0u);
  EXPECT_EQ(p, 1u);
  nr_pdsch_config_sweep_note_dci_phase(kCfg, 0x4601, 4);
  excl_census(&r, &t, &p);
  EXPECT_EQ(p, 2u);
  uint64_t r1, t1, a1;
  nr_pdsch_config_sweep_excl_restart_stats(&r1, &t1, &a1);
  EXPECT_EQ(r1 - r0, 1u);
  EXPECT_EQ(a1 - a0, 0u); // restarts 1 <= phases 1: sound
}
TEST_F(PdschConfigSweepK42, ExclCensusCountsTailTruncationSeparately) {
  sel();
  nr_td_excl_t e;
  nr_td_excl_none(&e);
  e.last[3] = 11;
  ASSERT_GE(nr_pdsch_config_sweep_exclude_key(kCfg, 0x4601, 0, &e), 0);
  fail(20);
  auto t = sel();
  ASSERT_GT(nr_pdsch_config_sweep_add_k0(&t, 3), 0); // appended k0 = 3 layer, partly excluded: truncation, no wipe
  uint32_t r = 9, tr = 9, p = 9;
  excl_census(&r, &tr, &p);
  EXPECT_EQ(r, 0u);
  EXPECT_EQ(tr, 1u);
  EXPECT_EQ(p, 0u);
}
TEST_F(PdschConfigSweepK42, ExclRestartAlarmFiresOnceWhenRestartsExceedPhases) {
  sel();
  nr_pdsch_config_sweep_note_dci_phase(kCfg, 0x4601, 0);
  uint64_t r0, t0, a0;
  nr_pdsch_config_sweep_excl_restart_stats(&r0, &t0, &a0);
  nr_td_excl_t e;
  nr_td_excl_none(&e);
  e.last[0] = 12; // removes the ends-on-13 entries
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(kCfg, 0x4601, 0, &e), 0); // restart 1, phases 1
  e.last[0] = 8; // removes the ends-on-11 entries
  nr_pdsch_config_sweep_exclude_key(kCfg, 0x4601, 0, &e); // restart 2 without a new phase: unsound
  uint64_t r1, t1, a1;
  nr_pdsch_config_sweep_excl_restart_stats(&r1, &t1, &a1);
  EXPECT_EQ(r1 - r0, 2u);
  EXPECT_EQ(a1 - a0, 1u);
  e.last[0] = 6;
  nr_pdsch_config_sweep_exclude_key(kCfg, 0x4601, 0, &e);
  nr_pdsch_config_sweep_excl_restart_stats(&r1, &t1, &a1);
  EXPECT_EQ(a1 - a0, 1u) << "the alarm repeats for the same context";
}
/* BC7b M1: once the observe path's type-B layer is truncated in full, it is not re-appended until the observed sets change. */
TEST_F(PdschConfigSweepK42, TruncatedTypeBLayerIsLatchedUntilTheObservedSetsChange) {
  legal = ab_legal;
  ASSERT_EQ(observe(0x4, 13), 6);
  uint64_t a0 = 0, l0 = 0, a = 0, l = 0;
  nr_pdsch_config_sweep_typeb_stats(&a0, &l0);
  observe(0x1, -1); // typeb_seen; the layer is appended and truncated in full: latched
  nr_pdsch_config_sweep_typeb_stats(&a, &l);
  ASSERT_EQ(a - a0, 1u);
  ASSERT_EQ(l - l0, 1u);
  for (int i = 0; i < 3; i++)
    observe(0x4, 13); // unchanged observed sets: no append / truncate loop
  nr_pdsch_config_sweep_typeb_stats(&a, &l);
  EXPECT_EQ(a - a0, 1u);
  EXPECT_EQ(l - l0, 1u);
  observe(0x20, 8); // a new mask (a type-B one): the latch no longer matches, the layer comes back and stays
  nr_pdsch_config_sweep_typeb_stats(&a, &l);
  EXPECT_EQ(a - a0, 2u);
  EXPECT_EQ(l - l0, 1u);
  EXPECT_EQ(count([](const auto &h) { return h.mapping_type == 1; }), 6);
}
/* BC7b M3: the cell-wide promotion is the union of every holder's set. Regression guard only: with today's call order the first
 * observation of a second RNTI always promotes, so two non-promoted holders cannot coexist and the old "first holder only" code
 * gives the same result here. */
TEST_F(PdschConfigSweepK42, PromotionIsTheUnionOfEveryHolder) {
  observe(0x884, 11, 0, 0x4601);
  observe(0x884, 13, 0, 0x4602); // promoted: {11} from 0x4601, then 13
  observe(0x884, 13, 0, 0x4603);
  const auto c = [&](int last) { return count([last](const auto &h) { return ends(h, last); }, 0, 0x4604); };
  EXPECT_EQ(c(11), 6);
  EXPECT_EQ(c(13), 6);
}
TEST_F(PdschConfigSweepK42, LegacyPinKeepsK0AndTheLastSymbolSetIsMonotone) {
  nr_pdsch_config_sweep_k0_legacy_set(1);
  ASSERT_EQ(observe(0x884, 13), 3); // legacy: the job's k0 = 0 is pinned
  observe(0x884, 11);
  EXPECT_EQ(count([](const auto &h) { return h.k0 == 1; }), 0);
  EXPECT_EQ(count([](const auto &h) { return ends(h, 11); }), 3);
  fail(10);
  for (int i = 0; i < 4; i++)
    observe(0x884, (i & 1) ? 11 : 13);
  EXPECT_EQ(count([](const auto &h) { return h.k0 == 1; }), 0);
  EXPECT_EQ(count([](const auto &) { return true; }), 6);
  EXPECT_EQ(trials(), 10u);
}
TEST_F(PdschRecovery, CertificationClearedOnReopen) {
  nr_pdsch_config_sweep_k0_legacy_set(0);
  auto t = recovery_select();
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&t, 0x1), 0);
  const auto old = recovery_settle(0);
  ASSERT_TRUE(old.settled);
  for (unsigned f = 0; f < 5000 && nr_pdsch_config_sweep_is_settled(800, 0x4601, 0, 0); ++f) {
    auto x = recovery_select();
    nr_pdsch_config_sweep_feedback(&x, false, nullptr);
  }
  ASSERT_FALSE(nr_pdsch_config_sweep_is_settled(800, 0x4601, 0, 0));
  auto fresh = recovery_select();
  // After a reopen the certification is gone: an observation no longer prunes k0 = 1.
  nr_pdsch_config_sweep_observe(&fresh, 4, 13, 0);
  fresh = recovery_select();
  EXPECT_GT(count_snapshot_k0(fresh, 1), 0);
  EXPECT_GT(count_snapshot_k0(fresh, 0), 0);
  nr_pdsch_config_sweep_k0_legacy_set(-1);
}

TEST_F(PdschRecovery, ClearedCertificationDoesNotComeBackAfterEvictionAndRecreation) {
  nr_pdsch_config_sweep_k0_legacy_set(0);
  auto t = recovery_select();
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&t, 0x1), 0);
  ASSERT_TRUE(recovery_settle(0).settled);
  for (unsigned f = 0; f < 5000 && nr_pdsch_config_sweep_is_settled(800, 0x4601, 0, 0); ++f) {
    auto x = recovery_select();
    nr_pdsch_config_sweep_feedback(&x, false, nullptr);
  }
  ASSERT_FALSE(nr_pdsch_config_sweep_is_settled(800, 0x4601, 0, 0)); // reopened: certification cleared
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS + 40; i++)
    recovery_select(5000 + i, (uint16_t)(0x5000 + i % 40)); // evict the context
  const auto again = recovery_select();
  EXPECT_GT(count_snapshot_k0(again, 0), 0);
  EXPECT_GT(count_snapshot_k0(again, 1), 0); // a persisted certification would have bound it to k0 = 0
}

// Opt-in benchmark of the real shared-bank reset, outside an OTA run.
TEST(PdschReset, Timing) {
  if (!getenv("ISAC_PDSCH_RESET_BENCH")) GTEST_SKIP();
  std::vector<double> us;
  for(int i=0;i<528;++i) {
    auto start=std::chrono::steady_clock::now();
    nr_pdsch_config_sweep_reset_all();
    double t=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    if(i>=16) us.push_back(t);
  }
  double sum=0; for(double v:us) sum+=v;
  std::sort(us.begin(),us.end());
  printf("PDSCHRESET n=%zu mean_us=%.2f p99_us=%.2f max_us=%.2f\n",us.size(),sum/us.size(),us[(99*us.size()+99)/100-1],us.back());
}

TEST(PdschReset, ClearsEvidenceAndRejectsTicketsAcrossReuse) {
  nr_pdsch_config_sweep_reset_all();
  const auto old0=select_context(777,0x4601,0);
  const auto old1=select_context(778,0x4602,1);
  nr_pdsch_config_sweep_feedback(&old0,true,nullptr);
  nr_pdsch_config_sweep_feedback(&old1,false,nullptr);
  nr_pdsch_config_sweep_state_t state{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&old0,&state));
  EXPECT_EQ(state.trials[old0.hypothesis],1u);
  nr_pdsch_config_sweep_reset_all();
  EXPECT_FALSE(nr_pdsch_config_sweep_snapshot(&old0,&state));
  EXPECT_FALSE(nr_pdsch_config_sweep_snapshot(&old1,&state));
  EXPECT_FALSE(nr_pdsch_config_sweep_feedback(&old0,true,nullptr));
  EXPECT_FALSE(nr_pdsch_config_sweep_feedback(&old1,true,nullptr));
  EXPECT_EQ(nr_pdsch_config_sweep_observe_mask(&old0,4),0);
  uint32_t passes=99,trials=99;
  nr_pdsch_config_sweep_context_stats(777,0x4601,0,0,&passes,&trials);
  EXPECT_EQ(passes,0u); EXPECT_EQ(trials,0u);
  EXPECT_EQ(nr_pdsch_config_sweep_settled_count(),0);
  EXPECT_FALSE(nr_pdsch_config_sweep_prior_get(nullptr,nullptr,nullptr,nullptr));
  EXPECT_FALSE(nr_pdsch_config_sweep_rnti_prior_get(0x4601,nullptr,nullptr,nullptr,nullptr));
  const auto fresh=select_context(777,0x4601,0);
  EXPECT_NE(fresh.generation,old0.generation);
  EXPECT_FALSE(nr_pdsch_config_sweep_feedback(&old0,true,nullptr));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&fresh,&state));
  EXPECT_EQ(state.winner,-1);
  for(int i=0;i<state.n_hyp;++i) { EXPECT_EQ(state.trials[i],0u); EXPECT_EQ(state.ok[i],0u); }
}

/* ---- Task 14: mapping type B and k0 >= 2 ---------------------------------------------------------
 * TS 38.214 Table 5.1.2.1-1 (normal CP): type A S 0..3, L 3..14, S+L <= 14; type B (Rel-16)
 * S 0..12, L 2..13, S+L <= 14. k0 may be 0..32; only values the air shows are enumerated. */
TEST(PdschConfigSweepTypeB, LegalTdaTablesMatchTs38214) {
  int a = 0, b = 0;
  for (int S = 0; S < 14; S++)
    for (int L = 1; L <= 14; L++) {
      a += nr_pdsch_tda_legal(0, S, L) ? 1 : 0;
      b += nr_pdsch_tda_legal(1, S, L) ? 1 : 0;
    }
  EXPECT_EQ(a, 42);
  EXPECT_EQ(b, 90);
  EXPECT_FALSE(nr_pdsch_tda_legal(2, 0, 7));
}

/* R30 item 1 (technique-d-regression.md): a FRESH context's catalog is mapping type A only, matching
 * base commit 222f98d072's pre-Task-14 (pre-dilution) size -- type B used to be built in
 * unconditionally (2016 -> 6336 pure, 2.9x) whether or not the cell even used it, which measurably
 * starved the type-A search this cell actually needed. Type B enters only once the DM-RS oracle
 * observes a mask type A cannot explain (see the observation tests below). */
TEST(PdschConfigSweepTypeB, FreshCatalogIsTypeAOnly) {
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  std::cerr << "[ MEASURED ] fresh pure catalog n_hyp=" << n << " max=" << NR_PDSCH_SWEEP_MAX_HYP
            << " bytes/context=" << sizeof(nr_pdsch_config_sweep_state_t) << std::endl;
  EXPECT_EQ(n, 2016);
  for (int i = 0; i < n; i++)
    EXPECT_EQ(st.hyp[i].mapping_type, 0);
}

/* The mapping type reaches the legality function (OAI's mask generator takes it), and a type-B entry
 * whose effective PDU equals a type-A one (same S, L, k0, mask, table) is ONE hypothesis: the TB CRC
 * cannot tell them apart, and two indistinguishable hypotheses could never be separated. */
static int32_t ab_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  if (maxlen != 1)
    return 0;
  if (!mapping_b)
    return start == 1 && length == 13 && add == 0 ? 0x4 : 0;
  if (start == 1 && length == 13 && add == 0)
    return 0x4; /* identical PDU to the type-A entry: must merge */
  return start == 5 && length == 4 && add == 1 ? 0x20 : 0;
}

/* The merge is catalog_add_mapping_type's own dedup, shared by init_legal and the evidence-triggered
 * add_typeb_layer(). Equivalence is scoped to the SAME (S,L,k0): the data RE range comes from (S,L),
 * so two DIFFERENT (S,L) that happen to produce the same absolute dmrs_mask are NOT the same
 * effective PDU and must NOT be merged (an earlier version of this test drove exactly that case and
 * was wrong -- it hid a real bug where the real mask generator's coincidental cross-(S,L) mask reuse
 * silently ate legitimate type-B entries, caught by DlAdaptive.TypeBTruthIsPinnedByOneOracleObservat
 * ionAndConverges in nr_dl_adaptive_test.cc). What DOES legitimately collide at the SAME (S,L) is two
 * different add_pos values landing on the same DM-RS symbol pattern. A fresh catalog no longer mixes
 * both mapping types in one call for the old test to observe pre-prune (R30 item 1), so this drives
 * the same dedup code within one mapping type instead. */
static int32_t dup_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  if (maxlen != 1 || mapping_b)
    return 0;
  if (start == 1 && length == 13 && (add == 0 || add == 1))
    return 0x4; /* two add_pos values, same effective PDU at the same (S,L,k0) -- must merge to one */
  return 0;
}
TEST(PdschConfigSweepTypeB, IdenticalEffectivePdusMergeToOneHypothesis) {
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init_legal(&st, 2, 0, dup_legal);
  EXPECT_EQ(n, 6); // one (S,L) worth of entries (k0{0,1} x 3 mcs tables), not two add_pos variants
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(st.hyp[i].tda_start, 1);
    EXPECT_EQ(st.hyp[i].tda_length, 13);
  }
}

/* R30 item 1's own wording: "type B enters a context only once the DM-RS oracle observes ... a
 * type-B-only mask." ab_legal's type-A catalog only ever produces mask 0x4 (S=1,L=13); 0x20
 * (S=5,L=4,add=1) cannot come from any type-A hypothesis, so it must trigger the widening and then
 * prune to exactly the entries that produce it -- "a type-B observation adds exactly the matching
 * type-B entries." */
TEST(PdschConfigSweepTypeB, ObservingATypeBOnlyMaskWidensToExactlyItsMatchingEntries) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0xCC, 0x4601, 0, 2, 0, ab_legal, &t, &h));
  nr_pdsch_config_sweep_state_t st{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  EXPECT_EQ(st.n_hyp, 6); // fresh: type A only (S=1,L=13 x k0{0,1} x 3 tables)
  for (int i = 0; i < st.n_hyp; i++)
    EXPECT_EQ(st.hyp[i].mapping_type, 0);

  ASSERT_GT(nr_pdsch_config_sweep_observe_mask(&t, 0x20), 0);
  // The prune re-numbered the catalog, so the pre-prune ticket is retired (lane perf 2026-09-27,
  // TicketIssuedBeforeAPruneCannotScoreAfterIt); look through a fresh one.
  EXPECT_FALSE(nr_pdsch_config_sweep_snapshot(&t, &st));
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0xCC, 0x4601, 0, 2, 0, ab_legal, &t, &h));
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  EXPECT_EQ(st.n_hyp, 6); // 1 (S,L) x k0{0,1} x 3 tables, all mapping type B
  for (int i = 0; i < st.n_hyp; i++) {
    EXPECT_EQ(st.hyp[i].mapping_type, 1);
    EXPECT_EQ(st.hyp[i].tda_start, 5);
    EXPECT_EQ(st.hyp[i].tda_length, 4);
    EXPECT_EQ(st.hyp[i].dmrs_mask, 0x20);
  }

  // A sibling context (same RNTI, different TDA index) created AFTER the observation inherits the
  // widening too -- mirrors the k0-layer mechanism (PdschConfigSweepK0.ObservedK0IsAddedToTheContext).
  nr_pdsch_sweep_ticket_t t1{};
  nr_pdsch_cfg_hypothesis_t h1{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0xCC, 0x4601, 1, 2, 0, ab_legal, &t1, &h1));
  nr_pdsch_config_sweep_state_t st1{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t1, &st1));
  int b = 0;
  for (int i = 0; i < st1.n_hyp; i++)
    b += st1.hyp[i].mapping_type == 1;
  EXPECT_EQ(b, 6);
}

/* dmrs-DownlinkForPDSCH-MappingTypeA and -MappingTypeB are separate RRC IEs: a prior learned on a
 * type-A entry says nothing about type-B add_pos/max_len (mcs-Table is shared). */
TEST(PdschConfigSweepTypeB, PriorFromTypeAKeepsTypeBEntriesOfTheSameTable) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  unsigned seed = 77;
  bool converged = false;
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  for (int i = 0; i < 200000 && !converged; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0xAB, 0x4601, 0, 2, 0, ab_legal, &t, &h));
    const bool truth = h.mapping_type == 0 && h.k0 == 0 && h.mcs_table == 1;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    converged = nr_pdsch_config_sweep_feedback(&t, truth && u < 0.54, nullptr);
  }
  ASSERT_TRUE(converged);
  // R30 item 1: type B enters only once observed. Tell this RNTI about the type-B-only mask (as the
  // DM-RS oracle would on air) before opening the sibling -- without this the sibling is type-A only.
  // The same observation also narrows by mask (prune_to_observed), so the sibling collapses straight to
  // the type-B entries sharing both the prior's mcs_table AND the observed mask -- if prune_prior
  // wrongly applied the type-A prior's add_pos to type-B entries too, they would have been dropped
  // already and nothing would survive the mask narrowing that follows.
  nr_pdsch_config_sweep_observe_mask(&t, 0x20);
  nr_pdsch_sweep_ticket_t t1{};
  nr_pdsch_cfg_hypothesis_t h1{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0xAB, 0x4601, 1, 2, 0, ab_legal, &t1, &h1));
  nr_pdsch_config_sweep_state_t st{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t1, &st));
  EXPECT_EQ(st.n_hyp, 2);  /* type B, table 1, k0 {0,1} -- its own add_pos 1, not the type-A prior's 0 */
  for (int i = 0; i < st.n_hyp; i++) {
    EXPECT_EQ(st.hyp[i].mcs_table, 1);
    EXPECT_EQ(st.hyp[i].mapping_type, 1);
    EXPECT_EQ(st.hyp[i].dmrs_add_pos, 1);
    EXPECT_EQ(st.hyp[i].dmrs_mask, 0x20);
  }
}

static int count_k0(const nr_pdsch_config_sweep_state_t &st, int k0)
{
  int n = 0;
  for (int i = 0; i < st.n_hyp; i++)
    n += st.hyp[i].k0 == k0;
  return n;
}
TEST(PdschConfigSweepK0, ObservedK0IsAddedToTheContext) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  const auto t = select_context(0x5150, 0x4601, 0);
  nr_pdsch_config_sweep_state_t st{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  const int before = st.n_hyp;
  EXPECT_EQ(count_k0(st, 3), 0);
  const int added = nr_pdsch_config_sweep_add_k0(&t, 3);
  EXPECT_GT(added, 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st)); /* the ticket stays valid: entries are appended */
  EXPECT_EQ(st.n_hyp, before + added);
  EXPECT_EQ(count_k0(st, 3), added);
  EXPECT_EQ(count_k0(st, 3), count_k0(st, 0)); /* one complete layer */
  EXPECT_EQ(nr_pdsch_config_sweep_add_k0(&t, 3), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_add_k0(&t, 33), 0); /* TS 38.214: k0 <= 32 */
  /* A context of the same RNTI created later carries the observed k0 as well. */
  const auto t1 = select_context(0x5150, 0x4601, 1);
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t1, &st));
  EXPECT_EQ(count_k0(st, 3), added);
}

/* A k0 layer the context tried and did not win on was a false k0-oracle hit: once the context converges
 * on another k0, later contexts of the RNTI are no longer seeded with it. */
TEST(PdschConfigSweepK0, ConvergenceOnAnotherK0DropsTheFalseLayer) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0xAC, 0x4601, 0, 2, 0, ab_legal, &t, &h));
  ASSERT_EQ(nr_pdsch_config_sweep_add_k0(&t, 3), 3); // R30 item 1: fresh catalog is type A only (3 mcs tables at k0=0)
  unsigned seed = 5;
  bool converged = false;
  nr_pdsch_cfg_hypothesis_t w{};
  for (int i = 0; i < 200000 && !converged; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0xAC, 0x4601, 0, 2, 0, ab_legal, &t, &h));
    const bool truth = h.mapping_type == 0 && h.k0 == 0 && h.mcs_table == 1;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    converged = nr_pdsch_config_sweep_feedback(&t, truth && u < 0.54, &w);
  }
  ASSERT_TRUE(converged);
  EXPECT_EQ(w.k0, 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0xAC, 0x4601, 1, 2, 0, ab_legal, &t, &h));
  nr_pdsch_config_sweep_state_t st{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &st));
  EXPECT_EQ(count_k0(st, 3), 0);
}

/* ---- Lane perf (2026-09-27): a prune compacts st->hyp[] IN PLACE, so every ticket issued before it
 * names an index that now belongs to a different hypothesis (or none). Such a ticket must be refused,
 * never credited to whatever landed on its old index. Before the fix the context generation did not
 * change on a prune, and a pre-prune ticket for old index 0..2 scored the post-prune entry 0..2. */
TEST(PdschConfigSweepOracle, TicketIssuedBeforeAPruneCannotScoreAfterIt) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  const uint16_t mask = (uint16_t)test_legal(0, 13, 1, 0, 2, 1); // S=1 L=13 add 2 len 1
  nr_pdsch_sweep_ticket_t old{};
  nr_pdsch_cfg_hypothesis_t h{};
  bool found = false;
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_HYP && !found; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x51, 0x4601, 0, 2, 0, test_legal, &old, &h));
    found = old.hypothesis < 3 && h.dmrs_mask != mask;
  }
  ASSERT_TRUE(found);
  ASSERT_EQ(nr_pdsch_config_sweep_observe(&old, mask, 13, 0), 6); // K39: prunes to 3 mcs tables x k0 {0,1}
  nr_pdsch_config_sweep_feedback(&old, true, nullptr);            // in flight across the prune
  nr_pdsch_sweep_ticket_t now{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x51, 0x4601, 0, 2, 0, test_legal, &now, &h));
  nr_pdsch_config_sweep_state_t state{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&now, &state));
  ASSERT_EQ(state.n_hyp, 6);
  for (int i = 0; i < state.n_hyp; i++) {
    EXPECT_EQ(state.trials[i], 0u) << "pre-prune ticket credited to hypothesis " << i;
    EXPECT_EQ(state.ok[i], 0u);
  }
  EXPECT_FALSE(nr_pdsch_config_sweep_snapshot(&old, &state)); // the old ticket names a dead layout
}

/* The same holds for the Qm oracle's table prune (observe_qm runs on the consumer after its own
 * feedback, but OTHER consumers' tickets are still in flight). */
TEST(PdschConfigSweepQm, TicketIssuedBeforeATablePruneCannotScoreAfterIt) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  nr_pdsch_sweep_ticket_t old{}, t{};
  nr_pdsch_cfg_hypothesis_t h{};
  bool found = false; /* an old index that survives the prune's re-numbering, so it WOULD be credited */
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_HYP && !found; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x52, 0x4601, 0, 2, 0, test_legal, &old, &h));
    found = old.hypothesis < 10;
  }
  ASSERT_TRUE(found);
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x52, 0x4601, 0, 2, 0, test_legal, &t, &h));
  nr_pdsch_config_sweep_observe_qm(&t, 20, 8);          // MCS 20 at Qm 8 is table 1 only
  ASSERT_GT(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);
  EXPECT_FALSE(nr_pdsch_config_sweep_feedback(&old, true, nullptr));
  nr_pdsch_sweep_ticket_t now{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x52, 0x4601, 0, 2, 0, test_legal, &now, &h));
  nr_pdsch_config_sweep_state_t state{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&now, &state));
  for (int i = 0; i < state.n_hyp; i++)
    EXPECT_EQ(state.trials[i], 0u);
}

/* A k0 hypothesis decodes the slot k0 after the DCI: every decode path (deferred fast path, deferred
 * normal path, in-line) must target that slot, or a k0 = 1 entry decodes the SAME samples as its
 * k0 = 0 twin, scores identically, and the sweep can never separate them (it correctly refuses to
 * pick between two indistinguishable hypotheses) -- measured: 0 CONVERGED on the phy-test bed. */
TEST(PdschConfigSweepK0, TargetSlotWrapsFrameAndSfn) {
  int f = -1, s = -1;
  nr_pdsch_k0_slot(100, 5, 20, 0, &f, &s);
  EXPECT_EQ(f, 100); EXPECT_EQ(s, 5);
  nr_pdsch_k0_slot(100, 19, 20, 1, &f, &s);
  EXPECT_EQ(f, 101); EXPECT_EQ(s, 0);
  nr_pdsch_k0_slot(1023, 18, 20, 3, &f, &s);
  EXPECT_EQ(f, 0); EXPECT_EQ(s, 1);
  nr_pdsch_k0_slot(7, 2, 20, 32, &f, &s);
  EXPECT_EQ(f, 8); EXPECT_EQ(s, 14);
}

TEST(PdschSweepK, K1IsBitIdenticalToNext) {
  static nr_pdsch_config_sweep_state_t a, b;
  nr_pdsch_config_sweep_init(&a, 4); nr_pdsch_config_sweep_init(&b, 4);
  unsigned seed = 7;
  for (int i = 0; i < 20000 && a.winner < 0; i++) {
    nr_pdsch_cfg_hypothesis_t ha, hb[NR_TD_MAX_K]; int ib[NR_TD_MAX_K];
    const int ia = nr_pdsch_config_sweep_next(&a, &ha);
    ASSERT_EQ(nr_pdsch_config_sweep_next_k(&b, 1, ib, hb), 1);
    ASSERT_EQ(ia, ib[0]);
    const bool ok = (ia == 3) && (rand_r(&seed) % 100 < 60);
    nr_pdsch_config_sweep_feed(&a, ia, ok);
    const nr_td_outcome_t o = {ib[0], NR_TD_FULL_TB, (uint8_t)(ok ? NR_TD_PASS : NR_TD_FAIL), false};
    nr_pdsch_config_sweep_feed_k(&b, &o, 1);
  }
  EXPECT_EQ(a.winner, b.winner);
}
/* Same, on a pruned catalog that actually converges, so the whole sequence up to the winner is compared. */
TEST(PdschSweepK, K1IsBitIdenticalToNextUntilConvergence) {
  static nr_pdsch_config_sweep_state_t a, b;
  nr_pdsch_config_sweep_init(&a, 4);
  ASSERT_GT(nr_pdsch_config_sweep_prune_to(&a, a.hyp[0].mcs_table, a.hyp[0].dmrs_add_pos, a.hyp[0].dmrs_max_len), 0);
  b = a;
  unsigned seed = 7;
  int i = 0;
  for (; i < 200000 && a.winner < 0; i++) {
    nr_pdsch_cfg_hypothesis_t ha, hb[NR_TD_MAX_K]; int ib[NR_TD_MAX_K];
    const int ia = nr_pdsch_config_sweep_next(&a, &ha);
    ASSERT_EQ(nr_pdsch_config_sweep_next_k(&b, 1, ib, hb), 1);
    ASSERT_EQ(ia, ib[0]);
    const bool ok = (ia == 3) && (rand_r(&seed) % 100 < 60);
    const int wa = nr_pdsch_config_sweep_feed(&a, ia, ok);
    const nr_td_outcome_t o = {ib[0], NR_TD_FULL_TB, (uint8_t)(ok ? NR_TD_PASS : NR_TD_FAIL), false};
    ASSERT_EQ(wa, nr_pdsch_config_sweep_feed_k(&b, &o, 1));
  }
  EXPECT_EQ(a.winner, 3);
  EXPECT_EQ(b.winner, 3);
  EXPECT_EQ(0, memcmp(a.trials, b.trials, sizeof(a.trials)));
  EXPECT_EQ(0, memcmp(a.ok, b.ok, sizeof(a.ok)));
  std::cout << "pruned catalog " << a.n_hyp << " hypotheses converged after " << i << " grants" << std::endl;
}
TEST(PdschSweepK, ZeroScoresKeepShuffleOrder) {
  static nr_pdsch_config_sweep_state_t a, b;
  nr_pdsch_config_sweep_init(&a, 4); nr_pdsch_config_sweep_init(&b, 4);
  nr_td_side_info_t s = {}; s.obs_dmrs_mask = -1; s.obs_qm = -1; s.obs_last_symbol = -1;
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1; s.w_sib1 = 1; /* weight on, no data */
  b.side = &s;
  for (int i = 0; i < 3 * a.n_hyp; i++) {
    nr_pdsch_cfg_hypothesis_t h; ASSERT_EQ(nr_pdsch_config_sweep_next(&a, &h), nr_pdsch_config_sweep_next(&b, &h));
    nr_pdsch_config_sweep_feed(&a, 0, false); nr_pdsch_config_sweep_feed(&b, 0, false);
  }
  b.side = nullptr; /* s is a local: do not leave the static state pointing at it */
}
TEST(PdschSweepK, ProbesAreDistinctAndDoNotAdvanceCursor) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  int idx[NR_TD_MAX_K]; nr_pdsch_cfg_hypothesis_t h[NR_TD_MAX_K];
  const int c0 = st.cursor; const int n = nr_pdsch_config_sweep_next_k(&st, 3, idx, h);
  ASSERT_EQ(n, 3); EXPECT_NE(idx[0], idx[1]); EXPECT_NE(idx[1], idx[2]); EXPECT_NE(idx[0], idx[2]);
  EXPECT_EQ(st.cursor, (c0 + 1) % st.n_hyp); /* only the main selection advanced it */
}
TEST(PdschSweepK, P1ProbeFailuresDoNotTouchKlStats) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  const nr_td_outcome_t o[2] = {{0, NR_TD_FULL_TB, NR_TD_FAIL, false}, {5, NR_TD_CB_PROBE, NR_TD_FAIL, true}};
  nr_pdsch_config_sweep_feed_k(&st, o, 2);
  EXPECT_EQ(st.trials[5], 0u); EXPECT_EQ(st.probe_fail[5], 1);
}
TEST(PdschSweepK, P2AdmissibleProbeFailIsOneKlFailureAndPassIsNothing) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4); st.p2 = true;
  const nr_td_outcome_t o[3] = {{0, NR_TD_FULL_TB, NR_TD_FAIL, false}, {5, NR_TD_CB_PROBE, NR_TD_FAIL, true},
                                {6, NR_TD_CB_PROBE, NR_TD_PASS, true}};
  nr_pdsch_config_sweep_feed_k(&st, o, 3);
  EXPECT_EQ(st.trials[5], 1u); EXPECT_EQ(st.ok[5], 0u);
  EXPECT_EQ(st.trials[6], 0u); EXPECT_EQ(st.probe_pass[6], 1);
  const nr_td_outcome_t na[2] = {{0, NR_TD_FULL_TB, NR_TD_FAIL, false}, {7, NR_TD_CB_PROBE, NR_TD_FAIL, false}};
  nr_pdsch_config_sweep_feed_k(&st, na, 2);
  EXPECT_EQ(st.trials[7], 0u); /* not admissible: no KL evidence */
}

/* Non-neutral side information: the round is the shuffle, stably partitioned by key -- the matching
 * hypotheses first, each group in the exact order the neutral twin's shuffle produced. */
TEST(PdschSweepK, ScoredRoundIsStablePartitionOfShuffle) {
  static nr_pdsch_config_sweep_state_t a, b;
  nr_pdsch_config_sweep_init(&a, 4); nr_pdsch_config_sweep_init(&b, 4);
  nr_td_side_info_t s = {}; s.obs_dmrs_mask = -1; s.obs_qm = -1; s.obs_last_symbol = -1;
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1;
  s.f_S = 1; s.f_L = 13; s.f_conf = 1; s.w_field = 1; /* key 2 for S=1 L=13, 1 for one of them, 0 else */
  b.side = &s;
  std::vector<int> ra, rb;
  for (int i = 0; i < a.n_hyp; i++) {
    nr_pdsch_cfg_hypothesis_t h;
    ra.push_back(nr_pdsch_config_sweep_next(&a, &h));
    rb.push_back(nr_pdsch_config_sweep_next(&b, &h));
  }
  EXPECT_EQ(a.random_state, b.random_state); /* same RNG consumption */
  std::vector<int> expect;
  for (int k = 2; k >= 0; k--)
    for (int i : ra)
      if ((a.hyp[i].tda_start == 1) + (a.hyp[i].tda_length == 13) == k)
        expect.push_back(i);
  EXPECT_EQ(rb, expect);
  EXPECT_NE(rb, ra);
  b.side = nullptr;
}
/* P1: a probe pass moves its hypothesis to the front of the next round (w_probe), no KL evidence. */
TEST(PdschSweepK, ProbePassBonusOrdersNextRound) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  nr_td_side_info_t s = {}; s.obs_dmrs_mask = -1; s.obs_qm = -1; s.obs_last_symbol = -1;
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1; s.w_probe = 1;
  st.side = &s;
  nr_pdsch_cfg_hypothesis_t h;
  for (int i = 0; i < st.n_hyp; i++) /* finish round 1 */
    nr_pdsch_config_sweep_next(&st, &h);
  ASSERT_EQ(st.cursor, 0);
  const nr_td_outcome_t o[2] = {{0, NR_TD_FULL_TB, NR_TD_INCONCLUSIVE, false}, {42, NR_TD_CB_PROBE, NR_TD_PASS, true}};
  nr_pdsch_config_sweep_feed_k(&st, o, 2);
  EXPECT_EQ(st.trials[0], 0u); /* inconclusive main outcome is not fed */
  EXPECT_EQ(st.trials[42], 0u);
  EXPECT_EQ(nr_pdsch_config_sweep_next(&st, &h), 42);
  st.side = nullptr;
}
TEST(PdschSweepK, ProbesSkipClearedAndWinnerReturnsOne) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  for (int i = 0; i < st.n_hyp; i++)
    st.trials[i] = 300; /* every hypothesis cleared (no pass) */
  int idx[NR_TD_MAX_K]; nr_pdsch_cfg_hypothesis_t h[NR_TD_MAX_K];
  EXPECT_EQ(nr_pdsch_config_sweep_next_k(&st, NR_TD_MAX_K + 5, idx, h), 1);
  st.ok[7] = 1; st.ok[9] = 1; /* not cleared any more; main goes to the hot one (most passes, lowest index) */
  const int n = nr_pdsch_config_sweep_next_k(&st, 4, idx, h); /* main is unfiltered; probes only 7 and 9 */
  std::vector<int> probes(idx + 1, idx + n);
  std::sort(probes.begin(), probes.end());
  std::vector<int> expect;
  if (idx[0] != 7) expect.push_back(7);
  if (idx[0] != 9) expect.push_back(9);
  EXPECT_EQ(probes, expect);
  st.winner = 9;
  EXPECT_EQ(nr_pdsch_config_sweep_next_k(&st, 4, idx, h), 1);
  EXPECT_EQ(idx[0], 9);
  EXPECT_EQ(nr_pdsch_config_sweep_next_k(&st, 0, idx, h), 0);
}
TEST(PdschSweepK, ProbeCountersSaturateAndClearOnPrune) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  st.probe_fail[3] = UINT16_MAX;
  const nr_td_outcome_t o[3] = {{0, NR_TD_FULL_TB, NR_TD_FAIL, false}, {3, NR_TD_CB_PROBE, NR_TD_FAIL, false},
                                {4, NR_TD_CB_PROBE, NR_TD_INCONCLUSIVE, false}};
  nr_pdsch_config_sweep_feed_k(&st, o, 3);
  EXPECT_EQ(st.probe_fail[3], UINT16_MAX);
  EXPECT_EQ(st.probe_inconclusive[4], 1);
  ASSERT_GT(nr_pdsch_config_sweep_prune_to(&st, st.hyp[0].mcs_table, st.hyp[0].dmrs_add_pos, st.hyp[0].dmrs_max_len), 0);
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_HYP; i++)
    ASSERT_EQ(st.probe_fail[i] | st.probe_inconclusive[i] | st.probe_pass[i], 0) << i;
}
/* P2 never double-scores one grant: a probe repeating the main hypothesis adds no extra KL failure. */
TEST(PdschSweepK, P2ProbeEqualToMainIsNotScoredTwice) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4); st.p2 = true;
  const nr_td_outcome_t o[3] = {{2, NR_TD_FULL_TB, NR_TD_FAIL, false}, {2, NR_TD_CB_PROBE, NR_TD_FAIL, true},
                                {5, NR_TD_CB_PROBE, NR_TD_FAIL, true}};
  nr_pdsch_config_sweep_feed_k(&st, o, 3);
  EXPECT_EQ(st.trials[2], 1u);
  EXPECT_EQ(st.trials[5], 1u);
}

/* Fix round 1: side/p2 are configuration and survive the catalog rebuild the runtime uses on reopen /
 * prior restore (template memcpy and the init_legal fallback), while the evidence is discarded. */
TEST(PdschSweepK, RebuildPreservesSideAndP2) {
  static nr_pdsch_config_sweep_state_t st;
  static nr_td_side_info_t s = {};
  ASSERT_GT(nr_pdsch_config_sweep_init_legal(&st, 2, 0, test_legal), 0);
  for (int round = 0; round < 2; round++) { /* round 0 builds the shared template, round 1 copies it */
    st.side = &s; st.p2 = true; st.trials[1] = 9; st.probe_pass[1] = 4;
    ASSERT_GT(nr_pdsch_config_sweep_rebuild(&st, 2, 0, test_legal), 0);
    EXPECT_EQ(st.side, &s); EXPECT_TRUE(st.p2);
    EXPECT_EQ(st.trials[1], 0u); EXPECT_EQ(st.probe_pass[1], 0);
  }
  st.side = &s; st.p2 = true; /* no legality: init_legal fallback path */
  nr_pdsch_config_sweep_rebuild(&st, 2, 0, nullptr);
  EXPECT_EQ(st.side, &s); EXPECT_TRUE(st.p2);
  st.side = nullptr;
}
/* A brand-new runtime context is neutral even when its buffer is recycled. */
TEST(PdschSweepK, FreshRuntimeContextIsNeutral) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_sweep_ticket_t t{}; nr_pdsch_cfg_hypothesis_t h;
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x77, 0x4711, 0, 2, 0, test_legal, &t, &h));
  static nr_pdsch_config_sweep_state_t snap;
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t, &snap));
  EXPECT_EQ(snap.side, nullptr); EXPECT_FALSE(snap.p2);
  nr_pdsch_config_sweep_reset_all();
}
/* A weight that makes every key NaN keeps the round exactly the shuffle (non-finite key = 0). */
TEST(PdschSweepK, NonFiniteKeysKeepShuffleOrder) {
  static nr_pdsch_config_sweep_state_t a, b;
  nr_pdsch_config_sweep_init(&a, 4); nr_pdsch_config_sweep_init(&b, 4);
  nr_td_side_info_t s = {}; s.obs_dmrs_mask = -1; s.obs_qm = -1; s.obs_last_symbol = -1;
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1;
  s.w_probe = NAN; /* the score's own weights are guarded by "w > 0"; the probe bonus is not */
  for (int i = 0; i < b.n_hyp; i++)
    b.probe_pass[i] = 1; /* every key NaN */
  b.side = &s;
  for (int i = 0; i < 2 * a.n_hyp; i++) {
    nr_pdsch_cfg_hypothesis_t h; ASSERT_EQ(nr_pdsch_config_sweep_next(&a, &h), nr_pdsch_config_sweep_next(&b, &h)) << i;
  }
  b.side = nullptr;
}
TEST(PdschSweepEquiv, SingleIsBitIdenticalToFeed) {
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4); memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  for (int t = 0; t < 20000; t++) {
    nr_pdsch_cfg_hypothesis_t h; const int i = nr_pdsch_config_sweep_next(a.get(), &h);
    const int j = nr_pdsch_config_sweep_next(b.get(), &h); ASSERT_EQ(i, j);
    const bool ok = (i == 7) && (t % 3 == 0);
    const int wa = nr_pdsch_config_sweep_feed(a.get(), i, ok), wb = nr_pdsch_config_sweep_feed_equiv(b.get(), &i, 1, ok, true);
    ASSERT_EQ(wa, wb); if (wa >= 0) break;
  }
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
  EXPECT_EQ(0, memcmp(a->ok, b->ok, sizeof(a->ok)));
}
TEST(PdschSweepEquiv, CreditsEveryMemberOnceIgnoringDuplicates) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  const int idx[] = {3, 5, 5, 9, -1, 1 << 20};
  nr_pdsch_config_sweep_feed_equiv(s.get(), idx, 6, true, true);
  EXPECT_EQ(s->trials[3], 1u); EXPECT_EQ(s->trials[5], 1u); EXPECT_EQ(s->trials[9], 1u);
  EXPECT_EQ(s->ok[3], 1u); EXPECT_EQ(s->ok[5], 1u); EXPECT_EQ(s->ok[9], 1u);
}
/* Same as SingleIsBitIdenticalToFeed on a pruned catalog that converges, so the KL separation and the
 * decision path (refactored into sweep_decide) are compared up to the winner. */
TEST(PdschSweepEquiv, SingleIsBitIdenticalToFeedUntilConvergence) {
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4);
  ASSERT_GT(nr_pdsch_config_sweep_prune_to(a.get(), a->hyp[0].mcs_table, a->hyp[0].dmrs_add_pos, a->hyp[0].dmrs_max_len), 0);
  memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  unsigned seed = 7;
  int t = 0;
  for (; t < 200000 && a->winner < 0; t++) {
    nr_pdsch_cfg_hypothesis_t h; const int i = nr_pdsch_config_sweep_next(a.get(), &h);
    ASSERT_EQ(i, nr_pdsch_config_sweep_next(b.get(), &h));
    const bool ok = (i == 3) && (rand_r(&seed) % 100 < 60);
    ASSERT_EQ(nr_pdsch_config_sweep_feed(a.get(), i, ok), nr_pdsch_config_sweep_feed_equiv(b.get(), &i, 1, ok, true)) << t;
  }
  EXPECT_EQ(a->winner, 3);
  EXPECT_EQ(b->winner, 3);
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
  EXPECT_EQ(0, memcmp(a->ok, b->ok, sizeof(a->ok)));
  /* After the winner, both keep returning it without crediting. */
  const int k = 5;
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(b.get(), &k, 1, true, true), 3);
  EXPECT_EQ(b->trials[5], a->trials[5]);
}
/* Degenerate inputs mirror _feed: NULL state -> -1; n < 1 or no valid index -> current winner, nothing credited. */
TEST(PdschSweepEquiv, DegenerateInputsMirrorFeed) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  const int bad[] = {-1, 1 << 20};
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(nullptr, bad, 2, true, true), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), bad, 0, true, true), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), nullptr, 1, true, true), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), bad, 2, true, true), -1);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(s->trials[i], 0u) << i;
}
/* The separation check fires when ANY credited member (not only idx[0]) reaches a multiple of 16 trials. */
TEST(PdschSweepEquiv, CheckFiresOnNonFirstMember) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  ASSERT_GT(nr_pdsch_config_sweep_prune_to(s.get(), s->hyp[0].mcs_table, s->hyp[0].dmrs_add_pos, s->hyp[0].dmrs_max_len), 0);
  for (int i = 0; i < s->n_hyp; i++) { s->trials[i] = 100; s->ok[i] = 0; }
  s->trials[3] = 79; s->ok[3] = 78; /* reaches 80 = 5 x 16 on this grant */
  s->trials[4] = 200; s->ok[4] = 0; /* reaches 201: alone it would not trigger the check */
  auto t = std::make_unique<nr_pdsch_config_sweep_state_t>(); memcpy((void *)t.get(), (void *)s.get(), sizeof(*s));
  EXPECT_EQ(nr_pdsch_config_sweep_feed(t.get(), 4, true), -1); /* control: no check on 201 trials */
  const int idx[] = {4, 3};
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), idx, 2, true, true), 3);
}
/* An invalid decoded index credits NO member, even valid ones: the class is defined relative to idx[0]. */
TEST(PdschSweepEquiv, InvalidDecodedIndexCreditsNothing) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  const int idx_neg[] = {-1, 3, 5}, idx_big[] = {1 << 20, 3, 5};
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), idx_neg, 3, true, true), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), idx_big, 3, false, true), -1);
  for (int i = 0; i < s->n_hyp; i++) {
    ASSERT_EQ(s->trials[i], 0u) << i;
    ASSERT_EQ(s->ok[i], 0u) << i;
  }
}

// ---- BC3: dormant (reversible) masks + fail-open (blind-convergence spec section 4) -------------------------
static bool keep_even(const nr_pdsch_cfg_hypothesis_t *h, const void *) { return (h->tda_length % 2) == 0; }
static bool keep_none(const nr_pdsch_cfg_hypothesis_t *, const void *) { return false; }
static bool keep_k0_0(const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->k0 == 0; }

TEST(PdschSweepDormant, NoMasksIsBitIdentical) {
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4); memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(b.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr), 0);
  nr_pdsch_config_sweep_clear_dormant(b.get(), NR_TD_DORMANT_PRIOR);
  for (int t = 0; t < 600000 && nr_pdsch_config_sweep_winner(a.get()) < 0; t++) {
    nr_pdsch_cfg_hypothesis_t h; const int i = nr_pdsch_config_sweep_next(a.get(), &h), j = nr_pdsch_config_sweep_next(b.get(), &h);
    ASSERT_EQ(i, j); const bool ok = (i == 11) && (((unsigned)t * 2654435761u) >> 16) % 10 < 7; // truth = 11 at ~70 %
    ASSERT_EQ(nr_pdsch_config_sweep_feed(a.get(), i, ok), nr_pdsch_config_sweep_feed(b.get(), j, ok));
  }
  ASSERT_GE(nr_pdsch_config_sweep_winner(a.get()), 0); // the loop must actually converge, or identity proves little
  ASSERT_GE(nr_pdsch_config_sweep_winner(b.get()), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(a.get()), nr_pdsch_config_sweep_winner(b.get()));
  EXPECT_EQ(a->n_hyp, b->n_hyp);
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
}
TEST(PdschSweepDormant, NoMasksIsBitIdenticalFeedKAndEquiv) {
  // Same sequence through next_k + feed_k (with P2 probes) and feed_equiv: masks set then cleared == never set.
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4); a->p2 = true; memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(b.get(), NR_TD_DORMANT_FIELD_BASE, keep_k0_0, nullptr), 0);
  nr_pdsch_config_sweep_clear_dormant(b.get(), NR_TD_DORMANT_FIELD_BASE);
  for (int t = 0; t < 600000 && nr_pdsch_config_sweep_winner(a.get()) < 0; t++) {
    int ia[NR_TD_MAX_K], ib[NR_TD_MAX_K]; nr_pdsch_cfg_hypothesis_t oa[NR_TD_MAX_K], ob[NR_TD_MAX_K];
    const int na = nr_pdsch_config_sweep_next_k(a.get(), 3, ia, oa), nb = nr_pdsch_config_sweep_next_k(b.get(), 3, ib, ob);
    ASSERT_EQ(na, nb);
    nr_td_outcome_t oc[NR_TD_MAX_K];
    for (int k = 0; k < na; k++) {
      ASSERT_EQ(ia[k], ib[k]);
      oc[k] = {ia[k], (uint8_t)(k ? NR_TD_CB_PROBE : NR_TD_FULL_TB),
               (uint8_t)(k == 0 ? ((ia[0] == 5 && (((unsigned)t * 2654435761u) >> 16) % 10 < 7) ? NR_TD_PASS : NR_TD_FAIL) : NR_TD_FAIL), k > 0};
    }
    ASSERT_EQ(nr_pdsch_config_sweep_feed_k(a.get(), oc, na), nr_pdsch_config_sweep_feed_k(b.get(), oc, na));
    const int e[2] = {ia[0], (ia[0] + 7) % a->n_hyp}; // non-truth class: both members always fail
    if (t % 4 == 0 && e[0] != 5 && e[1] != 5)
      ASSERT_EQ(nr_pdsch_config_sweep_feed_equiv(a.get(), e, 2, false, true),
                nr_pdsch_config_sweep_feed_equiv(b.get(), e, 2, false, true));
  }
  ASSERT_GE(nr_pdsch_config_sweep_winner(a.get()), 0); // converged on both
  ASSERT_GE(nr_pdsch_config_sweep_winner(b.get()), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(a.get()), nr_pdsch_config_sweep_winner(b.get()));
  EXPECT_EQ(a->n_hyp, b->n_hyp);
  ASSERT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
  ASSERT_EQ(a->since_pass, b->since_pass);
}
TEST(PdschSweepDormant, DormantNeverSelectedAndAccumulatesNothing) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE, keep_even, nullptr);
  for (int t = 0; t < 5000; t++) {
    nr_pdsch_cfg_hypothesis_t h; const int i = nr_pdsch_config_sweep_next(s.get(), &h);
    ASSERT_TRUE(nr_pdsch_config_sweep_is_active(s.get(), i)); nr_pdsch_config_sweep_feed(s.get(), i, false);
  }
  for (int i = 0; i < s->n_hyp; i++) if (!nr_pdsch_config_sweep_is_active(s.get(), i)) ASSERT_EQ(s->trials[i], 0u);
}
TEST(PdschSweepDormant, NextKProbesAreActiveAndExploitHotMustBeActive) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  int odd = -1; for (int i = 0; i < s->n_hyp && odd < 0; i++) if (s->hyp[i].tda_length % 2) odd = i;
  for (int k = 0; k < 20; k++) nr_pdsch_config_sweep_feed(s.get(), odd, true); // hot while still active
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  ASSERT_FALSE(nr_pdsch_config_sweep_is_active(s.get(), odd));
  for (int t = 0; t < 3000; t++) {
    int idx[NR_TD_MAX_K]; nr_pdsch_cfg_hypothesis_t out[NR_TD_MAX_K];
    const int n = nr_pdsch_config_sweep_next_k(s.get(), 4, idx, out);
    for (int k = 0; k < n; k++) ASSERT_TRUE(nr_pdsch_config_sweep_is_active(s.get(), idx[k]));
    nr_pdsch_config_sweep_feed(s.get(), idx[0], false);
  }
}
TEST(PdschSweepDormant, RngConsumptionIndependentOfMasks) {
  // The shuffle covers all n_hyp whatever is dormant: after one full round the RNG state equals the unmasked one.
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4); memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  nr_pdsch_config_sweep_set_dormant(b.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_cfg_hypothesis_t h;
  for (int t = 0; t < a->n_hyp; t++) nr_pdsch_config_sweep_next(a.get(), &h); // exactly one round
  for (int t = 0; t < nr_pdsch_config_sweep_n_active(b.get()); t++) nr_pdsch_config_sweep_next(b.get(), &h);
  nr_pdsch_config_sweep_next(a.get(), &h); nr_pdsch_config_sweep_next(b.get(), &h); // both start round 2
  // a consumed 2 shuffles; b consumed 2 shuffles once the all-inactive tail of round 1 was skipped
  EXPECT_EQ(a->random_state, b->random_state);
}
TEST(PdschSweepDormant, FeedOnDormantIsIgnored) { /* Review Focus 3 */
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  int odd = -1; for (int i = 0; i < s->n_hyp && odd < 0; i++) if (s->hyp[i].tda_length % 2) odd = i;
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_config_sweep_feed(s.get(), odd, true); nr_pdsch_config_sweep_feed_equiv(s.get(), &odd, 1, true, true);
  nr_td_outcome_t oc[2] = {{odd, NR_TD_FULL_TB, NR_TD_PASS, false}, {odd, NR_TD_CB_PROBE, NR_TD_FAIL, true}};
  s->p2 = true; nr_pdsch_config_sweep_feed_k(s.get(), oc, 2);
  EXPECT_EQ(s->trials[odd], 0u); EXPECT_EQ(s->ok[odd], 0u);
  EXPECT_EQ(s->probe_fail[odd], 0u);
  EXPECT_EQ(s->since_pass, 0u);
}
TEST(PdschSweepDormant, DormantRefusesToEmptyCatalogue) { /* Review Focus 2 */
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  EXPECT_EQ(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_none, nullptr), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  EXPECT_EQ(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_CAUSES, keep_even, nullptr), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_set_dormant(s.get(), -1, keep_even, nullptr), -1);
  // A second cause that together with the first would empty the set is also refused, leaving the first intact.
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  const int n1 = nr_pdsch_config_sweep_n_active(s.get());
  EXPECT_EQ(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE,
            [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->tda_length % 2 == 1; }, nullptr), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), n1);
}
TEST(PdschSweepDormant, ClearRestoresOnlyItsCause) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  const int n0 = s->n_hyp;
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  const int n1 = nr_pdsch_config_sweep_n_active(s.get());
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE, [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->k0 == 0; }, nullptr);
  ASSERT_LT(nr_pdsch_config_sweep_n_active(s.get()), n1);
  nr_pdsch_config_sweep_clear_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), n1); EXPECT_LT(n1, n0);
}
TEST(PdschSweepDormant, AcceptanceRangesOverActiveSetOnly) {
  // A dormant hypothesis with a spotless record must not block (or win) the decision among active ones.
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  int odd = -1, truth = -1;
  for (int i = 0; i < s->n_hyp; i++) { if (s->hyp[i].tda_length % 2 && odd < 0) odd = i; if (s->hyp[i].tda_length % 2 == 0 && truth < 0) truth = i; }
  for (int k = 0; k < 40; k++) nr_pdsch_config_sweep_feed(s.get(), odd, true); // dormant-to-be: 40/40, no winner yet
  ASSERT_LT(nr_pdsch_config_sweep_winner(s.get()), 0);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  drive(*s, truth, 0.7, 0.0, 400000);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(s.get()), truth);
  EXPECT_EQ(s->trials[odd], 40u);
}
TEST(PdschSweepDormant, FailOpenCountsTrialsNotTime) { /* Review Focus 1 */
  // Variant used: all active hypotheses fail, so no winner can form before `need` trials (fallback needs 300*na > need);
  // since_pass is also asserted directly.
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  EXPECT_FALSE(nr_pdsch_config_sweep_fail_open_due(s.get(), 1e-3, 0.05)); /* no trials yet */
  const int na = nr_pdsch_config_sweep_n_active(s.get());
  const int need = (int)ceil(na * log(1e3) / 0.05);
  for (int t = 0; t < need - 1; t++) { nr_pdsch_cfg_hypothesis_t h; nr_pdsch_config_sweep_feed(s.get(), nr_pdsch_config_sweep_next(s.get(), &h), false); }
  EXPECT_EQ(s->since_pass, (uint32_t)(need - 1));
  EXPECT_LT(nr_pdsch_config_sweep_winner(s.get()), 0);
  EXPECT_FALSE(nr_pdsch_config_sweep_fail_open_due(s.get(), 1e-3, 0.05));
  { nr_pdsch_cfg_hypothesis_t h; nr_pdsch_config_sweep_feed(s.get(), nr_pdsch_config_sweep_next(s.get(), &h), false); }
  EXPECT_EQ(s->since_pass, (uint32_t)need);
  EXPECT_TRUE(nr_pdsch_config_sweep_fail_open_due(s.get(), 1e-3, 0.05));
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  EXPECT_FALSE(nr_pdsch_config_sweep_fail_open_due(s.get(), 1e-3, 0.05)); /* already open */
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  nr_pdsch_config_sweep_set_fail_open(s.get(), false);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), na);
}
TEST(PdschSweepDormant, SincePassSemantics) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  int even = -1, odd = -1;
  for (int i = 0; i < s->n_hyp; i++) { if (s->hyp[i].tda_length % 2 == 0 && even < 0) even = i; if (s->hyp[i].tda_length % 2 && odd < 0) odd = i; }
  for (int k = 0; k < 5; k++) nr_pdsch_config_sweep_feed(s.get(), even, false);
  EXPECT_EQ(s->since_pass, 5u);
  const int e2[2] = {even, even + 1};
  nr_pdsch_config_sweep_feed_equiv(s.get(), e2, 2, false, true); // one call, two credited: +1
  EXPECT_EQ(s->since_pass, 6u);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  EXPECT_EQ(s->since_pass, 0u); // the active set changed
  nr_pdsch_config_sweep_feed(s.get(), even, false); EXPECT_EQ(s->since_pass, 1u);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr); // no bit changes
  EXPECT_EQ(s->since_pass, 1u);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true); EXPECT_EQ(s->since_pass, 0u);
  nr_pdsch_config_sweep_feed(s.get(), even, false);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true); EXPECT_EQ(s->since_pass, 1u); // no change
  nr_pdsch_config_sweep_set_fail_open(s.get(), false); EXPECT_EQ(s->since_pass, 0u);
  nr_pdsch_config_sweep_feed(s.get(), even, false);
  nr_pdsch_config_sweep_clear_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE); // empty cause: no change
  EXPECT_EQ(s->since_pass, 1u);
  nr_pdsch_config_sweep_clear_dormant(s.get(), NR_TD_DORMANT_PRIOR); EXPECT_EQ(s->since_pass, 0u);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_config_sweep_feed(s.get(), odd, true); // PASS on a dormant hypothesis: ignored, no reset
  EXPECT_EQ(s->since_pass, 0u);
  nr_pdsch_config_sweep_feed(s.get(), even, false); nr_pdsch_config_sweep_feed(s.get(), even, false);
  EXPECT_EQ(s->since_pass, 2u);
  // feed_k: main FAIL + admissible probe FAIL under p2 is ONE call: +1; a probe PASS neither counts nor resets.
  s->p2 = true;
  nr_td_outcome_t o1[3] = {{even, NR_TD_FULL_TB, NR_TD_FAIL, false}, {even + 2, NR_TD_CB_PROBE, NR_TD_FAIL, true},
                           {even + 4, NR_TD_CB_PROBE, NR_TD_PASS, true}};
  nr_pdsch_config_sweep_feed_k(s.get(), o1, 3);
  EXPECT_EQ(s->since_pass, 3u);
  nr_td_outcome_t o2[2] = {{even, NR_TD_FULL_TB, NR_TD_INCONCLUSIVE, false}, {even + 2, NR_TD_CB_PROBE, NR_TD_PASS, true}};
  nr_pdsch_config_sweep_feed_k(s.get(), o2, 2); // nothing credited
  EXPECT_EQ(s->since_pass, 3u);
  nr_pdsch_config_sweep_feed(s.get(), even, true); // PASS on an active hypothesis resets
  EXPECT_EQ(s->since_pass, 0u);
}
TEST(PdschSweepDormant, DestructivePruneCompactsMasks) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_config_sweep_prune_keep(s.get(), [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->k0 == 0; }, nullptr);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), s->hyp[i].tda_length % 2 == 0);
  for (int i = s->n_hyp; i < NR_PDSCH_SWEEP_MAX_HYP; i++) // no stale bits beyond the live catalogue
    for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++) ASSERT_EQ((s->dormant[c][i / 64] >> (i % 64)) & 1u, 0u);
}
TEST(PdschSweepDormant, PruneKeepResetsEvidenceAndRefusesEmpty) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  const int n0 = s->n_hyp;
  nr_pdsch_config_sweep_feed(s.get(), 3, false);
  EXPECT_EQ(nr_pdsch_config_sweep_prune_keep(s.get(), keep_none, nullptr), 0);
  EXPECT_EQ(s->n_hyp, n0); EXPECT_EQ(s->trials[3], 1u);
  const int n = nr_pdsch_config_sweep_prune_keep(s.get(), keep_k0_0, nullptr);
  EXPECT_EQ(n, s->n_hyp); EXPECT_LT(n, n0);
  for (int i = 0; i < s->n_hyp; i++) { ASSERT_EQ(s->trials[i], 0u); ASSERT_EQ(s->hyp[i].k0, 0); }
  EXPECT_EQ(s->since_pass, 0u); EXPECT_EQ(s->cursor, 0);
}
TEST(PdschSweepDormant, PruneNeverLeavesZeroActive) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_config_sweep_prune_keep(s.get(), [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->tda_length % 2 == 1; }, nullptr);
  EXPECT_GT(nr_pdsch_config_sweep_n_active(s.get()), 0);
}
TEST(PdschSweepDormant, RebuildPreservesMasksAndFailOpen) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  const int na = nr_pdsch_config_sweep_n_active(s.get());
  nr_pdsch_config_sweep_set_fail_open(s.get(), false);
  const int n_before = nr_pdsch_config_sweep_n_active(s.get());
  nr_pdsch_config_sweep_feed(s.get(), 0, false);
  nr_pdsch_config_sweep_rebuild(s.get(), 4, 0, nullptr);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), n_before); EXPECT_LT(n_before, na);
  EXPECT_EQ(s->since_pass, 0u); EXPECT_EQ(s->trials[0], 0u);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  nr_pdsch_config_sweep_rebuild(s.get(), 4, 0, nullptr);
  EXPECT_TRUE(s->fail_open);
}
TEST(PdschSweepDormant, DormantDecodedIdxZeroCreditsNothingInEquiv) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  int even = -1, odd = -1;
  for (int i = 0; i < s->n_hyp; i++) { if (s->hyp[i].tda_length % 2 == 0 && even < 0) even = i; if (s->hyp[i].tda_length % 2 && odd < 0) odd = i; }
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  const int e[2] = {odd, even}; // dormant idx[0], active equivalent member
  nr_pdsch_config_sweep_feed_equiv(s.get(), e, 2, true, true);
  EXPECT_EQ(s->trials[odd], 0u); EXPECT_EQ(s->trials[even], 0u); EXPECT_EQ(s->since_pass, 0u);
}
TEST(PdschSweepDormant, K0LayerInheritsDormancy) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE, keep_even, nullptr);
  const int n0 = s->n_hyp;
  const int added = nr_pdsch_config_sweep_add_k0_layer(s.get(), 3);
  ASSERT_GT(added, 0);
  ASSERT_EQ(s->n_hyp, n0 + added);
  int new_dormant = 0;
  for (int i = n0; i < s->n_hyp; i++) {
    ASSERT_EQ(s->hyp[i].k0, 3);
    ASSERT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), s->hyp[i].tda_length % 2 == 0) << i;
    new_dormant += !nr_pdsch_config_sweep_is_active(s.get(), i);
  }
  EXPECT_GT(new_dormant, 0);
}

/* ---- Lever C (BC2): CRC-pass acceptance, default off ---- */
/* The legacy feed_equiv/feed_attr carry no pick kind and give no fast-path credit (EXPLOIT); the fast-path tests use the _ex forms with EXPLORE. */
static int fe(nr_pdsch_config_sweep_state_t *s, const int *idx, int n, bool tb, bool nd)
{
  return nr_pdsch_config_sweep_feed_equiv_cx(s, idx, n, tb, nd, NR_TD_PICK_EXPLORE, true);
}
static int fa(nr_pdsch_config_sweep_state_t *s, int idx0, const int *cls, int n_cls, bool tb, bool nd)
{
  return nr_pdsch_config_sweep_feed_attr_cx(s, idx0, cls, n_cls, tb, nd, NR_TD_PICK_EXPLORE, true);
}
TEST(PdschSweepCrcAccept, MValues)
{
  EXPECT_EQ(nr_pdsch_config_sweep_crc_accept_m(10, 100), 2);
  EXPECT_EQ(nr_pdsch_config_sweep_crc_accept_m(750, 1000), 3);
  EXPECT_EQ(nr_pdsch_config_sweep_crc_accept_m(1, 0), 2);
}
TEST(PdschSweepCrcAccept, TwoUniquePassesDecideWhenClean)
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  s->crc_accept = true; s->sib_pmin = 0; /* these tests exercise fix A only; the sibling guard has its own tests */
  const int a = 7;
  int w = fe(s.get(), &a, 1, true, true);
  EXPECT_EQ(w, -1);
  w = fe(s.get(), &a, 1, true, true);
  EXPECT_EQ(w, 7);
}
TEST(PdschSweepCrcAccept, SecondUniquePasserBlocksTheRule)
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  s->crc_accept = true; s->sib_pmin = 0; /* these tests exercise fix A only; the sibling guard has its own tests */
  const int a = 7, b = 8;
  fe(s.get(), &a, 1, true, true);
  fe(s.get(), &b, 1, true, true);
  EXPECT_EQ(fe(s.get(), &a, 1, true, true), -1);
  EXPECT_TRUE(s->crc_accept_blocked);
}
TEST(PdschSweepCrcAccept, SharedPassIsNotUnique)
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  s->crc_accept = true; s->sib_pmin = 0; /* these tests exercise fix A only; the sibling guard has its own tests */
  const int cls[] = {7, 9};
  fe(s.get(), cls, 2, true, true);
  fe(s.get(), cls, 2, true, true);
  EXPECT_EQ(s->ok_unique[7], 0);
  EXPECT_EQ(s->winner, -1);
}
TEST(PdschSweepCrcAccept, RetransmissionPassIsNotUnique)
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  s->crc_accept = true; s->sib_pmin = 0; /* these tests exercise fix A only; the sibling guard has its own tests */
  const int a = 7;
  fe(s.get(), &a, 1, true, true);
  EXPECT_EQ(fe(s.get(), &a, 1, true, false), -1); /* HARQ retx of the same TB */
  EXPECT_EQ(s->ok_unique[7], 1);
}

/* ---- Lever C review fixes ---- */
static bool keep_not_arg(const nr_pdsch_cfg_hypothesis_t *h, const void *arg) { return h != (const nr_pdsch_cfg_hypothesis_t *)arg; }
static std::unique_ptr<nr_pdsch_config_sweep_state_t> crc_state()
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  s->crc_accept = true; s->sib_pmin = 0; /* these tests exercise fix A only; the sibling guard has its own tests */
  return s;
}
static void feed_new(nr_pdsch_config_sweep_state_t *s, const int *cls, int n, int times, int *last = nullptr)
{
  for (int i = 0; i < times; i++) {
    const int w = fe(s, cls, n, true, true);
    if (last)
      *last = w;
  }
}
TEST(PdschSweepCrcAccept, DormantTwinMakesPassNonUnique)
{
  auto s = crc_state();
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_not_arg, &s->hyp[9]), 0);
  ASSERT_FALSE(nr_pdsch_config_sweep_is_active(s.get(), 9));
  const int cls[] = {7, 9};
  int w = -1;
  feed_new(s.get(), cls, 2, 2, &w);
  EXPECT_EQ(w, -1);
  EXPECT_EQ(s->ok_unique[7], 0);
  EXPECT_EQ(s->trials[7], 2u); /* crediting itself is unchanged */
  EXPECT_EQ(s->trials[9], 0u);
}
TEST(PdschSweepCrcAccept, ActiveSetChangeRestartsLeverC)
{
  const int a = 7;
  {
    auto s = crc_state();
    feed_new(s.get(), &a, 1, 1);
    EXPECT_EQ(s->ok_unique[7], 1);
    ASSERT_GT(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_not_arg, &s->hyp[9]), 0);
    EXPECT_EQ(s->ok_unique[7], 0);
    int w = -1;
    feed_new(s.get(), &a, 1, 1, &w);
    EXPECT_EQ(w, -1);
    EXPECT_EQ(s->ok_unique[7], 1);
  }
  { /* clear_dormant and a real fail_open toggle restart it too, and unblock */
    auto s = crc_state();
    ASSERT_GT(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_not_arg, &s->hyp[9]), 0);
    const int b = 8;
    feed_new(s.get(), &a, 1, 1);
    feed_new(s.get(), &b, 1, 1);
    EXPECT_TRUE(s->crc_accept_blocked);
    nr_pdsch_config_sweep_clear_dormant(s.get(), NR_TD_DORMANT_PRIOR);
    EXPECT_FALSE(s->crc_accept_blocked);
    EXPECT_EQ(s->ok_unique[7], 0);
    feed_new(s.get(), &a, 1, 1);
    nr_pdsch_config_sweep_set_fail_open(s.get(), true);
    EXPECT_EQ(s->ok_unique[7], 0);
  }
}
TEST(PdschSweepCrcAccept, DormantNeverWinsNorBlocks)
{
  auto s = crc_state();
  const int a = 7, d = 9;
  feed_new(s.get(), &d, 1, 1); /* D earns a unique pass while active */
  ASSERT_EQ(s->ok_unique[9], 1);
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_not_arg, &s->hyp[9]), 0);
  /* set_dormant restarted the evidence; plant a stale unique count on the now-dormant hypothesis directly */
  s->ok_unique[9] = 5;
  int w = -1;
  feed_new(s.get(), &d, 1, 3, &w); /* dormant idx[0] credits nothing */
  EXPECT_EQ(w, -1);
  feed_new(s.get(), &a, 1, 1, &w);
  EXPECT_EQ(w, -1);
  EXPECT_FALSE(s->crc_accept_blocked); /* the dormant D with ok_unique>0 does not block */
  feed_new(s.get(), &a, 1, 1, &w);
  EXPECT_EQ(w, 7); /* and never wins over the active leader */
}
TEST(PdschSweepCrcAccept, BlockedStaysBlockedUntilPruneOrRebuild)
{
  auto s = crc_state();
  const int a = 7, b = 8;
  feed_new(s.get(), &a, 1, 1);
  feed_new(s.get(), &b, 1, 1);
  ASSERT_TRUE(s->crc_accept_blocked);
  int w = -1;
  feed_new(s.get(), &a, 1, 5, &w);
  EXPECT_EQ(w, -1);
  EXPECT_TRUE(s->crc_accept_blocked);
  /* prune_keep clears evidence and the block (indices move) */
  ASSERT_GT(nr_pdsch_config_sweep_prune_keep(s.get(), keep_even, nullptr), 0);
  ASSERT_LT(s->n_hyp, NR_PDSCH_SWEEP_MAX_HYP);
  EXPECT_FALSE(s->crc_accept_blocked);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(s->ok_unique[i], 0);
  EXPECT_TRUE(s->crc_accept);
}
TEST(PdschSweepCrcAccept, RebuildClearsEvidenceKeepsFlag)
{
  auto s = crc_state();
  const int a = 7;
  feed_new(s.get(), &a, 1, 1);
  nr_pdsch_config_sweep_rebuild(s.get(), 4, 0, nullptr);
  EXPECT_TRUE(s->crc_accept);
  EXPECT_FALSE(s->crc_accept_blocked);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(s->ok_unique[i], 0);
}
TEST(PdschSweepCrcAccept, LargeTUsesTmaxAndNActive)
{
  auto s = crc_state();
  ASSERT_GT(s->n_hyp, 100);
  const int a = 7;
  for (int i = 0; i < 1000; i++) /* T_max ~ 1000 on another hypothesis, as failures */
    fe(s.get(), &a, 1, false, true);
  if (s->winner >= 0)
    GTEST_SKIP() << "KL rule decided first";
  EXPECT_GE(nr_pdsch_config_sweep_crc_accept_m(s->n_hyp, 1000), 2);
  const int m = nr_pdsch_config_sweep_crc_accept_m(750, 1000);
  ASSERT_EQ(m, 3);
  feed_new(s.get(), &a, 1, 2);
  const int m_now = nr_pdsch_config_sweep_crc_accept_m(nr_pdsch_config_sweep_n_active(s.get()), s->trials[7]);
  std::cout << "n_active=" << nr_pdsch_config_sweep_n_active(s.get()) << " T=" << s->trials[7] << " m*=" << m_now << "\n";
  EXPECT_EQ(s->ok_unique[7] >= m_now, s->winner == 7); /* accept iff the count reached m*(n_active, T_max) */
}

/* ---- Lever P (partition / geometry acceptance) and feed_attr (BC2b) ---- */
static std::unique_ptr<nr_pdsch_config_sweep_state_t> geom_state()
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  s->geom_pin = true; s->sib_pmin = 0;
  return s;
}
static int other_geom(const nr_pdsch_config_sweep_state_t *s, int a)
{
  for (int i = 0; i < s->n_hyp; i++)
    if (nr_td_geom_key(&s->hyp[i]) != nr_td_geom_key(&s->hyp[a]))
      return i;
  return -1;
}
TEST(PdschSweepGeomPin, TwoPassesPinTheGeometryReversibly)
{
  auto s = geom_state();
  const int a = 7;
  const uint64_t g = nr_td_geom_key(&s->hyp[a]);
  fe(s.get(), &a, 1, true, true);
  fe(s.get(), &a, 1, true, true);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), nr_td_geom_key(&s->hyp[i]) == g);
  EXPECT_EQ(s->n_geom, 0); /* the pin's active-set change restarted the evidence */
  EXPECT_FALSE(s->geom_blocked);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, SecondGeometryWithAPassBlocks)
{
  auto s = geom_state();
  const int a = 7, b = other_geom(s.get(), a);
  ASSERT_GE(b, 0);
  fe(s.get(), &a, 1, true, true);
  fe(s.get(), &b, 1, true, true);
  fe(s.get(), &a, 1, true, true);
  EXPECT_TRUE(s->geom_blocked);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, RetransmissionDoesNotCount)
{
  auto s = geom_state();
  const int a = 7;
  fe(s.get(), &a, 1, true, true);
  fe(s.get(), &a, 1, true, false);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, FailedDecodeDoesNotCount)
{
  auto s = geom_state();
  const int a = 7;
  for (int i = 0; i < 3; i++)
    fe(s.get(), &a, 1, false, true);
  EXPECT_EQ(s->n_geom, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, OffIsBitIdentical)
{
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4);
  memcpy((void *)b.get(), (void *)a.get(), sizeof(*a)); /* b: geom_pin false (default) -- both identical; a drives the reference path */
  for (int t = 0; t < 600000 && nr_pdsch_config_sweep_winner(a.get()) < 0; t++) {
    nr_pdsch_cfg_hypothesis_t h;
    const int i = nr_pdsch_config_sweep_next(a.get(), &h), j = nr_pdsch_config_sweep_next(b.get(), &h);
    ASSERT_EQ(i, j);
    const bool ok = (i == 11) && (((unsigned)t * 2654435761u) >> 16) % 10 < 7;
    ASSERT_EQ(nr_pdsch_config_sweep_feed(a.get(), i, ok), fe(b.get(), &i, 1, ok, true));
  }
  ASSERT_GE(nr_pdsch_config_sweep_winner(a.get()), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(a.get()), nr_pdsch_config_sweep_winner(b.get()));
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
  EXPECT_EQ(0, memcmp(a->ok, b->ok, sizeof(a->ok)));
  EXPECT_EQ(a->since_pass, b->since_pass);
  EXPECT_EQ(b->n_geom, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(b.get()), b->n_hyp);
}
TEST(PdschSweepGeomPin, OffNeverTouchesGeometryState)
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  const int a = 7;
  for (int i = 0; i < 5; i++)
    fe(s.get(), &a, 1, true, true);
  EXPECT_EQ(s->n_geom, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, SlotOverflowBlocks)
{
  auto s = geom_state();
  std::vector<int> reps;
  std::vector<uint64_t> seen;
  for (int i = 0; i < s->n_hyp && (int)reps.size() < NR_TD_GEOM_SLOTS + 1; i++) {
    const uint64_t k = nr_td_geom_key(&s->hyp[i]);
    if (std::find(seen.begin(), seen.end(), k) == seen.end()) { seen.push_back(k); reps.push_back(i); }
  }
  ASSERT_EQ((int)reps.size(), NR_TD_GEOM_SLOTS + 1);
  /* every pass on a distinct geometry: the second slot already blocks; with <= 8 distinct it must be blocked by then */
  for (int r : reps)
    fe(s.get(), &r, 1, true, true);
  EXPECT_TRUE(s->geom_blocked);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, SingleActiveGeometryNeverRePins)
{
  /* Only one geometry is active (cause PRIOR): n_groups_active == 1, so passes pin nothing and the evidence is not restarted
   * (no set_dormant churn). set_dormant can never refuse a pin here: idx[0] is active and always in the kept group. */
  auto s = geom_state();
  const int a = 7;
  struct Arg { uint64_t g; } arg{nr_td_geom_key(&s->hyp[a])};
  auto keep_g = [](const nr_pdsch_cfg_hypothesis_t *h, const void *p) { return nr_td_geom_key(h) == ((const Arg *)p)->g; };
  ASSERT_GE(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_g, &arg), 0);
  const int n_before = nr_pdsch_config_sweep_n_active(s.get());
  ASSERT_LT(n_before, s->n_hyp);
  for (int i = 0; i < 4; i++)
    fe(s.get(), &a, 1, true, true);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), n_before);
  EXPECT_FALSE(s->geom_blocked);
  EXPECT_EQ(s->ok_geom[0], 4);
}
TEST(PdschSweepGeomPin, SkippedWhileFailOpen)
{
  auto s = geom_state();
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  const int a = 7;
  for (int i = 0; i < 4; i++)
    fe(s.get(), &a, 1, true, true);
  EXPECT_EQ(s->n_geom, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, ActiveSetChangeRestartsEvidence)
{
  auto s = geom_state();
  const int a = 7;
  fe(s.get(), &a, 1, true, true);
  ASSERT_EQ(s->n_geom, 1);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  EXPECT_EQ(s->n_geom, 0);
  nr_pdsch_config_sweep_set_fail_open(s.get(), false);
  fe(s.get(), &a, 1, true, true);
  ASSERT_EQ(s->n_geom, 1);
  nr_pdsch_config_sweep_prune_keep(s.get(), keep_k0_0, nullptr); /* destructive prune clears the evidence too */
  EXPECT_EQ(s->n_geom, 0);
  EXPECT_FALSE(s->geom_blocked);
}
TEST(PdschSweepGeomPin, BlockedStaysBlockedUntilRestart)
{
  auto s = geom_state();
  const int a = 7, b = other_geom(s.get(), a);
  fe(s.get(), &a, 1, true, true);
  fe(s.get(), &b, 1, true, true);
  ASSERT_TRUE(s->geom_blocked);
  for (int i = 0; i < 5; i++)
    fe(s.get(), &a, 1, true, true);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  EXPECT_FALSE(s->geom_blocked);
}
TEST(PdschSweepGeomPin, RebuildKeepsFlagClearsEvidence)
{
  auto s = geom_state();
  const int a = 7;
  fe(s.get(), &a, 1, true, true);
  nr_pdsch_config_sweep_rebuild(s.get(), 4, 0, nullptr);
  EXPECT_TRUE(s->geom_pin);
  EXPECT_EQ(s->n_geom, 0);
  EXPECT_FALSE(s->geom_blocked);
}
TEST(PdschSweepGeomPin, GeomDormantBitInheritedByK0LayerAndCauseCount)
{
  EXPECT_EQ(NR_TD_DORMANT_GEOM, 4);
  EXPECT_EQ(NR_TD_DORMANT_CAUSES, 6); /* ELIM (CB0 elimination channel) added as cause 5 */
  auto s = geom_state();
  /* pick a geometry at k0 == 0 (the lowest k0), pin it, then add a k0 layer: layer entries copy their source's GEOM bit */
  const int a = 7;
  ASSERT_EQ(s->hyp[a].k0, 0);
  fe(s.get(), &a, 1, true, true);
  fe(s.get(), &a, 1, true, true);
  const int n0 = s->n_hyp;
  ASSERT_LT(nr_pdsch_config_sweep_n_active(s.get()), n0);
  const int added = nr_pdsch_config_sweep_add_k0_layer(s.get(), 3);
  ASSERT_GT(added, 0);
  for (int i = n0; i < s->n_hyp; i++) {
    int src = -1;
    for (int j = 0; j < n0 && src < 0; j++) { /* the source entry: same fields but k0 == lowest */
      const auto &h = s->hyp[j], &n = s->hyp[i];
      if (h.k0 == 0 && h.tda_start == n.tda_start && h.tda_length == n.tda_length && h.dmrs_mask == n.dmrs_mask
          && h.mcs_table == n.mcs_table && h.dmrs_add_pos == n.dmrs_add_pos && h.dmrs_max_len == n.dmrs_max_len)
        src = j;
    }
    ASSERT_GE(src, 0);
    EXPECT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), nr_pdsch_config_sweep_is_active(s.get(), src));
  }
}
/* feed_attr: credits ONLY idx0; the FULL class (dormant included) decides uniqueness (lever C) and attribution (lever P). */
TEST(PdschSweepFeedAttr, OffIsBitIdenticalToFeed)
{
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4);
  memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  for (int t = 0; t < 600000 && nr_pdsch_config_sweep_winner(a.get()) < 0; t++) {
    nr_pdsch_cfg_hypothesis_t h;
    const int i = nr_pdsch_config_sweep_next(a.get(), &h), j = nr_pdsch_config_sweep_next(b.get(), &h);
    ASSERT_EQ(i, j);
    const bool ok = (i == 11) && (((unsigned)t * 2654435761u) >> 16) % 10 < 7;
    const int cls[3] = {i, (i + 7) % a->n_hyp, (i + 13) % a->n_hyp};
    ASSERT_EQ(nr_pdsch_config_sweep_feed(a.get(), i, ok), fa(b.get(), i, cls, 3, ok, t % 3 != 0));
  }
  ASSERT_GE(nr_pdsch_config_sweep_winner(a.get()), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(a.get()), nr_pdsch_config_sweep_winner(b.get()));
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
  EXPECT_EQ(0, memcmp(a->ok, b->ok, sizeof(a->ok)));
  EXPECT_EQ(a->since_pass, b->since_pass);
}
TEST(PdschSweepFeedAttr, CreditsOnlyIdx0)
{
  auto s = crc_state();
  const int cls[] = {7, 9, 11};
  fa(s.get(), 7, cls, 3, true, true);
  EXPECT_EQ(s->trials[7], 1u);
  EXPECT_EQ(s->trials[9], 0u);
  EXPECT_EQ(s->trials[11], 0u);
}
TEST(PdschSweepFeedAttr, FullClassMakesPassNonUniqueSingletonWouldBeUnsafe)
{
  auto s = crc_state();
  const int cls[] = {7, 9};
  fa(s.get(), 7, cls, 2, true, true);
  fa(s.get(), 7, cls, 2, true, true);
  EXPECT_EQ(s->ok_unique[7], 0);
  EXPECT_EQ(s->winner, -1);
  auto u = crc_state(); /* the same passes with the TRUE singleton class are unique and accept */
  const int one = 7;
  fa(u.get(), 7, &one, 1, true, true);
  EXPECT_EQ(fa(u.get(), 7, &one, 1, true, true), 7);
}
TEST(PdschSweepFeedAttr, DormantClassMemberCountsForUniqueness)
{
  auto s = crc_state();
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_not_arg, &s->hyp[9]), 0);
  const int cls[] = {7, 9};
  fa(s.get(), 7, cls, 2, true, true);
  fa(s.get(), 7, cls, 2, true, true);
  EXPECT_EQ(s->ok_unique[7], 0);
  EXPECT_EQ(s->winner, -1);
}
TEST(PdschSweepFeedAttr, Idx0NotInClassStillCountsAsMember)
{
  auto s = crc_state();
  const int cls[] = {9}; /* defensive: idx0 is always a member of its own class */
  fa(s.get(), 7, cls, 1, true, true);
  EXPECT_EQ(s->ok_unique[7], 0);
}
TEST(PdschSweepFeedAttr, GeomPinUsesClassAndCreditsOnlyIdx0)
{
  auto s = geom_state();
  const int cls[] = {7, 9}; /* a class spanning one geometry (twins) */
  const uint64_t g = nr_td_geom_key(&s->hyp[7]);
  fa(s.get(), 7, cls, 2, true, true);
  fa(s.get(), 7, cls, 2, true, true);
  EXPECT_EQ(s->trials[9], 0u);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), nr_td_geom_key(&s->hyp[i]) == g);
}
TEST(PdschSweepFeedAttr, ClassSpanningTwoGeometriesGivesNoPinEvidence)
{
  auto s = geom_state();
  const int b = other_geom(s.get(), 7);
  const int cls[] = {7, b}; /* caller bug / ambiguous attribution: lever P abstains */
  fa(s.get(), 7, cls, 2, true, true);
  EXPECT_EQ(s->n_geom, 0);
}
TEST(PdschSweepFeedAttr, FeedEquivIsAThinWrapperSameResult)
{
  auto a = crc_state(), b = crc_state();
  const int one = 7;
  for (int i = 0; i < 3; i++) {
    const int wa = fe(a.get(), &one, 1, true, true);
    const int wb = fa(b.get(), 7, &one, 1, true, true);
    ASSERT_EQ(wa, wb);
  }
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
  EXPECT_EQ(0, memcmp(a->ok_unique, b->ok_unique, sizeof(a->ok_unique)));
}

/* ---- Fix round 1: exploration-only fast-path evidence (fix A) and k0-sibling guard (fix B) ---- */
using Pick = nr_td_pick_t;
static std::unique_ptr<nr_pdsch_config_sweep_state_t> fp_state(bool c, bool p, float pmin)
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  s->crc_accept = c; s->geom_pin = p; s->sib_pmin = pmin;
  return s;
}
static int n_siblings(const nr_pdsch_config_sweep_state_t *s, int a)
{
  int n = 0;
  for (int i = 0; i < s->n_hyp; i++)
    if (i != a && s->hyp[i].tda_start == s->hyp[a].tda_start && s->hyp[i].tda_length == s->hyp[a].tda_length
        && s->hyp[i].mapping_type == s->hyp[a].mapping_type && s->hyp[i].dmrs_mask == s->hyp[a].dmrs_mask && s->hyp[i].k0 != s->hyp[a].k0
        && nr_pdsch_config_sweep_is_active(s, i))
      n++;
  return n;
}
TEST(PdschSweepFastPathEx, SibN)
{
  EXPECT_EQ(nr_pdsch_config_sweep_sib_n(1, 0.05, 1e-6), 277);
  EXPECT_EQ(nr_pdsch_config_sweep_sib_n(0, 0.05, 1e-6), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_sib_n(6, 0.05, 1e-6), (int)std::ceil(std::log(6e6) / 0.05));
  EXPECT_EQ(nr_pdsch_config_sweep_sib_n(3, 0.0, 1e-6), 0); /* disabled */
}
TEST(PdschSweepFastPathEx, NextExMatchesNextAndReportsKinds)
{
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4);
  memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  int n_exploit = 0;
  for (int t = 0; t < 400; t++) {
    nr_pdsch_cfg_hypothesis_t h1, h2;
    Pick k = NR_TD_PICK_SIBLING;
    const int i = nr_pdsch_config_sweep_next(a.get(), &h1), j = nr_pdsch_config_sweep_next_ex(b.get(), &h2, &k);
    ASSERT_EQ(i, j);
    ASSERT_NE(k, NR_TD_PICK_SIBLING); /* no lever on: never a sibling pick */
    n_exploit += k == NR_TD_PICK_EXPLOIT;
    const bool ok = t == 0; /* the first pick passes once: it becomes hot */
    nr_pdsch_config_sweep_feed(a.get(), i, ok);
    nr_pdsch_config_sweep_feed(b.get(), j, ok);
  }
  EXPECT_GT(n_exploit, 0); /* the first pick became hot */
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
}
TEST(PdschSweepFastPathEx, LegacyWrappersGiveNoFastPathCredit)
{
  /* feed_equiv / feed_attr carry no pick kind: EXPLOIT, so a lever paired with the old next() never accepts or pins (fails safe). */
  auto c = fp_state(true, false, 0), p = fp_state(false, true, 0);
  const int x = 7;
  for (int i = 0; i < 5; i++) {
    nr_pdsch_config_sweep_feed_equiv(c.get(), &x, 1, true, true);
    nr_pdsch_config_sweep_feed_attr(p.get(), x, &x, 1, true, true);
  }
  EXPECT_EQ(c->winner, -1);
  EXPECT_EQ(c->ok_unique[x], 0);
  EXPECT_EQ(c->fp_trials[x], 0);
  EXPECT_EQ(p->n_geom, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(p.get()), p->n_hyp);
  /* the _ex EXPLORE form is the fast-path credit */
  auto e = fp_state(true, false, 0);
  for (int i = 0; i < 2; i++)
    nr_pdsch_config_sweep_feed_equiv_cx(e.get(), &x, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(e->winner, 7);
  EXPECT_EQ(e->fp_trials[7], 2);
}
TEST(PdschSweepGeomPin, PinUsesGroupTrialSum)
{
  /* ok_geom sums the passes of every member of a group, so m_P* must use T_g = sum of fp_trials over the group, not a per-hypothesis T. */
  auto s = geom_state();
  int n_groups = 0, size_max = 0;
  std::vector<uint64_t> keys;
  for (int i = 0; i < s->n_hyp; i++) if (std::find(keys.begin(), keys.end(), nr_td_geom_key(&s->hyp[i])) == keys.end()) keys.push_back(nr_td_geom_key(&s->hyp[i]));
  n_groups = (int)keys.size();
  for (uint64_t k : keys) { int c = 0; for (int i = 0; i < s->n_hyp; i++) c += nr_td_geom_key(&s->hyp[i]) == k; size_max = std::max(size_max, c); }
  ASSERT_GE(size_max, 2);
  int T = -1;
  for (int t = 1; t < 5000 && T < 0; t++)
    if (nr_pdsch_config_sweep_crc_accept_m(n_groups, t + 2) == 2 && nr_pdsch_config_sweep_crc_accept_m(n_groups, (uint32_t)size_max * t) >= 3)
      T = t;
  ASSERT_GT(T, 0) << "no T with m(n, T) = 2 and m(n, size_max*T) = 3 for n_groups " << n_groups;
  for (int i = 0; i < s->n_hyp; i++)
    s->fp_trials[i] = (uint16_t)T;
  int ng = 0; uint32_t tg = 0;
  ASSERT_EQ(nr_pdsch_config_sweep_geom_groups(s.get(), &ng, &tg), 0);
  EXPECT_EQ(ng, n_groups);
  EXPECT_EQ(tg, (uint32_t)size_max * T);
  const int a = 7;
  for (int i = 0; i < 2; i++)
    nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp); /* per-hypothesis T would give m = 2 and pin here */
  EXPECT_EQ(s->ok_geom[0], 2);
  /* with the group sums small enough (m = 2) the same two passes pin */
  auto u = geom_state();
  for (int i = 0; i < 2; i++)
    nr_pdsch_config_sweep_feed_attr_cx(u.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_LT(nr_pdsch_config_sweep_n_active(u.get()), u->n_hyp);
}
TEST(PdschSweepGeomPin, GeomDormantIsClearedByFailOpenAndRebuild)
{
  auto s = geom_state();
  const int a = 7;
  for (int i = 0; i < 2; i++)
    nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  ASSERT_LT(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  nr_pdsch_config_sweep_set_fail_open(s.get(), false); /* the pin must not come back when fail-open ends */
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  for (int i = 0; i < 2; i++)
    nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  ASSERT_LT(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  nr_pdsch_config_sweep_rebuild(s.get(), 4, 0, nullptr);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  EXPECT_TRUE(s->geom_pin);
  /* other causes survive a rebuild (unchanged behaviour) */
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  const int na = nr_pdsch_config_sweep_n_active(s.get());
  nr_pdsch_config_sweep_rebuild(s.get(), 4, 0, nullptr);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), na);
}
TEST(PdschSweepFastPathEx, OnlyExplorePassesCountAndFpTrialsAreExploreOnly)
{
  auto s = fp_state(true, false, 0);
  const int a = 7;
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLOIT, true);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLOIT, true);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, false, true, NR_TD_PICK_SIBLING, true);
  EXPECT_EQ(s->ok_unique[a], 0);
  EXPECT_EQ(s->fp_trials[a], 0);
  EXPECT_EQ(s->trials[a], 3u); /* KL credit unchanged */
  EXPECT_EQ(s->winner, -1);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(s->ok_unique[a], 1);
  EXPECT_EQ(s->fp_trials[a], 1);
}
TEST(PdschSweepFastPathEx, GeomEvidenceIsExploreOnly)
{
  auto s = fp_state(false, true, 0);
  const int a = 7;
  for (int i = 0; i < 5; i++)
    nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLOIT, true);
  EXPECT_EQ(s->n_geom, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepFastPathEx, MStarUsesExploreTrialsOnly)
{
  /* 1000 EXPLOIT failures on a: KL trials 1000, fp_trials 0. m* from fp_trials: two explore passes still accept. */
  auto s = fp_state(true, false, 0);
  const int a = 7;
  for (int i = 0; i < 1000 && s->winner < 0; i++)
    nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, false, true, NR_TD_PICK_EXPLOIT, true);
  if (s->winner >= 0)
    GTEST_SKIP() << "KL decided first";
  ASSERT_GE(s->trials[a], 100u);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true), a);
  EXPECT_TRUE(s->winner_by_crc);
}
TEST(PdschSweepFastPathEx, RestartClearsFastPathStreams)
{
  auto s = fp_state(true, true, 0.05f);
  const int a = 7;
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, false, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, false, true, NR_TD_PICK_SIBLING, true);
  ASSERT_EQ(s->fp_trials[a], 1);
  ASSERT_EQ(s->sib_trials[a], 1);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  EXPECT_EQ(s->fp_trials[a], 0);
  EXPECT_EQ(s->sib_trials[a], 0);
  EXPECT_FALSE(s->sib_blocked);
}
/* Drive the lever-C sibling test: 2 explore passes on a, then follow next_ex (SIBLING picks) feeding failures. */
static int drive_siblings(nr_pdsch_config_sweep_state_t *s, int a, bool sibling_passes, int *n_sib_picks)
{
  const int cls = a;
  nr_pdsch_config_sweep_feed_attr_cx(s, a, &cls, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_config_sweep_feed_attr_cx(s, a, &cls, 1, true, true, NR_TD_PICK_EXPLORE, true);
  *n_sib_picks = 0;
  for (int t = 0; t < 20000 && s->winner < 0; t++) {
    nr_pdsch_cfg_hypothesis_t h;
    Pick k;
    const int i = nr_pdsch_config_sweep_next_ex(s, &h, &k);
    if (s->winner >= 0 || i < 0)
      break;
    if (k != NR_TD_PICK_SIBLING) {
      if (s->sib_blocked)
        break; /* the guard fired: nothing more to schedule deliberately */
      nr_pdsch_config_sweep_feed_attr_cx(s, i, &i, 1, false, true, k, true);
      continue;
    }
    ++*n_sib_picks;
    EXPECT_EQ(h.tda_start, s->hyp[a].tda_start);
    EXPECT_EQ(h.mapping_type, s->hyp[a].mapping_type);
    EXPECT_NE(h.k0, s->hyp[a].k0);
    nr_pdsch_config_sweep_feed_attr_cx(s, i, &i, 1, sibling_passes && *n_sib_picks == 5, true, k, true);
  }
  return s->winner;
}
TEST(PdschSweepSiblingGuard, LeverCWaitsForSiblingTrialsThenAccepts)
{
  auto s = fp_state(true, false, 0.05f);
  const int a = 7;
  const int ns = n_siblings(s.get(), a);
  ASSERT_GT(ns, 0);
  int picks = 0;
  EXPECT_EQ(drive_siblings(s.get(), a, false, &picks), a);
  const int need = nr_pdsch_config_sweep_sib_n(ns, 0.05, 1e-6);
  EXPECT_EQ(picks, need * ns);
  for (int i = 0; i < s->n_hyp; i++)
    if (i != a && s->hyp[i].tda_start == s->hyp[a].tda_start && s->hyp[i].tda_length == s->hyp[a].tda_length
        && s->hyp[i].mapping_type == s->hyp[a].mapping_type && s->hyp[i].dmrs_mask == s->hyp[a].dmrs_mask && s->hyp[i].k0 != s->hyp[a].k0)
      EXPECT_EQ(s->sib_trials[i], need);
}
TEST(PdschSweepSiblingGuard, SiblingPassBlocksLeverC)
{
  auto s = fp_state(true, false, 0.05f);
  int picks = 0;
  EXPECT_EQ(drive_siblings(s.get(), 7, true, &picks), -1);
  EXPECT_TRUE(s->sib_blocked);
  /* further explore passes cannot accept while blocked */
  const int a = 7;
  for (int i = 0; i < 5; i++)
    nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(s->winner, -1);
}
TEST(PdschSweepSiblingGuard, DisabledWhenPminZero)
{
  auto s = fp_state(true, false, 0);
  const int a = 7;
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true), a);
}
TEST(PdschSweepSiblingGuard, NoSiblingsAcceptsImmediately)
{
  auto s = fp_state(true, false, 0.05f);
  const int a = 7;
  /* make every k0 sibling of a dormant (another cause): no active sibling, the guard is vacuous */
  struct Arg { const nr_pdsch_cfg_hypothesis_t *h; } arg{&s->hyp[a]};
  auto keep = [](const nr_pdsch_cfg_hypothesis_t *h, const void *p) { return h->k0 == ((const Arg *)p)->h->k0; };
  /* BC9: only GEOM dormancy (a guarded pin) may hide a sibling; PRIOR/FIELD-dormant siblings are tested (DormantPriorSiblingIsNotIgnored) */
  ASSERT_GE(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_GEOM, keep, &arg), 0);
  ASSERT_EQ(n_siblings(s.get(), a), 0);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true), a);
}
TEST(PdschSweepSiblingGuard, NextIsNeverASiblingPick)
{
  auto s = fp_state(true, false, 0.05f);
  const int a = 7;
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  Pick k;
  nr_pdsch_cfg_hypothesis_t h;
  const int i = nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
  EXPECT_EQ(k, NR_TD_PICK_SIBLING);
  EXPECT_NE(s->hyp[i].k0, s->hyp[a].k0);
  EXPECT_EQ(s->sib_trials[i], 0);
}
TEST(PdschSweepSiblingGuard, LeverPWaitsForSiblingsThenPins)
{
  auto s = fp_state(false, true, 0.05f);
  const int a = 7;
  const uint64_t g = nr_td_geom_key(&s->hyp[a]);
  int picks = 0;
  drive_siblings(s.get(), a, false, &picks);
  EXPECT_GT(picks, 0);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), nr_td_geom_key(&s->hyp[i]) == g);
}
TEST(PdschSweepSiblingGuard, SiblingPassBlocksLeverP)
{
  auto s = fp_state(false, true, 0.05f);
  int picks = 0;
  drive_siblings(s.get(), 7, true, &picks);
  EXPECT_TRUE(s->sib_blocked);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}

TEST(PdschSweepSiblingGuard, SiblingsRequireTheSameMappingType)
{
  auto s = fp_state(true, false, 0.05f);
  const int a = 7;
  const int b = s->n_hyp++; /* same as a but mapping type B and another k0: NOT a sibling (different mapping) */
  s->hyp[b] = s->hyp[a]; s->hyp[b].mapping_type = 1; s->hyp[b].k0 = (uint8_t)(s->hyp[a].k0 ^ 1); s->order[b] = b;
  const int ns = n_siblings(s.get(), a);
  ASSERT_GT(ns, 0);
  int picks = 0;
  drive_siblings(s.get(), a, false, &picks); /* asserts every SIBLING pick has a's mapping type */
  EXPECT_EQ(s->sib_trials[b], 0);
  EXPECT_EQ(picks, nr_pdsch_config_sweep_sib_n(ns, 0.05, 1e-6) * ns);
}

/* ---- BC9: deterministic per-hypothesis exclusion (TDD direction, DCI adjacency) and certified fast-path evidence ---- */
extern "C" {
#include "nr_tdd_pattern.h"
}
/* rfsim cell pattern: 5 ms at 30 kHz = 10 slots, 7 DL, mixed slot 7 (6 DL, 4 UL symbols), 2 UL */
static nr_tdd_config_t bc9_tdd()
{
  nr_tdd_pattern_t p1{};
  p1.period_slots = 10; p1.dl_slots = 7; p1.dl_symbols = 6; p1.ul_symbols = 4; p1.ul_slots = 2;
  nr_tdd_config_t c{};
  EXPECT_TRUE(nr_tdd_config_init(&c, &p1, nullptr));
  return c;
}
static nr_td_excl_t bc9_excl_for_dci_slot(uint32_t dci_slot)
{
  const nr_tdd_config_t c = bc9_tdd();
  nr_td_excl_t e;
  nr_td_excl_none(&e);
  for (int k = 0; k <= NR_TD_K0_MAX; k++)
    e.last[k] = (int8_t)nr_tdd_pdsch_last_symbol(&c, dci_slot + (uint32_t)k);
  return e;
}
static int bc9_count(const nr_pdsch_config_sweep_state_t *s, int k0, int max_last = 13, int min_last = -1)
{
  int n = 0;
  for (int i = 0; i < s->n_hyp; i++) {
    const int last = s->hyp[i].tda_start + s->hyp[i].tda_length - 1;
    n += s->hyp[i].k0 == k0 && last <= max_last && last >= min_last;
  }
  return n;
}
struct PdschBc9 : testing::Test {
  void SetUp() override
  {
    nr_pdsch_config_sweep_reset_all();
    nr_pdsch_config_sweep_prior_reset();
    nr_pdsch_config_sweep_k0_legacy_set(0);
  }
  void TearDown() override { nr_pdsch_config_sweep_k0_legacy_set(-1); }
};

TEST_F(PdschBc9, TddUlSlotExcludesK0)
{
  /* a slot-7 DCI: k0 = 1, 2 land on UL slots 8, 9 (impossible for every entry); k0 = 0 lands on the mixed slot */
  const nr_td_excl_t e = bc9_excl_for_dci_slot(7);
  EXPECT_EQ(e.last[0], 9);
  EXPECT_EQ(e.last[1], -1);
  EXPECT_EQ(e.last[2], -1);
  EXPECT_EQ(e.last[3], 13);
  static nr_pdsch_config_sweep_state_t st;
  nr_pdsch_config_sweep_init(&st, 4);
  const int k0_short = bc9_count(&st, 0, 9);
  ASSERT_GT(nr_pdsch_config_sweep_exclude(&st, &e), 0);
  EXPECT_EQ(bc9_count(&st, 1), 0);
  EXPECT_EQ(bc9_count(&st, 0), k0_short); /* only k0 = 0 entries ending by symbol 9 */
  EXPECT_EQ(st.n_hyp, k0_short);
  /* live, keyed (row 1 plays the mixed-slot row: k39_fresh opens a 2-row key space): binds the (configuration, rnti, tda) context, persists across eviction, never leaks to another row */
  auto t = k39_fresh(1);
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 1, &e), 0);
  t = k39_fresh(1);
  EXPECT_EQ(k39_count(t, 1), 0);
  EXPECT_GT(k39_count(t, 0), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 1), UINT64_C(0x1)); /* certified k0 = 0 */
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 0), UINT64_C(0x3)); /* another row: universe {0,1} */
  const auto t0 = k39_fresh(0);
  EXPECT_GT(k39_count(t0, 1), 0);
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS + 40; i++)
    k39_fresh(0, 0x7000 + i, (uint16_t)(0x5000 + i % 40)); /* evict */
  t = k39_fresh(1);
  EXPECT_EQ(k39_count(t, 1), 0);
  EXPECT_GT(k39_count(t, 0), 0);
  /* a k0 >= 2 layer whose slot is UL is refused, one on a DL slot is appended and bound to the per-hypothesis rule */
  EXPECT_EQ(nr_pdsch_config_sweep_add_k0(&t, 2), 0);
  t = k39_fresh(1);
  EXPECT_GT(nr_pdsch_config_sweep_add_k0(&t, 3), 0);
  t = k39_fresh(1);
  EXPECT_GT(k39_count(t, 3), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 1), UINT64_C(0x9)); /* {0, 3}: no longer certified */
}

TEST_F(PdschBc9, MixedSlotExcludesOnlyLongEntries)
{
  /* a slot-6 DCI: k0 = 0 lands on DL slot 6 (no restriction), k0 = 1 on the mixed slot 7 (UL symbols 10..13) */
  const nr_td_excl_t e = bc9_excl_for_dci_slot(6);
  EXPECT_EQ(e.last[0], 13);
  EXPECT_EQ(e.last[1], 9);
  static nr_pdsch_config_sweep_state_t st;
  nr_pdsch_config_sweep_init(&st, 4);
  const int k0_all = bc9_count(&st, 0), k1_short = bc9_count(&st, 1, 9), k1_all = bc9_count(&st, 1);
  ASSERT_GT(k1_short, 0);
  ASSERT_LT(k1_short, k1_all);
  ASSERT_GT(nr_pdsch_config_sweep_exclude(&st, &e), 0);
  EXPECT_EQ(bc9_count(&st, 0), k0_all);    /* k0 = 0 untouched */
  EXPECT_EQ(bc9_count(&st, 1), k1_short);  /* k0 = 1 entries ending on the flexible / DL symbols survive */
  EXPECT_EQ(bc9_count(&st, 1, 13, 10), 0); /* those reaching the UL symbols are gone */
  /* an unconstrained exclusion keeps everything and the evidence */
  nr_td_excl_t none;
  nr_td_excl_none(&none);
  st.trials[0] = 5;
  EXPECT_EQ(nr_pdsch_config_sweep_exclude(&st, &none), st.n_hyp);
  EXPECT_EQ(st.trials[0], 5u);
  /* keyed: a partial exclusion leaves the row uncertified */
  k39_fresh(0);
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 0, &e), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 0), UINT64_C(0x3));
}

TEST_F(PdschBc9, AdjacencyExclusionPrunesK0AndRefusesAContradiction)
{
  k39_fresh(0);
  nr_td_excl_t f;
  nr_td_excl_none(&f);
  f.last[1] = -1; /* DCI adjacency: k0 = 1 impossible for this row */
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 0, &f), 0);
  auto t = k39_fresh(0);
  EXPECT_EQ(k39_count(t, 1), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 0), UINT64_C(0x1));
  /* a later constraint that would leave no k0 of the universe is evidence of a broken assumption: refused, nothing changes */
  nr_td_excl_t g;
  nr_td_excl_none(&g);
  g.last[0] = -1;
  EXPECT_EQ(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 0, &g), -1);
  t = k39_fresh(0);
  EXPECT_GT(k39_count(t, 0), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 0), UINT64_C(0x1));
}

TEST_F(PdschBc9, ExclusionClearedOnReopenOfTheContext)
{
  /* the per-key exclusion is persisted in the same LRU set as the k0 certification and dies with it on reopen */
  nr_td_excl_t f;
  nr_td_excl_none(&f);
  f.last[1] = -1;
  k39_fresh(0);
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 0, &f), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 0), UINT64_C(0x1));
  /* drive the context to convergence, then fail it until it reopens */
  nr_pdsch_cfg_hypothesis_t h{};
  nr_pdsch_sweep_ticket_t t{};
  int target = -1;
  for (int i = 0; i < 400000 && !nr_pdsch_config_sweep_is_settled(0x1234, 0x4601, 0, 0); i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 0, 0, test_legal, &t, &h));
    if (target < 0) target = t.hypothesis;
    nr_pdsch_config_sweep_feedback(&t, h.tda_start == 1 && h.tda_length == 13 && h.mcs_table == 0 && h.dmrs_add_pos == 1 && h.dmrs_max_len == 1, nullptr);
  }
  ASSERT_TRUE(nr_pdsch_config_sweep_is_settled(0x1234, 0x4601, 0, 0));
  for (int i = 0; i < 5000 && nr_pdsch_config_sweep_is_settled(0x1234, 0x4601, 0, 0); i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 0, 0, test_legal, &t, &h));
    nr_pdsch_config_sweep_feedback(&t, false, nullptr);
  }
  ASSERT_FALSE(nr_pdsch_config_sweep_is_settled(0x1234, 0x4601, 0, 0));
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 0), UINT64_C(0x3));
  /* the live context forgot it too: an observation (which re-applies the binding) keeps k0 = 1 */
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 0, 0, test_legal, &t, &h));
  nr_pdsch_config_sweep_observe(&t, (uint16_t)test_legal(0, 13, 1, 0, 1, 1), 13, 0);
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 0, 0, test_legal, &t, &h));
  EXPECT_GT(count_snapshot_k0(t, 1), 0);
  EXPECT_GT(count_snapshot_k0(t, 0), 0);
}

/* certified fast-path evidence: levers C/P count only explore passes on k0-unambiguous grants */
TEST(PdschBc9FastPath, UncertifiedPassDoesNotCountForPC)
{
  auto c = fp_state(true, false, 0);
  const int a = 7;
  for (int i = 0; i < 4; i++)
    EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(c.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, false), -1);
  EXPECT_EQ(c->ok_unique[a], 0);
  EXPECT_EQ(c->fp_trials[a], 0);
  EXPECT_EQ(c->ok[a], 4u); /* still KL evidence */
  nr_pdsch_config_sweep_feed_attr_cx(c.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(c.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true), a);
  auto p = fp_state(false, true, 0);
  for (int i = 0; i < 4; i++)
    nr_pdsch_config_sweep_feed_attr_cx(p.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, false);
  EXPECT_EQ(p->n_geom, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(p.get()), p->n_hyp); /* no pin */
  int idx[1] = {a};
  for (int i = 0; i < 4; i++)
    nr_pdsch_config_sweep_feed_equiv_cx(p.get(), idx, 1, true, true, NR_TD_PICK_EXPLORE, false);
  EXPECT_EQ(p->n_geom, 0);
}

/* BC2b carry-forward: a k0 sibling dormant through PRIOR / FIELD may be the truth (a wrong prior): it is tested, or blocks */
TEST(PdschBc9FastPath, DormantPriorSiblingIsNotIgnored)
{
  auto s = fp_state(true, false, 0.05f);
  const int a = 7;
  struct Arg { uint8_t k0; } arg{s->hyp[a].k0};
  auto keep = [](const nr_pdsch_cfg_hypothesis_t *h, const void *p) { return h->k0 == ((const Arg *)p)->k0; };
  ASSERT_GE(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep, &arg), 0);
  ASSERT_EQ(n_siblings(s.get(), a), 0); /* no ACTIVE sibling */
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true), -1); /* waits */
  nr_pdsch_cfg_hypothesis_t h;
  Pick k;
  const int sib = nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
  ASSERT_EQ(k, NR_TD_PICK_SIBLING);
  EXPECT_NE(h.k0, s->hyp[a].k0);
  EXPECT_FALSE(nr_pdsch_config_sweep_is_active(s.get(), sib)); /* the dormant sibling is tested */
  /* its test: a pass blocks the fast path, failures count without becoming KL evidence */
  const uint32_t kl_before = s->trials[sib];
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), sib, &sib, 1, false, true, NR_TD_PICK_SIBLING, true);
  EXPECT_EQ(s->sib_trials[sib], 1);
  EXPECT_EQ(s->trials[sib], kl_before);
  int picks = 0;
  for (int t = 0; t < 20000 && s->winner < 0; t++) {
    const int i = nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
    if (s->winner >= 0) break;
    if (k == NR_TD_PICK_SIBLING) picks++;
    nr_pdsch_config_sweep_feed_attr_cx(s.get(), i, &i, 1, false, true, k, true);
  }
  EXPECT_EQ(s->winner, a);
  EXPECT_GT(picks, 0);
  /* the pass case */
  auto b = fp_state(true, false, 0.05f);
  ASSERT_GE(nr_pdsch_config_sweep_set_dormant(b.get(), NR_TD_DORMANT_PRIOR, keep, &arg), 0);
  nr_pdsch_config_sweep_feed_attr_cx(b.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_config_sweep_feed_attr_cx(b.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  const int sb = nr_pdsch_config_sweep_next_ex(b.get(), &h, &k);
  ASSERT_EQ(k, NR_TD_PICK_SIBLING);
  nr_pdsch_config_sweep_feed_attr_cx(b.get(), sb, &sb, 1, true, true, NR_TD_PICK_SIBLING, true);
  EXPECT_TRUE(b->sib_blocked);
}

TEST(PdschBc9FastPath, SiblingInTddUlSlotIsExcludedNotScheduled)
{
  auto s = fp_state(true, false, 0.05f);
  nr_td_excl_t e;
  nr_td_excl_none(&e);
  e.last[1] = -1; /* the sibling offset lands on a UL slot */
  ASSERT_GT(nr_pdsch_config_sweep_exclude(s.get(), &e), 0);
  const int a = 7;
  ASSERT_EQ(s->hyp[a].k0, 0);
  int n_k1 = 0;
  for (int i = 0; i < s->n_hyp; i++)
    n_k1 += s->hyp[i].k0 == 1;
  EXPECT_EQ(n_k1, 0); /* excluded from the catalogue, not dormant */
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_cfg_hypothesis_t h;
  Pick k;
  nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
  EXPECT_NE(k, NR_TD_PICK_SIBLING);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true), a); /* no sibling tests needed */
}

TEST(PdschBc9FastPath, UndecodableSiblingSkipIsBounded)
{
  auto s = fp_state(true, false, 0.05f);
  const int a = 7;
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_cfg_hypothesis_t h;
  Pick k;
  int i = nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
  ASSERT_EQ(k, NR_TD_PICK_SIBLING);
  const uint16_t before = s->sib_trials[i];
  nr_pdsch_config_sweep_sib_skip(s.get(), i); /* its slot was not captured */
  EXPECT_EQ(s->sib_trials[i], before);        /* a skip is no sibling trial */
  nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
  EXPECT_NE(k, NR_TD_PICK_SIBLING); /* the RNTI does not stall on it: one normal pick */
  nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
  EXPECT_EQ(k, NR_TD_PICK_SIBLING); /* then the test resumes */
  /* review M6: a sibling that stays undecodable ends the fast path (fail-safe) after a bounded number of skips */
  for (int n = 0; n < 4 * NR_TD_SIB_SKIP_MAX && !s->sib_blocked; n++) { /* every other pick is a sibling */
    i = nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
    if (k == NR_TD_PICK_SIBLING)
      nr_pdsch_config_sweep_sib_skip(s.get(), i);
  }
  EXPECT_TRUE(s->sib_blocked);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true), -1); /* KL only */
}

/* Review M8: a sibling test is a NEW-DATA trial; a retransmission (a possible HARQ trap) does not count toward N_sib. */
TEST(PdschBc9FastPath, SiblingRetransmissionDoesNotCount)
{
  auto s = fp_state(true, false, 0.05f);
  const int a = 7;
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE, true);
  nr_pdsch_cfg_hypothesis_t h;
  Pick k;
  const int i = nr_pdsch_config_sweep_next_ex(s.get(), &h, &k);
  ASSERT_EQ(k, NR_TD_PICK_SIBLING);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), i, &i, 1, false, false, NR_TD_PICK_SIBLING, true);
  EXPECT_EQ(s->sib_trials[i], 0);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), i, &i, 1, false, true, NR_TD_PICK_SIBLING, true);
  EXPECT_EQ(s->sib_trials[i], 1);
  nr_pdsch_config_sweep_feed_attr_cx(s.get(), i, &i, 1, true, false, NR_TD_PICK_SIBLING, true);
  EXPECT_TRUE(s->sib_blocked); /* any sibling pass still blocks (conservative) */
}

/* Review M5 / M3 runtime support: a lock-free epoch that moves whenever a persisted certification/exclusion can disappear
 * (reopen, RNTI eviction, LRU eviction, reset), and the ticket carries its configuration key. */
TEST_F(PdschBc9, ConstraintEpochMovesOnReopenAndTicketCarriesConfiguration)
{
  auto t = k39_fresh(0);
  EXPECT_EQ(t.configuration, 0x1234u);
  const uint64_t e0 = nr_pdsch_config_sweep_cert_epoch();
  nr_td_excl_t f;
  nr_td_excl_none(&f);
  f.last[1] = -1;
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 0, &f), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_cert_epoch(), e0); /* adding a constraint never invalidates a cache */
  nr_pdsch_config_sweep_reset_all();
  EXPECT_NE(nr_pdsch_config_sweep_cert_epoch(), e0);
}


/* Runtime input of the certified flag: the ticket's hypothesis, its alive k0-sibling offsets and the alive MCS tables. */
TEST_F(PdschBc9, TicketSiblingsReportsAliveK0OffsetsAndTables)
{
  auto t = k39_fresh(0);
  nr_pdsch_cfg_hypothesis_t h{};
  uint64_t sib = 0;
  uint8_t tables = 0;
  ASSERT_TRUE(nr_pdsch_config_sweep_ticket_siblings(&t, &h, &sib, &tables));
  EXPECT_EQ(sib, UINT64_C(1) << (h.k0 ^ 1)); /* the full catalogue has both k0 for every geometry */
  EXPECT_EQ(tables, 0x7);
  nr_td_excl_t f;
  nr_td_excl_none(&f);
  f.last[1] = -1;
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 0, &f), 0);
  EXPECT_FALSE(nr_pdsch_config_sweep_ticket_siblings(&t, &h, &sib, &tables)); /* stale ticket after the prune */
  t = k39_fresh(0);
  ASSERT_TRUE(nr_pdsch_config_sweep_ticket_siblings(&t, &h, &sib, &tables));
  EXPECT_EQ(h.k0, 0);
  EXPECT_EQ(sib, 0u); /* no sibling left: such a grant can never be certified */
}

/* Runtime fast path of the accept hook: one lock to learn the RNTI has no deterministic constraint at all. */
TEST_F(PdschBc9, RntiConstrainedOnlyAfterAnExclusionOrCertification)
{
  EXPECT_FALSE(nr_pdsch_config_sweep_rnti_constrained(0x4601, 0x1234)); /* unknown RNTI */
  auto t = k39_fresh(0);
  EXPECT_FALSE(nr_pdsch_config_sweep_rnti_constrained(0x4601, 0x1234));
  nr_td_excl_t f;
  nr_td_excl_none(&f);
  f.last[1] = -1;
  ASSERT_GT(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 0, &f), 0);
  EXPECT_TRUE(nr_pdsch_config_sweep_rnti_constrained(0x4601, 0x1234));
  EXPECT_FALSE(nr_pdsch_config_sweep_rnti_constrained(0x4601, 0x9999)); /* another configuration */
  auto u = k39_fresh(0, 0x5555);
  ASSERT_GT(nr_pdsch_config_sweep_certify_k0(&u, 0x1), 0);
  EXPECT_TRUE(nr_pdsch_config_sweep_rnti_constrained(0x4601, 0x5555));
}

/* Review I1: a caller that does not state certification gets none (the _ex forms are fail-safe once levers C/P are on). */
TEST(PdschBc9FastPath, ExFormsAreNeverCertified)
{
  auto c = fp_state(true, false, 0);
  const int a = 7;
  for (int i = 0; i < 4; i++)
    EXPECT_EQ(nr_pdsch_config_sweep_feed_attr_ex(c.get(), a, &a, 1, true, true, NR_TD_PICK_EXPLORE), -1);
  int idx[1] = {a};
  for (int i = 0; i < 4; i++)
    EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv_ex(c.get(), idx, 1, true, true, NR_TD_PICK_EXPLORE), -1);
  EXPECT_EQ(c->ok_unique[a], 0);
  EXPECT_EQ(c->fp_trials[a], 0);
}

/* Review I3: a converged context drops the k0-oracle layers it did not win on (statistical). The universe that defines
 * "row k0 certified" -- and so a destructive adjacency prune -- must not shrink with it. */
TEST_F(PdschBc9, K0UniverseDoesNotShrinkWithTheStatisticalLayerDrop)
{
  nr_pdsch_sweep_ticket_t t{};
  nr_pdsch_cfg_hypothesis_t h{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 0, 0, test_legal, &t, &h));
  ASSERT_GT(nr_pdsch_config_sweep_add_k0(&t, 3), 0); /* the k0 oracle saw k0 = 3 */
  for (int i = 0; i < 600000 && !nr_pdsch_config_sweep_is_settled(0x1234, 0x4601, 0, 0); i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 0, 0, test_legal, &t, &h));
    nr_pdsch_config_sweep_feedback(&t, h.k0 == 0 && h.tda_start == 1 && h.tda_length == 13 && h.mcs_table == 0
                                           && h.dmrs_add_pos == 1 && h.dmrs_max_len == 1, nullptr);
  }
  ASSERT_TRUE(nr_pdsch_config_sweep_is_settled(0x1234, 0x4601, 0, 0)); /* converged on k0 = 0: layer 3 dropped from k0_seen */
  /* another row of the RNTI: k0 = 1 excluded deterministically; k0 = 3 is still possible (the universe kept it) */
  k39_fresh(1);
  nr_td_excl_t f;
  nr_td_excl_none(&f);
  f.last[1] = -1;
  ASSERT_GE(nr_pdsch_config_sweep_exclude_key(0x1234, 0x4601, 1, &f), 0);
  EXPECT_EQ(nr_pdsch_config_sweep_row_k0_allowed(0x1234, 0x4601, 1), UINT64_C(0x9)); /* {0, 3}: NOT certified */
}

/* Follow-up 1: the BC9 census (nr_pdsch_passive_bc9_note) runs AFTER the KL feedback and only READS the sweep
 * (ticket_siblings, rnti_constrained, row_k0_allowed, cert_epoch). With no constraint, the KL selection/feedback sequence
 * must be identical with those reads (BC9 on) and without them (ISAC_TD_DCI_ADJ=0). Two RNTIs x two rows, an observation,
 * convergence and prior publication: every decision the KL path makes is recorded and compared. */
static std::vector<int64_t> bc9_kl_trace(bool census_reads)
{
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  std::vector<int64_t> tr;
  unsigned seed = 4242;
  const uint16_t rntis[2] = {0x4601, 0x4602};
  for (int g = 0; g < 40000; g++) {
    const uint16_t rnti = rntis[g & 1];
    const uint8_t tda = (uint8_t)((g >> 1) % 2);
    nr_pdsch_sweep_ticket_t t{};
    nr_pdsch_cfg_hypothesis_t h{};
    if (!nr_pdsch_config_sweep_select(0x1234, rnti, tda, 0, 0, test_legal, &t, &h)) {
      tr.push_back(-1);
      continue;
    }
    if (g < 8) /* one DM-RS observation per (RNTI, row): k0 {0,1} x 3 tables survive, so the contexts converge */
      nr_pdsch_config_sweep_observe(&t, (uint16_t)test_legal(0, 13, 1, 0, 1, 1), 13, 0);
    const bool truth = h.k0 == 0 && h.tda_start == 1 && h.tda_length == 13 && h.mcs_table == 0 && h.dmrs_add_pos == 1
                       && h.dmrs_max_len == 1;
    const bool pass = truth && (double)rand_r(&seed) / RAND_MAX < 0.8;
    nr_pdsch_cfg_hypothesis_t w{};
    const bool conv = nr_pdsch_config_sweep_feedback(&t, pass, &w);
    if (census_reads) { /* exactly what the census does after the feedback */
      nr_pdsch_cfg_hypothesis_t hh{};
      uint64_t sib = 0;
      uint8_t tables = 0;
      nr_pdsch_config_sweep_ticket_siblings(&t, &hh, &sib, &tables);
      (void)nr_pdsch_config_sweep_rnti_constrained(rnti, t.configuration);
      (void)nr_pdsch_config_sweep_row_k0_allowed(0x1234, rnti, tda);
      (void)nr_pdsch_config_sweep_row_k0_allowed(0x1234, rnti, (uint8_t)(tda ^ 1));
      (void)nr_pdsch_config_sweep_cert_epoch();
    }
    tr.push_back((int64_t)t.hypothesis << 8 | (int64_t)t.settled << 4 | (int64_t)conv << 2 | (int64_t)pass);
  }
  return tr;
}
TEST_F(PdschBc9, CensusReadsLeaveTheKlSequenceUnchanged)
{
  const auto off = bc9_kl_trace(false);
  const auto on = bc9_kl_trace(true);
  ASSERT_EQ(off.size(), on.size());
  size_t first_diff = off.size();
  for (size_t i = 0; i < off.size() && first_diff == off.size(); i++)
    if (off[i] != on[i])
      first_diff = i;
  EXPECT_EQ(first_diff, off.size()) << "KL sequence diverges at grant " << first_diff;
  int settled = 0;
  for (auto v : off)
    settled += (v >> 4) & 1;
  EXPECT_GT(settled, 0); /* the run reached convergence (and the prior/sibling-row path) */
}

// ---- CB0 elimination channel (levers spec 5.4 redesign; ISAC_TD_CB0_ELIM) ----------------------------------------------------
/* One grant of the CB0 channel model: a shared latent draw u (same IQ for every decode of the grant). The truth's TB passes iff u < p,
 * its CB0 iff u < q (q >= p: pointwise CB0 >= TB, the section 9.3 same-decoder condition). The twin (index `twin`, -1 = none) is
 * computation-identical on grants that do not exercise its field (prob 1 - e): then it passes exactly as the truth, else it fails.
 * `cb0_always` (-1 = none) passes every CB0 and fails every TB (a hypothesis CB0 cannot refute). Everything else fails both. */
struct Cb0Model { int truth, twin = -1, cb0_always = -1; double p, q, e = 0; };
static int cb0_grant(nr_pdsch_config_sweep_state_t *s, const Cb0Model &m, std::mt19937_64 &rng, bool feed_cb0 = true)
{
  std::uniform_real_distribution<double> ud(0, 1);
  const double u = ud(rng);
  const bool ex = ud(rng) < m.e;
  auto tb = [&](int h) { return h == m.truth ? u < m.p : (h == m.twin && !ex ? u < m.p : false); };
  auto cb = [&](int h) { return h == m.truth ? u < m.q : (h == m.twin && !ex ? u < m.q : h == m.cb0_always); };
  nr_pdsch_cfg_hypothesis_t h;
  const int i = nr_pdsch_config_sweep_next(s, &h);
  int w = nr_pdsch_config_sweep_feed(s, i, tb(i));
  if (feed_cb0 && w < 0) {
    std::vector<int> idx;
    std::vector<char> pass;
    for (int k = 0; k < s->n_hyp; k++)
      if (nr_pdsch_config_sweep_is_active(s, k)) { idx.push_back(k); pass.push_back(cb(k)); }
    bool pb[NR_PDSCH_SWEEP_MAX_HYP];
    for (size_t k = 0; k < pass.size(); k++) pb[k] = pass[k];
    w = nr_pdsch_config_sweep_feed_cb0(s, idx.data(), (int)idx.size(), pb, true);
  }
  return w;
}
static std::unique_ptr<nr_pdsch_config_sweep_state_t> cb0_state(int keep_n)
{
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(s.get(), 4);
  if (keep_n > 0 && keep_n < s->n_hyp) { /* smaller catalogue: keep the first keep_n entries (fast Monte Carlo) */
    struct A { int n; } a{keep_n};
    static int counter;
    counter = 0;
    nr_pdsch_config_sweep_prune_keep(s.get(), [](const nr_pdsch_cfg_hypothesis_t *, const void *arg) { return counter++ < ((const A *)arg)->n; }, &a);
  }
  s->cb0_elim = true;
  return s;
}
TEST(PdschSweepCb0, OffIsBitIdentical)
{
  nr_pdsch_config_sweep_cb0_elim_env_set(-1);
  unsetenv("ISAC_TD_CB0_ELIM");
  EXPECT_FALSE(nr_pdsch_config_sweep_cb0_elim_env()); /* default 0 */
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4);
  memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  ASSERT_FALSE(a->cb0_elim);
  Cb0Model m{11, -1, -1, 0.7, 0.9, 0.0};
  std::mt19937_64 ra(5), rb(5);
  for (int t = 0; t < 600000 && nr_pdsch_config_sweep_winner(a.get()) < 0; t++)
    ASSERT_EQ(cb0_grant(a.get(), m, ra, false), cb0_grant(b.get(), m, rb, true)) << t; /* b calls feed_cb0 with the flag off */
  ASSERT_GE(nr_pdsch_config_sweep_winner(a.get()), 0);
  EXPECT_EQ(0, memcmp((const void *)a.get(), (const void *)b.get(), sizeof(*a))); /* the whole state, incl. cb0 counters and masks */
}
TEST(PdschSweepCb0, PassAndFailBothCount)
{
  auto s = cb0_state(64);
  const int idx[5] = {3, 4, 5, 4, 999}; /* duplicate 4 and out-of-range 999 credit nothing extra */
  const bool pass[5] = {true, false, true, true, true};
  nr_pdsch_config_sweep_feed_cb0(s.get(), idx, 5, pass, true);
  EXPECT_EQ(s->cb0_trials[3], 1u); EXPECT_EQ(s->cb0_pass[3], 1u);
  EXPECT_EQ(s->cb0_trials[4], 1u); EXPECT_EQ(s->cb0_pass[4], 0u); /* a FAIL counts as a trial */
  EXPECT_EQ(s->cb0_trials[5], 1u); EXPECT_EQ(s->cb0_pass[5], 1u); /* a PASS counts too (symmetric channel) */
  /* never mixed into the full-TB evidence */
  for (int i = 0; i < s->n_hyp; i++) { ASSERT_EQ(s->trials[i], 0u); ASSERT_EQ(s->ok[i], 0u); }
  /* inadmissible (different decoder / rank > 1 under K38 / stale samples): nothing */
  nr_pdsch_config_sweep_feed_cb0(s.get(), idx, 3, pass, false);
  EXPECT_EQ(s->cb0_trials[3], 1u);
  /* dormant: nothing */
  auto s2 = cb0_state(64);
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(s2.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr), 0);
  int d = -1;
  for (int i = 0; i < s2->n_hyp && d < 0; i++) if (!nr_pdsch_config_sweep_is_active(s2.get(), i)) d = i;
  ASSERT_GE(d, 0);
  const bool f = false;
  nr_pdsch_config_sweep_feed_cb0(s2.get(), &d, 1, &f, true);
  EXPECT_EQ(s2->cb0_trials[d], 0u);
  /* flag off: nothing */
  auto s3 = cb0_state(64);
  s3->cb0_elim = false;
  nr_pdsch_config_sweep_feed_cb0(s3.get(), idx, 3, pass, true);
  EXPECT_EQ(s3->cb0_trials[3], 0u);
}
TEST(PdschSweepCb0, EliminationNeverEliminatesTruth)
{
  /* Worst case q == p (CB0 no better than TB) and a twin that is identical on half the grants; plus q > p. 300 runs. */
  int decided = 0, elim_any = 0;
  for (int run = 0; run < 300; run++) {
    auto s = cb0_state(48);
    const Cb0Model m{run % 48, (run + 7) % 48, -1, 0.15 + 0.5 * (run % 3) / 2.0, run % 2 ? 0.15 + 0.5 * (run % 3) / 2.0 : 0.95, 0.5};
    std::mt19937_64 rng(1000 + run);
    for (int t = 0; t < 40000 && nr_pdsch_config_sweep_winner(s.get()) < 0; t++) {
      cb0_grant(s.get(), m, rng);
      ASSERT_FALSE(nr_pdsch_config_sweep_is_eliminated(s.get(), m.truth)) << "run " << run << " grant " << t;
    }
    const int w = nr_pdsch_config_sweep_winner(s.get());
    if (w >= 0) { decided++; EXPECT_EQ(w, m.truth) << run; }
    for (int i = 0; i < s->n_hyp; i++) elim_any += nr_pdsch_config_sweep_is_eliminated(s.get(), i);
  }
  EXPECT_GT(decided, 250);
  EXPECT_GT(elim_any, 0); /* the channel actually eliminated something */
}
TEST(PdschSweepCb0, EliminatedAreDormantAndFailOpenRestores)
{
  auto s = cb0_state(64);
  const Cb0Model m{10, -1, -1, 0.5, 0.6, 0};
  std::mt19937_64 rng(3);
  int t = 0;
  for (; t < 20000 && nr_pdsch_config_sweep_n_active(s.get()) == s->n_hyp; t++) cb0_grant(s.get(), m, rng);
  ASSERT_LT(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp) << "nothing eliminated";
  int e = -1;
  for (int i = 0; i < s->n_hyp && e < 0; i++) if (nr_pdsch_config_sweep_is_eliminated(s.get(), i)) e = i;
  ASSERT_GE(e, 0);
  EXPECT_FALSE(nr_pdsch_config_sweep_is_active(s.get(), e));
  EXPECT_TRUE((s->dormant[NR_TD_DORMANT_ELIM][e / 64] >> (e % 64)) & 1u);
  /* not deleted: still in the catalogue; not scheduled */
  const int n_before = s->n_hyp;
  const uint32_t tr = s->trials[e], c0 = s->cb0_trials[e];
  for (int k = 0; k < 2000 && nr_pdsch_config_sweep_winner(s.get()) < 0; k++) {
    nr_pdsch_cfg_hypothesis_t h;
    ASSERT_NE(nr_pdsch_config_sweep_next(s.get(), &h), e);
    cb0_grant(s.get(), m, rng);
  }
  EXPECT_EQ(s->n_hyp, n_before);
  EXPECT_EQ(s->trials[e], tr);
  EXPECT_EQ(s->cb0_trials[e], c0); /* an eliminated hypothesis accumulates nothing */
  /* fail-open restores every eliminated hypothesis and discards the CB0 evidence */
  auto f = cb0_state(64);
  std::mt19937_64 r2(3);
  for (int k = 0; k < 20000 && nr_pdsch_config_sweep_n_active(f.get()) == f->n_hyp; k++) cb0_grant(f.get(), m, r2);
  ASSERT_LT(nr_pdsch_config_sweep_n_active(f.get()), f->n_hyp);
  nr_pdsch_config_sweep_set_fail_open(f.get(), true);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(f.get()), f->n_hyp);
  for (int i = 0; i < f->n_hyp; i++) {
    ASSERT_FALSE(nr_pdsch_config_sweep_is_eliminated(f.get(), i));
    ASSERT_EQ(f->cb0_trials[i], 0u);
  }
  /* while fail-open, the channel is off (TB-only safe mode) */
  const int i0 = 1; const bool no = false;
  nr_pdsch_config_sweep_feed_cb0(f.get(), &i0, 1, &no, true);
  EXPECT_EQ(f->cb0_trials[1], 0u);
  nr_pdsch_config_sweep_set_fail_open(f.get(), false);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(f.get()), f->n_hyp); /* the cleared mask stays cleared */
}
TEST(PdschSweepCb0, LeaderStillNeedsFullTbSeparation)
{
  /* (1) a perfect twin (identical TB and CB0 on every grant): CB0 can never separate it, so even after every other hypothesis is
   *     eliminated the KL rule must stay undecided. */
  auto s = cb0_state(64);
  const Cb0Model m{20, 21, -1, 0.5, 0.7, 0.0};
  std::mt19937_64 rng(9);
  for (int t = 0; t < 30000; t++) cb0_grant(s.get(), m, rng);
  EXPECT_EQ(nr_pdsch_config_sweep_winner(s.get()), -1);
  EXPECT_TRUE(nr_pdsch_config_sweep_is_active(s.get(), 21));
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), 2); /* everything else eliminated */
  /* (2) a hypothesis that passes every CB0 but never a TB is never elected (CB0 never elects) and never eliminated */
  auto c = cb0_state(64);
  const Cb0Model mc{30, -1, 31, 0.4, 0.6, 0.0};
  std::mt19937_64 r2(10);
  for (int t = 0; t < 100000 && nr_pdsch_config_sweep_winner(c.get()) < 0; t++) {
    cb0_grant(c.get(), mc, r2);
    ASSERT_FALSE(nr_pdsch_config_sweep_is_eliminated(c.get(), 31));
  }
  EXPECT_EQ(nr_pdsch_config_sweep_winner(c.get()), 30);
  EXPECT_GE(c->trials[30], 64u); /* the winner carries its own full-TB evidence */
}
TEST(PdschSweepCb0, Cb0EvidenceFollowsTheCatalogue)
{
  EXPECT_EQ(NR_TD_DORMANT_ELIM, 5);
  auto s = cb0_state(0);
  const Cb0Model m{10, -1, -1, 0.5, 0.6, 0};
  std::mt19937_64 rng(4);
  for (int t = 0; t < 400000 && nr_pdsch_config_sweep_n_active(s.get()) == s->n_hyp; t++) cb0_grant(s.get(), m, rng);
  ASSERT_LT(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  /* a k0 layer does not inherit ELIM (another k0 is another hypothesis) */
  auto k = std::make_unique<nr_pdsch_config_sweep_state_t>();
  memcpy((void *)k.get(), (void *)s.get(), sizeof(*s));
  const int n0 = k->n_hyp;
  ASSERT_GT(nr_pdsch_config_sweep_add_k0_layer(k.get(), 5), 0);
  for (int i = n0; i < k->n_hyp; i++) { ASSERT_FALSE(nr_pdsch_config_sweep_is_eliminated(k.get(), i)); ASSERT_EQ(k->cb0_trials[i], 0u); }
  /* a destructive prune clears the CB0 evidence and the ELIM mask with trials/ok */
  auto p = std::make_unique<nr_pdsch_config_sweep_state_t>();
  memcpy((void *)p.get(), (void *)s.get(), sizeof(*s));
  ASSERT_GT(nr_pdsch_config_sweep_prune_keep(p.get(), keep_even, nullptr), 0);
  for (int i = 0; i < p->n_hyp; i++) { ASSERT_EQ(p->cb0_trials[i], 0u); ASSERT_FALSE(nr_pdsch_config_sweep_is_eliminated(p.get(), i)); }
  /* rebuild keeps the flag (configuration) and drops the evidence-derived mask */
  nr_pdsch_config_sweep_rebuild(s.get(), 4, 0, nullptr);
  EXPECT_TRUE(s->cb0_elim);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
  for (int i = 0; i < s->n_hyp; i++) ASSERT_EQ(s->cb0_trials[i], 0u);
}
TEST(PdschSweepCb0, EliminationCutsTheSearch)
{
  /* Same draws, flag off vs on: the channel must reach the same (true) winner with far fewer grants. */
  long g_off = 0, g_on = 0;
  for (int run = 0; run < 3; run++) {
    for (int on = 0; on < 2; on++) {
      auto s = cb0_state(0);
      s->cb0_elim = on;
      const Cb0Model m{(run * 37) % s->n_hyp, -1, -1, 0.5, 0.6, 0};
      std::mt19937_64 rng(77 + run);
      long t = 0;
      for (; t < 3000000 && nr_pdsch_config_sweep_winner(s.get()) < 0; t++) cb0_grant(s.get(), m, rng);
      ASSERT_EQ(nr_pdsch_config_sweep_winner(s.get()), m.truth) << run << on;
      (on ? g_on : g_off) += t;
    }
  }
  EXPECT_LT(g_on * 5, g_off) << "on " << g_on << " off " << g_off;
}
TEST(PdschSweepCb0, EliminationUsesTheCb0UpperBound)
{
  /* Leader L (full-TB 600/1000: LB ~0.5) vs an uncertain h (CB0 3/10 after this call: empirical rate below the LB, UB above it): h stays; a hypothesis whose CB0
   * rate is clearly below (10/2000) goes. The rule is one-sided on the CB0 UPPER bound (a lower-bound rule would kill h). */
  auto s = cb0_state(64);
  const int L = 3, h = 4, low = 5;
  s->trials[L] = 1000; s->ok[L] = 600;
  s->cb0_trials[h] = 9; s->cb0_pass[h] = 3; /* rate 0.33 below LB_tb(L) ~0.47 but UB_cb0 far above */
  s->cb0_trials[low] = 1999; s->cb0_pass[low] = 10;
  const int idx[2] = {h, low};
  const bool pass[2] = {false, false};
  nr_pdsch_config_sweep_feed_cb0(s.get(), idx, 2, pass, true);
  EXPECT_FALSE(nr_pdsch_config_sweep_is_eliminated(s.get(), h));
  EXPECT_TRUE(nr_pdsch_config_sweep_is_eliminated(s.get(), low));
  EXPECT_FALSE(nr_pdsch_config_sweep_is_eliminated(s.get(), L)); /* the leader is never eliminated */
  EXPECT_EQ(nr_pdsch_config_sweep_winner(s.get()), -1); /* L still needs full-TB separation from h and the 0-trial others */
}
TEST(PdschSweepCb0, DecoderPinnedPerContext)
{
  /* CRC evidence is not exchangeable between the CUDA and CPU LDPC decoders (~1 dB apart, MEASURED DGX bit-exact harness): the first
   * admissible CB0 batch pins the context's CB0 decoder; a batch from another decoder (e.g. a circuit-breaker fallback) credits nothing.
   * The pin is CB0 evidence and is cleared with it (prune, fail-open). */
  auto s = cb0_state(64);
  const int idx[2] = {3, 4};
  const bool pass[2] = {true, false};
  nr_pdsch_config_sweep_feed_cb0_dec(s.get(), idx, 2, pass, true, NR_TD_DEC_CUDA);
  EXPECT_EQ(s->cb0_trials[3], 1u);
  nr_pdsch_config_sweep_feed_cb0_dec(s.get(), idx, 2, pass, true, NR_TD_DEC_CPU); /* switch mid-context: inadmissible */
  EXPECT_EQ(s->cb0_trials[3], 1u);
  nr_pdsch_config_sweep_feed_cb0_dec(s.get(), idx, 2, pass, true, NR_TD_DEC_CUDA);
  EXPECT_EQ(s->cb0_trials[3], 2u);
  nr_pdsch_config_sweep_feed_cb0(s.get(), idx, 2, pass, true); /* the plain form is the CPU decoder */
  EXPECT_EQ(s->cb0_trials[3], 2u);
  /* an inadmissible batch does not pin */
  auto u = cb0_state(64);
  nr_pdsch_config_sweep_feed_cb0_dec(u.get(), idx, 2, pass, false, NR_TD_DEC_CUDA);
  nr_pdsch_config_sweep_feed_cb0(u.get(), idx, 2, pass, true);
  EXPECT_EQ(u->cb0_trials[3], 1u);
  /* fail-open clears the pin with the evidence */
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  nr_pdsch_config_sweep_set_fail_open(s.get(), false);
  nr_pdsch_config_sweep_feed_cb0(s.get(), idx, 2, pass, true);
  EXPECT_EQ(s->cb0_trials[3], 1u);
  /* a destructive prune too */
  ASSERT_GT(nr_pdsch_config_sweep_prune_keep(s.get(), keep_even, nullptr), 0);
  nr_pdsch_config_sweep_feed_cb0_dec(s.get(), idx, 2, pass, true, NR_TD_DEC_CUDA);
  EXPECT_EQ(s->cb0_trials[3], 1u);
}
