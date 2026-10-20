/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* K38: a code-block-0 probe must hand the LDPC decoder the same CB0 LLRs as a whole-slot decode of the
 * same hypothesis. The probe's buffer holds the same LLRs up to its horizon and zeros after it; the
 * LLR-norm shift must not see the difference. */
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>
extern "C" {
#include "nr_llr_norm.h"
#include "nr_mmse_exact.h"
}

namespace {
/* The rank-4 pin49r4 grant the rfsim harness sampled: 273 PRB, 4 layers, 64QAM, S=1 L=13, DM-RS on
 * 2 and 11 (no data there), so 11 data symbols of 273*12*6*4 LLRs. Fixed-point MMSE scale: mean |LLR|
 * ~450 (the norm's own comment: 454 measured on this bed). */
constexpr uint32_t kPerSym = 273u * 12u * 6u * 4u;
constexpr uint32_t kG = 11u * kPerSym;
constexpr uint32_t kTbs = 589848u; /* any TBS with C > 1 */

std::vector<int16_t> full_llrs(uint32_t seed)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> d(0.0, 560.0);
  std::vector<int16_t> v(kG);
  for (auto &x : v) {
    double s = d(rng);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    x = (int16_t)s;
  }
  return v;
}

/* What the probe's buffer holds: the symbols up to the horizon (code block 0's span plus one symbol of
 * slack, rounded up to whole symbols), then zeros (skipped symbols contribute no LLRs). */
std::vector<int16_t> probe_llrs(const std::vector<int16_t> &full, uint32_t C)
{
  const uint32_t E0 = (kG + C - 1) / C;
  uint32_t have = 0;
  while (have < E0 + kPerSym)
    have += kPerSym;
  std::vector<int16_t> p(full.size(), 0);
  std::copy(full.begin(), full.begin() + have, p.begin());
  return p;
}
} // namespace

TEST(LlrNorm, NumCbMatches38212)
{
  /* B = TBS + 24 <= Kcb: one block (Kcb 8448 / 3840) */
  EXPECT_EQ(nr_llr_norm_num_cb(8424u, 1), 1u);
  EXPECT_EQ(nr_llr_norm_num_cb(8425u, 1), 2u);
  EXPECT_EQ(nr_llr_norm_num_cb(3816u, 2), 1u);
  EXPECT_EQ(nr_llr_norm_num_cb(3817u, 2), 2u);
  /* C = ceil(B / (Kcb - 24)) */
  EXPECT_EQ(nr_llr_norm_num_cb(2u * 8424u - 24u, 1), 2u);
  EXPECT_EQ(nr_llr_norm_num_cb(2u * 8424u - 23u, 1), 3u);
}

TEST(LlrNorm, SpanIsCodeBlockZeroCeiling)
{
  EXPECT_EQ(nr_llr_norm_span(1000u, 1u), 1000u);
  EXPECT_EQ(nr_llr_norm_span(1000u, 3u), 334u);
  EXPECT_EQ(nr_llr_norm_span(999u, 3u), 333u);
}

TEST(LlrNorm, ShiftBringsMeanUnderTarget)
{
  std::vector<int16_t> v(4096, 0);
  EXPECT_EQ(nr_llr_norm_shift(v.data(), (uint32_t)v.size()), 0);
  std::fill(v.begin(), v.end(), (int16_t)40);
  EXPECT_EQ(nr_llr_norm_shift(v.data(), (uint32_t)v.size()), 0);
  std::fill(v.begin(), v.end(), (int16_t)-41);
  EXPECT_EQ(nr_llr_norm_shift(v.data(), (uint32_t)v.size()), 1);
  std::fill(v.begin(), v.end(), (int16_t)450);
  EXPECT_EQ(nr_llr_norm_shift(v.data(), (uint32_t)v.size()), 4);
  EXPECT_EQ(nr_llr_norm_shift(v.data(), 0u), 0);
}

/* The K38 defect, kept as a guard: over all G the probe's zeros dilute the mean and the shift drops. */
TEST(LlrNorm, WholeBufferStatisticSeesTheProbeZeros)
{
  const auto full = full_llrs(1);
  const uint32_t C = nr_llr_norm_num_cb(kTbs, 1);
  ASSERT_GT(C, 1u);
  const auto probe = probe_llrs(full, C);
  EXPECT_GT(nr_llr_norm_shift(full.data(), kG), nr_llr_norm_shift(probe.data(), kG));
}

/* The fix: the statistic over code block 0's span gives the probe and the full decode the same shift,
 * hence bit-identical CB0 LLRs at the decoder input, for every code-block count. */
TEST(LlrNorm, ProbeAndFullGetTheSameShiftAndCb0)
{
  for (uint32_t seed = 1; seed <= 5; seed++) {
    const auto full = full_llrs(seed);
    for (uint32_t C = 2; C <= 80; C++) {
      const auto probe = probe_llrs(full, C);
      const uint32_t span = nr_llr_norm_span(kG, C);
      const int kf = nr_llr_norm_shift(full.data(), span);
      const int kp = nr_llr_norm_shift(probe.data(), span);
      ASSERT_EQ(kf, kp) << "seed " << seed << " C " << C;
      ASSERT_GE(kf, 3);
      for (uint32_t i = 0; i < span; i++)
        ASSERT_EQ(full[i] >> kf, probe[i] >> kp) << "seed " << seed << " C " << C << " i " << i;
    }
  }
}

/* TIMRANK34: the rank-3 equaliser input at TIM's receive level. |h| ~ 20 LSB on 4 branches, 3 layers, the third layer 9 dB weak
 * and the channel conditioned at ~20 dB, QPSK data, y = H s rounded to int16 (no other noise). The exact solve must return
 * every QPSK sign; the stock route (G and H^H y >> shift rounded to int16 first, shift = log2(|h|^2)/2 + 1) loses them. */
TEST(MmseExact, Rank3LowLevelIllConditioned)
{
  std::mt19937 rng(7);
  std::normal_distribution<double> g(0.0, 1.0);
  const int nl = 3, nrx = 4, shift = 5;
  const double gl[3] = {20.0, 18.0, 7.0};
  int bad_exact = 0, bad_stock = 0, n = 0;
  for (int t = 0; t < 2000; t++) {
    double hr[3][4], hi[3][4];
    for (int a = 0; a < nrx; a++) { /* layer 1 = 0.9 layer 0 + 0.44 fresh: correlated columns */
      const double c0r = g(rng), c0i = g(rng);
      hr[0][a] = gl[0] * c0r / 1.41; hi[0][a] = gl[0] * c0i / 1.41;
      hr[1][a] = gl[1] * (0.9 * c0r + 0.44 * g(rng)) / 1.41; hi[1][a] = gl[1] * (0.9 * c0i + 0.44 * g(rng)) / 1.41;
      hr[2][a] = gl[2] * g(rng) / 1.41; hi[2][a] = gl[2] * g(rng) / 1.41;
    }
    int16_t hre[4][4] = {}, him[4][4] = {}, yre[4] = {}, yim[4] = {};
    double sr[3], si[3];
    for (int l = 0; l < nl; l++) {
      sr[l] = (rng() & 1) ? 0.7071 : -0.7071; si[l] = (rng() & 1) ? 0.7071 : -0.7071;
      for (int a = 0; a < nrx; a++) { hre[l][a] = (int16_t)lround(hr[l][a]); him[l][a] = (int16_t)lround(hi[l][a]); }
    }
    for (int a = 0; a < nrx; a++) {
      double ar = 0, ai = 0;
      for (int l = 0; l < nl; l++) { ar += hre[l][a] * sr[l] - him[l][a] * si[l]; ai += hre[l][a] * si[l] + him[l][a] * sr[l]; }
      yre[a] = (int16_t)lround(ar); yim[a] = (int16_t)lround(ai);
    }
    double xre[4], xim[4], ainv[4], res = -1;
    ASSERT_TRUE(nr_mmse_exact_re(nl, nrx, hre, him, yre, yim, 1.0 / (1 << shift), 0.0, xre, xim, ainv, &res));
    EXPECT_GE(res, 0.0);
    /* stock route: same solve on the int16-rounded G and b (identity: pre-rounded inputs, sc = 1) is not expressible
     * through int16 h/y, so emulate it here in plain double */
    double Gr[3][3], Gi[3][3], br[3], bi[3];
    for (int r = 0; r < nl; r++) {
      double ur = 0, ui = 0;
      for (int c = 0; c < nl; c++) {
        double vr = 0, vi = 0;
        for (int a = 0; a < nrx; a++) { vr += hre[r][a] * hre[c][a] + him[r][a] * him[c][a]; vi += hre[r][a] * him[c][a] - him[r][a] * hre[c][a]; }
        Gr[r][c] = std::nearbyint(vr / (1 << shift)); Gi[r][c] = std::nearbyint(vi / (1 << shift));
      }
      for (int a = 0; a < nrx; a++) { ur += hre[r][a] * yre[a] + him[r][a] * yim[a]; ui += hre[r][a] * yim[a] - him[r][a] * yre[a]; }
      br[r] = std::nearbyint(ur / (1 << shift)); bi[r] = std::nearbyint(ui / (1 << shift));
    }
    /* Cramer-free: Gauss-Jordan on the rounded 3x3 */
    double Mr[3][4], Mi[3][4];
    for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) { Mr[r][c] = Gr[r][c]; Mi[r][c] = Gi[r][c]; } Mr[r][3] = br[r]; Mi[r][3] = bi[r]; }
    bool sing = false;
    for (int k = 0; k < 3 && !sing; k++) {
      int pv = k;
      for (int r = k + 1; r < 3; r++) if (std::hypot(Mr[r][k], Mi[r][k]) > std::hypot(Mr[pv][k], Mi[pv][k])) pv = r;
      if (std::hypot(Mr[pv][k], Mi[pv][k]) < 1e-9) { sing = true; break; }
      for (int c = 0; c < 4; c++) { std::swap(Mr[k][c], Mr[pv][c]); std::swap(Mi[k][c], Mi[pv][c]); }
      const double d = Mr[k][k] * Mr[k][k] + Mi[k][k] * Mi[k][k], ir = Mr[k][k] / d, ii = -Mi[k][k] / d;
      for (int c = 0; c < 4; c++) { const double a = Mr[k][c] * ir - Mi[k][c] * ii, b = Mr[k][c] * ii + Mi[k][c] * ir; Mr[k][c] = a; Mi[k][c] = b; }
      for (int r = 0; r < 3; r++) if (r != k) { const double fr = Mr[r][k], fi = Mi[r][k];
        for (int c = 0; c < 4; c++) { Mr[r][c] -= fr * Mr[k][c] - fi * Mi[k][c]; Mi[r][c] -= fr * Mi[k][c] + fi * Mr[k][c]; } }
    }
    for (int l = 0; l < nl; l++) {
      n += 2;
      bad_exact += (xre[l] > 0) != (sr[l] > 0);
      bad_exact += (xim[l] > 0) != (si[l] > 0);
      bad_stock += sing || (Mr[l][3] > 0) != (sr[l] > 0);
      bad_stock += sing || (Mi[l][3] > 0) != (si[l] > 0);
    }
  }
  std::printf("MmseExact rank 3, |h| ~ 20 LSB: sign errors exact %d / stock-int16 %d of %d\n", bad_exact, bad_stock, n);
  EXPECT_LT(bad_exact, n / 200) << "exact solve sign errors " << bad_exact << "/" << n;
  EXPECT_GT(bad_stock, n / 20) << "int16-rounded G: sign errors " << bad_stock << "/" << n;
}

/* the residual is the energy outside span(H); MMSE (s2 > 0) shrinks a layer by mu = 1 - s2 [A^-1]_ll exactly when the
 * columns are orthogonal */
TEST(MmseExact, ResidualAndUnbiasedScale)
{
  const int16_t hre[4][4] = {{100, 0, 0, 0}, {0, 50, 0, 0}}, him[4][4] = {};
  const int16_t yre[4] = {70, -35, 9, 0}, yim[4] = {-70, 35, 0, -12};
  double xre[4], xim[4], ainv[4], res = 0;
  ASSERT_TRUE(nr_mmse_exact_re(2, 4, hre, him, yre, yim, 1.0 / 64, 0.0, xre, xim, ainv, &res));
  EXPECT_NEAR(res, 9.0 * 9.0 + 12.0 * 12.0, 1e-6);
  EXPECT_NEAR(xre[0], 0.7, 1e-9); EXPECT_NEAR(xim[1], 0.7, 1e-9);
  const double s2 = 20.0;
  ASSERT_TRUE(nr_mmse_exact_re(2, 4, hre, him, yre, yim, 1.0 / 64, s2, xre, xim, ainv, nullptr));
  for (int l = 0; l < 2; l++) {
    const double mu = 1.0 - s2 * ainv[l];
    EXPECT_NEAR(xre[l] / mu, l == 0 ? 0.7 : -0.7, 1e-9);
  }
}

/* MMSE3GPU: synthetic 3- and 4-layer symbols, QPSK through a 4 RX channel at |h| ~ 20-120 LSB with known complex noise
 * variance 2 sigma^2 per RX. Mode 3 (nr_mmse3_symbol, the CPU path) must estimate s2 ~ 2 sigma^2 / 2^shift from the
 * residual (nl < nrx), decode the signs, and give positive per-layer thresholds. */
namespace {
struct M3Sym {
  int nl, nrx, len;
  std::vector<int16_t> y, h; /* rows of 2 * len int16: y[a], h[r * nrx + a] */
  std::vector<int> sr, si;   /* QPSK signs [r * len + i] */
};
M3Sym m3_make(int nl, int nrx, int len, double sigma, uint32_t seed)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> g(0.0, 1.0);
  M3Sym s{nl, nrx, len, std::vector<int16_t>(2 * nrx * len), std::vector<int16_t>(2 * nl * nrx * len), std::vector<int>(nl * len),
          std::vector<int>(nl * len)};
  const double lvl[4] = {120.0, 90.0, 40.0, 20.0};
  double base[4][4][2];
  for (int r = 0; r < nl; r++)
    for (int a = 0; a < nrx; a++) { base[r][a][0] = g(rng); base[r][a][1] = g(rng); }
  for (int i = 0; i < len; i++) {
    for (int r = 0; r < nl; r++) {
      s.sr[r * len + i] = (rng() & 1) ? 1 : -1;
      s.si[r * len + i] = (rng() & 1) ? 1 : -1;
    }
    const double ph = 0.02 * i; /* slow frequency rotation */
    for (int a = 0; a < nrx; a++) {
      double ar = 0.0, ai = 0.0;
      for (int r = 0; r < nl; r++) {
        const double c = std::cos(ph * (r + 1)), sn = std::sin(ph * (r + 1));
        const double hr = lvl[r] / 1.41 * (base[r][a][0] * c - base[r][a][1] * sn), hi = lvl[r] / 1.41 * (base[r][a][0] * sn + base[r][a][1] * c);
        const int16_t qr = (int16_t)lround(hr), qi = (int16_t)lround(hi);
        s.h[(r * nrx + a) * 2 * len + 2 * i] = qr;
        s.h[(r * nrx + a) * 2 * len + 2 * i + 1] = qi;
        const double xr = 0.7071 * s.sr[r * len + i], xi = 0.7071 * s.si[r * len + i];
        ar += qr * xr - qi * xi;
        ai += qr * xi + qi * xr;
      }
      s.y[a * 2 * len + 2 * i] = (int16_t)lround(ar + sigma * g(rng));
      s.y[a * 2 * len + 2 * i + 1] = (int16_t)lround(ai + sigma * g(rng));
    }
  }
  return s;
}
double m3_run(const M3Sym &s, int shift, uint32_t nvar, double nvfac, int16_t qa, int16_t qb, int16_t qr, std::vector<int16_t> &z,
              std::vector<int16_t> &mg)
{
  z.assign(2 * s.nl * s.len, 0);
  mg.assign(3 * 2 * s.nl * s.len, 0);
  const int16_t *yr[4], *hr[16];
  int16_t *zr[4], *m0[4], *m1[4], *m2[4];
  for (int a = 0; a < s.nrx; a++) yr[a] = &s.y[a * 2 * s.len];
  for (int r = 0; r < s.nl * s.nrx; r++) hr[r] = &s.h[r * 2 * s.len];
  for (int r = 0; r < s.nl; r++) {
    zr[r] = &z[r * 2 * s.len];
    m0[r] = &mg[(0 * s.nl + r) * 2 * s.len];
    m1[r] = &mg[(1 * s.nl + r) * 2 * s.len];
    m2[r] = &mg[(2 * s.nl + r) * 2 * s.len];
  }
  return nr_mmse3_symbol(s.nl, s.nrx, s.len, yr, hr, shift, nvar, nvfac, qa, qb, qr, zr, m0, m1, m2);
}
} // namespace

TEST(MmseExact, Mode3SymbolKnownNoise)
{
  for (int nl = 3; nl <= 4; nl++) {
    const double sigma = 4.0;
    const M3Sym s = m3_make(nl, 4, 600, sigma, 11 + nl);
    std::vector<int16_t> z, mg;
    const int shift = 6;
    const uint32_t nvar = 1; /* chest nvar, ~21 dB under the data noise (MMSE3CAL), so s2 ~ 4x 2 sigma^2 at nrx == nl */
    const double s2 = m3_run(s, shift, nvar, std::pow(10.0, 2.1), 20724, 0, 0, z, mg);
    const double want = 2.0 * sigma * sigma / (1 << shift);
    if (nl == 3) /* residual estimate over the one free dimension */
      EXPECT_NEAR(s2 / want, 1.0, 0.15) << "s2 " << s2 << " want " << want;
    else /* nrx == nl: nvar x 10^2.1 */
      EXPECT_NEAR(s2, (double)nvar / (1 << shift) * std::pow(10.0, 2.1), 1e-9);
    int bad = 0, n = 0, magpos = 0;
    for (int r = 0; r < nl; r++)
      for (int i = 0; i < s.len; i++) {
        bad += (z[r * 2 * s.len + 2 * i] > 0) != (s.sr[r * s.len + i] > 0);
        bad += (z[r * 2 * s.len + 2 * i + 1] > 0) != (s.si[r * s.len + i] > 0);
        magpos += mg[r * 2 * s.len + 2 * i] > 0;
        n += 2;
      }
    std::printf("MmseExact mode 3 nl=%d: s2 %.4f (2 sigma^2 / 2^shift %.4f), sign errors %d of %d, mag > 0 on %d of %d\n", nl, s2, want,
                bad, n, magpos, n / 2);
    EXPECT_LT(bad, nl == 3 ? n / 100 : n / 10); /* square 4x4 Rayleigh with a 20-LSB layer: ~5 % (weak layer), not a bug */
    EXPECT_EQ(magpos, n / 2);
  }
}

/* MMSE3GPU: the GPU mode 3 (libpdsch_gpu.so, ISAC_PDSCH_GPU_EXACT_DEMOD_MMSE3) against nr_mmse3_symbol, bit for bit, on a
 * synthetic 13-symbol grant per (Nl, Qm). Skipped without the module or a device (the module is built next to this test). */
#include <dlfcn.h>
#include "nr_pdsch_gpu_demod.h"
TEST(MmseExact, GpuMode3BitExact)
{
  if (!dlopen("./libnr_gpu_platform.so", RTLD_NOW | RTLD_GLOBAL)) /* the module's nr_gpu_* imports (the softmodem links it) */
    GTEST_SKIP() << "no ./libnr_gpu_platform.so: " << dlerror();
  void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!dl)
    GTEST_SKIP() << "no ./libpdsch_gpu.so: " << dlerror();
  auto init = (nr_gpu_demod_init_fn)dlsym(dl, "nr_pdsch_gpu_demod_init");
  auto run = (nr_gpu_demod_run_fn)dlsym(dl, "nr_pdsch_gpu_demod_run");
  auto rel = (nr_gpu_demod_release_fn)dlsym(dl, "nr_pdsch_gpu_demod_release");
  ASSERT_TRUE(init && run && rel);
  if (init(4) != 0)
    GTEST_SKIP() << "nr_pdsch_gpu_demod_init failed (no CUDA device?)";
  const int N = 512, nrx = 4, len = 288, ext = (len + 15) & ~15, buf = ext, first = 1, nsym = 13;
  long tot_bad = 0, tot = 0;
  for (int nl = 2; nl <= 4; nl++)
    for (int qm = 2; qm <= 8; qm += 2) {
      std::vector<int16_t> rx(2 * nrx * 14 * N), est(2 * nl * nrx * 14 * N), gy(2 * 14 * ext), gh(2 * 14 * ext);
      std::vector<M3Sym> sy;
      for (int m = first; m < first + nsym; m++) {
        sy.push_back(m3_make(nl, nrx, len, 3.0 + m % 4, 100 * nl + 10 * qm + m));
        const M3Sym &s = sy.back();
        for (int i = 0; i < len; i++) {
          for (int a = 0; a < nrx; a++)
            for (int c = 0; c < 2; c++)
              rx[2 * ((size_t)a * 14 * N + m * N + i) + c] = s.y[a * 2 * len + 2 * i + c];
          for (int r = 0; r < nl * nrx; r++)
            for (int c = 0; c < 2; c++)
              est[2 * ((size_t)r * 14 * N + m * N + i) + c] = s.h[r * 2 * len + 2 * i + c];
          gy[2 * (m * ext + i)] = (int16_t)i;
          gy[2 * (m * ext + i) + 1] = 1;
          gh[2 * (m * ext + i)] = (int16_t)i;
          gh[2 * (m * ext + i) + 1] = (int16_t)(m + 1);
        }
      }
      const size_t bn = (size_t)2 * 14 * NR_GPU_DEMOD_MAX_NL * buf;
      std::vector<int16_t> comp(bn), mag(bn), magb(bn), magr(bn), llr((size_t)nsym * len * qm * nl);
      nr_gpu_demod_job_t j = {};
      j.nbRx = nrx; j.nl = nl; j.qm = qm; j.N = N;
      j.start_symbol = first; j.nb_symbols = nsym; j.first_symbol = first;
      j.rx_stride = 14 * N; j.est_stride = 14 * N; j.buf = buf; j.ext = ext;
      uint32_t off = 0;
      for (int m = first; m < first + nsym; m++) {
        j.sym_mask |= (uint16_t)(1u << m);
        j.nre[m] = len;
        j.llr_off[m] = off;
        off += len * qm * nl;
      }
      j.rxdataF = rx.data(); j.est = est.data(); j.geo_y = gy.data(); j.geo_h = gh.data();
      j.comp = comp.data(); j.mag = mag.data(); j.magb = magb.data(); j.magr = magr.data(); j.llr = llr.data();
      j.thr_coef = std::pow(10.0, -1.2);
      j.qam_mode = 3; j.mag_per_layer = 1; j.nvar = 40; j.m3_nvfac = std::pow(10.0, 2.1);
      ASSERT_EQ(run(&j), 0);
      rel(j.ctx);
      long bad = 0;
      for (int m = first; m < first + nsym; m++) {
        const int16_t qa = qm == 4 ? 20724 : qm == 6 ? 20225 : qm == 8 ? 20106 : 0, qb = qm == 6 ? 10112 : qm == 8 ? 10053 : 0,
                      qr = qm == 8 ? 5026 : 0; /* QAM16_n1 ... QAM256_n3 */
        std::vector<int16_t> z, mg;
        m3_run(sy[m - first], j.log2_maxh, j.nvar, j.m3_nvfac, qa, qb, qr, z, mg);
        for (int r = 0; r < nl; r++)
          for (int i = 0; i < 2 * len; i++) {
            const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * buf * 2 + i;
            bad += comp[row] != z[r * 2 * len + i];
            bad += mag[row] != mg[(0 * nl + r) * 2 * len + i];
            bad += magb[row] != mg[(1 * nl + r) * 2 * len + i];
            bad += magr[row] != mg[(2 * nl + r) * 2 * len + i];
            tot += 4;
          }
      }
      if (bad)
        std::printf("GPU mode 3 nl=%d qm=%d shift %d: %ld mismatching int16 outputs\n", nl, qm, j.log2_maxh, bad);
      tot_bad += bad;
    }
  std::printf("GPU mode 3 vs CPU nr_mmse3_symbol: %ld of %ld int16 outputs differ\n", tot_bad, tot);
  EXPECT_EQ(tot_bad, 0);
}
