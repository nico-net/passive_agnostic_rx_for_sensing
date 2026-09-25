// Offline tests for the AL1 cover. Expected counts come from an independent Python enumeration of the
// TS 38.211 7.3.2.2 rules (session 2026-09-25), not from this module.
#include <chrono>
#include <cstdio>
#include <set>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_al1_map.h"
}

namespace {
struct Shape { int rb, d, maps, distinct, cover; };
const Shape kShapes[] = {{48, 1, 81, 88, 11}, {270, 1, 181, 90, 2}, {270, 2, 1081, 990, 11},
                         {270, 3, 946, 1080, 10}, {216, 2, 865, 792, 11}};

std::set<std::vector<uint16_t>> family(int rb, int d, nr_pdcch_al1_map_t m)
{
  std::set<std::vector<uint16_t>> f;
  for (int j = 0; j < rb * d / 6; j++) {
    uint16_t r[6];
    EXPECT_EQ(nr_pdcch_al1_regset(rb, d, m, j, r), 6);
    f.insert(std::vector<uint16_t>(r, r + 6));
  }
  return f;
}
}  // namespace

TEST(Al1Map, EnumerationMatchesTheSpecCount) {
  std::vector<nr_pdcch_al1_map_t> m(NR_PDCCH_AL1_MAX_MAPS);
  for (const auto &s : kShapes)
    EXPECT_EQ(nr_pdcch_al1_enumerate(s.rb, s.d, m.data(), (int)m.size()), s.maps) << s.rb << "x" << s.d;
}

TEST(Al1Map, RegsetIsSortedAndInRange) {
  uint16_t r[6];
  ASSERT_EQ(nr_pdcch_al1_regset(270, 2, {2, 3, 17}, 11, r), 6);
  for (int k = 0; k < 6; k++) {
    EXPECT_LT(r[k], 540);
    if (k) EXPECT_LT(r[k - 1], r[k]);
  }
  EXPECT_EQ(nr_pdcch_al1_regset(270, 2, {2, 3, 17}, 90, r), 0);  // CCE out of range
  EXPECT_EQ(nr_pdcch_al1_regset(270, 3, {2, 2, 0}, 0, r), 0);    // L=2 illegal at D=3
}

TEST(Al1Map, CoverUnionEqualsFullUnion) {
  std::vector<nr_pdcch_al1_map_t> all(NR_PDCCH_AL1_MAX_MAPS), cov(NR_PDCCH_AL1_MAX_COVER);
  for (const auto &s : kShapes) {
    const int n = nr_pdcch_al1_enumerate(s.rb, s.d, all.data(), (int)all.size());
    std::set<std::vector<uint16_t>> full, covered;
    for (int i = 0; i < n; i++)
      for (const auto &r : family(s.rb, s.d, all[i])) full.insert(r);
    const int nc = nr_pdcch_al1_cover(s.rb, s.d, cov.data(), (int)cov.size());
    for (int i = 0; i < nc; i++)
      for (const auto &r : family(s.rb, s.d, cov[i])) covered.insert(r);
    EXPECT_EQ((int)full.size(), s.distinct) << s.rb << "x" << s.d;
    EXPECT_EQ(covered, full) << s.rb << "x" << s.d;
    EXPECT_EQ(nc, s.cover) << s.rb << "x" << s.d;
    EXPECT_EQ(cov[0].bundle, 0) << "non-interleaved must lead the cover";
  }
}

TEST(Al1Map, CoverFitsTheLaneBoundForEveryLegalShape) {
  std::vector<nr_pdcch_al1_map_t> cov(NR_PDCCH_AL1_MAX_COVER);
  for (int d = 1; d <= 3; d++)
    for (int rb = 6; rb <= 270; rb += 6) {
      const int nc = nr_pdcch_al1_cover(rb, d, cov.data(), (int)cov.size());
      EXPECT_GT(nc, 0) << rb << "x" << d;
      EXPECT_LT(nc, NR_PDCCH_AL1_MAX_COVER) << rb << "x" << d << " would hit the cap";
    }
}

TEST(Al1Map, CoverIsFastEnoughForTheLanePath) {
  std::vector<nr_pdcch_al1_map_t> cov(NR_PDCCH_AL1_MAX_COVER);
  auto t0 = std::chrono::steady_clock::now();
  nr_pdcch_al1_cover(270, 2, cov.data(), (int)cov.size());
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("AL1 cover 270x2: %.2f ms\n", ms);
  EXPECT_LT(ms, 20.0);
}

TEST(Al1Map, NarrowKeepsTheTruthAndCollapsesToItsFamily) {
  const nr_pdcch_al1_map_t truth = {2, 3, 101};
  std::vector<nr_pdcch_al1_map_t> cand(NR_PDCCH_AL1_MAX_MAPS);
  int n = nr_pdcch_al1_enumerate(270, 2, cand.data(), (int)cand.size());
  uint16_t obs[3][6];
  for (int k = 0; k < 3; k++) ASSERT_EQ(nr_pdcch_al1_regset(270, 2, truth, 5 + 29 * k, obs[k]), 6);
  const int n1 = nr_pdcch_al1_narrow(270, 2, obs, 1, cand.data(), n);
  bool kept = false;
  for (int i = 0; i < n1; i++)
    kept |= cand[i].bundle == truth.bundle && cand[i].interleaver == truth.interleaver && cand[i].shift == truth.shift;
  EXPECT_TRUE(kept);
  const int n3 = nr_pdcch_al1_narrow(270, 2, obs, 3, cand.data(), n1);
  EXPECT_LE(n3, n1);
  EXPECT_EQ(nr_pdcch_al1_family_count(270, 2, cand.data(), n3), 1);
}

TEST(Al1Map, EveryBundle6MappingSharesTheNonInterleavedFamily) {
  std::vector<nr_pdcch_al1_map_t> all(NR_PDCCH_AL1_MAX_MAPS);
  const int n = nr_pdcch_al1_enumerate(270, 1, all.data(), (int)all.size());
  std::vector<nr_pdcch_al1_map_t> l6 = {{0, 0, 0}};
  for (int i = 0; i < n; i++)
    if (all[i].bundle == 6) l6.push_back(all[i]);
  EXPECT_GT(l6.size(), 1u);
  EXPECT_EQ(nr_pdcch_al1_family_count(270, 1, l6.data(), (int)l6.size()), 1);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
