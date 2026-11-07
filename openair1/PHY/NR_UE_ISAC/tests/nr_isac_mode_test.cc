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

static unsigned bit(int s) { return 1u << s; }

TEST(IsacMode, SensingDefaults)
{
  nr_isac_mode_cfg_t o;
  ASSERT_TRUE(nr_isac_mode_resolve("sensing", 0, -1, -1, &o));
  EXPECT_EQ(o.mode, NR_ISAC_MODE_SENSING);
  EXPECT_EQ(o.sources, bit(NR_ISAC_SRC_SSB) | bit(NR_ISAC_SRC_CSI_RS) | bit(NR_ISAC_SRC_PDSCH_DMRS) | bit(NR_ISAC_SRC_PDSCH_DATA)
                           | bit(NR_ISAC_SRC_PUSCH_DMRS) | bit(NR_ISAC_SRC_SRS)); // no PRACH unless explicit
  EXPECT_EQ(o.pdsch_decode, 2);
  EXPECT_EQ(o.pusch_decode, 1);
  EXPECT_EQ(o.min_prb, 51);
  EXPECT_FALSE(o.narrowed);
}

TEST(IsacMode, SensingExplicitSourcesAllOutsideSetFallBack)
{
  nr_isac_mode_cfg_t o;
  ASSERT_TRUE(nr_isac_mode_resolve("sensing", 1u << 20, -1, -1, &o)); // no valid source left: never 0 ("unset")
  EXPECT_NE(o.sources, 0u);
  EXPECT_TRUE(o.narrowed);
}

TEST(IsacMode, SensingNarrowedByExplicitSources)
{
  nr_isac_mode_cfg_t o;
  const unsigned want = bit(NR_ISAC_SRC_SSB) | bit(NR_ISAC_SRC_PDSCH_DMRS);
  ASSERT_TRUE(nr_isac_mode_resolve("sensing", want, 1, 0, &o));
  EXPECT_EQ(o.sources, want);
  EXPECT_EQ(o.pdsch_decode, 1);
  EXPECT_EQ(o.pusch_decode, 0);
  EXPECT_FALSE(o.narrowed); // narrowing is what the key is for; only widening attempts are reported
  // never widens: a source outside the set (pusch_data) and decode levels above the mode's are cut
  ASSERT_TRUE(nr_isac_mode_resolve("sensing", want | bit(NR_ISAC_SRC_PUSCH_DATA), 3, 2, &o));
  EXPECT_EQ(o.sources, want);
  EXPECT_EQ(o.pdsch_decode, 2);
  EXPECT_EQ(o.pusch_decode, 1);
  EXPECT_TRUE(o.narrowed);
  // prach only when explicit
  ASSERT_TRUE(nr_isac_mode_resolve("sensing", bit(NR_ISAC_SRC_PRACH), -1, -1, &o));
  EXPECT_EQ(o.sources, bit(NR_ISAC_SRC_PRACH));
}

TEST(IsacMode, MinPrbKey)
{
  EXPECT_EQ(nr_isac_sense_min_prb(), 51);
  EXPECT_TRUE(nr_isac_sense_min_prb_set(64));
  EXPECT_EQ(nr_isac_sense_min_prb(), 64);
  EXPECT_FALSE(nr_isac_sense_min_prb_set(0));
  EXPECT_EQ(nr_isac_sense_min_prb(), 51);
  EXPECT_TRUE(nr_isac_sense_min_prb_set(64));
  EXPECT_FALSE(nr_isac_sense_min_prb_set(-5));
  EXPECT_EQ(nr_isac_sense_min_prb(), 51);
}

TEST(IsacMode, ContinuityKeys)
{
  EXPECT_EQ(nr_isac_sense_min_prb_floor(), 32);
  EXPECT_EQ(nr_isac_sense_cont_target_ms(), 25);
  EXPECT_TRUE(nr_isac_sense_min_prb_floor_set(16));
  EXPECT_EQ(nr_isac_sense_min_prb_floor(), 16);
  EXPECT_FALSE(nr_isac_sense_min_prb_floor_set(0));
  EXPECT_EQ(nr_isac_sense_min_prb_floor(), 32);
  EXPECT_TRUE(nr_isac_sense_cont_target_ms_set(50));
  EXPECT_EQ(nr_isac_sense_cont_target_ms(), 50);
  EXPECT_FALSE(nr_isac_sense_cont_target_ms_set(-1));
  EXPECT_EQ(nr_isac_sense_cont_target_ms(), 25);
}

TEST(IsacMode, TrafficOnlySolverHooks)
{
  for (nr_isac_mode_t m : {NR_ISAC_MODE_LEGACY, NR_ISAC_MODE_SENSING}) {
    nr_isac_mode_set(m);
    EXPECT_FALSE(nr_traffic_hooks_on()); /* E3/E4 hooks (rt.c t_mode) stay off: sensing keeps the legacy solver */
  }
  nr_isac_mode_set(NR_ISAC_MODE_TRAFFIC);
  EXPECT_TRUE(nr_traffic_hooks_on());
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
}
