/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Live-density timing diagnostic for family alignment plus current-CPI variance. */
#include "detector_cuda.h"
#include "sync_correction.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace nr_isac;

namespace {
CfrWindow dense_window()
{
  CfrWindow window;
  window.antennas = 4;
  window.rows = 192;
  window.subcarriers = 3276;
  window.scs_hz = 30000.0;
  window.fc_hz = 3499440000.0;
  const size_t cells = static_cast<size_t>(window.rows) * window.subcarriers;
  window.values.resize(window.antennas * cells);
  window.observed.assign(cells, 0);
  window.row_time_slots.resize(window.rows);
  window.row_slot_idx.resize(window.rows);
  window.row_slot_frac.resize(window.rows);
  window.row_source_mask.resize(window.rows);
  uint32_t random_state = 0x51A0AAu;
  for (uint32_t row = 0; row < window.rows; ++row) {
    const uint32_t family = row % 12;
    window.row_time_slots[row] = row / 6.0;
    window.row_slot_idx[row] = row / 6;
    window.row_slot_frac[row] = family >= 6 ? .5 : 0.0;
    window.row_source_mask[row] = 1u << (family % 6);
    for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
      const bool occupied = (family % 3 == 0 && subcarrier % 7 != 0)
                            || (family % 3 == 1 && subcarrier >= 30 && subcarrier < 3240)
                            || (family % 3 == 2 && subcarrier % 5 != 1);
      window.observed[window.cell(row, subcarrier)] = occupied;
      for (uint32_t antenna = 0; antenna < window.antennas; ++antenna) {
        random_state = 1664525u * random_state + 1013904223u;
        const double noise = (static_cast<double>(random_state >> 8) / 16777216.0 - .5) * .004;
        const double delay = .035 * (row / 12);
        const double phase = -.0031 * (family + 1.0) * subcarrier
                             - 2.0 * PI * delay * subcarrier / window.subcarriers
                             + .11 * antenna + .013 * row + noise;
        const double amplitude = (1.0 + .006 * (row / 12)) * (1.0 + .025 * antenna);
        window.values[window.sample(antenna, row, subcarrier)] =
            std::polar(static_cast<float>(amplitude), static_cast<float>(phase));
      }
    }
  }
  return window;
}

struct Sample {
  double alignment_ms;
  double variance_ms;
  double combined_ms;
};
} // namespace

int main()
{
  setenv("NR_ISAC_CUDA_DETECTOR", "1", 1);
  if (!detector_cuda_available()) {
    std::puts("CUDA family-processing benchmark skipped: no enabled CUDA device");
    return 77;
  }
  setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
  detector_cuda_warmup();
  const CfrWindow input = dense_window();
  auto run = [&]() {
    CfrWindow aligned = input;
    const auto started = std::chrono::steady_clock::now();
    const double variance = estimate_current_cpi_variance(input);
    const auto variance_done = std::chrono::steady_clock::now();
    const auto stats = align_allocation_families(aligned, true);
    const auto alignment_done = std::chrono::steady_clock::now();
    if (!(variance > 0.0) || stats.aligned_rows != input.rows - stats.families)
      std::abort();
    return Sample{
        std::chrono::duration<double, std::milli>(alignment_done - variance_done).count(),
        std::chrono::duration<double, std::milli>(variance_done - started).count(),
        std::chrono::duration<double, std::milli>(alignment_done - started).count()};
  };
  const Sample warmup = run();
  std::vector<Sample> samples;
  for (uint32_t repetition = 0; repetition < 7; ++repetition) samples.push_back(run());
  std::sort(samples.begin(), samples.end(), [](const Sample& left, const Sample& right) {
    return left.combined_ms < right.combined_ms;
  });
  const Sample median = samples[samples.size() / 2];
  const char* configured_limit = std::getenv("NR_ISAC_CUDA_FAMILY_BENCHMARK_MAX_MS");
  const double limit_ms = configured_limit ? std::strtod(configured_limit, nullptr) : 40.0;
  std::printf("CUDA family dense CPI: warmup=%.3f ms median_variance=%.3f ms "
              "median_alignment=%.3f ms min_combined=%.3f ms median_combined=%.3f ms "
              "max_combined=%.3f ms limit=%.3f ms\n",
              warmup.combined_ms, median.variance_ms, median.alignment_ms,
              samples.front().combined_ms, median.combined_ms, samples.back().combined_ms,
              limit_ms);
  if (!(median.combined_ms < limit_ms)) {
    std::fputs("CUDA family processing misses required CPI throughput\n", stderr);
    return EXIT_FAILURE;
  }

  auto run_dual = [&]() {
    CfrWindow fused = input;
    CfrWindow dl_only = input;
    // Exercise distinct provenance-preserved DL contents and occupancy; this must not collapse
    // into the fused tensor merely to improve timing.
    for (uint32_t row = 0; row < dl_only.rows; ++row)
      for (uint32_t subcarrier = 0; subcarrier < dl_only.subcarriers; ++subcarrier) {
        if (row % 7 == 6) dl_only.observed[dl_only.cell(row, subcarrier)] = 0;
        for (uint32_t antenna = 0; antenna < dl_only.antennas; ++antenna)
          dl_only.values[dl_only.sample(antenna, row, subcarrier)] *=
              std::polar(0.81f, static_cast<float>(0.002 * row - 0.0001 * subcarrier));
      }
    const auto started = std::chrono::steady_clock::now();
    const auto stats = align_allocation_families_pair(fused, dl_only, true);
    const auto done = std::chrono::steady_clock::now();
    if (stats.first.aligned_rows == 0 || stats.second.aligned_rows == 0) std::abort();
    return std::chrono::duration<double, std::milli>(done - started).count();
  };
  (void)run_dual();
  std::vector<double> dual_samples;
  for (uint32_t repetition = 0; repetition < 7; ++repetition) dual_samples.push_back(run_dual());
  std::sort(dual_samples.begin(), dual_samples.end());
  const double dual_median = dual_samples[dual_samples.size() / 2];
  const char* configured_dual_limit = std::getenv("NR_ISAC_CUDA_DUAL_FAMILY_BENCHMARK_MAX_MS");
  const double dual_limit_ms = configured_dual_limit
      ? std::strtod(configured_dual_limit, nullptr) : 30.0;
  std::printf("CUDA dual family 4x192x3276: min_combined=%.3f ms median_combined=%.3f ms "
              "max_combined=%.3f ms limit=%.3f ms\n",
              dual_samples.front(), dual_median, dual_samples.back(), dual_limit_ms);
  if (!(dual_median < dual_limit_ms)) {
    std::fputs("CUDA dual family alignment misses required combined throughput\n", stderr);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
