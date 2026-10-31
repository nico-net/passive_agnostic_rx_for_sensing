#include <cmath>
#include <gtest/gtest.h>
#include <cstring>
#include <random>
#include <complex>
#include <vector>
#include "nr_pdsch_gpu_demod.cuh"
extern "C" {
#include "nr_mrc_weights.h"
}
TEST(MrcWeights, EqualNoiseIsPlainMrc) {
  const uint32_t nv[4] = {100, 100, 100, 100};
  double w[4];
  EXPECT_EQ(nr_mrc_noise_weights(nv, 4, w), 4);
  for (double x : w) EXPECT_DOUBLE_EQ(x, 1.0);
  EXPECT_EQ(nr_mrc_effective_branches(w, 4), 4);
}
TEST(MrcWeights, TheMeasuredRigSilencesTheNoisyBranches) {
  // chest nvar measured OTA 2026-09-14 (brdiag): branch 0 clean, branches 1-3 up to 37 dB noisier.
  const uint32_t nv[4] = {57, 23000, 281559, 6314};
  double w[4];
  nr_mrc_noise_weights(nv, 4, w);
  EXPECT_DOUBLE_EQ(w[0], 1.0);
  EXPECT_LT(w[1] * w[1], 0.01);   // < -20 dB
  EXPECT_LT(w[2] * w[2], 0.001);  // < -30 dB
  EXPECT_LT(w[3] * w[3], 0.01);
  // the combined output needs no more headroom than one branch
  EXPECT_EQ(nr_mrc_effective_branches(w, 4), 2); // ceil(1 + ~0.02) -- never charges 2 bits for 4 branches
}
TEST(MrcWeights, CombinedSnrIsNeverBelowTheBestBranch) {
  // SNR of the weighted sum with per-branch signal power P_a and noise N_a, weights w_a^2 = Nmin/N_a:
  // (sum w_a^2 P_a)^2 / (sum w_a^4 P_a N_a) -- must be >= max_a P_a/N_a for MRC (equality at 1 branch).
  const uint32_t nv[4] = {57, 23000, 281559, 6314};
  const double P[4] = {1000.0, 800.0, 6000.0, 300.0};
  double w[4];
  nr_mrc_noise_weights(nv, 4, w);
  double num = 0, den = 0, best = 0;
  for (int a = 0; a < 4; a++) {
    const double g = w[a] * w[a];
    num += g * P[a]; den += g * g * P[a] * nv[a];
    best = std::max(best, P[a] / nv[a]);
  }
  EXPECT_GE(num * num / den, best * 0.999);
}
TEST(MrcWeights, ABranchWithoutAnEstimateGetsZero) {
  const uint32_t nv[4] = {0, 200, 0, 0};
  double w[4];
  EXPECT_EQ(nr_mrc_noise_weights(nv, 4, w), 1);
  EXPECT_EQ(w[0], 0.0); EXPECT_EQ(w[1], 1.0);
}

TEST(MrcWeights, FourBranchSumDoesNotSaturateBeforeCancellation) {
  EXPECT_EQ(nr_mrc_average4(30000,30000,-30000,-30000),0);
  EXPECT_EQ(nr_mrc_average4(30000,-30000,30000,-30000),0);
  EXPECT_EQ(nr_mrc_average4(32767,32767,32767,32767),32767);
  EXPECT_EQ(nr_mrc_average4(-32768,-32768,-32768,-32768),-32768);
  EXPECT_EQ(nr_mrc_average4(0,0,0,12000),3000);
}

/* ---- RANK1COMB: per-branch noise-weighted MRC of the integer rank-1 combiner (nr_mrc_noise_wq15, its device twin nrd_noise_wq, nrd_decide) ---- */
TEST(MrcNoiseWq, EqualNoiseIsNeutral) {
  const uint32_t nv[4] = {500, 500, 500, 500};
  uint16_t wq[4];
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xF, nv, 1200, wq), 0);
  for (uint16_t w : wq) EXPECT_EQ(w, 32768);
}
TEST(MrcNoiseWq, UnequalNoiseWeightsAreSqrtOfTheRatio) {
  const uint32_t nv[4] = {100, 200, 400, 1600}; /* 0 / 3 / 6 / 12 dB, 100 PRB: the shrinkage toward the mean is 0.7 % */
  uint16_t wq[4];
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xF, nv, 1200, wq), 1);
  EXPECT_EQ(wq[0], 32768);
  EXPECT_NEAR(wq[1], 23170, 300); /* 32768 / sqrt(2) */
  EXPECT_NEAR(wq[2], 16384, 300);
  EXPECT_NEAR(wq[3], 8192, 300);
  EXPECT_GE(wq[1], 23170 - 1); EXPECT_GE(wq[3], 8192 - 1); /* shrinkage only pulls the weights toward 1 */
}
TEST(MrcNoiseWq, EstimationNoiseAloneGivesNoWeightsOnASmallGrant) {
  /* branches whose estimates differ by +-10 %: the relative variance of one estimate is c / p = 0.05 at 10 PRB */
  const uint32_t nv[4] = {100, 112, 90, 105};
  uint16_t wq[4];
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xF, nv, 120, wq), 0);
  for (uint16_t w : wq) EXPECT_EQ(w, 32768);
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xF, nv, 12000, wq), 1); /* 1000 PRB: the same spread is significant */
  EXPECT_LT(wq[1], 32768);
}
TEST(MrcNoiseWq, TheReferenceIsTheLeastNoisyLiveBranch) {
  /* branch 0 is a dead (noise-floor) branch with a tiny residual: the live mask excludes it and it must not shrink the others */
  const uint32_t nv[4] = {5, 100, 400, 100};
  uint16_t wq[4];
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xE, nv, 1200, wq), 1);
  EXPECT_EQ(wq[0], 0);
  EXPECT_EQ(wq[1], 32768);
  EXPECT_NEAR(wq[2], 16384, 400);
  EXPECT_EQ(wq[3], 32768);
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0x2, nv, 1200, wq), 0); /* one live branch: nothing to weight */
}
TEST(MrcNoiseWq, ALiveBranchWithoutAnEstimateOrNoNoiseArrayIsNeutral) {
  const uint32_t nv[4] = {0, 200, 100, 100};
  uint16_t wq[4];
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xF, nv, 1200, wq), 0);
  for (uint16_t w : wq) EXPECT_EQ(w, 32768);
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xF, NULL, 1200, wq), 0);
  EXPECT_EQ(nr_mrc_noise_wq15(4, 0xE, nv, 1200, wq), 1); /* the estimate-less branch is masked: the others are weighted */
}
TEST(MrcNoiseWq, DeviceTwinEqualsTheCpuFunction) {
  std::mt19937 rng(7);
  for (int t = 0; t < 20000; t++) {
    uint32_t nv[4];
    for (int a = 0; a < 4; a++) nv[a] = (rng() % 11 == 0) ? 0 : 1 + (rng() % (1u << (1 + rng() % 24)));
    const int live = 1 + rng() % 15;
    const uint32_t nre = 12 * (rng() % 300);
    uint16_t w1[4], w2[4];
    const int r1 = nr_mrc_noise_wq15(4, live, nv, nre, w1), r2 = nrd_noise_wq(4, live, nv, nre, w2);
    ASSERT_EQ(r1, r2);
    for (int a = 0; a < 4; a++) ASSERT_EQ(w1[a], w2[a]);
  }
}
TEST(MrcNoiseWq, ScaleIsRoundedQ15AndIdentityAtOne) {
  EXPECT_EQ(nr_mrc_scale_q15(12345, 32768), 12345);
  EXPECT_EQ(nr_mrc_scale_q15(-12345, 32768), -12345);
  EXPECT_EQ(nr_mrc_scale_q15(1000, 16384), 500);
  EXPECT_EQ(nr_mrc_scale_q15(-32768, 32768), -32768);
  EXPECT_EQ(nr_mrc_scale_q15(32767, 0), 0);
  nrd_c16 v = {1001, -999}, o = nrd_scale_q15(v, 23170);
  EXPECT_EQ(o.r, nr_mrc_scale_q15(1001, 23170));
  EXPECT_EQ(o.i, nr_mrc_scale_q15(-999, 23170));
}
TEST(MrcNoiseWq, HeadroomTwinAndWeightedEnergyRule) {
  const int32_t avg[4] = {1000, 1000, 900, 800};
  const uint32_t nv[4] = {100, 100, 400, 100};
  nrd_decision d;
  const int32_t med[16] = {0};
  nrd_decide(1, 4, 3, std::pow(10.0, -1.2), 0, 0, avg, med, nv, 1200, &d);
  ASSERT_EQ(d.wactive, 1);
  int32_t avgs;
  int contributing;
  nr_mrc_weighted_headroom(4, d.live_mask, avg, d.wq, &avgs, &contributing);
  EXPECT_EQ(avgs, 1000);
  EXPECT_EQ(contributing, 3); /* 1000 + 1000 + 900/4 + 800 = 3025 -> round(3.025) */
  EXPECT_EQ(d.log2_maxh, (nrd_log2_approx((uint32_t)avgs) >> 1) + 1 + nrd_log2_approx((uint32_t)contributing));
  /* equal noise: the former decision, bit for bit */
  const uint32_t nve[4] = {7, 7, 7, 7};
  nrd_decision d0, d1;
  nrd_decide(1, 4, 3, std::pow(10.0, -1.2), 0, 0, avg, med, nve, 1200, &d0);
  nrd_decide(1, 4, 3, std::pow(10.0, -1.2), 0, 0, avg, med, NULL, 1200, &d1);
  EXPECT_EQ(d0.wactive, 0);
  EXPECT_EQ(d0.log2_maxh, d1.log2_maxh);
  EXPECT_EQ(d0.live_mask, d1.live_mask);
}
/* The integer combiner on scaled samples is the noise-weighted MRC: post-combining SNR (measured on the fixed-point output) against theory, and against the unweighted sum. */
TEST(MrcNoiseWq, CombinerSnrFollowsSumOfInverseNoise) {
  std::mt19937 rng(11);
  std::normal_distribution<double> G(0, 1);
  const int n = 6000, shift = 9;
  const double sig[4] = {1.0, 2.0, 4.0, 8.0}; /* per-branch noise std (0, 6, 12, 18 dB power) in units of the 1000-amplitude channel */
  uint32_t nv[4];
  for (int a = 0; a < 4; a++) nv[a] = (uint32_t)(sig[a] * sig[a] * 100); /* what the chest residual would measure, up to a common factor */
  uint16_t wq[4];
  ASSERT_EQ(nr_mrc_noise_wq15(4, 0xF, nv, 1200, wq), 1);
  std::vector<nrd_c16> y[2][4], h[2][4], comp[2];
  double snr[2];
  for (int variant = 0; variant < 2; variant++) {
    for (int a = 0; a < 4; a++) {
      y[variant][a].resize(n);
      h[variant][a].resize(n);
    }
    comp[variant].resize(n);
  }
  std::vector<std::complex<double>> xs(n);
  for (int i = 0; i < n; i++) xs[i] = {(rng() & 1) ? 1.0 : -1.0, (rng() & 1) ? 1.0 : -1.0};
  for (int a = 0; a < 4; a++)
    for (int i = 0; i < n; i++) {
      const std::complex<double> hh(1000, 0), nz(G(rng) * sig[a] * 150, G(rng) * sig[a] * 150), yy = hh * xs[i] * 2.0 + nz;
      for (int v = 0; v < 2; v++) {
        nrd_c16 yv = {(int16_t)std::lround(yy.real()), (int16_t)std::lround(yy.imag())}, hv = {1000, 0};
        if (v == 1) {
          yv = nrd_scale_q15(yv, wq[a]);
          hv = nrd_scale_q15(hv, wq[a]);
        }
        y[v][a][i] = yv;
        h[v][a][i] = hv;
      }
    }
  for (int v = 0; v < 2; v++) {
    std::vector<nrd_c16> yy((size_t)4 * n), hh((size_t)4 * n), mag(n), mb(n), mc(n);
    for (int a = 0; a < 4; a++) {
      memcpy(&yy[(size_t)a * n], y[v][a].data(), n * sizeof(nrd_c16));
      memcpy(&hh[(size_t)a * n], h[v][a].data(), n * sizeof(nrd_c16));
    }
    for (int i = 0; i < n; i++) nrd_comp_re(4, n, i, 2, shift, yy.data(), hh.data(), comp[v].data(), mag.data(), mb.data(), mc.data());
    /* z = g x + e : fit g */
    double num = 0, den = 0;
    for (int i = 0; i < n; i++) {
      num += comp[v][i].r * xs[i].real() + comp[v][i].i * xs[i].imag();
      den += 2.0;
    }
    const double g = num / den;
    double e2 = 0;
    for (int i = 0; i < n; i++) e2 += std::pow(comp[v][i].r - g * xs[i].real(), 2) + std::pow(comp[v][i].i - g * xs[i].imag(), 2);
    snr[v] = 10 * std::log10(g * g * 2.0 * n / e2);
  }
  /* theory: weighted = (amplitude 2000)^2 sum 1/sig^2 / 150^2 ; unweighted = (4 * 2000)^2 / sum sig^2 / 150^2 (per complex sample, x power 2) */
  const double th_w = 10 * std::log10(4e6 * (1 + 1 / 4.0 + 1 / 16.0 + 1 / 64.0) / (150.0 * 150.0));
  const double th_u = 10 * std::log10(16 * 4e6 / ((1 + 4 + 16 + 64) * 150.0 * 150.0));
  EXPECT_NEAR(snr[1], th_w, 0.4);
  EXPECT_NEAR(snr[0], th_u, 0.4);
  EXPECT_GT(snr[1] - snr[0], 5.0); /* an equal-noise sum of branches 18 dB apart is worse than its best branch; the weighted one is not */
}
