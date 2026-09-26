#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_prb_set.h"
}
extern "C" int nr_prb_gather_index(const nr_prb_seg_t *seg, int nseg, int re_per_prb, int *out, int max);

TEST(PrbSet, RbgSizeTable) {  // 38.214 Table 5.1.2.2.1-1
  EXPECT_EQ(nr_rbg_size(36, 0), 2);  EXPECT_EQ(nr_rbg_size(36, 1), 4);
  EXPECT_EQ(nr_rbg_size(37, 0), 4);  EXPECT_EQ(nr_rbg_size(72, 1), 8);
  EXPECT_EQ(nr_rbg_size(73, 0), 8);  EXPECT_EQ(nr_rbg_size(144, 1), 16);
  EXPECT_EQ(nr_rbg_size(273, 0), 16); EXPECT_EQ(nr_rbg_size(273, 1), 16);
  EXPECT_EQ(nr_rbg_size(0, 0), 0);   EXPECT_EQ(nr_rbg_size(276, 0), 0);
}

TEST(PrbSet, Type0AlignedBwp) {
  // start 0, size 51, config1 -> P=4, N_RBG = 13, last RBG = 51 mod 4 = 3 RBs.
  EXPECT_EQ(nr_rbg_count(0, 51, 4), 13);
  uint16_t p[NR_PRB_SET_MAX];
  const int n = nr_ra_type0_prbs((1u << 12) | 1u, 0, 51, 4, p, NR_PRB_SET_MAX);  // RBG 0 (MSB) and RBG 12
  ASSERT_EQ(n, 7);
  const uint16_t want[7] = {0, 1, 2, 3, 48, 49, 50};
  for (int i = 0; i < 7; i++) EXPECT_EQ(p[i], want[i]);
}

TEST(PrbSet, Type0MisalignedBwpHasShortFirstAndLastRbg) {
  // start 3, size 20, P=4: N_RBG = ceil(23/4) = 6; RBG0 = {0} (4-3=1 RB), RBG5 = {17,18,19} ((3+20) mod 4 = 3).
  EXPECT_EQ(nr_rbg_count(3, 20, 4), 6);
  uint16_t p[NR_PRB_SET_MAX];
  ASSERT_EQ(nr_ra_type0_prbs(1u << 5, 3, 20, 4, p, NR_PRB_SET_MAX), 1);
  EXPECT_EQ(p[0], 0);
  ASSERT_EQ(nr_ra_type0_prbs(1u, 3, 20, 4, p, NR_PRB_SET_MAX), 3);
  EXPECT_EQ(p[0], 17); EXPECT_EQ(p[2], 19);
  ASSERT_EQ(nr_ra_type0_prbs((1u << 6) - 1, 3, 20, 4, p, NR_PRB_SET_MAX), 20);  // all RBGs = whole BWP
}

TEST(PrbSet, DynamicSwitchMsbSelectsType) {
  // width = 1 + max(N_RBG=13, riv_bits=11) = 14; MSB (bit 13) = 1 -> type 1.
  uint32_t t0 = 0, riv = 0;
  EXPECT_EQ(nr_fdra_dynamic_split((1u << 13) | 0x2AB, 13, 11, &t0, &riv), 1);
  EXPECT_EQ(riv, 0x2ABu);
  EXPECT_EQ(nr_fdra_dynamic_split(0x1801, 13, 11, &t0, &riv), 0);
  EXPECT_EQ(t0, 0x1801u);
}

TEST(PrbSet, InterleavedVrbEvenBundleCount) {
  // start 0, size 10, L=2: 5 bundles, C=2; f = {0,2,1,3,4} (last bundle fixed).
  uint16_t p[10];
  ASSERT_EQ(nr_vrb_to_prb_interleaved(0, 10, 2, 0, 10, p), 10);
  const uint16_t want[10] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9};
  for (int i = 0; i < 10; i++) EXPECT_EQ(p[i], want[i]) << i;
}

TEST(PrbSet, InterleavedVrbSevenBundles) {
  // start 0, size 14, L=2: 7 bundles, C=3; f = {0,3,1,4,2,5,6}.
  uint16_t p[14];
  ASSERT_EQ(nr_vrb_to_prb_interleaved(0, 14, 2, 0, 14, p), 14);
  const uint16_t want[14] = {0, 1, 6, 7, 2, 3, 8, 9, 4, 5, 10, 11, 12, 13};
  for (int i = 0; i < 14; i++) EXPECT_EQ(p[i], want[i]) << i;
}

TEST(PrbSet, InterleavedVrbMisalignedIsIdentityWithThreeBundles) {
  // start 1, size 10, L=4: bundles {0..2},{3..6},{7..9}; C=1 -> f = {0,1,2}.
  uint16_t p[10];
  ASSERT_EQ(nr_vrb_to_prb_interleaved(1, 10, 4, 0, 10, p), 10);
  for (int i = 0; i < 10; i++) EXPECT_EQ(p[i], i);
}

TEST(PrbSet, InterleavedVrbIsAPermutation) {
  for (int start : {0, 1, 2, 3, 5})
    for (int L : {2, 4})
      for (int size : {24, 51, 106, 273}) {
        std::vector<uint16_t> p(size);
        ASSERT_EQ(nr_vrb_to_prb_interleaved(start, size, L, 0, size, p.data()), size);
        std::vector<int> seen(size, 0);
        for (auto v : p) { ASSERT_LT(v, size); seen[v]++; }
        for (int i = 0; i < size; i++) EXPECT_EQ(seen[i], 1) << start << "/" << L << "/" << size;
      }
}

TEST(PrbSet, SegmentsFollowDataOrderAndPrgBoundaries) {
  const uint16_t il[10] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9};
  nr_prb_seg_t s[16];
  ASSERT_EQ(nr_prb_segments(il, 10, 0, 0, s, 16), 4);
  EXPECT_EQ(s[0].prb_start, 0); EXPECT_EQ(s[0].n_prb, 2); EXPECT_EQ(s[0].data_index, 0);
  EXPECT_EQ(s[1].prb_start, 4); EXPECT_EQ(s[1].data_index, 2);
  EXPECT_EQ(s[2].prb_start, 2); EXPECT_EQ(s[2].data_index, 4);
  EXPECT_EQ(s[3].prb_start, 6); EXPECT_EQ(s[3].n_prb, 4);
  uint16_t c[10];
  for (int i = 0; i < 10; i++) c[i] = (uint16_t)i;
  ASSERT_EQ(nr_prb_segments(c, 10, 0, 2, s, 16), 5);  // PRG 2 on CRB 0..9
  ASSERT_EQ(nr_prb_segments(c, 10, 1, 4, s, 16), 3);  // PRG 4 on CRB 1..10: breaks at CRB 4 and 8
  EXPECT_EQ(s[0].n_prb, 3); EXPECT_EQ(s[1].n_prb, 4); EXPECT_EQ(s[2].n_prb, 3);
  ASSERT_EQ(nr_prb_segments(c, 10, 0, 0, s, 16), 1);  // wideband contiguous = today's single segment
  EXPECT_EQ(nr_prb_segments(c, 10, 0, 2, s, 4), -1);  // does not fit
}

TEST(PrbSet, GatherConcatenatesSegmentsInDataOrder) {
  // Two segments: PRBs 4-5 (data 0-1) then PRBs 0-1 (data 2-3); 2 REs per PRB for the test.
  const nr_prb_seg_t s[2] = {{4, 2, 0}, {0, 2, 2}};
  int idx[8];
  ASSERT_EQ(nr_prb_gather_index(s, 2, 2, idx, 8), 8);
  const int want[8] = {8, 9, 10, 11, 0, 1, 2, 3};  // RE index within the symbol, BWP-relative
  for (int i = 0; i < 8; i++) EXPECT_EQ(idx[i], want[i]);
}

TEST(PrbSet, ListNormaliseRejectsDuplicateOutOfBwpAndOverLength) {
  // The pure core of nr_pdsch_passive_alloc_normalise(): every producer's PRB list goes through it.
  uint32_t bm[9] = {0xdeadbeef};
  int lo = -7, hi = -7;
  const uint16_t ok[4] = {40, 3, 4, 100};  // data order, not sorted
  ASSERT_TRUE(nr_prb_list_normalise(ok, 4, 106, bm, 9, &lo, &hi));
  EXPECT_EQ(lo, 3); EXPECT_EQ(hi, 100);
  EXPECT_EQ(bm[0], (1u << 3) | (1u << 4)); EXPECT_EQ(bm[1], 1u << 8); EXPECT_EQ(bm[3], 1u << 4);
  const uint16_t dup[3] = {5, 6, 5};
  bm[0] = 0x1234u; lo = hi = -7;
  EXPECT_FALSE(nr_prb_list_normalise(dup, 3, 106, bm, 9, &lo, &hi));
  EXPECT_EQ(bm[0], 0x1234u) << "outputs must be untouched on failure";
  EXPECT_EQ(lo, -7);
  const uint16_t oob[2] = {10, 106};  // PRB 106 is outside a 106-PRB BWP
  EXPECT_FALSE(nr_prb_list_normalise(oob, 2, 106, bm, 9, &lo, &hi));
  std::vector<uint16_t> big(NR_PRB_SET_MAX + 1);
  for (int i = 0; i <= NR_PRB_SET_MAX; i++) big[i] = (uint16_t)i;
  EXPECT_FALSE(nr_prb_list_normalise(big.data(), NR_PRB_SET_MAX + 1, 275, bm, 9, &lo, &hi));
  EXPECT_TRUE(nr_prb_list_normalise(big.data(), NR_PRB_SET_MAX, 275, bm, 9, &lo, &hi));
  EXPECT_FALSE(nr_prb_list_normalise(ok, 0, 106, bm, 9, &lo, &hi));
}

TEST(PrbSet, FdraPrbsDecodesBothRivCasesAndTheModes) {
  uint16_t p[NR_PRB_SET_MAX];
  int t0 = -1;
  // RIV case 2 (L-1 > N/2): S=10, L=80 on N=106 -> RIV = N(N-L+1) + (N-1-S) = 2957.
  ASSERT_EQ(nr_fdra_prbs(2957, NR_FDRA_TYPE1, 0, 13, 0, 106, p, NR_PRB_SET_MAX, &t0), 80);
  EXPECT_EQ(t0, 0); EXPECT_EQ(p[0], 10); EXPECT_EQ(p[79], 89);
  EXPECT_EQ(nr_fdra_prbs(106 * 107 / 2, NR_FDRA_TYPE1, 0, 13, 0, 106, p, NR_PRB_SET_MAX, nullptr), 0);
  EXPECT_EQ(nr_fdra_prbs(0, NR_FDRA_TYPE0_CFG1, 14, 13, 0, 106, p, NR_PRB_SET_MAX, &t0), 0) << "empty bitmap";
  ASSERT_EQ(nr_fdra_prbs(1u << 13, NR_FDRA_TYPE0_CFG1, 14, 13, 0, 106, p, NR_PRB_SET_MAX, &t0), 8);  // RBG 0, P=8
  EXPECT_EQ(t0, 1); EXPECT_EQ(p[7], 7);
  // dynamicSwitch config2 (N_RBG 7, RIV 13 -> width 14): MSB 0 = bitmap in the 7 LSBs, MSB 1 = RIV.
  ASSERT_EQ(nr_fdra_prbs(1u, NR_FDRA_DYN_CFG2, 7, 13, 0, 106, p, NR_PRB_SET_MAX, &t0), 106 - 96);  // last RBG, P=16
  EXPECT_EQ(t0, 1);
  ASSERT_EQ(nr_fdra_prbs((1u << 13) | 2957u, NR_FDRA_DYN_CFG2, 7, 13, 0, 106, p, NR_PRB_SET_MAX, &t0), 80);
  EXPECT_EQ(t0, 0);
  EXPECT_EQ(nr_fdra_bits(NR_FDRA_TYPE1, 14, 13), 13);
  EXPECT_EQ(nr_fdra_bits(NR_FDRA_TYPE0_CFG2, 7, 13), 7);
  EXPECT_EQ(nr_fdra_bits(NR_FDRA_DYN_CFG1, 14, 13), 15);
  EXPECT_EQ(nr_fdra_bits(NR_FDRA_DYN_CFG2, 7, 13), 14);
}

TEST(PrbSet, DmrsOracleCrbAddsBwpStart) {
  // BWPStart = 0 must be bit-identical to a bare BWP-relative index (the lab cell).
  EXPECT_EQ(nr_dmrs_oracle_crb(0, 0), 0);
  EXPECT_EQ(nr_dmrs_oracle_crb(0, 5), 5);
  // BWPStart = 20, rb0 = 5 -> absolute CRB 25 (a dedicated BWP not starting at CRB 0).
  EXPECT_EQ(nr_dmrs_oracle_crb(20, 5), 25);
  // Carrier edge: a 273-PRB carrier's last valid CRB index is 272.
  EXPECT_EQ(nr_dmrs_oracle_crb(260, 12), 272);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
