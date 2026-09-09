/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <optional>
#include <vector>

namespace nr_isac {

struct BistaticGeometry {
  Vec3 tx;
  Vec3 rx;
  double baseline_m() const { return norm(tx - rx); }
  void validate() const;
};

struct EnuTrackerConfig {
  double gate_chi2_2d = 9.21034037197618;
  double gate_chi2_4d = 13.276704135987622;
  uint32_t confirm_updates = 3;
  uint32_t confirm_window = 5;
  uint32_t maximum_coasts = 3;
  double maximum_propagation_s = 2.0;
  double adaptation_alpha = 0.25;
  uint32_t maximum_tracks = 8;
  // hierarchical_tracker.py freezes the birth-derived acceleration variance; the standalone
  // enu_tracker.py updates it from successive velocity corrections.
  bool adapt_acceleration_variance = true;
  double maximum_tangential_speed_mps = 50.0;
  double aoa_temporal_sigma = 3.0;
  double minimum_aoa_temporal_stddev_deg = 1.0;
};

double wrap_radians(double value);
Vec3 direction_from_angles(double azimuth_rad, double elevation_rad);
Vec3 position_from_measurement(const BistaticGeometry& geometry, double excess_range_m,
                               double azimuth_rad, double elevation_rad);
std::vector<double> enu_measurement_model(const std::vector<double>& state,
                                          const BistaticGeometry& geometry,
                                          bool with_angles);
Matrix enu_measurement_jacobian(const std::vector<double>& state,
                                const BistaticGeometry& geometry,
                                bool with_angles);
Matrix enu_transition(double dt);
Matrix enu_process_noise(double dt, double acceleration_variance);

struct EnuInnovation {
  bool valid = false;
  bool with_angles = false;
  double nis = 0.0;
  std::vector<double> residual;
  Matrix jacobian;
  Matrix noise;
};

class EnuTrack {
public:
  EnuTrack(uint64_t id, double time_s, const Detection& measurement,
           const BistaticGeometry& geometry, double range_resolution_m,
           double rate_resolution_mps, double dwell_s, const EnuTrackerConfig& config,
           std::optional<size_t> detection_index,
           std::optional<double> stage1_range_m = std::nullopt,
           std::optional<double> stage1_rate_mps = std::nullopt);

  void predict(double time_s);
  EnuInnovation innovation(const Detection& measurement, double range_resolution_m,
                           double rate_resolution_mps, bool force_2d) const;
  EnuInnovation innovation_for_geometry(const Detection& measurement,
                                        double range_resolution_m,
                                        double rate_resolution_mps, bool force_2d,
                                        const BistaticGeometry& geometry) const;
  bool update(const Detection& measurement, double range_resolution_m,
              double rate_resolution_mps, std::optional<size_t> detection_index,
              std::optional<double> stage1_range_m,
              std::optional<double> stage1_rate_mps);
  bool update_for_geometry(const Detection& measurement, double range_resolution_m,
                           double rate_resolution_mps,
                           const BistaticGeometry& geometry);
  EnuInnovation angle_innovation(const AoaEstimate& aoa) const;
  bool update_angles(const AoaEstimate& aoa);
  void coast(bool ul_motion_active = false);
  TrackSnapshot snapshot() const;
  void cap_birth_uncertainty(double maximum_velocity_variance,
                             double maximum_acceleration_variance);

  uint64_t id() const { return id_; }
  uint32_t coasts() const { return coasts_; }
  const std::string& status() const { return status_; }
  bool updated() const { return updated_; }
  const std::vector<double>& state() const { return x_; }
  const Matrix& covariance() const { return p_; }
  bool last_update_used_angles() const { return last_update_used_angles_; }
  bool auxiliary_aoa_allowed() const { return auxiliary_aoa_allowed_; }
  uint32_t temporal_aoa_rejections() const { return temporal_aoa_rejections_; }
  uint32_t auxiliary_aoa_updates() const { return auxiliary_aoa_updates_; }

private:
  Detection temporally_gated_measurement(const Detection& measurement);
  uint64_t id_ = 0;
  BistaticGeometry geometry_;
  EnuTrackerConfig config_;
  std::vector<double> x_{0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  Matrix p_{6, 6};
  double acceleration_variance_ = 0.0;
  double time_s_ = 0.0;
  double birth_time_s_ = 0.0;
  double last_update_time_s_ = 0.0;
  std::string status_ = "confirmed";
  uint32_t coasts_ = 0;
  uint32_t total_updates_ = 1;
  uint32_t confirmed_updates_ = 1;
  std::vector<uint8_t> recent_{1};
  std::optional<double> nis_;
  std::optional<double> nis_ewma_;
  bool updated_ = true;
  std::optional<size_t> associated_index_;
  std::optional<double> stage1_range_m_;
  std::optional<double> stage1_rate_mps_;
  double last_aoa_gate_time_s_ = 0.0;
  bool last_update_used_angles_ = true;
  bool auxiliary_aoa_allowed_ = false;
  uint32_t temporal_aoa_rejections_ = 0;
  uint32_t auxiliary_aoa_updates_ = 0;
};

} // namespace nr_isac
