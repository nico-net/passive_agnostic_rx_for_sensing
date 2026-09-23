// openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_gf2_rnti.h"
#include "openair1/PHY/gold.h"
}

static void gold_sequence(uint32_t c_init, int n_bits, uint8_t *out)
{
  uint32_t x1 = 0, x2 = c_init;
  out[0] = (uint8_t)(gold_generic(&x1, &x2, 1) & 1);
  for (int n = 1; n < n_bits; n++)
    out[n] = (uint8_t)(gold_generic(&x1, &x2, 0) & 1);
}

TEST(Gf2Rnti, RecoversTheKnownLabCellRnti) {
  const uint16_t n_id = 2, true_rnti = 0x4615;
  const int n_bits = 64;
  uint32_t c_init = ((uint32_t)true_rnti << 16) + n_id;
  uint8_t seq[64];
  gold_sequence(c_init, n_bits, seq);
  uint16_t recovered = 0;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered), 1);
  EXPECT_EQ(recovered, true_rnti);
}

TEST(Gf2Rnti, RecoversRntiZero) {
  const uint16_t n_id = 2, true_rnti = 0;
  const int n_bits = 64;
  uint32_t c_init = ((uint32_t)true_rnti << 16) + n_id;
  uint8_t seq[64];
  gold_sequence(c_init, n_bits, seq);
  uint16_t recovered = 0xFFFF;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered), 1);
  EXPECT_EQ(recovered, true_rnti);
}

TEST(Gf2Rnti, RecoversAnMsbOnlyRnti) {
  // Regression test for the bug found and fixed this session: the plan's original model dropped/
  // mis-shifted c_init's bit 31 (RNTI's MSB, bit 15), making 0x8000-differing RNTI pairs
  // indistinguishable. This RNTI has ONLY that bit set.
  const uint16_t n_id = 2, true_rnti = 0x8000;
  const int n_bits = 64;
  uint32_t c_init = ((uint32_t)true_rnti << 16) + n_id;
  uint8_t seq[64];
  gold_sequence(c_init, n_bits, seq);
  uint16_t recovered = 0;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered), 1);
  EXPECT_EQ(recovered, true_rnti);
}

TEST(Gf2Rnti, RejectsInsufficientBits) {
  uint8_t seq[8] = {0};
  uint16_t recovered = 0;
  EXPECT_EQ(nr_pdcch_gf2_rnti_recover(seq, 8, 2, &recovered), 0);
}
