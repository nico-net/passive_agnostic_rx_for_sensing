#include <cmath>
#include <gtest/gtest.h>
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
