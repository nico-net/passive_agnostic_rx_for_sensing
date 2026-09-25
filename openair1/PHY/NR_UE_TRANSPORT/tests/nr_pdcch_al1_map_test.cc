// Offline tests for the AL1 cover. Expected counts come from an independent Python enumeration of the
// TS 38.211 7.3.2.2 rules (session 2026-09-25), not from this module.
#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstring>
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

TEST(Al1Map, CoverSurvivesTheLaneTypeRoundTrip) {
  // Mirrors al1_cover_for_span() in nr_pdcch_blind_monitor.c: same field order, same widths.
  struct lane_map { uint8_t bundle; uint8_t interleaver; uint16_t shift; };
  static_assert(sizeof(lane_map) == sizeof(nr_pdcch_al1_map_t), "layout drift");
  std::vector<nr_pdcch_al1_map_t> cov(NR_PDCCH_AL1_MAX_COVER);
  const int nc = nr_pdcch_al1_cover(216, 2, cov.data(), (int)cov.size());
  ASSERT_EQ(nc, 11);
  for (int i = 0; i < nc; i++) {
    const lane_map l = {cov[i].bundle, cov[i].interleaver, cov[i].shift};
    EXPECT_EQ(l.bundle, cov[i].bundle);
    EXPECT_EQ(l.interleaver, cov[i].interleaver);
    EXPECT_EQ(l.shift, cov[i].shift);
  }
}

namespace {
std::vector<uint16_t> union_buf() { return std::vector<uint16_t>((size_t)NR_PDCCH_AL1_UNION_MAX * 6); }
uint16_t (*as_sets(std::vector<uint16_t> &b))[6] { return reinterpret_cast<uint16_t (*)[6]>(b.data()); }
}  // namespace

TEST(Al1Map, UnionOfOneFamilyIsItsCces) {
  const nr_pdcch_al1_map_t m = {0, 0, 0};
  auto buf = union_buf();
  const int n = nr_pdcch_al1_union(270, 1, &m, 1, as_sets(buf), NR_PDCCH_AL1_UNION_MAX);
  EXPECT_EQ(n, 45);
  for (int j = 0; j < 45; j++) {  // family-major: cand[0]'s sets come first, in CCE order
    uint16_t r[6];
    ASSERT_EQ(nr_pdcch_al1_regset(270, 1, m, j, r), 6);
    EXPECT_TRUE(std::equal(r, r + 6, as_sets(buf)[j])) << "cce " << j;
  }
}

TEST(Al1Map, UnionContainsTheTruthAfterNarrowing) {
  const nr_pdcch_al1_map_t truth = {2, 3, 101};
  std::vector<nr_pdcch_al1_map_t> cand(NR_PDCCH_AL1_MAX_MAPS);
  const int n = nr_pdcch_al1_enumerate(270, 2, cand.data(), (int)cand.size());
  uint16_t obs[1][6];
  ASSERT_EQ(nr_pdcch_al1_regset(270, 2, truth, 7, obs[0]), 6);
  const int n1 = nr_pdcch_al1_narrow(270, 2, obs, 1, cand.data(), n);
  const int fam = nr_pdcch_al1_family_count(270, 2, cand.data(), n1);
  auto buf = union_buf();
  const int nu = nr_pdcch_al1_union(270, 2, cand.data(), n1, as_sets(buf), NR_PDCCH_AL1_UNION_MAX);
  printf("270x2 truth 2/3/101, one observation: %d mappings, %d families, union %d sets\n", n1, fam, nu);
  ASSERT_LE(nu, NR_PDCCH_AL1_UNION_MAX);
  std::set<std::vector<uint16_t>> u;
  for (int i = 0; i < nu; i++) u.insert(std::vector<uint16_t>(as_sets(buf)[i], as_sets(buf)[i] + 6));
  EXPECT_EQ((int)u.size(), nu) << "union must be deduplicated";
  const auto t = family(270, 2, truth);
  EXPECT_EQ((int)t.size(), 90);
  for (const auto &r : t) EXPECT_TRUE(u.count(r)) << "truth set missing from the union";
  EXPECT_LE(nu, 90 * fam);
}

TEST(Al1Map, UnionCoversEverySurvivingFamilyWhenOneObservationIsAmbiguous) {
  // Measured: truth 2/2/0 observed at CCE 0 leaves 45 AL1 families at 270x2 (truth 2/3/101 above always pins 1).
  const nr_pdcch_al1_map_t truth = {2, 2, 0};
  std::vector<nr_pdcch_al1_map_t> cand(NR_PDCCH_AL1_MAX_MAPS);
  int n = nr_pdcch_al1_enumerate(270, 2, cand.data(), (int)cand.size());
  uint16_t obs[1][6];
  ASSERT_EQ(nr_pdcch_al1_regset(270, 2, truth, 0, obs[0]), 6);
  n = nr_pdcch_al1_narrow(270, 2, obs, 1, cand.data(), n);
  const int fam = nr_pdcch_al1_family_reps(270, 2, cand.data(), n);
  EXPECT_EQ(fam, 45);
  auto buf = union_buf();
  const int nu = nr_pdcch_al1_union(270, 2, cand.data(), fam, as_sets(buf), NR_PDCCH_AL1_UNION_MAX);
  ASSERT_LE(nu, NR_PDCCH_AL1_UNION_MAX);
  EXPECT_GT(nu, 90);
  EXPECT_LE(nu, 90 * fam);
  std::set<std::vector<uint16_t>> u;
  for (int i = 0; i < nu; i++) u.insert(std::vector<uint16_t>(as_sets(buf)[i], as_sets(buf)[i] + 6));
  for (int f = 0; f < fam; f++)
    for (const auto &r : family(270, 2, cand[f])) EXPECT_TRUE(u.count(r)) << "family " << f;
  for (const auto &r : family(270, 2, truth)) EXPECT_TRUE(u.count(r));
}

TEST(Al1Map, SurvivingFamiliesFitTheBankBoundForEveryLegalShape) {
  // Families left by ONE AL1 observation = distinct AL1 families containing that REG set. Exhaustive
  // over every legal shape and every distinct AL1 REG set; the argmax is cross-checked through the
  // production narrow() + family_count().
  std::vector<nr_pdcch_al1_map_t> all(NR_PDCCH_AL1_MAX_MAPS);
  int gmax = 0, grb = 0, gd = 0;
  for (int d = 1; d <= 3; d++)
    for (int rb = 6; rb <= 270; rb += 6) {
      const int n = nr_pdcch_al1_enumerate(rb, d, all.data(), (int)all.size());
      const int ncce = rb * d / 6;
      std::set<std::vector<uint64_t>> fams;
      for (int i = 0; i < n; i++) {
        std::vector<uint64_t> f(ncce);
        for (int j = 0; j < ncce; j++) {
          uint16_t r[6];
          ASSERT_EQ(nr_pdcch_al1_regset(rb, d, all[i], j, r), 6);
          uint64_t k = 0;
          for (int q = 0; q < 6; q++) k = (k << 10) | r[q];
          f[j] = k;
        }
        std::sort(f.begin(), f.end());
        fams.insert(f);
      }
      std::map<uint64_t, int> per_set;
      for (const auto &f : fams)
        for (uint64_t k : f) per_set[k]++;
      int mx = 0;
      uint64_t arg = 0;
      for (const auto &kv : per_set)
        if (kv.second > mx) { mx = kv.second; arg = kv.first; }
      uint16_t obs[1][6];
      for (int q = 5; q >= 0; q--) { obs[0][q] = (uint16_t)(arg & 1023); arg >>= 10; }
      std::vector<nr_pdcch_al1_map_t> c(all.begin(), all.begin() + n);
      const int m = nr_pdcch_al1_narrow(rb, d, obs, 1, c.data(), n);
      ASSERT_EQ(nr_pdcch_al1_family_count(rb, d, c.data(), m), mx) << rb << "x" << d;
      EXPECT_LT(mx, NR_PDCCH_AL1_MAX_FAM) << rb << "x" << d;
      if (mx > gmax) { gmax = mx; grb = rb; gd = d; }
    }
  printf("max AL1 families surviving one observation: %d (first at %dx%d), bound %d\n", gmax, grb, gd,
         NR_PDCCH_AL1_MAX_FAM);
}

TEST(Al1Map, UnionReportsTheUncappedCount) {
  const nr_pdcch_al1_map_t m = {0, 0, 0};
  uint16_t rs[10][6];
  EXPECT_EQ(nr_pdcch_al1_union(270, 1, &m, 1, rs, 10), 45);  // snprintf-style: total, only 10 written
}

TEST(Al1Map, UnionCapHoldsTheFullCatalogueOfEveryLegalShape) {
  std::vector<nr_pdcch_al1_map_t> all(NR_PDCCH_AL1_MAX_MAPS);
  for (int d = 1; d <= 3; d++)
    for (int rb = 6; rb <= 270; rb += 6) {
      const int n = nr_pdcch_al1_enumerate(rb, d, all.data(), (int)all.size());
      EXPECT_LE(nr_pdcch_al1_union(rb, d, all.data(), n, NULL, 0), NR_PDCCH_AL1_UNION_MAX) << rb << "x" << d;
    }
}

TEST(Al1Map, FamilyRepsKeepTheFirstMappingOfEachFamily) {
  const nr_pdcch_al1_map_t truth = {2, 3, 101};
  std::vector<nr_pdcch_al1_map_t> cand(NR_PDCCH_AL1_MAX_MAPS);
  int n = nr_pdcch_al1_enumerate(270, 2, cand.data(), (int)cand.size());
  uint16_t obs[1][6];
  ASSERT_EQ(nr_pdcch_al1_regset(270, 2, truth, 7, obs[0]), 6);
  n = nr_pdcch_al1_narrow(270, 2, obs, 1, cand.data(), n);
  const nr_pdcch_al1_map_t first = cand[0];
  const int fam = nr_pdcch_al1_family_count(270, 2, cand.data(), n);
  const int r = nr_pdcch_al1_family_reps(270, 2, cand.data(), n);
  EXPECT_EQ(r, fam);
  EXPECT_EQ(nr_pdcch_al1_family_count(270, 2, cand.data(), r), r);
  EXPECT_EQ(cand[0].bundle, first.bundle);
  EXPECT_EQ(cand[0].shift, first.shift);
}

namespace {
// Transcription of nr_pdcch_demapping_deinterleaving() (dci_nr.c) for one candidate, as the reference.
void ref_demap(int nrb, int D, int Lin, int R, int shift, int cce, int Lc, const uint32_t *llr, int stride, uint32_t *out)
{
  const int N = nrb * D, C = Lin ? N / (R * Lin) : 0, Lb = Lin ? Lin : 6;
  const int B_rb = Lb / D, per = 6 / Lb, maxb = (N / 6) * per;
  std::vector<int> f(maxb), ord;
  for (int nb = 0, c = 0, r = 0; nb < maxb; nb++) {
    if (!Lin) f[nb] = nb;
    else { if (r == R) { r = 0; c++; } f[nb] = ((r * C) + c + shift) % (N / Lb); r++; }
  }
  for (int nb = 0; nb < maxb; nb++)
    for (int bc = cce * per; bc < (cce + Lc) * per; bc++)
      if (f[bc] == nb) ord.push_back(nb);
  int rbc = 0;
  for (int s = 0; s < D; s++)
    for (int k = 0; k < per * Lc; k++) {
      memcpy(out + 9 * rbc, llr + ord[k] * B_rb * 9 + s * stride, sizeof(uint32_t) * B_rb * 9);
      rbc += B_rb;
    }
}
}  // namespace

TEST(Al1Map, DemapByRegsetMatchesTheOaiDemapper) {
  const Shape sh[] = {{48, 1}, {48, 2}, {54, 3}, {270, 2}, {108, 3}};
  std::vector<nr_pdcch_al1_map_t> all(NR_PDCCH_AL1_MAX_MAPS);
  int checked = 0;
  for (const auto &s : sh) {
    const int stride = s.rb * 9;
    std::vector<uint32_t> llr((size_t)stride * s.d);
    for (size_t i = 0; i < llr.size(); i++) llr[i] = (uint32_t)(i * 2654435761u);  // unique fingerprint per RE
    const int n = nr_pdcch_al1_enumerate(s.rb, s.d, all.data(), (int)all.size());
    for (int i = 0; i < n; i++)
      for (int j = 0; j < s.rb * s.d / 6; j++) {
        uint16_t r[6];
        ASSERT_EQ(nr_pdcch_al1_regset(s.rb, s.d, all[i], j, r), 6);
        uint32_t want[54], got[54];
        ref_demap(s.rb, s.d, all[i].bundle, all[i].interleaver, all[i].shift, j, 1, llr.data(), stride, want);
        ASSERT_EQ(nr_pdcch_al1_demap(s.d, r, llr.data(), stride, got), 54);
        ASSERT_EQ(memcmp(want, got, sizeof(want)), 0) << s.rb << "x" << s.d << " map " << i << " cce " << j;
        checked++;
      }
  }
  printf("demap-by-regset == OAI demapper on %d (mapping, CCE) pairs\n", checked);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
