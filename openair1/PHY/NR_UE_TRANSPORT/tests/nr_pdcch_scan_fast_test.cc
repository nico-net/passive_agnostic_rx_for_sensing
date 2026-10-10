#include "PHY/NR_UE_TRANSPORT/nr_pdcch_scan_llr_cache.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_scan_score.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_scan_gpu_ref.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

TEST(PdcchScanFast, ExactExtentCacheCopiesBitsAndRejectsDifferentDemodulation)
{
  nr_pdcch_scan_llr_cache_t cache{};
  nr_pdcch_scan_llr_key_t key{};
  key.absolute_slot = 1001;
  key.samples = &cache;
  key.n_rb = 36;
  key.rb_offset = 42;
  key.n_id = 62922;
  key.fft_size = 4096;
  key.n_ant = 4;
  key.first_symbol = 1;
  key.duration = 2;
  std::array<int16_t, 648> ref{}, out{};
  for (size_t i = 0; i < ref.size(); i++) ref[i] = (int16_t)((i * 53) ^ 0x55aa);
  ASSERT_TRUE(nr_pdcch_scan_llr_put(&cache, &key, ref.data(), sizeof(ref), sizeof(ref)));
  ASSERT_TRUE(nr_pdcch_scan_llr_get(&cache, &key, out.data(), sizeof(out)));
  EXPECT_EQ(0, memcmp(ref.data(), out.data(), sizeof(ref)));
  auto changed = key;
  changed.absolute_slot++;
  EXPECT_FALSE(nr_pdcch_scan_llr_get(&cache, &changed, out.data(), sizeof(out)));
  changed = key; changed.n_id++;
  EXPECT_FALSE(nr_pdcch_scan_llr_get(&cache, &changed, out.data(), sizeof(out)));
  changed = key; changed.rb_offset++;
  EXPECT_FALSE(nr_pdcch_scan_llr_get(&cache, &changed, out.data(), sizeof(out)));
  changed = key; changed.n_rb++;
  EXPECT_FALSE(nr_pdcch_scan_llr_get(&cache, &changed, out.data(), sizeof(out)));
  changed = key; changed.frequency_domain_resource[0] = 1;
  EXPECT_FALSE(nr_pdcch_scan_llr_get(&cache, &changed, out.data(), sizeof(out)));
  changed = key; changed.fo_bits++;
  EXPECT_FALSE(nr_pdcch_scan_llr_get(&cache, &changed, out.data(), sizeof(out)));
  for (auto &e : cache.entry) free(e.data);
}

TEST(PdcchScanFast, NeonScoreMatchesScalarForPdcchLengthsAndClipExtremes)
{
  std::array<int16_t, 1728> llr{};
  uint32_t x = 0x12345678;
  for (int n : {108, 216, 432, 864, 1728}) {
    for (int rep = 0; rep < 64; rep++) {
      for (int i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        llr[i] = (int16_t)(x >> 16);
      }
      llr[0] = INT16_MIN;
      llr[n - 1] = INT16_MAX;
      double legacy_abs = 0.0, legacy_sq = 0.0;
      for (int i = 0; i < n; i++) {
        const double v = llr[i] < 0 ? -(double)llr[i] : (double)llr[i];
        legacy_abs += v;
        legacy_sq += v * v;
      }
      EXPECT_EQ(legacy_abs * legacy_abs / ((double)n * legacy_sq),
                nr_pdcch_scan_score_scalar(llr.data(), n));
      EXPECT_EQ(nr_pdcch_scan_score_scalar(llr.data(), n), nr_pdcch_scan_score_neon(llr.data(), n));
      int64_t sum_scalar = -1, sum_neon = -2;
      EXPECT_EQ(nr_pdcch_scan_score_scalar_sum(llr.data(), n, &sum_scalar),
                nr_pdcch_scan_score_neon_sum(llr.data(), n, &sum_neon));
      EXPECT_EQ(sum_scalar, sum_neon);
      EXPECT_EQ(nr_pdcch_scan_abs_sum_scalar(llr.data(), n), nr_pdcch_scan_abs_sum_neon(llr.data(), n));
    }
  }
  llr.fill(0);
  EXPECT_EQ(0.0, nr_pdcch_scan_score_neon(llr.data(), 108));
}

TEST(PdcchScanFast, GpuReferenceRequiresEverySoftBit)
{
  std::array<int16_t, 108> cpu{}, gpu{};
  for (int i = 0; i < 108; i++) cpu[i] = gpu[i] = (int16_t)(i - 54);
  EXPECT_TRUE(nr_pdcch_scan_gpu_reference_accept(cpu.data(), gpu.data(), sizeof(cpu), 108, 108));
  gpu[107]++;
  EXPECT_FALSE(nr_pdcch_scan_gpu_reference_accept(cpu.data(), gpu.data(), sizeof(cpu), 108, 108));
  EXPECT_EQ(cpu[107], 53);
  gpu[107]--;
  EXPECT_FALSE(nr_pdcch_scan_gpu_reference_accept(cpu.data(), gpu.data(), sizeof(cpu), 107, 108));
  EXPECT_FALSE(nr_pdcch_scan_gpu_reference_accept(cpu.data(), gpu.data(), sizeof(cpu), 108, 107));
}
