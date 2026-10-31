// T26b grant solver (nr_pdsch_solver.c): spec helpers and the search -> candidate -> solved state machine against a
// simulated cell whose CRC oracle passes only the true (reading, S per TDA index, add_pos, table).
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <random>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "nr_pdsch_solver.h"
#include "nr_signal_evidence.h"
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

/* SOLVERTDDEND (2026-10-24). rfsim 273 PRB 4 RX: the special slot (6 DL symbols: PDSCH S1 L5) is decoded as a full slot (S1 L13) until
 * SOLVER_PHASE_FAIL failed full decodes have accumulated at its phase -- 13 of the 15 post-solve full failures of the arm. With the
 * slot's broadcast DL end the phase is searched from its first grant, DL end first; without it (-1) the old wait stands. */
static int RunTddEnd(bool tdd_known, int *fail_before_first_ok, int *full_ok_out, int *full_out)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(71);
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), 16, 1, 1, 2}, {c.truth.data(), (int)c.truth.size(), 16, 1, 1, 2}};
  int first_ok = -1, sp_fail = 0, full_ok = 0, full = 0;
  for (int g = 0; g < 3000; g++) {
    const int ph = g % 20;
    const bool sp = ph == 7 || ph == 17;
    nr_pdsch_solver_set_now(1000 + g);
    nr_pdsch_solver_set_slot_last(tdd_known ? (sp ? 5 : 13) : -1);
    nr_dci_bits_t p = {{(uint64_t)rng() & ~1ull, 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x1234, p, 49, 2, 2, rd, 2, 8, v);
    for (int i = 0; i < n; i++) {
      const bool ok = v[i].reading == 1 && v[i].add_pos == c.add_pos && v[i].table == c.table &&
                      (sp ? (v[i].S == 1 && v[i].L == 5) : (v[i].S == 1 && v[i].L == 13));
      if (!v[i].probe) {
        if (sp && ok && first_ok < 0) first_ok = g;
        if (sp && !ok && first_ok < 0 && nr_pdsch_solver_rep(0x1234) >= 0) sp_fail++;
        if (!sp) { full++; full_ok += ok; }
      }
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
  }
  nr_pdsch_solver_set_slot_last(-1);
  nr_pdsch_solver_reset();
  *fail_before_first_ok = sp_fail;
  *full_ok_out = full_ok;
  *full_out = full;
  return first_ok;
}
TEST(PdschSolverPhase, TddEndSearchesTheSpecialSlotFromItsFirstGrant)
{
  int f_tdd, f_old, ok_tdd, ok_old, n_tdd, n_old;
  const int first_tdd = RunTddEnd(true, &f_tdd, &ok_tdd, &n_tdd);
  const int first_old = RunTddEnd(false, &f_old, &ok_old, &n_old);
  printf("[solver] tdd end: special slot first ok at grant %d after %d failed full decodes (TDD known) vs %d after %d (unknown)\n",
         first_tdd, f_tdd, first_old, f_old);
  ASSERT_GT(first_tdd, 0);
  ASSERT_GT(first_old, 0);
  EXPECT_LE(f_tdd, 2) << "with the broadcast DL end the special slot is searched at its first post-solve grant";
  EXPECT_GE(f_old, 8) << "without TDD knowledge the phase learner still waits for its failures (unchanged rule)";
  EXPECT_LT(first_tdd, first_old);
  EXPECT_GE(ok_tdd, n_tdd - 5) << "full slots are unaffected";
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

TEST(TdaField, DedicatedListWiderThanTwoBitsIsLearntFromCrcPasses)
{
  /* PCI 415 on air (2026-10-29): SIB1 gives 8 common rows (S1/S2 only, field width 3), the C-RNTI DCIs follow a dedicated list
   * whose row S0 L14 is not among them; bit 16 = 1 <=> S0 L14 decodes. The learner must not be held to the common list's width. */
  nr_tda_field_reset();
  const uint8_t S8[8] = {1, 1, 1, 1, 2, 2, 2, 2}, L8[8] = {13, 12, 6, 4, 12, 11, 5, 3};
  nr_tda_field_set_rows(S8, L8, 8);
  const nr_dci_bits_t v0 = tf_bits("1100000000100000000000000100011000110010010011000"), v1 = tf_bits("1100000000100000100000000100011000110010010011000");
  uint8_t S[2], L[2];
  bool det = true;
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 13, 2, S, L, &det), 0); /* full slot, nothing learnt: the solver's reading stands */
  EXPECT_FALSE(det);
  for (int i = 0; i < 30; i++) { nr_tda_field_note_ok(16, v0, 49, false, 1, 13); nr_tda_field_note_ok(16, v1, 49, false, 0, 14); }
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 13, 2, S, L, &det), 1);
  EXPECT_TRUE(det);
  EXPECT_EQ(S[0], 0); EXPECT_EQ(L[0], 14); /* the learnt row, not a common-list row */
  EXPECT_EQ(nr_tda_field_rows(16, v0, 49, 13, 2, S, L, &det), 1);
  EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 13);
}

TEST(TdaField, FourBitFieldIsLearnt)
{
  /* a 16-row dedicated list: values 4 and 5 of bits 16..19 name different rows; their 1..3-bit prefixes are equal */
  nr_tda_field_reset();
  const nr_dci_bits_t a = tf_bits("1100000000100000010000000100011000110010010011000"), b = tf_bits("1100000000100000010100000100011000110010010011000");
  EXPECT_EQ(nr_dci_bits_field(&a, 49, 16, 4), 4u);
  EXPECT_EQ(nr_dci_bits_field(&b, 49, 16, 4), 5u);
  for (int i = 0; i < 30; i++) { nr_tda_field_note_ok(16, a, 49, false, 2, 12); nr_tda_field_note_ok(16, b, 49, false, 0, 14); }
  uint8_t S[2], L[2];
  bool det = false;
  EXPECT_EQ(nr_tda_field_rows(16, b, 49, 13, 2, S, L, &det), 1);
  EXPECT_TRUE(det);
  EXPECT_EQ(S[0], 0); EXPECT_EQ(L[0], 14);
  EXPECT_EQ(nr_tda_field_rows(16, a, 49, 13, 2, S, L, &det), 1);
  EXPECT_EQ(S[0], 2); EXPECT_EQ(L[0], 12);
}

TEST(TdaField, ANewCellForgetsTheLearntFieldButKeepsItsSib1Rows)
{
  nr_tda_field_reset();
  const uint8_t S3[3] = {1, 1, 1}, L3[3] = {13, 5, 3};
  nr_tda_field_set_rows(S3, L3, 3);
  const nr_dci_bits_t v0 = tf_bits("1100000000100000000000000100011000110010010011000"), v1 = tf_bits("1100000000100000100000000100011000110010010011000");
  for (int i = 0; i < 30; i++) { nr_tda_field_note_ok(16, v0, 49, false, 1, 13); nr_tda_field_note_ok(16, v1, 49, false, 0, 14); }
  uint8_t S[2], L[2];
  bool det = false;
  ASSERT_EQ(nr_tda_field_rows(16, v1, 49, 13, 2, S, L, &det), 1);
  ASSERT_TRUE(det);
  nr_tda_field_forget();
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 13, 2, S, L, &det), 0); /* full slot, nothing learnt for this cell yet */
  EXPECT_FALSE(det);
  EXPECT_EQ(nr_tda_field_rows(16, v1, 49, 9, 2, S, L, &det), 2); /* the SIB1 rows are still the mixed-slot prior */
  EXPECT_EQ(L[0], 5);
}

TEST(TdaField, AValueThatNeverPassesFindsItsCrossSlotRow)
{
  /* PCI 415 shape (2026-10-30 OTA): 7 DL slots, special slot 7 (6 DL symbols, 4 UL), UL 8-9. The DCIs of slot 6 carry TDRA values the two
   * learnt rows do not explain and never pass at K0 = 0. Exploration walks the TDD-legal candidates on those grants; the first CRC pass is learnt. */
  nr_tda_field_reset();
  const uint8_t S8[8] = {1, 1, 1, 1, 2, 2, 2, 2}, L8[8] = {13, 12, 6, 4, 12, 11, 5, 3};
  nr_tda_field_set_rows(S8, L8, 8);
  const nr_dci_bits_t v0 = tf_bits("1100000000100000000000000100011000110010010011000"), v4 = tf_bits("1100000000100000100000000100011000110010010011000"),
                      v2 = tf_bits("1100000000100000010000000100011000110010010011000");
  EXPECT_EQ(nr_dci_bits_field(&v2, 49, 16, 3), 2u);
  for (int i = 0; i < 30; i++) { nr_tda_field_note_ok(16, v0, 49, false, 1, 13); nr_tda_field_note_ok(16, v4, 49, false, 0, 14); }
  const int8_t last6[3] = {13, 9, -1}, dl6[3] = {14, 6, 0}; /* DCI in slot 6: slot 7 is the special slot, slot 8 UL */
  uint8_t S[2], L[2], K0[2];
  bool det = false;
  ASSERT_EQ(nr_tda_field_rows_k0(16, v2, 49, last6, dl6, 2, S, L, K0, &det), 1) << "no evidence yet: the SIB1 row of value 2 (a guess)";
  EXPECT_TRUE(det);
  EXPECT_EQ(K0[0], 0); EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 6);
  for (int i = 0; i < 32; i++) nr_tda_field_note_result(16, v2, 49, false, false);
  bool found = false;
  for (int i = 0; i < 12 && !found; i++) {
    ASSERT_EQ(nr_tda_field_rows_k0(16, v2, 49, last6, dl6, 2, S, L, K0, &det), 1);
    ASSERT_LE(K0[0], 1) << "slot 8 is UL: no K0 = 2 candidate";
    ASSERT_LE(S[0] + L[0] - 1, K0[0] ? 9 : 13) << "every candidate fits its slot";
    if (K0[0] == 1 && S[0] == 1 && L[0] == 5) { /* the transmitter's row: CRC pass */
      nr_tda_field_note_ok_k0(16, v2, 49, false, 1, 5, 1);
      found = true;
    } else
      nr_tda_field_note_result(16, v2, 49, false, false);
  }
  ASSERT_TRUE(found) << "the candidate list holds K0 1, S 1, L 5 (ends at the last DL symbol of the special slot)";
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(nr_tda_field_rows_k0(16, v2, 49, last6, dl6, 2, S, L, K0, &det), 1);
    EXPECT_EQ(K0[0], 1); EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 5);
  }
  ASSERT_EQ(nr_tda_field_rows_k0(16, v0, 49, last6, dl6, 2, S, L, K0, &det), 1);
  EXPECT_EQ(K0[0], 0); EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 13); /* the learnt same-slot rows are untouched */
}

TEST(PmExplore, AnUnreadBitThatNeverPassesFindsInterleavedMapping)
{
  /* PCI 415 (2026-10-30 OTA DCIBITS): the one pre-MCS bit at 19 (after a 3-bit TDRA field at 16) never passes when 1 (0/379). */
  nr_tda_field_forget();
  const nr_dci_bits_t b0 = tf_bits("1100000000100000000000000100011000110010010011000"), b1 = tf_bits("1100000000100000000100000100011000110010010011000");
  EXPECT_EQ(nr_dci_bits_field(&b1, 49, 19, 1), 1u);
  EXPECT_EQ(nr_pm_hyp(19, 1, b1, 49), NR_PM_HYP_PLAIN) << "no evidence: plain";
  for (int i = 0; i < 40; i++) { nr_pm_note_result(19, 1, b0, 49, NR_PM_HYP_PLAIN, i % 2); nr_pm_note_result(19, 1, b1, 49, NR_PM_HYP_PLAIN, false); }
  EXPECT_EQ(nr_pm_hyp(19, 1, b0, 49), NR_PM_HYP_PLAIN) << "value 0 passes plain";
  const int h1 = nr_pm_hyp(19, 1, b1, 49), h2 = nr_pm_hyp(19, 1, b1, 49);
  EXPECT_NE(h1, NR_PM_HYP_PLAIN);
  EXPECT_NE(h1, h2) << "the hypotheses are tried in turn";
  nr_pm_note_result(19, 1, b1, 49, NR_PM_HYP_ILV2, false);
  nr_pm_note_result(19, 1, b1, 49, NR_PM_HYP_ILV4, true);
  for (int i = 0; i < 3; i++) EXPECT_EQ(nr_pm_hyp(19, 1, b1, 49), NR_PM_HYP_ILV4) << "learnt";
  nr_tda_field_forget();
  EXPECT_EQ(nr_pm_hyp(19, 1, b1, 49), NR_PM_HYP_PLAIN) << "a new cell starts over";
}

TEST(TdaField, AnUndetectedFieldBootstrapsFromADeadValue)
{
  /* PCI 403 (2026-10-30 OTA): no SIB1 list, field at bit 15; value 0 = S1 L13 passes, value 1 = S2 L12 is never tried by the reading or
   * the S toggle, so it never passes and the field is never detected. Exploration before detection finds it. */
  nr_tda_field_reset();
  const nr_dci_bits_t v0 = tf_bits("110000000010000000000000010001100011001001001100"), v1 = tf_bits("110000000010000100000000010001100011001001001100");
  EXPECT_EQ(nr_dci_bits_field(&v1, 48, 15, 1), 1u);
  for (int i = 0; i < 10; i++) { nr_tda_field_note_ok(15, v0, 48, false, 1, 13); nr_tda_field_note_result(15, v0, 48, false, true); }
  const int8_t last[3] = {13, 13, 13}, dl[3] = {14, 14, 14};
  uint8_t S[2], L[2], K0[2];
  bool det = true;
  for (int i = 0; i < 31; i++) nr_tda_field_note_result(15, v1, 48, false, false);
  EXPECT_EQ(nr_tda_field_rows_k0(15, v1, 48, last, dl, 2, S, L, K0, &det), 0) << "31 failures: not yet";
  EXPECT_FALSE(det);
  nr_tda_field_note_result(15, v1, 48, false, false);
  ASSERT_EQ(nr_tda_field_rows_k0(15, v1, 48, last, dl, 2, S, L, K0, &det), 1);
  EXPECT_FALSE(det);
  EXPECT_EQ(K0[0], 0); EXPECT_EQ(S[0], 2); EXPECT_EQ(L[0], 12) << "the untried start symbol first";
  for (int i = 0; i < 8; i++) nr_tda_field_note_ok_k0(15, v1, 48, false, 2, 12, 0);
  ASSERT_EQ(nr_tda_field_rows_k0(15, v1, 48, last, dl, 2, S, L, K0, &det), 1);
  EXPECT_TRUE(det) << "two values, two rows: rule (b) detects the field";
  EXPECT_EQ(S[0], 2); EXPECT_EQ(L[0], 12);
  ASSERT_EQ(nr_tda_field_rows_k0(15, v0, 48, last, dl, 2, S, L, K0, &det), 1);
  EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 13);
}

TEST(TdaField, AnUndetectedWiderFieldBootstrapsFromItsSecondBit)
{
  /* PCI 403 (2026-10-30 OTA, DCIBITS): bit 15 is always 0, bit 16 = 1 never passes (0/1396), value 0 passes rarely (436/6604); a
   * second reading of the same DCIs puts the field at bit 14. The 2-bit value 01 must be explored. */
  nr_tda_field_reset();
  const nr_dci_bits_t v0 = tf_bits("110000000010000000000000010001100011001001001100"), v1 = tf_bits("110000000010000010000000010001100011001001001100");
  ASSERT_EQ(nr_dci_bits_field(&v1, 48, 15, 1), 0u);
  ASSERT_EQ(nr_dci_bits_field(&v1, 48, 16, 1), 1u);
  const int8_t last[3] = {13, 13, 13}, dl[3] = {14, 14, 14};
  uint8_t S[2], L[2], K0[2];
  bool det = true;
  for (int i = 0; i < 400; i++) {
    nr_tda_field_note_dci(15, i % 5 ? v0 : v1, 48, false);
    nr_tda_field_note_dci(14, i % 5 ? v0 : v1, 48, false);
    if (i % 5) {
      const bool ok = i % 10 == 1;
      if (ok) nr_tda_field_note_ok(15, v0, 48, false, 1, 13);
      nr_tda_field_note_result(15, v0, 48, false, ok);
      nr_tda_field_note_result(14, v0, 48, false, ok);
    } else {
      nr_tda_field_note_result(15, v1, 48, false, false);
      nr_tda_field_note_result(14, v1, 48, false, false);
    }
  }
  ASSERT_EQ(nr_tda_field_rows_k0(15, v1, 48, last, dl, 2, S, L, K0, &det), 1);
  EXPECT_FALSE(det);
  EXPECT_EQ(S[0], 2); EXPECT_EQ(L[0], 12);
}

TEST(TdaField, AStrayPassDoesNotHideADeadValue)
{
  /* PCI 403 (2026-10-30 OTA, learner dump): value 01 passed ONCE (S1 L13, the solver's reading) in 1453 decodes, value 00 1 in 13. The
   * stray pass neither makes the value alive nor a learnt row; the explored row's third pass detects the field and is then used. */
  nr_tda_field_reset();
  const nr_dci_bits_t v0 = tf_bits("110000000010000000000000010001100011001001001100"), v1 = tf_bits("110000000010000010000000010001100011001001001100");
  const int8_t last[3] = {13, 13, 13}, dl[3] = {14, 14, 14};
  uint8_t S[2], L[2], K0[2];
  bool det = true;
  for (int i = 0; i < 260; i++) {
    const bool ok = i % 13 == 0;
    if (ok) nr_tda_field_note_ok(15, v0, 48, false, 1, 13);
    nr_tda_field_note_result(15, v0, 48, false, ok);
  }
  nr_tda_field_note_ok(15, v1, 48, false, 1, 13); /* the stray pass */
  nr_tda_field_note_result(15, v1, 48, false, true);
  for (int i = 0; i < 300; i++) nr_tda_field_note_result(15, v1, 48, false, false);
  ASSERT_EQ(nr_tda_field_rows_k0(15, v1, 48, last, dl, 2, S, L, K0, &det), 1);
  EXPECT_FALSE(det);
  EXPECT_EQ(S[0], 2); EXPECT_EQ(L[0], 12) << "explored, not the stray row";
  for (int i = 0; i < 3; i++) { nr_tda_field_note_ok_k0(15, v1, 48, false, 2, 12, 0); nr_tda_field_note_result(15, v1, 48, false, true); }
  ASSERT_EQ(nr_tda_field_rows_k0(15, v1, 48, last, dl, 2, S, L, K0, &det), 1);
  EXPECT_TRUE(det) << "three passes of the explored row detect the field";
  EXPECT_EQ(S[0], 2); EXPECT_EQ(L[0], 12);
  ASSERT_EQ(nr_tda_field_rows_k0(15, v0, 48, last, dl, 2, S, L, K0, &det), 1);
  EXPECT_EQ(S[0], 1); EXPECT_EQ(L[0], 13);
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
  for (int i = 0; i < NR_PDSCH_SOLVER_RNTIS; i++) EXPECT_GT(PlanProbes((uint16_t)(0x4000 + i), 8), 0);
  nr_pdsch_solver_set_now(12000);
  EXPECT_EQ(PlanProbes(0x6000, 8), 0); /* all active within 2 s: refused */
  nr_pdsch_solver_set_now(14500);
  for (int i = 1; i < NR_PDSCH_SOLVER_RNTIS; i++) PlanProbes((uint16_t)(0x4000 + i), 8); /* 0x4000 stays idle */
  EXPECT_GT(PlanProbes(0x6000, 8), 0); /* reuses 0x4000's entry */
  EXPECT_EQ(PlanProbes(0x6001, 8), 0); /* nothing else idle */
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

/* SOLVERREFUND (2026-10-24). A planned probe the producer refuses before any decode (probe bucket, ring fill, per-occasion cap, expired
 * IQ) comes back with credit=false. It used to stay charged to the coverage table, so the least-tried-first search skipped that
 * hypothesis for a whole round: [MEASURED, sens6 rfsim 273 PRB 4 RX] half the planned probes were throttled and the true hypothesis,
 * throttled once, was never planned again. An untested probe must be planned again first. */
TEST(PdschSolverRefund, UntestedProbeIsPlannedAgainFirst)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  const uint16_t id = 100;
  nr_pdsch_solver_reading_t rd = {&id, 1, 16, 1, 1, 2};
  nr_dci_bits_t p = {{42, 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR], w[NR_PDSCH_SOLVER_MAX_VAR];
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 3, v), 3);
  nr_pdsch_solver_feedback(v[0].token, false, true);  // tested: CRC failed
  nr_pdsch_solver_feedback(v[1].token, false, false); // throttled before decode
  nr_pdsch_solver_feedback(v[2].token, false, false); // cut by the per-occasion cap
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 2, w), 2);
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(w[i].S, v[i + 1].S) << "variant " << i;
    EXPECT_EQ(w[i].add_pos, v[i + 1].add_pos) << "variant " << i;
    EXPECT_EQ(w[i].table, v[i + 1].table) << "variant " << i;
    nr_pdsch_solver_feedback(w[i].token, false, true);
  }
  // the tested hypothesis keeps its charge: it is not planned before the remaining untried ones
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 1, w), 1);
  EXPECT_FALSE(w[0].S == v[0].S && w[0].add_pos == v[0].add_pos && w[0].table == v[0].table);
  nr_pdsch_solver_feedback(w[0].token, false, true);
  nr_pdsch_solver_reset();
}

/* The 273-PRB failure as a cell: every grant's second planned probe is refused (throttled). The true reading still solves, and the
 * refused probes neither burn the hypothesis nor push the RNTI into the 1-in-8 search back-off. */
TEST(PdschSolverRefund, ThrottledSecondProbeStillSolves)
{
  Cell c;
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? c.truth : c.wrong).push_back((uint16_t)i);
  c.table = 0;
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  std::mt19937 rng(77);
  nr_pdsch_solver_reading_t rd[2] = {{c.wrong.data(), (int)c.wrong.size(), 273, 1, 1, 9},
                                     {c.truth.data(), (int)c.truth.size(), 273, 1, 1, 9}};
  int solved_at = -1;
  for (int g = 0; g < 3000 && solved_at < 0; g++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    const int tda = (int)(p.w[0] & 1);
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x4321, p, 49, 9, 2, rd, 2, 2, v);
    for (int i = 0; i < n; i++) {
      if (i == 1) { nr_pdsch_solver_feedback(v[i].token, false, false); continue; } // refused before decode
      const bool ok = v[i].reading == 1 && v[i].S == c.S_true[tda] && v[i].add_pos == c.add_pos && v[i].table == c.table;
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
    if (nr_pdsch_solver_rep(0x4321) >= 0) solved_at = g + 1;
  }
  printf("[solver] second probe of every grant throttled: solved after %d grants\n", solved_at);
  ASSERT_GT(solved_at, 0);
  const int rep = nr_pdsch_solver_rep(0x4321);
  EXPECT_TRUE(rep >= 10 && rep < 15);
  EXPECT_LE(solved_at, 400);
  nr_pdsch_solver_reset();
}

/* SOLVERREFUND, second gap: a probe whose pending entry the 4096-entry FIFO overwrites before any feedback can never report back;
 * it is untested and must be refunded like an uncredited one. */
TEST(PdschSolverRefund, OverwrittenPendingEntryIsRefunded)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(nullptr, nullptr);
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  const uint16_t id = 100, other = 200;
  nr_pdsch_solver_reading_t rd = {&id, 1, 16, 1, 1, 2}, ro = {&other, 1, 16, 1, 1, 2};
  nr_dci_bits_t p = {{42, 0, 0}};
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR], w[NR_PDSCH_SOLVER_MAX_VAR];
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 2, v), 2); // planned, never reported back (lost in the queue)
  int emitted = 0;
  for (int g = 0; emitted < 4200 && g < 5000; g++) { // another RNTI's probes wrap the FIFO over 0x7788's entries
    nr_dci_bits_t q = {{(uint64_t)(1000 + g), 0, 0}};
    const int n = nr_pdsch_solver_plan(0x1111, q, 49, 2, 2, &ro, 1, 24, w);
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(w[i].token, false, false); // uncredited: no search back-off
    emitted += n;
  }
  ASSERT_GE(emitted, 4096);
  nr_pdsch_solver_feedback(v[0].token, false, true); // stale tokens: their entries are gone, nothing is credited
  nr_pdsch_solver_feedback(v[1].token, false, true);
  ASSERT_EQ(nr_pdsch_solver_plan(0x7788, p, 49, 2, 2, &rd, 1, 2, w), 2);
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(w[i].S, v[i].S) << "variant " << i;
    EXPECT_EQ(w[i].add_pos, v[i].add_pos) << "variant " << i;
    EXPECT_EQ(w[i].table, v[i].table) << "variant " << i;
    nr_pdsch_solver_feedback(w[i].token, false, true);
  }
  nr_pdsch_solver_reset();
}

/* SOLVERGATE: the probes the planner was granted are admitted even when the grant's own enqueues push the ring to the fill bound
 * (20-deep ring, 25 % bound: the old per-probe re-check refused the 2nd probe of a 2-probe plan at 3 queued jobs). */
TEST(ProbeGate, PlannedProbesAreAdmittedFromTheReservation)
{
  nr_probe_gate_t g;
  nr_probe_gate_init(&g, 0.5, 16.0);
  const int depth = 20, bound = 25;
  int held = 3, resv = 0;
  auto headroom = [&]() { const int n = (bound * depth - 100 * held + 99) / 100; return n > 0 ? n : 0; };
  auto ring_ok = [&]() { return 100 * held / depth < bound; };
  const int planned = nr_probe_gate_reserve(&g, 10, headroom(), 24, &resv);
  EXPECT_EQ(planned, 2) << "16 tokens, but only 2 more jobs keep the ring below 25 %";
  int admitted = 0;
  for (int k = 0; k < planned; k++)
    if (nr_probe_gate_take(&g, 10, ring_ok(), &resv)) { admitted++; held++; }
  EXPECT_EQ(admitted, planned) << "every planned probe is admitted";
  EXPECT_FALSE(nr_probe_gate_take(&g, 10, ring_ok(), &resv)) << "beyond the plan the live bound still holds (ring at 25 %)";
  EXPECT_EQ(g.resv_jobs, 0);
  EXPECT_NEAR(g.tokens, 14.0, 1e-9);
}

TEST(ProbeGate, ConcurrentPlannersShareTheBucketAndUnusedProbesReturn)
{
  nr_probe_gate_t g;
  nr_probe_gate_init(&g, 0.5, 4.0);
  int ra = 0, rb = 0;
  EXPECT_EQ(nr_probe_gate_reserve(&g, 1, 3, 24, &ra), 3) << "A: 4 tokens, 3 jobs of ring headroom";
  EXPECT_EQ(nr_probe_gate_reserve(&g, 1, 3, 24, &rb), 0) << "B: the headroom is promised to A";
  EXPECT_TRUE(nr_probe_gate_take(&g, 1, false, &ra));
  nr_probe_gate_release(&g, &ra); // A planned 1 of its 3
  EXPECT_EQ(ra, 0);
  EXPECT_EQ(g.resv_jobs, 0);
  EXPECT_NEAR(g.tokens, 3.0, 1e-9);
  EXPECT_EQ(nr_probe_gate_reserve(&g, 1, 2, 24, &rb), 2) << "B gets what A returned (ring headroom now 2)";
  nr_probe_gate_release(&g, &rb);
  EXPECT_EQ(nr_probe_gate_reserve(&g, 3, -1, 24, &rb), 4) << "no ring: tokens refill at 0.5/slot up to the burst";
  nr_probe_gate_release(&g, &rb);
  EXPECT_NEAR(g.tokens, 4.0, 1e-9);
}


/* ---- PARK (nr_signal_evidence.h): decode effort follows the DM-RS evidence of the RNTI against the cell's own noise floor ---- */
namespace {
struct ParkSim {
  std::mt19937 rng{12345};
  nse_ref_t R;
  double noise_mu = -7.0, noise_sd = 2.5, pass_mu = 3.0, pass_sd = 4.0; // real signals of the cell: 3 +- 4 dB (weakest tenth: -2.1 dB)
  double g(double mu, double sd) { return std::normal_distribution<double>(mu, sd)(rng); }
  ParkSim()
  {
    memset(&R, 0, sizeof R);
    for (int i = 0; i < 6000; i++) nse_ref_add(&R, (float)g(noise_mu, noise_sd)); // decodes of unsolved RNTIs: wrong hypotheses
    for (int i = 0; i < 60; i++) nse_ref_note_pass(&R, (float)g(pass_mu, pass_sd)); // TB passes of solved RNTIs
  }
  /* one grant of an RNTI decoded under K = 14 hypothesis classes; class 0 carries the RNTI at sig_mu (< -90: no class sees it) */
  nse_event_t grant(nse_rnti_t *s, uint64_t abs, double sig_mu, int K = 14, bool cover = true)
  {
    nse_event_t last = NSE_NONE;
    for (int h = 0; h < K; h++) {
      const double v = (h == 0 && sig_mu > -90.0) ? g(sig_mu, noise_sd) : g(noise_mu, noise_sd);
      const nse_event_t e = nse_note(s, &R, abs, (uint32_t)h, (float)v, false, false, cover);
      if (e != NSE_NONE) last = e;
    }
    return last;
  }
};
} // namespace

TEST(Park, NoiseFloorRntiIsParkedWithinBoundedGrants)
{
  int parked = 0, worst = 0;
  double mean = 0;
  const int trials = 200;
  for (int t = 0; t < trials; t++) {
    ParkSim sim;
    sim.rng.seed(1000 + t);
    nse_rnti_t s;
    memset(&s, 0, sizeof s);
    int at = -1;
    for (int k = 1; k <= 300 && at < 0; k++) {
      sim.grant(&s, (uint64_t)k, -100.0);
      if (s.parked) at = k;
    }
    if (at > 0) { parked++; mean += at; worst = std::max(worst, at); }
  }
  printf("[PARK] noise-floor RNTI: parked %d/%d, mean %.1f grants, worst %d\n", parked, trials, mean / std::max(parked, 1), worst);
  EXPECT_GE(parked, trials * 99 / 100);
  EXPECT_LE(mean / std::max(parked, 1), 40.0);
}

TEST(Park, WeakButRealRntiIsNotParkedBeyondTheErrorRate)
{
  // a signal on one of 14 classes: the weakest tenth of the cell's real signals, and a typical one; 500 grants each
  const double lvl[2] = {3.0 - 1.2816 * 4.0, 3.0};
  for (int c = 0; c < 2; c++) {
    int fp = 0;
    const int trials = 300;
    for (int t = 0; t < trials; t++) {
      ParkSim sim;
      sim.rng.seed(5000 + t);
      nse_rnti_t s;
      memset(&s, 0, sizeof s);
      bool ever = false;
      for (int k = 1; k <= 500; k++) { sim.grant(&s, (uint64_t)k, lvl[c]); ever |= s.parked; }
      fp += ever;
    }
    printf("[PARK] real RNTI at %.1f dB: ever parked in %d/%d (%.1f %%), design bound beta = %.0f %%\n", lvl[c], fp, trials, 100.0 * fp / trials,
           100.0 * NSE_BETA);
    EXPECT_LE(fp, (int)(trials * 3 * NSE_BETA)) << "level " << lvl[c];
  }
}

TEST(Park, ParkedRntiGainingSignalIsUnparkedWithinTheBound)
{
  int unparked = 0, worst = 0;
  double mean = 0;
  const int trials = 100;
  for (int t = 0; t < trials; t++) {
    ParkSim sim;
    sim.rng.seed(9000 + t);
    nse_rnti_t s;
    memset(&s, 0, sizeof s);
    uint64_t abs = 0;
    for (int k = 0; k < 300 && !s.parked; k++) sim.grant(&s, ++abs, -100.0);
    ASSERT_TRUE(s.parked);
    // the UE turns toward us: the weakest tenth level; only the re-tested grants are decoded
    int at = -1;
    for (uint32_t gno = 1; gno <= 4000 && at < 0; gno++) {
      if (!nse_due(&s, gno)) continue;
      sim.grant(&s, ++abs, 3.0 - 1.2816 * 4.0);
      if (!s.parked) at = (int)gno;
    }
    if (at > 0) { unparked++; mean += at; worst = std::max(worst, at); }
  }
  printf("[PARK] un-park after the UE gains signal: %d/%d, mean %.0f grants, worst %d (thinning cap 1 in %d)\n", unparked, trials,
         mean / std::max(unparked, 1), worst, 1 << NSE_THIN_MAX_SHIFT);
  EXPECT_EQ(unparked, trials);
  EXPECT_LE(worst, 2000);
}

TEST(Park, NoDecisionWithoutMeasuredReferenceShapeOrSearchCoverage)
{
  ParkSim sim;
  nse_rnti_t s;
  memset(&s, 0, sizeof s);
  for (int k = 1; k <= 400; k++) sim.grant(&s, (uint64_t)k, -100.0, 14, false); // the search has not covered the hypotheses yet
  EXPECT_FALSE(s.parked);
  nse_ref_t small;
  memset(&small, 0, sizeof small); // below the reference sample size, no TB passes seen
  nse_rnti_t s2;
  memset(&s2, 0, sizeof s2);
  for (int k = 1; k <= 100; k++)
    for (int h = 0; h < 14; h++) nse_note(&s2, &small, (uint64_t)k, (uint32_t)h, -7.f, false, false, true);
  EXPECT_FALSE(s2.parked);
  ParkSim sim2;
  sim2.R.pr_n = 0; // pool ready but no real signal seen in this cell: the alternative is unknown
  nse_rnti_t s3;
  memset(&s3, 0, sizeof s3);
  for (int k = 1; k <= 400; k++) sim2.grant(&s3, (uint64_t)k, -100.0);
  EXPECT_FALSE(s3.parked);
}

TEST(Park, ASolvedRntisDecodesNeverJoinTheNoiseReference)
{
  ParkSim sim;
  const uint64_t n0 = sim.R.n;
  nse_rnti_t s;
  memset(&s, 0, sizeof s);
  for (int k = 1; k <= 50; k++) nse_note(&s, &sim.R, (uint64_t)k, 0, 12.f, true, true, true);
  EXPECT_EQ(sim.R.n, n0);
  EXPECT_FALSE(s.parked);
}

TEST(Park, GateMarginIsTightenedOnlyByTheCellsOwnDecodes)
{
  nse_gate_t G;
  memset(&G, 0, sizeof G);
  EXPECT_DOUBLE_EQ(nse_gate_margin(&G, 5.0), 5.0) << "no data: the design margin";
  std::mt19937 rng(7);
  for (int i = 0; i < 20000; i++) { // margins -5..15 dB, CRC passes only from margin 0 up (probability 0.5)
    const double m = -5.0 + 20.0 * (rng() % 100000) / 100000.0;
    nse_gate_note(&G, m, m >= 0.0 && (rng() & 1));
  }
  const double M = nse_gate_margin(&G, 5.0);
  printf("[PARK] cell-measured gate margin %.1f dB (design 5.0)\n", M);
  EXPECT_LE(M, 0.0 + 1e-9);
  EXPECT_GE(M, -1.0) << "passes begin at margin 0: the safe region ends there";
  nse_gate_t H;
  memset(&H, 0, sizeof H);
  for (int i = 0; i < 20000; i++) nse_gate_note(&H, -4.5 + 0.5 * (i % 3), (i % 50) == 0); // passes at margin -4.5: nothing to gate
  EXPECT_DOUBLE_EQ(nse_gate_margin(&H, 5.0), 5.0);
}

TEST(Park, SolverParksANoiseRntiThinsItsSearchAndKeepsRetesting)
{
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(77);
  Cell cell; // one RNTI solves, and its TB passes show the cell what a real signal looks like
  for (int i = 0; i < 100; i++) (i >= 10 && i < 15 ? cell.truth : cell.wrong).push_back((uint16_t)i);
  ASSERT_GT(RunCell(cell, 0x8f5a, -1, 400, rng), 0);
  auto noise = [&] { return (float)std::normal_distribution<double>(-7.0, 2.5)(rng); };
  auto real = [&] { return (float)std::normal_distribution<double>(3.0, 4.0)(rng); };
  const uint16_t layout = 10;
  nr_pdsch_solver_reading_t rd0 = {&layout, 1, 16, 1, 1, 2};
  // background RNTIs fill the cell's noise reference (never solved: wrong hypotheses)
  for (uint16_t r = 0x2001; r < 0x2001 + 30; r++) {
    nr_dci_bits_t p = {{42, 0, 0}};
    nr_pdsch_solver_set_now(10);
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(r, p, 49, 2, 2, &rd0, 1, 8, v);
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
    for (int k = 0; k < 120; k++) nr_pdsch_solver_note_dmrs(r, 10, (uint32_t)(k % 14), noise(), false, true, 2, 0, false);
  }
  for (int k = 0; k < 20; k++) nr_pdsch_solver_note_dmrs(0x8f5a, 5, 0, real(), true, false, 2, 0, true);
  const uint16_t victim = 0x9f52;
  const uint16_t lay2 = 11;
  nr_pdsch_solver_reading_t rd = {&lay2, 1, 16, 1, 1, 2};
  int planned_active = 0, planned_parked = 0, parked_grants = 0;
  for (int g = 1; g <= 4000; g++) {
    nr_dci_bits_t p = {{(uint64_t)g, 0, 0}};
    nr_pdsch_solver_set_now((uint64_t)(100 + g));
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const bool was_parked = nr_pdsch_solver_parked(victim);
    const int n = nr_pdsch_solver_plan(victim, p, 49, 2, 2, &rd, 1, 8, v);
    (was_parked ? planned_parked : planned_active) += n > 0;
    parked_grants += was_parked;
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
    for (int h = 0; h < n * 2; h++) nr_pdsch_solver_note_dmrs(victim, (uint64_t)(100 + g), (uint32_t)(h % 14), noise(), false, true, 2, 0, false);
  }
  printf("[PARK] solver: active grants planned %d, parked grants %d of which re-tested %d\n", planned_active, parked_grants, planned_parked);
  EXPECT_TRUE(nr_pdsch_solver_parked(victim));
  EXPECT_GT(parked_grants, 1500);
  EXPECT_GT(planned_parked, 0) << "never to zero: periodic re-test";
  EXPECT_LT(planned_parked * 8, parked_grants) << "thinned";
  EXPECT_FALSE(nr_pdsch_solver_parked(0x8f5a)) << "a solved RNTI with signal is never parked";
}

/* ===== A1 / A2 / A3: time to the first CRC of a NEW RNTI (SEEDBACKOFF, SEEDTUPLES, pool identities) =====
 * [SIMULATED] cells: RRC templates (layout set, add_pos, MCS table, S per TDA index, DM-RS scrambling) and RNTIs on them; a probe passes
 * the CRC iff it carries its RNTI's template on all of: reading, S, add_pos, table (Qm, R) and scrambling. */
namespace {
struct Tmpl {
  std::vector<uint16_t> truth;
  int add_pos, table;
  int S_true[2];
};
struct Sim {
  std::vector<uint16_t> wrong;
  std::vector<Tmpl> t;
  int key = 7;
  std::vector<uint16_t> alt;   // the CORESET / CSI-RS ids the caller offers every RNTI (non-own list)
};
struct SimOut { int first_crc = -1, solved = -1, probes = 0, probes_after_200 = 0; };
// Runs one RNTI of template `ti` whose DM-RS scrambling is `scr` for up to max_grants grants. own_after >= 0: the blind scan delivers
// the RNTI's own identity `scr` from that grant on (own mode); pool: the cell's pool ids offered to the planner while the RNTI has none.
SimOut SimRnti(const Sim &s, uint16_t rnti, int ti, int32_t scr, int own_after, const std::vector<int32_t> &pool, int max_grants,
               std::mt19937 &rng, float fit = -1.f, bool full_after = false)
{
  std::vector<nr_pdsch_solver_reading_t> rd;
  rd.push_back({s.wrong.data(), (int)s.wrong.size(), 16, 1, 1, 0, fit});
  for (auto &t : s.t) rd.push_back({t.truth.data(), (int)t.truth.size(), 16, 1, 1, 0, fit});
  SimOut o;
  static uint64_t now = 1000;
  for (int g = 0; g < max_grants; g++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    const int tda = (int)(p.w[0] & 1);
    const uint8_t mcs = (g % 3 == 0) ? 2 : 0;
    const bool own = own_after >= 0 && g >= own_after;
    const int32_t one[1] = {scr};
    if (own) nr_pdsch_solver_set_alt_scr32(s.key, one, 1, true, true);
    else nr_pdsch_solver_set_alt_scr(s.key, s.alt.data(), (int)s.alt.size(), false);
    if (!own) nr_pdsch_solver_set_pool_ids(pool.data(), (int)pool.size());
    nr_pdsch_solver_set_now(now += 7);
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, mcs, 2, rd.data(), (int)rd.size(), 8, v);
    const bool solved_before = nr_pdsch_solver_rep(rnti) >= 0;
    for (int i = 0; i < n; i++) {
      int qa, ra, qb, rb;
      nr_pdsch_solver_mcs_qr(v[i].table, mcs, &qa, &ra);
      nr_pdsch_solver_mcs_qr(s.t[ti].table, mcs, &qb, &rb);
      const bool ok = v[i].reading == 1 + ti && v[i].S == s.t[ti].S_true[tda] && v[i].add_pos == s.t[ti].add_pos && qa == qb && ra == rb
                      && v[i].scr == scr;
      if (v[i].probe) { o.probes++; if (o.solved < 0 && g >= 200) o.probes_after_200++; }
      if (ok && v[i].probe && o.first_crc < 0) o.first_crc = g + 1;
      nr_pdsch_solver_feedback(v[i].token, ok && (v[i].probe || full_after || solved_before), true);
    }
    if (o.solved < 0 && nr_pdsch_solver_rep(rnti) >= 0) { o.solved = g + 1; if (!full_after) break; }
  }
  return o;
}
// brings `n` RNTIs of template ti to solved with >= 20 CRC-proven full TBs each (they seed the cell); scr(k) = the k-th RNTI's scrambling
void SolvePopulation(const Sim &s, uint16_t base, int ti, int n, int32_t (*scr)(int), std::mt19937 &rng)
{
  for (int k = 0; k < n; k++) {
    const SimOut o = SimRnti(s, (uint16_t)(base + k), ti, scr(k), 0, {}, 60, rng, -1.f, true);
    ASSERT_GT(o.solved, 0) << "population RNTI " << k;
  }
}
int32_t scr_415(int) { return 415; }
int32_t scr_249(int) { return 249; }
int32_t scr_ue(int k) { return (101 + 37 * k) | (424 << 16); }  // 923-like: DM-RS id per UE, data id = PCI + 1 form
Sim MakeSim()
{
  Sim s;
  for (int i = 30; i < 130; i++) s.wrong.push_back((uint16_t)i);
  Tmpl a, b;
  for (int i = 10; i < 15; i++) a.truth.push_back((uint16_t)i);
  for (int i = 20; i < 25; i++) b.truth.push_back((uint16_t)i);
  a.add_pos = 1; a.table = 1; a.S_true[0] = 1; a.S_true[1] = 0;
  b.add_pos = 2; b.table = 0; b.S_true[0] = 0; b.S_true[1] = 2;
  s.t = {a, b};
  return s;
}
void Stats(const char *tag, std::vector<int> v, int never)
{
  std::sort(v.begin(), v.end());
  printf("[A123] %s: first CRC grant p50 %d p90 %d max %d, never %d of %zu\n", tag, v.empty() ? -1 : v[v.size() / 2],
         v.empty() ? -1 : v[v.size() * 9 / 10], v.empty() ? -1 : v.back(), never, v.size() + never);
}
} // namespace

// 415-like: two RRC templates with two cell-wide identities (6 RNTIs on T0 / scr 415, 4 on T1 / scr 249). New RNTIs of either template.
TEST(SeedNewRnti, TwoTemplateCellFindsBothTemplatesFromTheFirstGrants)
{
  Sim s = MakeSim();
  s.alt = {415, 249};
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_hq_fn(nullptr, nullptr); // an earlier test may have left the HARQ verifier
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(501);
  SolvePopulation(s, 0x1000, 0, 6, scr_415, rng);
  SolvePopulation(s, 0x1100, 1, 4, scr_249, rng);
  std::vector<int> f[2];
  int never[2] = {0, 0};
  for (int ti = 0; ti < 2; ti++)
    for (int k = 0; k < 20; k++) {
      const SimOut o = SimRnti(s, (uint16_t)(0x2000 + 32 * ti + k), ti, ti ? 249 : 415, -1, {}, 400, rng);
      if (o.first_crc > 0) f[ti].push_back(o.first_crc); else never[ti]++;
    }
  Stats("415-like T0 new RNTIs", f[0], never[0]);
  Stats("415-like T1 new RNTIs", f[1], never[1]);
  EXPECT_EQ(never[0] + never[1], 0);
  std::sort(f[1].begin(), f[1].end());
  ASSERT_FALSE(f[1].empty());
  EXPECT_LE(f[1][f[1].size() * 9 / 10], 10) << "the minority template (4 of 10 RNTIs) is seeded within a few grants";
  nr_pdsch_solver_set_alt_scr(-1, nullptr, 0, false);
  nr_pdsch_solver_reset();
}

// 403-like: one template, one cell-wide identity. A pool of other ids must not slow it down (cell-wide identity: the pool is not used).
TEST(SeedNewRnti, CellWideIdentityIgnoresThePool)
{
  Sim s = MakeSim();
  s.alt = {415, 249};
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_hq_fn(nullptr, nullptr); // an earlier test may have left the HARQ verifier
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(502);
  SolvePopulation(s, 0x1000, 0, 5, scr_415, rng);
  std::vector<int> a, b;
  const std::vector<int32_t> pool = {5000, 6000, 7000, 8000, 9000};
  for (int k = 0; k < 20; k++) {
    a.push_back(SimRnti(s, (uint16_t)(0x2000 + k), 0, 415, -1, {}, 400, rng).first_crc);
    b.push_back(SimRnti(s, (uint16_t)(0x2100 + k), 0, 415, -1, pool, 400, rng).first_crc);
  }
  Stats("403-like without pool", a, 0);
  Stats("403-like with pool", b, 0);
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  EXPECT_GT(a[0], 0);
  EXPECT_LE(b[b.size() / 2], 2);
  EXPECT_LE(b[b.size() * 9 / 10], a[a.size() * 9 / 10] + 1);
  nr_pdsch_solver_reset();
}

// 923-like: per-UE DM-RS identity. The pool offers the identities the cell has shown; the scan delivers the RNTI's own after 20 grants.
TEST(SeedNewRnti, PerUeIdentityIsFoundInThePoolBeforeTheScanAnswers)
{
  Sim s = MakeSim();
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_hq_fn(nullptr, nullptr); // an earlier test may have left the HARQ verifier
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(503);
  SolvePopulation(s, 0x1000, 0, 6, scr_ue, rng);
  std::vector<int32_t> pool;
  for (int k = 0; k < 6; k++) pool.push_back(101 + 37 * k);
  pool.push_back(4000); pool.push_back(4100);
  std::vector<int> in_pool, with_scan_only;
  for (int k = 0; k < 6; k++) { // a returning UE: new RNTI, an identity the cell has shown (the k-th of the pool)
    in_pool.push_back(SimRnti(s, (uint16_t)(0x2000 + k), 0, scr_ue(k), 20, pool, 400, rng).first_crc);
  }
  for (int k = 0; k < 6; k++) { // a new UE: identity not in the pool; only the scan (grant 20) can bring it
    with_scan_only.push_back(SimRnti(s, (uint16_t)(0x2100 + k), 0, (9000 + 11 * k) | (424 << 16), 20, pool, 400, rng).first_crc);
  }
  Stats("923-like id in pool", in_pool, 0);
  Stats("923-like id not in pool (scan at grant 20)", with_scan_only, 0);
  std::sort(in_pool.begin(), in_pool.end());
  std::sort(with_scan_only.begin(), with_scan_only.end());
  EXPECT_GT(in_pool[0], 0);
  EXPECT_LT(in_pool[in_pool.size() - 1], 20) << "found by the pool before the scan's first answer";
  EXPECT_GE(with_scan_only[0], 20) << "an identity nobody has shown still waits for the scan";
  EXPECT_GT(with_scan_only[0], 0);
  nr_pdsch_solver_reset();
}

// A1: once the coverage search is thinned (>= 200 probes, no DM-RS support) the seeded probe still runs every grant, at most one probe.
TEST(SeedNewRnti, BackoffThinsOnlyTheCoverageSearch)
{
  Sim s = MakeSim();
  s.alt = {415};
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_hq_fn(nullptr, nullptr); // an earlier test may have left the HARQ verifier
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(504);
  SolvePopulation(s, 0x1000, 0, 4, scr_415, rng);
  // a RNTI that spends 200+ probes on a layout nobody reads (no DM-RS support: fit < 0), then its true template appears
  Sim noise = s;
  noise.t = {s.t[0]};
  const int32_t kWrongScr = 77;
  const SimOut burn = SimRnti(noise, 0x3000, 0, kWrongScr, -1, {}, 600, rng); // never passes (wrong scr): burns probes
  EXPECT_EQ(burn.first_crc, -1);
  EXPECT_GE(burn.probes, 200);
  int with_probe = 0, over_one = 0, total = 0;
  std::vector<nr_pdsch_solver_reading_t> rd = {{s.wrong.data(), (int)s.wrong.size(), 16, 1, 1, 0, -1.f},
                                               {s.t[0].truth.data(), (int)s.t[0].truth.size(), 16, 1, 1, 0, -1.f}};
  nr_pdsch_solver_set_alt_scr(s.key, s.alt.data(), 1, false);
  for (int g = 0; g < 800; g++) {
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(0x3000, p, 49, 0, 2, rd.data(), 2, 8, v);
    int seeded = 0;
    for (int i = 0; i < n; i++) {
      seeded += v[i].reading == 1 && v[i].scr == 415;
      nr_pdsch_solver_feedback(v[i].token, false, true);
    }
    total += n;
    with_probe += n > 0;
    over_one += n > 1 && g % 8 != 0;
  }
  printf("[A123] backoff window: grants with a probe %d of 800, probes %d, thinned grants with >1 probe %d\n", with_probe, total, over_one);
  EXPECT_EQ(with_probe, 800) << "the seeded probe runs on every grant";
  EXPECT_LE(over_one, 800 / 8) << "more than one probe only on the 1 grant in 8 that keeps the coverage search";
  nr_pdsch_solver_reset();
}

// Bounded cost: solved RNTIs plan the same probes whatever the seeds (only unsolved RNTIs seed), and a parked noise RNTI stays thinned.
TEST(SeedNewRnti, SolvedRntisPlanNoSeedProbes)
{
  Sim s = MakeSim();
  s.alt = {415, 249};
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_hq_fn(nullptr, nullptr); // an earlier test may have left the HARQ verifier
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(505);
  SolvePopulation(s, 0x1000, 0, 6, scr_415, rng);
  SolvePopulation(s, 0x1100, 1, 4, scr_249, rng);
  // steady state: 10 solved RNTIs, 300 grants each, probes per grant (refinement and phase probes only)
  std::vector<nr_pdsch_solver_reading_t> rd = {{s.t[0].truth.data(), (int)s.t[0].truth.size(), 16, 1, 1, 0, -1.f}};
  int probes = 0, grants = 0;
  for (int g = 0; g < 300; g++)
    for (int k = 0; k < 6; k++) {
      nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
      nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
      nr_pdsch_solver_set_alt_scr(s.key, s.alt.data(), 2, false);
      const int n = nr_pdsch_solver_plan((uint16_t)(0x1000 + k), p, 49, 0, 2, rd.data(), 1, 8, v);
      for (int i = 0; i < n; i++) {
        probes += v[i].probe;
        nr_pdsch_solver_feedback(v[i].token, true, true);
      }
      grants++;
    }
  printf("[A123] solved RNTIs: %d probes over %d grants\n", probes, grants);
  EXPECT_EQ(probes, 0) << "a solved RNTI with one reading plans full decodes only";
  nr_pdsch_solver_reset();
}

// The PARK design composes: a parked noise RNTI stays thinned even when the cell has seed tuples (its re-test grants take the backoff).
TEST(SeedNewRnti, ParkedNoiseRntiStaysThinnedWithSeedsAvailable)
{
  Sim s = MakeSim();
  s.alt = {415, 249};
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_hq_fn(nullptr, nullptr); // an earlier test may have left the HARQ verifier
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(506);
  SolvePopulation(s, 0x1000, 0, 6, scr_415, rng);
  auto noise = [&] { return (float)std::normal_distribution<double>(-7.0, 2.5)(rng); };
  auto real = [&] { return (float)std::normal_distribution<double>(3.0, 4.0)(rng); };
  const uint16_t layout = 10;
  nr_pdsch_solver_reading_t rd0 = {&layout, 1, 16, 1, 1, 2};
  for (uint16_t r = 0x2001; r < 0x2001 + 30; r++) {
    nr_dci_bits_t p = {{42, 0, 0}};
    nr_pdsch_solver_set_now(10);
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(r, p, 49, 2, 2, &rd0, 1, 8, v);
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
    for (int k = 0; k < 120; k++) nr_pdsch_solver_note_dmrs(r, 10, (uint32_t)(k % 14), noise(), false, true, 2, 0, false);
  }
  for (int k = 0; k < 20; k++) nr_pdsch_solver_note_dmrs(0x1000, 5, 0, real(), true, false, 2, 0, true);
  const uint16_t victim = 0x9f52;
  std::vector<nr_pdsch_solver_reading_t> rd = {{s.wrong.data(), (int)s.wrong.size(), 16, 1, 1, 2, -1.f},
                                               {s.t[0].truth.data(), (int)s.t[0].truth.size(), 16, 1, 1, 2, -1.f}};
  int probes_active = 0, probes_parked = 0, parked_grants = 0, planned_parked = 0, active_grants = 0;
  for (int g = 1; g <= 4000; g++) {
    nr_dci_bits_t p = {{(uint64_t)g, 0, 0}};
    nr_pdsch_solver_set_now((uint64_t)(100 + g));
    nr_pdsch_solver_set_alt_scr(s.key, s.alt.data(), 2, false);
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const bool was_parked = nr_pdsch_solver_parked(victim);
    const int n = nr_pdsch_solver_plan(victim, p, 49, 2, 2, rd.data(), 2, 8, v);
    (was_parked ? probes_parked : probes_active) += n;
    parked_grants += was_parked;
    active_grants += !was_parked;
    planned_parked += was_parked && n > 0;
    for (int i = 0; i < n; i++) nr_pdsch_solver_feedback(v[i].token, false, true);
    for (int h = 0; h < n * 2; h++) nr_pdsch_solver_note_dmrs(victim, (uint64_t)(100 + g), (uint32_t)(h % 14), noise(), false, true, 2, 0, false);
  }
  printf("[A123] parked noise RNTI with seeds: active grants %d probes %d; parked grants %d probes %d on %d grants\n", active_grants,
         probes_active, parked_grants, probes_parked, planned_parked);
  EXPECT_GT(parked_grants, 1500);
  EXPECT_LT(planned_parked * 8, parked_grants) << "thinned";
  EXPECT_LE(probes_parked, 8 * planned_parked);
  nr_pdsch_solver_reset();
}

/* FIRSTSLOT [SIMULATED]: the start symbol of a grant is fixed by the slot phase (TDD pattern), not by the DCI's tda value. The population's tda-keyed
 * S maps are then whatever phases they solved at; a new RNTI's first seeded probe must still carry the phase's S. Reading 1 (truth) has a twin
 * reading 2 holding the other half of the rep's ambiguity set: the twin is the true one for this cell (rep unrefined), it must be seeded too. */
namespace {
int PhaseS(uint64_t abs) { return ((abs % 20) % 3 == 0) ? 1 : 0; }
// one grant of RNTI `rnti`; returns true when a probe on the true reading/S/scr passed in this grant
bool PhaseGrant(const Sim &s, const std::vector<nr_pdsch_solver_reading_t> &rd, int true_rd, uint16_t rnti, int32_t scr, std::mt19937 &rng,
                uint64_t &now, bool full_after, bool *solved_out)
{
  nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
  const uint8_t mcs = 0;
  nr_pdsch_solver_set_alt_scr(s.key, s.alt.data(), (int)s.alt.size(), false);
  nr_pdsch_solver_set_now(now += 7);
  nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
  const int n = nr_pdsch_solver_plan(rnti, p, 49, mcs, 2, rd.data(), (int)rd.size(), 8, v);
  const bool solved_before = nr_pdsch_solver_rep(rnti) >= 0;
  bool hit = false;
  for (int i = 0; i < n; i++) {
    int qa, ra, qb, rb;
    nr_pdsch_solver_mcs_qr(v[i].table, mcs, &qa, &ra);
    nr_pdsch_solver_mcs_qr(s.t[0].table, mcs, &qb, &rb);
    const bool ok = v[i].reading == true_rd && v[i].S == PhaseS(now) && v[i].add_pos == s.t[0].add_pos && qa == qb && ra == rb && v[i].scr == scr;
    hit |= ok && v[i].probe;
    nr_pdsch_solver_feedback(v[i].token, ok && (v[i].probe || full_after || solved_before), true);
  }
  *solved_out = nr_pdsch_solver_rep(rnti) >= 0;
  return hit;
}
} // namespace

TEST(SeedNewRnti, FirstSeededProbeCarriesThePhaseStartSymbol)
{
  Sim s = MakeSim();
  s.alt = {415};
  nr_pdsch_solver_reset();
  nr_pdsch_solver_set_hq_fn(nullptr, nullptr);
  nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
  std::mt19937 rng(777);
  std::vector<nr_pdsch_solver_reading_t> rd;
  rd.push_back({s.wrong.data(), (int)s.wrong.size(), 16, 1, 1, 0, -1.f});
  rd.push_back({s.t[0].truth.data(), (int)s.t[0].truth.size(), 16, 1, 1, 0, -1.f});
  uint64_t now = 1000;
  for (int k = 0; k < 8; k++) { // the population: solved, with >= 20 full decodes each
    bool solved = false;
    int g = 0;
    for (; g < 400 && !(solved && g > 60); g++) PhaseGrant(s, rd, 1, (uint16_t)(0x1000 + k), 415, rng, now, true, &solved);
    ASSERT_TRUE(solved) << "population RNTI " << k;
  }
  int first_grant_pass = 0, new_rntis = 40;
  for (int k = 0; k < new_rntis; k++) {
    bool solved = false;
    first_grant_pass += PhaseGrant(s, rd, 1, (uint16_t)(0x2000 + k), 415, rng, now, false, &solved);
  }
  printf("[FIRSTSLOT] new RNTIs passing in their very first grant: %d of %d\n", first_grant_pass, new_rntis);
  EXPECT_GE(first_grant_pass, new_rntis * 3 / 4) << "the first-slot seed must carry the slot phase's start symbol";
  nr_pdsch_solver_reset();
}
