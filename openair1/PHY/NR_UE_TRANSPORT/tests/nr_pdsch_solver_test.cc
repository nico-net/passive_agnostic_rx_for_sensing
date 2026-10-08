// T26b grant solver (nr_pdsch_solver.c): spec helpers and the search -> candidate -> solved state machine against a
// simulated cell whose CRC oracle passes only the true (reading, S per TDA index, add_pos, table).
#include <cstdlib>
#include <random>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "nr_pdsch_solver.h"
}
extern "C" configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  if (s) fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  if (assert) abort(); else exit(EXIT_SUCCESS);
}
static const int kEnable = setenv("ISAC_PDSCH_SOLVER", "1", 1);

TEST(PdschSolver, DmrsMaskFollowsTable7411_2_3)
{
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(2, 1, 13, 1), 0x804);       // {2, 11}: the PCI 415 grants
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(2, 0, 14, 1), 0x804);
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(2, 0, 14, 2), 0x884);       // {2, 7, 11}
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(2, 0, 14, 3), 0x924);       // {2, 5, 8, 11}
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(2, 0, 14, 0), 0x4);
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(3, 0, 12, 1), (1 << 3) | (1 << 9));
}

TEST(PdschSolver, McsTables)
{
  int qm, r2;
  ASSERT_TRUE(nr_pdsch_solver_mcs_qr(1, 20, &qm, &r2));
  EXPECT_EQ(qm, 8);
  EXPECT_EQ(r2, 1365); // 682.5 x 2
  ASSERT_TRUE(nr_pdsch_solver_mcs_qr(0, 0, &qm, &r2));
  EXPECT_EQ(r2, 240);
  EXPECT_FALSE(nr_pdsch_solver_mcs_qr(1, 28, &qm, &r2));
}

namespace {
int tda_fn(void *, uint16_t, nr_dci_bits_t p) { return (int)(p.w[0] & 1); }
struct Cell {
  std::vector<uint16_t> wrong, truth;
  int S_true[2] = {1, 0};
  int add_pos = 1, table = 1;
};
// returns grants needed to solve (or -1)
int RunCell(const Cell &c, uint16_t rnti, int false_pass_at, int max_grants, std::mt19937 &rng)
{
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), 16}, {c.truth.data(), (int)c.truth.size(), 16}};
  for (int g = 0; g < max_grants; g++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    const int tda = (int)(p.w[0] & 1);
    const uint8_t mcs = (g % 3 == 0) ? 2 : 0; // MCS 2 discriminates tables 0/1, MCS 0 does not
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, mcs, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++) {
      int qa, ra, qb, rb;
      nr_pdsch_solver_mcs_qr(v[i].table, mcs, &qa, &ra);
      nr_pdsch_solver_mcs_qr(c.table, mcs, &qb, &rb);
      bool ok = v[i].reading == 1 && v[i].S == c.S_true[tda] && v[i].add_pos == c.add_pos && qa == qb && ra == rb;
      if (g == false_pass_at && i == 0 && v[i].reading == 0) ok = true; // one chance CRC pass on a wrong reading
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
    const int rep = nr_pdsch_solver_rep(rnti);
    if (rep >= 0)
      return rep >= 10 && rep < 15 ? g + 1 : -2;
  }
  return -1;
}
} // namespace

TEST(PdschSolver, SolvesTheSimulatedCellWithTwoIndependentPasses)
{
  ASSERT_EQ(kEnable, 0);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  std::mt19937 rng(5);
  const int g = RunCell(c, 0x8f5a, -1, 400, rng);
  printf("[solver] solved after %d grants\n", g);
  ASSERT_GT(g, 0);
  EXPECT_LE(g, 120);
  // solved: a known TDA index decodes in full with its own S, an unknown one is learnt by probes
  nr_pdsch_solver_reading_t rd[1] = {{c.truth.data(), (int)c.truth.size(), 16}};
  int fulls = 0;
  for (int k = 0; k < 40; k++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x8f5a, p, 49, 20, 2, rd, 1, 8, v);
    for (int i = 0; i < n; i++) {
      const bool ok = v[i].S == c.S_true[p.w[0] & 1] && v[i].add_pos == 1 && v[i].table == 1;
      fulls += !v[i].probe && ok;
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
  }
  EXPECT_GE(fulls, 30) << "once solved (S and table learnt) every grant is one full decode";
}

TEST(PdschSolver, AChanceCrcPassDoesNotSolveTheWrongLayouts)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  std::mt19937 rng(9);
  const int g = RunCell(c, 0x1234, 0, 600, rng);
  printf("[solver] with a chance pass first: solved after %d grants\n", g);
  EXPECT_GT(g, 0) << "-2 = solved on the wrong layouts, -1 = never";
}

TEST(PdschSolver, TheCellPriorSolvesASecondRntiFaster)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  std::mt19937 rng(11);
  const int g1 = RunCell(c, 0x0001, -1, 400, rng);
  const int g2 = RunCell(c, 0x0002, -1, 400, rng);
  printf("[solver] first RNTI %d grants, second RNTI %d grants\n", g1, g2);
  ASSERT_GT(g1, 0);
  ASSERT_GT(g2, 0);
  EXPECT_LE(g2, 4);
}

TEST(PdschSolver, CsiRsRateMatchingIsAPerRntiCrcHypothesis)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  std::mt19937 rng(13);
  ASSERT_GT(RunCell(c, 0x0042, -1, 400, rng), 0);
  nr_pdsch_solver_reading_t rd[1] = {{c.truth.data(), (int)c.truth.size(), 16}};
  // a slot with discovered CSI-RS: the solver probes with and without rate matching; truth = rate-matched
  int learnt_after = -1;
  for (int k = 0; k < 20 && learnt_after < 0; k++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan_slot(0x0042, p, 49, 0, 2, true, rd, 1, 8, v);
    bool both = false, any_rm = false, any_no = false;
    for (int i = 0; i < n; i++) {
      any_rm |= v[i].rm == 1; any_no |= v[i].rm == 0;
      const bool ok = v[i].rm == 1 && v[i].S == c.S_true[p.w[0] & 1] && v[i].add_pos == 1;
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
    both = any_rm && any_no;
    if (!both && n > 0) learnt_after = k;
  }
  EXPECT_GE(learnt_after, 1) << "after the first CRC pass only the rate-matched variant remains";
  // a slot without CSI-RS never rate-matches
  nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  const int n = nr_pdsch_solver_plan_slot(0x0042, p, 49, 0, 2, false, rd, 1, 8, v);
  for (int i = 0; i < n; i++) EXPECT_EQ(v[i].rm, 0);
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
