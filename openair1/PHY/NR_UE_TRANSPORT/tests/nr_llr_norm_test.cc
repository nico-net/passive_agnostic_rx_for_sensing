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
