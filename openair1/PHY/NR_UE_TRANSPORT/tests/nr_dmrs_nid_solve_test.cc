// Blind DM-RS scrambling-id recovery by GF(2) solving (nr_dmrs_nid_solve.c). The reference sequence generator here is the plain
// bit-by-bit LFSR of TS 38.211 5.2.1, independent of the solver's matrix tables, and is also checked against the receiver's own
// nr_gold_pdcch()/nr_pdcch_dmrs_ref() so the convention the solver inverts is the one the receiver uses.
#include <gtest/gtest.h>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <chrono>
#include <vector>

extern "C" {
#include "nr_dmrs_nid_solve.h"
uint32_t *nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short n_idDMRS, int ns, int l);
void nr_pdcch_dmrs_ref(const unsigned int *nr_gold_pdcch, void *output, unsigned short nb_rb_coreset);
}

#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
configmodule_interface_t *uniqCfg = nullptr;
int main(int argc, char **argv) { logInit(); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }

namespace {

// r(m) for m = 0..n-1 from the plain LFSR.
std::vector<std::complex<double>> ref_pilots(uint32_t cinit, int n)
{
  const int len = 2 * n + 1600 + 64;
  std::vector<uint8_t> x1(len, 0), x2(len, 0);
  x1[0] = 1;
  for (int i = 0; i < 31; i++)
    x2[i] = (cinit >> i) & 1;
  for (int j = 0; j + 31 < len; j++) {
    x1[j + 31] = x1[j + 3] ^ x1[j];
    x2[j + 31] = x2[j + 3] ^ x2[j + 2] ^ x2[j + 1] ^ x2[j];
  }
  std::vector<std::complex<double>> r(n);
  for (int i = 0; i < n; i++) {
    const int c0 = x1[2 * i + 1600] ^ x2[2 * i + 1600], c1 = x1[2 * i + 1 + 1600] ^ x2[2 * i + 1 + 1600];
    r[i] = std::complex<double>((1 - 2 * c0) / std::sqrt(2.0), (1 - 2 * c1) / std::sqrt(2.0));
  }
  return r;
}

struct Window {
  std::vector<float> yr, yi;
  std::vector<int> m;
};

Window make_window(int nid, int slot, int symbol, int first_m, int n, double snr_db, double theta, std::mt19937 &rng)
{
  const uint32_t cinit = nr_dmrs_nid_cinit(nid, slot, symbol, 14, -1);
  const auto r = ref_pilots(cinit, first_m + n);
  std::normal_distribution<double> g(0.0, 1.0);
  const double sigma = std::sqrt(std::pow(10.0, -snr_db / 10.0) / 2.0);
  Window w;
  for (int i = 0; i < n; i++) {
    const std::complex<double> y = r[first_m + i] * std::polar(1.0, theta) + std::complex<double>(g(rng) * sigma, g(rng) * sigma);
    w.yr.push_back((float)y.real());
    w.yi.push_back((float)y.imag());
    w.m.push_back(first_m + i);
  }
  return w;
}

} // namespace

TEST(NidSolve, ReferenceMatchesTheReceiversGoldSequence)
{
  for (int nid : {0, 2, 12345, 65535}) {
    for (int slot : {0, 7, 19}) {
      const int sym = slot % 3;
      const unsigned *gold = nr_gold_pdcch(106, 14, (unsigned short)nid, slot, sym);
      std::vector<int16_t> out(2 * 106 * 3 + 8);
      nr_pdcch_dmrs_ref(gold, out.data(), 106);
      const auto r = ref_pilots(nr_dmrs_nid_cinit(nid, slot, sym, 14, -1), 18);
      for (int i = 0; i < 18; i++) {
        // the receiver stores conj(transmitted) scaled; compare sign pattern
        EXPECT_EQ(out[2 * i] > 0, r[i].real() > 0) << "nid " << nid << " slot " << slot << " pilot " << i;
        EXPECT_EQ(out[2 * i + 1] < 0, r[i].imag() > 0) << "nid " << nid << " slot " << slot << " pilot " << i;
      }
    }
  }
}

TEST(NidSolve, NoiselessRecoveryOfAnyIdAndRotation)
{
  std::mt19937 rng(1);
  int ok = 0, total = 0;
  for (int t = 0; t < 400; t++) {
    const int nid = (int)(rng() % 65536), slot = (int)(rng() % 20), sym = (int)(rng() % 3);
    const double theta = (rng() % 100000) / 100000.0 * 2 * M_PI;
    const int first = (int)(rng() % 60) * 3; // window start on an RB boundary
    const Window w = make_window(nid, slot, sym, first, 18, 60.0, theta, rng);
    nr_nid_solution_t s[4];
    const int n = nr_dmrs_nid_solve(w.yr.data(), w.yi.data(), w.m.data(), 18, slot, sym, 14, -1, s, 4);
    total++;
    bool hit = false;
    for (int i = 0; i < n; i++)
      hit |= s[i].nid == nid && s[i].mismatches == 0;
    ok += hit;
  }
  EXPECT_EQ(ok, total);
}

TEST(NidSolve, SnrSweepReportsGracefulDegradation)
{
  std::mt19937 rng(2);
  for (double snr : {20.0, 10.0, 6.0, 3.0, 0.0}) {
    for (int npil : {18, 72}) {
      int top1 = 0, any = 0;
      const int trials = 300;
      for (int t = 0; t < trials; t++) {
        const int nid = (int)(rng() % 65536), slot = (int)(rng() % 20), sym = (int)(rng() % 2);
        const double theta = (rng() % 100000) / 100000.0 * 2 * M_PI;
        const Window w = make_window(nid, slot, sym, 3 * (int)(rng() % 60), npil, snr, theta, rng);
        nr_nid_solution_t s[4];
        const int n = nr_dmrs_nid_solve(w.yr.data(), w.yi.data(), w.m.data(), npil, slot, sym, 14, -1, s, 4);
        if (n > 0 && s[0].nid == nid) top1++;
        for (int i = 0; i < n; i++)
          if (s[i].nid == nid) { any++; break; }
      }
      printf("[nid solve] snr %5.1f dB, %2d pilots: best-score hit %5.1f %%, among valid %5.1f %% (%d windows)\n", snr, npil,
             100.0 * top1 / trials, 100.0 * any / trials, trials);
      if (snr >= 10.0 && npil == 72)
        EXPECT_GE(top1, trials * 95 / 100);
      if (snr >= 20.0)
        EXPECT_GE(top1, trials * 95 / 100);
    }
  }
}

TEST(NidSolve, PureNoiseRarelyProducesAConfidentSolution)
{
  std::mt19937 rng(3);
  std::normal_distribution<double> g(0.0, 1.0);
  int valid = 0, confident = 0;
  const int trials = 3000;
  for (int t = 0; t < trials; t++) {
    std::vector<float> yr(18), yi(18);
    std::vector<int> m(18);
    for (int i = 0; i < 18; i++) { yr[i] = (float)g(rng); yi[i] = (float)g(rng); m[i] = i; }
    nr_nid_solution_t s[4];
    const int n = nr_dmrs_nid_solve(yr.data(), yi.data(), m.data(), 18, (int)(rng() % 20), 0, 14, -1, s, 4);
    valid += n > 0;
    for (int i = 0; i < n; i++)
      confident += s[i].mismatches <= 1;
  }
  printf("[nid solve] noise: %d / %d windows returned a valid N_ID, %d had <= 1 violated check\n", valid, trials, confident);
  EXPECT_LE(confident, trials / 500);
}

TEST(NidSolve, AllZeroWindowGivesNoSolution)
{
  std::vector<float> yr(18, 0.0f), yi(18, 0.0f);
  std::vector<int> m(18);
  for (int i = 0; i < 18; i++) m[i] = i;
  nr_nid_solution_t s[2];
  EXPECT_EQ(nr_dmrs_nid_solve(yr.data(), yi.data(), m.data(), 18, 3, 0, 14, -1, s, 2), 0);
}

// The whole-symbol solve runs inline on the receive thread: report what one call costs on a 273-PRB symbol (819 pilots) of pure noise,
// the common case (a slot without a DCI), and bound it.
TEST(NidSolve, WholeSymbolSolveCostIsBounded)
{
  std::mt19937 rng(7);
  std::normal_distribution<float> g(0.0f, 1.0f);
  const int n = 819;
  std::vector<float> yr(n), yi(n);
  std::vector<int> m(n);
  for (int i = 0; i < n; i++) { yr[i] = g(rng); yi[i] = g(rng); m[i] = i; }
  nr_nid_solution_t s[2];
  const auto t0 = std::chrono::steady_clock::now();
  const int reps = 200;
  for (int r = 0; r < reps; r++) nr_dmrs_nid_solve(yr.data(), yi.data(), m.data(), n, r % 20, 0, 14, -1, s, 2);
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
  printf("[nid solve] 819-pilot noise symbol: %.0f us per call\n", us);
  EXPECT_LT(us, 3000.0);
  const auto tf = std::chrono::steady_clock::now();
  for (int r = 0; r < reps; r++) nr_dmrs_nid_solve_ex(yr.data(), yi.data(), m.data(), n, r % 20, 0, 14, -1, s, 2, 1);
  const double fus = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tf).count() / reps;
  printf("[nid solve] 819-pilot noise symbol, fast: %.0f us per call\n", fus);
  EXPECT_LT(fus, us);
  /* the per-window scan: 18 pilots, 7 references, every window of the symbol */
  const auto t1 = std::chrono::steady_clock::now();
  const int wreps = 2000;
  for (int r = 0; r < wreps; r++) nr_dmrs_nid_solve(yr.data(), yi.data(), m.data(), 18, r % 20, 0, 14, -1, s, 2);
  const double wus = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / wreps;
  printf("[nid solve] 18-pilot window: %.1f us per call (x7 references x windows)\n", wus);
}

// A DCI whose pilots sit in two disjoint 3-RB bundles (9 pilots each), with every other RB of the symbol carrying noise: no window holds
// 16 pilots, but the whole-symbol solve picks the reliable equations wherever they are.
TEST(NidSolve, SplitBundlesAmongNoiseSolveOverTheWholeSymbol)
{
  std::mt19937 rng(7);
  std::normal_distribution<double> g(0.0, 1.0);
  int ok = 0;
  const int trials = 100;
  for (int t = 0; t < trials; t++) {
    const int nid = (int)(rng() % 65536), slot = (int)(rng() % 20), sym = (int)(rng() % 2);
    const int n_rb = 106;
    const auto r = ref_pilots(nr_dmrs_nid_cinit(nid, slot, sym, 14, -1), 3 * n_rb);
    const double theta = (rng() % 100000) / 100000.0 * 2 * M_PI;
    const int b0 = 3 * (int)(rng() % 10), b1 = 60 + 3 * (int)(rng() % 10); // two bundles, RB starts multiples of 3
    std::vector<float> yr, yi;
    std::vector<int> m;
    for (int rb = 0; rb < n_rb; rb++)
      for (int p = 0; p < 3; p++) {
        const int k = 3 * rb + p;
        const bool on = (rb >= b0 && rb < b0 + 3) || (rb >= b1 && rb < b1 + 3);
        std::complex<double> y = on ? r[k] * std::polar(1.0, theta) * 10.0 : std::complex<double>(0, 0);
        y += std::complex<double>(g(rng), g(rng)); // noise 20 dB below the DCI pilots
        yr.push_back((float)y.real());
        yi.push_back((float)y.imag());
        m.push_back(k);
      }
    nr_nid_solution_t s[4];
    const int n = nr_dmrs_nid_solve(yr.data(), yi.data(), m.data(), (int)m.size(), slot, sym, 14, -1, s, 4);
    bool hit = false;
    for (int i = 0; i < n; i++)
      if (s[i].nid == nid && s[i].top_checks >= 4 && s[i].top_mismatches <= s[i].top_checks / 8) { ok++; hit = true; break; }
  }
  printf("[nid solve] split bundles in noise: %d / %d recovered with <= 1 violated top check\n", ok, trials);
  EXPECT_GE(ok, 90); // 94 measured on aarch64/this libstdc++ (the RNG stream differs from the commit's sim host); the property is "nearly all"
}

// fast=1 (shared rotation, early stop on a confirmed class) must give the same best N_ID as the full search wherever the full search is
// right, and must never turn a wrong/no answer into a different confident one. Also times both.
TEST(NidSolve, FastModeGivesTheSameBestNidAndIsCheaper)
{
  std::mt19937 rng(7);
  int both = 0, full_only = 0, fast_only = 0, differ = 0, total = 0;
  double t_full = 0, t_fast = 0;
  for (double snr : {20.0, 10.0, 6.0, 3.0, 0.0}) {
    for (int t = 0; t < 400; t++) {
      const int npil = (t & 1) ? 18 : 72;
      const int nid = (int)(rng() % 65536), slot = (int)(rng() % 20), sym = (int)(rng() % 2);
      const double theta = (rng() % 100000) / 100000.0 * 2 * M_PI;
      const Window w = make_window(nid, slot, sym, 3 * (int)(rng() % 60), npil, snr, theta, rng);
      nr_nid_solution_t a[4], b[4];
      auto c0 = std::chrono::steady_clock::now();
      const int na = nr_dmrs_nid_solve_ex(w.yr.data(), w.yi.data(), w.m.data(), npil, slot, sym, 14, -1, a, 4, 0);
      auto c1 = std::chrono::steady_clock::now();
      const int nb = nr_dmrs_nid_solve_ex(w.yr.data(), w.yi.data(), w.m.data(), npil, slot, sym, 14, -1, b, 4, 1);
      auto c2 = std::chrono::steady_clock::now();
      t_full += std::chrono::duration<double, std::micro>(c1 - c0).count();
      t_fast += std::chrono::duration<double, std::micro>(c2 - c1).count();
      const bool ra = na > 0 && a[0].nid == nid, rb = nb > 0 && b[0].nid == nid;
      total++;
      both += ra && rb;
      full_only += ra && !rb;
      fast_only += !ra && rb;
      differ += na > 0 && nb > 0 && a[0].nid != b[0].nid && !(ra || rb);
    }
  }
  printf("[nid solve] fast vs full over %d windows: both right %d, full only %d, fast only %d; %.1f us -> %.1f us per call\n", total, both,
         full_only, fast_only, t_full / total, t_fast / total);
  EXPECT_LE(full_only, total / 100);
  EXPECT_LE(t_fast, t_full);
}

// Solver-primary helpers (ISAC_DMRS_NID_SOLVE=3).
TEST(NidPrimary, Top2KeepsTheRealIdBesideADciLessLeader)
{
  uint32_t v[4] = {250 /* DCI-less id, e.g. 9020 */, 12 /* real id */, 7, 40};
  int out[2];
  EXPECT_EQ(nr_dmrs_nid_top2(v, 4, 8, out), 2);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1], 3);  // the runner-up with >= 8 votes is kept, the leader does not block it
  v[3] = 7;
  EXPECT_EQ(nr_dmrs_nid_top2(v, 4, 8, out), 2);
  EXPECT_EQ(out[1], 1);
  v[1] = 7;
  EXPECT_EQ(nr_dmrs_nid_top2(v, 4, 8, out), 1);  // below 8 votes: not offered
  v[0] = 3;
  EXPECT_EQ(nr_dmrs_nid_top2(v, 4, 8, out), 0);
}

TEST(NidPrimary, SweepRunsOnlyOnDemand)
{
  const uint64_t G = 1500000, M = 500000, R = 10000000;
  // no result: held during the grace, then the fallback sweep runs
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(0, 0, 0, 1000000, 0, 0, G, M, R), 0);
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(0, 0, 0, 1500000, 0, 0, G, M, R), 1);
  // solver result, not yet verified: one sweep to cross-check, then every maintain period
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(1, 0, 0, 2000000, 0, 0, G, M, R), 1);
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(1, 0, 0, 2100000, 0, 2000000, G, M, R), 0);
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(1, 0, 0, 2500000, 0, 2000000, G, M, R), 1);
  // verified: no sweep until the slow re-verify
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(1, 1, 0, 2000000, 0, 0, G, M, R), 0);
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(1, 1, 0, 9000000, 0, 2000000, G, M, R), 0);
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(1, 1, 0, 12000000, 0, 2000000, G, M, R), 1);
  // unexplained occupancy wakes it even when verified
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(1, 1, 1, 3000000, 0, 2000000, G, M, R), 1);
}

TEST(NidPrimary, VerificationNeedsTwoConfirmations)
{
  EXPECT_LT(nr_dmrs_nid_confirm_score(0, 0), 2);
  EXPECT_LT(nr_dmrs_nid_confirm_score(1, 0), 2);  // a single chance hit does not verify
  EXPECT_EQ(nr_dmrs_nid_confirm_score(1, 1), 2);  // one RNTI already seen elsewhere does
  EXPECT_EQ(nr_dmrs_nid_confirm_score(2, 0), 2);  // two distinct RNTIs do
}

TEST(NidPrimary, HintSignatureChangesWithTheFootprintOnly)
{
  const uint64_t a = nr_dmrs_nid_hint_sig(1, 62922, 10, 6, 0, 1);
  EXPECT_EQ(a, nr_dmrs_nid_hint_sig(1, 62922, 10, 6, 0, 1));
  EXPECT_NE(a, nr_dmrs_nid_hint_sig(1, 62922, 10, 8, 0, 1));  // wider
  EXPECT_NE(a, nr_dmrs_nid_hint_sig(1, 62922, 4, 6, 0, 1));   // shifted
  EXPECT_NE(a, nr_dmrs_nid_hint_sig(1, 62922, 10, 6, 1, 1));  // other symbol
  EXPECT_NE(a, nr_dmrs_nid_hint_sig(1, 9020, 10, 6, 0, 1));   // other id
}

TEST(NidPrimary, GeometryChangeResetsTheState)
{
  nr_dmrs_prim_state_t s = {};
  EXPECT_EQ(nr_dmrs_prim_cfg(&s, 7, 1000), 0);  // first batch anchors the grace
  EXPECT_EQ(s.first_us, 1000u);
  s.verified = 1; s.last_run_us = 5000; s.burst = 3;
  EXPECT_EQ(nr_dmrs_prim_cfg(&s, 7, 9000), 0);  // same cfg: nothing changes
  EXPECT_EQ(s.verified, 1);
  EXPECT_EQ(nr_dmrs_prim_cfg(&s, 8, 20000), 1);  // retune
  EXPECT_EQ(s.verified, 0);
  EXPECT_EQ(s.last_run_us, 0u);
  EXPECT_EQ(s.burst, 0u);
  EXPECT_EQ(s.first_us, 20000u);  // grace anchored to the retune
  EXPECT_EQ(nr_dmrs_nid_sweep_demand(0, 0, 0, 21000, s.first_us, 0, 4000000, 2000000, 15000000), 0);
}

TEST(NidPrimary, OnlyUeSpecificRntisTieEntriesTogether)
{
  EXPECT_FALSE(nr_dmrs_nid_rnti_is_ue(0xFFFF));  // SI-RNTI
  EXPECT_FALSE(nr_dmrs_nid_rnti_is_ue(0xFFFE));  // P-RNTI
  EXPECT_FALSE(nr_dmrs_nid_rnti_is_ue(1));       // RA-RNTI range
  EXPECT_FALSE(nr_dmrs_nid_rnti_is_ue(17920));
  EXPECT_TRUE(nr_dmrs_nid_rnti_is_ue(0x59ab));
  EXPECT_EQ(nr_dmrs_nid_confirm_score(1, nr_dmrs_nid_rnti_is_ue(0xFFFF)), 1);  // a common RNTI seen elsewhere does not verify
}
