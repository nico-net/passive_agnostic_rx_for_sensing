// D4 IRC (2026-10-31): DM-RS covariance whitening in front of the mode-3 MMSE (nr_mmse_exact.h, nr_irc_final / nr_mmse3_symbol_w).
// A 4 RX channel with one co-channel interferer of fixed spatial signature per 8-PRB group; the covariance sums are built the way the
// chest does it, from LS-residual vectors (quantised to int16). Spec: the whitened receiver beats the scalar-noise MMSE in SINR,
// loses < 0.2 dB when the noise is white, and falls back to the scalar model when the covariance is not usable.
#include <gtest/gtest.h>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
extern "C" {
#include "nr_mmse_exact.h"
}

namespace {
typedef std::complex<double> cd;

struct Trial {
  long bits = 0, errs = 0;
};

double q_inv_sinr(double ber) /* QPSK: BER = Q(sqrt(SINR)) per dimension; returns SINR in dB */
{
  double lo = 0.0, hi = 60.0;
  for (int k = 0; k < 80; k++) {
    const double m = 0.5 * (lo + hi);
    const double b = 0.5 * std::erfc(std::sqrt(m) / std::sqrt(2.0));
    (b > ber ? lo : hi) = m;
  }
  return 10.0 * std::log10(0.5 * (lo + hi));
}

// groups of 96 REs; inr_db < -100: no interferer. n_resid residual vectors per group, scaled by resid_amp relative to the data-domain noise.
void run(int nl, int nrx, double sigma, double inr_db, int n_resid, double resid_amp, bool use_irc, int groups, uint32_t seed, Trial &t, double *gain_db = nullptr,
         uint32_t nvar = 1)
{
  std::mt19937 rng(seed);
  std::normal_distribution<double> g(0.0, 1.0);
  const int len = 96;
  double gsum = 0.0;
  for (int gi = 0; gi < groups; gi++) {
    cd H[4][4], a[4];
    for (int r = 0; r < nl; r++)
      for (int k = 0; k < nrx; k++)
        H[r][k] = cd(g(rng), g(rng)) * (60.0 / std::sqrt(2.0));
    double an = 0;
    for (int k = 0; k < nrx; k++) { a[k] = cd(g(rng), g(rng)); an += std::norm(a[k]); }
    const bool has_i = inr_db > -100.0;
    const double iamp = sigma * std::pow(10.0, inr_db / 20.0) / std::sqrt(an / nrx); // per-RX interference power = INR x noise
    // spatial noise draw: n + a * b
    auto draw = [&](double scale) {
      cd v[4];
      const cd b(g(rng), g(rng));
      for (int k = 0; k < nrx; k++)
        v[k] = scale * (cd(g(rng), g(rng)) * (sigma / std::sqrt(2.0)) + (has_i ? iamp * a[k] * b / std::sqrt(2.0) : cd(0)));
      return std::vector<cd>(v, v + nrx);
    };
    nr_irc_sum_t s;
    std::memset(&s, 0, sizeof(s));
    for (int q = 0; q < n_resid; q++) {
      const std::vector<cd> e = draw(resid_amp);
      long long er[4], ei[4];
      for (int k = 0; k < nrx; k++) { er[k] = std::lround(e[k].real()); ei[k] = std::lround(e[k].imag()); }
      for (int x = 0; x < nrx; x++)
        for (int y = 0; y < nrx; y++) {
          s.re[x][y] += er[x] * er[y] + ei[x] * ei[y];
          s.im[x][y] += ei[x] * er[y] - er[x] * ei[y];
        }
      s.n++;
    }
    double w[NR_IRC_WSTRIDE];
    nr_irc_final(&s, nrx, w);
    gsum += w[22];
    std::vector<int16_t> y(nrx * 2 * len), h(nl * nrx * 2 * len);
    std::vector<int> sr(nl * len), si(nl * len);
    for (int i = 0; i < len; i++) {
      cd x[4];
      for (int r = 0; r < nl; r++) {
        sr[r * len + i] = (rng() & 1) ? 1 : -1;
        si[r * len + i] = (rng() & 1) ? 1 : -1;
        x[r] = cd(sr[r * len + i], si[r * len + i]) * 0.7071;
      }
      const std::vector<cd> n = draw(1.0);
      for (int k = 0; k < nrx; k++) {
        cd v = n[k];
        for (int r = 0; r < nl; r++) {
          const int16_t hr = (int16_t)std::lround(H[r][k].real()), hi = (int16_t)std::lround(H[r][k].imag());
          h[(r * nrx + k) * 2 * len + 2 * i] = hr;
          h[(r * nrx + k) * 2 * len + 2 * i + 1] = hi;
          v += cd(hr, hi) * x[r];
        }
        y[k * 2 * len + 2 * i] = (int16_t)std::lround(v.real());
        y[k * 2 * len + 2 * i + 1] = (int16_t)std::lround(v.imag());
      }
    }
    std::vector<int16_t> z(2 * nl * len), m0(2 * nl * len), m1(2 * nl * len), m2(2 * nl * len);
    const int16_t *yr[4], *hr[16];
    int16_t *zr[4], *p0[4], *p1[4], *p2[4];
    for (int k = 0; k < nrx; k++) yr[k] = &y[k * 2 * len];
    for (int r = 0; r < nl * nrx; r++) hr[r] = &h[r * 2 * len];
    for (int r = 0; r < nl; r++) { zr[r] = &z[r * 2 * len]; p0[r] = &m0[r * 2 * len]; p1[r] = &m1[r * 2 * len]; p2[r] = &m2[r * 2 * len]; }
    nr_mmse3_symbol_w(nl, nrx, len, yr, hr, 6, nvar, std::pow(10.0, 2.1), 20724, 0, 0, zr, p0, p1, p2, use_irc ? w : nullptr, len);
    for (int r = 0; r < nl; r++)
      for (int i = 0; i < len; i++) {
        t.errs += (z[r * 2 * len + 2 * i] > 0) != (sr[r * len + i] > 0);
        t.errs += (z[r * 2 * len + 2 * i + 1] > 0) != (si[r * len + i] > 0);
        t.bits += 2;
      }
  }
  if (gain_db) *gain_db = 10.0 * std::log10(gsum / groups);
}
} // namespace

TEST(Irc, InterfererGainOverScalarMmse)
{
  for (int nl = 1 + 1; nl <= 3; nl++) {
    Trial a, b;
    double est = 0;
    run(nl, 4, 14.0, 10.0, 48, 0.4, false, 400, 5 + nl, a);
    run(nl, 4, 14.0, 10.0, 48, 0.4, true, 400, 5 + nl, b, &est);
    const double ba = (double)a.errs / a.bits, bb = (double)b.errs / b.bits;
    const double gain = q_inv_sinr(bb) - q_inv_sinr(ba);
    std::printf("Irc nl=%d INR 10 dB: BER scalar %.4f, IRC %.4f -> SINR gain %.2f dB (estimate %.2f dB)\n", nl, ba, bb, gain, est);
    EXPECT_GT(gain, 2.0);
    EXPECT_GT(est, 1.0);
  }
}

TEST(Irc, WhiteNoiseNoLoss)
{
  for (int nl = 2; nl <= 3; nl++)
    for (int n : {24, 48, 96}) {
      Trial a, b;
      double est = 0;
      run(nl, 4, 40.0, -200.0, n, 0.4, false, 600, 21 + nl, a);
      run(nl, 4, 40.0, -200.0, n, 0.4, true, 600, 21 + nl, b, &est);
      const double ba = (double)a.errs / a.bits, bb = (double)b.errs / b.bits;
      const double loss = q_inv_sinr(ba) - q_inv_sinr(bb);
      std::printf("Irc nl=%d white noise n=%d: BER scalar %.4f, IRC %.4f -> loss %.3f dB (estimate %.3f dB)\n", nl, n, ba, bb, loss, est);
      EXPECT_LT(loss, 0.2);
    }
}

TEST(Irc, FallbackToScalar)
{
  double w[NR_IRC_WSTRIDE];
  nr_irc_sum_t s;
  std::memset(&s, 0, sizeof(s));
  nr_irc_final(&s, 4, w); // no samples
  EXPECT_EQ(w[20], 0.0);
  s.n = 100; // zero covariance: no energy
  nr_irc_final(&s, 4, w);
  EXPECT_EQ(w[20], 0.0);
  for (int a = 0; a < 4; a++) s.re[a][a] = 100 * 50;
  s.n = 3; // fewer samples than 2 nrx
  nr_irc_final(&s, 4, w);
  EXPECT_EQ(w[20], 0.0);
  for (int a = 0; a < 4; a++) EXPECT_EQ(w[NR_IRC_TRI(a, a)], 1.0);
  s.n = 100;
  nr_irc_final(&s, 4, w);
  EXPECT_EQ(w[20], 1.0);
  nr_irc_final(&s, 1, w); // one branch: nothing to whiten
  EXPECT_EQ(w[20], 0.0);
}

TEST(Irc, WeightedPathIsScalarWhenInactive)
{
  // an inactive group (identity) must reproduce nr_mmse3_symbol bit for bit
  std::mt19937 rng(3);
  const int len = 96, nl = 2, nrx = 4;
  std::vector<int16_t> y(nrx * 2 * len), h(nl * nrx * 2 * len);
  for (auto &v : y) v = (int16_t)((int)(rng() % 400) - 200);
  for (auto &v : h) v = (int16_t)((int)(rng() % 200) - 100);
  const int16_t *yr[4], *hr[16];
  for (int k = 0; k < nrx; k++) yr[k] = &y[k * 2 * len];
  for (int r = 0; r < nl * nrx; r++) hr[r] = &h[r * 2 * len];
  std::vector<int16_t> o[2][4]; // [variant][comp, mag, magb, magr]
  int16_t *ptr[2][4][4];
  for (int v = 0; v < 2; v++)
    for (int c = 0; c < 4; c++) {
      o[v][c].assign(nl * 2 * len, 0);
      for (int r = 0; r < nl; r++) ptr[v][c][r] = &o[v][c][r * 2 * len];
    }
  const double s2a = nr_mmse3_symbol(nl, nrx, len, yr, hr, 6, 1, 125.9, 20724, 0, 0, ptr[0][0], ptr[0][1], ptr[0][2], ptr[0][3]);
  double w[NR_IRC_WSTRIDE];
  nr_irc_sum_t s;
  std::memset(&s, 0, sizeof(s));
  nr_irc_final(&s, nrx, w);
  const double s2b = nr_mmse3_symbol_w(nl, nrx, len, yr, hr, 6, 1, 125.9, 20724, 0, 0, ptr[1][0], ptr[1][1], ptr[1][2], ptr[1][3], w, len);
  EXPECT_EQ(s2a, s2b);
  for (int c = 0; c < 4; c++) EXPECT_EQ(o[0][c], o[1][c]);
}

// nrx == nl (rank 4 on 4 RX): no noise-only dimension, s2 = nvar x nvfac x the groups' noise_fac. White: no loss; with an interferer the whitening still helps.
TEST(Irc, SquareChannelRank4)
{
  for (double inr : {-200.0, 10.0}) {
    const double sigma = inr < -100 ? 12.0 : 16.0;
    const uint32_t nvar = (uint32_t)std::lround(2.0 * sigma * sigma / std::pow(10.0, 2.1)); /* the calibrated s2 = 2 sigma^2 / 2^shift */
    Trial a, b;
    double est = 0;
    run(4, 4, sigma, inr, 48, 0.4, false, 600, 91, a, nullptr, nvar);
    run(4, 4, sigma, inr, 48, 0.4, true, 600, 91, b, &est, nvar);
    const double ba = (double)a.errs / a.bits, bb = (double)b.errs / b.bits;
    const double gain = q_inv_sinr(bb) - q_inv_sinr(ba);
    std::printf("Irc nl=4 (square) INR %.0f dB: BER scalar %.4f, IRC %.4f -> SINR gain %.2f dB (estimate %.2f dB)\n", inr, ba, bb, gain, est);
    if (inr < -100)
      EXPECT_GT(gain, -0.2);
    else
      EXPECT_GT(gain, 0.5);
  }
}
