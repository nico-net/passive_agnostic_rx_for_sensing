/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** CPU/CUDA decision-equivalence check for the native continuous-CLEAN detector. */
#include "clean_detector.h"
#include "detector_cuda.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

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

// Fixed-seed LCG + Box-Muller, deterministic across runs/platforms. Same shape as
// python_parity_test.cc's seeded_gaussian() and detector_cuda_benchmark.cc's dense_window()
// noise, reused here so CLEAN's CFAR background statistic sees a genuine (not degenerate/
// noiseless) population -- P17.
double seeded_gaussian(uint32_t& state)
{
  auto next_uniform = [&]() {
    state = 1664525u * state + 1013904223u;
    return std::max((state >> 8) / 16777216.0, 1e-12);
  };
  const double u1 = next_uniform(), u2 = next_uniform();
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * PI * u2);
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
  const double noise_sigma = 0.05;
  uint32_t noise_state = 0xC0FFEEu;
  for (uint32_t row = 0; row < window.rows; ++row) {
    window.row_time_slots[row] = row;
    window.row_slot_idx[row] = row;
    const double time = row * slot_duration_s(window.scs_hz);
    for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
      const double phase = -2.0 * PI * range_bin * subcarrier / window.subcarriers
                           - 2.0 * PI * rate * window.fc_hz * time / C_MPS;
      for (uint32_t antenna = 0; antenna < window.antennas; ++antenna) {
        const std::complex<double> signal = std::polar(1.0, phase + antenna_phase[antenna]);
        const std::complex<double> noise(noise_sigma * seeded_gaussian(noise_state),
                                         noise_sigma * seeded_gaussian(noise_state));
        window.values[window.sample(antenna, row, subcarrier)] =
            static_cast<std::complex<float>>(signal + noise);
      }
    }
  }
  return window;
}

// canonical CLEAN (clean_detector.h) accepts one independent receiver; split spatial RF channels
// first, mirroring sensing_engine.cc's independent_receiver_view().
CfrWindow receiver_view(const CfrWindow& input, uint32_t receiver)
{
  CfrWindow output = input;
  output.antennas = 1;
  const size_t cells = static_cast<size_t>(input.rows) * input.subcarriers;
  output.values.assign(input.values.begin() + receiver * cells,
                       input.values.begin() + (receiver + 1) * cells);
  return output;
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
    // P17: 300 m (16 range bins @ 512 sc/30 kHz) leaves too few CFAR training cells after the
    // detector's own guard exclusion -- every CPI reads as noise before any component is
    // scored (see clean_detector.cc's P17(d) warning). 700 m (36 bins) clears that floor.
    config.maximum_range_m = 700.0;
    config.maximum_target_speed_mps = 50.0;
    // Declared false-alarm budget required by clean_detector.cc's admission gate
    // (config.false_object_intensity_per_s * dwell_s must lie in (0,1)); this fixture's dwell is
    // ~0.0315 s, so 1.0/s keeps the budget comfortably inside range.
    config.false_object_intensity_per_s = 1.0;
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
    // Canonical CLEAN accepts one independent receiver: split the 4-antenna window into per-
    // receiver 1-antenna views (P11), mirroring sensing_engine.cc's independent_receiver_view()
    // and process_receiver() -- one CLEAN run per receiver, not one 4-antenna run.
    std::vector<DetectorResult> cpu(window.antennas), cuda(window.antennas),
        cuda_fused_only(window.antennas);
    std::vector<DiagnosticLikelihoodMap> cuda_dl_only(window.antennas);
    setenv("NR_ISAC_REQUIRE_CUDA", "0", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "0", 1);
    for (uint32_t receiver = 0; receiver < window.antennas; ++receiver)
      cpu[receiver] = detect_clean_with_diagnostic(
          receiver_view(window, receiver), receiver_view(dl_window, receiver), config);
    setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "1", 1);
    detector_cuda_warmup();
    for (uint32_t receiver = 0; receiver < window.antennas; ++receiver) {
      const CfrWindow rx_window = receiver_view(window, receiver);
      const CfrWindow rx_dl_window = receiver_view(dl_window, receiver);
      cuda[receiver] = detect_clean_with_diagnostic(rx_window, rx_dl_window, config);
      cuda_fused_only[receiver] = detect_clean(rx_window, config);
      cuda_dl_only[receiver] = diagnostic_likelihood_map(rx_dl_window, config);
    }
    for (uint32_t receiver = 0; receiver < window.antennas; ++receiver) {
      require(cuda[receiver].initial_likelihood == cuda_fused_only[receiver].initial_likelihood
                  && cuda[receiver].initial_weighted_energy
                         == cuda_fused_only[receiver].initial_weighted_energy
                  && cuda[receiver].final_weighted_energy
                         == cuda_fused_only[receiver].final_weighted_energy,
              "concurrent diagnostic changed fused CUDA detector results");
      require(cuda[receiver].components.size() == cuda_fused_only[receiver].components.size(),
              "concurrent diagnostic changed fused component count");
      for (size_t index = 0; index < cuda[receiver].components.size(); ++index) {
        const auto& concurrent = cuda[receiver].components[index];
        const auto& isolated = cuda_fused_only[receiver].components[index];
        require(concurrent.range_bin == isolated.range_bin
                    && concurrent.doppler_bin == isolated.doppler_bin
                    && concurrent.score == isolated.score
                    && concurrent.raw_score == isolated.raw_score
                    && concurrent.local.z == isolated.local.z,
                "concurrent diagnostic changed a fused component");
      }
      require(cuda[receiver].initial_dl_likelihood == cuda_dl_only[receiver].likelihood,
              "concurrent and standalone CUDA diagnostic maps differ");
      // Non-vacuous per P11: every receiver must actually yield a component, with finite energy.
      require(!cpu[receiver].components.empty() && !cuda[receiver].components.empty(),
              "a receiver yielded no CLEAN component");
      require(std::isfinite(cpu[receiver].components.front().weighted_energy_removed)
                  && std::isfinite(cuda[receiver].components.front().weighted_energy_removed),
              "a receiver's component energy is not finite");
      require(cpu[receiver].components.size() == cuda[receiver].components.size(),
              "CPU/CUDA component count differs");
      require(cpu[receiver].objects.size() == cuda[receiver].objects.size(),
              "CPU/CUDA object count differs");
      require(cpu[receiver].initial_likelihood.size() == cuda[receiver].initial_likelihood.size(),
              "CPU/CUDA initial map shape differs");
      require(cpu[receiver].initial_dl_likelihood.size()
                      == cuda[receiver].initial_dl_likelihood.size()
                  && cpu[receiver].initial_dl_likelihood.size()
                         == cpu[receiver].initial_likelihood.size(),
              "CPU/CUDA DL-only map shape differs");
      require(cpu[receiver].dl_observed_re_count == cuda[receiver].dl_observed_re_count
                  && cpu[receiver].dl_observed_re_count > 0,
              "CPU/CUDA DL-only observed-RE count differs");
      double map_scale = 0.0, map_error = 0.0;
      for (size_t index = 0; index < cpu[receiver].initial_likelihood.size(); ++index) {
        const double expected = cpu[receiver].initial_likelihood[index];
        const double actual = cuda[receiver].initial_likelihood[index];
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
      for (size_t index = 0; index < cpu[receiver].initial_dl_likelihood.size(); ++index) {
        const double expected_dl = cpu[receiver].initial_dl_likelihood[index];
        const double actual_dl = cuda[receiver].initial_dl_likelihood[index];
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
      const auto& expected = cpu[receiver].components.front();
      const auto& actual = cuda[receiver].components.front();
      close(actual.range_bin, expected.range_bin, 3e-4, 0.0, "continuous range parity");
      close(actual.doppler_bin, expected.doppler_bin, 3e-4, 0.0, "continuous Doppler parity");
      close(actual.score, expected.score, 1e-3, 5e-5, "component score parity");
      close(actual.raw_score, expected.raw_score, 1e-3, 5e-5, "raw score parity");
      close(actual.local.z, expected.local.z, 2e-3, 2e-4, "local statistic parity");
      require(actual.local.training_cells == expected.local.training_cells,
              "local training-cell count differs");
      close(cuda[receiver].initial_weighted_energy, cpu[receiver].initial_weighted_energy,
            1e-6, 1e-10, "initial residual energy parity");
      close(cuda[receiver].final_weighted_energy, cpu[receiver].final_weighted_energy,
            1e-5, 2e-5, "final residual energy parity");
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "CUDA detector parity failed: %s\n", error.what());
    return EXIT_FAILURE;
  }
  std::puts("CUDA detector matches CPU decisions and diagnostics");
  return EXIT_SUCCESS;
}
