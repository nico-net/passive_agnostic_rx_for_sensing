/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include <stdlib.h>
#include <vector>
#include <iostream>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
extern "C" {
#include "nr_td_legal.h"
#include "nr_rate_matching.h"
}
/* Link stubs for the LOG/config layer pulled in by the real nr_rate_matching.c. */
configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *, const char *, const int, const char *, const int) { abort(); }

/* Legality function from nr_pdsch_config_sweep_test.cc: models a cell admitting mapping type A only. */
static int32_t test_legal(int, int length, int start, int mapping_b, int add, int maxlen)
{
  return mapping_b ? 0 : 1 + start * 1000 + length * 40 + add * 3 + maxlen;
}

TEST(TdLegal, MaskAndCount)
{
  nr_td_mask_t a = {}, b = {}, d = {};
  nr_td_mask_set(&a, 1);
  nr_td_mask_set(&a, 70);
  nr_td_mask_set(&a, 200);
  nr_td_mask_set(&b, 70);
  nr_td_mask_set(&b, 200);
  nr_td_mask_set(&b, 201);
  nr_td_mask_and(&d, &a, 233);
  nr_td_mask_and(&d, &b, 233); /* d starts all-zero: AND keeps zero */
  EXPECT_EQ(nr_td_mask_count(&d, 233), 0);
  d = a;
  nr_td_mask_and(&d, &b, 233);
  EXPECT_EQ(nr_td_mask_count(&d, 233), 2);
  EXPECT_TRUE(nr_td_mask_get(&d, 70));
  EXPECT_FALSE(nr_td_mask_get(&d, 1));
}
TEST(TdLegal, CountMasksTail)
{
  nr_td_mask_t a = {};
  nr_td_mask_set(&a, 5);
  nr_td_mask_set(&a, 100);
  EXPECT_EQ(nr_td_mask_count(&a, 100), 1);
  EXPECT_EQ(nr_td_mask_count(&a, 101), 2);
}
TEST(TdLegal, TypicalGeometryIsFeasible)
{
  const nr_td_rm_geom_t g = {50000, 25000, 4, 1, 3, 8448, 0, 384, 1, 25344, 0};
  EXPECT_TRUE(nr_td_rm_feasible(&g));
}
TEST(TdLegal, ZeroGOrTbsIsInfeasible)
{
  nr_td_rm_geom_t g = {0, 25000, 4, 1, 3, 8448, 0, 384, 1, 25344, 0};
  EXPECT_FALSE(nr_td_rm_feasible(&g));
  g.G = 50000;
  g.tbs = 0;
  EXPECT_FALSE(nr_td_rm_feasible(&g));
}
/* Real tuples logged at the real decoder (nrLDPC_coding_segment_decoder.c, throwaway LOG_I) in two rfsim runs
 * (106 PRB and 273 PRB, 150 s each; evidence: tests/passive_rx/dgx_host_snapshot_2026-09-30/td4b_reject_vectors.txt).
 * All were accepted by the real rate recovery. Fields: G tbs Qm Nl C K F Zc BG Ncb rv. */
TEST(TdLegal, RealRfsimGeometriesAreFeasible)
{
  const nr_td_rm_geom_t v[] = {
      {30528, 19968, 2, 1, 3, 7040, 352, 320, 1, 21120, 0},
      {11448, 7552, 2, 1, 1, 7744, 168, 352, 1, 23232, 0},
      {30528, 7432, 2, 1, 2, 3840, 88, 384, 2, 19200, 0},
      {144144, 86040, 4, 1, 11, 8448, 600, 384, 1, 25344, 0},
      {78624, 52224, 2, 1, 7, 7744, 256, 352, 1, 23232, 0},
  };
  for (const nr_td_rm_geom_t &g : v)
    EXPECT_TRUE(nr_td_rm_feasible(&g));
}
TEST(TdSignature, McsTableOnlyDifferenceSharesSignatureWhenQmEqual)
{
  nr_pdsch_cfg_hypothesis_t a = {}, b = {};
  a.tda_start = b.tda_start = 1;
  a.tda_length = b.tda_length = 13;
  a.dmrs_mask = b.dmrs_mask = 0x804;
  a.mcs_table = 0;
  b.mcs_table = 1;
  EXPECT_EQ(nr_td_signature(&a, 1, 4), nr_td_signature(&b, 1, 4));
  EXPECT_NE(nr_td_signature(&a, 1, 4), nr_td_signature(&b, 1, 6)); /* different Qm => different LLRs */
}
TEST(TdSignature, DmrsMaskOrTdraChangesSignature)
{
  nr_pdsch_cfg_hypothesis_t a = {}, b = {};
  a.tda_start = b.tda_start = 1;
  a.tda_length = b.tda_length = 13;
  a.dmrs_mask = 0x804;
  b.dmrs_mask = 0x4;
  EXPECT_NE(nr_td_signature(&a, 1, 2), nr_td_signature(&b, 1, 2));
  b.dmrs_mask = 0x804;
  b.tda_length = 12;
  EXPECT_NE(nr_td_signature(&a, 1, 2), nr_td_signature(&b, 1, 2));
}
TEST(TdSignature, AddPosDistinguishesWhenMaskZero)
{
  /* In legacy/pure catalogs, dmrs_mask == 0 for all hypotheses, so dmrs_add_pos differences
   * must be captured in the signature. */
  nr_pdsch_cfg_hypothesis_t a = {}, b = {};
  a.tda_start = b.tda_start = 1;
  a.tda_length = b.tda_length = 13;
  a.dmrs_mask = b.dmrs_mask = 0; /* legacy: mask is always 0 */
  a.dmrs_add_pos = 0;
  b.dmrs_add_pos = 1;
  EXPECT_NE(nr_td_signature(&a, 1, 2), nr_td_signature(&b, 1, 2));
}
TEST(TdSignature, CountEdgeCases)
{
  nr_pdsch_cfg_hypothesis_t h = {};
  /* n=0 returns 0 (empty catalog, not an error) */
  EXPECT_EQ(nr_td_count_signatures(nullptr, 0, 1, nullptr), 0);
  /* n=1 returns 1 */
  h.tda_start = 1;
  h.tda_length = 13;
  std::vector<int> qm1(1, 2);
  EXPECT_EQ(nr_td_count_signatures(&h, 1, 1, qm1.data()), 1);
  /* duplicates: two identical hypotheses with same qm */
  std::vector<nr_pdsch_cfg_hypothesis_t> hyps(2, h);
  std::vector<int> qm2(2, 2);
  EXPECT_EQ(nr_td_count_signatures(hyps.data(), 2, 1, qm2.data()), 1);
  /* NULL pointers */
  EXPECT_EQ(nr_td_count_signatures(nullptr, 1, 1, qm1.data()), -1);
  EXPECT_EQ(nr_td_count_signatures(hyps.data(), 1, 1, nullptr), -1);
}
TEST(TdSignature, CountOnFullCatalog)
{
  static nr_pdsch_config_sweep_state_t st_legacy, st_real;

  /* Legacy (mask-0) catalog */
  nr_pdsch_config_sweep_init(&st_legacy, 4);
  std::vector<int> qm_legacy(st_legacy.n_hyp, 2);
  const int n_legacy = nr_td_count_signatures(st_legacy.hyp, st_legacy.n_hyp, 1, qm_legacy.data());
  EXPECT_GT(n_legacy, 0);
  EXPECT_LT(n_legacy, st_legacy.n_hyp);
  std::cout << "catalog " << st_legacy.n_hyp << " hypotheses -> " << n_legacy << " signatures (legacy mask-0 catalog)" << std::endl;

  /* Synthetic legality (test_legal mask, not a real DM-RS table) catalog */
  nr_pdsch_config_sweep_init_legal(&st_real, 4, 1 /*typeA only*/, test_legal);
  std::vector<int> qm_real(st_real.n_hyp, 2);
  const int n_real = nr_td_count_signatures(st_real.hyp, st_real.n_hyp, 1, qm_real.data());
  EXPECT_GT(n_real, 0);
  EXPECT_LT(n_real, st_real.n_hyp);
  std::cout << "catalog " << st_real.n_hyp << " hypotheses -> " << n_real << " signatures (synthetic legality: test_legal mask, not a real DM-RS table)" << std::endl;
}
/* Differential test against the REAL nr_rate_matching_ldpc_rx (linked from nr_rate_matching.c). E = 0 keeps every
 * data loop empty (they are bounded by k < E), so only the reject checks run; the function returns -1 exactly when it
 * rejects the geometry. */
TEST(TdLegal, MatchesRealRxRateMatching)
{
  srand(0x4b4b);
  static int16_t d[68 * 384 + 16];
  static int16_t soft[1];
  int n_rej = 0, n_acc = 0, n_rej_pos = 0;
  for (int it = 0; it < 6000; it++) {
    const int bg = 1 + (rand() & 1);
    const int zlist[] = {64, 128, 224, 256, 288, 320, 352, 384};
    const int Z = zlist[rand() % 8];
    const int C = 1 + rand() % 150;
    const int N = (bg == 1 ? 66 : 50) * Z;
    const int K = (bg == 1 ? 22 : 10) * Z;
    int F = (rand() % 4 == 0) ? K : rand() % 1200; /* includes F so large that K-F-2Z < 0 */
    const uint32_t tbslbrm = (rand() % 5 == 0) ? 0 : (uint32_t)(1 + rand() % 1500000) / (1 + rand() % 64);
    const int rv = rand() & 3;
    uint32_t Ncb = N;
    if (tbslbrm != 0) {
      const uint32_t nref = 3 * tbslbrm / (2 * C);
      Ncb = nref < (uint32_t)N ? nref : N;
    }
    nr_td_rm_geom_t g = {(uint32_t)(1 + rand() % 200000), (uint32_t)(1 + rand() % 100000), (1 + rand() % 4) * 2, 1 + rand() % 4, C, K, F, Z, bg, Ncb, rv};
    const int real_ok = nr_rate_matching_ldpc_rx(tbslbrm, bg, Z, d, soft, C, rv, 0, 0, F, (uint32_t)(K - F - 2 * Z)) != -1;
    EXPECT_EQ(nr_td_rm_feasible(&g), real_ok != 0) << "it " << it << " BG " << bg << " Z " << Z << " C " << C << " K " << K << " F " << F
                                                   << " tbslbrm " << tbslbrm << " Ncb " << Ncb;
    (real_ok ? n_acc : n_rej)++;
    if (!real_ok && K - F - 2 * Z >= 0)
      n_rej_pos++;
  }
  EXPECT_GE(n_rej_pos, 50); /* rejects from a non-negative Foffset > Ncb, not only the negative wrap */
  EXPECT_GE(n_rej, 100);
  EXPECT_GE(n_acc, 100);
}
/* Directed boundary sweep: F is set so that Foffset = K-F-2Zc is exactly Ncb-1, Ncb, Ncb+1. */
TEST(TdLegal, FoffsetBoundaryMatchesRealRx)
{
  srand(0x4b4c);
  static int16_t d[68 * 384 + 16];
  static int16_t soft[1];
  const int zlist[] = {64, 128, 224, 256, 288, 320, 352, 384};
  int n = 0;
  while (n < 300) {
    const int bg = 1 + (rand() & 1);
    const int Z = zlist[rand() % 8];
    const int C = 1 + rand() % 150;
    const int N = (bg == 1 ? 66 : 50) * Z;
    const int K = (bg == 1 ? 22 : 10) * Z;
    const uint32_t tbslbrm = 1 + rand() % 1500000;
    const uint32_t nref = 3 * tbslbrm / (2 * C);
    const uint32_t Ncb = nref < (uint32_t)N ? nref : N;
    if ((int)Ncb + 2 > K - 2 * Z) /* need F = K-2Z-Ncb-delta >= 0 for delta in -1..1 */
      continue;
    for (int delta = -1; delta <= 1; delta++) {
      const int F = K - 2 * Z - (int)Ncb - delta;
      ASSERT_GE(F, 0);
      nr_td_rm_geom_t g = {1000, 1000, 2, 1, C, K, F, Z, bg, Ncb, 0};
      const bool real_ok = nr_rate_matching_ldpc_rx(tbslbrm, bg, Z, d, soft, C, 0, 0, 0, F, (uint32_t)(K - F - 2 * Z)) != -1;
      EXPECT_EQ(nr_td_rm_feasible(&g), real_ok) << "BG " << bg << " Z " << Z << " C " << C << " delta " << delta << " Ncb " << Ncb;
      EXPECT_EQ(real_ok, delta <= 0) << "Foffset = Ncb + " << delta;
    }
    n++;
  }
}
TEST(TdLegal, CAbove255IsInfeasible)
{
  nr_td_rm_geom_t g = {50000, 25000, 4, 1, 256, 8448, 0, 384, 1, 25344, 0}; /* real C is uint8_t: 256 wraps to 0 */
  EXPECT_FALSE(nr_td_rm_feasible(&g));
}
/* Rates are nr_get_code_rate_dl() units: R x 1024 x 10 (4900 = 490/1024). */
TEST(TdEquiv, SameComputationSameKey) {
  nr_pdsch_cfg_hypothesis_t a = {}; a.tda_start = 2; a.tda_length = 12; a.dmrs_mask = 0x884; a.dmrs_max_len = 1;
  nr_pdsch_cfg_hypothesis_t b = a; b.mcs_table = 2; /* 64QAM vs 64QAM-LowSE: same LBRM class (Qm_max 6) */
  /* Same Qm and R (in real OAI tables only at MCS 29-31; the unit test passes equal values directly). */
  EXPECT_EQ(nr_td_equiv_key(&a, 1, 4, 4900), nr_td_equiv_key(&b, 1, 4, 4900));
}
TEST(TdEquiv, LbrmClassSplitsTable1) {
  nr_pdsch_cfg_hypothesis_t a = {}; a.tda_start = 2; a.tda_length = 12; a.dmrs_mask = 0x884; a.dmrs_max_len = 1;
  nr_pdsch_cfg_hypothesis_t b = a; b.mcs_table = 1; /* 256QAM: tbl_lbrm Qm_max 8 -> different Ncb when LBRM binds */
  EXPECT_NE(nr_td_equiv_key(&a, 1, 4, 4900), nr_td_equiv_key(&b, 1, 4, 4900));
}
TEST(TdEquiv, DifferentRateOrGeometryDifferentKey) {
  nr_pdsch_cfg_hypothesis_t a = {}; a.tda_start = 2; a.tda_length = 12; a.dmrs_mask = 0x884; a.dmrs_max_len = 1;
  nr_pdsch_cfg_hypothesis_t b = a;
  EXPECT_NE(nr_td_equiv_key(&a, 1, 4, 4900), nr_td_equiv_key(&b, 1, 4, 5530));
  b.k0 = 1;
  EXPECT_NE(nr_td_equiv_key(&a, 1, 4, 4900), nr_td_equiv_key(&b, 1, 4, 4900));
}
/* The signature never reaches bit 42 even with every field at the maximum its bit slot holds, and the rate field
 * (bits 42..59) neither spills into the LBRM bit nor is truncated at the largest real rate 9480. */
TEST(TdEquiv, RateFieldDoesNotOverlapSignature) {
  nr_pdsch_cfg_hypothesis_t h_max = {};
  h_max.tda_start = 15; h_max.tda_length = 15; h_max.k0 = 63; h_max.mapping_type = 3;
  h_max.dmrs_mask = 0xFFFF; h_max.dmrs_max_len = 3; h_max.dmrs_add_pos = 3;
  EXPECT_EQ(nr_td_signature(&h_max, 15, 15) >> 42, 0u);
  const uint64_t k_max = nr_td_equiv_key(&h_max, 15, 15, 0x3FFFF);
  EXPECT_EQ(k_max >> 42, 0x3FFFFu); /* rate fills 42..59 exactly, bit 60 clear for table 0 */
  EXPECT_EQ(k_max & ((1ull << 42) - 1), nr_td_signature(&h_max, 15, 15));
  h_max.mcs_table = 1;
  EXPECT_EQ(nr_td_equiv_key(&h_max, 15, 15, 0x3FFFF) >> 42, 0x3FFFFu | (1u << 18)); /* LBRM bit 60 distinct */
  h_max.mcs_table = 0;
  EXPECT_EQ((nr_td_equiv_key(&h_max, 15, 15, 9480) >> 42), 9480u);
  EXPECT_NE(nr_td_equiv_key(&h_max, 15, 15, 9480), nr_td_equiv_key(&h_max, 15, 15, 9479));
  EXPECT_NE(nr_td_equiv_key(&h_max, 15, 15, 9480), nr_td_equiv_key(&h_max, 15, 15, 0x3FFFF));
}
int main(int argc, char **argv)
{
  logInit(); /* the real rate matching reports rejects through LOG_E */
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
