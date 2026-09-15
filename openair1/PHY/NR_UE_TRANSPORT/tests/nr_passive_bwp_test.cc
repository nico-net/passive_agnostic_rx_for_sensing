#include <gtest/gtest.h>
#include <random>
#include <vector>

extern "C" {
#include "nr_passive_bwp.h"
}

namespace {

uint32_t riv_encode(uint16_t n, uint16_t s, uint16_t l)
{
  return (l - 1u <= n / 2u) ? n * (l - 1u) + s : n * (n - l + 1u) + (n - 1u - s);
}

/* Per-PRB DM-RS coherence of one grant: high on the grant's own PRBs, noise elsewhere. Optionally
 * other UEs' grants light up extra PRBs (their DM-RS uses the same CRB0-referenced sequence). */
std::vector<float> coh_for(int carrier, int abs_start, int len, std::mt19937 &rng, int other_start = -1,
                           int other_len = 0)
{
  std::uniform_real_distribution<float> noise(0.0f, 0.25f), sig(0.8f, 1.0f);
  std::vector<float> c(carrier);
  for (int p = 0; p < carrier; p++) c[p] = noise(rng);
  for (int p = abs_start; p < abs_start + len; p++) c[p] = sig(rng);
  for (int p = other_start; other_start >= 0 && p < other_start + other_len; p++) c[p] = sig(rng);
  return c;
}

struct Pbwp : ::testing::Test {
  nr_pbwp_t *t = nullptr;
  void SetUp() override { t = new nr_pbwp_t{}; }
  void TearDown() override { nr_pbwp_free(t); delete t; }
};

TEST(PassiveBwp, RivRoundTripsForEveryAllocation)
{
  for (uint16_t n : {24, 51, 106, 273})
    for (uint16_t s = 0; s < n; s++)
      for (uint16_t l = 1; s + l <= n; l++) {
        uint16_t s2, l2;
        ASSERT_TRUE(nr_pbwp_riv_decode(riv_encode(n, s, l), n, &s2, &l2));
        ASSERT_EQ(s2, s);
        ASSERT_EQ(l2, l);
      }
}

TEST(PassiveBwp, RivWidthsMatchTheSpec)
{
  EXPECT_EQ(nr_pbwp_riv_bits(106), 13); /* 5671 */
  EXPECT_EQ(nr_pbwp_riv_bits(273), 16); /* 37401 */
  EXPECT_EQ(nr_pbwp_riv_bits(51), 11);  /* 1326 */
  EXPECT_EQ(nr_pbwp_riv_bits(24), 9);   /* 300 */
}

TEST_F(Pbwp, LengthTracksOnlyTheRivWidth)
{
  nr_pbwp_init(t, 106, 0, 106, 47);
  EXPECT_EQ(nr_pbwp_len_for_size(t, 106), 47);
  EXPECT_EQ(nr_pbwp_len_for_size(t, 51), 45);
  EXPECT_EQ(nr_pbwp_len_for_size(t, 24), 43);
  EXPECT_GT(t->n_cand, 0);
  for (int k = 0; k < t->n_cand; k++) EXPECT_NE(t->cand_len[k], 47);
}

TEST_F(Pbwp, AnUnknownRntiCannotRegisterABwp)
{
  nr_pbwp_init(t, 106, 0, 106, 47);
  for (int i = 0; i < 100; i++) EXPECT_EQ(nr_pbwp_probe_accept(t, 0x1234, 45), -1);
  EXPECT_EQ(t->n, 1) << "noise accepts (never-seen RNTIs) must not create BWPs";
}

TEST_F(Pbwp, AKnownRntiAtANewLengthRegistersItWithTheSizeRange)
{
  nr_pbwp_init(t, 106, 0, 106, 47);
  nr_pbwp_on_accept(t, 0x4601, 0);
  int idx = -1;
  for (int i = 0; i < NR_PBWP_NEW_HITS; i++) idx = nr_pbwp_probe_accept(t, 0x4601, 45);
  ASSERT_EQ(idx, 1);
  EXPECT_LE(t->e[1].size_lo, 51);
  EXPECT_GE(t->e[1].size_hi, 51);
  EXPECT_EQ(t->e[1].start, -1);
  EXPECT_EQ(nr_pbwp_riv_bits(t->e[1].size_lo), nr_pbwp_riv_bits(t->e[1].size_hi));
}

TEST_F(Pbwp, DmrsCoherenceResolvesSizeAndStartOnABusyCell)
{
  /* Truth: dedicated BWP of 51 PRBs starting at CRB 30 on a 106-PRB carrier. A second UE's grants
   * on the initial BWP light up other PRBs in the same slots. */
  const int C = 106, N = 51, S = 30;
  nr_pbwp_init(t, C, 0, 106, 47);
  nr_pbwp_on_accept(t, 0x4601, 0);
  int idx = -1;
  for (int i = 0; i < NR_PBWP_NEW_HITS; i++) idx = nr_pbwp_probe_accept(t, 0x4601, nr_pbwp_len_for_size(t, N));
  ASSERT_GE(idx, 1);
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> ds(0, N - 1);
  bool done = false;
  int grants = 0;
  while (!done && grants < 400) {
    const uint16_t s = (uint16_t)ds(rng), l = (uint16_t)(1 + rng() % (N - s));
    auto c = coh_for(C, S + s, l, rng, (int)(rng() % 60), 10 + (int)(rng() % 30));
    done = nr_pbwp_score_grant(t, idx, riv_encode(N, s, l), c.data());
    grants++;
  }
  ASSERT_TRUE(done) << "not resolved after " << grants << " grants";
  EXPECT_EQ(t->e[idx].size_lo, N);
  EXPECT_EQ(t->e[idx].start, S);
  std::cerr << "[ MEASURED ] resolved (size " << N << ", start " << S << ") after " << grants << " grants\n";
}

TEST_F(Pbwp, FullBandGrantsAloneResolveTheBwp)
{
  /* The loaded-cell case: the scheduler fills the whole BWP every time. */
  const int C = 273, N = 200, S = 40;
  nr_pbwp_init(t, C, 0, 273, 48);
  nr_pbwp_on_accept(t, 0x4b00, 0);
  int idx = -1;
  for (int i = 0; i < NR_PBWP_NEW_HITS; i++) idx = nr_pbwp_probe_accept(t, 0x4b00, nr_pbwp_len_for_size(t, N));
  ASSERT_GE(idx, 1);
  std::mt19937 rng(3);
  bool done = false;
  int grants = 0;
  while (!done && grants < 200) {
    auto c = coh_for(C, S, N, rng);
    done = nr_pbwp_score_grant(t, idx, riv_encode(N, 0, N), c.data());
    grants++;
  }
  ASSERT_TRUE(done);
  EXPECT_EQ(t->e[idx].size_lo, N);
  EXPECT_EQ(t->e[idx].start, S);
}

TEST_F(Pbwp, AWrongResolutionIsUndoneByTheTbCrc)
{
  nr_pbwp_init(t, 106, 0, 106, 47);
  nr_pbwp_on_accept(t, 0x4601, 0);
  int idx = -1;
  for (int i = 0; i < NR_PBWP_NEW_HITS; i++) idx = nr_pbwp_probe_accept(t, 0x4601, 45);
  ASSERT_GE(idx, 1);
  t->e[idx].size_lo = t->e[idx].size_hi = 51; /* pretend the vote picked this */
  t->e[idx].start = 3;
  free(t->e[idx].score);
  t->e[idx].score = nullptr;
  ASSERT_TRUE(nr_pbwp_resolved(t, idx));
  for (int i = 0; i < 32; i++) nr_pbwp_feed_crc(t, idx, false);
  EXPECT_FALSE(nr_pbwp_resolved(t, idx));
  EXPECT_NE(t->e[idx].score, nullptr);
}

TEST_F(Pbwp, RrcSwitchIsSeenAsALengthChange)
{
  nr_pbwp_init(t, 106, 0, 106, 47);
  t->e[1] = nr_pbwp_entry_t{.dci_len = 45, .size_lo = 51, .size_hi = 51, .start = 30};
  t->n = 2;
  EXPECT_FALSE(nr_pbwp_on_accept(t, 0x4601, 0));
  EXPECT_FALSE(nr_pbwp_on_accept(t, 0x4601, 0));
  EXPECT_TRUE(nr_pbwp_on_accept(t, 0x4601, nr_pbwp_entry_for_len(t, 0x4601, 45)));
  EXPECT_EQ(t->rnti_bwp[0x4601], 2);
  EXPECT_EQ(t->switches, 1u);
}

TEST_F(Pbwp, DciIndicatorSwitchUsesTheTargetBwpOnceBound)
{
  nr_pbwp_init(t, 106, 0, 106, 47);
  t->e[1] = nr_pbwp_entry_t{.dci_len = 45, .size_lo = 51, .size_hi = 51, .start = 30};
  t->n = 2;
  for (int i = 0; i < NR_PBWP_IND_LEARN; i++) EXPECT_EQ(nr_pbwp_indicator(t, 0x4601, 0, 0), 0);
  for (int i = 0; i < NR_PBWP_IND_LEARN; i++) EXPECT_EQ(nr_pbwp_indicator(t, 0x4602, 1, 1), 1);
  EXPECT_EQ(nr_pbwp_indicator(t, 0x4601, 0, 1), 1) << "indicator 1 on BWP 0 is a switch grant for BWP 1";
  EXPECT_EQ(nr_pbwp_indicator(t, 0x4601, 1, 1), 1);
}

TEST(PassiveBwp, SwitchGrantFdraFollowsTheSpec)
{
  /* current 13-bit field (106 PRB), target 11 bits (51 PRB): LSBs kept */
  EXPECT_EQ(nr_pbwp_translate_riv(0x1abc, 13, 11), 0x1abcu & 0x7ff);
  /* current 11 bits, target 13: zeros prepended, i.e. the value is unchanged */
  EXPECT_EQ(nr_pbwp_translate_riv(0x5a5, 11, 13), 0x5a5u);
}

} // namespace
