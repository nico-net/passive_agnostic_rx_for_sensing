// mpci A5: two-cell interleave tests of the PDSCH solver, the async-recovery lane and the per-cell decode evidence / DL HARQ store
// (nr_pdsch_solver.c, nr_pdsch_recovery.c, nr_pdsch_passive_decode.c, converted to NR_CELL_STATIC).
// Method (same as A3): workload X on cell A and a DIFFERENT workload Y on cell B. Solo reference per cell first; then
//   (a) alternating step by step on ONE thread that re-binds with nr_cell_bind between the calls,
//   (b) alternating step by step on TWO threads, each bound to its own cell (a baton orders the steps deterministically).
// Each cell's trace must equal its solo trace. The same RNTI / HARQ id is used in both cells on purpose.
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include "nr_pdsch_solver.h"
#include "nr_pdsch_recovery.h"
#include "nr_pdsch_cell_prealloc.h"
/* nr_pdsch_passive_decode.h pulls the whole PHY: only the evidence entry points used here */
uint16_t nr_pdsch_passive_data_id_current(uint16_t rnti, uint16_t pci, int dmrs_id, bool advance_ok);
void nr_pdsch_passive_data_id_feed(uint16_t rnti, bool tb_crc_ok);
void nr_pdsch_passive_crc_note(uint16_t rnti, bool dedicated, bool crc_ok);
uint32_t nr_pdsch_passive_rnti_ded_fails(uint16_t rnti);
bool nr_pdsch_passive_link_healthy(uint16_t rnti);
}

namespace {

constexpr int kCellA = 0, kCellB = 1;

// A workload runs steps 0..n-1 on the cell it is bound to and appends to its own trace; step n-1 is the final state read-out.
struct Workload {
  virtual ~Workload() = default;
  virtual int steps() const = 0;
  virtual void step(int i) = 0;
  std::string trace;
  void log(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    trace += b;
    trace += '\n';
  }
};

using Factory = std::function<std::unique_ptr<Workload>()>;

std::string solo(const Factory &mk, int cell)
{
  nr_cell_bind(cell);
  auto w = mk();
  for (int i = 0; i < w->steps(); i++) w->step(i);
  nr_cell_bind(0);
  return w->trace;
}

// (a) one thread, re-binding between calls
void run_one_thread(Workload &a, Workload &b, int ca, int cb)
{
  const int n = std::max(a.steps(), b.steps());
  for (int i = 0; i < n; i++) {
    if (i < a.steps()) { nr_cell_bind(ca); a.step(i); }
    if (i < b.steps()) { nr_cell_bind(cb); b.step(i); }
  }
  nr_cell_bind(0);
}

// (b) two threads bound to their cells, strictly alternating
void run_two_threads(Workload &a, Workload &b, int ca, int cb)
{
  std::mutex m;
  std::condition_variable cv;
  int turn = 0; // 0: A, 1: B
  auto body = [&](Workload &w, int cell, int me) {
    nr_cell_bind(cell);
    for (int i = 0; i < w.steps(); i++) {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [&] { return turn == me; });
      w.step(i);
      turn = 1 - me;
      cv.notify_all();
    }
    // the cell with fewer steps hands the baton over for good
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return turn == me; });
    turn = 1 - me;
    cv.notify_all();
  };
  // A and B have the same number of steps in all workloads below, so the strict alternation never starves
  std::thread ta(body, std::ref(a), ca, 0), tb(body, std::ref(b), cb, 1);
  ta.join();
  tb.join();
}

void expect_isolated(const Factory &mkA, const Factory &mkB)
{
  // every phase uses fresh cells (2/3 solo, 4/5 one re-binding thread, 6/7 two bound threads): the evidence has no reset
  const std::string soloA = solo(mkA, 2), soloB = solo(mkB, 3);
  ASSERT_FALSE(soloA.empty());
  ASSERT_NE(soloA, soloB) << "the two workloads must differ, otherwise the test proves nothing";
  {
    auto a = mkA(), b = mkB();
    run_one_thread(*a, *b, 4, 5);
    EXPECT_EQ(a->trace, soloA) << "cell A, alternating on one re-binding thread";
    EXPECT_EQ(b->trace, soloB) << "cell B, alternating on one re-binding thread";
  }
  {
    auto a = mkA(), b = mkB();
    run_two_threads(*a, *b, 6, 7);
    EXPECT_EQ(a->trace, soloA) << "cell A, two bound threads";
    EXPECT_EQ(b->trace, soloB) << "cell B, two bound threads";
  }
}

// ---------------------------------------------------------------- solver
int tda_fn(void *, uint16_t, nr_dci_bits_t p) { return (int)(p.w[0] & 1); }

struct SolverWl : Workload {
  std::vector<uint16_t> wrong, truth;
  int S_true[2];
  int add_pos, table, seed;
  uint16_t rnti;
  std::mt19937 rng;
  SolverWl(int idLo, int idHi, int s0, int s1, int seed_, uint16_t rnti_) : S_true{s0, s1}, add_pos(1), table(1), seed(seed_), rnti(rnti_), rng(seed_)
  {
    for (int i = 0; i < 100; i++) (i >= idLo && i < idHi ? truth : wrong).push_back((uint16_t)i);
  }
  int steps() const override { return 121; } // 120 grants + final read-out
  void step(int g) override
  {
    if (g == 0) {
      nr_pdsch_solver_reset();
      nr_pdsch_solver_set_tda_fn(tda_fn, nullptr);
    }
    if (g == 120) {
      nr_pdsch_solver_stats_t st;
      nr_pdsch_solver_stats(&st);
      log("rep=%d planned=%lu probes=%lu fulls=%lu probe_ok=%lu full_ok=%lu cand=%lu solved=%lu dropped=%lu", nr_pdsch_solver_rep(rnti), (unsigned long)st.planned,
          (unsigned long)st.probes, (unsigned long)st.fulls, (unsigned long)st.probe_ok, (unsigned long)st.full_ok, (unsigned long)st.candidates,
          (unsigned long)st.solved, (unsigned long)st.dropped);
      return;
    }
    nr_pdsch_solver_reading_t rd[2] = {{wrong.data(), (int)wrong.size(), 16}, {truth.data(), (int)truth.size(), 16}};
    nr_dci_bits_t p = {{(uint64_t)rng(), 0, 0}};
    const int tda = (int)(p.w[0] & 1);
    const uint8_t mcs = (g % 3 == 0) ? 2 : 0;
    nr_pdsch_solver_var_t v[NR_PDSCH_SOLVER_MAX_VAR];
    const int n = nr_pdsch_solver_plan(rnti, p, 49, mcs, 2, rd, 2, 8, v);
    std::string line = "g" + std::to_string(g) + " n=" + std::to_string(n);
    for (int i = 0; i < n; i++) {
      int qa, ra, qb, rb;
      nr_pdsch_solver_mcs_qr(v[i].table, mcs, &qa, &ra);
      nr_pdsch_solver_mcs_qr(table, mcs, &qb, &rb);
      const bool ok = v[i].reading == 1 && v[i].S == S_true[tda] && v[i].add_pos == add_pos && qa == qb && ra == rb;
      line += " [r" + std::to_string(v[i].reading) + " S" + std::to_string(v[i].S) + " ap" + std::to_string(v[i].add_pos) + " t" + std::to_string(v[i].table) +
              (v[i].probe ? " P" : " F") + (ok ? " ok" : " no") + "]";
      nr_pdsch_solver_feedback(v[i].token, ok, true);
    }
    log("%s rep=%d", line.c_str(), nr_pdsch_solver_rep(rnti));
  }
};

TEST(MpciA5Solver, TwoCellsSameRntiDifferentTruthNeverMix)
{
  // same RNTI in both cells, different true layouts / start symbols
  expect_isolated([] { return std::unique_ptr<Workload>(new SolverWl(10, 15, 1, 0, 5, 0x8f5a)); },
                  [] { return std::unique_ptr<Workload>(new SolverWl(30, 35, 2, 1, 9, 0x8f5a)); });
}

TEST(MpciA5Solver, ResetOfOneCellKeepsTheOther)
{
  nr_cell_bind(kCellA);
  nr_pdsch_solver_reset();
  nr_cell_bind(kCellB);
  nr_pdsch_solver_reset();
  SolverWl a(10, 15, 1, 0, 5, 0x1111), b(30, 35, 2, 1, 9, 0x1111);
  for (int g = 1; g < 60; g++) {
    nr_cell_bind(kCellA); a.step(g);
    nr_cell_bind(kCellB); b.step(g);
  }
  nr_pdsch_solver_stats_t sb0;
  nr_cell_bind(kCellB);
  nr_pdsch_solver_stats(&sb0);
  nr_cell_bind(kCellA);
  nr_pdsch_solver_reset();
  nr_pdsch_solver_stats_t sa, sb1;
  nr_pdsch_solver_stats(&sa);
  nr_cell_bind(kCellB);
  nr_pdsch_solver_stats(&sb1);
  nr_cell_bind(0);
  EXPECT_EQ(sa.planned, 0u) << "cell A was reset";
  EXPECT_GT(sb0.planned, 0u);
  EXPECT_EQ(sb1.planned, sb0.planned) << "cell B must not be touched by cell A's reset";
  EXPECT_EQ(sb1.probes, sb0.probes);
}

// ---------------------------------------------------------------- decode evidence (data-id walk, CRC census) and DL HARQ store
struct EvidenceWl : Workload {
  uint16_t rnti, pci;
  std::vector<int> pattern; // CRC verdicts fed to the evidence of the RNTI
  EvidenceWl(uint16_t r, uint16_t p, std::vector<int> pat) : rnti(r), pci(p), pattern(std::move(pat)) {}
  int steps() const override { return (int)pattern.size() + 1; }
  void step(int i) override
  {
    if (i == (int)pattern.size()) {
      log("final ded_fails=%u healthy=%d cur=%u", nr_pdsch_passive_rnti_ded_fails(rnti), nr_pdsch_passive_link_healthy(rnti) ? 1 : 0,
          nr_pdsch_passive_data_id_current(rnti, pci, 1, false));
      return;
    }
    const bool ok = pattern[i] != 0;
    nr_pdsch_passive_crc_note(rnti, true, ok);
    const uint16_t id = nr_pdsch_passive_data_id_current(rnti, pci, 1, i >= 4); // the walk starts after 4 fails
    nr_pdsch_passive_data_id_feed(rnti, ok);
    log("i%d ok=%d id=%u fails=%u", i, ok ? 1 : 0, id, nr_pdsch_passive_rnti_ded_fails(rnti));
  }
};

TEST(MpciA5Decode, EvidenceOfTheSameRntiInTwoCellsStaysSeparate)
{
  // cell A: only failures (the walk keeps moving); cell B: a few passes then failures. Same RNTI, different PCI.
  std::vector<int> pa(60, 0), pb(60, 0);
  for (int i = 5; i < 20; i++) pb[i] = (i % 3 != 0);
  expect_isolated([=] { return std::unique_ptr<Workload>(new EvidenceWl(0x4601, 100, pa)); },
                  [=] { return std::unique_ptr<Workload>(new EvidenceWl(0x4601, 421, pb)); });
}

struct HarqWl : Workload {
  bool store; // this cell stores a failed first transmission; the other only asks
  uint8_t ndi, pid;
  HarqWl(bool s, uint8_t n, uint8_t p) : store(s), ndi(n), pid(p) {}
  int steps() const override { return 4; }
  void step(int i) override
  {
    const uint16_t rnti = 0x4601; // the same RNTI and HARQ id in both cells
    switch (i) {
      case 0: log("used0=%d", nr_pdsch_passive_harqc_test(2, 0, 0, 0, 0, 0)); break;
      case 1:
        if (store) log("store=%d", nr_pdsch_passive_harqc_test(0, rnti, pid, ndi, 0, 2000));
        else log("nostore");
        break;
      case 2: log("decide rv2 same ndi same tbs -> %d", nr_pdsch_passive_harqc_test(1, rnti, pid, ndi, 2, 2000)); break;
      case 3: log("used=%d", nr_pdsch_passive_harqc_test(2, 0, 0, 0, 0, 0)); break;
    }
  }
};

TEST(MpciA5Harq, SameRntiAndHarqIdInTwoCellsNeverCombine)
{
  // direct: cell A holds a failed first transmission; a retransmission (rv 2, same NDI, same TBS) combines in A and is a FIRST transmission in B
  nr_cell_bind(kCellA);
  ASSERT_EQ(nr_pdsch_passive_harqc_test(0, 0x4601, 3, 1, 0, 2000), 0);
  EXPECT_EQ(nr_pdsch_passive_harqc_test(1, 0x4601, 3, 1, 2, 2000), 1) << "cell A: the retransmission combines";
  nr_cell_bind(kCellB);
  EXPECT_EQ(nr_pdsch_passive_harqc_test(2, 0, 0, 0, 0, 0), 0) << "cell B's store is empty";
  EXPECT_EQ(nr_pdsch_passive_harqc_test(1, 0x4601, 3, 1, 2, 2000), 0) << "cell B: same (rnti, pid), nothing stored in this cell: first transmission, no combine";
  nr_cell_bind(0);
  // interleaved: A stores, B (same key, other NDI) asks; each cell equals its solo run
  expect_isolated([] { return std::unique_ptr<Workload>(new HarqWl(true, 1, 3)); }, [] { return std::unique_ptr<Workload>(new HarqWl(false, 1, 3)); });
}

// ---------------------------------------------------------------- recovery lane: one worker per cell, bound to its cell
std::atomic<int> g_run_cell[2][8];
std::atomic<int> g_run_n[2];
std::atomic<int> g_fb_n[2][3];
int run_hook(void *, const nr_recov_key_t *k)
{
  const int c = nr_cell_idx(); // the worker thread is bound to the cell that started it
  const int n = g_run_n[c]++;
  if (n < 8) g_run_cell[c][n] = (int)k->rnti;
  return 0;
}
void fb_hook(void *, const nr_recov_key_t *, int outcome) { g_fb_n[nr_cell_idx()][outcome]++; }
void done_hook(void *) {}

TEST(MpciA5Recovery, EachCellHasItsOwnWorkerBoundToItsCell)
{
  std::atomic<int> bad_cell{0};
  auto cell_main = [&](int cell, uint16_t rnti_base) {
    nr_cell_bind(cell);
    nr_pdsch_recovery_set_job_bytes(256);
    nr_pdsch_recovery_set_hooks(run_hook, nullptr, fb_hook, done_hook);
    nr_pdsch_recovery_set_busy_hook(nullptr);
    nr_pdsch_recovery_test_idle_wait_ns(2000000);
    if (nr_pdsch_recovery_start(8, "") != 0) { bad_cell++; return; }
    for (int j = 0; j < 4; j++) {
      void *s = nr_pdsch_recovery_acquire();
      if (!s) { bad_cell++; continue; }
      std::memset(s, 0, 8);
      nr_recov_key_t k = {(uint16_t)(rnti_base + j), 3, 1, 100u + j, 0};
      if (nr_pdsch_recovery_submit(&k, s) != 0) bad_cell++;
    }
    for (int i = 0; i < 400 && g_fb_n[cell][1] < 4; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    nr_pdsch_recovery_stop();
  };
  std::thread ta(cell_main, kCellA, 0x1000), tb(cell_main, kCellB, 0x2000);
  ta.join();
  tb.join();
  EXPECT_EQ(bad_cell.load(), 0);
  EXPECT_EQ(g_run_n[0].load(), 4) << "cell 0's jobs ran on cell 0's worker only";
  EXPECT_EQ(g_run_n[1].load(), 4) << "cell 1's jobs ran on cell 1's worker only";
  for (int j = 0; j < 4; j++) {
    EXPECT_EQ(g_run_cell[0][j].load() & 0xF000, 0x1000);
    EXPECT_EQ(g_run_cell[1][j].load() & 0xF000, 0x2000);
  }
  EXPECT_EQ(g_fb_n[0][1].load(), 4);
  EXPECT_EQ(g_fb_n[1][1].load(), 4);
}

TEST(MpciA5Recovery, StaleNoteOfOneCellDoesNotKillTheOthersRescue)
{
  nr_cell_bind(kCellA);
  nr_pdsch_recovery_note_newer(0x4601, 3, 1, 5000); // a newer slot of (0x4601, pid 3) in cell A
  nr_recov_key_t k = {0x4601, 3, 1, 4000, 0};
  EXPECT_EQ(nr_pdsch_recovery_publish(&k, nullptr, 0), 1) << "cell A: stale";
  nr_cell_bind(kCellB);
  EXPECT_EQ(nr_pdsch_recovery_publish(&k, nullptr, 0), 0) << "cell B: the same key is not stale here";
  nr_cell_bind(0);
}

// ---------------------------------------------------------------- heap objects: preallocation touches the cell's objects once
TEST(MpciA5Prealloc, PreallocIsIdempotentAndPerCell)
{
  nr_pdsch_cell_prealloc(0);
  nr_pdsch_cell_prealloc(1);
  nr_pdsch_cell_prealloc(1);
  SUCCEED();
}

} // namespace

extern "C" configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  if (s) fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  if (assert) abort(); else exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
  logInit();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
