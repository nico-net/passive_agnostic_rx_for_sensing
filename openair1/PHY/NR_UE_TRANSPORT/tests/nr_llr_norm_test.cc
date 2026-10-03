/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* K38: a code-block-0 probe must hand the LDPC decoder the same CB0 LLRs as a whole-slot decode of the
 * same hypothesis. The probe's buffer holds the same LLRs up to its horizon and zeros after it; the
 * LLR-norm shift must not see the difference. */
#include <gtest/gtest.h>
#include <random>
#include <vector>
extern "C" {
#include "nr_llr_norm.h"
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
