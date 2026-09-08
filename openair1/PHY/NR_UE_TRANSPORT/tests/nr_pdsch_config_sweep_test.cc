#include <cstdlib>
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
TEST(PdschConfigSweep, SeparatesUesTdaAndConfiguration) {
  nr_pdsch_config_sweep_reset_all();
  const auto a=select_context(1,0x4601,0), b=select_context(1,0x4602,0),
             c=select_context(1,0x4601,1), d=select_context(2,0x4601,0);
  nr_pdsch_config_sweep_feedback(&a,true,nullptr);
  for(auto ticket : {a,b,c,d}) {
    nr_pdsch_config_sweep_state_t state{};
    ASSERT_TRUE(nr_pdsch_config_sweep_snapshot(&ticket,&state));
    EXPECT_EQ(state.trials[ticket.hypothesis], ticket.generation==a.generation ? 1u : 0u);
  }
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
TEST(PdschConfigSweep, IndependentContextsCanConvergeToDifferentConfigurations) {
  nr_pdsch_config_sweep_reset_all();
  for(int i=0;i<400*NR_PDSCH_SWEEP_MAX_HYP;i++) {
    for(int ctx=0;ctx<3;ctx++) {
      auto ticket=select_context(1,ctx==1 ? 0x4602 : 0x4601,ctx==2 ? 1 : 0);
      nr_pdsch_config_sweep_feedback(&ticket,ticket.hypothesis==ctx+2,nullptr);
    }
  }
  for(int ctx=0;ctx<3;ctx++) {
    auto ticket=select_context(1,ctx==1 ? 0x4602 : 0x4601,ctx==2 ? 1 : 0);
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
