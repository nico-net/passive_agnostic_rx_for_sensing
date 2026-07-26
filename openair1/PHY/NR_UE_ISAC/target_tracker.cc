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

#include "target_tracker.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include "common/utils/LOG/log.h"
}

namespace nr_isac {

void target_tracker::init_from(const sensing_detection_t& d)
{
  x_r_ = (double)d.range_m;
  // Seed the rate from the detection's own Doppler: the velocity axis reports bistatic range-rate
  // (see rvm.vel_res_mps), i.e. exactly this state's second component, so it can be used directly.
  x_v_ = (double)d.vel_mps;
  // Initial covariance: range known to about one measurement sigma; rate known far less well (the
  // Doppler bin is coarse and, on a short first observation, may be aliased), so start it loose and
  // let the filter tighten it.
  x_a_  = 0.0; // no acceleration evidence yet (CA model only)
  p00_  = (double)args.track_r_var_m2;
  p01_  = 0.0;
  p11_  = (double)(args.track_init_vel_var_m2s2);
  // CA extension: acceleration unknown at birth, so its variance starts large and its cross-terms
  // zero; the filter tightens them from the innovations. Unused (kept zero) in the CV model.
  p02_  = 0.0;
  p12_  = 0.0;
  p22_  = ca_ ? (double)args.track_init_acc_var : 0.0;
  nis_ewma_   = 1.0; // fresh track: no manoeuvre evidence yet
  gone_count_ = 0;
  init_ = true;
}

const sensing_track_t& target_tracker::update(const std::vector<sensing_detection_t>& detections, double dt_s)
{
  out_.updated      = false;
  out_.innovation_m = 0.0f;
  out_.nis          = 0.0f;

  if (!init_) {
    if (detections.empty()) {
      out_ = sensing_track_t(); // still no track
      return out_;
    }
    // Initialise on the strongest detection: with no prediction to gate against, SNR is the only
    // discriminator available.
    const sensing_detection_t* best = &detections[0];
    for (const auto& d : detections) {
      if (d.snr_db > best->snr_db) {
        best = &d;
      }
    }
    init_from(*best);
    out_.active         = true;
    out_.updated        = true;
    out_.coast_count    = 0;
    out_.range_m        = (float)x_r_;
    out_.range_rate_mps = (float)x_v_;
    out_.sigma_range_m  = (float)std::sqrt(std::max(p00_, 0.0));
    return out_;
  }

  // ---- Predict ----
  const double dt = (dt_s > 0.0) ? dt_s : 0.0;

  // Adaptive process-noise scale (same policy for both models -- see defs_nr_UE_ISAC.h): q is
  // multiplied by the smoothed NIS. While the model explains the innovations, NIS ~ 1 and this is a
  // no-op; under an unmodelled manoeuvre the innovations grow, NIS_ewma rises, q inflates, P⁻ enlarges
  // -- which both speeds re-convergence and widens the association gate -- then decays back on its own.
  double q = (double)args.track_q_accel;
  if (args.track_q_adapt_enable) {
    const double mult = std::min((double)args.track_q_mult_max, std::max(1.0, nis_ewma_));
    q *= mult;
    out_.q_mult = (float)mult;
  } else {
    out_.q_mult = 1.0f;
  }
  out_.nis_ewma = (float)nis_ewma_;
  const double dt2 = dt * dt, dt3 = dt2 * dt;

  if (!ca_) {
    // Constant-velocity: x⁻ = F·x, F = [[1,dt],[0,1]]; P⁻ = F P Fᵀ + Q, Q = q·white-noise-accel.
    x_r_ += x_v_ * dt;
    const double p00 = p00_ + dt * (2.0 * p01_ + dt * p11_);
    const double p01 = p01_ + dt * p11_;
    const double p11 = p11_;
    p00_ = p00 + q * dt3 / 3.0;
    p01_ = p01 + q * dt2 / 2.0;
    p11_ = p11 + q * dt;
  } else {
    // Constant-ACCELERATION: state [r, rdot, rddot], F = [[1,dt,dt²/2],[0,1,dt],[0,0,1]].
    // Acceleration is carried in the state and PREDICTED forward, so continuous curvature (a circular
    // path's centripetal term) is anticipated rather than only chased after the fact as in CV. Q is
    // the white-noise-JERK model (q now a jerk PSD): the residual unmodelled term is the RATE OF
    // CHANGE of acceleration, which for smooth motion is small -- that is why CA fits a turn tightly.
    const double dt4 = dt3 * dt, dt5 = dt4 * dt;
    x_r_ += x_v_ * dt + 0.5 * x_a_ * dt2;
    x_v_ += x_a_ * dt;
    // P⁻ = F P Fᵀ, symmetric 3x3 (build via the explicit expansion of F P Fᵀ).
    const double n00 = p00_ + 2.0 * dt * p01_ + dt2 * p11_ + dt2 * p02_ + dt3 * p12_ + 0.25 * dt4 * p22_;
    const double n01 = p01_ + dt * p11_ + dt * p02_ + 1.5 * dt2 * p12_ + 0.5 * dt3 * p22_;
    const double n02 = p02_ + dt * p12_ + 0.5 * dt2 * p22_;
    const double n11 = p11_ + 2.0 * dt * p12_ + dt2 * p22_;
    const double n12 = p12_ + dt * p22_;
    const double n22 = p22_;
    // + Q (white-noise jerk, spectral density q).
    p00_ = n00 + q * dt5 / 20.0;
    p01_ = n01 + q * dt4 / 8.0;
    p02_ = n02 + q * dt3 / 6.0;
    p11_ = n11 + q * dt3 / 3.0;
    p12_ = n12 + q * dt2 / 2.0;
    p22_ = n22 + q * dt;
  }

  // ---- Associate + update with a 2-D measurement z = [range, range-rate] ----
  // BOTH components are measured every CPI: range_doppler reports each detection's range AND its
  // bistatic range-rate (the Doppler bin). Using only range (the original H = [1,0]) threw the
  // Doppler away and left velocity observable only through the range SEQUENCE -- so a velocity STEP
  // was invisible until the range gap grew enough to notice, by which point the filter had already
  // coasted with a frozen, wrong velocity (verified: a 3->16 m/s step made it coast ~2.5 s and drop).
  // Measuring range-rate directly makes a step observable the instant it happens. Model is now H =
  // I(2x2), R = diag(R_range, R_vel); innovation, S, gate and update are all 2-D (Mahalanobis
  // distance, chi-square with 2 DoF -- hence the ×2 on gate_sq).
  const double Rr = (double)args.track_r_var_m2;    // range measurement variance
  const double Rv = (double)args.track_rv_var_m2s2; // range-rate (Doppler) measurement variance
  // Predicted innovation covariance S = P⁻ + R (H = I): 2x2 symmetric [s00 s01; s01 s11].
  const double s00 = p00_ + Rr, s01 = p01_, s11 = p11_ + Rv;
  const double detS = s00 * s11 - s01 * s01;
  const double iS00 = (detS != 0.0) ? (s11 / detS) : 0.0;
  const double iS01 = (detS != 0.0) ? (-s01 / detS) : 0.0;
  const double iS11 = (detS != 0.0) ? (s00 / detS) : 0.0;
  const double gate_sq = (double)args.track_gate_sigma * (double)args.track_gate_sigma * 2.0; // 2 DoF

  const sensing_detection_t* nearest = nullptr;
  double best_nis = 0.0, best_yr = 0.0, best_yv = 0.0;
  for (const auto& d : detections) {
    const double yr  = (double)d.range_m - x_r_;
    const double yv  = (double)d.vel_mps - x_v_;
    const double nis = yr * (iS00 * yr + iS01 * yv) + yv * (iS01 * yr + iS11 * yv); // yᵀ S⁻¹ y
    if (nearest == nullptr || nis < best_nis) {
      nearest = &d; best_nis = nis; best_yr = yr; best_yv = yv;
    }
  }
  const double nearest_nis = (nearest != nullptr && detS != 0.0) ? best_nis : 0.0;
  const double adapt_win_sq = (double)args.track_q_adapt_window_sigma * (double)args.track_q_adapt_window_sigma * 2.0;
  const bool   near_miss    = (nearest != nullptr && detS != 0.0 && nearest_nis <= adapt_win_sq);
  // Accept if inside the normal 3-sigma gate, OR -- crucial for manoeuvres -- if the track is ALREADY
  // coasting and there is a plausible near-miss. A hard gate otherwise rejects the very detection that
  // would fix the state: at a velocity step the range-rate innovation is large, so the (correct)
  // detection looks like an outlier and is gated out, freezing velocity and coasting forever. But one
  // clean miss followed by a detection near the prediction in BOTH range and range-rate is far more
  // likely the target manoeuvring than a false alarm, so after the first coast we take it and let the
  // 2-D Kalman update (which sees the measured range-rate) snap the velocity to the new value. Result:
  // a manoeuvre costs at most ~1 coast CPI instead of a multi-second coast-then-drop. A GENUINE false
  // alarm still can't hijack the track -- it must fall inside the adapt window (near the prediction),
  // and a real target reappearing resets everything on the next accept.
  const bool   in_gate     = (nearest != nullptr && detS != 0.0 &&
                              (nearest_nis <= gate_sq || (out_.coast_count > 0 && near_miss)));

  // Drive the adaptive-q EWMA from the NEAREST detection's NIS, even when it is JUST outside the
  // accept gate. This is the key to surviving a hard manoeuvre: a strong acceleration pushes the
  // target's own detection past the 3-sigma gate, so if the EWMA only ever saw ACCEPTED detections
  // it would never learn the model had broken (it would coast, q would stay at baseline, and the
  // track would die -- exactly the failure the offline test reproduced). A near-miss IS the manoeuvre
  // signal. Bounded two ways so a far false alarm / mirror can't hijack it: only feed when the
  // near-miss is within track_q_adapt_window_sigma (a wider "this is plausibly our target manoeuvring,
  // not a spurious blip far away" window), and cap the fed value so one wild sample can't saturate
  // the EWMA in a single step.
  if (args.track_q_adapt_enable && nearest != nullptr && detS != 0.0) {
    if (nearest_nis <= adapt_win_sq) {
      const double a   = (double)args.track_nis_ewma_alpha;
      const double fed = std::min(nearest_nis, (double)args.track_q_mult_max);
      nis_ewma_        = (1.0 - a) * nis_ewma_ + a * fed;
    }
  }

  if (in_gate) {
    // ---- Update. Measurement z = [range, range-rate], H picks the first two states (H = [I2 | 0]),
    // so S (= top-left 2x2 of P⁻ + R) and its inverse are identical for both models. Only the gain's
    // state dimension differs: K = P⁻ Hᵀ S⁻¹ has one row per state, using each state's covariance with
    // range (col 0) and range-rate (col 1). ----
    if (!ca_) {
      const double K00 = p00_ * iS00 + p01_ * iS01;
      const double K01 = p00_ * iS01 + p01_ * iS11;
      const double K10 = p01_ * iS00 + p11_ * iS01;
      const double K11 = p01_ * iS01 + p11_ * iS11;
      x_r_ += K00 * best_yr + K01 * best_yv;
      x_v_ += K10 * best_yr + K11 * best_yv;
      const double a00 = 1.0 - K00, a01 = -K01, a10 = -K10, a11 = 1.0 - K11;
      const double n00 = a00 * p00_ + a01 * p01_;
      const double n01 = a00 * p01_ + a01 * p11_;
      const double n11 = a10 * p01_ + a11 * p11_;
      p00_ = n00;
      p01_ = n01;
      p11_ = n11;
    } else {
      // 3-state gain: K row i = [P⁻(i,0), P⁻(i,1)] · S⁻¹  (i = r, v, a).
      const double Kr0 = p00_ * iS00 + p01_ * iS01, Kr1 = p00_ * iS01 + p01_ * iS11;
      const double Kv0 = p01_ * iS00 + p11_ * iS01, Kv1 = p01_ * iS01 + p11_ * iS11;
      const double Ka0 = p02_ * iS00 + p12_ * iS01, Ka1 = p02_ * iS01 + p12_ * iS11;
      x_r_ += Kr0 * best_yr + Kr1 * best_yv;
      x_v_ += Kv0 * best_yr + Kv1 * best_yv;
      x_a_ += Ka0 * best_yr + Ka1 * best_yv;
      // P = (I − K H) P⁻. K H is 3x3 with columns 0,1 = K and column 2 = 0. Compute row by row into a
      // fresh symmetric matrix. m(i,0)=P⁻(i,0)-Kr0/Kv0/Ka0·..., done explicitly via the (I−KH) rows.
      const double m00 = (1.0 - Kr0) * p00_ + (-Kr1) * p01_;
      const double m01 = (1.0 - Kr0) * p01_ + (-Kr1) * p11_;
      const double m02 = (1.0 - Kr0) * p02_ + (-Kr1) * p12_;
      const double m11 = (-Kv0) * p01_ + (1.0 - Kv1) * p11_;
      const double m12 = (-Kv0) * p02_ + (1.0 - Kv1) * p12_;
      // row a: (I−KH) row = [−Ka0, −Ka1, 1]
      const double m22 = (-Ka0) * p02_ + (-Ka1) * p12_ + p22_;
      p00_ = m00; p01_ = m01; p02_ = m02;
      p11_ = m11; p12_ = m12; p22_ = m22;
    }

    out_.updated      = true;
    out_.innovation_m = (float)best_yr;
    out_.nis          = (float)nearest_nis;
    out_.coast_count  = 0;
    gone_count_       = 0;
    out_.nis_ewma     = (float)nis_ewma_;
  } else {
    // ---- Coast: keep the prediction, let P grow (which widens the gate for re-acquisition) ----
    out_.coast_count++;
    // Distinguish a MANOEUVRE (a detection sits just outside the accept gate -- near_miss -- so the
    // target is present, just moving in a way the CV model briefly can't follow) from a GONE target
    // (nothing anywhere near the prediction). Only the latter should count toward dropping the track.
    // This is what lets the filter ride out an arbitrarily hard manoeuvre without a per-route patience
    // setting: as long as SOMETHING plausible is nearby each CPI, q keeps inflating, the gate keeps
    // widening, and it re-acquires; a genuine disappearance still drops after track_max_coast true
    // misses. gone_count_ is reset on any accepted update.
    if (!near_miss) {
      gone_count_++;
    }
    if (gone_count_ > args.track_max_coast) {
      init_ = false;
      out_  = sensing_track_t();
      return out_;
    }
  }

  out_.active         = true;
  out_.range_m        = (float)x_r_;
  out_.range_rate_mps = (float)x_v_;
  out_.sigma_range_m  = (float)std::sqrt(std::max(p00_, 0.0));
  return out_;
}

} // namespace nr_isac
