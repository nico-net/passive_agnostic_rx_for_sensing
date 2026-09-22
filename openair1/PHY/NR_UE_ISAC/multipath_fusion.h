/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "enu_tracker.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace nr_isac {

enum class ReflectionLeg : uint8_t { receive = 0, transmit = 1 };

struct ReflectorPlane {
  uint64_t id = 0;
  Vec3 normal{1.0, 0.0, 0.0};
  double offset_m = 0.0; // normal . point = offset
  Matrix covariance{3, 3}; // azimuth, elevation, offset
  ReflectionLeg leg = ReflectionLeg::receive;
  uint64_t first_cpi_sequence = 0;
  uint64_t last_cpi_sequence = 0;
  uint64_t supporting_cpis = 0;
  uint64_t supporting_pairs = 0;
  uint64_t supporting_parent_tracks = 0;
  double bic = std::numeric_limits<double>::infinity();
  bool admitted = false;
};

Vec3 mirror_across_plane(Vec3 point, const ReflectorPlane& plane);

/** DL excess range/range-rate through one stationary specular reflection. */
std::array<double, 2> reflected_dl_model(const std::array<double, 6>& target,
                                         const BistaticGeometry& geometry,
                                         const ReflectorPlane& plane);

/** Convert a reflected observation into the equivalent virtual-anchor direct geometry. */
std::pair<BistaticGeometry, Detection> virtualize_reflected_detection(
    const BistaticGeometry& geometry, const Detection& detection,
    const ReflectorPlane& plane);

struct SoftEvidenceEvaluation {
  bool available = false;
  double log_bayes_factor = 0.0;
  Detection posterior_mode;
  uint64_t integrated_cells = 0;
};

/** Predictive marginal on an unthresholded detector map.
 *
 * The search support is the part of the Gaussian prediction whose density remains representable
 * in double precision.  The null scale and map multiplicity are measured by CLEAN in this CPI;
 * no scenario range/rate gate is accepted here.
 */
SoftEvidenceEvaluation evaluate_soft_evidence(
    const SoftRangeRateEvidence& surface,
    const std::array<double, 2>& predicted_range_rate,
    const Matrix& predicted_covariance);

struct RotatingPredictiveScore {
  bool available = false;
  uint32_t evaluated_folds = 0;
  double median_log_likelihood_ratio = 0.0;
  double bic = std::numeric_limits<double>::infinity();
};

/** Aggregate correlated leave-one-receiver-out folds without multiplying their likelihoods. */
RotatingPredictiveScore predictive_bic_from_correlated_folds(
    const std::vector<std::optional<double>>& folds,
    uint32_t fitted_parameter_count,
    uint64_t scalar_observation_count);

struct ReflectorPathMeasurement {
  uint64_t cpi_sequence = 0;
  double time_s = 0.0;
  uint64_t parent_track_id = 0;
  uint64_t path_track_id = 0;
  std::array<double, 6> parent_state{};
  std::array<double, 6> path_state{};
  uint32_t receiver_index = 0;
  BistaticGeometry geometry;
  Detection detection;
};

struct ReflectorPairDecision {
  bool evidence_ready = false;
  bool shared_reflector_preferred = false;
  uint64_t reflector_id = 0;
  double shared_reflector_bic = std::numeric_limits<double>::infinity();
  double independent_target_bic = std::numeric_limits<double>::infinity();
};

/** Causal one-bounce plane bank.
 *
 * A plane is first fitted only to a retained track-pair history spanning multiple CPIs and remains
 * provisional. It is admitted only after a second, distinct physical target parent is explained
 * better by their joint three-parameter plane than by two separately fitted planes under BIC.
 * This allows a physical wall to be reused without inventing one per candidate, a map, or GT.
 */
class SharedReflectorBank {
public:
  SharedReflectorBank();
  ~SharedReflectorBank();
  SharedReflectorBank(const SharedReflectorBank&) = delete;
  SharedReflectorBank& operator=(const SharedReflectorBank&) = delete;
  SharedReflectorBank(SharedReflectorBank&&) noexcept;
  SharedReflectorBank& operator=(SharedReflectorBank&&) noexcept;
  ReflectorPairDecision observe_pair(
      uint64_t cpi_sequence,
      uint64_t parent_track_id,
      uint64_t path_track_id,
      const std::array<double, 6>& parent_state,
      const std::array<double, 6>& path_state,
      const std::vector<ReflectorPathMeasurement>& measurements);

  ReflectorPairDecision classify_pair(uint64_t parent_track_id,
                                      uint64_t path_track_id) const;
  const std::vector<ReflectorPlane>& planes() const;
  void retain_tracks(const std::set<uint64_t>& active_track_ids);
  void reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nr_isac
