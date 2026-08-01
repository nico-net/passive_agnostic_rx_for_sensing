/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_ISAC/det_quality.h
 * \brief Adaptive per-detection quality: P(this detection is a real target), estimated online.
 *
 * ## What problem this solves
 *
 * A measured separability study over recorded captures (`tests/sensing_sim/analyze_separability.py`)
 * showed the single-receiver detection stream is far from information-limited: real detections and
 * ghosts — INCLUDING scheduling-induced Doppler harmonics — separate at ROC AUC 0.94 on SNR alone,
 * and temporal persistence adds information ORTHOGONAL to SNR (it separates within every SNR
 * quartile, AUC 0.64–0.86; the combination reaches 0.96).
 *
 * The obvious exploitation — "drop detections below N dB" — was rejected deliberately. An absolute
 * threshold does not survive a change of receiver gain, traffic pattern, CPI length or scene, because
 * the whole SNR population moves bodily: the two captures this was validated on settle at learned
 * null medians of 12.7 dB and 18.1 dB respectively. What actually carries the information is where a
 * detection sits in ITS OWN CPI's distribution (`snr_rank_in_cpi` AUC 0.93), which is scale-free.
 *
 * ## The model, and why nothing here is a tuned constant
 *
 * Per CPI, over that CPI's own detections:
 *
 *  1. **Robust null.** median and MAD of the detection SNR population, EMA-smoothed ACROSS CPIs so it
 *     tracks gain/traffic drift instead of one CPI's luck. Measured precision is below 50 %, so the
 *     bulk of detections ARE false alarms and the robust centre estimates that population without
 *     assuming its parameters.
 *  2. `z = (snr − median) / MAD` — scale-free, so `z ~ N(0,1)` under the null BY CONSTRUCTION.
 *  3. `P(z | false alarm) = N(0, 1)`.
 *  4. `P(z | real) = N(mu_r, 1)`, with `mu_r` estimated online as the EMA of the posterior-weighted
 *     mean `z`. Self-bootstrapping, floored so the two components cannot collapse onto each other.
 *  5. **Persistence**: how many of the last `PERSIST_WINDOW` CPIs held a detection in the same
 *     resolution cell. Class-conditional likelihoods are Laplace-smoothed counts updated online, so
 *     the weight given to persistence is learned rather than set.
 *  6. Naive-Bayes posterior; admit above the decision boundary.
 *
 * The boundary is `cost_ratio / (1 + cost_ratio)`, i.e. plain Bayes. `cost_ratio = 1` (the default)
 * is the symmetric-cost optimum — not a knob that was tuned to this data. An operator who wants to
 * trade precision for recall sets it in units of relative cost, never in dB.
 *
 * ## Measured (offline, on recorded captures; see GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md §7.12)
 *
 * Detection precision 32 % → 90 % and 55 % → 88 % on two captures, with **0/16 and 2/36 harmonics
 * surviving**. Through to fused single-receiver world tracks: precision 46 % → 59 % and 47 % → 66 %,
 * median world error 17.8 m → 7.9 m and 18.4 m → 6.1 m.
 *
 * Not free: recall of real detections falls to ~73–82 %, so per-target coverage drops. Raise
 * `cost_ratio` above 1 to keep more.
 */

#ifndef NR_ISAC_DET_QUALITY_H
#define NR_ISAC_DET_QUALITY_H

#include <cstdint>
#include <deque>
#include <vector>

#include "defs_nr_UE_ISAC.h"

namespace nr_isac {

/// How many previous CPIs contribute to the persistence feature.
constexpr uint32_t DQ_PERSIST_WINDOW = 4;
/// "Same resolution cell" tolerance for persistence, in BINS — deliberately not metres, so it carries
/// across bandwidths and CPI lengths without becoming a scene-specific constant.
constexpr int DQ_PERSIST_RANGE_BINS = 5;
constexpr int DQ_PERSIST_DOPP_BINS  = 4;

/**
 * @brief Online estimator of P(real target) for each detection in a CPI.
 *
 * Stateful across CPIs (that is the point — every distribution it uses is learned). One instance per
 * sensing engine; not thread-safe, and only ever touched from the engine thread.
 */
class det_quality
{
public:
  explicit det_quality(float cost_ratio = 1.0f);

  /**
   * @brief Score one CPI's detections, then fold them into the online estimates.
   *
   * @param dets        this CPI's detections (read-only; filtering is the caller's decision)
   * @param range_res_m  range resolution, to express each detection's own motion in bins
   * @param cpi_dt_s     time between CPIs (s). Together with a detection's own range-rate this says
   *                     where it WAS in previous CPIs, which is what makes the persistence feature
   *                     work for a moving target instead of penalising it.
   * @param p_real      out, one posterior per detection, same order
   */
  void score(const std::vector<sensing_detection_t>& dets, float range_res_m, double cpi_dt_s,
             std::vector<float>& p_real);

  /// Posterior above which a detection is admitted (Bayes boundary for the configured cost ratio).
  float boundary() const { return boundary_; }

  // --- learned state, exposed for the per-CPI diagnostic log and for tests ---------------------
  double null_median_db() const { return med_; }
  double null_mad_db() const { return mad_; }
  double separation_sigma() const { return mu_r_; }  ///< mu_r: real/false-alarm separation, in null sigmas
  double var_real() const { return var_r_; }
  double var_null() const { return var_0_; }
  double prior_real() const { return prior_; }
  bool   warmed_up() const { return have_null_; }

  /**
   * @brief P_D: the per-CPI probability that a target which EXISTS is detected, measured entirely
   *        receiver-side. Negative until enough CPIs have accumulated to estimate it.
   *
   * This is the `persistence_of()` feature read as what it physically is. A detection's persistence
   * is the number of the previous @ref DQ_PERSIST_WINDOW CPIs that held a compatible (motion-
   * compensated) detection, so `persistence / WINDOW` is a direct estimate of how often the object
   * generating it gets detected. Averaged over the CPI's detections, WEIGHTED BY `p_real` so that
   * false alarms -- which by construction do not persist -- cannot drag it down.
   *
   * **Why the central node needs this rather than estimating P_D itself**: the tracker's own estimate
   * is the association rate of the tracks it currently believes in, which is self-reinforcing. Ghost
   * tracks dominate that population and rarely associate, so the estimate collapses (measured: 0.23
   * against a true per-target rate of ~0.85), and a low P_D makes a miss nearly free in the existence
   * recursion, which is exactly what lets ghosts survive and keeps the estimate low. Measured from
   * DETECTIONS instead, the quantity is independent of any track belief and the loop has no input.
   *
   * **Known bias, stated rather than hidden**: persistence is only observable for detections that
   * exist, so this estimates P(detected in a neighbouring CPI | detected now) and is biased HIGH
   * relative to a true unconditional P_D -- a target missed in every CPI contributes nothing at all.
   * That is the right direction for the failure it addresses (the tracker's estimate is biased
   * catastrophically LOW), but it is not an unbiased estimator and should not be quoted as one.
   */
  double detection_rate() const { return p_detect_; }

private:
  uint32_t persistence_of(uint32_t range_bin, uint32_t dopp_bin, double drift_bins_per_cpi) const;

  float  boundary_;
  bool   have_null_ = false;
  double med_       = 0.0;
  double mad_       = 1.0;
  double mu_r_      = 1.5;   ///< initial separation guess; converges within a few CPIs
  /// Class variances, LEARNED. Fixing the real class at 1 was measured to be badly wrong (its true
  /// spread is 2.5–9.5): the model then reads a weak-but-genuine target as "too far below mu_r to be
  /// real" and discards it. var_r is additionally constrained >= var_0 — an identifiability
  /// constraint, not a tuned value: real targets vary in RCS, range and aspect, so their SNR spread
  /// is necessarily at least the noise class's, and without it the EM has a degenerate fixed point
  /// (narrow var_r -> only the strongest count as real -> their variance is small -> var_r stays
  /// narrow), measured collapsing to 0.26 against a true 2.51.
  double var_r_     = 1.0;
  double var_0_     = 1.0;
  double prior_     = 0.35;  ///< P(real), tracked online
  double p_detect_  = -1.0;  ///< P_D from persistence; negative until estimable (see detection_rate)

  /// Laplace-smoothed class-conditional persistence counts, indexed by persistence 0..WINDOW.
  double pc_real_[DQ_PERSIST_WINDOW + 1];
  double pc_fa_[DQ_PERSIST_WINDOW + 1];

  struct cell_t {
    uint32_t r, d;
  };
  std::deque<std::vector<cell_t>> recent_; ///< occupied cells of the last WINDOW CPIs
};

} // namespace nr_isac

#endif // NR_ISAC_DET_QUALITY_H
