#include <gtest/gtest.h>
#include "nr_isac.h"
#include "nr_isac_mode.h"

TEST(IsacMode, LegacyPassesThrough)
{
  for (const char *s : {(const char *)nullptr, ""}) {
    nr_isac_mode_cfg_t o;
    ASSERT_TRUE(nr_isac_mode_resolve(s, 0x5, 2, 1, &o));
    EXPECT_EQ(o.mode, NR_ISAC_MODE_LEGACY);
    EXPECT_EQ(o.sources, 0x5u);
    EXPECT_EQ(o.pdsch_decode, 2);
    EXPECT_EQ(o.pusch_decode, 1);
    EXPECT_FALSE(o.narrowed);
  }
}

TEST(IsacMode, TrafficDefaults)
{
  nr_isac_mode_cfg_t o;
  ASSERT_TRUE(nr_isac_mode_resolve("traffic", 0, -1, -1, &o));
  EXPECT_EQ(o.mode, NR_ISAC_MODE_TRAFFIC);
  EXPECT_EQ(o.sources, 0u);
  EXPECT_EQ(o.pdsch_decode, 1);
  EXPECT_EQ(o.pusch_decode, 1);
  EXPECT_FALSE(o.narrowed);
}

TEST(IsacMode, TrafficRefusesCfrSources)
{
  nr_isac_mode_cfg_t o;
  ASSERT_TRUE(nr_isac_mode_resolve("traffic", 1u << NR_ISAC_SRC_PDSCH_DATA, 2, 2, &o));
  EXPECT_EQ(o.sources, 0u);
  EXPECT_EQ(o.pdsch_decode, 1);
  EXPECT_EQ(o.pusch_decode, 1);
  EXPECT_TRUE(o.narrowed);
}

TEST(IsacMode, TrafficKeepsExplicitZero)
{
  nr_isac_mode_cfg_t o;
  ASSERT_TRUE(nr_isac_mode_resolve("traffic", 0, 0, 0, &o));
  EXPECT_EQ(o.pdsch_decode, 0);
  EXPECT_EQ(o.pusch_decode, 0);
  EXPECT_FALSE(o.narrowed);
}

TEST(IsacMode, UnknownModeFails)
{
  nr_isac_mode_cfg_t o;
  EXPECT_FALSE(nr_isac_mode_resolve("radar", 0, -1, -1, &o));
}

TEST(IsacMode, TrafficNarrowsDecodeAloneAndLogsViaNarrowed)
{
  nr_isac_mode_cfg_t o;
  ASSERT_TRUE(nr_isac_mode_resolve("traffic", 0, 2, -1, &o));
  EXPECT_EQ(o.pdsch_decode, 1);
  EXPECT_TRUE(o.narrowed);
  ASSERT_TRUE(nr_isac_mode_resolve("traffic", 0, -1, 2, &o));
  EXPECT_EQ(o.pusch_decode, 1);
  EXPECT_TRUE(o.narrowed);
}
