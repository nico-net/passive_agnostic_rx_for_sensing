/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <array>
#include <complex>
#include <vector>

namespace nr_isac {

struct AoaIsolation {
  bool valid = false;
  std::string reason;
  std::array<std::complex<double>, 4> response{};
  bool phase_covariance_valid = false;
  Matrix baseline_phase_covariance{3, 3};
  double unique_energy_fraction = 0.0;
  double residual_energy_fraction = 0.0;
};

std::array<std::complex<double>, 4> surveyed_los_steering(
    const ArrayGeometry& geometry, Vec3 tx_position, Vec3 rx_position, double carrier_hz);

std::array<std::complex<double>, 4> project_array_response(
    const CfrWindow& window, const std::vector<uint8_t>& observed,
    double range_m, double range_rate_mps);

AoaIsolation isolate_target_response(
    const CfrWindow& aligned_window, const std::vector<uint8_t>& observed,
    double target_range_m, double target_rate_mps,
    const std::vector<std::pair<double, double>>& nuisance_range_rate,
    const std::array<std::complex<double>, 4>& los_spatial_steering,
    double los_range_m = 0.0);

AoaEstimate grid_free_upa_aoa(
    const std::array<std::complex<double>, 4>& response,
    const ArrayGeometry& geometry, double carrier_hz,
    const Matrix* baseline_phase_covariance = nullptr);

/** Apply the Python response-selection policy to all accepted objects. */
void attach_aoa(CfrWindow aligned_unprojected,
                const std::vector<CleanComponent>& all_components,
                const Axes& axes, const PipelineConfig& config,
                std::vector<Detection>& detections,
                bool surveyed_los_available = true);

} // namespace nr_isac
