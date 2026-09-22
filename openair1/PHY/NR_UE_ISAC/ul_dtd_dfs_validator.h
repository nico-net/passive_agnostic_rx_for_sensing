/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"
#include "multipath_fusion.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace nr_isac {

/** Receiver-local PUSCH measurements after direct-path delay/frequency referencing.
 *
 * Each Detection is expressed as
 *   [c(tau_reflected-tau_direct), -(c/fc)(f_reflected-f_direct)].
 * Direct and reflected estimates must originate from the same PUSCH resources.  Cable delay,
 * receiver timing offset, and common CFO are therefore nuisance terms, not geometry inputs.
 */
struct UlDifferentialReceiverBatch {
  // Protocol identity of the PUSCH transmitter (normally C-RNTI plus cell/session epoch).
  // Zero means that the transmitter identity was not available and must never be fused with a
  // different CPI or used to establish the shared UE nuisance state.
  uint64_t session_id = 0;
  // Fingerprint of measured PUSCH allocation/time support shared by participating RXs.
  // Zero is legacy/unverified provenance and is neutral in unified mode.
  uint64_t allocation_support_id = 0;
  uint32_t receiver_index = 0;
  Vec3 receiver_position;
  std::vector<Detection> detections;
  double range_resolution_m = 0.0;
  double rate_resolution_mps = 0.0;
  double maximum_differential_range_m = 0.0;
  double maximum_abs_differential_rate_mps = 0.0;
  double target_time_offset_s = 0.0;
  // Receiver-local direct-path range-rate from the same PUSCH. A common UE oscillator offset is
  // estimated as a nuisance across receivers, so only FDOA structure is used; fixed cable delays
  // and absolute receiver time never enter this measurement.
  bool direct_path_rate_valid = false;
  double direct_path_range_rate_mps = 0.0;
  double direct_path_rate_variance_mps2 = std::numeric_limits<double>::infinity();
  // Optional serving-cell range obtained from genuine OAI TA telemetry. Passive FFT placement is
  // not TA and must leave this false. The same per-session value may be copied into each RX batch;
  // the validator checks that copies agree before using it.
  bool serving_range_valid = false;
  Vec3 serving_transmitter_position;
  double serving_range_m = 0.0;
  double serving_range_variance_m2 = std::numeric_limits<double>::infinity();
  bool same_pusch_reference = false;
  bool observable = false;
  // False censors the remaining search; it does not invalidate already accepted peaks.
  // No incompatible/missing peak from such a batch may count as evidence of absence.
  bool search_complete = false;
};

enum class UlDifferentialDecision {
  unavailable,
  support,
  contradiction,
};

const char* ul_differential_decision_name(UlDifferentialDecision decision);

struct UlDtdDfsValidation {
  UlDifferentialDecision decision = UlDifferentialDecision::unavailable;
  double log_bayes_factor = 0.0;
  uint32_t observable_receivers = 0;
  uint32_t evaluated_folds = 0;
  uint32_t supporting_folds = 0;
  uint64_t training_hypotheses = 0;
  uint64_t fitted_ue_states = 0;
  bool cross_fold_ue_consistent = false;
  bool ue_state_valid = false;
  bool ue_velocity_identifiable = false;
  // True only when genuine protocol direct-path telemetry (currently OAI TA) independently
  // anchors the session. The tracker may alternatively set establishment eligibility after two
  // distinct confirmed DL tracks support the same per-RNTI provisional state.
  bool direct_session_anchor_valid = false;
  bool session_establishment_eligible = false;
  bool processing_deadline_exhausted = false;
  double ue_state_time_s = 0.0;
  std::array<double, 6> ue_state{};
  Matrix ue_covariance{6, 6};
  std::string reason = "insufficient_independent_ul_evidence";
};

/** Truth-blind rotating three-fit/one-held-out DTD+DFS validator.
 *
 * A fold fits the unknown six-state UE nuisance from three receivers, then evaluates the fourth
 * receiver under a Gaussian target return versus the measured uniform false-object density over
 * the declared detector support.  All four receiver rotations are evaluated.  No UE coordinates,
 * target class/count, scenario label, fixed residual gate, or calibration table is accepted.
 */
class UlDtdDfsValidator {
public:
  UlDtdDfsValidation evaluate(
      double time_s,
      const std::array<double, 6>& target_state,
      const Matrix& target_covariance,
      const std::vector<UlDifferentialReceiverBatch>& batches,
      double processing_budget_s = std::numeric_limits<double>::infinity()) const;

  /** Score the same measurements/one shared causal UE under an admitted one-bounce plane.
   * Missing UL or a not-yet-established UE remains unavailable and therefore neutral.
   */
  UlDtdDfsValidation evaluate_reflected(
      double time_s,
      const std::array<double, 6>& target_state,
      const Matrix& target_covariance,
      const std::vector<UlDifferentialReceiverBatch>& batches,
      const ReflectorPlane& plane,
      double processing_budget_s = std::numeric_limits<double>::infinity()) const;

  void commit(double time_s, const UlDtdDfsValidation& validation,
              double establishment_log_threshold = 0.0);
  // Called once per session/CPI on a preselected confirmed DL anchor. It never commits a
  // candidate-specific UE; the caller commits at most this one shared-state proposal.
  // Initial bootstrap requires rotating four-RX spatial holdout. Later three-RX time holdouts
  // may validate/propagate the existing state without shrinking its covariance or refitting it.
  UlDtdDfsValidation propose_shared_update(
      double time_s, const std::array<double, 6>& target, const Matrix& covariance,
      const std::vector<UlDifferentialReceiverBatch>& batches) const;
  void reset();
  bool has_causal_ue_state() const
  { return ue_state_.has_value() && ue_state_->established; }
  bool has_provisional_ue_state() const
  { return ue_state_.has_value() && !ue_state_->established; }
  bool has_direct_session_anchor() const
  { return ue_state_.has_value() && ue_state_->established
           && ue_state_->direct_session_anchor_valid; }

private:
  struct CausalUeState {
    double time_s = 0.0;
    std::array<double, 6> state{};
    Matrix covariance{6, 6};
    bool dfs_identifiable = false;
    bool established = false;
    bool direct_session_anchor_valid = false;
    uint32_t independent_time_confirmations = 0;
    double cumulative_log_evidence = 0.0;
  };
  std::optional<CausalUeState> ue_state_;
};

std::array<double, 2> ul_dtd_dfs_model(
    const std::array<double, 6>& target_state,
    const std::array<double, 6>& ue_state,
    Vec3 receiver_position);

std::array<double, 2> ul_dtd_dfs_reflected_model(
    const std::array<double, 6>& target_state,
    const std::array<double, 6>& ue_state,
    Vec3 receiver_position,
    const ReflectorPlane& plane);

} // namespace nr_isac
