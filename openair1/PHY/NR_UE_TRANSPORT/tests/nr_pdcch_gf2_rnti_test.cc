// openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc
#include <gtest/gtest.h>
#include <vector>
extern "C" {
#include "nr_pdcch_gf2_rnti.h"
}

// INDEPENDENT reference generator: a direct, bit-serial TS 38.211 5.2.1 Gold-sequence LFSR that
// shares NO code with nr_pdcch_gf2_rnti.c's gold_seq() (which calls the real, word-batched
// gold_generic() from gold.h). This is deliberate -- the module's first version had two real bugs
// (wrong bit extraction from gold_generic's word output, and an unmasked c_init) that were invisible
// to its own tests specifically BECAUSE the test helper was a copy of the code under test and
// therefore could not disagree with it. This reference is cross-checked against the real
// gold_generic() independently, in Python, in docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py
// (96 bits, 21 (rnti, n_id) combinations) before being trusted here.
static void ref_gold_bitserial(uint32_t c_init31, int n_bits, uint8_t *out)
{
  const int Nc = 1600;
  const int total = Nc + n_bits;
  std::vector<uint8_t> x1(total + 31, 0), x2(total + 31, 0);
  x1[0] = 1;
  for (int n = 0; n < 31; n++)
    x2[n] = (uint8_t)((c_init31 >> n) & 1);
  for (int n = 0; n < total; n++) {
    x1[n + 31] = x1[n + 3] ^ x1[n];
    x2[n + 31] = (uint8_t)(x2[n + 3] ^ x2[n + 2] ^ x2[n + 1] ^ x2[n]);
  }
  for (int n = 0; n < n_bits; n++)
    out[n] = (uint8_t)(x1[Nc + n] ^ x2[Nc + n]);
}

// TS 38.211 7.3.2.3, masked exactly like this project's own dci_nr.c:1073 (`% (1U << 31)`).
static uint32_t ref_c_init(uint32_t rnti, uint16_t n_id)
{
  return ((rnti << 16) + n_id) % (1u << 31);
}

TEST(Gf2Rnti, RecoversTheKnownLabCellRntiLow15Bits) {
  const uint16_t n_id = 2, true_rnti = 0x4615;
  const int n_bits = 64;
  uint8_t seq[64];
  ref_gold_bitserial(ref_c_init(true_rnti, n_id), n_bits, seq);
  uint16_t recovered = 0xFFFF;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered), 1);
  EXPECT_EQ(recovered, true_rnti & 0x7FFF);
}

TEST(Gf2Rnti, RecoversRntiZero) {
  const uint16_t n_id = 2, true_rnti = 0;
  const int n_bits = 64;
  uint8_t seq[64];
  ref_gold_bitserial(ref_c_init(true_rnti, n_id), n_bits, seq);
  uint16_t recovered = 0xFFFF;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered), 1);
  EXPECT_EQ(recovered, true_rnti);
}

// Regression test for the bug an external review found: RNTI bit 15 maps to c_init's bit 31, which
// TS 38.211's `mod 2^31` (dci_nr.c:1073) removes before the sequence is generated at all. Two RNTIs
// differing ONLY in bit 15 must therefore recover to the SAME 15-bit value -- if they didn't, the
// module would be claiming to recover information the real scrambling sequence physically does not
// carry.
TEST(Gf2Rnti, MsbOnlyDifferenceIsUnrecoverableByDesign) {
  const uint16_t n_id = 2;
  const int n_bits = 64;
  uint8_t seq_lo[64], seq_hi[64];
  ref_gold_bitserial(ref_c_init(0x4615, n_id), n_bits, seq_lo);
  ref_gold_bitserial(ref_c_init(0xC615, n_id), n_bits, seq_hi);
  EXPECT_EQ(memcmp(seq_lo, seq_hi, n_bits), 0) << "reference generator itself must agree with the "
                                                   "physical fact under test before the module is judged";
  uint16_t recovered_lo = 0xFFFF, recovered_hi = 0xFFFF;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq_lo, n_bits, n_id, &recovered_lo), 1);
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq_hi, n_bits, n_id, &recovered_hi), 1);
  EXPECT_EQ(recovered_lo, recovered_hi);
  EXPECT_EQ(recovered_lo, 0x4615 & 0x7FFF);
}

TEST(Gf2Rnti, RejectsInsufficientBits) {
  uint8_t seq[8] = {0};
  uint16_t recovered = 0;
  EXPECT_EQ(nr_pdcch_gf2_rnti_recover(seq, 8, 2, &recovered), 0);
}

// Regression test for the bug an external review found: the first version had no consistency check
// and reported success on essentially any input. Random noise must be REJECTED, not silently
// "solved" into a wrong RNTI.
TEST(Gf2Rnti, RejectsRandomNoise) {
  const uint16_t n_id = 2;
  const int n_bits = 64;
  unsigned seed = 12345;
  int false_accepts = 0;
  for (int trial = 0; trial < 200; trial++) {
    uint8_t seq[64];
    for (int i = 0; i < n_bits; i++) {
      seed = seed * 1103515245u + 12345u;
      seq[i] = (uint8_t)((seed >> 16) & 1);
    }
    uint16_t recovered = 0;
    if (nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered) == 1)
      false_accepts++;
  }
  EXPECT_EQ(false_accepts, 0);
}

// Regression test for the same missing-consistency-check bug: a real, valid sequence scored against
// the WRONG n_id must also be rejected, not resolved into some other n_id's answer.
TEST(Gf2Rnti, RejectsWrongNid) {
  const uint16_t true_n_id = 2, wrong_n_id = 3, true_rnti = 0x4615;
  const int n_bits = 64;
  uint8_t seq[64];
  ref_gold_bitserial(ref_c_init(true_rnti, true_n_id), n_bits, seq);
  uint16_t recovered = 0;
  EXPECT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, wrong_n_id, &recovered), 0);
}
