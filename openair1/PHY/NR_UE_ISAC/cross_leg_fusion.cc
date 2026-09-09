/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "cross_leg_fusion.h"

#include "enu_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace nr_isac {
namespace {

bool valid_covariance(const AoaEstimate& estimate)
{
  if (!estimate.valid || !estimate.covariance_valid
      || estimate.covariance_rad2.rows() != 2 || estimate.covariance_rad2.cols() != 2)
    return false;
  for (size_t r = 0; r < 2; ++r)
    for (size_t c = 0; c < 2; ++c)
      if (!std::isfinite(estimate.covariance_rad2(r, c))) return false;
  return true;
}

double angle_pair_cost(const AoaEstimate& dl, const AoaEstimate& ul,
                       const CrossLegFusionConfig& config)
{
  if (!valid_covariance(dl) || !valid_covariance(ul))
    return std::numeric_limits<double>::infinity();
  std::vector<double> residual{
      wrap_radians((dl.azimuth_deg - ul.azimuth_deg) * PI / 180.0),
      (dl.elevation_deg - ul.elevation_deg) * PI / 180.0};
  Matrix covariance = symmetrized(dl.covariance_rad2) + symmetrized(ul.covariance_rad2);
  const double floor = std::pow(config.minimum_cross_leg_stddev_deg * PI / 180.0, 2);
  covariance(0, 0) += floor;
  covariance(1, 1) += floor;
  try {
    return quadratic(residual, pseudoinverse_symmetric(covariance));
  } catch (...) {
    return std::numeric_limits<double>::infinity();
  }
}

AoaEstimate auxiliary_covariance_floor(AoaEstimate estimate,
                                       const CrossLegFusionConfig& config)
{
  const double floor = std::pow(config.minimum_cross_leg_stddev_deg * PI / 180.0, 2);
  estimate.covariance_rad2 = symmetrized(estimate.covariance_rad2);
  estimate.covariance_rad2(0, 0) += floor;
  estimate.covariance_rad2(1, 1) += floor;
  estimate.covariance_rad2 = positive_semidefinite(estimate.covariance_rad2);
  estimate.covariance_status = "measured_plus_cross_leg_systematic_floor";
  return estimate;
}

} // namespace

CrossLegFusionResult confirm_and_fuse_dl_with_ul(
    const std::vector<Detection>& dl_measurements,
    const std::vector<Detection>& ul_measurements,
    bool use_ul_aoa,
    CrossLegFusionConfig config)
{
  if (!(config.angle_gate_chi2 > 0.0)
      || !(config.minimum_cross_leg_stddev_deg > 0.0))
    throw std::invalid_argument("cross-leg angular gate parameters must be positive");
  CrossLegFusionResult result;
  result.dl_measurements = dl_measurements;
  result.diagnostics.ul_motion_active = !ul_measurements.empty();
  std::vector<AoaEstimate> raw_ul_aoa;
  if (use_ul_aoa)
    for (const auto& measurement : ul_measurements)
      if (valid_covariance(measurement.aoa)) {
        raw_ul_aoa.push_back(measurement.aoa);
        result.auxiliary_ul_aoa.push_back(
            auxiliary_covariance_floor(measurement.aoa, config));
      }

  if (!use_ul_aoa || raw_ul_aoa.empty()) {
    result.diagnostics.mode = use_ul_aoa ? "no_admitted_ul_aoa" : "no_ul_aoa";
    for (auto& measurement : result.dl_measurements) {
      measurement.ul_confirmation_supported = result.diagnostics.ul_motion_active;
      measurement.ul_confirmation_candidate_specific = false;
      measurement.ul_confirmation_status = result.diagnostics.ul_motion_active
          ? "scene_motion_support" : "no_ul_motion_support";
    }
    result.auxiliary_ul_aoa.clear();
    return result;
  }

  std::vector<std::tuple<double, size_t, size_t>> pairs;
  for (size_t dl_index = 0; dl_index < result.dl_measurements.size(); ++dl_index)
    for (size_t ul_index = 0; ul_index < raw_ul_aoa.size(); ++ul_index) {
      const double cost = angle_pair_cost(
          result.dl_measurements[dl_index].aoa, raw_ul_aoa[ul_index], config);
      if (cost <= config.angle_gate_chi2)
        pairs.emplace_back(cost, dl_index, ul_index);
    }
  std::sort(pairs.begin(), pairs.end());
  std::set<size_t> assigned_dl;
  std::set<size_t> assigned_ul;
  for (const auto& [cost, dl_index, ul_index] : pairs) {
    (void)cost;
    if (assigned_dl.count(dl_index) || assigned_ul.count(ul_index)) continue;
    assigned_dl.insert(dl_index);
    assigned_ul.insert(ul_index);
    auto& measurement = result.dl_measurements[dl_index];
    measurement.ul_confirmation_supported = true;
    measurement.ul_confirmation_candidate_specific = true;
    measurement.ul_confirmation_status = "candidate_specific_bearing_match";
  }
  for (size_t index = 0; index < result.dl_measurements.size(); ++index) {
    if (assigned_dl.count(index)) continue;
    auto& measurement = result.dl_measurements[index];
    measurement.ul_confirmation_supported = result.diagnostics.ul_motion_active;
    measurement.ul_confirmation_candidate_specific = false;
    measurement.ul_confirmation_status = "no_cross_leg_bearing_match_preserved";
  }
  result.diagnostics.candidate_specific_matches = assigned_dl.size();
  result.diagnostics.mode = "ul_receive_bearing_confirmation";
  return result;
}

} // namespace nr_isac
