// Passive UL receive-branch selection (nr_ul_branch_select.h): a branch is left out only when its DM-RS is statistically undetectable
// against its own measured noise; weak live branches stay (the combiner weights them by their own channel estimate).
#include <gtest/gtest.h>
#include <random>
#include "PHY/NR_TRANSPORT/nr_ul_branch_select.h"

namespace {
const double g = NR_UL_BRANCH_SMOOTH_G;
// the statistics of a branch with DM-RS SNR snr_db (linear power ratio h2 / sigma2), noise sigma2 per RE, cnt noise samples
struct Br { double sig2, res2; uint32_t cnt; };
Br branch(double snr_db, double sigma2, uint32_t cnt)
{
  const double h2 = sigma2 * pow(10.0, snr_db / 10.0);
  return {h2 + g * sigma2, (1.0 - g) * sigma2, cnt};  // mean statistics: sig2 = |H|^2 + g sigma^2, resid2 = (1 - g) sigma^2
}
uint32_t sel(const Br *b, int n)
{
  double s[8], r[8];
  uint32_t c[8];
  for (int i = 0; i < n; i++) { s[i] = b[i].sig2; r[i] = b[i].res2; c[i] = b[i].cnt; }
  return nr_ul_branch_select(s, r, c, n);
}
}  // namespace

TEST(UlBranchSelect, FourLiveBranchesAllCombine)
{
  const Br b[4] = {branch(30, 100, 3000), branch(28, 100, 3000), branch(31, 120, 3000), branch(29, 90, 3000)};
  EXPECT_EQ(sel(b, 4), 0xFu);
}

TEST(UlBranchSelect, TheOldRigBranches123DeadLeavesBranch0)
{
  // dead inputs: only the smoothed noise floor (h2 = 0), each with its own noise level
  const Br b[4] = {branch(30, 100, 3000), {g * 40.0, (1 - g) * 40.0, 3000}, {g * 90.0, (1 - g) * 90.0, 3000}, {g * 10.0, (1 - g) * 10.0, 3000}};
  EXPECT_EQ(sel(b, 4), 0x1u);
}

TEST(UlBranchSelect, ADeadBranch0IsExcludedToo)
{
  const Br b[4] = {{g * 70.0, (1 - g) * 70.0, 3000}, branch(30, 100, 3000), branch(29, 100, 3000), branch(31, 100, 3000)};
  EXPECT_EQ(sel(b, 4), 0xEu);
}

TEST(UlBranchSelect, AWeakLiveBranchStaysNoPowerRatioExcludesIt)
{
  // 25 dB below the strongest but 5 dB over its own noise: live
  const Br b[2] = {branch(40, 1.0, 3000), branch(5, 1e4, 3000)};
  EXPECT_EQ(sel(b, 2), 0x3u);
}

TEST(UlBranchSelect, TheTestFollowsThePilotCount)
{
  // the same -12 dB SNR: undetectable with a handful of pilots, detectable with thousands; neither is a fixed power threshold
  const Br few[2] = {branch(30, 100, 40), branch(-12, 100, 40)};
  const Br many[2] = {branch(30, 100, 40000), branch(-12, 100, 40000)};
  EXPECT_EQ(sel(few, 2), 0x1u);
  EXPECT_EQ(sel(many, 2), 0x3u);
}

TEST(UlBranchSelect, NoNoiseEstimateOrNoDetectionKeepsEverything)
{
  const Br nonoise[3] = {branch(30, 100, 3000), {1.0, 0.0, 0}, {2.0, 0.0, 0}};
  EXPECT_EQ(sel(nonoise, 3), 0x7u);
  const Br none[2] = {{g * 50.0, (1 - g) * 50.0, 3000}, {g * 60.0, (1 - g) * 60.0, 3000}};
  EXPECT_EQ(sel(none, 2), 0x3u);  // nothing detected anywhere: all branches
}

TEST(UlBranchSelect, FalseExclusionOfALiveBranchIsRare)
{
  // simulate the statistic of a branch at SNR 0 dB with n_eff independent samples; sd(sig2) = g sigma2 / sqrt(cnt g) for the noise part
  std::mt19937 rng(1);
  const uint32_t cnt = 2000;
  int dead = 0;
  for (int i = 0; i < 4000; i++) {
    std::normal_distribution<double> nd(0.0, 1.0);
    const double neff = cnt * g;
    // noise-only branch: mean of neff exponentials of mean g sigma2 (sigma2 = 1)
    double acc = 0;
    for (int k = 0; k < (int)neff; k++) { const double a = nd(rng), b = nd(rng); acc += 0.5 * (a * a + b * b); }
    const Br noise_only = {g * (acc / neff), 1.0 - g, cnt};
    dead += nr_ul_branch_live(noise_only.sig2, noise_only.res2, noise_only.cnt, 4);
  }
  EXPECT_LE(dead, 8);  // a noise-only branch is called live about alpha / 4 of the time (here 4 x 0.025 % allowed 0.2 %)
}

TEST(UlBranchSelect, TableFollowsABranchThatDiesAndRecovers)
{
  nr_ul_branch_tab_t t{};
  for (int i = 0; i < 20; i++) nr_ul_branch_tab_note(&t, 0xF, 4);
  EXPECT_EQ(nr_ul_branch_tab_keep(&t, 4), -1);
  for (int i = 0; i < 40; i++) nr_ul_branch_tab_note(&t, 0x7, 4);
  EXPECT_EQ(nr_ul_branch_tab_keep(&t, 4), -2);  // three live of four: the per-grant mask decides, no all-branch GPU pass
  for (int i = 0; i < 80; i++) nr_ul_branch_tab_note(&t, 0xF, 4);
  EXPECT_EQ(nr_ul_branch_tab_keep(&t, 4), -1);
  nr_ul_branch_tab_t o{};
  for (int i = 0; i < 20; i++) nr_ul_branch_tab_note(&o, 0x1, 4);  // the old rig
  EXPECT_EQ(nr_ul_branch_tab_keep(&o, 4), 0);
  nr_ul_branch_tab_t w{};
  for (unsigned i = 0; i + 1 < NR_UL_BRANCH_TAB_MIN; i++) nr_ul_branch_tab_note(&w, 0xF, 4);
  EXPECT_EQ(nr_ul_branch_tab_keep(&w, 4), -2);  // too few grants measured
  EXPECT_EQ(nr_ul_branch_tab_keep(&w, 1), -1);
}
