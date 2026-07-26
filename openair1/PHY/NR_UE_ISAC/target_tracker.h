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

/*! \file openair1/PHY/NR_UE_ISAC/target_tracker.h
 * \brief Per-CPI target track: 2-state constant-velocity Kalman filter over the detection stream.
 *
 * WHAT THIS IS FOR. range_doppler emits an independent set of CFAR detections every CPI, each
 * quantized to an integer range bin. Consumed raw, that stream has two weaknesses: the reported
 * range can only take multiples of range_res (a visible staircase, ~0.5 bin of quantization error),
 * and there is no association across CPIs — nothing links "the detection at 122 m this CPI" to "the
 * one at 125 m last CPI", so a single false alarm outranking the target flips the reported position.
 * This class adds the missing temporal layer: it predicts where the target should be, associates the
 * nearest gated detection, and maintains a continuous (sub-bin) range/range-rate estimate that
 * coasts through CPIs where the target is missed entirely.
 *
 * MODEL. State x = [r, rdot]^T: differential bistatic range (m) and its rate (m/s) -- the same two
 * quantities range_doppler's axes report, so no unit conversion is needed anywhere. Constant-velocity
 * dynamics with continuous white-noise acceleration:
 *
 *     predict:  x⁻ = F·x,            F = [[1, dt], [0, 1]]
 *               P⁻ = F·P·Fᵀ + Q,     Q = q·[[dt³/3, dt²/2], [dt²/2, dt]]
 *     update:   y  = z − H·x⁻,       H = [1, 0]          (innovation)
 *               S  = H·P⁻·Hᵀ + R                          (innovation covariance)
 *               K  = P⁻·Hᵀ·S⁻¹                            (gain)
 *               x  = x⁻ + K·y
 *               P  = (I − K·H)·P⁻
 *
 * Written out as scalars (2x2 symmetric P kept as p00/p01/p11) rather than with a matrix library --
 * same dependency-free style as isac_fft/eca_clutter, and a 2-state filter does not justify one.
 *
 * ASSOCIATION + GATING. Of all detections in a CPI, the one closest to the prediction is used, and
 * only if it passes the normalized-innovation gate y²/S <= gate_sq (3σ by default). Everything else
 * is ignored. When nothing passes -- target genuinely missed, or every detection is a false alarm --
 * the filter COASTS: the prediction becomes the estimate and P keeps growing, which naturally widens
 * the gate so the track can re-acquire. After `max_coast` consecutive coasts the track is dropped and
 * the next accepted detection re-initialises it, so a lost target does not leave a stale ghost track
 * wandering forever.
 *
 * WHY KALMAN AND NOT ALPHA-BETA. An alpha-beta filter is the same predictor with two *fixed* gains.
 * That is a poor fit here because dt is genuinely irregular (CPIs close whenever enough reference
 * occurrences have accumulated, which depends on traffic) and because the confidence in the track
 * must change over time -- tight after a run of good detections, deliberately loose after coasting.
 * The Kalman gain adapts to both for free via P; fixed gains would have to be tuned for one regime
 * and would be wrong in the other. The cost is a 2x2 covariance update, which is a handful of flops
 * once per CPI.
 *
 * PARAMETER PROVENANCE (measured on tests/sensing_sim at 100 MHz / 273 PRB, 2026-07-23 -- see the
 * defaults in defs_nr_UE_ISAC.h): R from the observed detrended residual variance (2.6 m², versus
 * 0.78 m² for pure bin quantization Δ²/12 -- the excess is real detection noise, so the larger,
 * measured value is used); q from the scenario's actual range-rate drift (~0.05 m/s² over the run).
 * Both are config-exposed because both are geometry/scenario dependent; `track_nis` logs the
 * normalized innovation squared so the pair can be re-tuned by consistency (a well-matched filter
 * has mean NIS ≈ 1 for this 1-D measurement) rather than by eye.
 *
 * RT-safety: engine thread only, called from sensing_engine::process_cpi() after range_doppler has
 * produced this CPI's detections. Fixed-size state, no allocation.
 */

#ifndef NR_ISAC_TARGET_TRACKER_H
#define NR_ISAC_TARGET_TRACKER_H

#include <cstdint>
#include <vector>

#include "defs_nr_UE_ISAC.h"

namespace nr_isac {

/// One CPI's track state, emitted alongside the raw detections.
struct sensing_track_t {
  uint32_t track_id    = 0;     ///< stable per-track identity (0 for the legacy single-track path;
                                ///< assigned by multi_target_tracker for MOT)
  bool   active        = false; ///< a track exists (initialised and not dropped)
  bool   updated       = false; ///< a detection passed the gate this CPI (false => coasted)
  float  range_m       = 0.0f;  ///< filtered differential bistatic range, CONTINUOUS (sub-bin)
  float  range_rate_mps = 0.0f; ///< filtered range rate
  float  innovation_m  = 0.0f;  ///< z − prediction for the accepted detection (0 when coasting)
  float  nis           = 0.0f;  ///< normalized innovation squared, y²/S (0 when coasting); mean ≈ 1
                                ///< when R/q are consistent with reality -- the tuning diagnostic
  float  q_mult        = 1.0f;  ///< current NIS-driven inflation of q (1 = baseline, no maneuver
                                ///< detected); >1 means the CV model is being out-run and the filter
                                ///< has loosened itself. Logged so a maneuver is visible.
  float  nis_ewma      = 0.0f;  ///< smoothed NIS driving q_mult
  float  sigma_range_m = 0.0f;  ///< sqrt(P00), the filter's own 1σ range uncertainty
  uint32_t coast_count = 0;     ///< consecutive CPIs without an accepted detection
};

/**
 * @brief 2-state constant-velocity Kalman tracker over the per-CPI detection stream.
 * See the file comment for the model, gating/coasting policy, and parameter provenance.
 */
class target_tracker
{
public:
  explicit target_tracker(const nr_isac_args_t& args_) : args(args_), ca_(args_.track_model == "ca") {}

  /**
   * @brief Advances the track by one CPI.
   * @param detections this CPI's CFAR detections (may be empty)
   * @param dt_s       elapsed time since the previous CPI, seconds (<=0 => treated as a re-init)
   * @return the resulting track state (also retrievable via last())
   */
  const sensing_track_t& update(const std::vector<sensing_detection_t>& detections, double dt_s);

  const sensing_track_t& last() const { return out_; }
  void                   reset() { init_ = false; out_ = sensing_track_t(); }

private:
  void init_from(const sensing_detection_t& d);

  nr_isac_args_t   args;
  sensing_track_t  out_;

  bool   init_ = false;
  bool   ca_   = false;     ///< constant-ACCELERATION model (3-state) instead of constant-velocity;
                            ///< set from args.track_model=="ca" at construction. Adds x_a_ + the p*2_
                            ///< covariance terms; the CV path leaves them zero and is unchanged.
  double nis_ewma_ = 1.0;   ///< EWMA of NIS; drives the adaptive q (see defs_nr_UE_ISAC.h)
  uint32_t gone_count_ = 0; ///< consecutive TRUE misses (coast with no near-miss); only this drops the
                            ///< track, so a manoeuvre (which always leaves a near-miss) never does
  double x_r_  = 0.0, x_v_ = 0.0, x_a_ = 0.0;  ///< state [r, rdot, rddot] (rddot used only when ca_)
  double p00_ = 0.0, p01_ = 0.0, p11_ = 0.0;   ///< symmetric covariance, 2x2 (CV) core ...
  double p02_ = 0.0, p12_ = 0.0, p22_ = 0.0;   ///< ... extended to 3x3 when ca_
};

} // namespace nr_isac

#endif // NR_ISAC_TARGET_TRACKER_H
