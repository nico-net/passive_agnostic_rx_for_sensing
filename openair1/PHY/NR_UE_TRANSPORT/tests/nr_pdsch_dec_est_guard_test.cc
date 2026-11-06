// The identity guard of the decoder-estimate export: the estimate feeds the DM-RS tap only for the grant and hypothesis it was made for.
#include <gtest/gtest.h>
#include "nr_pdsch_dec_est_guard.h"

static nr_dec_est_id base_id() { return {.rnti = 0x1234, .first_rb = 10, .nsc = 12 * 217, .ports = 0x3, .slot = 4242, .nant = 4, .nl = 2}; }

TEST(DecEstGuard, MatchesAndEachMismatchFieldHasItsReason) {
  const nr_dec_est_id want = base_id();
  nr_dec_est_id h = base_id();
  EXPECT_EQ(0, nr_dec_est_match(&h, &want));
  h = base_id(); h.rnti ^= 1;        EXPECT_EQ(NR_DER_G_ID, nr_dec_est_match(&h, &want));
  h = base_id(); h.slot += 1;        EXPECT_EQ(NR_DER_G_ID, nr_dec_est_match(&h, &want));
  h = base_id(); h.nant = 2;         EXPECT_EQ(NR_DER_G_SHAPE, nr_dec_est_match(&h, &want));
  h = base_id(); h.nl = 1;           EXPECT_EQ(NR_DER_G_SHAPE, nr_dec_est_match(&h, &want)); // a retry body with another layer count
  h = base_id(); h.ports = 0x5;      EXPECT_EQ(NR_DER_G_SHAPE, nr_dec_est_match(&h, &want)); // another DM-RS port hypothesis
  h = base_id(); h.nsc -= 12;        EXPECT_EQ(NR_DER_G_WIDTH, nr_dec_est_match(&h, &want));
  h = base_id(); h.first_rb += 1;    EXPECT_EQ(NR_DER_G_WIDTH, nr_dec_est_match(&h, &want));
}

TEST(DecEstGuard, EstimateSymbolMustBeADmrsSymbolOfTheGrant) {
  const uint32_t pos = (1u << 2) | (1u << 11); // DM-RS at 2 and 11
  EXPECT_TRUE(nr_dec_est_sym_ok(2, pos, 2, 12));
  EXPECT_TRUE(nr_dec_est_sym_ok(11, pos, 2, 12)); // not the first DM-RS symbol: accepted, the record says which
  EXPECT_FALSE(nr_dec_est_sym_ok(5, pos, 2, 12)); // no DM-RS there
  EXPECT_FALSE(nr_dec_est_sym_ok(2, pos, 3, 11)); // DM-RS symbol outside the grant's symbols
  EXPECT_FALSE(nr_dec_est_sym_ok(12, pos | (1u << 12), 2, 10));
}

TEST(DecEstGuard, OneRunListIsExactAndAPermutedListFallsBack) {
  // a list that is one contiguous run: the tap's first run is the whole grant, so the widths agree and the decoder's estimate is used
  uint16_t one[6] = {10, 11, 12, 13, 14, 15};
  EXPECT_EQ(6, nr_prb_list_run(one, 6));
  nr_dec_est_id want = base_id(), have = base_id();
  have.nsc = want.nsc = 12 * nr_prb_list_run(one, 6);
  have.first_rb = want.first_rb = one[0];
  EXPECT_EQ(0, nr_dec_est_match(&have, &want));
  // a permuted (interleaved) list: the tap covers only the ascending prefix run, the decoder (seg_path) kept the segmented first run or nothing; a decoder estimate of the whole grant must not match
  uint16_t perm[6] = {10, 11, 14, 15, 12, 13};
  EXPECT_EQ(2, nr_prb_list_run(perm, 6));
  want.nsc = 12 * nr_prb_list_run(perm, 6);
  want.first_rb = perm[0];
  have.nsc = 12 * 6; // the whole-grant width
  EXPECT_EQ(NR_DER_G_WIDTH, nr_dec_est_match(&have, &want));
  have.nsc = 12 * 2; // first run only, same start: usable (segmented grant, labelled)
  EXPECT_EQ(0, nr_dec_est_match(&have, &want));
  // run edge cases
  EXPECT_EQ(0, nr_prb_list_run(one, 0));
  uint16_t sing[1] = {7};
  EXPECT_EQ(1, nr_prb_list_run(sing, 1));
}
