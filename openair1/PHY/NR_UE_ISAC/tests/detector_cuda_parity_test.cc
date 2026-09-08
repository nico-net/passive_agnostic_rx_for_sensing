/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** CPU/CUDA decision-equivalence check for the native continuous-CLEAN detector. */
#include "detector.h"
#include "detector_cuda.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace nr_isac;

namespace {
void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

void close(double actual, double expected, double absolute, double relative, const char* message)
{
  const double tolerance = absolute + relative * std::max(std::abs(actual), std::abs(expected));
  if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > tolerance) {
    std::fprintf(stderr, "%s: CUDA=%.17g CPU=%.17g tolerance=%.6g\n",
                 message, actual, expected, tolerance);
    throw std::runtime_error(message);
  }
}

CfrWindow fixture()
{
  CfrWindow window;
  window.antennas = 4;
  window.rows = 64;
  window.subcarriers = 512;
  window.scs_hz = 30000.0;
  window.fc_hz = 3499440000.0;
  const size_t cells = static_cast<size_t>(window.rows) * window.subcarriers;
  window.values.resize(window.antennas * cells);
  window.observed.assign(cells, 1);
  window.row_time_slots.resize(window.rows);
  window.row_slot_idx.resize(window.rows);
  window.row_slot_frac.assign(window.rows, 0.0);
  window.row_source_mask.assign(window.rows, 1u << NR_ISAC_SRC_CSI_RS);
  const double range_bin = 12.25, doppler_bin = 32.3125;
  const double dwell = (window.rows - 1) * slot_duration_s(window.scs_hz);
  const double rate_resolution = C_MPS * (window.rows - 1.0)
                                 / (window.fc_hz * window.rows * dwell);
  const double rate = -(doppler_bin - window.rows / 2.0) * rate_resolution;
  const double antenna_phase[4]{0.0, .25, -.4, .7};
  for (uint32_t row = 0; row < window.rows; ++row) {
    window.row_time_slots[row] = row;
    window.row_slot_idx[row] = row;
    const double time = row * slot_duration_s(window.scs_hz);
    for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
      const double phase = -2.0 * PI * range_bin * subcarrier / window.subcarriers
                           - 2.0 * PI * rate * window.fc_hz * time / C_MPS;
      for (uint32_t antenna = 0; antenna < window.antennas; ++antenna)
        window.values[window.sample(antenna, row, subcarrier)] =
            std::polar(1.0f, static_cast<float>(phase + antenna_phase[antenna]));
    }
  }
  return window;
}
} // namespace

int main()
{
  if (!detector_cuda_available()) {
    std::puts("CUDA detector parity skipped: no enabled CUDA device");
    return EXIT_SUCCESS;
  }
  try {
    PipelineConfig config;
    config.maximum_components = 1;
    config.maximum_objects = 1;
    config.maximum_range_m = 300.0;
    config.maximum_target_speed_mps = 50.0;
    config.capture_rvm = true;
    auto window = fixture();
    for (uint32_t row = 0; row < window.rows; ++row)
      window.row_source_mask[row] = row % 4 == 0
          ? 1u << NR_ISAC_SRC_PUSCH_DATA
          : 1u << NR_ISAC_SRC_PDSCH_DATA;
    CfrWindow dl_window = window;
    for (uint32_t row = 0; row < dl_window.rows; ++row)
      if ((dl_window.row_source_mask[row] & DL_SOURCE_BITS) == 0)
        for (uint32_t subcarrier = 0; subcarrier < dl_window.subcarriers; ++subcarrier)
          dl_window.observed[dl_window.cell(row, subcarrier)] = 0;
    setenv("NR_ISAC_CUDA_DETECTOR", "0", 1);
    const auto cpu = detect_clean(window, config);
    const auto cpu_dl = diagnostic_likelihood_map(dl_window, config);
    setenv("NR_ISAC_CUDA_DETECTOR", "1", 1);
    detector_cuda_warmup();
    const auto cuda = detect_clean(window, config);
    const auto cuda_dl = diagnostic_likelihood_map(dl_window, config);
    require(cpu.components.size() == 1 && cuda.components.size() == 1,
            "CPU/CUDA component count differs");
    require(cpu.objects.size() == cuda.objects.size(), "CPU/CUDA object count differs");
    require(cpu.initial_likelihood.size() == cuda.initial_likelihood.size(),
            "CPU/CUDA initial map shape differs");
    require(cpu_dl.likelihood.size() == cuda_dl.likelihood.size()
                && cpu_dl.likelihood.size() == cpu.initial_likelihood.size(),
            "CPU/CUDA DL-only map shape differs");
    require(cpu_dl.axes.observed_re_count == cuda_dl.axes.observed_re_count
                && cpu_dl.axes.observed_re_count > 0,
            "CPU/CUDA DL-only observed-RE count differs");
    double map_scale = 0.0, map_error = 0.0;
    for (size_t index = 0; index < cpu.initial_likelihood.size(); ++index) {
      const double expected = cpu.initial_likelihood[index];
      const double actual = cuda.initial_likelihood[index];
      if (!std::isfinite(expected) || !std::isfinite(actual)) {
        require(std::isfinite(expected) == std::isfinite(actual),
                "CPU/CUDA initial map support differs");
        continue;
      }
      map_scale = std::max(map_scale, std::abs(expected));
      map_error = std::max(map_error, std::abs(actual - expected));
    }
    require(map_error <= 5e-5 * std::max(map_scale, 1.0),
            "CPU/CUDA initial likelihood exceeds complex64 tolerance");
    double dl_map_scale = 0.0, dl_map_error = 0.0;
    for (size_t index = 0; index < cpu_dl.likelihood.size(); ++index) {
      const double expected_dl = cpu_dl.likelihood[index];
      const double actual_dl = cuda_dl.likelihood[index];
      if (!std::isfinite(expected_dl) || !std::isfinite(actual_dl)) {
        require(std::isfinite(expected_dl) == std::isfinite(actual_dl),
                "CPU/CUDA DL-only map support differs");
        continue;
      }
      dl_map_scale = std::max(dl_map_scale, std::abs(expected_dl));
      dl_map_error = std::max(dl_map_error, std::abs(actual_dl - expected_dl));
    }
    require(dl_map_error <= 5e-5 * std::max(dl_map_scale, 1.0),
            "CPU/CUDA DL-only likelihood exceeds complex64 tolerance");
    const auto& expected = cpu.components.front();
    const auto& actual = cuda.components.front();
    close(actual.range_bin, expected.range_bin, 3e-4, 0.0, "continuous range parity");
    close(actual.doppler_bin, expected.doppler_bin, 3e-4, 0.0, "continuous Doppler parity");
    close(actual.score, expected.score, 1e-3, 5e-5, "component score parity");
    close(actual.raw_score, expected.raw_score, 1e-3, 5e-5, "raw score parity");
    close(actual.local.z, expected.local.z, 2e-3, 2e-4, "local statistic parity");
    require(actual.local.training_cells == expected.local.training_cells,
            "local training-cell count differs");
    close(cuda.initial_weighted_energy, cpu.initial_weighted_energy, 1e-6, 1e-10,
          "initial residual energy parity");
    close(cuda.final_weighted_energy, cpu.final_weighted_energy, 1e-5, 2e-5,
          "final residual energy parity");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "CUDA detector parity failed: %s\n", error.what());
    return EXIT_FAILURE;
  }
  std::puts("CUDA detector matches CPU decisions and diagnostics");
  return EXIT_SUCCESS;
}
