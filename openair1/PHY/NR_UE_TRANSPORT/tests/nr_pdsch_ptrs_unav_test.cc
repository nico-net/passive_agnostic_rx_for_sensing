#include <cstdlib>
#include <iostream>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_ptrs_unav.h"
}

// Lab-cell shape: 273 PRB, allocation S=1 L=13, DM-RS on symbols 2 and 11 (mask 0x804).
static constexpr uint16_t RB = 273;
static constexpr uint8_t  S = 1, L = 13;
static constexpr uint16_t DMRS = 0x804;

TEST(PtrsUnav, RefusesArgumentsThatDescribeNoPtrs) {
  EXPECT_EQ(nr_pdsch_ptrs_unav_res(0, S, L, DMRS, 2, 1, 1), 0u);
  EXPECT_EQ(nr_pdsch_ptrs_unav_res(RB, S, 0, DMRS, 2, 1, 1), 0u);
  EXPECT_EQ(nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 1, 0), 0u);   // no ports
  EXPECT_EQ(nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 3, 1, 1), 0u);   // K must be 2 or 4
  EXPECT_EQ(nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 3, 1), 0u);   // L must be 1, 2 or 4
}

TEST(PtrsUnav, OneSubcarrierPerKResourceBlocksRoundingUp) {
  // A partial group still carries a PT-RS subcarrier. Dropping it would under-count G on any
  // allocation that is not a multiple of K -- 273 is odd, so this is the common case, not a corner.
  const uint32_t k2 = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 1, 1);
  const uint32_t k4 = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 4, 1, 1);
  ASSERT_GT(k2, 0u);
  ASSERT_GT(k4, 0u);
  EXPECT_EQ(k2 % 137u, 0u) << "273 PRB at K=2 must give ceil(273/2) = 137 subcarriers per symbol";
  EXPECT_EQ(k4 % 69u, 0u)  << "273 PRB at K=4 must give ceil(273/4) = 69 subcarriers per symbol";
  EXPECT_GT(k2, k4) << "a denser frequency comb must cost MORE resource elements";
}

TEST(PtrsUnav, DmrsSymbolsCarryNoPtrs) {
  // PT-RS is not mapped on a DM-RS symbol (TS 38.211 7.4.1.2.2). Adding a DM-RS symbol inside the
  // allocation must therefore not increase the count.
  const uint32_t one_dmrs = nr_pdsch_ptrs_unav_res(RB, S, L, 0x004, 2, 1, 1);
  const uint32_t two_dmrs = nr_pdsch_ptrs_unav_res(RB, S, L, 0x804, 2, 1, 1);
  EXPECT_LE(two_dmrs, one_dmrs) << "a DM-RS symbol was counted as carrying PT-RS";
}

TEST(PtrsUnav, PtrsStartsAtTheAllocationsFirstSymbol) {
  // TS 38.214 5.1.6.3: l_ref is the allocation's first symbol and PT-RS sits at l_ref + i*L from
  // i = 0, so symbols BEFORE the first DM-RS do carry PT-RS -- exactly what set_ptrs_symb_idx()
  // (the extraction side) does. The old expectation of 0 here left G one PT-RS symbol short of
  // the LLR count on every mapping-A grant (measured on the rank-4 bed, 2026-09-16).
  const uint32_t early = nr_pdsch_ptrs_unav_res(RB, 0, 3, 0x004, 2, 1, 1);   // symbols 0,1 carry, 2 is DM-RS
  EXPECT_EQ(early, 2u * 137u);
}

TEST(PtrsUnav, TimeDensityScalesTheCount) {
  const uint32_t l1 = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 1, 1);
  const uint32_t l2 = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 2, 1);
  const uint32_t l4 = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 4, 1);
  EXPECT_GT(l1, l2);
  EXPECT_GT(l2, l4);
  EXPECT_GT(l4, 0u) << "even the sparsest density must place some PT-RS";
}

TEST(PtrsUnav, PortsScaleLinearly) {
  const uint32_t p1 = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 2, 1);
  const uint32_t p2 = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, 2, 2, 2);
  EXPECT_EQ(p2, 2u * p1);
}

TEST(PtrsUnav, TheSweptDensitySetIsTheSpecSet) {
  // K in {2,4}, L in {1,2,4}, all six pairs present exactly once -- if this set is wrong the sweep
  // can never reach the cell's real density and PT-RS grants silently never decode.
  ASSERT_EQ(NR_PTRS_DENSITY_N, 6);
  int seen[5][5] = {{0}};
  for (int i = 0; i < NR_PTRS_DENSITY_N; i++) {
    const uint8_t k = nr_ptrs_density_k[i], l = nr_ptrs_density_l[i];
    EXPECT_TRUE(k == 2 || k == 4);
    EXPECT_TRUE(l == 1 || l == 2 || l == 4);
    seen[k][l]++;
  }
  EXPECT_EQ(seen[2][1] + seen[2][2] + seen[2][4], 3);
  EXPECT_EQ(seen[4][1] + seen[4][2] + seen[4][4], 3);
  for (int k : {2, 4}) for (int l : {1, 2, 4}) EXPECT_EQ(seen[k][l], 1) << "k=" << k << " l=" << l;
}

TEST(PtrsUnav, EveryDensityGivesAUsableGReduction) {
  // The whole point: unav_res must be a meaningful fraction of the allocation, or sweeping it
  // could not change a decode outcome and the sweep would be pointless.
  const uint32_t total_re = (uint32_t)RB * 12u * L;
  for (int i = 0; i < NR_PTRS_DENSITY_N; i++) {
    const uint32_t u = nr_pdsch_ptrs_unav_res(RB, S, L, DMRS, nr_ptrs_density_k[i],
                                              nr_ptrs_density_l[i], 1);
    EXPECT_GT(u, 0u);
    EXPECT_LT(u, total_re / 4u) << "PT-RS cannot plausibly consume a quarter of the allocation";
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---- PT-RS density sweep (2026-09-15) ---------------------------------------------------------
TEST(PtrsSweep, ACellWithoutPtrsLatchesAbsent) {
  nr_ptrs_sweep_t s; nr_ptrs_sweep_init(&s);
  unsigned seed = 1; int latched = -1;
  for (int t = 0; t < 2000 && latched < 0; t++) {
    const int a = nr_ptrs_sweep_pick(&s);
    const bool ok = (a == 0) && (rand_r(&seed) % 100 < 60);   // only "absent" decodes, at 60 %
    latched = nr_ptrs_sweep_feed(&s, a, ok);
  }
  EXPECT_EQ(latched, 0);
  std::cerr << "[ MEASURED ] no-PT-RS cell latched 'absent' after " << (s.tr[0]+s.tr[1]+s.tr[2]+s.tr[3]+s.tr[4]+s.tr[5]+s.tr[6])
            << " grants (wrong arms tried " << (s.tr[1]+s.tr[2]+s.tr[3]+s.tr[4]+s.tr[5]+s.tr[6]) << " times)\n";
}
TEST(PtrsSweep, FindsTheConfiguredDensityAtALowDecodeRate) {
  // A commercial wide-band cell: PT-RS K=2 L=1, and even the right arm decodes only 30 %.
  nr_ptrs_sweep_t s; nr_ptrs_sweep_init(&s);
  unsigned seed = 9; int latched = -1;
  for (int t = 0; t < 5000 && latched < 0; t++) {
    const int a = nr_ptrs_sweep_pick(&s);
    latched = nr_ptrs_sweep_feed(&s, a, a == 1 && (rand_r(&seed) % 100 < 30));
  }
  EXPECT_EQ(latched, 1);
  uint8_t K = 0, L = 0;
  ASSERT_TRUE(nr_ptrs_sweep_arm(latched, &K, &L));
  EXPECT_EQ(K, 2); EXPECT_EQ(L, 1);
}
TEST(PtrsSweep, NeverLatchesWhenNothingDecodes) {
  nr_ptrs_sweep_t s; nr_ptrs_sweep_init(&s);
  for (int t = 0; t < 3000; t++) EXPECT_EQ(nr_ptrs_sweep_feed(&s, nr_ptrs_sweep_pick(&s), false), -1);
  for (int a = 0; a < NR_PTRS_ARMS; a++) EXPECT_GT(s.tr[a], 0u) << "arm " << a << " starved";
}
