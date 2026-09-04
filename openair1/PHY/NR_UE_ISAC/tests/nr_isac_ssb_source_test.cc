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

// The parser function is C++ (nr_isac.cc), declared here with C++ linkage.
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
