/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "small_matrix.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nr_isac {

/** One pre-update innovation whitened by its predicted measurement covariance. */
struct OriginInnovation {
  uint32_t receiver_index = 0;
  std::array<double, 2> whitened{}; // range, range-rate
};

/** Causal, target-blind comparison of measurement-origin hypotheses.
 *
 * The values are approximate log Bayes factors obtained from BIC differences.  They are exposed
 * independently because static motion, correlated innovations, and track duplication are related
 * clues and must not be multiplied as though they were independent measurements.
 */
struct OriginHypothesisEvidence {
  std::string decision = "clutter_or_new";
  bool ready = false;
  std::array<double, 3> log_evidence{}; // direct target, static/slow multipath, clutter/new
  double temporal_structure_log_bayes_factor = 0.0;
  double cross_receiver_structure_log_bayes_factor = 0.0;
  double stationary_log_bayes_factor = 0.0;
  double ul_direct_log_bayes_factor = 0.0;
  double exclusivity_log_bayes_factor = 0.0;
  uint64_t exclusivity_competitor_track_id = 0;
  uint64_t observed_epochs = 0;
  uint64_t cross_receiver_epochs = 0;
};

/** Bounded-memory sufficient statistics for the first, non-learned origin layer.
 *
 * No GT, class, map, reflector position, target count, or scenario-specific constant enters this
 * object.  The detector and kinematic filter remain unchanged; this layer is diagnostic until its
 * discrimination has been validated on independent channel/OTA data.
 */
class CausalOriginHypotheses {
public:
  CausalOriginHypotheses() = default;
  ~CausalOriginHypotheses();
  CausalOriginHypotheses(const CausalOriginHypotheses& other);
  CausalOriginHypotheses& operator=(const CausalOriginHypotheses& other);
  CausalOriginHypotheses(CausalOriginHypotheses&&) noexcept = default;
  CausalOriginHypotheses& operator=(CausalOriginHypotheses&&) noexcept = default;
  void observe_epoch(const std::vector<OriginInnovation>& innovations,
                     const std::array<double, 3>& velocity_mps,
                     const Matrix& velocity_covariance,
                     uint32_t cross_receiver_support);
  void apply_ul_direct_evidence(double log_bayes_factor);
  void apply_exclusivity_evidence(double log_bayes_factor,
                                  uint64_t competitor_track_id);
  OriginHypothesisEvidence evaluate(double direct_vs_clutter_log_evidence) const;

private:
  struct ScalarAutoregression;
  struct BivariateMoments;
  std::array<std::shared_ptr<ScalarAutoregression>, 2> temporal_;
  std::map<std::array<uint32_t, 3>, std::shared_ptr<BivariateMoments>> cross_receiver_;
  uint64_t observed_epochs_ = 0;
  uint64_t cross_receiver_epochs_ = 0;
  std::array<double, 3> velocity_mps_{};
  Matrix velocity_covariance_{3, 3};
  bool have_velocity_ = false;
  double ul_direct_log_bayes_factor_ = 0.0;
  bool have_ul_evidence_ = false;
  double exclusivity_log_bayes_factor_ = 0.0;
  uint64_t exclusivity_competitor_track_id_ = 0;
  bool have_exclusivity_evidence_ = false;
};

} // namespace nr_isac
