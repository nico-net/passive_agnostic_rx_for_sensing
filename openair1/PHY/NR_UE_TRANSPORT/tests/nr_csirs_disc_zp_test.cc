/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Blind ZP-CSI-RS proposer: synthetic TDD cell (7 DL slots of 10), PDSCH on a few RBs per slot, ZP pattern holes on a lattice. */
#include "nr_csirs_disc.h"
#include "nr_csirs_disc_zp.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include <cstdlib>

extern "C" {
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

namespace {
constexpr int NRB = 64;
constexpr int NANT = 4;

struct Cell {
  std::mt19937 rng;
  nr_csirs_disc_ring_t ring = {};
  std::vector<int16_t> grid;
  std::vector<nr_csirs_disc_slot_meta_t> meta;
  std::vector<float> feat;
  explicit Cell(unsigned seed, int nant = NANT, int nrb = NRB) : rng(seed)
  {
    ring.n_ant = nant;
    ring.n_rb = nrb;
    ring.slots_per_frame = 20;
    ring.capacity = 1;
    ring.slot_stride = (size_t)nant * 14 * 24 * nrb;
    grid.assign(ring.slot_stride, 0);
    meta.assign(1, {0, 0, 0, 1});
    ring.grid_base = grid.data();
    ring.meta = meta.data();
    feat.resize(nr_csirs_zp_feat_len(nrb));
  }
  /* hole(l, rb, sc): this RE carries no PDSCH in this slot */
  template <class Hole> void slot(uint32_t abs, bool dl, int rb0, int nrb_alloc, Hole hole, double snr_db = 20)
  {
    std::normal_distribution<double> n(0, 100);
    const double a = 100 * std::pow(10.0, snr_db / 20) / std::sqrt(2.0);
    for (int ant = 0; ant < ring.n_ant; ++ant)
      for (int l = 0; l < 14; ++l) {
        int16_t *v = nr_csirs_disc_ring_sym(&ring, 0, ant, l);
        for (int k = 0; k < 12 * ring.n_rb; ++k) {
          const int rb = k / 12, sc = k % 12;
          double re = n(rng), im = n(rng);
          if (dl && l >= 2 && rb >= rb0 && rb < rb0 + nrb_alloc && !hole(abs, l, rb, sc)) {
            re += a * ((rng() & 1) ? 1 : -1);
            im += a * ((rng() & 1) ? 1 : -1);
          }
          v[2 * k] = (int16_t)std::lround(re);
          v[2 * k + 1] = (int16_t)std::lround(im);
        }
      }
    meta[0].abs_slot = abs;
    nr_csirs_zp_features_cpu(&ring, 0, feat.data());
  }
  /* PDSCH of several narrow allocations (the carrier is mostly idle), Rayleigh fading per RB and antenna, and a hole that is not perfectly empty: it keeps leak_db of the data level
   * (inter-carrier leakage of its loaded neighbours, residual frequency offset). `ranges` are (first RB, RBs). */
  template <class Hole> void slot_multi(uint32_t abs, bool dl, const std::vector<std::pair<int, int>> &ranges, Hole hole, double leak_db, double snr_db = 20)
  {
    std::normal_distribution<double> n(0, 100), g(0, 1);
    const double a = 100 * std::pow(10.0, snr_db / 20) / std::sqrt(2.0), leak = std::pow(10.0, leak_db / 20);
    for (int ant = 0; ant < ring.n_ant; ++ant) {
      std::vector<double> fade(ring.n_rb);
      for (int rb = 0; rb < ring.n_rb; ++rb)
        fade[rb] = std::hypot(g(rng), g(rng)) / std::sqrt(2.0);
      for (int l = 0; l < 14; ++l) {
        int16_t *v = nr_csirs_disc_ring_sym(&ring, 0, ant, l);
        for (int k = 0; k < 12 * ring.n_rb; ++k) {
          const int rb = k / 12, sc = k % 12;
          double re = n(rng), im = n(rng);
          bool in = false;
          for (auto &r : ranges)
            in |= rb >= r.first && rb < r.first + r.second;
          if (dl && l >= 2 && in) {
            const double amp = a * fade[rb] * (hole(abs, l, rb, sc) ? leak : 1.0);
            re += amp * ((rng() & 1) ? 1 : -1);
            im += amp * ((rng() & 1) ? 1 : -1);
          }
          v[2 * k] = (int16_t)std::lround(re);
          v[2 * k + 1] = (int16_t)std::lround(im);
        }
      }
    }
    meta[0].abs_slot = abs;
    nr_csirs_zp_features_cpu(&ring, 0, feat.data());
  }
};

/* Drives a detector with `windows` contiguous 640-slot windows. */
template <class Hole, class Alloc>
int run(nr_csirs_zp_det_t *det, Cell &cell, int windows, Hole hole, Alloc alloc, nr_csirs_disc_fp_t *out, int max_out)
{
  uint32_t abs = 12345;
  for (int w = 0; w < windows; ++w)
    for (int s = 0; s < 640; ++s, ++abs) {
      const bool dl = abs % 10 < 7;
      int rb0, n;
      alloc(abs, &rb0, &n);
      cell.slot(abs, dl, rb0, n, hole);
      nr_csirs_zp_feed(det, abs, cell.feat.data());
    }
  return nr_csirs_zp_propose(det, out, max_out);
}
/* PDSCH of random width at the low RBs, in about 2 of 3 DL slots */
auto alloc_low(std::mt19937 &g)
{
  return [&g](uint32_t, int *rb0, int *n) {
    *rb0 = 0;
    *n = (g() % 3 == 0) ? 0 : 8 + (int)(g() % 17);
  };
}
}  // namespace

/* a 4-port ZP (row 4 pattern, subcarriers 8-11) on symbol 8 at period 40, offset 4 */
TEST(CsirsDiscZp, FindsAZpPatternOnItsLattice)
{
  Cell cell(1);
  auto *det = nr_csirs_zp_new(NRB);
  ASSERT_NE(det, nullptr);
  auto zp = [](uint32_t abs, int l, int, int sc) { return abs % 40 == 4 && l == 8 && sc >= 8; };
  nr_csirs_disc_fp_t fp[8];
  const int n = run(det, cell, 6, zp, alloc_low(cell.rng), fp, 8);
  ASSERT_GE(n, 1);
  printf("ZP proposal: sym=0x%x sc=0x%x dens=%u rb=%u:%u P/O=%u/%u z=%.1f\n", fp[0].sym_mask, fp[0].sc_mask, fp[0].density,
         fp[0].rb_start, fp[0].n_rb, fp[0].period, fp[0].offset, fp[0].strength);
  EXPECT_EQ(fp[0].sym_mask, 1u << 8);
  EXPECT_EQ(fp[0].sc_mask, 0x0f00u);
  EXPECT_EQ(fp[0].density, 2);
  EXPECT_EQ(fp[0].rb_start, 0);
  EXPECT_GE(fp[0].n_rb, 8);
  EXPECT_LE(fp[0].n_rb, 24);          /* the RBs the PDSCH ever used */
  EXPECT_EQ(fp[0].period, 40);
  EXPECT_EQ(fp[0].offset, 4);
  for (int i = 1; i < n; ++i)
    printf("extra: sym=0x%x sc=0x%x dens=%u rb=%u:%u P/O=%u/%u z=%.1f\n", fp[i].sym_mask, fp[i].sc_mask, fp[i].density, fp[i].rb_start, fp[i].n_rb, fp[i].period, fp[i].offset, fp[i].strength);
  EXPECT_EQ(n, 1);                    /* nothing else is proposed */
  /* the footprint is a legal Table 7.4.1.5.3-1 pattern: row 4 */
  nr_csirs_disc_hyp_t h[16];
  const int nh = nr_csirs_disc_hypotheses(fp, 1, h, 16);
  ASSERT_GE(nh, 1);
  bool row4 = false;
  for (int i = 0; i < nh; ++i)
    row4 |= h[i].fit == 1.0f && h[i].row == 4 && h[i].symb_l0 == 8;
  EXPECT_TRUE(row4);
  nr_csirs_zp_free(det);
}

/* a ZP on two symbols (the two symbols of a two-symbol row) offered as one footprint, and on a lattice of period 20 */
TEST(CsirsDiscZp, TwoSymbolsAndPeriod20)
{
  Cell cell(2);
  auto *det = nr_csirs_zp_new(NRB);
  auto zp = [](uint32_t abs, int l, int, int sc) { return abs % 20 == 3 && (l == 5 || l == 6) && (sc == 0 || sc == 1 || sc == 6 || sc == 7); };
  nr_csirs_disc_fp_t fp[8];
  const int n = run(det, cell, 6, zp, alloc_low(cell.rng), fp, 8);
  ASSERT_GE(n, 1);
  bool both = false;
  for (int i = 0; i < n; ++i) {
    printf("ZP proposal: sym=0x%x sc=0x%x dens=%u rb=%u:%u P/O=%u/%u z=%.1f\n", fp[i].sym_mask, fp[i].sc_mask, fp[i].density,
           fp[i].rb_start, fp[i].n_rb, fp[i].period, fp[i].offset, fp[i].strength);
    EXPECT_EQ(fp[i].sc_mask, 0x00c3u);
    EXPECT_EQ(fp[i].offset, 3);
    EXPECT_TRUE(fp[i].period == 20);
    both |= fp[i].sym_mask == ((1u << 5) | (1u << 6));
  }
  EXPECT_TRUE(both);
  nr_csirs_zp_free(det);
}

/* half density: only the even RBs are empty */
TEST(CsirsDiscZp, HalfDensityIsParityResolved)
{
  Cell cell(3);
  auto *det = nr_csirs_zp_new(NRB);
  auto zp = [](uint32_t abs, int l, int rb, int sc) { return abs % 16 == 5 && l == 7 && sc == 4 && rb % 2 == 0; };
  nr_csirs_disc_fp_t fp[8];
  const int n = run(det, cell, 6, zp, alloc_low(cell.rng), fp, 8);
  ASSERT_GE(n, 1);
  EXPECT_EQ(fp[0].sym_mask, 1u << 7);
  EXPECT_EQ(fp[0].sc_mask, 1u << 4);
  EXPECT_EQ(fp[0].density, 0);
  EXPECT_EQ(fp[0].period, 16);
  EXPECT_EQ(fp[0].offset, 5);
  nr_csirs_zp_free(det);
}

/* the holes that every cell has are not a ZP: the other CDM group of the DM-RS (empty in every PDSCH slot at the DM-RS symbols), a
 * CORESET-like gap in the first symbols, nothing periodic */
TEST(CsirsDiscZp, StructuralHolesAreNotProposed)
{
  Cell cell(4);
  auto *det = nr_csirs_zp_new(NRB);
  auto structural = [](uint32_t, int l, int, int sc) { return (l == 2 || l == 11) && (sc & 1); };
  nr_csirs_disc_fp_t fp[8];
  EXPECT_EQ(run(det, cell, 6, structural, alloc_low(cell.rng), fp, 8), 0);
  nr_csirs_zp_free(det);
}

/* a ZP that exists beside the structural holes is still found, alone */
TEST(CsirsDiscZp, ZpBesideStructuralHoles)
{
  Cell cell(5);
  auto *det = nr_csirs_zp_new(NRB);
  auto h = [](uint32_t abs, int l, int, int sc) { return ((l == 2 || l == 11) && (sc & 1)) || (abs % 80 == 14 && l == 9 && sc >= 4 && sc < 8); };
  nr_csirs_disc_fp_t fp[8];
  const int n = run(det, cell, 8, h, alloc_low(cell.rng), fp, 8);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(fp[0].sym_mask, 1u << 9);
  EXPECT_EQ(fp[0].sc_mask, 0x00f0u);
  EXPECT_EQ(fp[0].period % 80, 0) << "a multiple would only be a coarser lattice of the same slots";
  EXPECT_EQ(fp[0].offset % 80, 14 % 80);
  nr_csirs_zp_free(det);
}

/* noise and idle slots, and PDSCH without any hole: no proposal, over many independent cells */
TEST(CsirsDiscZp, NoFalseAlarms)
{
  for (unsigned seed = 10; seed < 18; ++seed) {
    Cell cell(seed, seed % 2 ? 4 : 1);
    auto *det = nr_csirs_zp_new(NRB);
    auto none = [](uint32_t, int, int, int) { return false; };
    nr_csirs_disc_fp_t fp[8];
    const bool idle = seed % 3 == 0;
    auto alloc = [&](uint32_t, int *rb0, int *n) {
      *rb0 = 0;
      *n = idle ? 0 : 4 + (int)(cell.rng() % 40);
    };
    EXPECT_EQ(run(det, cell, 4, none, alloc, fp, 8), 0) << seed;
    nr_csirs_zp_free(det);
  }
}

/* a ZP seen only when PDSCH is present: slots without any PDSCH on the lattice must not erase it (idle tiles are not dark, not bright) */
TEST(CsirsDiscZp, ResetAndForgetting)
{
  Cell cell(6);
  auto *det = nr_csirs_zp_new(NRB);
  auto zp = [](uint32_t abs, int l, int, int sc) { return abs % 40 == 4 && l == 8 && sc >= 8; };
  nr_csirs_disc_fp_t fp[8];
  ASSERT_GE(run(det, cell, 6, zp, alloc_low(cell.rng), fp, 8), 1);
  EXPECT_EQ(nr_csirs_zp_slots(det), 6u * 640);
  nr_csirs_zp_reset(det);
  EXPECT_EQ(nr_csirs_zp_slots(det), 0u);
  EXPECT_EQ(nr_csirs_zp_propose(det, fp, 8), 0);
  nr_csirs_zp_free(det);
}

/* a footprint found from part of a pattern (one subcarrier of the 4-port row 4, even RBs only) points at the legal rows that contain it, the row itself among them */
TEST(CsirsDiscZp, PartialFootprintPointsAtContainingRows)
{
  nr_csirs_disc_fp_t fp = {};
  fp.sym_mask = 1u << 8;
  fp.sc_mask = 1u << 10;
  fp.density = 0;
  fp.rb_start = 0;
  fp.n_rb = 64;
  fp.period = 40;
  fp.offset = 4;
  nr_csirs_disc_hyp_t rows[8];
  const int n = nr_csirs_disc_rows_covering(&fp, rows, 8);
  ASSERT_GE(n, 1);
  bool row4 = false;
  for (int i = 0; i < n; ++i) {
    EXPECT_EQ(rows[i].period, 40);
    EXPECT_EQ(rows[i].offset, 4);
    row4 |= rows[i].row == 4 && rows[i].freq_domain == 4 && rows[i].symb_l0 == 8;
    if (i) EXPECT_LE(rows[i].fit, rows[i - 1].fit) << "closest first";
  }
  EXPECT_TRUE(row4);
}

/* The lab configuration of the OCUDU gNB: a 4-port fd-CDM2 row 4 ZP-CSI-RS at symbol 8, subcarriers k0..k0+3 = 8..11 of EVERY RB, period 40 offset 4; PDSCH is narrow (1-2 allocations of 5-32
 * RB anywhere in the carrier, in nearly every DL slot), fades per RB and antenna, and the empty REs leak -22 dB of the data level. All four subcarriers, both RB parities, must come out as one pattern. */
TEST(CsirsDiscZp, Row4AtSymbol8WithNarrowFadedPdschAndLeakage)
{
  const int nrb = 96;
  for (int nant : {2, 4}) {
    Cell cell(100 + nant, nant, nrb);
    auto *det = nr_csirs_zp_new(nrb);
    ASSERT_NE(det, nullptr);
    auto zp = [](uint32_t abs, int l, int, int sc) { return abs % 40 == 4 && l == 8 && sc >= 8 && sc < 12; };
    uint32_t abs = 7000;
    for (int w = 0; w < 5; ++w)
      for (int s = 0; s < 640; ++s, ++abs) {
        const bool dl = abs % 10 < 7;
        std::vector<std::pair<int, int>> ranges;
        const int ues = 1 + (int)(cell.rng() % 2);
        for (int u = 0; u < ues; ++u) {
          const int n = 5 + (int)(cell.rng() % 28);
          ranges.push_back({(int)(cell.rng() % (nrb - n + 1)), n});
        }
        if (cell.rng() % 8 == 0)
          ranges.clear();
        cell.slot_multi(abs, dl, ranges, zp, -22);
        nr_csirs_zp_feed(det, abs, cell.feat.data());
      }
    nr_csirs_disc_fp_t fp[8];
    const int n = nr_csirs_zp_propose(det, fp, 8);
    char cells[512];
    nr_csirs_zp_cells(det, 40, 4, cells, sizeof(cells));
    printf("n_ant %d: %d proposals; cells[%s]\n", nant, n, cells);
    ASSERT_GE(n, 1) << "n_ant " << nant;
    for (int i = 0; i < n; ++i)
      printf("  sym=0x%x sc=0x%x dens=%u rb=%u:%u P/O=%u/%u z=%.1f\n", fp[i].sym_mask, fp[i].sc_mask, fp[i].density, fp[i].rb_start, fp[i].n_rb, fp[i].period, fp[i].offset, fp[i].strength);
    EXPECT_EQ(fp[0].sym_mask, 1u << 8);
    EXPECT_EQ(fp[0].sc_mask, 0x0f00u) << "all four subcarriers";
    EXPECT_EQ(fp[0].density, 2) << "both RB parities, one every-RB pattern";
    EXPECT_EQ(fp[0].period, 40);
    EXPECT_EQ(fp[0].offset, 4);
    EXPECT_GE(fp[0].n_rb, nrb - 8) << "the extent the traffic ever covered";
    nr_csirs_disc_hyp_t h[16];
    const int nh = nr_csirs_disc_hypotheses(fp, 1, h, 16);
    bool row4 = false;
    for (int i = 0; i < nh; ++i)
      row4 |= h[i].fit == 1.0f && h[i].row == 4 && h[i].symb_l0 == 8 && h[i].freq_domain == 4;
    EXPECT_TRUE(row4);
    nr_csirs_zp_free(det);
  }
}

/* a carrier that is loaded across its whole width in every DL slot (no idle tile to show the noise floor in the slot) */
TEST(CsirsDiscZp, FullBandPdschStillShowsTheHoles)
{
  const int nrb = 48;
  Cell cell(77, 4, nrb);
  auto *det = nr_csirs_zp_new(nrb);
  auto zp = [](uint32_t abs, int l, int, int sc) { return abs % 40 == 4 && l == 8 && sc >= 8 && sc < 12; };
  uint32_t abs = 9000;
  for (int w = 0; w < 4; ++w)
    for (int s = 0; s < 640; ++s, ++abs) {
      const bool dl = abs % 10 < 8;
      cell.slot_multi(abs, dl, {{0, nrb}}, zp, -22);
      nr_csirs_zp_feed(det, abs, cell.feat.data());
    }
  nr_csirs_disc_fp_t fp[8];
  const int n = nr_csirs_zp_propose(det, fp, 8);
  ASSERT_GE(n, 1);
  EXPECT_EQ(fp[0].sym_mask, 1u << 8);
  EXPECT_EQ(fp[0].sc_mask, 0x0f00u);
  EXPECT_EQ(fp[0].period, 40);
  EXPECT_EQ(fp[0].offset, 4);
  nr_csirs_zp_free(det);
}
