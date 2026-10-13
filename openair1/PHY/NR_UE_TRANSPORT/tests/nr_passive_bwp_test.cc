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
    for (int i = 0; i < NR_PBWP_NEW_HITS; i++) idx = nr_pbwp_probe_accept(t, rnti, len, (uint32_t)(i + 1));
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
      if (nr_pbwp_score_grant(t, idx, nr_dci_bits_from_u64(payload_for(len, d, ind, N, riv_encode(N, s, l), rng)), c.data())) return g;
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

TEST_F(Pbwp, DistinctNoiseRntisCannotRegisterABwp)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  for (int i = 0; i < 100; i++) EXPECT_EQ(nr_pbwp_probe_accept(t, (uint16_t)(0x2000 + 7 * i), 45, 0), -1);
  EXPECT_EQ(t->n, 1) << "noise accepts (distinct random RNTIs) must not create BWPs";
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

TEST_F(Pbwp, ALayoutSearchWithNoPassAtAllRefutesTheGeometryAndItIsNotChosenAgain)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  const int idx = register_len(0x4601, 45);
  ASSERT_GE(idx, 1);
  t->e[idx].start = 3;
  t->e[idx].size = 51;
  t->e[idx].ind_bits = 1;
  for (int i = 0; i < 32; i++) nr_pbwp_feed_crc_search(t, idx, false); /* a search fails by design: 32 means nothing */
  EXPECT_TRUE(nr_pbwp_resolved(t, idx));
  for (int i = 32; i < NR_PBWP_SEARCH_REFUTE_TRIES; i++) nr_pbwp_feed_crc_search(t, idx, false);
  EXPECT_FALSE(nr_pbwp_resolved(t, idx));
  ASSERT_EQ(t->e[idx].n_excl, 1);
  EXPECT_EQ(t->e[idx].excl[0].size, 51);
  EXPECT_EQ(t->e[idx].excl[0].start, 3);
  /* one pass anywhere spares a hypothesis for good */
  t->e[idx].start = 4;
  t->e[idx].size = 51;
  t->e[idx].crc_try = 0;
  nr_pbwp_feed_crc_search(t, idx, true);
  for (int i = 0; i < 2 * NR_PBWP_SEARCH_REFUTE_TRIES; i++) nr_pbwp_feed_crc_search(t, idx, false);
  EXPECT_TRUE(nr_pbwp_resolved(t, idx));
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

/* Synthetic CORESET-symbol observations: base CORESET on windows 0-3 always lit; the dedicated BWP's
 * CORESET on windows [lo, hi] lit where a DCI lands (rate `p` per window), DM-RS referenced to ref_rb;
 * noise windows below threshold. */
void observe_cs(nr_pbwp_t *t, int n_occ, int lo, int hi, int ref_rb, bool two_symbols, double p, unsigned seed,
                int base_lo = 0, int base_hi = 3, double base_p = 1.0)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  const int n_win = 17; /* 106 PRB */
  for (int o = 0; o < n_occ; o++) {
    for (int sym = 0; sym < 2; sym++) {
      std::vector<float> corr(n_win);
      std::vector<int16_t> ref(n_win);
      for (int w = 0; w < n_win; w++) {
        corr[w] = 0.3f * u(rng);
        ref[w] = (int16_t)(w * 6 - (int)(rng() % 6));
        const bool dci = w >= lo && w <= hi && u(rng) < p && (sym == 0 || two_symbols);
        const bool common = w <= 3 && (base_p >= 1.0 || u(rng) < base_p);
        if (common || dci) { corr[w] = 0.9f + 0.1f * u(rng); ref[w] = common ? 0 : (int16_t)ref_rb; }
      }
      nr_pbwp_coreset_observe(t, n_win, base_lo, base_hi, 0, corr.data(), ref.data(), sym, 0.8f);
    }
  }
}

TEST_F(Pbwp, CoresetOfADedicatedBwpIsFoundWithItsOaiReference)
{
  /* OAI: BWP 1 = 40 PRB at CRB 30 -> CORESET 24 RB at CRB 30, 2 symbols, DM-RS referenced to CRB 30 */
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  observe_cs(t, 400, 5, 8, 30, true, 0.25, 5);
  int start, n, dur, ref;
  ASSERT_TRUE(nr_pbwp_coreset_hypothesis(t, &start, &n, &dur, &ref));
  EXPECT_EQ(start, 30);
  EXPECT_EQ(n, 24);
  EXPECT_EQ(dur, 2);
  EXPECT_EQ(ref, 30);
}

TEST_F(Pbwp, CoresetWithTheSpecReferenceAndOneSymbol)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  observe_cs(t, 400, 9, 14, 0, false, 0.2, 9);
  int start, n, dur, ref;
  ASSERT_TRUE(nr_pbwp_coreset_hypothesis(t, &start, &n, &dur, &ref));
  EXPECT_EQ(start, 54);
  EXPECT_EQ(n, 36);
  EXPECT_EQ(dur, 1);
  EXPECT_EQ(ref, 0);
}

TEST_F(Pbwp, ADedicatedCoresetInsideAWideConfiguredOneIsToldApartByItsReference)
{
  /* The rfsim case that hid it: configured CORESET = 16 groups (RB 0-95) covers the dedicated BWP's CORESET
   * at RB 30-53; only its DM-RS reference (CRB 30, the BWP start on OAI) distinguishes it. */
  nr_pbwp_init(t, 106, 0, 106, 45, 0);
  observe_cs(t, 400, 5, 8, 30, true, 0.25, 21, 0, 15);
  int start, n, dur, ref;
  ASSERT_TRUE(nr_pbwp_coreset_hypothesis(t, &start, &n, &dur, &ref));
  EXPECT_EQ(start, 30);
  EXPECT_EQ(ref, 30);
}

TEST_F(Pbwp, ASpecReferencedDedicatedCoresetInsideTheConfiguredOneIsFoundByOccupancy)
{
  /* Measured on rfsim: the dedicated BWP's CORESET (RB 36-47) is referenced to CRB 0 like the configured
   * 16-group one around it, so only its occupancy after the switch tells it apart: the moved UEs' DCIs
   * all land there, the configured CORESET keeps common traffic (5 % here). */
  nr_pbwp_init(t, 106, 0, 106, 45, 0);
  ASSERT_EQ(register_len(0x4601, 44), 1);
  observe_cs(t, 400, 6, 7, 0, true, 0.25, 31, 0, 15, 0.05);
  int start, n, dur, ref;
  ASSERT_TRUE(nr_pbwp_coreset_hypothesis(t, &start, &n, &dur, &ref));
  EXPECT_EQ(start, 36);
  EXPECT_EQ(n, 12);
  EXPECT_EQ(dur, 2);
  EXPECT_EQ(ref, 0);
}

TEST_F(Pbwp, OccupancyInsideTheConfiguredCoresetMeansNothingBeforeANewBwp)
{
  nr_pbwp_init(t, 106, 0, 106, 45, 0);
  observe_cs(t, 400, 6, 7, 0, true, 0.25, 31, 0, 15, 0.05); /* same traffic, no new DCI length yet */
  int start, n, dur, ref;
  EXPECT_FALSE(nr_pbwp_coreset_hypothesis(t, &start, &n, &dur, &ref));
}

TEST_F(Pbwp, RepetitionProvesAnRntiWhenEveryUeLeftTheBaseBwp)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  int idx = -1;
  for (int i = 0; i < NR_PBWP_NEW_HITS; i++) idx = nr_pbwp_probe_accept(t, 0x4601, 45, (uint32_t)(i + 1)); /* never proven */
  EXPECT_EQ(idx, 1);
  EXPECT_TRUE(nr_pbwp_rnti_seen(t, 0x4601));
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  for (uint16_t r = 1; r <= 50; r++) EXPECT_EQ(nr_pbwp_probe_accept(t, (uint16_t)(0x1000 + r), 45, 0), -1)
      << "distinct noise RNTIs must not register a BWP";
}

TEST_F(Pbwp, NoCoresetIsDeclaredFromNoiseOrTooEarly)
{
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  observe_cs(t, 400, 99, 99, 0, false, 0.0, 3); /* only the base CORESET lit */
  int start, n, dur, ref;
  EXPECT_FALSE(nr_pbwp_coreset_hypothesis(t, &start, &n, &dur, &ref));
  nr_pbwp_init(t, 106, 0, 106, 47, 0);
  observe_cs(t, 20, 5, 8, 30, true, 0.9, 4); /* lit, but fewer than NR_PBWP_CS_MIN_OCC occasions */
  EXPECT_FALSE(nr_pbwp_coreset_hypothesis(t, &start, &n, &dur, &ref));
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
  EXPECT_EQ(nr_pbwp_riv_field(nr_dci_bits_from_u64(p), 45, 2, nr_pbwp_riv_bits(24)), 123u);
}

} // namespace


/* A repeated RNTI whose payload never changes is a degenerate polar fixed point (measured on the sa-bed: the same artifact RNTI at
 * length 44 in every run), not a UE. It must never register a length; the same RNTI with a varying payload must. */
TEST_F(Pbwp, InvariantPayloadNeverRegistersALength)
{
  nr_pbwp_init(t, 106, 0, 106, 48, 1);
  const uint16_t len = t->cand_len[0];
  for (int i = 0; i < 200; i++)
    EXPECT_EQ(nr_pbwp_probe_accept(t, 0xd93d, len, 0xabcdef01u), -1) << "invariant payload registered at call " << i;
  int idx = -1;
  for (int i = 0; i < NR_PBWP_NEW_HITS + 1 && idx < 0; i++)
    idx = nr_pbwp_probe_accept(t, 0x4f61, len, 0x1000u + (uint32_t)i);
  EXPECT_GT(idx, 0) << "a varying payload under one RNTI must register";
}
