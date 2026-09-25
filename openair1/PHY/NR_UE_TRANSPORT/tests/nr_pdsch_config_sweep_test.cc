#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_config_sweep.h"
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
  const int n = nr_pdsch_config_sweep_observe(&t, mask, 13, 0);
  EXPECT_EQ(n, 3);
  nr_pdsch_sweep_ticket_t t2{};
  for (int i = 0; i < 12; i++) {
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0x1234, 0x4601, 0, 2, 0, test_legal, &t2, &h));
    EXPECT_EQ(h.dmrs_mask, mask);
    EXPECT_EQ(h.tda_start + h.tda_length - 1, 13);
    EXPECT_EQ(h.k0, 0);
  }
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

TEST(PdschConfigSweepTypeB, CatalogIncludesTypeBAndFits) {
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  int nb = 0;
  for (int i = 0; i < n; i++)
    nb += st.hyp[i].mapping_type == 1;
  std::cerr << "[ MEASURED ] pure catalog n_hyp=" << n << " (type B " << nb << ") max=" << NR_PDSCH_SWEEP_MAX_HYP
            << " bytes/context=" << sizeof(nr_pdsch_config_sweep_state_t) << std::endl;
  EXPECT_GT(nb, 0);
  EXPECT_LT(n, NR_PDSCH_SWEEP_MAX_HYP);
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
TEST(PdschConfigSweepTypeB, TypeBReachesTheMaskGeneratorAndMergesIdenticalPdus) {
  nr_pdsch_config_sweep_state_t st;
  ASSERT_EQ(nr_pdsch_config_sweep_init_legal(&st, 2, 0, ab_legal), 12); /* 2 (S,L) x k0{0,1} x 3 tables */
  int b = 0;
  for (int i = 0; i < st.n_hyp; i++)
    if (st.hyp[i].mapping_type == 1) {
      b++;
      EXPECT_EQ(st.hyp[i].tda_start, 5);
      EXPECT_EQ(st.hyp[i].dmrs_mask, 0x20);
    }
  EXPECT_EQ(b, 6);
}

/* dmrs-DownlinkForPDSCH-MappingTypeA and -MappingTypeB are separate RRC IEs: a prior learned on a
 * type-A entry says nothing about type-B add_pos/max_len (mcs-Table is shared). */
TEST(PdschConfigSweepTypeB, PriorFromTypeAKeepsTypeBEntriesOfTheSameTable) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  unsigned seed = 77;
  bool converged = false;
  for (int i = 0; i < 200000 && !converged; i++) {
    nr_pdsch_sweep_ticket_t t{};
    nr_pdsch_cfg_hypothesis_t h{};
    ASSERT_TRUE(nr_pdsch_config_sweep_select(0xAB, 0x4601, 0, 2, 0, ab_legal, &t, &h));
    const bool truth = h.mapping_type == 0 && h.k0 == 0 && h.mcs_table == 1;
    const double u = (double)rand_r(&seed) / (double)RAND_MAX;
    converged = nr_pdsch_config_sweep_feedback(&t, truth && u < 0.54, nullptr);
  }
  ASSERT_TRUE(converged);
  nr_pdsch_sweep_ticket_t t1{};
  nr_pdsch_cfg_hypothesis_t h1{};
  ASSERT_TRUE(nr_pdsch_config_sweep_select(0xAB, 0x4601, 1, 2, 0, ab_legal, &t1, &h1));
  nr_pdsch_config_sweep_state_t st{};
  ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&t1, &st));
  int b = 0;
  for (int i = 0; i < st.n_hyp; i++) {
    EXPECT_EQ(st.hyp[i].mcs_table, 1);
    b += st.hyp[i].mapping_type == 1;
  }
  EXPECT_EQ(b, 2);         /* type B, table 1, k0 {0,1} -- its add_pos 1 is not the type-A prior's 0 */
  EXPECT_EQ(st.n_hyp, 4);
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
