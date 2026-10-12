// T26b grant solver (nr_pdsch_solver.c): spec helpers and the search -> candidate -> solved state machine against a
// simulated cell whose CRC oracle passes only the true (reading, S per TDA index, add_pos, table).
#include <cstdlib>
#include <atomic>
#include <random>
#include <thread>
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

TEST(PdschSolver, ConcurrentScanConsumersKeepTheirOwnGrantContext)
{
  setenv("ISAC_PDSCH_SOLVER_THREAD_CTX", "1", 1);
  setenv("ISAC_PDSCH_SOLVER_SCR", "1", 1);
  nr_pdsch_solver_reset();
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  struct Result { int n; bool own_scr; } result[2] = {};
  auto work = [&](int k) {
    const uint16_t id = (uint16_t)(249 + k * 166);
    const uint16_t layout = (uint16_t)(10 + k);
    nr_pdsch_solver_reading_t rd = {&layout, 1, 16, 1, 1, 2};
    nr_dci_bits_t p = {{(uint64_t)(42 + k), 0, 0}};
    nr_pdsch_solver_set_now((uint64_t)(100 + k));
    nr_pdsch_solver_set_alt_scr(1000 + k, &id, 1, true);
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan((uint16_t)(0x1100 + k), p, 49, 2, 2, &rd, 1, 8, v);
    bool own = n > 0;
    for (int i = 0; i < n; i++) {
      own &= v[i].scr == id;
      nr_pdsch_solver_feedback(v[i].token, false, true);
    }
    result[k] = {n, own};
  };
  std::thread a(work, 0), b(work, 1);
  while (ready.load() != 2) std::this_thread::yield();
  go.store(true);
  a.join(); b.join();
  EXPECT_GT(result[0].n, 0);
  EXPECT_GT(result[1].n, 0);
  EXPECT_TRUE(result[0].own_scr);
  EXPECT_TRUE(result[1].own_scr);
}

TEST(PdschSolver, OneLayerQpskReadingsGetFirstProbesAndBoundedFallback)
{
  setenv("ISAC_PDSCH_SOLVER_NL_PREF", "3", 1);
  nr_pdsch_solver_reset();
  uint16_t two_ids[] = {1, 2}, one_ids[] = {3, 4};
  nr_pdsch_solver_reading_t rd[] = {
      {two_ids, 2, 16, 0x3, 2, 2}, {one_ids, 2, 16, 0x1, 1, 9}};
  nr_dci_bits_t p = {{42, 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  int n = nr_pdsch_solver_plan(0x1111, p, 49, 2, 2, rd, 2, 8, v);
  ASSERT_GT(n, 0);
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(v[i].reading, 1);
    int qm, r2;
    ASSERT_TRUE(nr_pdsch_solver_mcs_qr(v[i].table, rd[1].mcs, &qm, &r2));
    EXPECT_EQ(qm, 2);
    nr_pdsch_solver_feedback(v[i].token, false, true);
  }
  rd[1].n_layers = 2;
  EXPECT_EQ(nr_pdsch_solver_plan(0x2222, p, 49, 2, 2, rd, 2, 8, v), 0);
  EXPECT_EQ(nr_pdsch_solver_plan(0x2222, p, 49, 2, 2, rd, 2, 8, v), 0);
  n = nr_pdsch_solver_plan(0x2222, p, 49, 2, 2, rd, 2, 8, v);
  EXPECT_GT(n, 0);
  for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
  unsetenv("ISAC_PDSCH_SOLVER_NL_PREF");
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, ConfirmationRejectsIncompatibleMcsTables)
{
  setenv("ISAC_PDSCH_SOLVER_NL_PREF", "3", 1);
  nr_pdsch_solver_reset();
  const uint16_t layout = 42;
  nr_pdsch_solver_reading_t rd = {&layout, 1, 16, 1, 1, 2};
  bool first = false, second = false;
  for (int g = 0; g < 12 && !second; g++) {
    nr_dci_bits_t p = {{(uint64_t)(100 + g), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x3344, p, 49, 2, 2, &rd, 1, 8, v);
    for (int i = 0; i < n; i++) {
      const bool ok = v[i].S == 1 && v[i].add_pos == 1
                      && ((!first && v[i].table == 1) || (first && !second && g > 0 && v[i].table == 0));
      nr_pdsch_solver_feedback(v[i].token, ok, true);
      if (ok) { if (!first) first = true; else second = true; }
    }
  }
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(nr_pdsch_solver_rep(0x3344), -1);
  unsetenv("ISAC_PDSCH_SOLVER_NL_PREF");
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, CoverageSurvivesReorderedReadingsAndIdentityChanges)
{
  setenv("ISAC_PDSCH_SOLVER_COVERAGE", "1", 1);
  setenv("ISAC_PDSCH_SOLVER_NL_PREF", "32", 1);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
  uint16_t ids[9];
  bool seen[2][9] = {};
  for (int i = 0; i < 9; i++) ids[i] = 100 + i;
  for (int g = 0; g < 18; g++) {
    // Identical physical hypotheses move between reading indices with each DCI.
    nr_pdsch_solver_reading_t rd[9];
    for (int i = 0; i < 9; i++) rd[i] = {&ids[(i + g) % 9], 1, 16, 1, 1, 2};
    const uint16_t scr = 200 + (g % 2);
    nr_pdsch_solver_set_alt_scr(77, &scr, 1, true);
    nr_dci_bits_t p = {{(uint64_t)(1000 + g), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, rd, 9, 1, v);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(v[0].scr, scr);
    const int id = *rd[v[0].reading].ids - 100;
    EXPECT_FALSE(seen[g % 2][id]) << "a shifted reading must not consume another layout's search progress";
    seen[g % 2][id] = true;
    nr_pdsch_solver_feedback(v[0].token, false, true);
  }
  for (auto &lane : seen) for (bool tested : lane) EXPECT_TRUE(tested);
  unsetenv("ISAC_PDSCH_SOLVER_COVERAGE");
  unsetenv("ISAC_PDSCH_SOLVER_NL_PREF");
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, CoverageKeepsStartSymbolSearchSeparateForEachTdra)
{
  setenv("ISAC_PDSCH_SOLVER_COVERAGE", "1", 1);
  setenv("ISAC_PDSCH_SOLVER_NL_PREF", "1", 1);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  const uint16_t id = 100;
  nr_pdsch_solver_reading_t rd = {&id, 1, 16, 1, 1, 2};
  for (int g = 0; g < 4; g++) {
    nr_dci_bits_t p = {{(uint64_t)(g % 2), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 1, v), 1);
    EXPECT_EQ(v[0].S, g < 2 ? 1 : 0) << "both TDRA indices need their own start-symbol trials";
    nr_pdsch_solver_feedback(v[0].token, false, true);
  }
  EXPECT_EQ(nr_pdsch_solver_rep(0x7788), -1);
  unsetenv("ISAC_PDSCH_SOLVER_COVERAGE");
  unsetenv("ISAC_PDSCH_SOLVER_NL_PREF");
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, IneligibleTablesDoNotConsumeLaterQpskTrials)
{
  setenv("ISAC_PDSCH_SOLVER_COVERAGE", "1", 1);
  setenv("ISAC_PDSCH_SOLVER_NL_PREF", "1", 1);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  const uint16_t id = 100;
  nr_pdsch_solver_reading_t rd = {&id, 1, 16, 1, 1, 14};
  nr_dci_bits_t p = {{42, 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 14, 2, &rd, 1, 1, v), 1);
  EXPECT_EQ(v[0].table, 2); // only the low-rate table is QPSK at this MCS
  nr_pdsch_solver_feedback(v[0].token, false, true);
  rd.mcs = 1;
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 1, 2, &rd, 1, 1, v), 1);
  EXPECT_EQ(v[0].table, 1);
  EXPECT_EQ(v[0].S, 1); // this physical hypothesis has not been tried yet
  nr_pdsch_solver_feedback(v[0].token, false, true);
  unsetenv("ISAC_PDSCH_SOLVER_COVERAGE");
  unsetenv("ISAC_PDSCH_SOLVER_NL_PREF");
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, LayoutRecoveryNeedsCreditedCrcFailures)
{
  setenv("ISAC_PDSCH_SOLVER_LAYOUT_RECOVER", "3", 1);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  const uint16_t id = 100;
  nr_pdsch_solver_reading_t rd = {&id, 1, 16, 1, 1, 2};
  nr_dci_bits_t p = {{42, 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  EXPECT_FALSE(nr_pdsch_solver_reopen_layouts(0x7788));
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 3, v), 3);
  nr_pdsch_solver_feedback(v[0].token, false, false); // expired IQ is not refutation
  nr_pdsch_solver_feedback(v[1].token, false, true);
  nr_pdsch_solver_feedback(v[2].token, true, true);
  EXPECT_FALSE(nr_pdsch_solver_reopen_layouts(0x7788));
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 3, v), 3);
  for (int i = 0; i < 3; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
  EXPECT_TRUE(nr_pdsch_solver_reopen_layouts(0x7788));
  EXPECT_EQ(nr_pdsch_solver_rep(0x7788), -1); // reopening is not a lock
  unsetenv("ISAC_PDSCH_SOLVER_LAYOUT_RECOVER");
  nr_pdsch_solver_reset();
  EXPECT_FALSE(nr_pdsch_solver_reopen_layouts(0x7788));
}

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

// TIMBIMODAL ISAC_PDSCH_SOLVER_PRIOR_CRC=1: a solution seeds other RNTIs (cell prior, REUSE) only once it decoded a full
// TB. RNTI 0x00a1 is "solved" on add_pos 2 by two CRC passes (as by two chance CRC-16 passes OTA, ab_Tr2_1_094729), but no
// full TB ever decodes. With one probe per grant, RNTI 0x00b2's probe goes to that layout while it is the prior.
static int FirstAddPos(uint16_t rnti, const Cell &c, std::mt19937 &rng)
{
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), 16}, {c.truth.data(), (int)c.truth.size(), 16}};
  nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  const int n = nr_pdsch_solver_plan(rnti, p, 49, 0, 2, rd, 2, 1, v);
  for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
  return n > 0 ? v[0].add_pos : -1;
}
static void FullDecodes(uint16_t rnti, const Cell &c, int k, bool ok, std::mt19937 &rng)
{
  nr_pdsch_solver_reading_t rd[1] = {{c.truth.data(), (int)c.truth.size(), 16}};
  for (int g = 0; g < k; g++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, 0, 2, rd, 1, 8, v);
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, ok && !v[i].probe, true);
  }
}
TEST(PdschSolver, OnlyACrcProvenSolutionSeedsTheCell)
{
  setenv("ISAC_PDSCH_SOLVER_REUSE", "1", 1);
  Cell fake;
  fake.add_pos = 2;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? fake.truth : fake.wrong).push_back((uint16_t)i);
  for (int on = 0; on < 2; on++) {
    on ? setenv("ISAC_PDSCH_SOLVER_PRIOR_CRC", "1", 1) : unsetenv("ISAC_PDSCH_SOLVER_PRIOR_CRC");
    nr_pdsch_solver_reset();
    nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
    std::mt19937 rng(21);
    ASSERT_GT(RunCell(fake, 0x00a1, -1, 400, rng), 0);
    EXPECT_EQ(FirstAddPos(0x00b2, fake, rng), on ? 1 : 2) << "solved, no full TB yet (REUSE)";
    FullDecodes(0x00a1, fake, 700, false, rng); // never decodes: reopened after 256 fulls
    EXPECT_LT(nr_pdsch_solver_rep(0x00a1), 0);
    EXPECT_EQ(FirstAddPos(0x00b2, fake, rng), on ? 1 : 2) << "reopened: flag off keeps the stale cell prior forever";
  }
  // control: a solution that decodes a full TB seeds the cell with the flag on
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(22);
  ASSERT_GT(RunCell(fake, 0x00a1, -1, 400, rng), 0);
  FullDecodes(0x00a1, fake, 20, true, rng);
  EXPECT_EQ(FirstAddPos(0x00b2, fake, rng), 2);
  unsetenv("ISAC_PDSCH_SOLVER_PRIOR_CRC");
  unsetenv("ISAC_PDSCH_SOLVER_REUSE");
  nr_pdsch_solver_reset();
}

/* TIMSOLVE2 ISAC_PDSCH_SOLVER_SIGNIF=1. A busy RNTI that already spent 20000 credited search probes (CRC16 code blocks: chance
 * passes Poisson with mean 20000 x 2^-16 = 0.31) is not solved by two consistent CRC16 passes (P = 0.037); a quiet RNTI is; one
 * CRC24 pass (TBS > 3824) solves at once. Flag off: two passes always solve. */
static int SolveGrants(uint16_t rnti, const Cell &c, int mcs, int nrb, int max_grants, std::mt19937 &rng)
{
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), (uint16_t)nrb, 1, 1, (uint8_t)mcs},
                                     {c.truth.data(), (int)c.truth.size(), (uint16_t)nrb, 1, 1, (uint8_t)mcs}};
  for (int g = 0; g < max_grants; g++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, (uint8_t)mcs, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++)
      nr_pdsch_solver_feedback(v[i].token, v[i].reading == 1 && v[i].S == c.S_true[p.w[0] & 1] && v[i].add_pos == c.add_pos
                                               && v[i].table == c.table, true);
    if (nr_pdsch_solver_rep(rnti) >= 0) return g + 1;
  }
  return -1;
}
static void BurnProbes(uint16_t rnti, const Cell &c, int n_probes, std::mt19937 &rng)
{
  nr_pdsch_solver_reading_t rd[1] = {{c.wrong.data(), (int)c.wrong.size(), 16}};
  for (int k = 0; k < n_probes;) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, 0, 2, rd, 1, 8, v);
    for (int i = 0; i < n; i++, k++) nr_pdsch_solver_feedback(v[i].token, false, true);
    if (n == 0) break;
  }
}
TEST(PdschSolver, SignificanceRuleScalesTheSolveEvidenceWithTheProbesSpent)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  int grants[2][3];
  for (int on = 0; on < 2; on++) {
    on ? setenv("ISAC_PDSCH_SOLVER_SIGNIF", "1", 1) : unsetenv("ISAC_PDSCH_SOLVER_SIGNIF");
    nr_pdsch_solver_reset();
    nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
    std::mt19937 rng(31);
    grants[on][0] = SolveGrants(0x0101, c, 0, 16, 400, rng);  /* quiet, CRC16 (MCS 0 on 16 PRBs) */
    BurnProbes(0x0202, c, 20000, rng);
    grants[on][1] = SolveGrants(0x0202, c, 0, 16, 400, rng);  /* busy, CRC16 */
    grants[on][2] = SolveGrants(0x0303, c, 20, 16, 400, rng); /* CRC24: 64QAM/256QAM-class MCS 20 on 16 PRBs */
    printf("[solver] signif=%d grants to solve: quiet %d, busy %d, crc24 %d\n", on, grants[on][0], grants[on][1], grants[on][2]);
    for (int k = 0; k < 3; k++) ASSERT_GT(grants[on][k], 0);
  }
  EXPECT_EQ(grants[1][0], grants[0][0]) << "two CRC16 passes still solve a quiet RNTI";
  EXPECT_GT(grants[1][1], grants[0][1]) << "a busy RNTI needs more passes";
  EXPECT_LT(grants[1][2], grants[0][2]) << "one CRC24 pass solves";
  unsetenv("ISAC_PDSCH_SOLVER_SIGNIF");
  nr_pdsch_solver_reset();
}

/* TIMSOLVE2 ISAC_PDSCH_SOLVER_PRIOR_IDS=1: once a solution is CRC-proven, an unsolved RNTI tries the proven layout dims with
 * EACH of its own identities in the proven form (here DM-RS id + data id 923) on one grant; flag off: one identity per grant. */
TEST(PdschSolver, ProvenDimsAreTriedWithEveryOwnIdentity)
{
  setenv("ISAC_PDSCH_SOLVER_REUSE", "1", 1);
  setenv("ISAC_PDSCH_SOLVER_PRIOR_CRC", "1", 1);
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  const int32_t kForm = (923 + 1) << 16;
  for (int on = 0; on < 2; on++) {
    on ? setenv("ISAC_PDSCH_SOLVER_PRIOR_IDS", "1", 1) : unsetenv("ISAC_PDSCH_SOLVER_PRIOR_IDS");
    nr_pdsch_solver_reset();
    nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
    std::mt19937 rng(41);
    const int32_t a_ids[1] = {101 | kForm};
    nr_pdsch_solver_set_alt_scr32(7, a_ids, 1, true, true);
    ASSERT_GT(RunCell(c, 0x0a0a, -1, 400, rng), 0);
    FullDecodes(0x0a0a, c, 4, true, rng); /* CRC-proven */
    const int32_t b_ids[4] = {202 | kForm, 303 | kForm, 202, 303};
    nr_pdsch_solver_set_alt_scr32(7, b_ids, 4, true, true);
    nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), 16}, {c.truth.data(), (int)c.truth.size(), 16}};
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x0b0b, p, 49, 0, 2, rd, 2, 8, v);
    bool id202 = false, id303 = false;
    for (int i = 0; i < n; i++) {
      if (v[i].reading == 1 && v[i].add_pos == c.add_pos && v[i].S == c.S_true[p.w[0] & 1]) {
        id202 |= v[i].scr == (202 | kForm);
        id303 |= v[i].scr == (303 | kForm);
      }
      nr_pdsch_solver_feedback(v[i].token, false, true);
    }
    EXPECT_EQ(id202 && id303, on == 1) << "on: both own identities on the proven dims in one grant; off: one per grant";
  }
  unsetenv("ISAC_PDSCH_SOLVER_PRIOR_IDS");
  unsetenv("ISAC_PDSCH_SOLVER_PRIOR_CRC");
  unsetenv("ISAC_PDSCH_SOLVER_REUSE");
  nr_pdsch_solver_reset();
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/* SOLVERCOVER ISAC_PDSCH_SOLVER_FAIRSHARE=1: an RNTI with a grant in every slot and one with a grant every 10 slots share the
 * probe rate equally; without the share the busy one plans ~24 probes per grant. */
static int PlanProbes(uint16_t rnti, int budget)
{
  static const uint16_t a[1] = {3}, b[1] = {9};
  nr_pdsch_solver_reading_t rd[2] = {{a, 1, 16, 1, 1, 0}, {b, 1, 16, 1, 1, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  nr_dci_bits_t p = {{0, 0, 0}};
  const int n = nr_pdsch_solver_plan(rnti, p, 49, 0, 2, rd, 2, budget, v);
  for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
  return n;
}
TEST(PdschSolver, FairShareSplitsTheProbeRateBetweenActiveRntis)
{
  for (int on = 0; on < 2; on++) {
    on ? setenv("ISAC_PDSCH_SOLVER_FAIRSHARE", "1", 1) : unsetenv("ISAC_PDSCH_SOLVER_FAIRSHARE");
    nr_pdsch_solver_reset();
    int busy = 0, quiet = 0;
    for (uint64_t s = 1000; s < 3000; s++) {
      nr_pdsch_solver_set_now(s);
      int b = nr_pdsch_solver_fair_budget(0x5114, 0.5, true, 24);
      if (b > 0) busy += PlanProbes(0x5114, b);
      if (s % 10 == 0) {
        b = nr_pdsch_solver_fair_budget(0x5098, 0.5, true, 24);
        if (b > 0) quiet += PlanProbes(0x5098, b);
      }
    }
    if (!on) {
      EXPECT_GT(busy, 20 * 2000); /* no share: every grant takes its full budget */
    } else {
      EXPECT_LE(busy, 0.25 * 2000 + 48); /* half of 0.5 probes/slot, plus the cold-start budget */
      EXPECT_GE(quiet, 0.25 * 2000 - 48);
      EXPECT_LE(quiet, 0.25 * 2000 + 48);
      EXPECT_EQ(nr_pdsch_solver_fair_budget(0x5114, 0.5, false, 24), 24); /* work-conserving: no cap without contention */
    }
  }
  unsetenv("ISAC_PDSCH_SOLVER_FAIRSHARE");
  nr_pdsch_solver_reset();
}

/* SOLVERCOVER: the 64-entry RNTI table used to refuse every RNTI after the 64th forever. With FAIRSHARE the least recently
 * active entry idle for >= 2 s of signal (4000 slots) is reused; an entry active within 2 s is never evicted. */
TEST(PdschSolver, FullTableReusesOnlyIdleEntries)
{
  setenv("ISAC_PDSCH_SOLVER_FAIRSHARE", "1", 1);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_now(10000);
  for (int i = 0; i < 64; i++) EXPECT_GT(PlanProbes((uint16_t)(0x4000 + i), 8), 0);
  nr_pdsch_solver_set_now(12000);
  EXPECT_EQ(PlanProbes(0x5000, 8), 0); /* all active within 2 s: refused */
  nr_pdsch_solver_set_now(14500);
  for (int i = 1; i < 64; i++) PlanProbes((uint16_t)(0x4000 + i), 8); /* 0x4000 stays idle */
  EXPECT_GT(PlanProbes(0x5000, 8), 0); /* reuses 0x4000's entry */
  EXPECT_EQ(PlanProbes(0x5001, 8), 0); /* nothing else idle */
  unsetenv("ISAC_PDSCH_SOLVER_FAIRSHARE");
  nr_pdsch_solver_reset();
}

/* DCIRV ISAC_DCI11_RV_CONSISTENCY=1: layout 10 reads the NDI/RV/pid fields one bit off layout 11 (the true one). The gNB sends only
 * new transmissions (rv 0, NDI toggling each pid cycle), so the shifted reading shows rv != 0 on about half of the grants. */
namespace {
// MSB-first 16-bit word (bit 0 is the TDA selector of tda_fn): ndi, rv (2), pid (4); layout 10 reads every field one bit early,
// i.e. rv = [true rv LSB, pid MSB] as on the OTA TIM cell.
bool hq_fn(void *, uint16_t layout, nr_dci_bits_t p, uint8_t *ndi, uint8_t *rv, uint8_t *pid)
{
  const int o = layout == 10 ? 1 : 0;
  auto fld = [&](int pos, int w) { return (unsigned)(p.w[0] >> (16 - pos - w)) & ((1u << w) - 1); };
  *ndi = (uint8_t)fld(o, 1);
  *rv = (uint8_t)fld(o + 1, 2);
  *pid = (uint8_t)fld(o + 3, 4);
  return true;
}
nr_dci_bits_t Dci(int g, uint64_t tda) { return {{tda | (uint64_t)((g / 16) & 1) << 15 | (uint64_t)(g % 16) << 9, 0, 0}}; }
void NoteDcis(uint16_t rnti, int from, int to)
{
  for (int g = from; g < to; g++) nr_pdsch_solver_note_dci(rnti, Dci(g, 0), (uint64_t)g + 1);
}
void RvcSetup(bool on)
{
  on ? setenv("ISAC_DCI11_RV_CONSISTENCY", "1", 1) : unsetenv("ISAC_DCI11_RV_CONSISTENCY");
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  nr_pdsch_solver_set_hq_fn(hq_fn, nullptr);
}
Cell RvcCell()
{
  Cell c;
  c.truth = {10, 11};
  for (int i = 20; i < 60; i++) c.wrong.push_back((uint16_t)i);
  return c;
}
} // namespace

TEST(PdschSolverRvc, BinomialBounds)
{
  EXPECT_FALSE(nr_pdsch_solver_hq_refuted(10, 0));
  EXPECT_FALSE(nr_pdsch_solver_hq_refuted(3, 3)); // too few grants to refute anything
  EXPECT_TRUE(nr_pdsch_solver_hq_refuted(48, 20));
  EXPECT_FALSE(nr_pdsch_solver_hq_refuted(48, 8));
  EXPECT_FALSE(nr_pdsch_solver_hq_verified(8, 0));
  EXPECT_TRUE(nr_pdsch_solver_hq_verified(12, 0));
  EXPECT_FALSE(nr_pdsch_solver_hq_verified(48, 20));
}

TEST(PdschSolverRvc, ShiftedLayoutLosesTheRepresentative)
{
  for (int on = 0; on < 2; on++) {
    RvcSetup(on);
    std::mt19937 rng(3);
    const Cell c = RvcCell();
    NoteDcis(0x5264, 0, 64);
    ASSERT_GT(SolveGrants(0x5264, c, 0, 16, 400, rng), 0);
    EXPECT_EQ(nr_pdsch_solver_rep(0x5264), on ? 11 : 10) << "flag " << on;
  }
  unsetenv("ISAC_DCI11_RV_CONSISTENCY");
  nr_pdsch_solver_reset();
}

TEST(PdschSolverRvc, SolvedRntiIsReselectedByLaterHarqEvidenceAndRescueNotes)
{
  RvcSetup(true);
  std::mt19937 rng(4);
  const Cell c = RvcCell();
  ASSERT_GT(SolveGrants(0x5264, c, 0, 16, 400, rng), 0);
  ASSERT_EQ(nr_pdsch_solver_rep(0x5264), 10) << "no HARQ evidence yet: lowest id";
  NoteDcis(0x5264, 0, 12); // too few to refute, so only the rescue notes may move it
  EXPECT_EQ(nr_pdsch_solver_rep(0x5264), 10);
  nr_pdsch_solver_note_rv_mismatch(0x5264);
  nr_pdsch_solver_note_rv_mismatch(0x5264);
  EXPECT_EQ(nr_pdsch_solver_rep(0x5264), 10);
  nr_pdsch_solver_note_rv_mismatch(0x5264);
  EXPECT_EQ(nr_pdsch_solver_rep(0x5264), 11) << "3 RV rescues re-select";
  // a second RNTI: enough evidence alone moves it
  ASSERT_GT(SolveGrants(0x5265, c, 0, 16, 400, rng), 0);
  ASSERT_EQ(nr_pdsch_solver_rep(0x5265), 10);
  NoteDcis(0x5265, 0, 64);
  EXPECT_EQ(nr_pdsch_solver_rep(0x5265), 11);
  unsetenv("ISAC_DCI11_RV_CONSISTENCY");
  nr_pdsch_solver_reset();
}

TEST(PdschSolverRvc, McsZeroSolutionSeedsTheCellOnlyOnceHarqVerified)
{
  setenv("ISAC_PDSCH_SOLVER_REUSE", "1", 1);
  for (int on = 0; on < 2; on++) {
    RvcSetup(on);
    std::mt19937 rng(5);
    Cell c;
    c.add_pos = 2;
    c.truth = {11};
    for (int i = 20; i < 60; i++) c.wrong.push_back((uint16_t)i);
    ASSERT_GT(SolveGrants(0x00a1, c, 0, 16, 400, rng), 0);
    EXPECT_EQ(FirstAddPos(0x00b2, c, rng), on ? 1 : 2) << "MCS-0-only evidence" << (on ? ": no seeding" : "");
    NoteDcis(0x00a1, 0, 64);
    EXPECT_EQ(FirstAddPos(0x00b3, c, rng), 2) << "HARQ readings of the representative verify it";
  }
  unsetenv("ISAC_DCI11_RV_CONSISTENCY");
  unsetenv("ISAC_PDSCH_SOLVER_REUSE");
  nr_pdsch_solver_reset();
}
