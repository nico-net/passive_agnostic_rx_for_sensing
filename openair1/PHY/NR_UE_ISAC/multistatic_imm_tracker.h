/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "enu_tracker.h"
#include "motion_tracker.h"
#include "multipath_fusion.h"
#include "origin_hypothesis.h"
#include "ul_dtd_dfs_validator.h"
#include "existence_evidence.h"
#include "birth_map_evidence.h"

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace nr_isac {

/** One receiver's independent range/range-rate measurements for a common CPI.
 *
 * A spatially separated X410 channel is a receiver, not an element of a coherent AoA array.  Its
 * direct-path clock correction and detector run independently before this structure is formed.
 */
struct ReceiverDetectionBatch {
  uint32_t receiver_index = 0;
  BistaticGeometry geometry;
  std::vector<Detection> detections;
  double range_resolution_m = 0.0;
  double rate_resolution_mps = 0.0;
  // Unthresholded first-pass evidence for proposal-driven fuse-then-detect. Shared ownership keeps
  // the previous CPI available for causal 3+1 validation without another map copy.
  std::shared_ptr<const SoftRangeRateEvidence> soft_evidence;
  // Acquisition epoch relative to update() time; never report completion time.
  double time_offset_s = 0.0;
  double occupied_bandwidth_fraction = 0.0;
  double valid_re_fraction = 0.0;
  double measured_dwell_s = 0.0;
  bool reference_valid = false;
};

struct MultistaticImmTrackerConfig {
  // 0: frozen baseline; 1..6: B..G preregistered ablations. Experimental until gates pass.
  uint32_t evidence_mode = 0;
  uint32_t lifecycle_features = 0; // experimental bitmask: birth=1, grouping=2, retirement=4
  double evidence_window_s = 0.150;
  double existence_threshold_scale = 1.0;
  uint32_t maximum_local_map_cells = 4096;
  // The sole statistical operating point is inherited from the detector declaration. Association
  // gates are recomputed per CPI from this rate, elapsed time, and the actual hypothesis count.
  double false_object_intensity_per_s = 0.0;
  double maximum_propagation_s = 2.0;

  // A birth needs independent geometry; subsequent epochs may contain any receiver subset.
  // Legacy mode 0 uses causal two-CPI tracklets. Canonical evidence modes use
  // three distinct receiver seeds, optionally asynchronous, followed by NEW
  // acquisition evidence for confirmation; seed measurements are not rescored.
  uint32_t birth_minimum_receivers = 3;
  // Infinity (default) means admit every numerically full-rank geometry and carry its measured
  // uncertainty. Deployments may impose an explicit conditioning policy without changing code.
  double birth_maximum_jacobian_condition = std::numeric_limits<double>::infinity();
  uint32_t birth_maximum_iterations = 40;

  // Three target-blind kinematic regimes: smooth CV, CA, and horizontal coordinated turn.
  std::array<double, 3> initial_model_probability{0.60, 0.30, 0.10};
  std::array<std::array<double, 3>, 3> model_transition{{
      {{0.94, 0.05, 0.01}}, {{0.06, 0.89, 0.05}}, {{0.03, 0.07, 0.90}}}};
  double cv_acceleration_psd = 0.8;
  double ca_jerk_psd = 3.0;
  double turn_jerk_psd = 10.0;
  double cv_acceleration_decay_s = 0.20;
  // Required application surveillance bound; zero deliberately fails validation.
  double maximum_target_speed_mps = 0.0;
  double maximum_range_m = 0.0;

  // EXPERIMENTAL, REVERTIBLE: when true, a birth candidate's discrete elevation-ambiguity roots
  // (see fit_asynchronous's two-root algebraic ambiguity) are additionally scored against the raw
  // UL differential observation using this DECLARED, assumed-known UE position -- a genuinely
  // independent illuminator geometry from the gNB, so a root that is degenerate under the DL
  // bistatic geometry generically is not also degenerate under the UE-illuminated one. This does
  // NOT go through the existing anchor-gated UlDtdDfsValidator (which structurally cannot help the
  // very first birth -- see propose_shared_update). Default false leaves every existing code path
  // byte-for-byte unchanged; this is an explicit opt-in, not a silent behavior change. A real
  // deployment would source this from genuine OAI Timing Advance telemetry (gNB<->UE range; see
  // serving_range_m) plus an independent angle, or a receiver-side fit of the UE's own direct-path
  // transmission -- neither is implemented here. This flag exists to test the disambiguation
  // architecture itself against a position assumed known, not to claim OTA extraction is solved.
  bool ue_position_known = false;
  std::array<double, 3> ue_position{0.0, 0.0, 0.0};
};

struct MultistaticTrackerProcessingStats {
  uint64_t local_map_cells = 0;
  uint64_t asynchronous_proposals = 0;
  uint64_t soft_supported_epochs = 0;
  uint64_t visibility_negative_epochs = 0;
  uint64_t evidence_retirements = 0;
  double birth_elapsed_s = 0.0;
  double ul_elapsed_s = 0.0;
  double processing_budget_s = 0.0;
  double processing_elapsed_s = 0.0;
  uint64_t association_hypotheses = 0;
  uint64_t tracklet_hypotheses = 0;
  uint64_t birth_tuple_hypotheses = 0;
  uint64_t birth_queue_pops = 0;
  uint64_t birth_fit_attempts = 0;
  // OFFLINE DIAGNOSTIC: birth candidates rejected because an available, independent (not part of
  // the fitting triple) receiver's own seed inside the evidence window contradicted the fitted
  // state -- see the cross-check in the birth loop in multistatic_evidence.inc.
  uint64_t birth_cross_check_rejections = 0;
  // OFFLINE DIAGNOSTIC: how many birth_fit_attempts actually produced a numerically valid fit
  // (fit.valid==true in fit_asynchronous), before the cross-check or existing-track conflict test.
  uint64_t birth_fit_valid = 0;
  // OFFLINE DIAGNOSTIC: birth candidates rejected because the fitted position was more than 1 m
  // below the receiver's own ground-level datum -- physically impossible for any declared class.
  uint64_t birth_ground_floor_rejections = 0;
  // OFFLINE DIAGNOSTIC: how many birth fits had 2+ distinct valid trilateration roots and so had
  // their reported covariance widened to honestly reflect the multi-root spread (see
  // fit_asynchronous's moment-matching in multistatic_evidence.inc).
  uint64_t birth_multimodal_covariance_widenings = 0;
  // OFFLINE DIAGNOSTIC: how many birth candidates were refit using all 4 receivers (the original
  // 3-receiver triple plus an independent 4th seed in the same evidence window) instead of being
  // left as an exactly-determined, elevation-ill-conditioned 3-receiver solution -- see the
  // over-determined refit in the birth loop in multistatic_evidence.inc.
  uint64_t birth_four_receiver_refits = 0;
  // EXPERIMENTAL/OFFLINE DIAGNOSTIC: how many birth candidates had their discrete elevation root
  // chosen using the assumed-known UE position's independent illuminator geometry (config_
  // .ue_position_known) instead of, or in addition to, the same-illuminator DL 4th receiver.
  uint64_t birth_ue_geometry_disambiguations = 0;
  // EXPERIMENTAL/OFFLINE DIAGNOSTIC: continuous per-CPI EKF corrections applied from UL differential
  // returns using the assumed-known UE position as a second illuminator (config_.ue_position_known).
  uint64_t ul_track_corrections = 0;
  // EXPERIMENTAL/OFFLINE DIAGNOSTIC: per-CPI track scorings against the UL returns under the
  // assumed-known UE illuminator, used as existence evidence when the anchor-gated validator
  // yields nothing. Negative scores retire tracks the UL returns cannot explain.
  uint64_t ul_ue_geometry_validations = 0;
  // OFFLINE DIAGNOSTIC: seed counts per receiver index in the birth history, this CPI.
  std::array<uint64_t, 4> birth_seeds_by_receiver{0, 0, 0, 0};
  uint64_t admitted_birth_solutions = 0;
  uint64_t active_internal_tracks = 0;
  uint64_t reportable_tracks = 0;
  uint64_t emission_suppressed_tracks = 0;
  uint64_t soft_map_queries = 0;
  uint64_t conditioned_soft_queries = 0;
  uint64_t soft_explanation_reductions = 0;
  uint64_t duplicate_consolidations = 0;
  uint64_t owned_seed_exclusions = 0;
  uint64_t object_discrepancy_updates = 0;
  uint64_t birth_families_created = 0;
  uint64_t birth_branches_created = 0;
  uint64_t birth_branches_tested = 0;
  uint64_t birth_families_promoted = 0;
  uint64_t birth_families_expired = 0;
  uint64_t pending_birth_families = 0;
  uint64_t object_path_models_tested = 0;
  uint64_t grouped_path_epochs = 0;
  uint64_t explanation_negative_epochs = 0;
  double lifecycle_elapsed_s = 0.0;
  uint64_t predictive_folds = 0;
  uint64_t soft_birth_rejections = 0;
  uint64_t reflected_birth_rejections = 0;
  uint64_t reflected_path_assignments = 0;
  uint64_t reflector_pair_updates = 0;
  uint64_t admitted_reflector_planes = 0;
  // OFFLINE DIAGNOSTIC: the best-supported provisional (not-yet-admitted) reflector plane this CPI,
  // if any was fitted. Admission requires supporting_parent_tracks>=2 (see multipath_fusion.cc), which
  // is structurally unreachable in a single-real-target scenario -- these fields let an offline
  // investigation see whether a physically plausible plane was still fitted from the available
  // track-pair history, even though it could never be admitted/acted upon here.
  bool best_provisional_plane_valid = false;
  double best_provisional_plane_bic = 0.0;
  double best_provisional_plane_offset_m = 0.0;
  std::array<double, 3> best_provisional_plane_normal{0.0, 0.0, 0.0};
  uint64_t best_provisional_plane_supporting_pairs = 0;
  uint64_t best_provisional_plane_supporting_parent_tracks = 0;
  // OFFLINE DIAGNOSTIC: micro-Doppler sideband folding in update_evidence()'s observation
  // construction (the live path -- see multistatic_evidence.inc). "candidates" counts detections
  // tagged as a probable sideband of a stronger co-range component (tag_micro_doppler_families(),
  // clean_detector.cc); "suppressed" counts how many were actually folded into their family's
  // least-Doppler-extreme member before ever becoming an independent observation.
  uint64_t micro_doppler_sideband_candidates = 0;
  uint64_t micro_doppler_sideband_suppressed = 0;
  uint64_t ul_validation_attempts = 0;
  uint64_t ul_supported_birth_solutions = 0;
  uint64_t ul_contradicted_birth_solutions = 0;
  uint64_t ul_unavailable_birth_solutions = 0;
  uint64_t ul_track_validation_attempts = 0;
  uint64_t ul_supported_track_updates = 0;
  uint64_t ul_contradicted_track_updates = 0;
  uint64_t ul_unavailable_track_updates = 0;
  uint64_t ul_ue_bootstrap_attempts = 0;
  uint64_t ul_ue_bootstrap_promotions = 0;
  uint64_t ul_ue_crossvalidated_proposals = 0;
  uint64_t ul_ue_causal_confirmations = 0;
  uint64_t ul_ue_inconsistent_proposals = 0;
  uint64_t ul_ue_bootstrap_unavailable = 0;
  uint64_t ul_ue_multiplicity_rejections = 0;
  uint64_t ul_unknown_session_batches = 0;
  uint64_t ul_session_count = 0;
  uint64_t ul_established_session_count = 0;
  uint64_t ul_training_hypotheses = 0;
  uint64_t ul_fitted_ue_states = 0;
  uint64_t ul_processing_deferred = 0;
  bool birth_search_deadline_exhausted = false;
  bool processing_deadline_exhausted = false;
};

/** Multi-receiver 3-D IMM-EKF with receiver-subset updates.
 *
 * It deliberately contains no class labels, truth coordinates, expected target counts, or
 * scenario-specific priors.  New tracks are admitted by measured multilateration residual and
 * Jacobian condition only.  A false negative at one receiver therefore reduces information but is
 * not converted into a global miss.
 */
class MultistaticImmTracker {
public:
  explicit MultistaticImmTracker(std::vector<BistaticGeometry> geometries,
                                 MultistaticImmTrackerConfig config = {});
  ~MultistaticImmTracker();
  void reset();
  void update(double air_time_s, const std::vector<ReceiverDetectionBatch>& batches,
              uint64_t cpi_sequence,
              double processing_budget_s = SPATIAL_CPI_DURATION_S,
              const std::vector<UlDifferentialReceiverBatch>& ul_batches = {});

  std::vector<TrackSnapshot> snapshots() const;
  std::vector<TrackSnapshot> reportable_snapshots() const;
  const MultistaticTrackerProcessingStats& last_processing_stats() const
  {
    return last_processing_stats_;
  }
  TrackSnapshot planning_snapshot(double air_time_s, uint32_t receiver_index = 0) const;
  std::vector<ConfirmedTrackView> confirmed_tracks(uint32_t receiver_index) const;
  struct Track;

private:
  void update_evidence(double time, const std::vector<ReceiverDetectionBatch>& batches,
                       uint64_t sequence, double processing_budget_s,
                       const std::vector<UlDifferentialReceiverBatch>& ul_batches);
  struct EvidenceHistory;
  std::unique_ptr<EvidenceHistory> evidence_history_;
  struct BirthSolution;
  struct BirthHistory;
  struct UeBootstrapProposal {
    double time_s = 0.0;
    uint64_t target_track_id = 0;
    UlDtdDfsValidation validation;
  };
  struct UeDirectRateSample {
    double time_s = 0.0;
    double range_rate_mps = 0.0;
    double variance_mps2 = std::numeric_limits<double>::infinity();
  };
  std::vector<BistaticGeometry> geometries_;
  MultistaticImmTrackerConfig config_;
  std::vector<Track> tracks_;
  std::unique_ptr<BirthHistory> birth_history_;
  // One causal nuisance state per protocol session. A target hypothesis never owns or shares a
  // private convenient UE state.
  std::map<uint64_t, UlDtdDfsValidator> ul_validators_;
  std::map<uint64_t, double> ul_session_last_seen_s_;
  // One proposal per measured session/CPI, selected with an explicit multiplicity charge. The
  // bounded history records which confirmed target identities supported receiver-held-out and
  // later-time-held-out validation; an individual candidate never owns the UE state.
  std::map<uint64_t, std::deque<UeBootstrapProposal>> ul_bootstrap_history_;
  // DL identities that independently supported the current provisional session state. They can
  // establish a no-TA nuisance estimate, but negative target evidence additionally needs genuine
  // TA or a strict majority among at least three independent UE sessions.
  std::map<uint64_t, std::set<uint64_t>> ul_bootstrap_target_ids_;
  // Persistent same-PUSCH direct-path measurements are session data, not target data. A causal
  // robust local trend per RX removes isolated CFO/reference failures before those measurements
  // validate a UE proposal. Histories are bounded by the same externally configured propagation
  // horizon as track evidence and are never filled across missing CPIs.
  std::map<uint64_t,
           std::map<uint32_t, std::deque<UeDirectRateSample>>>
      ul_direct_rate_history_;
  // Number of causal CPIs in which two tracks simultaneously had independent three-receiver
  // support. This is sufficient for a BIC shared-motion exclusivity comparison and stores no
  // measurements or scene labels.
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> exclusivity_overlap_epochs_;
  SharedReflectorBank reflector_bank_;
  uint64_t next_id_ = 1;
  double time_s_ = 0.0;
  bool have_time_ = false;
  MultistaticTrackerProcessingStats last_processing_stats_;
};

} // namespace nr_isac
