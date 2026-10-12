/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/* test_nr_pss_decim: decimated float PSS correlator (ISAC_PSS_DECIM) and the multi-capture fold (ISAC_PSS_ACC).
 * Synthetic NR PSS (TS 38.211 7.4.2.2) placed off-centre in a 4096-point grid, complex AWGN, 4 RX. */
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
#include "nr_pss_decim.h"

namespace {
const int N = 4096, D = 16, NANT = 4;
const int KC0 = -1500; /* PSS centre bin (signed), i.e. an SSB 45 MHz below DC at 30 kHz */

std::vector<int16_t> pss_time(int nid2, double amp)
{
  int x[127 + 7] = {0, 1, 1, 0, 1, 1, 1};
  for (int i = 0; i < 127; i++)
    x[i + 7] = (x[i + 4] + x[i]) % 2;
  std::vector<double> re(N), im(N);
  for (int k = 0; k < 127; k++) {
    const int d = 1 - 2 * x[(k + 43 * nid2) % 127];
    const int bin = KC0 - 63 + k;
    for (int n = 0; n < N; n++) {
      const double a = 2 * M_PI * bin * n / N;
      re[n] += d * cos(a);
      im[n] += d * sin(a);
    }
  }
  std::vector<int16_t> o(2 * N);
  for (int n = 0; n < N; n++) {
    o[2 * n] = (int16_t)lround(re[n] * amp);
    o[2 * n + 1] = (int16_t)lround(im[n] * amp);
  }
  return o;
}

/* one capture of len samples per antenna: noise sigma per component + PSS (time ref scaled) at pos with random phase */
std::vector<std::vector<int16_t>> capture(int len, int pos, double sig, double sigma, std::mt19937 &g)
{
  std::normal_distribution<double> nd(0, sigma);
  std::uniform_real_distribution<double> ph(0, 2 * M_PI);
  const auto ref = pss_time(1, 1.0);
  std::vector<std::vector<int16_t>> c(NANT, std::vector<int16_t>(2 * (len + N)));
  for (int a = 0; a < NANT; a++) {
    const double p = ph(g), cr = cos(p) * sig, ci = sin(p) * sig;
    for (int n = 0; n < len + N; n++) {
      double r = nd(g), i = nd(g);
      if (n >= pos && n < pos + N && sig > 0) {
        const double sr = ref[2 * (n - pos)], si = ref[2 * (n - pos) + 1];
        r += sr * cr - si * ci;
        i += sr * ci + si * cr;
      }
      c[a][2 * n] = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(r)));
      c[a][2 * n + 1] = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(i)));
    }
  }
  return c;
}

std::vector<float> profile(const std::vector<std::vector<int16_t>> &c, int len, const std::vector<int16_t> &ref, int s0 = 0)
{
  const int kc = KC0;
  std::vector<nr_cf_t> rd(N / D + 3);
  const int L = nr_pss_decim_ref(ref.data(), N, D, kc, rd.data());
  const int npos = len / D;
  std::vector<float> prof(npos, 0.f);
  std::vector<nr_cf_t> xd(npos + L);
  for (const auto &a : c) {
    nr_pss_decimate(a.data(), len + N, s0, npos + L, D, kc, N, xd.data());
    nr_pss_corr_acc(xd.data(), npos, rd.data(), L, prof.data());
  }
  return prof;
}
} // namespace

TEST(PssDecim, FactorAndDcBiasedReference)
{
  EXPECT_EQ(nr_pss_decim_factor(4096), 16);
  EXPECT_EQ(nr_pss_decim_factor(256), 0);
  /* the int16 IDFT reference of the receiver has a large DC bias (TIM 923 ssb 233); it must not move the peak */
  std::mt19937 g(5);
  const int len = 61440, pos = 11000;
  const auto c = capture(len, pos, 1.0, 20.0, g);
  auto ref = pss_time(1, 1.0);
  for (int n = 0; n < N; n++) {
    ref[2 * n] += 40;
    ref[2 * n + 1] += 40;
  }
  const auto pr = profile(c, len, ref);
  const int m = (int)(std::max_element(pr.begin(), pr.end()) - pr.begin());
  EXPECT_NEAR(m * D, pos, 2 * D);
}

TEST(PssDecim, PeakAtTruePositionAndSequence)
{
  std::mt19937 g(1);
  const int len = 61440, pos = 23456;
  const auto c = capture(len, pos, 1.0, 20.0, g);
  int best_s = -1, best_m = -1;
  float best = -1;
  for (int s = 0; s < 3; s++) {
    const auto pr = profile(c, len, pss_time(s, 1.0));
    const int m = (int)(std::max_element(pr.begin(), pr.end()) - pr.begin());
    if (pr[m] > best) {
      best = pr[m];
      best_s = s;
      best_m = m;
    }
  }
  EXPECT_EQ(best_s, 1);
  EXPECT_LE(std::abs(best_m * D - pos), D);
}

/* Weak PSS: detection = profile argmax within +-1 decimated sample of the truth (SSS/PBCH then verify). The same trial is
 * scored after 1 capture (K=1) and after 8 folded captures (K=8, fresh noise and channel phase each capture). Prints the
 * per-sample SNR at 90 % detection for both, interpolated between 1 dB steps: the sensitivity gain of the fold. */
TEST(PssDecim, FoldSensitivityGain)
{
  const int len = 61440, pos = 20000, trials = 20, K = 8;
  const auto ref = pss_time(1, 1.0);
  const double sigma = 200.0; /* per component; |ref| rms = sqrt(127) */
  std::vector<double> snrs, p1, pk;
  for (double snr_db = -40; snr_db <= -20; snr_db += 1) {
    int h1 = 0, hk = 0;
    std::mt19937 g(1000 + (int)(snr_db * 10));
    const double sig = sigma * std::sqrt(2.0) * std::pow(10, snr_db / 20) / std::sqrt(127.0);
    for (int t = 0; t < trials; t++) {
      nr_pss_fold_t f = {};
      for (int k = 0; k < K; k++) {
        auto pr = profile(capture(len, pos, sig, sigma, g), len, ref);
        nr_pss_fold_update(&f, 1, (int64_t)k * 2 * (len / D), pr.data(), (int)pr.size(), 2 * (len / D), K, 7);
        const int m = (int)(std::max_element(pr.begin(), pr.end()) - pr.begin());
        const bool hit = std::abs(m - pos / D) <= 1;
        if (k == 0)
          h1 += hit;
        if (k == K - 1)
          hk += hit;
      }
      nr_pss_fold_reset(&f);
    }
    snrs.push_back(snr_db);
    p1.push_back((double)h1 / trials);
    pk.push_back((double)hk / trials);
    if (h1 == trials)
      break;
  }
  auto at90 = [&](const std::vector<double> &p) {
    for (size_t i = 1; i < p.size(); i++)
      if (p[i] >= 0.9 && p[i - 1] < 0.9)
        return snrs[i - 1] + (0.9 - p[i - 1]) / (p[i] - p[i - 1]) * (snrs[i] - snrs[i - 1]);
    return p.size() && p[0] >= 0.9 ? snrs[0] : 99.0;
  };
  const double s1 = at90(p1), sk = at90(pk);
  for (size_t i = 0; i < snrs.size(); i++)
    printf("PSS_DECIM snr %.0f dB: K=1 %.2f K=%d %.2f\n", snrs[i], p1[i], K, pk[i]);
  printf("PSS_DECIM sensitivity (per-sample SNR, 4 RX, 0.5 ms window, 90%% detection): K=1 %.1f dB, K=%d %.1f dB, gain %.1f dB\n",
         s1, K, sk, s1 - sk);
  EXPECT_LT(s1, 99);
  EXPECT_LT(sk, s1 - 1.0); /* the fold must buy sensitivity */
}

TEST(PssDecim, FoldResetsOnKeyChangeAndBackwardsTime)
{
  nr_pss_fold_t f = {};
  float a[4] = {1, 1, 1, 1};
  nr_pss_fold_update(&f, 0, 0, a, 4, 8, 4, 1);
  float b[4] = {1, 1, 1, 1};
  nr_pss_fold_update(&f, 0, 8, b, 4, 8, 4, 1);
  EXPECT_FLOAT_EQ(b[0], 1.75f); /* 1*(1-1/4) + 1 */
  float c[4] = {1, 1, 1, 1};
  nr_pss_fold_update(&f, 0, 16, c, 4, 8, 4, 2); /* new key -> fresh */
  EXPECT_FLOAT_EQ(c[0], 1.0f);
  float d[4] = {1, 1, 1, 1};
  nr_pss_fold_update(&f, 0, 0, d, 4, 8, 4, 2); /* stream restarted (time went back > period) -> fresh */
  EXPECT_FLOAT_EQ(d[0], 1.0f);
  nr_pss_fold_reset(&f);
}
