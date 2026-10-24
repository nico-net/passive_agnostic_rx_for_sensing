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

/* DCI11-RESOLVER: the one-layer-QPSK preference (and its WL-SOLVER rotation) and the 48-RB / BWP-width search cap are gone --
 * the tests OneLayerQpskReadingsGetFirstProbesAndFallBackAtOnce, QpskPreferenceRotatesSoAnotherModulationIsProbed and
 * SearchCapFollowsBwpWidth pinned exactly those priors. The search is now ordered and weighted by the DM-RS fit of each
 * reading (reading.fit) and decided by CRC. */
TEST(PdschSolver, TheDmrsFitOrdersTheSearchNotTheModulation)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  /* Iliad's situation: a wrong layout always offers a one-layer QPSK reading, the true one is 64QAM over the full BWP. */
  uint16_t qpsk_ids[] = {1, 2}, qam_ids[] = {3, 4};
  nr_pdsch_solver_reading_t rd[] = {{qpsk_ids, 2, 20, 0x1, 1, 3, 0.15f}, {qam_ids, 2, 51, 0x1, 1, 21, 0.95f}};
  nr_dci_bits_t p = {{42, 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  int n = nr_pdsch_solver_plan(0x3333, p, 48, 21, 2, rd, 2, 8, v);
  ASSERT_GT(n, 0);
  EXPECT_EQ(v[0].reading, 1) << "the reading the DM-RS supports is probed first, whatever its modulation";
  int qam = 0, qpsk = 0;
  for (int i = 0; i < n; i++) { (v[i].reading == 1 ? qam : qpsk)++; nr_pdsch_solver_feedback(v[i].token, false, true); }
  for (int g = 0; g < 60; g++) {
    p.w[0] = 100 + g;
    n = nr_pdsch_solver_plan(0x3333, p, 48, 21, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++) { (v[i].reading == 1 ? qam : qpsk)++; nr_pdsch_solver_feedback(v[i].token, false, true); }
  }
  EXPECT_GT(qam, 3 * qpsk) << "probe share follows the fit";
  EXPECT_GT(qpsk, 0) << "a low fit lowers the share, it never removes a reading";
  nr_pdsch_solver_reset();
}

/* The 1-in-8 search back-off after 200 fruitless probes is a compute bound for a search the air does not support; a DCI whose
 * reading is well supported by the DM-RS (fit >= 0.8) is still searched. */
TEST(PdschSolver, BackoffAppliesOnlyWithoutDmrsSupport)
{
  for (float fit : {-1.f, 0.9f}) {
    nr_pdsch_solver_reset();
    nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
    nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
    uint16_t ids[] = {1, 2};
    nr_pdsch_solver_reading_t rd[] = {{ids, 2, 20, 0x1, 1, 3, fit}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    int planned = 0;
    for (int g = 0; g < 400; g++) {
      nr_dci_bits_t p = {{(uint64_t)(9 + g), 0, 0}};
      const int n = nr_pdsch_solver_plan(0x3535, p, 49, 3, 2, rd, 1, 8, v);
      if (g >= 200) planned += n > 0;
      for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
    }
    if (fit < 0) EXPECT_LT(planned, 40) << "no DM-RS evidence: backed off";
    else EXPECT_EQ(planned, 200) << "DM-RS-supported grants keep being searched";
  }
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, WithoutDmrsEvidenceEveryReadingIsProbedAlike)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  uint16_t a_ids[] = {1, 2}, b_ids[] = {3, 4};
  nr_pdsch_solver_reading_t rd[] = {{a_ids, 2, 20, 0x1, 1, 3, -1.f}, {b_ids, 2, 273, 0x3, 2, 27, -1.f}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  int a = 0, b = 0;
  for (int g = 0; g < 40; g++) {
    nr_dci_bits_t p = {{(uint64_t)(7 + g), 0, 0}};
    const int n = nr_pdsch_solver_plan(0x3434, p, 49, 3, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++) { (v[i].reading ? b : a)++; nr_pdsch_solver_feedback(v[i].token, false, true); }
  }
  EXPECT_GT(a, 0);
  EXPECT_GT(b, 0) << "a 273-RB two-layer 64QAM reading is searched like any other (no width cap, no modulation preference)";
  EXPECT_LT(abs(a - b), (a + b) / 3) << "equal weight without evidence";
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, ConfirmationRejectsIncompatibleMcsTables)
{
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
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, CoverageSurvivesReorderedReadingsAndIdentityChanges)
{
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
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, CoverageKeepsStartSymbolSearchSeparateForEachTdra)
{
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
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, IneligibleTablesDoNotConsumeLaterQpskTrials)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  const uint16_t id = 100;
  nr_pdsch_solver_reading_t rd = {&id, 1, 16, 1, 1, 14};
  nr_dci_bits_t p = {{42, 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 14, 2, &rd, 1, 1, v), 1);
  /* DCI11-RESOLVER: no QPSK-only table filter any more (it made MCS 14 try only table 2); every table this MCS reads
   * differently is a hypothesis, the first in the coverage order being table 1 at S 1 */
  EXPECT_EQ(v[0].table, 1);
  EXPECT_EQ(v[0].S, 1);
  nr_pdsch_solver_feedback(v[0].token, false, true);
  rd.mcs = 1;
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 1, 2, &rd, 1, 1, v), 1);
  EXPECT_EQ(v[0].table, 1);
  EXPECT_EQ(v[0].S, 0); // (table 1, S 1) was tried on the MCS-14 grant: the same physical hypothesis is not repeated
  nr_pdsch_solver_feedback(v[0].token, false, true);
  nr_pdsch_solver_reset();
}

TEST(PdschSolver, LayoutRecoveryNeedsCreditedCrcFailures)
{
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
  EXPECT_FALSE(nr_pdsch_solver_reopen_layouts(0x7788)); // 1 credited failure of the 32 needed
  bool reopened = false;
  for (int round = 0; round < 60 && !reopened; round++) {
    const int n = nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 3, v);
    if (n == 0) break;
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
    reopened = nr_pdsch_solver_reopen_layouts(0x7788);
  }
  EXPECT_TRUE(reopened);
  EXPECT_EQ(nr_pdsch_solver_rep(0x7788), -1); // reopening is not a lock
}

TEST(PdschSolver, SolvesTheSimulatedCellWithTwoIndependentPasses)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  std::mt19937 rng(5);
  const int g = RunCell(c, 0x8f5a, -1, 400, rng);
  printf("[solver] solved after %d grants\n", g);
  ASSERT_GT(g, 0);
  EXPECT_LE(g, 400);
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

static void FullDecodes(uint16_t rnti, const Cell &c, int k, bool ok, std::mt19937 &rng);
TEST(PdschSolver, TheCellPriorSolvesASecondRntiFaster)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  std::mt19937 rng(11);
  const int g1 = RunCell(c, 0x0001, -1, 400, rng);
  ASSERT_GT(g1, 0);
  FullDecodes(0x0001, c, 20, true, rng); // the cell prior only comes from a CRC-proven solution
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

// TIMBIMODAL: a solution seeds other RNTIs (cell prior, REUSE) only once it decoded a full
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
  Cell fake;
  fake.add_pos = 2;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? fake.truth : fake.wrong).push_back((uint16_t)i);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(21);
  ASSERT_GT(RunCell(fake, 0x00a1, -1, 400, rng), 0);
  EXPECT_EQ(FirstAddPos(0x00b2, fake, rng), 1) << "solved, no full TB yet: no seeding";
  FullDecodes(0x00a1, fake, 700, false, rng); // never decodes: reopened after 256 fulls
  EXPECT_LT(nr_pdsch_solver_rep(0x00a1), 0);
  EXPECT_EQ(FirstAddPos(0x00b2, fake, rng), 1);
  // control: a solution that decodes a full TB seeds the cell
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng2(22);
  ASSERT_GT(RunCell(fake, 0x00a1, -1, 400, rng2), 0);
  FullDecodes(0x00a1, fake, 20, true, rng2);
  EXPECT_EQ(FirstAddPos(0x00b2, fake, rng2), 2);
  nr_pdsch_solver_reset();
}

/* TIMSOLVE2. A busy RNTI that already spent 20000 credited search probes (CRC16 code blocks: chance
 * passes Poisson with mean 20000 x 2^-16 = 0.31) is not solved by two consistent CRC16 passes (P = 0.037); a quiet RNTI is; one
 * CRC24 pass (TBS > 3824) solves at once. */
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
  for (int k = 0, g = 0; k < n_probes && g < 8 * n_probes; g++) { // grants in the backoff window plan nothing
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, 0, 2, rd, 1, 8, v);
    for (int i = 0; i < n; i++, k++) nr_pdsch_solver_feedback(v[i].token, false, true);
  }
}
TEST(PdschSolver, SignificanceRuleScalesTheSolveEvidenceWithTheProbesSpent)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(31);
  int grants[3];
  grants[0] = SolveGrants(0x0101, c, 0, 16, 4000, rng);  /* quiet, CRC16 (MCS 0 on 16 PRBs) */
  BurnProbes(0x0202, c, 20000, rng);
  grants[1] = SolveGrants(0x0202, c, 0, 16, 4000, rng);  /* busy, CRC16 */
  grants[2] = SolveGrants(0x0303, c, 20, 16, 4000, rng); /* CRC24: 64QAM/256QAM-class MCS 20 on 16 PRBs */
  printf("[solver] grants to solve: quiet %d, busy %d, crc24 %d\n", grants[0], grants[1], grants[2]);
  for (int k = 0; k < 3; k++) ASSERT_GT(grants[k], 0);
  EXPECT_GT(grants[1], grants[0]) << "a busy RNTI needs more passes";
}

/* SOLVERWIDE (2026-10-23) bounded the search to readings <= 48 PRB and opened wide readings only after 16 grants without a
 * narrow one; WL-SOLVER widened the bound to the BWP below 96 PRB. DCI11-RESOLVER removes the bound: a full-carrier reading is
 * searched from the first grant (the tests WideOnlyRntiIsSolvedAfterTheWaitAndNarrowIsUnchanged / OneNarrowReadingKeepsTheOldBound
 * pinned the bound and the wait). */
static int CountProbes(uint16_t rnti, const Cell &c, int nrb, int grants, std::mt19937 &rng)
{
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), (uint16_t)nrb, 1, 1, 0},
                                     {c.truth.data(), (int)c.truth.size(), (uint16_t)nrb, 1, 1, 0}};
  int total = 0;
  for (int g = 0; g < grants; g++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, 0, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
    total += n;
  }
  return total;
}
TEST(PdschSolverWide, WideReadingsAreSearchedFromTheFirstGrant)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(41);
  EXPECT_GT(CountProbes(0x1234, c, 106, 1, rng), 0) << "a full-carrier grant is probed at once";
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 ra(42), rb(42), rc(42);
  const int gw = SolveGrants(0x1234, c, 0, 106, 4000, ra);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  const int g273 = SolveGrants(0x2345, c, 0, 273, 4000, rb);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  const int gn = SolveGrants(0x0101, c, 0, 16, 4000, rc);
  printf("[solver] 106 PRB solved after %d grants, 273 PRB after %d, 16 PRB %d\n", gw, g273, gn);
  ASSERT_GT(gn, 0);
  EXPECT_LE(gw, gn) << "a wide reading is searched like a narrow one (its larger TB may even solve on one CRC24 pass)";
  EXPECT_LE(g273, gn);
}

/* SOLVERTDDL (2026-10-23). A solved RNTI's second TDA index is a TDD special-slot allocation whose PDSCH is shorter than 14 - S.
 * Before: every probe used L = 14 - S and failed, S was never learnt and the grants were decoded with wrong parameters (rfsim
 * 106 PRB, slots 7/17: 13 % of all grants, skip=1). Now the length is searched too and learnt per TDA index. */
TEST(PdschSolverTddL, SpecialSlotLengthIsLearntAfterSolving)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(51);
  { // solve on tda-0 grants only (the special slot has not been seen yet)
    nr_pdsch_solver_reading_t r2[2] = {{c.wrong.data(), (int)c.wrong.size(), 16}, {c.truth.data(), (int)c.truth.size(), 16}};
    for (int g = 0; g < 400 && nr_pdsch_solver_rep(0x1234) < 0; g++) {
      nr_dci_bits_t p = {{(uint64_t)rng() & ~1ull, 0, 0}};
      nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
      const int n = nr_pdsch_solver_plan(0x1234, p, 49, (g % 3 == 0) ? 2 : 0, 2, r2, 2, 8, v);
      for (int i = 0; i < n; i++)
        nr_pdsch_solver_feedback(v[i].token, v[i].reading == 1 && v[i].S == 1 && v[i].L == 13 && v[i].add_pos == c.add_pos && v[i].table == c.table, true);
    }
  }
  ASSERT_GE(nr_pdsch_solver_rep(0x1234), 0);
  // tda 0: S=1 L=13 (full slot). tda 1 (special slot): S=0, L=8. The cell's S_true[1] is replaced here.
  nr_pdsch_solver_reading_t rd[1] = {{c.truth.data(), (int)c.truth.size(), 16, 1, 1, 2}};
  int full_ok_tda1 = 0, grants_tda1 = 0, first_ok = -1;
  for (int k = 0; k < 600; k++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    const int tda = (int)(p.w[0] & 1);
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x1234, p, 49, 2, 2, rd, 1, 8, v);
    grants_tda1 += tda;
    for (int i = 0; i < n; i++) {
      const bool ok = v[i].add_pos == c.add_pos && v[i].table == c.table &&
                      (tda == 0 ? (v[i].S == 1 && v[i].L == 13) : (v[i].S == 0 && v[i].L == 8));
      if (!v[i].probe && ok && tda == 1) { full_ok_tda1++; if (first_ok < 0) first_ok = grants_tda1; }
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
  }
  printf("[solver] special slot: first full decode with the learnt length after %d tda-1 grants, %d of %d tda-1 grants\n", first_ok,
         full_ok_tda1, grants_tda1);
  ASSERT_GT(first_ok, 0);
  EXPECT_LE(first_ok, 40);
  EXPECT_GE(full_ok_tda1, grants_tda1 - 40);
  nr_pdsch_solver_reset();
}

/* SOLVERPHASE (2026-10-23). rfsim 106 PRB: slots 7 and 17 of 20 are TDD special slots whose PDSCH is shorter and starts elsewhere, but the DCI
 * layout the solver settles on carries no TDA field, so S / L learnt on full slots were applied to them: 13 % of all grants decoded with
 * wrong parameters (skip=1, CRC fail). A solved RNTI whose full decodes fail at one slot phase while it decodes elsewhere searches (S, L) there. */
TEST(PdschSolverPhase, SpecialSlotPhaseIsLearntFromItsOwnFailures)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(61);
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), 16, 1, 1, 2}, {c.truth.data(), (int)c.truth.size(), 16, 1, 1, 2}};
  int special_ok = 0, special = 0, full_ok = 0, full = 0, first_ok = -1;
  for (int g = 0; g < 4000; g++) {
    const int ph = g % 20;
    const bool sp = ph == 7 || ph == 17;
    nr_pdsch_solver_set_now(1000 + g);
    nr_dci_bits_t p = {{(uint64_t)rng() & ~1ull, 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x1234, p, 49, 2, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++) {
      const bool ok = v[i].reading == 1 && v[i].add_pos == c.add_pos && v[i].table == c.table &&
                      (sp ? (v[i].S == 0 && v[i].L == 8) : (v[i].S == 1 && v[i].L == 13));
      if (!v[i].probe) { (sp ? special : full)++; (sp ? special_ok : full_ok) += ok; if (sp && ok && first_ok < 0) first_ok = g; }
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
  }
  printf("[solver] phase: special slots %d of %d full decodes ok (first at grant %d), full slots %d of %d\n", special_ok, special, first_ok,
         full_ok, full);
  ASSERT_GT(first_ok, 0);
  EXPECT_GE(special_ok, special - 400) << "after the learning the special slots decode";
  EXPECT_GE(full_ok, full - 5) << "full slots are unaffected";
  nr_pdsch_solver_reset();
}

/* DCI11-RESOLVER: the mixed slot of the rfsim 106 bed. Of the five CRC-equivalent true layouts only layout 12 reads the TDA field
 * (bit 1 here: index 1 in the mixed slot, whose PDSCH is S0 L8); the others read no TDA field (index 0 always), and the lowest of
 * them solves first. The phase learner's pass in the mixed slot is (S, L) evidence: a layout mapping S1 L13 and S0 L8 to one index
 * is inconsistent, so the representative moves to layout 12, and from then on the TDA value of EACH grant picks the allocation --
 * also for a mixed-slot grant in a slot phase that was never learnt (which the phase learner alone cannot do). */
static int tda_fn_mixed(void *, uint16_t layout, nr_dci_bits_t p) { return layout == 12 ? (int)((p.w[0] >> 1) & 1) : 0; }
TEST(PdschSolverPhase, MixedSlotIsDecidedByTheTdaFieldOnceItExplainsThePasses)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn_mixed, nullptr);
  std::mt19937 rng(67);
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), 16, 1, 1, 2}, {c.truth.data(), (int)c.truth.size(), 16, 1, 1, 2}};
  int late_ok = 0, late = 0;
  for (int g = 0; g < 6000; g++) {
    const int ph = g % 20;
    const bool sp = g < 5000 ? (ph == 7 || ph == 17) : (ph == 3); /* last 1000 grants: the mixed slot moves to an unlearnt phase */
    nr_pdsch_solver_set_now(1000 + g);
    nr_dci_bits_t p = {{((uint64_t)rng() & ~3ull) | (sp ? 2u : 0u), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x1234, p, 49, 2, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++) {
      const bool ok = v[i].reading == 1 && v[i].add_pos == c.add_pos && v[i].table == c.table &&
                      (sp ? (v[i].S == 0 && v[i].L == 8) : (v[i].S == 1 && v[i].L == 13));
      if (!v[i].probe && g >= 5000 && sp) { late++; late_ok += ok; }
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
  }
  printf("[solver] mixed slot by TDA field: rep %d, mixed-slot grants in an unlearnt phase %d of %d ok\n", nr_pdsch_solver_rep(0x1234),
         late_ok, late);
  EXPECT_EQ(nr_pdsch_solver_rep(0x1234), 12) << "the layout whose TDA field explains the mixed slot must represent the RNTI";
  ASSERT_GT(late, 20);
  EXPECT_GE(late_ok, late - 2) << "the TDA value of the grant, not the slot phase, picks the allocation";
  nr_pdsch_solver_reset();
}

/* TIMSOLVE2: once a solution is CRC-proven, an unsolved RNTI tries the proven layout dims with
 * EACH of its own identities in the proven form (here DM-RS id + data id 923) on one grant. */
TEST(PdschSolver, ProvenDimsAreTriedWithEveryOwnIdentity)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  const int32_t kForm = (923 + 1) << 16;
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
  EXPECT_TRUE(id202 && id303) << "both own identities on the proven dims in one grant";
  nr_pdsch_solver_reset();
}

/* Fix A: the DCI 1_1 TDRA field. Payloads are the measured TIM (PCI 923) DCIs: 47 bits, bits 16-17 right before the MCS
 * (bits 18-22), `01` in the TDD special slot (slot 13), `00` elsewhere [MEASURED, DGX replay raw_pci923_tim_12s_tile3, RMDCI]. */
static nr_dci_bits_t tf_bits(const char *s)
{
  nr_dci_bits_t b = {{0, 0, 0}};
  const int n = (int)strlen(s);
  for (int i = 0; i < n; i++)
    if (s[i] == '1') b.w[0] |= UINT64_C(1) << (n - 1 - i);
  return b;
}
static const char *kTimSlot13 = "11010000000000000100001100001000010001101010000";
static const char *kTimSlot6 = "11010000000000000000001100001000010001101010000"; /* same DCI with the field `00` */

TEST(TdaField, TimSpecialSlotPicksRow1AndRejectsIllegalRow0)
{
  nr_tda_field_reset();
  const uint8_t S3[3] = {1, 1, 1}, L3[3] = {13, 5, 3};
  nr_tda_field_set_rows(S3, L3, 3);
  const nr_dci_bits_t p13 = tf_bits(kTimSlot13), p6 = tf_bits(kTimSlot6);
  EXPECT_EQ(nr_dci_bits_field(&p13, 47, 16, 2), 1u);
  EXPECT_EQ(nr_dci_bits_field(&p13, 47, 18, 5), 1u); /* MCS 1 right behind the field */
  uint8_t S[2], L[2];
  bool det = true;
  /* before any evidence: the mixed slot (last PDSCH symbol 9) keeps only the SIB1 rows that fit, row 0 (S1 L13) is rejected */
  EXPECT_EQ(nr_tda_field_rows(16, p13, 47, 9, 2, S, L, &det), 2);
  EXPECT_FALSE(det);
  EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 5); EXPECT_EQ(L[1], 3);
  EXPECT_EQ(nr_tda_field_rows(16, p6, 47, 13, 2, S, L, &det), 0); /* full slot, no detected field: the solver's reading stands */
  /* evidence: `00` in 60 full slots, `01` in 20 mixed slots */
  for (int i = 0; i < 60; i++) nr_tda_field_note_dci(16, p6, 47, false);
  for (int i = 0; i < 20; i++) nr_tda_field_note_dci(16, p13, 47, true);
  EXPECT_EQ(nr_tda_field_rows(16, p13, 47, 9, 2, S, L, &det), 1); /* detected: the field names the row, no second decode */
  EXPECT_TRUE(det);
  EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 5);  /* value 1 -> SIB1 row 1 first, mask 0x4 (DM-RS at 2 only) */
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(2, S[0], L[0], 1), 0x4);
  EXPECT_EQ(nr_tda_field_rows(16, p6, 47, 13, 2, S, L, &det), 1);
  EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 13); /* slot 6: row 0, mask 0x804 */
  EXPECT_EQ(nr_pdsch_solver_dmrs_mask(2, S[0], L[0], 1), 0x804);
  /* a value that names an illegal row in a mixed slot gives no row (the solver's reading stands) */
  EXPECT_EQ(nr_tda_field_rows(16, p6, 47, 9, 2, S, L, &det), 0);
}

TEST(TdaField, ConstantFieldIsNotAccepted)
{
  nr_tda_field_reset();
  const uint8_t S3[3] = {1, 1, 1}, L3[3] = {13, 5, 3};
  nr_tda_field_set_rows(S3, L3, 3);
  const nr_dci_bits_t p6 = tf_bits(kTimSlot6);
  for (int i = 0; i < 200; i++) { nr_tda_field_note_dci(16, p6, 47, i % 5 == 0); }
  uint8_t S[2], L[2];
  bool det = true;
  nr_tda_field_rows(16, p6, 47, 13, 2, S, L, &det);
  EXPECT_FALSE(det);
}

TEST(TdaField, NsaCellLearnsRowsFromCrcPasses)
{
  /* V (PCI 415, NSA, no SIB1 list): bit 16 = 1 <=> S0 L14 decodes, 0 <=> S1 L13 [MEASURED, DGX replay b, 448 of 449 / 1195 of 1197] */
  nr_tda_field_reset();
  const nr_dci_bits_t v0 = tf_bits("1100000000100000000000000100011000110010010011000"), v1 = tf_bits("1100000000100000100000000100011000110010010011000");
  uint8_t S[2], L[2];
  bool det = true;
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 13, 2, S, L, &det), 0);
  for (int i = 0; i < 30; i++) { nr_tda_field_note_ok(16, v0, 49, false, 1, 13); nr_tda_field_note_ok(16, v1, 49, false, 0, 14); }
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 13, 2, S, L, &det), 1);
  EXPECT_TRUE(det);
  EXPECT_EQ(S[0], 0); EXPECT_EQ(L[0], 14);
  EXPECT_EQ(nr_tda_field_rows(16, v0, 49, 13, 2, S, L, &det), 1);
  EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 13);
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 9, 2, S, L, &det), 0); /* S0 L14 does not fit a mixed slot; nothing learnt there */
}

TEST(TdaField, UncorrelatedBitIsNotAccepted)
{
  nr_tda_field_reset();
  const nr_dci_bits_t v0 = tf_bits("1100000000100000000000000100011000110010010011000"), v1 = tf_bits("1100000000100000100000000100011000110010010011000");
  for (int i = 0; i < 100; i++) { nr_tda_field_note_ok(16, i % 2 ? v1 : v0, 49, false, 1, 13); }
  uint8_t S[2], L[2];
  bool det = true;
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 13, 2, S, L, &det), 0);
  EXPECT_FALSE(det);
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/* SOLVERCOVER: an RNTI with a grant in every slot and one with a grant every 10 slots share the
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
  EXPECT_LE(busy, 0.25 * 2000 + 48); /* half of 0.5 probes/slot, plus the cold-start budget */
  EXPECT_GE(quiet, 0.25 * 2000 - 48);
  EXPECT_LE(quiet, 0.25 * 2000 + 48);
  EXPECT_EQ(nr_pdsch_solver_fair_budget(0x5114, 0.5, false, 24), 24); /* work-conserving: no cap without contention */
  nr_pdsch_solver_reset();
}

/* SOLVERCOVER: the 64-entry RNTI table used to refuse every RNTI after the 64th forever. The least recently
 * active entry idle for >= 2 s of signal (4000 slots) is reused; an entry active within 2 s is never evicted. */
TEST(PdschSolver, FullTableReusesOnlyIdleEntries)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_now(10000);
  for (int i = 0; i < 64; i++) EXPECT_GT(PlanProbes((uint16_t)(0x4000 + i), 8), 0);
  nr_pdsch_solver_set_now(12000);
  EXPECT_EQ(PlanProbes(0x5000, 8), 0); /* all active within 2 s: refused */
  nr_pdsch_solver_set_now(14500);
  for (int i = 1; i < 64; i++) PlanProbes((uint16_t)(0x4000 + i), 8); /* 0x4000 stays idle */
  EXPECT_GT(PlanProbes(0x5000, 8), 0); /* reuses 0x4000's entry */
  EXPECT_EQ(PlanProbes(0x5001, 8), 0); /* nothing else idle */
  nr_pdsch_solver_reset();
}

/* DCIRV: layout 10 reads the NDI/RV/pid fields one bit off layout 11 (the true one). The gNB sends only
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
void RvcSetup()
{
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
  RvcSetup();
  std::mt19937 rng(3);
  const Cell c = RvcCell();
  NoteDcis(0x5264, 0, 64);
  ASSERT_GT(SolveGrants(0x5264, c, 0, 16, 4000, rng), 0);
  EXPECT_EQ(nr_pdsch_solver_rep(0x5264), 11);
  nr_pdsch_solver_reset();
}

TEST(PdschSolverRvc, SolvedRntiIsReselectedByLaterHarqEvidenceAndRescueNotes)
{
  RvcSetup();
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
  nr_pdsch_solver_reset();
}

TEST(PdschSolverRvc, McsZeroSolutionSeedsTheCellOnlyOnceHarqVerified)
{
  RvcSetup();
  std::mt19937 rng(5);
  Cell c;
  c.add_pos = 2;
  c.truth = {11};
  for (int i = 20; i < 60; i++) c.wrong.push_back((uint16_t)i);
  ASSERT_GT(SolveGrants(0x00a1, c, 0, 16, 4000, rng), 0);
  EXPECT_EQ(FirstAddPos(0x00b2, c, rng), 1) << "MCS-0-only evidence: no seeding";
  NoteDcis(0x00a1, 0, 64);
  EXPECT_EQ(FirstAddPos(0x00b3, c, rng), 1) << "PRIOR_CRC (frozen on): HARQ verification alone still needs a CRC-proven full TB";
  FullDecodes(0x00a1, c, 20, true, rng);
  EXPECT_EQ(FirstAddPos(0x00b4, c, rng), 2) << "HARQ-verified and CRC-proven: seeds the cell";
  nr_pdsch_solver_reset();
}

/* CRC-BUGS 2026-10-19: a solved RNTI whose representative is in no reading of a DCI gets no full decode of another reading (it used to
 * decode reading 0, another layout's MCS/NDI/rv: Vodafone OTA 787 such decodes, 1 CRC pass); a DCI that has it decodes as before. */
TEST(PdschSolver, SolvedRntiNeverFullDecodesAReadingWithoutItsRepresentative)
{
  RvcSetup();
  std::mt19937 rng(6);
  const Cell c = RvcCell();
  ASSERT_GT(SolveGrants(0x6ba9, c, 0, 16, 400, rng), 0);
  const int rep = nr_pdsch_solver_rep(0x6ba9);
  ASSERT_GE(rep, 10);
  std::vector<uint16_t> other = {30, 31}, with = {(uint16_t)rep};
  nr_pdsch_solver_reading_t rd_other[1] = {{other.data(), 2, 16, 1, 1, 0}};
  nr_pdsch_solver_reading_t rd_both[2] = {{other.data(), 2, 16, 1, 1, 0}, {with.data(), 1, 16, 1, 1, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  const nr_dci_bits_t p = {{0, 0, 0}};
  int n = nr_pdsch_solver_plan(0x6ba9, p, 49, 0, 2, rd_other, 1, 8, v);
  for (int i = 0; i < n; i++) {
    EXPECT_TRUE(v[i].probe) << "only refinement probes on a DCI without the representative's reading";
    nr_pdsch_solver_feedback(v[i].token, false, false);
  }
  n = nr_pdsch_solver_plan(0x6ba9, p, 49, 0, 2, rd_both, 2, 8, v);
  int full = 0;
  for (int i = 0; i < n; i++) {
    full += !v[i].probe && v[i].reading == 1;
    EXPECT_FALSE(!v[i].probe && v[i].reading == 0);
    nr_pdsch_solver_feedback(v[i].token, false, false);
  }
  EXPECT_EQ(full, 1) << "the representative's reading is the full decode";
  nr_pdsch_solver_reset();
}
