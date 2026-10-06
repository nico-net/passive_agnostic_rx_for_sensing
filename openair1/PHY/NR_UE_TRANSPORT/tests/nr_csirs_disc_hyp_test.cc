/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
#include "nr_csirs_disc.h"

/* Keep the C++ test independent of nr_phy_common.h: that header contains C
 * variably-modified-array declarations which C++ cannot parse. */
typedef struct {
  int size;
  int ports;
  int kprime;
  int lprime;
  int j[16];
  int koverline[16];
  int loverline[16];
} test_csi_mapping_parms_t;
test_csi_mapping_parms_t get_csi_mapping_parms(int row, int b, int l0, int l1);
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int)
{
  std::abort();
}
}

struct Rule {
  int bits, ones, cdm;
  std::vector<int> densities;
  int l0_max;
  bool l1;
};

static const std::vector<Rule> &rules()
{
  static const std::vector<Rule> v = {
      {4, 1, 0, {3}, 13, false},       {12, 1, 0, {0, 1, 2}, 13, false},
      {6, 1, 1, {0, 1, 2}, 13, false}, {3, 1, 1, {2}, 13, false},
      {6, 1, 1, {2}, 12, false},       {6, 4, 1, {2}, 13, false},
      {6, 2, 1, {2}, 12, false},       {6, 2, 2, {2}, 12, false},
      {6, 6, 1, {2}, 13, false},       {6, 3, 2, {2}, 12, false},
      {6, 4, 1, {0, 1, 2}, 12, false}, {6, 4, 2, {0, 1, 2}, 12, false},
      {6, 3, 1, {0, 1, 2}, 10, true},  {6, 3, 2, {0, 1, 2}, 10, true},
      {6, 3, 3, {0, 1, 2}, 10, false}, {6, 4, 1, {0, 1, 2}, 10, true},
      {6, 4, 2, {0, 1, 2}, 10, true},  {6, 4, 3, {0, 1, 2}, 10, false},
  };
  return v;
}

static nr_csirs_disc_fp_t footprint(int row, int bitmap, int l0, int l1, int density)
{
  const test_csi_mapping_parms_t p = get_csi_mapping_parms(row, bitmap, l0, l1);
  uint16_t by_symbol[NR_CSIRS_DISC_NSYM]{};
  for (int i = 0; i < p.size; ++i)
    for (int kp = 0; kp <= p.kprime; ++kp)
      for (int lp = 0; lp <= p.lprime; ++lp)
        by_symbol[p.loverline[i] + lp] |= uint16_t{1} << (p.koverline[i] + kp);

  nr_csirs_disc_fp_t fp{};
  for (int l = 0; l < NR_CSIRS_DISC_NSYM; ++l) {
    if (!by_symbol[l])
      continue;
    fp.sym_mask |= uint16_t{1} << l;
    if (!fp.sc_mask)
      fp.sc_mask = by_symbol[l];
    EXPECT_EQ(fp.sc_mask, by_symbol[l]) << "row " << row << " has a non-rectangular footprint";
  }
  fp.density = density;
  fp.rb_start = 7;
  fp.n_rb = 52;
  fp.period = 40;
  fp.offset = 3;
  fp.strength = 6.0f;
  return fp;
}

static bool contains(const std::vector<nr_csirs_disc_hyp_t> &out,
                     int row,
                     int bitmap,
                     int l0,
                     int l1,
                     int cdm,
                     int density)
{
  return std::any_of(out.begin(), out.end(), [&](const nr_csirs_disc_hyp_t &h) {
    return h.row == row && h.freq_domain == bitmap && h.symb_l0 == l0 && h.symb_l1 == l1 && h.cdm_type == cdm
           && h.density == density && h.rb_start == 7 && h.n_rb == 52 && h.period == 40 && h.offset == 3
           && h.fp_index == 0;
  });
}

TEST(CsirsDiscHyp, EveryLegalConfigurationIsRecovered)
{
  int configurations = 0;
  int max_set = 0;
  std::vector<nr_csirs_disc_hyp_t> out(NR_CSIRS_DISC_MAX_HYP);
  for (int row = 1; row <= 18; ++row) {
    const Rule &rule = rules()[row - 1];
    for (int bitmap = 1; bitmap < (1 << rule.bits); ++bitmap) {
      if (__builtin_popcount(static_cast<unsigned>(bitmap)) != rule.ones)
        continue;
      for (int l0 = 0; l0 <= rule.l0_max; ++l0) {
        const int l1_first = rule.l1 ? std::max(2, l0 + 2) : 0;
        const int l1_last = rule.l1 ? 12 : 0;
        for (int l1 = l1_first; l1 <= l1_last; ++l1) {
          for (int density : rule.densities) {
            const nr_csirs_disc_fp_t fp = footprint(row, bitmap, l0, l1, density);
            const int n = nr_csirs_disc_hypotheses(&fp, 1, out.data(), out.size());
            ASSERT_GT(n, 0) << "row=" << row << " fd=" << bitmap << " l0=" << l0 << " l1=" << l1
                            << " density=" << density;
            out.resize(n);
            EXPECT_TRUE(contains(out, row, bitmap, l0, l1, rule.cdm, density))
                << "row=" << row << " fd=" << bitmap << " l0=" << l0 << " l1=" << l1
                << " density=" << density;
            max_set = std::max(max_set, n);
            configurations++;
            out.resize(NR_CSIRS_DISC_MAX_HYP);
          }
        }
      }
    }
  }
  std::printf("CSIRS_DISC_HYP configurations=%d max_hypothesis_set=%d bound=%d\n",
              configurations,
              max_set,
              NR_CSIRS_DISC_MAX_HYP);
  EXPECT_GT(configurations, 10000);
  EXPECT_LE(max_set, NR_CSIRS_DISC_MAX_HYP);
}

TEST(CsirsDiscHyp, OneMissingRepresentableReStillContainsTruth)
{
  std::vector<nr_csirs_disc_hyp_t> out(NR_CSIRS_DISC_MAX_HYP);
  for (int row = 1; row <= 18; ++row) {
    const Rule &rule = rules()[row - 1];
    int bitmap = 0;
    for (int bit = 0; bit < rule.ones; ++bit)
      bitmap |= 1 << bit;
    const int l0 = 2;
    const int l1 = rule.l1 ? 6 : 0;
    for (int density : rule.densities) {
      nr_csirs_disc_fp_t fp = footprint(row, bitmap, l0, l1, density);
      const int area = __builtin_popcount(fp.sym_mask) * __builtin_popcount(fp.sc_mask);
      const int lost = __builtin_popcount(fp.sc_mask) > 1 ? __builtin_popcount(fp.sym_mask)
                                                          : __builtin_popcount(fp.sc_mask);
      if (5 * (area - lost) < 4 * area)
        continue; /* The rectangular footprint cannot express an isolated loss on a multi-symbol pattern. */
      if (__builtin_popcount(fp.sc_mask) > 1)
        fp.sc_mask &= fp.sc_mask - 1;
      else
        fp.sym_mask &= fp.sym_mask - 1;
      const int n = nr_csirs_disc_hypotheses(&fp, 1, out.data(), out.size());
      ASSERT_GT(n, 0) << "row=" << row;
      out.resize(n);
      EXPECT_TRUE(contains(out, row, bitmap, l0, l1, rule.cdm, density)) << "row=" << row;
      out.resize(NR_CSIRS_DISC_MAX_HYP);
    }
  }
}

TEST(CsirsDiscHyp, EmptyAndInvalidInputsReturnZero)
{
  nr_csirs_disc_hyp_t out[8]{};
  nr_csirs_disc_fp_t fp{};
  EXPECT_EQ(nr_csirs_disc_hypotheses(nullptr, 1, out, 8), 0);
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 1, out, 8), 0);
  fp.sym_mask = 1;
  fp.sc_mask = 1;
  fp.density = 4;
  fp.rb_start = 0;
  fp.n_rb = 1;
  fp.period = 40;
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 1, out, 8), 0);
  fp.density = 2;
  fp.period = 7;
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 1, out, 8), 0);
  fp.period = 40;
  fp.offset = 40;
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 1, out, 8), 0);
  fp.offset = 0;
  fp.n_rb = 276;
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 1, out, 8), 0);
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 0, out, 8), 0);
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 1, nullptr, 8), 0);
  EXPECT_EQ(nr_csirs_disc_hypotheses(&fp, 1, out, 0), 0);
}

TEST(CsirsDiscHyp, MultipleFootprintsCarryTheirIndex)
{
  nr_csirs_disc_fp_t fps[] = {footprint(9, 0x3f, 4, 0, 2), footprint(18, 0x0f, 6, 0, 0)};
  std::vector<nr_csirs_disc_hyp_t> out(NR_CSIRS_DISC_MAX_HYP);
  const int n = nr_csirs_disc_hypotheses(fps, 2, out.data(), out.size());
  ASSERT_GT(n, 0);
  bool row9 = false;
  bool row18 = false;
  for (int i = 0; i < n; ++i) {
    row9 |= out[i].row == 9 && out[i].freq_domain == 0x3f && out[i].symb_l0 == 4 && out[i].fp_index == 0;
    row18 |= out[i].row == 18 && out[i].freq_domain == 0x0f && out[i].symb_l0 == 6 && out[i].fp_index == 1;
  }
  EXPECT_TRUE(row9);
  EXPECT_TRUE(row18);
}

TEST(CsirsDiscHyp, OutputIsRankedAndBounded)
{
  const nr_csirs_disc_fp_t fp = footprint(16, 0x0f, 4, 8, 2);
  nr_csirs_disc_hyp_t out[2]{};
  ASSERT_EQ(nr_csirs_disc_hypotheses(&fp, 1, out, 2), 2);
  EXPECT_FLOAT_EQ(out[0].fit, 1.0f);
  EXPECT_FLOAT_EQ(out[1].fit, 1.0f);
  EXPECT_LE(out[0].row, out[1].row);
}
