/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* G1: nr_gpu_cell_bytes (nr_gpu_cell.h): geometry, the sizing expressions, dev/pin split, monotonicity, defaults; prints the per-cell table. Pure C arithmetic, builds
 * and runs without a GPU (the cloud build runs the same test). The comparison against what the GPU modules really allocate is test_nr_gpu_cell_bytes_gpu. */
#include <gtest/gtest.h>
#include <cstdio>
#include "nr_gpu_cell.h"

namespace {
nr_gpu_cell_params_t P(int rb, int rx, int scs = 30) {
  nr_gpu_cell_params_t p{};
  p.n_rb = rb; p.n_rx = rx; p.scs_khz = scs;
  return p;
}
}

TEST(GpuCell, GeometryMatchesTheOaiTable) {
  struct { int rb, scs, fft, cp, cp0; size_t sps; } t[] = {
      {51, 30, 1024, 72, 88, 88 + 1024 + 13 * (1024 + 72)}, {106, 30, 2048, 144, 176, 176 + 2048 + 13 * (2048 + 144)}, {273, 30, 4096, 288, 352, 352 + 4096 + 13 * (4096 + 288)}};
  for (auto &e : t) {
    auto g = nr_gpu_cell_geom(&(const nr_gpu_cell_params_t &)P(e.rb, 4, e.scs));
    EXPECT_EQ(e.fft, g.fft) << e.rb;
    EXPECT_EQ(e.cp, g.cp) << e.rb;
    EXPECT_EQ(e.cp0, g.cp0) << e.rb;
    EXPECT_EQ(e.sps, g.sps) << e.rb;
    EXPECT_EQ(14, g.ns);
  }
  // 273 PRB at 30 kHz: 4096 x 30 kHz = 122.88 MS/s, samples per slot 61440 in the first half-subframe slot's CP layout
  EXPECT_EQ(4096u + 352u + 13u * (4096u + 288u), nr_gpu_cell_geom(&(const nr_gpu_cell_params_t &)P(273, 4)).sps - 0u);
}

TEST(GpuCell, DefaultsAreTheLargestGridFourRx) {
  nr_gpu_cell_params_t z{};
  for (int k = 0; k < NR_GPU_CELL_KIND_N; k++) EXPECT_EQ(nr_gpu_cell_bytes(&z, k), nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(273, 4, 30), k)) << nr_gpu_cell_kind_name(k);
  EXPECT_EQ(0u, nr_gpu_cell_bytes(nullptr, NR_GPU_CELL_HARQ));
  EXPECT_EQ(0u, nr_gpu_cell_bytes(&z, NR_GPU_CELL_KIND_N));
}

TEST(GpuCell, HarqIsTheFixedSoftStore) {
  for (int rb : {51, 106, 273})
    for (int rx : {1, 4}) {
      const auto m = nr_gpu_cell_mem(&(const nr_gpu_cell_params_t &)P(rb, rx), NR_GPU_CELL_HARQ);
      EXPECT_EQ((size_t)2048 * 66 * 384 * 2, m.dev);
      EXPECT_EQ(0u, m.pin);
    }
}

TEST(GpuCell, SplitAndFormulas) {
  const auto p = P(106, 4);
  const auto g = nr_gpu_cell_geom(&p);
  auto f = nr_gpu_cell_mem(&p, NR_GPU_CELL_PDSCH_FEP);
  EXPECT_EQ((size_t)4 * (g.sps + g.fft) * 4 + 14 * 4 + (size_t)14 * g.fft * 8, f.pin); // h_rx + h_rot + h_check
  EXPECT_EQ((size_t)4 * (g.sps + g.fft) * 4 + 14 * 4 + 2 * (size_t)4 * 14 * g.fft * 8, f.dev); // d_rx + d_rot + d_fft_in + d_rxF
  auto e = nr_gpu_cell_mem(&p, NR_GPU_CELL_PDSCH_FEP_EXACT);
  EXPECT_EQ(e.dev, e.pin);
  EXPECT_EQ((size_t)4 * 14 * g.fft * 4 + 8 * ((size_t)4 * (g.sps + g.fft) * 4), e.dev);
  auto c = nr_gpu_cell_mem(&p, NR_GPU_CELL_PDCCH_FEP);
  EXPECT_EQ((size_t)4 * 3 * g.fft * 8, c.dev);
  EXPECT_EQ(c.dev, c.pin);
  auto s = nr_gpu_cell_mem(&p, NR_GPU_CELL_NID_STAGING);
  EXPECT_EQ((size_t)3 * 336 * 3 * 12 * 106 * 4, s.pin);
  EXPECT_EQ(0u, s.dev);
  for (int k = 0; k < NR_GPU_CELL_KIND_N; k++) {
    const auto m = nr_gpu_cell_mem(&p, k);
    EXPECT_EQ(m.dev + m.pin, nr_gpu_cell_bytes(&p, k)) << nr_gpu_cell_kind_name(k);
  }
}

TEST(GpuCell, NidWindowScalesWithWindowAndGrid) {
  auto p = P(106, 4);
  p.nid_window = 1;
  const size_t w1 = nr_gpu_cell_bytes(&p, NR_GPU_CELL_NID_WINDOW);
  p.nid_window = 10;
  const size_t w10 = nr_gpu_cell_bytes(&p, NR_GPU_CELL_NID_WINDOW);
  // (4n+1) terms break exact linearity; the per-window part dominates
  EXPECT_GT(w10, 9 * w1);
  EXPECT_LT(w10, 10 * w1 + 400000);
  auto q = P(51, 4);
  q.nid_window = 10;
  EXPECT_LT(nr_gpu_cell_bytes(&q, NR_GPU_CELL_NID_WINDOW), w10);
  p.nid_dur = 1;
  EXPECT_LT(nr_gpu_cell_bytes(&p, NR_GPU_CELL_NID_WINDOW), w10) << "a shorter CORESET keeps less IQ";
}

TEST(GpuCell, MonotoneInGridAndAntennas) {
  for (int k : {NR_GPU_CELL_PDSCH_FEP, NR_GPU_CELL_PDSCH_FEP_EXACT, NR_GPU_CELL_PDCCH_FEP, NR_GPU_CELL_NID_WINDOW, NR_GPU_CELL_NID_STAGING, NR_GPU_CELL_CSIRS_FIXED, NR_GPU_CELL_CSIRS_SLOT}) {
    EXPECT_LT(nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(51, 4), k), nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(106, 4), k)) << nr_gpu_cell_kind_name(k);
    EXPECT_LT(nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(106, 4), k), nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(273, 4), k)) << nr_gpu_cell_kind_name(k);
  }
  for (int k : {NR_GPU_CELL_PDSCH_FEP, NR_GPU_CELL_PDSCH_FEP_EXACT, NR_GPU_CELL_PDCCH_FEP, NR_GPU_CELL_CSIRS_FIXED, NR_GPU_CELL_CSIRS_SLOT})
    EXPECT_LT(nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(106, 1), k), nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(106, 4), k)) << nr_gpu_cell_kind_name(k);
  // the nID window does not depend on the antennas (the nID path reads one stream)
  EXPECT_EQ(nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(106, 1), NR_GPU_CELL_NID_WINDOW), nr_gpu_cell_bytes(&(const nr_gpu_cell_params_t &)P(106, 4), NR_GPU_CELL_NID_WINDOW));
}

TEST(GpuCell, PrintTable) {
  std::printf("GPUCELL table [CODE, nr_gpu_cell.h; bytes dev/pin]\n kind");
  for (int rb : {51, 106, 273})
    for (int rx : {1, 4}) std::printf("\t%dPRB/%dRX", rb, rx);
  std::printf("\n");
  for (int k = 0; k < NR_GPU_CELL_KIND_N; k++) {
    std::printf(" %s", nr_gpu_cell_kind_name(k));
    for (int rb : {51, 106, 273})
      for (int rx : {1, 4}) {
        const auto m = nr_gpu_cell_mem(&(const nr_gpu_cell_params_t &)P(rb, rx), k);
        std::printf("\t%zu/%zu", m.dev, m.pin);
      }
    std::printf("\n");
  }
  SUCCEED();
}
