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

/* A DCI 1_1 payload of `len` bits: identifier 1 | indicator (d bits) | FDRA (riv_bits(n)) | noise. */
uint64_t payload_for(uint16_t len, uint8_t d, uint8_t ind, uint16_t n, uint32_t riv, std::mt19937 &rng)
{
  const uint8_t rb = nr_pbwp_riv_bits(n);
  const int low = len - 1 - d - rb;
  uint64_t p = (uint64_t)rng() & ((1ull << low) - 1);
  p |= (uint64_t)riv << low;
  p |= (uint64_t)ind << (len - 1 - d);
  p |= 1ull << (len - 1);
  return p;
}

/* Per-PRB DM-RS coherence of one grant: high on the grant's own PRBs, noise elsewhere, plus an
 * optional second UE's grant (same CRB0-referenced sequence, so it lights up too). */
std::vector<float> coh_for(int carrier, int abs_start, int len, std::mt19937 &rng, int other_start = -1,
                           int other_len = 0)
{
  std::uniform_real_distribution<float> noise(0.0f, 0.25f), sig(0.8f, 1.0f);
  std::vector<float> c(carrier);
  for (int p = 0; p < carrier; p++) c[p] = noise(rng);
  for (int p = abs_start; p < abs_start + len; p++) c[p] = sig(rng);
  for (int p = other_start; other_start >= 0 && p < other_start + other_len && p < carrier; p++) c[p] = sig(rng);
  return c;
}

struct Pbwp : ::testing::Test {
  nr_pbwp_t *t = nullptr;
  void SetUp() override { t = new nr_pbwp_t{}; }
  void TearDown() override { nr_pbwp_free(t); delete t; }
  int register_len(uint16_t rnti, uint16_t len)
  {
    nr_pbwp_mark_seen(t, rnti);
    int idx = -1;
    for (int i = 0; i < NR_PBWP_NEW_HITS; i++) idx = nr_pbwp_probe_accept(t, rnti, len);
    return idx;
  }
  /* Drive grants until resolution; returns the grant count, or -1. */
  int resolve(int idx, int C, int N, int S, uint8_t d, uint8_t ind, bool full_band, bool busy, unsigned seed)
  {
    std::mt19937 rng(seed);
    const uint16_t len = t->e[idx].dci_len;
    for (int g = 1; g <= 400; g++) {
      uint16_t s = 0, l = (uint16_t)N;
      if (!full_band) { s = (uint16_t)(rng() % N); l = (uint16_t)(1 + rng() % (N - s)); }
      auto c = busy ? coh_for(C, S + s, l, rng, (int)(rng() % C), 10 + (int)(rng() % 30)) : coh_for(C, S + s, l, rng);
      if (nr_pbwp_score_grant(t, idx, payload_for(len, d, ind, N, riv_encode(N, s, l), rng), c.data())) return g;
    }
    return -1;
  }
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
  EXPECT_EQ(nr_pbwp_riv_bits(106), 13);
  EXPECT_EQ(nr_pbwp_riv_bits(273), 16);
  EXPECT_EQ(nr_pbwp_riv_bits(51), 11);
  EXPECT_EQ(nr_pbwp_riv_bits(24), 9);
}

TEST_F(Pbwp, LengthTracksTheRivAndIndicatorWidths)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  EXPECT_EQ(nr_pbwp_len_for(t, 106, 0), 47);
  EXPECT_EQ(nr_pbwp_len_for(t, 51, 0), 45);
  EXPECT_EQ(nr_pbwp_len_for(t, 51, 2), 47) << "51 PRB + 2-bit indicator collides with the base length";
  EXPECT_EQ(nr_pbwp_len_for(t, 24, 2), 45);
  for (int k = 0; k < t->n_cand; k++) EXPECT_NE(t->cand_len[k], 47);
}

TEST_F(Pbwp, AnUnprovenRntiCannotRegisterABwp)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  for (int i = 0; i < 100; i++) EXPECT_EQ(nr_pbwp_probe_accept(t, 0x1234, 45), -1);
  EXPECT_EQ(t->n, 1) << "noise accepts (never-proven RNTIs) must not create BWPs";
}

TEST_F(Pbwp, AProvenRntiAtANewLengthRegistersOneGroupPerIndicatorWidth)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  const int idx = register_len(0x4601, 45);
  ASSERT_EQ(idx, 1);
  EXPECT_EQ(t->e[1].start, -1);
  EXPECT_GE(t->e[1].ng, 2) << "45 bits = (d=0, 11-bit FDRA) or (d=1, 10) or (d=2, 9)";
}

TEST_F(Pbwp, DmrsCoherenceResolvesSizeStartAndIndicatorOnABusyCell)
{
  /* Truth: 40-PRB BWP at CRB 30 of a 106-PRB carrier with a 2-bit indicator (two dedicated BWPs),
   * other UEs' grants lighting random PRBs in the same slots. (51 PRB + 2 bits would give exactly the
   * base 47 -- a length collision, see CollidingGeometryIsNotDiscoverable.) */
  const int C = 106, N = 40, S = 30;
  nr_pbwp_init(t, C, 0, 106, 47, 0);
  const int idx = register_len(0x4601, nr_pbwp_len_for(t, N, 2));
  ASSERT_GE(idx, 1);
  const int g = resolve(idx, C, N, S, 2, 1, false, true, 7);
  ASSERT_GT(g, 0) << "not resolved";
  EXPECT_EQ(t->e[idx].size, N);
  EXPECT_EQ(t->e[idx].start, S);
  EXPECT_EQ(t->e[idx].ind_bits, 2);
  std::cerr << "[ MEASURED ] busy cell: resolved (d 2, size " << N << ", start " << S << ") after " << g << " grants\n";
}

TEST_F(Pbwp, ResolvesWithoutAnIndicatorToo)
{
  const int C = 106, N = 51, S = 30;
  nr_pbwp_init(t, C, 0, 106, 47, 0);
  const int idx = register_len(0x4601, nr_pbwp_len_for(t, N, 0));
  ASSERT_GE(idx, 1);
  ASSERT_GT(resolve(idx, C, N, S, 0, 0, false, true, 11), 0);
  EXPECT_EQ(t->e[idx].size, N);
  EXPECT_EQ(t->e[idx].start, S);
  EXPECT_EQ(t->e[idx].ind_bits, 0);
}

TEST_F(Pbwp, FullBandGrantsAloneResolveTheBwp)
{
  /* The loaded-cell case: the scheduler fills the whole BWP every time. */
  const int C = 273, N = 100, S = 40;
  nr_pbwp_init(t, C, 0, 273, 48, 0);
  const int idx = register_len(0x4b00, nr_pbwp_len_for(t, N, 1));
  ASSERT_GE(idx, 1);
  const int g = resolve(idx, C, N, S, 1, 0, true, false, 3);
  ASSERT_GT(g, 0);
  EXPECT_EQ(t->e[idx].size, N);
  EXPECT_EQ(t->e[idx].start, S);
  EXPECT_EQ(t->e[idx].ind_bits, 1);
  std::cerr << "[ MEASURED ] full-band only: resolved after " << g << " grants\n";
}

TEST_F(Pbwp, AWrongResolutionIsUndoneByTheTbCrc)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  const int idx = register_len(0x4601, 45);
  ASSERT_GE(idx, 1);
  t->e[idx].start = 3; /* pretend the vote picked this */
  t->e[idx].size = 51;
  ASSERT_TRUE(nr_pbwp_resolved(t, idx));
  for (int i = 0; i < 32; i++) nr_pbwp_feed_crc(t, idx, false);
  EXPECT_FALSE(nr_pbwp_resolved(t, idx));
  EXPECT_GT(t->e[idx].ng, 0);
}

TEST_F(Pbwp, RrcSwitchIsSeenAsALengthChange)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  t->e[1] = nr_pbwp_entry_t{.dci_len = 45, .start = 30, .size = 51};
  t->n = 2;
  EXPECT_FALSE(nr_pbwp_on_accept(t, 0x4601, 0));
  EXPECT_FALSE(nr_pbwp_on_accept(t, 0x4601, 0));
  EXPECT_TRUE(nr_pbwp_on_accept(t, 0x4601, nr_pbwp_entry_for_len(t, 0x4601, 45)));
  EXPECT_EQ(t->rnti_bwp[0x4601], 2);
  EXPECT_EQ(t->switches, 1u);
}

TEST_F(Pbwp, DciIndicatorSwitchUsesTheTargetBwpOnceBound)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  t->e[1] = nr_pbwp_entry_t{.dci_len = 45, .start = 30, .size = 51};
  t->n = 2;
  for (int i = 0; i < NR_PBWP_IND_LEARN; i++) EXPECT_EQ(nr_pbwp_indicator(t, 0, 0), 0);
  for (int i = 0; i < NR_PBWP_IND_LEARN; i++) EXPECT_EQ(nr_pbwp_indicator(t, 1, 1), 1);
  EXPECT_EQ(nr_pbwp_indicator(t, 0, 1), 1) << "indicator 1 on BWP 0 is a switch grant for BWP 1";
}

TEST_F(Pbwp, CollidingGeometryIsNotDiscoverable)
{
  /* Known limitation, pinned: a BWP whose (FDRA + indicator) width equals the base one arrives at the
   * base length, so no new length ever appears and it is read with the base layout. */
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  EXPECT_EQ(nr_pbwp_len_for(t, 51, 2), 47);
  EXPECT_EQ(register_len(0x4601, 47), -1);
}

TEST(PassiveBwp, SwitchGrantFdraFollowsTheSpec)
{
  EXPECT_EQ(nr_pbwp_translate_riv(0x1abc, 13, 11), 0x1abcu & 0x7ff);
  EXPECT_EQ(nr_pbwp_translate_riv(0x5a5, 11, 13), 0x5a5u);
}

TEST(PassiveBwp, RivFieldSitsAfterTheIndicator)
{
  std::mt19937 rng(1);
  const uint64_t p = payload_for(45, 2, 3, 24, 123, rng);
  EXPECT_EQ(nr_pbwp_riv_field(p, 45, 2, nr_pbwp_riv_bits(24)), 123u);
}

} // namespace
