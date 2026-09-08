/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/** CPU/CUDA parity for allocation-family alignment and current-CPI variance. */
#include "detector_cuda.h"
#include "sync_correction.h"

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

CfrWindow fixture(bool repeated = true)
{
  CfrWindow window;
  window.antennas = 4;
  window.rows = 12;
  window.subcarriers = 128;
  window.scs_hz = 30000.0;
  window.fc_hz = 3499440000.0;
  const size_t cells = static_cast<size_t>(window.rows) * window.subcarriers;
  window.values.resize(window.antennas * cells);
  window.observed.assign(cells, 0);
  window.row_time_slots.resize(window.rows);
  window.row_slot_idx.resize(window.rows);
  window.row_slot_frac.resize(window.rows);
  window.row_source_mask.resize(window.rows);
  for (uint32_t row = 0; row < window.rows; ++row) {
    const uint32_t family = repeated ? row % 3 : row;
    window.row_time_slots[row] = row;
    window.row_slot_idx[row] = row;
    window.row_slot_frac[row] = repeated ? (family == 1 ? .5 : 0.0) : row / 28.0;
    window.row_source_mask[row] = repeated ? (1u << family) : (1u << (row % 7));
    for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
      const bool occupied = repeated
                                ? ((family == 0 && subcarrier % 5 != 0)
                                   || (family == 1 && subcarrier >= 7 && subcarrier < 119)
                                   || (family == 2 && subcarrier % 3 != 1))
                                : subcarrier % 4 != row % 4;
      window.observed[window.cell(row, subcarrier)] = occupied;
      for (uint32_t antenna = 0; antenna < window.antennas; ++antenna) {
        const double delay = .17 * (row / 3);
        const double phase = -.021 * subcarrier * (family + 1.0)
                             - 2.0 * PI * delay * subcarrier / window.subcarriers
                             + .13 * antenna + .07 * row;
        const double amplitude = (1.0 + .04 * (row / 3)) * (1.0 + .03 * antenna);
        const double ripple = .002 * std::sin(.31 * row + .17 * subcarrier + antenna);
        window.values[window.sample(antenna, row, subcarrier)] =
            std::polar(static_cast<float>(amplitude + ripple), static_cast<float>(phase));
      }
    }
  }
  return window;
}

void compare_values(const CfrWindow& actual, const CfrWindow& expected, double tolerance)
{
  require(actual.values.size() == expected.values.size(), "aligned tensor shape differs");
  double maximum_error = 0.0;
  for (size_t index = 0; index < actual.values.size(); ++index)
    maximum_error = std::max(maximum_error,
                             static_cast<double>(std::abs(actual.values[index] - expected.values[index])));
  if (maximum_error > tolerance) {
    std::fprintf(stderr, "family alignment maximum complex error %.9g exceeds %.9g\n",
                 maximum_error, tolerance);
    throw std::runtime_error("CPU/C/CUDA aligned tensor differs");
  }
}
} // namespace

int main()
{
  if (!detector_cuda_available()) {
    std::puts("CUDA family-processing parity skipped: no enabled CUDA device");
    return EXIT_SUCCESS;
  }
  try {
    detector_cuda_warmup();
    for (bool subtract_static : {false, true}) {
      CfrWindow cpu = fixture();
      CfrWindow cuda = cpu;
      setenv("NR_ISAC_REQUIRE_CUDA", "0", 1);
      setenv("NR_ISAC_CUDA_DETECTOR", "0", 1);
      const auto expected = align_allocation_families(cpu, subtract_static);
      setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
      setenv("NR_ISAC_CUDA_DETECTOR", "1", 1);
      const auto actual = align_allocation_families(cuda, subtract_static);
      require(actual.families == expected.families
                  && actual.repeated_families == expected.repeated_families
                  && actual.aligned_rows == expected.aligned_rows
                  && actual.singleton_rows == expected.singleton_rows,
              "CPU/CUDA family statistics differ");
      compare_values(cuda, cpu, 2e-5);
    }

    // The dual path must be decision-equivalent to two independent alignments even when its
    // provenance-preserved DL view has different complex samples and occupancy.
    CfrWindow cpu_fused = fixture();
    CfrWindow cpu_dl = fixture();
    for (uint32_t row = 0; row < cpu_dl.rows; ++row)
      for (uint32_t subcarrier = 0; subcarrier < cpu_dl.subcarriers; ++subcarrier) {
        if (row % 4 == 3) cpu_dl.observed[cpu_dl.cell(row, subcarrier)] = 0;
        for (uint32_t antenna = 0; antenna < cpu_dl.antennas; ++antenna)
          cpu_dl.values[cpu_dl.sample(antenna, row, subcarrier)] *=
              std::polar(0.73f, static_cast<float>(0.03 * row - 0.001 * subcarrier));
      }
    CfrWindow cuda_fused = cpu_fused;
    CfrWindow cuda_dl = cpu_dl;
    setenv("NR_ISAC_REQUIRE_CUDA", "0", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "0", 1);
    const auto expected_fused = align_allocation_families(cpu_fused, true);
    const auto expected_dl = align_allocation_families(cpu_dl, true);
    setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "1", 1);
    const auto actual_pair = align_allocation_families_pair(cuda_fused, cuda_dl, true);
    require(actual_pair.first.families == expected_fused.families
                && actual_pair.first.repeated_families == expected_fused.repeated_families
                && actual_pair.first.aligned_rows == expected_fused.aligned_rows
                && actual_pair.first.singleton_rows == expected_fused.singleton_rows
                && actual_pair.second.families == expected_dl.families
                && actual_pair.second.repeated_families == expected_dl.repeated_families
                && actual_pair.second.aligned_rows == expected_dl.aligned_rows
                && actual_pair.second.singleton_rows == expected_dl.singleton_rows,
            "dual CUDA family statistics differ from sequential CPU alignment");
    compare_values(cuda_fused, cpu_fused, 2e-5);
    compare_values(cuda_dl, cpu_dl, 2e-5);

    const CfrWindow covariance = fixture();
    uint32_t cpu_families = 0, cuda_families = 0;
    uint64_t cpu_samples = 0, cuda_samples = 0;
    setenv("NR_ISAC_REQUIRE_CUDA", "0", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "0", 1);
    const double cpu_variance = estimate_current_cpi_variance(
        covariance, &cpu_families, &cpu_samples);
    setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "1", 1);
    const double cuda_variance = estimate_current_cpi_variance(
        covariance, &cuda_families, &cuda_samples);
    require(cpu_families == cuda_families && cpu_samples == cuda_samples,
            "CPU/CUDA covariance diagnostics differ");
    require(std::abs(cpu_variance - cuda_variance)
                <= 1e-12 * std::max(1.0, std::abs(cpu_variance)),
            "CPU/CUDA within-family variance differs");

    const CfrWindow raw = fixture(false);
    setenv("NR_ISAC_REQUIRE_CUDA", "0", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "0", 1);
    const double cpu_raw = estimate_current_cpi_variance(raw);
    setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
    setenv("NR_ISAC_CUDA_DETECTOR", "1", 1);
    const double cuda_raw = estimate_current_cpi_variance(raw);
    require(std::abs(cpu_raw - cuda_raw) <= 1e-12 * std::max(1.0, std::abs(cpu_raw)),
            "CPU/CUDA raw-power variance fallback differs");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "CUDA family-processing parity failed: %s\n", error.what());
    return EXIT_FAILURE;
  }
  std::puts("CUDA family alignment and current-CPI variance match CPU semantics");
  return EXIT_SUCCESS;
}
