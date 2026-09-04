/*! \file openair1/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc
 * \brief Phase 2 (roadmap artifact): the type-system plumbing for NR_ISAC_SRC_SSB, tested in
 * isolation from any RT tap. Written first per this project's own precedent — a prior source
 * ("pusch_dmrs") silently returned the WRONG bit from source_bit_from_token() because a branch
 * forgot the `1u <<` shift and happened to collide with a different source's already-shifted
 * value; nothing caught it until it was found by inspection. This test exists so the same class
 * of bug fails a build instead of shipping quietly for a different source.
 */
#include <string>

#include <gtest/gtest.h>

extern "C" {
#include "nr_isac.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}

#include "detection_report.h"

// Stubs for external dependencies (LOG/CONFIG_LIB)
extern "C" configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int a)
{
  (void)file;
  (void)function;
  (void)line;
  (void)s;
  (void)a;
}

// Use the qualified name for the function from the namespace
using nr_isac::nr_isac_source_to_ref_type;

// Forward declaration for C++ function (defined in nr_isac.cc, outside anonymous namespace)
uint32_t source_bit_from_token(const std::string& tok);

TEST(SsbSource, TokenMapsToTheShiftedSsbBitNotABareEnum) {
  const uint32_t bit = source_bit_from_token("ssb");
  EXPECT_EQ(bit, 1u << NR_ISAC_SRC_SSB);
  // The exact failure mode this guards: a forgotten shift would return NR_ISAC_SRC_SSB itself
  // (6), which is NOT equal to 1u << NR_ISAC_SRC_SSB (64) and would silently collide with
  // whatever OTHER source happens to have bit value 6 if the enum ever grows that far — assert
  // the shift explicitly rather than only checking non-zero.
  EXPECT_NE(bit, static_cast<uint32_t>(NR_ISAC_SRC_SSB));
}

TEST(SsbSource, UnknownTokenStillReturnsZero) {
  // Regression guard: adding a new branch must not accidentally widen the fallthrough.
  EXPECT_EQ(source_bit_from_token("not_a_real_source"), 0u);
}

TEST(SsbSource, RefTypeIsExplicitNotTheDefaultFallback) {
  // detection_report.cc's switch defaults to "csi_rs" for anything unhandled. This assertion
  // fails loudly if a future refactor removes the SSB case and lets it fall through silently.
  EXPECT_STREQ(nr_isac_source_to_ref_type(NR_ISAC_SRC_SSB), "ssb");
  EXPECT_STRNE(nr_isac_source_to_ref_type(NR_ISAC_SRC_SSB), "csi_rs");
}

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
