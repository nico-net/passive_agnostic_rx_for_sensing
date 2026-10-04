/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* K32: the passive PDSCH chest cache hit test. A hit must mean "the estimate a miss would compute is
 * bit-identical", so every input of the estimator is in the key. */
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_chest_key.h"
}
static nr_pdsch_chest_key_t K()
{
  nr_pdsch_chest_key_t k = {};
  k.slot = 5;
  k.dmrs_pos = 0x884;
  k.dmrs_ports = 1;
  k.n_ant = 4;
  k.only_ant = -1;
  return k;
}
TEST(ChestKey, PortsAboveEightDoNotAlias)
{
  auto a = K(), b = K();
  a.dmrs_ports = 0x001;
  b.dmrs_ports = 0x100;
  EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b));
}
TEST(ChestKey, SegmentedNeverHits)
{
  auto a = K(), b = K();
  a.seg = b.seg = 1;
  EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b));
}
TEST(ChestKey, FoOrAntennaCountChangeMisses)
{
  auto a = K(), b = K();
  b.fo_hz = 10;
  EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b));
  b = K();
  b.n_ant = 1;
  EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b));
}
TEST(ChestKey, IdenticalHits)
{
  auto a = K(), b = K();
  EXPECT_TRUE(nr_pdsch_chest_key_eq(&a, &b));
}
/* Every remaining estimator input: changing any one of them alone must miss. */
TEST(ChestKey, EveryFieldIsInTheKey)
{
  const auto base = K();
  nr_pdsch_chest_key_t v[16];
  for (auto &x : v)
    x = base;
  v[0].slot = 6;
  v[1].dmrs_pos = 0x084; /* full-slot mask: a probe truncated to [S, horizon) is a different set */
  v[2].cfg_type = 1;
  v[3].nscid = 1;
  v[4].cdm = 2;
  v[5].nl = 2;
  v[6].scr = 7;
  v[7].rb_lo = 3;
  v[8].rb_n = 10;
  v[9].bwp_start = 1;
  v[10].bwp_size = 51;
  v[11].ref_point = 1;
  v[12].only_ant = 0;
  v[13].dmrs_ports = 0x003;
  v[14].n_ant = 2;
  v[15].fo_hz = -0.5;
  for (int i = 0; i < 16; i++)
    EXPECT_FALSE(nr_pdsch_chest_key_eq(&base, &v[i])) << "field " << i;
}
TEST(ChestKey, OneSidedSegNeverHits)
{
  auto a = K(), b = K();
  b.seg = 1;
  EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b));
  EXPECT_FALSE(nr_pdsch_chest_key_eq(&b, &a));
}
