/* Receiver source enums and PBCH subcarrier-axis geometry. */
#include <gtest/gtest.h>
#include "nr_isac.h"
TEST(SsbSource, EnumCountIncludesSsb) {
  // NR_ISAC_SRC_COUNT sizes the enabled-source bitmask (sources_mask) — every source must be
  // strictly below it or its bit is silently unreachable.
  EXPECT_LT(static_cast<int>(NR_ISAC_SRC_SSB), static_cast<int>(NR_ISAC_SRC_COUNT));
}

TEST(SsbSource, IlluminatorIsDownlinkLikeEveryOtherDlSource) {
  // nr_isac_source_illum() special-cases only the two PUSCH (uplink-illuminated) sources; SSB is
  // transmitted by the gNB, so it must fall through to NR_ISAC_ILLUM_DL like csi_rs/pdsch_*.
  EXPECT_EQ(nr_isac_source_illum(NR_ISAC_SRC_SSB), static_cast<unsigned>(NR_ISAC_ILLUM_DL));
}

extern "C" {
#include "nr_isac_ssb_axis.h"
}

// Real cell facts from this project's own Phase 1 work (PHASE1_CSS0_AUTOCONF_HANDOVER.md and the
// live-verified UE blind scan "GSCN: 8019, with SSB offset: 1478"), not synthetic numbers — ARFCN
// 630000, PCI 2, 273 PRB / 100 MHz, SCS 30 kHz, ssb_start_subcarrier = 1478.
TEST(SsbAxis, MatchesTheLiveVerifiedCellAtIndexZero) {
  uint32_t k_abs[240];
  // k_ssb = 0 is the common case for this deployment (no sub-RB SSB shift); ofdm_symbol_size for
  // 273 PRB / 30 kHz numerology 1 is 4096 (the standard OAI FFT size at this bandwidth/SCS combo
  // -- cross-check against fp->ofdm_symbol_size in a live run before trusting this constant, see
  // Task 3's Step 1).
  nr_isac_ssb_k_abs(/*ssb_start_subcarrier=*/1478, /*k_ssb=*/0, /*ofdm_symbol_size=*/4096, k_abs);
  // The SAME derivation Phase 1 used and log-verified for this exact cell:
  // ssb_offset_point_a = (ssb_start_subcarrier - k_ssb) / 12 = 1478 / 12 = 123 (integer division,
  // matching nr_ue_dci_configuration.c's own ssb_offset_point_a computation).
  // k_abs[0] must be exactly ssb_offset_point_a * 12 (the RB-aligned start), because index 0 of the
  // 240-wide SSB channel estimate sits at the first subcarrier of the SSB's own lowest RB.
  const uint32_t expected_ssb_offset_point_a = (1478 - 0) / 12;
  EXPECT_EQ(k_abs[0], expected_ssb_offset_point_a * 12);
}

TEST(SsbAxis, IsMonotonicAndContiguousOverAllTwoHundredFortySubcarriers) {
  uint32_t k_abs[240];
  nr_isac_ssb_k_abs(1478, 0, 4096, k_abs);
  for (int i = 1; i < 240; i++) {
    // Every fusion source shares one absolute-subcarrier axis; a gap or wraparound here would
    // silently misplace SSB's Ĥ relative to every other source's k_abs on the same carrier.
    EXPECT_EQ(k_abs[i], k_abs[i - 1] + 1) << "discontinuity at index " << i;
  }
}

TEST(SsbAxis, HandlesAWraparoundNearTheFftEdgeWithoutOverflowing) {
  uint32_t k_abs[240];
  // A pathological but legal input: an SSB placed such that its 240-subcarrier span would cross
  // the FFT buffer's own wraparound point (mirrors how existing k index computations elsewhere in
  // this codebase, e.g. dci_nr.c's `(cs_sc + rb*12 + ...) % symb_sz`, are all modulo the symbol
  // size). Use a small ofdm_symbol_size so 240 subcarriers genuinely wrap.
  nr_isac_ssb_k_abs(/*ssb_start_subcarrier=*/200, /*k_ssb=*/0, /*ofdm_symbol_size=*/256, k_abs);
  for (int i = 0; i < 240; i++) {
    EXPECT_LT(k_abs[i], 256u);
  }
}

TEST(SsbAxis, NeverProducesGarbageOnANegativeOffset) {
  uint32_t k_abs[240];
  // A mismatched/un-normalized k_ssb (demonstrating the guard from Finding 2): ssb_start_subcarrier=0
  // is tiny (typical for some ARFCN values), and k_ssb=100 is large-ish but still sub-240, giving
  // ssb_offset_point_a = (0 - 100) / 12 = -9 (integer division is floor-ish on negatives in C),
  // so base_sc = -108, which is negative. Without the guard, (base_sc + i) % ofdm_symbol_size
  // would produce negative remainders that cast to near-UINT32_MAX garbage. With the guard, all
  // results must be valid indices [0, ofdm_symbol_size).
  nr_isac_ssb_k_abs(/*ssb_start_subcarrier=*/0, /*k_ssb=*/100, /*ofdm_symbol_size=*/4096, k_abs);
  for (int i = 0; i < 240; i++) {
    EXPECT_LT(k_abs[i], 4096u) << "index " << i << " produced garbage (value " << k_abs[i] << ")";
  }
}

TEST(SsbAxis, SsbKssbNotHardcoded) {
  const int k_ssb = nr_isac_ssb_normalize_k_ssb(10, 1, 1);
  ASSERT_EQ(k_ssb, 5);
  uint32_t k_abs[240];
  nr_isac_ssb_k_abs(149, k_ssb, 4096, k_abs);
  EXPECT_EQ(k_abs[0], 144u);
  EXPECT_EQ(nr_isac_ssb_normalize_k_ssb(25, 1, 1), 25);
}
