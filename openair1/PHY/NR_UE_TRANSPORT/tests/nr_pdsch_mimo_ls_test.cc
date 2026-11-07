// Windowed least-squares per-layer channel estimate for a multi-layer PDSCH (nr_pdsch_mimo_ls.c): synthetic 4-RX, Nl 2..4,
// frequency-selective channel, known QAM layer symbols. Reference "DM-RS estimate" = a 1-pilot-per-RE-pair LS on the same noisy
// samples at a DM-RS-like density (one orthogonal pilot RE per 2 subcarriers per layer, 2 pilot symbols).
#include <gtest/gtest.h>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
extern "C" {
#include "nr_pdsch_mimo_ls.h"
}
namespace {
using cd = std::complex<double>;
constexpr int NANT = 4;

struct Grant {
  int nl, nre;
  std::vector<int16_t> x;            // nre*nl complex
  std::vector<uint32_t> k, sym;
  std::vector<int16_t> y;            // NANT planes of nre
  std::vector<cd> htrue;             // [(l*NANT+a)*nre + i]
  double nvar;                       // per-RE complex noise variance
};

// Multipath channel: taps at delays (samples of a 4096 FFT) with random complex gains per (rx, layer).
Grant make(int nl, int nsym, int nsc, double snr_db, double delay_max, uint32_t seed, bool correlated = false)
{
  std::mt19937 g(seed);
  std::normal_distribution<double> n01(0, 1);
  std::uniform_int_distribution<int> q(0, 3);
  Grant G;
  G.nl = nl; G.nre = nsym * nsc;
  G.x.resize(2 * G.nre * nl); G.k.resize(G.nre); G.sym.resize(G.nre); G.y.assign(2 * NANT * (size_t)G.nre, 0);
  G.htrue.resize((size_t)nl * NANT * G.nre);
  const double amp = 4096;                                            // QPSK-ish layer symbols of magnitude 'amp'
  const int ntap = 4;
  std::vector<std::vector<cd>> gain(nl * NANT, std::vector<cd>(ntap));
  std::vector<double> dly(ntap);
  for (int t = 0; t < ntap; t++) dly[t] = delay_max * t / (ntap - 1);
  for (auto &v : gain) for (auto &c : v) c = cd(n01(g), n01(g)) / std::sqrt(2.0 * ntap);
  const double sigp = amp * amp;                                      // |x|^2 per layer; y power ~ nl*sigp*|h|^2
  G.nvar = nl * sigp / std::pow(10.0, snr_db / 10);
  std::normal_distribution<double> nz(0, std::sqrt(G.nvar / 2));
  for (int s = 0; s < nsym; s++)
    for (int c = 0; c < nsc; c++) {
      const int i = s * nsc + c;
      G.k[i] = 100 + c; G.sym[i] = 2 + s;
      cd x[4];
      for (int l = 0; l < nl; l++) {
        const int b = q(g);
        x[l] = amp / std::sqrt(2.0) * cd(b & 1 ? 1 : -1, b & 2 ? 1 : -1);
        if (correlated && l > 0) x[l] = x[0];
        G.x[2 * (i * nl + l)] = (int16_t)std::lround(x[l].real());
        G.x[2 * (i * nl + l) + 1] = (int16_t)std::lround(x[l].imag());
        x[l] = cd(G.x[2 * (i * nl + l)], G.x[2 * (i * nl + l) + 1]);
      }
      for (int a = 0; a < NANT; a++) {
        cd yy = 0;
        for (int l = 0; l < nl; l++) {
          cd h = 0;
          for (int t = 0; t < ntap; t++) h += gain[l * NANT + a][t] * std::polar(1.0, -2 * M_PI * dly[t] * G.k[i] / 4096.0);
          G.htrue[((size_t)l * NANT + a) * G.nre + i] = h;
          yy += h * x[l];
        }
        yy += cd(nz(g), nz(g));
        G.y[2 * ((size_t)a * G.nre + i)] = (int16_t)std::lround(yy.real());
        G.y[2 * ((size_t)a * G.nre + i) + 1] = (int16_t)std::lround(yy.imag());
      }
    }
  return G;
}

struct Est { std::vector<float> h; std::vector<uint32_t> k, sym; std::vector<uint16_t> raw; int n; nr_pdsch_mimo_ls_info_t info; };
Est run(const Grant &G)
{
  const size_t cap = G.nre / 4 + 8;
  Est e;
  e.h.assign(2 * (size_t)G.nl * NANT * cap, 0); e.k.resize(cap); e.sym.resize(cap); e.raw.resize(cap);
  e.n = nr_pdsch_mimo_ls_cfr(G.nre, G.nl, NANT, G.x.data(), G.k.data(), G.sym.data(), G.y.data(), G.nre, (int)cap, cap, e.h.data(),
                             e.k.data(), e.sym.data(), e.raw.data(), &e.info);
  return e;
}
// Mean squared error of the LS estimate against the true CFR at the window centre RE, normalised by mean |h|^2.
double ls_nmse(const Grant &G, const Est &e)
{
  const size_t cap = G.nre / 4 + 8;
  double err = 0, pw = 0; int cnt = 0;
  for (int w = 0; w < e.n; w++) {
    // find the RE index of the window centre
    int i = 0;
    for (; i < G.nre; i++) if (G.k[i] == e.k[w] && G.sym[i] == e.sym[w]) break;
    for (int l = 0; l < G.nl; l++) for (int a = 0; a < NANT; a++) {
      const cd est(e.h[2 * ((size_t)(l * NANT + a) * cap + w)], e.h[2 * ((size_t)(l * NANT + a) * cap + w) + 1]);
      const cd tr = G.htrue[((size_t)l * NANT + a) * G.nre + i];
      err += std::norm(est - tr); pw += std::norm(tr); cnt++;
    }
  }
  return err / pw;
}
// DM-RS-style reference: two pilot symbols, per layer a comb (layer l on subcarriers k == l mod nl, orthogonal), LS per pilot RE, then
// linear interpolation across the pilot subcarriers and average of the two pilot symbols (the usual chest), error at the same RE set.
double dmrs_nmse(const Grant &G, double snr_db, uint32_t seed, int nsc, const Est &e)
{
  std::mt19937 g(seed);
  std::normal_distribution<double> nz(0, 1);
  const double amp = 4096, nvar = G.nl * amp * amp / std::pow(10.0, snr_db / 10);
  const size_t cap = G.nre / 4 + 8;
  double err = 0, pw = 0;
  // pilot estimate Hp[l][a][c] on subcarriers c = l, l+nl... (true channel at the first symbol; same noise level per pilot RE as data RE)
  for (int w = 0; w < e.n; w++) {
    int i = 0;
    for (; i < G.nre; i++) if (G.k[i] == e.k[w] && G.sym[i] == e.sym[w]) break;
    const int c = i % nsc;
    for (int l = 0; l < G.nl; l++) for (int a = 0; a < NANT; a++) {
      // nearest two pilot REs of layer l around c on the comb of spacing nl*2
      const int sp = G.nl * 2, c0 = (c / sp) * sp + l * 2, c1 = c0 + sp;
      auto pil = [&](int cc) {
        const int ii = (i / nsc) * nsc + std::min(cc, nsc - 1);
        cd h = G.htrue[((size_t)l * NANT + a) * G.nre + ii];
        return h + cd(nz(g), nz(g)) * std::sqrt(nvar / 2) / amp;     // LS on a unit-power-amp pilot
      };
      const cd p0 = pil(c0), p1 = pil(c1);
      const double t = (double)(c - c0) / sp;
      const cd est = p0 * (1 - t) + p1 * t;
      err += std::norm(est - G.htrue[((size_t)l * NANT + a) * G.nre + i]);
      pw += std::norm(G.htrue[((size_t)l * NANT + a) * G.nre + i]);
    }
  }
  (void)cap;
  return err / pw;
}

class MimoLs : public ::testing::TestWithParam<int> {};
TEST_P(MimoLs, BeatsDmrsEstimateAtSameSnr)
{
  const int nl = GetParam();
  const double snr = 15;
  double ls = 0, dm = 0;
  for (uint32_t seed = 1; seed <= 5; seed++) {
    const Grant G = make(nl, 8, 288, snr, 6.0, seed);
    const Est e = run(G);
    ASSERT_GT(e.n, 0);
    ls += ls_nmse(G, e);
    dm += dmrs_nmse(G, snr, 100 + seed, 288, e);
  }
  std::printf("Nl=%d snr=%.0f dB: LS nmse %.4g vs DM-RS %.4g (mean of 5 seeds)\n", nl, snr, ls / 5, dm / 5);
  EXPECT_LT(ls, dm) << "Nl=" << nl << " LS nmse " << ls / 5 << " vs DM-RS " << dm / 5;
}
INSTANTIATE_TEST_SUITE_P(Layers, MimoLs, ::testing::Values(2, 3, 4));

TEST(MimoLsWindow, ExactOnNoiseless)
{
  const Grant G = make(2, 1, 8, 200, 0.0, 7);                            // flat channel, ~no noise
  float h[2 * 2 * NANT]; double res;
  ASSERT_EQ(nr_pdsch_mimo_ls_window(8, 2, NANT, G.x.data(), G.y.data(), G.nre, h, &res), 0);
  for (int l = 0; l < 2; l++) for (int a = 0; a < NANT; a++) {
    const cd tr = G.htrue[((size_t)l * NANT + a) * G.nre];
    EXPECT_NEAR(h[2 * (l * NANT + a)], tr.real(), 2e-3);
    EXPECT_NEAR(h[2 * (l * NANT + a) + 1], tr.imag(), 2e-3);
  }
}

TEST(MimoLsWindow, IllConditionedWindowSkipped)
{
  const Grant G = make(2, 1, 8, 30, 0.0, 3, /*correlated=*/true);       // both layers carry the same symbol: X^H X is singular
  float h[2 * 2 * NANT]; double res = -1;
  EXPECT_EQ(nr_pdsch_mimo_ls_window(8, 2, NANT, G.x.data(), G.y.data(), G.nre, h, &res), -1);
  EXPECT_EQ(res, -1);
  const Est e = run(make(2, 4, 96, 30, 0.0, 3, true));
  EXPECT_EQ(e.n, 0);                                                      // nothing estimable, nothing exported
}

TEST(MimoLsWindow, SomeWindowsSkippedOthersKept)
{
  // two layers identical on the first half of the RE, independent on the second
  Grant G = make(2, 1, 96, 25, 0.0, 11);
  for (int i = 0; i < 48; i++) { G.x[2 * (i * 2 + 1)] = G.x[2 * (i * 2)]; G.x[2 * (i * 2 + 1) + 1] = G.x[2 * (i * 2) + 1]; }
  const Est e = run(G);
  EXPECT_GT(e.info.n_skipped, 0);
  EXPECT_GT(e.n, 0);
  for (int w = 0; w < e.n; w++) {
    EXPECT_GE(e.k[w], 100u + 44u) << "a window inside the singular half was exported";
  }
}

TEST(MimoLsLadder, WindowTracksDelaySpread)
{
  const Est flat = run(make(2, 8, 288, 25, 0.0, 5));
  const Est sel = run(make(2, 8, 288, 25, 40.0, 5));
  EXPECT_GT(flat.info.win, sel.info.win);
  EXPECT_GE(sel.info.win, 4);                                              // never below 2*Nl
  EXPECT_LE(flat.info.win, NR_MIMO_LS_MAX_WIN);
}

TEST(MimoLsLayout, WindowsStayInsideSymbolAndRun)
{
  const Grant G = make(3, 5, 60, 20, 4.0, 9);
  const Est e = run(G);
  ASSERT_GT(e.n, 0);
  int total = 0;
  for (int w = 0; w < e.n; w++) {
    total += e.raw[w];
    EXPECT_GE(e.sym[w], 2u); EXPECT_LE(e.sym[w], 6u);
    EXPECT_GE(e.raw[w], 6);                                                // >= 2*Nl
    if (w) EXPECT_TRUE(e.sym[w] > e.sym[w - 1] || e.k[w] > e.k[w - 1]);
  }
  EXPECT_LE(total, G.nre);                                                 // an RE is pooled at most once
  EXPECT_GT(total, G.nre / 2);                                             // most windows are well conditioned
}
}  // namespace

// out_nv (nr_pdsch_mimo_ls_cfr_nv): the per-row, per-layer noise variance of the window estimate, sigma_y^2 (G^-1)_ll, checked against the empirical error variance of the estimate on a
// flat channel (so the window ladder may widen freely) over Monte-Carlo noise, Nl = 2, 3, 4 with 4 RX. The row's own v-hat is a chi-square estimate (dof nant (n - nl)), so the MEAN over many
// rows is compared (unbiased), and the sample variance of v-hat/v is reported.
TEST(MimoLsNoiseVar, OutNvMatchesEmpiricalErrorVariance)
{
  for (int nl : {2, 3, 4}) {
    double sum_err = 0, sum_nv = 0, sum_raw = 0; long cnt = 0;
    for (uint32_t seed = 1; seed <= 60; seed++) {
      const Grant G = make(nl, 4, 48, 12.0, 0.0, 1000 * nl + seed);
      const size_t cap = G.nre / 4 + 8;
      std::vector<float> h(2 * (size_t)G.nl * NANT * cap), nv((size_t)G.nl * cap);
      std::vector<uint32_t> k(cap), sym(cap);
      std::vector<uint16_t> raw(cap);
      nr_pdsch_mimo_ls_info_t info;
      const int n = nr_pdsch_mimo_ls_cfr_nv(G.nre, G.nl, NANT, G.x.data(), G.k.data(), G.sym.data(), G.y.data(), G.nre, (int)cap, cap, h.data(), k.data(), sym.data(), raw.data(), nv.data(), &info);
      ASSERT_GT(n, 0);
      for (int w = 0; w < n; w++) {
        int i = 0;
        for (; i < G.nre; i++) if (G.k[i] == k[w] && G.sym[i] == sym[w]) break;
        for (int l = 0; l < nl; l++) for (int a = 0; a < NANT; a++) {
          const cd est(h[2 * ((size_t)(l * NANT + a) * cap + w)], h[2 * ((size_t)(l * NANT + a) * cap + w) + 1]);
          sum_err += std::norm(est - G.htrue[((size_t)l * NANT + a) * G.nre + i]);
          sum_nv += nv[(size_t)l * cap + w];
          cnt++;
        }
        sum_raw += raw[w];
      }
    }
    const double r = sum_nv / sum_err;
    printf("MIMO_NV Nl=%d: mean out_nv / empirical error variance = %.3f over %ld estimates (mean window %.1f REs)\n", nl, r, cnt, sum_raw / (cnt / (nl * NANT)));
    EXPECT_GT(r, 0.9) << nl;
    EXPECT_LT(r, 1.1) << nl;
  }
  // the old entry point is the same function without the noise planes
  const Grant G = make(2, 4, 48, 12.0, 0.0, 5);
  const size_t cap = G.nre / 4 + 8;
  std::vector<float> h1(2 * (size_t)2 * NANT * cap), h2(h1.size()), nv(2 * cap);
  std::vector<uint32_t> k(cap), s(cap); std::vector<uint16_t> raw(cap);
  const int n1 = nr_pdsch_mimo_ls_cfr(G.nre, 2, NANT, G.x.data(), G.k.data(), G.sym.data(), G.y.data(), G.nre, (int)cap, cap, h1.data(), k.data(), s.data(), raw.data(), nullptr);
  const int n2 = nr_pdsch_mimo_ls_cfr_nv(G.nre, 2, NANT, G.x.data(), G.k.data(), G.sym.data(), G.y.data(), G.nre, (int)cap, cap, h2.data(), k.data(), s.data(), raw.data(), nv.data(), nullptr);
  EXPECT_EQ(n1, n2);
  EXPECT_EQ(h1, h2);
}
