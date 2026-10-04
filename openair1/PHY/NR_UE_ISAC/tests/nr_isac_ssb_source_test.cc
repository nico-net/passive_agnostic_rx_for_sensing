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


TEST(SsbAxis, ActualReKeepsSubRbOffset)
{
  uint32_t k[240];
  nr_isac_ssb_k_abs(1478,0,4096,k);
  for(int i=0;i<240;i++) EXPECT_EQ(k[i],1478u+i);
}
TEST(SsbAxis, AbsoluteAxisDoesNotWrapAtFftEdge)
{
  uint32_t k[240];
  nr_isac_ssb_k_abs(200,0,256,k);
  for(int i=0;i<240;i++) EXPECT_EQ(k[i],200u+i);
}
TEST(SsbAxis, SsbKssbNotHardcoded)
{
  const int k_ssb=nr_isac_ssb_normalize_k_ssb(10,1,1);
  EXPECT_EQ(k_ssb,5);
  uint32_t k[240];
  nr_isac_ssb_k_abs(149,k_ssb,4096,k);
  EXPECT_EQ(k[0],149u);
  EXPECT_EQ(nr_isac_ssb_normalize_k_ssb(25,1,1),25);
}
