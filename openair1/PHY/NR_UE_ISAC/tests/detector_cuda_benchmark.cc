/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Deterministic live-density detector timing diagnostic.
 *
 * Mirrors the observed four-channel, about-192-row, 3276-subcarrier CPI shape and exercises all
 * eight sequential CLEAN passes, including continuous refinement and local statistics. The first
 * call is deliberately excluded so CUDA context, cuFFT-plan, and code-loading startup do not count
 * against steady-state stream throughput.
 */
#include "detector.h"
#include "detector_cuda.h"

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
  window.values.assign(window.antennas * cells, {});
  window.observed.assign(cells, 1);
  window.row_time_slots.resize(window.rows);
  window.row_slot_idx.resize(window.rows);
  window.row_slot_frac.resize(window.rows);
  window.row_source_mask.resize(window.rows, (1u << NR_ISAC_SRC_PDSCH_DATA)
                                              | (1u << NR_ISAC_SRC_PUSCH_DATA));
  // 192 allocation rows distributed across the 32 ms CPI, as in the retained traffic capture.
  for (uint32_t row = 0; row < window.rows; ++row) {
    const double slot = row * (63.5 / (window.rows - 1));
    window.row_time_slots[row] = slot;
    window.row_slot_idx[row] = static_cast<uint32_t>(slot);
    window.row_slot_frac[row] = slot - std::floor(slot);
  }
  const double dwell = (window.row_time_slots.back() - window.row_time_slots.front())
                       * slot_duration_s(window.scs_hz);
  const double rate_resolution = C_MPS * (window.rows - 1.0)
                                 / (window.fc_hz * window.rows * dwell);
  const double ranges[8]{5.25, 13.40, 24.75, 36.15, 49.50, 63.20, 78.35, 94.10};
  const double dopplers[8]{84.2, 88.4, 91.6, 94.7, 97.8, 101.1, 104.3, 107.6};
  const double amplitudes[8]{1.0, .83, .72, .63, .55, .48, .42, .36};
  const double antenna_phase[4]{0.0, .23, -.41, .68};
  uint32_t random_state = 0x51A0AAu;
  for (uint32_t antenna = 0; antenna < window.antennas; ++antenna)
    for (uint32_t row = 0; row < window.rows; ++row) {
      const double time_s = (window.row_time_slots[row] - window.row_time_slots[0])
                            * slot_duration_s(window.scs_hz);
      for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
        std::complex<double> value;
        for (uint32_t component = 0; component < 8; ++component) {
          const double rate = -(dopplers[component] - window.rows / 2.0) * rate_resolution;
          const double phase = -2.0 * PI * ranges[component] * subcarrier / window.subcarriers
                               - 2.0 * PI * rate * window.fc_hz * time_s / C_MPS
                               + antenna_phase[antenna] * (component + 1.0) / 8.0;
          value += std::polar(amplitudes[component], phase);
        }
        random_state = 1664525u * random_state + 1013904223u;
        const double noise_real = (static_cast<double>(random_state >> 8) / 16777216.0 - .5) * .002;
        random_state = 1664525u * random_state + 1013904223u;
        const double noise_imag = (static_cast<double>(random_state >> 8) / 16777216.0 - .5) * .002;
        value += std::complex<double>(noise_real, noise_imag);
        window.values[window.sample(antenna, row, subcarrier)] = {
            static_cast<float>(value.real()), static_cast<float>(value.imag())};
      }
    }
  return window;
}
} // namespace

int main()
{
  if (!detector_cuda_available()) {
    std::puts("CUDA detector benchmark skipped: no enabled CUDA device");
    return 77;
  }
  setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
  detector_cuda_warmup();
  PipelineConfig config;
  const char* configured_components = std::getenv("NR_ISAC_CUDA_BENCHMARK_COMPONENTS");
  config.maximum_components = configured_components
                                  ? static_cast<uint32_t>(std::strtoul(configured_components, nullptr, 10))
                                  : 8;
  config.maximum_objects = 8;
  config.maximum_range_m = 312.283810417;
  config.maximum_target_speed_mps = 50.0;
  const CfrWindow window = dense_window();
  auto run = [&]() {
    const auto started = std::chrono::steady_clock::now();
    const auto result = detect_clean(window, config);
    const auto stopped = std::chrono::steady_clock::now();
    if (result.components.size() != config.maximum_components || result.initial_likelihood.empty()) {
      std::fprintf(stderr, "dense CUDA detector returned %zu components and %zu map cells\n",
                   result.components.size(), result.initial_likelihood.size());
      std::exit(EXIT_FAILURE);
    }
    for (const auto& component : result.components)
      if (!component.local.valid || !std::isfinite(component.local.z)) {
        std::fputs("dense CUDA detector lost a component local statistic\n", stderr);
        std::exit(EXIT_FAILURE);
      }
    return std::chrono::duration<double, std::milli>(stopped - started).count();
  };
  const double warmup_ms = run();
  std::vector<double> samples;
  for (uint32_t repetition = 0; repetition < 5; ++repetition)
    samples.push_back(run());
  std::sort(samples.begin(), samples.end());
  const double median_ms = samples[samples.size() / 2];
  const char* configured_limit = std::getenv("NR_ISAC_CUDA_BENCHMARK_MAX_MS");
  const double limit_ms = configured_limit ? std::strtod(configured_limit, nullptr) : 200.0;
  std::printf("CUDA detector dense CPI: warmup=%.3f ms min=%.3f ms median=%.3f ms max=%.3f ms limit=%.3f ms\n",
              warmup_ms, samples.front(), median_ms, samples.back(), limit_ms);
  if (!(median_ms < limit_ms)) {
    std::fprintf(stderr, "CUDA detector misses required steady-state CPI throughput\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
