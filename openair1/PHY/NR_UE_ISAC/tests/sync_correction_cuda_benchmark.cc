/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** Deterministic live-density timing diagnostic for common-mode sync correction. */
#include "sync_correction.h"
#include "sync_correction_cuda.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace nr_isac;

namespace {

const std::array<std::complex<double>, 4> LOS_SPATIAL{
    std::polar(1.0, 0.0), std::polar(1.0, .23),
    std::polar(1.0, -.41), std::polar(1.0, .68)};

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
  window.observed.assign(cells, 1);
  window.row_time_slots.resize(window.rows);
  window.row_slot_idx.resize(window.rows);
  window.row_slot_frac.resize(window.rows);
  window.row_source_mask.assign(window.rows, (1u << NR_ISAC_SRC_PDSCH_DATA)
                                              | (1u << NR_ISAC_SRC_PUSCH_DATA));
  uint32_t state = 0x51A0C0DEu;
  for (uint32_t row = 0; row < window.rows; ++row) {
    const double slot = row * (63.5 / (window.rows - 1));
    const double time = slot * slot_duration_s(window.scs_hz);
    window.row_time_slots[row] = slot;
    window.row_slot_idx[row] = static_cast<uint32_t>(slot);
    window.row_slot_frac[row] = slot - std::floor(slot);
    const double row_delay = 2.25 + .15e-6 * window.subcarriers * window.scs_hz * time;
    const double cpe = .23 + 2.0 * PI * 17.0 * time;
    for (uint32_t carrier = 0; carrier < window.subcarriers; ++carrier) {
      const double centered = carrier - .5 * (window.subcarriers - 1.0);
      const double delay_phase = -2.0 * PI * row_delay * centered / window.subcarriers;
      for (uint32_t antenna = 0; antenna < window.antennas; ++antenna) {
        state = 1664525u * state + 1013904223u;
        const double noise_real = (static_cast<double>(state >> 8) / 16777216.0 - .5) * .01;
        state = 1664525u * state + 1013904223u;
        const double noise_imag = (static_cast<double>(state >> 8) / 16777216.0 - .5) * .01;
        const std::complex<double> los = 3.0 * LOS_SPATIAL[antenna]
                                         * std::polar(1.0, delay_phase + cpe);
        const std::complex<double> target = .7 * std::polar(
            1.0, -2.0 * PI * 12.4 * carrier / window.subcarriers + .04 * row + .11 * antenna);
        const std::complex<double> value = los + target
                                           + std::complex<double>(noise_real, noise_imag);
        window.values[window.sample(antenna, row, carrier)] = {
            static_cast<float>(value.real()), static_cast<float>(value.imag())};
      }
    }
  }
  return window;
}

} // namespace

int main()
{
  std::string error;
  if (!warmup_sync_correction_cuda(512, 3276, 4, &error)) {
    std::fprintf(stderr, "CUDA sync-correction warmup failed: %s\n", error.c_str());
    return EXIT_FAILURE;
  }
  setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
  SyncEstimate estimate;
  estimate.rows = 192;
  estimate.los_bins = 2.25;
  estimate.sto_applied = true;
  estimate.sfo_applied = true;
  estimate.sfo_ppm = .15;
  estimate.cfo_applied = true;
  estimate.cfo_hz = 17.0;
  const CfrWindow input = dense_window();
  auto run = [&](CfrWindow* output = nullptr) {
    CfrWindow corrected = input;
    const auto started = std::chrono::steady_clock::now();
    apply_sync_correction(corrected, estimate, 0.0, LOS_SPATIAL);
    const auto stopped = std::chrono::steady_clock::now();
    if (!std::isfinite(corrected.values[corrected.sample(3, 191, 3275)].real()))
      throw std::runtime_error("CUDA sync correction produced a non-finite sample");
    if (output) *output = std::move(corrected);
    return std::chrono::duration<double, std::milli>(stopped - started).count();
  };
  const double first_ms = run();
  std::vector<double> samples;
  for (uint32_t repetition = 0; repetition < 5; ++repetition)
    samples.push_back(run());
  std::sort(samples.begin(), samples.end());
  const double median_ms = samples[samples.size() / 2];
  const char* configured_limit = std::getenv("NR_ISAC_CUDA_SYNC_CORRECTION_MAX_MS");
  const double limit_ms = configured_limit ? std::strtod(configured_limit, nullptr) : 25.0;
  std::printf("CUDA sync correction dense CPI: first=%.3f ms min=%.3f ms median=%.3f ms "
              "max=%.3f ms limit=%.3f ms\n",
              first_ms, samples.front(), median_ms, samples.back(), limit_ms);
  if (!(median_ms < limit_ms)) {
    std::fputs("CUDA sync correction misses required steady-state throughput\n", stderr);
    return EXIT_FAILURE;
  }

  CfrWindow cuda_corrected;
  run(&cuda_corrected);
  unsetenv("NR_ISAC_REQUIRE_CUDA");
  setenv("NR_ISAC_DISABLE_CUDA_SYNC", "1", 1);
  CfrWindow cpu_corrected = input;
  apply_sync_correction(cpu_corrected, estimate, 0.0, LOS_SPATIAL);
  unsetenv("NR_ISAC_DISABLE_CUDA_SYNC");
  double maximum_error = 0.0;
  for (size_t index = 0; index < cpu_corrected.values.size(); ++index)
    maximum_error = std::max(
        maximum_error,
        std::abs(static_cast<std::complex<double>>(cpu_corrected.values[index])
                 - static_cast<std::complex<double>>(cuda_corrected.values[index])));
  std::printf("CUDA/CPU dense correction parity: max_complex_error=%.9g limit=0.0002\n",
              maximum_error);
  if (!(maximum_error < 2e-4)) {
    std::fputs("CUDA sync correction differs from the CPU reference\n", stderr);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
