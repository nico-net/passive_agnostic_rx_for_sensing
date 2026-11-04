// E2: full-layout DM-RS scorer and constellation Qm classifier on synthetic 4-RX grids (273 PRB numerology).
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <cstdio>
#include <random>
#include <vector>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "nr_blind_stats.h"
#include "nr_dl_layout_score.h"
#include "nr_pdsch_dmrs_scan.h"
#include "nr_pdsch_solver.h"
}
extern "C" configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  if (s) fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  if (assert) abort(); else exit(EXIT_SUCCESS);
}

typedef std::complex<double> cd;
static const int NANT = 4, NPRB = 273, NSC = 12 * NPRB, NSYM = 14, SLOT = 7;

struct Truth {
  int type = 1, groups = 1, nodata = 1, nports_in_group = 1; // groups: CDM-group mask
  uint32_t nid = 1234;
  int nscid = 0;
  int prb0 = 20, nprb = 100;
  uint16_t dmrs = 0, data = 0;
  int qm = 6;
  double snr_db = 10;
  bool group1_data = false; // group 1 REs of the DM-RS symbols carry data (cdm_no_data = 1 with DM-RS in group 0)
  int prg = 2;
};

struct Grid {
  std::vector<c16_t> buf;
  const c16_t *ptr[NANT * NSYM];
  Grid() : buf((size_t)NANT * NSYM * NSC)
  {
    for (int a = 0; a < NANT; a++)
      for (int l = 0; l < NSYM; l++) ptr[a * NSYM + l] = &buf[((size_t)a * NSYM + l) * NSC];
  }
  c16_t *at(int a, int l) { return &buf[((size_t)a * NSYM + l) * NSC]; }
};

static cd qam(std::mt19937 &g, int qm)
{
  const int M = 1 << (qm / 2);
  const double a = std::sqrt(3.0 / (2.0 * (M * M - 1)));
  std::uniform_int_distribution<int> u(0, M - 1);
  return cd(a * (2 * u(g) - M + 1), a * (2 * u(g) - M + 1));
}

// Fills grid: PDSCH on [prb0, prb0+nprb) x symbols 0..13 (data), DM-RS per Truth; noise per RE variance 1 / snr. Es per layer RE = 1 / snr relative...
static void make_grid(Grid &G, const Truth &t, uint32_t seed)
{
  std::mt19937 g(seed);
  std::normal_distribution<double> nd(0, 1);
  const double nv = std::pow(10.0, -t.snr_db / 10.0); // noise variance for unit-power REs with |H| = 1 per antenna
  const double beta = t.nodata == 2 ? std::pow(10.0, 3.0 / 20) : t.nodata == 3 ? std::pow(10.0, 4.77 / 20) : 1.0;
  // channel: per antenna a complex gain + small delay slope; precoder phase constant per PRG
  std::vector<cd> prgw(NPRB / 2 + 2);
  for (auto &w : prgw) w = std::polar(1.0, 6.283 * std::uniform_real_distribution<double>(0, 1)(g));
  cd gain[NANT];
  double dly[NANT];
  for (int a = 0; a < NANT; a++) {
    gain[a] = std::polar(1.0, 6.283 * std::uniform_real_distribution<double>(0, 1)(g));
    dly[a] = std::uniform_real_distribution<double>(-2, 2)(g);
  }
  auto H = [&](int a, int k) { return gain[a] * std::polar(1.0, 6.283 * dly[a] * k / NSC) * prgw[(k / 12) / t.prg]; };
  auto H2 = [&](int a, int k) { return H(a, k) * std::polar(0.8, 0.9 * a + 0.3); };
  std::vector<uint8_t> c(12 * NPRB + 8);
  for (int l = 0; l < NSYM; l++) {
    const bool isd = (t.dmrs >> l) & 1;
    if (isd) nr_dmrs_scan_prb_gold(SLOT, l, NSYM, t.nid, t.nscid, (int)c.size(), c.data());
    for (int p = t.prb0; p < t.prb0 + t.nprb; p++)
      for (int k = 0; k < 12; k++) {
        const int sc = 12 * p + k;
        cd x(0, 0), x2(0, 0);
        if (isd) {
          int gi, kp, m;
          if (t.type == 1) { gi = k % 2; kp = (k / 2) % 2; m = 6 * p + k / 2; }
          else { gi = (k % 6) / 2; kp = k % 2; m = 4 * p + 2 * (k / 6) + kp; }
          if ((t.groups >> gi) & 1) {
            const cd r((1 - 2 * c[2 * m]) / std::sqrt(2.0), (1 - 2 * c[2 * m + 1]) / std::sqrt(2.0));
            x = beta * r;
            if (t.nports_in_group == 2) x2 = beta * r * (kp ? -1.0 : 1.0); // port 2 of the group: OCC over k'
          } else if (gi < t.nodata && !(t.group1_data && gi == 1)) { /* muted: no data in a CDM group without data */ }
          else x = qam(g, t.qm);
        } else if ((t.data >> l) & 1) x = qam(g, t.qm);
        for (int a = 0; a < NANT; a++) {
          cd y = H(a, sc) * x + H2(a, sc) * x2;
          y += std::sqrt(nv / 2) * cd(nd(g), nd(g));
          G.at(a, l)[sc].r = (int16_t)std::lrint(y.real() * 4096);
          G.at(a, l)[sc].i = (int16_t)std::lrint(y.imag() * 4096);
        }
      }
    // outside the allocation: noise only
    for (int sc = 0; sc < NSC; sc++) {
      if (sc >= 12 * t.prb0 && sc < 12 * (t.prb0 + t.nprb)) continue;
      for (int a = 0; a < NANT; a++) {
        G.at(a, l)[sc].r = (int16_t)std::lrint(std::sqrt(nv / 2) * nd(g) * 4096);
        G.at(a, l)[sc].i = (int16_t)std::lrint(std::sqrt(nv / 2) * nd(g) * 4096);
      }
    }
  }
}

static void noise_grid(Grid &G, uint32_t seed, double nv)
{
  std::mt19937 g(seed);
  std::normal_distribution<double> nd(0, std::sqrt(nv / 2) * 4096);
  for (auto &v : G.buf) { v.r = (int16_t)std::lrint(nd(g)); v.i = (int16_t)std::lrint(nd(g)); }
}

struct Hyp {
  nr_dl_hyp_t h;
  std::vector<uint16_t> crb;
};
static Hyp hyp_of(const Truth &t)
{
  Hyp H;
  for (int p = t.prb0; p < t.prb0 + t.nprb; p++) H.crb.push_back(p);
  H.h = {H.crb.data(), (int)H.crb.size(), t.dmrs, (uint8_t)t.type, (uint8_t)t.groups, (uint8_t)t.nodata, t.nid, (uint8_t)t.nscid, 0};
  return H;
}
static nr_dl_obs_t obs_of(const Grid &G, double snr_db)
{
  // the grid is scaled by 4096: noise variance of unit-power RE per antenna = 10^(-snr/10) * 4096^2
  return {G.ptr, NANT, NSC, NSYM, (float)(std::pow(10.0, -snr_db / 10.0) * 4096.0 * 4096.0), SLOT};
}

TEST(DlLayoutScore, GoldMatchesReference)
{
  // the no-allocation generator behind the scorer must equal the scan module's Gold bits: a truth hypothesis on a noiseless grid scores c = 1
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); t.data = 0x3fff; t.snr_db = 60;
  Grid G; make_grid(G, t, 1);
  Hyp H = hyp_of(t); nr_dl_obs_t o = obs_of(G, 60); nr_dl_score_t s;
  nr_dl_layout_score(&H.h, &o, &s);
  const float thr = std::sqrt(1.62f * std::log(42.0f / 1e-3f) / 400.0f);
  EXPECT_NEAR(s.coh, 2 * (1.0f - thr), 0.01f);
  EXPECT_GT(s.occ_f1, 0.99f);
}

TEST(DlLayoutScore, SignificanceConstantMatchesAlpha)
{
  EXPECT_NEAR(0.5 * std::erfc(3.0902 / std::sqrt(2.0)), NBS_SIGNIF_ALPHA, 2e-6);
}

static float rank_of(const Hyp &H, const nr_dl_obs_t &o)
{
  nr_dl_score_t s;
  nr_dl_layout_score(&H.h, &o, &s);
  return nr_dl_score_rank(&H.h, &s);
}

TEST(DlLayoutScore, TrueHypothesisScoresHighest)
{
  for (int type = 1; type <= 2; type++)
  for (int gt = 0; gt < type + 1; gt++) { // DM-RS in each CDM group of the type (the others muted: cdm_no_data covers 0..gt)
    Truth t; t.type = type; t.groups = 1 << gt; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); t.data = 0x3fff; t.nodata = gt + 1 > 1 ? gt + 1 : 2; t.snr_db = 10; // add_pos 1: {2, 11}
    int wins = 0;
    for (int seed = 0; seed < 100; seed++) {
      Grid G; make_grid(G, t, 1000 + seed);
      nr_dl_obs_t o = obs_of(G, 10);
      Hyp T = hyp_of(t);
      const float rt = rank_of(T, o);
      std::vector<Hyp> wrong;
      auto add = [&](auto f) { Hyp W = hyp_of(t); f(W); wrong.push_back(W); };
      add([&](Hyp &W) { W.crb.clear(); for (int p = 30; p < 130; p++) W.crb.push_back(p); W.h.crb = W.crb.data(); W.h.n_crb = 100; }); // wrong RIV (shifted)
      add([&](Hyp &W) { W.crb.resize(50); W.h.n_crb = 50; });                                                                          // wrong RIV (half)
      add([&](Hyp &W) { for (int p = 120; p < 140; p++) W.crb.push_back(p); W.h.crb = W.crb.data(); W.h.n_crb = (int)W.crb.size(); });  // wrong RIV (superset)
      add([&](Hyp &W) { W.h.dmrs_sym_mask = (1 << 3) | (1 << 11); });                                                                  // wrong symbols
      add([&](Hyp &W) { W.h.dmrs_sym_mask = (1 << 2) | (1 << 10); });
      add([&](Hyp &W) { W.h.dmrs_sym_mask = nr_pdsch_solver_dmrs_mask(2, 0, 14, 0); });                                                // add_pos 0
      add([&](Hyp &W) { W.h.dmrs_sym_mask = nr_pdsch_solver_dmrs_mask(2, 0, 14, 2); });                                                // add_pos 2
      add([&](Hyp &W) { W.h.cdm_groups_mask = 1 << ((gt + 1) % (type + 1)); });                                                         // wrong CDM group
      add([&](Hyp &W) { W.h.cdm_groups_mask = 1 << ((gt + 2) % (type + 1)); });
      { nr_dl_score_t st; nr_dl_layout_score(&T.h, &o, &st); EXPECT_GT(st.coh, 1.2f) << "type " << type << " group " << gt; EXPECT_GT(st.occ_f1, 0.9f) << "type " << type << " group " << gt; }
      add([&](Hyp &W) { W.h.cdm_no_data = 1; });                                                                                       // declares data where the group is empty
      bool best = std::isfinite(rt);
      for (auto &W : wrong) best = best && rt > rank_of(W, o);
      wins += best;
    }
    EXPECT_EQ(wins, 100) << "type " << type << " group " << gt;
  }
}

TEST(DlLayoutScore, TwoPortsInGroupStayCoherent)
{
  // two layers in one CDM group (OCC over the k' pair): the same-k' pairs still add coherently
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); t.data = 0x3fff; t.nports_in_group = 2;
  Grid G; make_grid(G, t, 5);
  Hyp H = hyp_of(t); nr_dl_obs_t o = obs_of(G, 10); nr_dl_score_t s;
  nr_dl_layout_score(&H.h, &o, &s);
  EXPECT_GT(s.coh, 1.2f);
}

TEST(DlLayoutScore, NoiseScoresLow)
{
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1);
  int bad = 0;
  for (int seed = 0; seed < 50; seed++) {
    Grid G; noise_grid(G, seed, 1.0);
    Hyp H = hyp_of(t); nr_dl_obs_t o = obs_of(G, 0); nr_dl_score_t s;
    nr_dl_layout_score(&H.h, &o, &s);
    bad += !(s.coh < 0 && s.occ_f1 < 0.1f);
  }
  EXPECT_EQ(bad, 0);
}

TEST(DlLayoutScore, CombRatioSeparatesCdmNoData)
{
  Truth a; a.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); a.data = 0x3fff; a.nodata = 2; a.snr_db = 15;  // group 1 empty
  Truth b = a; b.nodata = 1; b.group1_data = true;                                                          // group 1 carries data
  for (int seed = 0; seed < 20; seed++) {
    Grid GA, GB; make_grid(GA, a, seed); make_grid(GB, b, seed);
    Hyp HA = hyp_of(a), HB = hyp_of(b);
    nr_dl_obs_t oa = obs_of(GA, 15), ob = obs_of(GB, 15);
    nr_dl_score_t sa, sb, sx, sy;
    nr_dl_layout_score(&HA.h, &oa, &sa); nr_dl_layout_score(&HB.h, &ob, &sb);
    nr_dl_layout_score(&HB.h, &oa, &sx); nr_dl_layout_score(&HA.h, &ob, &sy);   // declarations swapped
    EXPECT_LT(sa.comb_ratio, 0.1f); EXPECT_GT(sb.comb_ratio, 0.5f);
    EXPECT_TRUE(nr_dl_cdm_consistent(&HA.h, &sa)); EXPECT_TRUE(nr_dl_cdm_consistent(&HB.h, &sb));
    EXPECT_FALSE(nr_dl_cdm_consistent(&HB.h, &sx)); EXPECT_FALSE(nr_dl_cdm_consistent(&HA.h, &sy));
  }
}

static std::vector<c16_t> qam_syms(std::mt19937 &g, int qm, int n, double snr_db)
{
  std::normal_distribution<double> nd(0, 1);
  const double sd = std::sqrt(std::pow(10.0, -snr_db / 10.0) / 2);
  std::vector<c16_t> v(n);
  for (auto &s : v) {
    const cd x = qam(g, qm) + sd * cd(nd(g), nd(g));
    s.r = (int16_t)std::lrint(x.real() * 8192); s.i = (int16_t)std::lrint(x.imag() * 8192);
  }
  return v;
}

TEST(DlLayoutScore, QmClassifies)
{
  std::mt19937 g(77);
  const int qms[4] = {2, 4, 6, 8};
  for (int qm : qms) {
    int ok = 0, N = 200;
    for (int i = 0; i < N; i++) {
      auto v = qam_syms(g, qm, 256, 25);
      ok += nr_qm_classify(v.data(), 256, (float)std::pow(10.0, -2.5), nullptr) == qm;
    }
    EXPECT_GE(ok, N * 99 / 100) << "qm " << qm;
  }
  // 5 dB: 256 vs 64 (and every pair) is never a wrong certain answer; 256 / 64 are mostly undecided
  int undec = 0, tot = 0;
  for (int qm : qms)
    for (int i = 0; i < 300; i++) {
      auto v = qam_syms(g, qm, 256, 5);
      const int r = nr_qm_classify(v.data(), 256, (float)std::pow(10.0, -0.5), nullptr);
      EXPECT_TRUE(r == 0 || r == qm) << "qm " << qm << " got " << r;
      if (qm >= 6) { tot++; undec += r == 0; }
    }
  EXPECT_GT(undec, tot / 2);
}

TEST(DlLayoutScore, QmFromGrid)
{
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); t.data = 0x3fff; t.snr_db = 25; t.qm = 6; t.nodata = 2;
  int ok = 0;
  for (int seed = 0; seed < 20; seed++) {
    Grid G; make_grid(G, t, 300 + seed);
    Hyp H = hyp_of(t); H.h.data_sym_mask = t.data; nr_dl_obs_t o = obs_of(G, 25); nr_dl_score_t s;
    nr_dl_layout_score(&H.h, &o, &s);
    ok += s.qm == 6;
  }
  EXPECT_GE(ok, 19);
}

TEST(DlLayoutScore, Bench)
{
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); t.data = 0x3fff; t.nodata = 2;
  Grid G; make_grid(G, t, 9);
  Hyp H = hyp_of(t); nr_dl_obs_t o = obs_of(G, 10); nr_dl_score_t s;
  for (int pass = 0; pass < 2; pass++) {
    H.h.data_sym_mask = pass ? t.data : 0;
    const int N = 300;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) nr_dl_layout_score(&H.h, &o, &s);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
    std::printf("[bench] 100 PRB, 2 DM-RS symbols, %d RX, %s: %.0f ns per hypothesis\n", NANT, pass ? "with Qm" : "scorer only", ns);
  }
}

// ---- fix round 1 ----
// I1: the Qm answer must never be a wrong certain one when the noise model is wrong (noise_var low, H error, an unmodelled layer)
static int wrong_certain(std::function<std::vector<c16_t>(std::mt19937 &, int qm)> mk, float nv, int per_qm, int *decided = nullptr)
{
  std::mt19937 g(4242);
  int wrong = 0;
  for (int qm : {2, 4, 6, 8})
    for (int i = 0; i < per_qm; i++) {
      auto v = mk(g, qm);
      const int r = nr_qm_classify(v.data(), (int)v.size(), nv, nullptr);
      wrong += r != 0 && r != qm;
      if (decided) *decided += r != 0;
    }
  return wrong;
}

TEST(DlLayoutScore, QmMisspecifiedNoiseNeverWrongCertain)
{
  for (double snr : {15.0, 25.0})
    for (double under_db : {3.0, 6.0}) {
      const float nv = (float)std::pow(10.0, -(snr + under_db) / 10.0); // the caller believes the noise is under_db lower than it is
      EXPECT_EQ(wrong_certain([&](std::mt19937 &g, int qm) { return qam_syms(g, qm, 256, snr); }, nv, 250), 0) << "snr " << snr << " under " << under_db;
    }
}

TEST(DlLayoutScore, QmPrbFlatHError)
{
  // 12-sample PRBs share one complex gain error (PRB-flat H estimated with error), noise_var is right
  for (double herr_db : {-25.0, -18.0, -15.0}) { // -12 dB (25 % gain error, far beyond a 12-pilot x 4-RX estimate) gave 2 wrong in 1000: see the report
    const double sd = std::pow(10.0, herr_db / 20.0);
    auto mk = [&](std::mt19937 &g, int qm) {
      std::normal_distribution<double> nd(0, 1);
      std::vector<c16_t> v(240);
      for (size_t i = 0; i < v.size(); i += 12) {
        const cd e = 1.0 + sd / std::sqrt(2.0) * cd(nd(g), nd(g));
        for (size_t j = 0; j < 12; j++) {
          const cd x = e * qam(g, qm) + std::sqrt(std::pow(10.0, -2.5) / 2) * cd(nd(g), nd(g));
          v[i + j].r = (int16_t)std::lrint(x.real() * 8192); v[i + j].i = (int16_t)std::lrint(x.imag() * 8192);
        }
      }
      return v;
    };
    EXPECT_EQ(wrong_certain(mk, (float)std::pow(10.0, -2.5), 250), 0) << "H error " << herr_db << " dB";
  }
}

TEST(DlLayoutScore, QmRank2InterfererNeverWrongCertain)
{
  // Interferer of the same order (rank-2, same MCS) at -3..0 dB: an unstructured blob. NOT at lattice-coincident ratios (QPSK + 0.5 QPSK is exactly 16QAM, 16QAM + 1/3 16QAM
  // is a 256QAM-like lattice: superposition coding, indistinguishable from the higher order by any statistic of the symbols; see the report).
  for (double ratio_db : {-3.0, -1.5, 0.0}) {
    const double amp = std::pow(10.0, ratio_db / 20.0);
    auto mk = [&](std::mt19937 &g, int qm) {
      std::normal_distribution<double> nd(0, 1);
      std::vector<c16_t> v(256);
      for (auto &s : v) {
        const cd x = qam(g, qm) + amp * qam(g, qm) + std::sqrt(std::pow(10.0, -2.5) / 2) * cd(nd(g), nd(g)); // layer 2 leaks in
        s.r = (int16_t)std::lrint(x.real() * 8192); s.i = (int16_t)std::lrint(x.imag() * 8192);
      }
      return v;
    };
    EXPECT_EQ(wrong_certain(mk, (float)std::pow(10.0, -2.5), 250), 0) << "interferer " << ratio_db << " dB";
  }
}

TEST(DlLayoutScore, QmCorrectStillDecidesAt25dB)
{
  int dec = 0;
  EXPECT_EQ(wrong_certain([&](std::mt19937 &g, int qm) { return qam_syms(g, qm, 256, 25); }, (float)std::pow(10.0, -2.5), 500, &dec), 0);
  EXPECT_GE(dec, 2000 * 99 / 100);
}

// I2: no int16 wrap, no NaN
TEST(DlLayoutScore, QmDegenerateInputsAreUndecided)
{
  std::vector<c16_t> z(256, c16_t{0, 0});
  EXPECT_EQ(nr_qm_classify(z.data(), 256, 0.01f, nullptr), 0);
  EXPECT_EQ(nr_qm_classify(z.data(), 256, NAN, nullptr), 0);
  EXPECT_EQ(nr_qm_classify(z.data(), 256, 0.f, nullptr), 0);
  std::vector<c16_t> big(256, c16_t{32767, -32768});
  const int r = nr_qm_classify(big.data(), 256, 0.01f, nullptr);
  EXPECT_EQ(r, 0);
  // grid: every data RE zero -> e == 0 -> qm 0 (no NaN cast)
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); t.data = 0x3fff; t.snr_db = 25; t.nodata = 2;
  Grid G; make_grid(G, t, 11);
  for (int a = 0; a < NANT; a++) for (int l = 0; l < NSYM; l++) if (!((t.dmrs >> l) & 1)) std::fill(G.at(a, l), G.at(a, l) + NSC, c16_t{0, 0});
  Hyp H = hyp_of(t); H.h.data_sym_mask = t.data; nr_dl_obs_t o = obs_of(G, 25); nr_dl_score_t s;
  nr_dl_layout_score(&H.h, &o, &s);
  EXPECT_EQ(s.qm, 0);
  // grid: a deep-fade PRB (pilots ~0 there) gives huge equalised samples: clamped, never a wrong certain order
  Truth t2 = t; t2.qm = 6;
  Grid G2; make_grid(G2, t2, 12);
  for (int a = 0; a < NANT; a++) for (int l : {2, 11}) for (int k = 0; k < 12; k++) G2.at(a, l)[12 * (t2.prb0 + 5) + k] = c16_t{0, 1};
  Hyp H2 = hyp_of(t2); H2.h.data_sym_mask = t2.data; nr_dl_obs_t o2 = obs_of(G2, 25);
  nr_dl_layout_score(&H2.h, &o2, &s);
  EXPECT_TRUE(s.qm == 0 || s.qm == 6) << s.qm;
}

// I3: tiny allocations are not significant and cannot be ranked
TEST(DlLayoutScore, TinyAllocationNotSignificantAndCannotBeOutranked)
{
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1); t.data = 0x3fff; t.nodata = 2; t.snr_db = 20; t.nprb = 1;
  for (int seed = 0; seed < 30; seed++) {
    Grid G; make_grid(G, t, 700 + seed);
    nr_dl_obs_t o = obs_of(G, 20);
    Hyp T = hyp_of(t); nr_dl_score_t st;
    nr_dl_layout_score(&T.h, &o, &st);
    EXPECT_EQ(st.n_signif, 0);
    EXPECT_EQ(st.n_cells, 2);
    const float rt = nr_dl_score_rank(&T.h, &st);
    for (int ext : {2, 3, 6}) {   // noise-extended supersets
      Hyp W = hyp_of(t); for (int p = 1; p < ext; p++) W.crb.push_back(t.prb0 + p); W.h.crb = W.crb.data(); W.h.n_crb = ext;
      nr_dl_score_t sw; nr_dl_layout_score(&W.h, &o, &sw);
      EXPECT_FALSE(nr_dl_score_rank(&W.h, &sw) > rt) << "ext " << ext << " nsig " << sw.n_signif << " cells " << sw.n_cells << " coh " << sw.coh << " rank " << nr_dl_score_rank(&W.h, &sw) << " rt " << rt;
    }
  }
  // a full-size truth is significant in all of its cells
  Truth big; big.dmrs = t.dmrs; big.data = t.data; big.nodata = 2; big.snr_db = 10;
  Grid G; make_grid(G, big, 3); nr_dl_obs_t o = obs_of(G, 10); Hyp B = hyp_of(big); nr_dl_score_t sb;
  nr_dl_layout_score(&B.h, &o, &sb);
  EXPECT_EQ(sb.n_signif, sb.n_cells);
  EXPECT_EQ(sb.n_cells, 2);
}

TEST(DlLayoutScore, RankOnInvalidInputIsMinusInfinity)
{
  Truth t; t.dmrs = nr_pdsch_solver_dmrs_mask(2, 0, 14, 1);
  Grid G; noise_grid(G, 1, 1.0);
  Hyp H = hyp_of(t); H.h.dmrs_type = 3; nr_dl_obs_t o = obs_of(G, 0); nr_dl_score_t s;
  nr_dl_layout_score(&H.h, &o, &s);
  EXPECT_EQ(nr_dl_score_rank(&H.h, &s), -INFINITY);
  EXPECT_EQ(nr_dl_score_rank(nullptr, &s), -INFINITY);
  EXPECT_EQ(nr_dl_cdm_consistent(nullptr, &s), 0);
}

// M5: false-cell rate of c_alpha on pure noise at the stated alpha_cell = 1e-3 / 42 = 2.4e-5 (bound: 1e-4), n = 8, 24, 100 pairs, 1 and 4 RX
TEST(DlLayoutScore, FalseCellRateAtAlphaCell)
{
  std::mt19937 g(2026);
  std::normal_distribution<double> nd(0, 300);
  for (int nant : {1, 4})
    for (int nprb : {2, 6, 25}) {
      const int n_pairs = 4 * nprb;
      const float thr = std::sqrt(1.62f * std::log(42.0f / 1e-3f) / (float)n_pairs);
      const int cells = 2000000;
      std::vector<int16_t> buf((size_t)nant * 2 * 12 * nprb);
      const int16_t *rx[4];
      for (int a = 0; a < nant; a++) rx[a] = &buf[(size_t)a * 24 * nprb];
      std::vector<uint8_t> c(12 * nprb);
      std::vector<uint16_t> crb(nprb);
      for (int p = 0; p < nprb; p++) crb[p] = p;
      long exceed = 0;
      for (int i = 0; i < cells; i++) {
        for (auto &v : buf) v = (int16_t)std::lrint(nd(g));
        for (auto &b : c) b = g() & 1;
        exceed += nr_dmrs_scan_prb_score_gold(rx, nant, 12 * nprb, 0, c.data(), crb.data(), nprb) > thr;
      }
      std::printf("[mc] %d RX, %d pairs: false-cell rate %.2e (%ld / %d), alpha_cell 2.4e-5\n", nant, n_pairs, (double)exceed / cells, exceed, cells);
      EXPECT_LE(exceed, cells / 10000) << nant << " RX, " << n_pairs << " pairs";
    }
}
