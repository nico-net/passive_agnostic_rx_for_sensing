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
 */

/*! \file openair1/PHY/NR_UE_ISAC/hierarchical_tracker.cc
 * \brief Implementation of Hierarchical Global 3D Tracker and Closed-Loop Feedback.
 */

#include "hierarchical_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace nr_isac {

namespace {

inline double wrap_pi(double rad)
{
  double val = std::fmod(rad + M_PI, 2.0 * M_PI);
  if (val < 0.0) val += 2.0 * M_PI;
  return val - M_PI;
}

inline double deg2rad(double deg)
{
  return deg * (M_PI / 180.0);
}

inline double rad2deg(double rad)
{
  return rad * (180.0 / M_PI);
}

// Theoretical sinc power envelope along range axis
double envelope_range(double dr_m, double range_res_m, double margin_db = 3.0)
{
  if (range_res_m <= 0.0) return 1.0;
  double xi_r = std::abs(dr_m) / range_res_m;
  if (xi_r < 1.0) return 1.0;
  double margin_lin = std::pow(10.0, margin_db / 10.0);
  double bound = margin_lin / (M_PI * M_PI * xi_r * xi_r);
  return std::min(1.0, bound);
}

// Theoretical sinc power envelope along Doppler/range-rate axis
double envelope_doppler(double dv_mps, double rate_res_mps, double margin_db = 3.0)
{
  if (rate_res_mps <= 0.0) return 1.0;
  double xi_v = std::abs(dv_mps) / rate_res_mps;
  if (xi_v < 1.0) return 1.0;
  double margin_lin = std::pow(10.0, margin_db / 10.0);
  double bound = margin_lin / (M_PI * M_PI * xi_v * xi_v);
  return std::min(1.0, bound);
}

// 2D delay-Doppler ambiguity power envelope
double ambiguity_psf_2d(double dr_m, double dv_mps, double range_res_m, double rate_res_mps, double margin_db = 3.0)
{
  return envelope_range(dr_m, range_res_m, margin_db) * envelope_doppler(dv_mps, rate_res_mps, margin_db);
}

// Invert a 2x2 symmetric positive-definite matrix
bool invert_2x2(const double A[2][2], double inv[2][2])
{
  double det = A[0][0] * A[1][1] - A[0][1] * A[1][0];
  if (det <= 1e-15 || !std::isfinite(det)) {
    return false;
  }
  double inv_det = 1.0 / det;
  inv[0][0] =  A[1][1] * inv_det;
  inv[0][1] = -A[0][1] * inv_det;
  inv[1][0] = -A[1][0] * inv_det;
  inv[1][1] =  A[0][0] * inv_det;
  return true;
}

// Cholesky decomposition of 4x4 symmetric matrix: A = L * L^T
bool cholesky_4x4(const double A[4][4], double L[4][4])
{
  std::memset(L, 0, 16 * sizeof(double));
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j <= i; j++) {
      double sum = 0.0;
      for (int k = 0; k < j; k++) {
        sum += L[i][k] * L[j][k];
      }
      if (i == j) {
        double val = A[i][i] - sum;
        if (val <= 1e-15 || !std::isfinite(val)) return false;
        L[i][j] = std::sqrt(val);
      } else {
        L[i][j] = (A[i][j] - sum) / L[j][j];
      }
    }
  }
  return true;
}

// Solve L * L^T * x = b for 4x4 Cholesky factor L
void cholesky_solve_4x4(const double L[4][4], const double b[4], double x[4])
{
  double y[4];
  // Forward substitution L * y = b
  for (int i = 0; i < 4; i++) {
    double sum = 0.0;
    for (int k = 0; k < i; k++) {
      sum += L[i][k] * y[k];
    }
    y[i] = (b[i] - sum) / L[i][i];
  }
  // Backward substitution L^T * x = y
  for (int i = 3; i >= 0; i--) {
    double sum = 0.0;
    for (int k = i + 1; k < 4; k++) {
      sum += L[k][i] * x[k];
    }
    x[i] = (y[i] - sum) / L[i][i];
  }
}

} // anonymous namespace

bool is_aperture_sidelobe(float range_m, float vel_mps, float score,
                          float trk_range_m, float trk_vel_mps, float trk_score,
                          float range_res_m, float vel_res_mps, float margin_db)
{
  if (trk_score <= 0.0f || score > trk_score) {
    return false;
  }
  double dr = std::abs((double)range_m - (double)trk_range_m);
  double dv = std::abs((double)vel_mps - (double)trk_vel_mps);

  // Inside primary resolved mainlobe cell: part of main peak, not a sidelobe
  if (dr <= 0.8 * (double)range_res_m && dv <= 0.8 * (double)vel_res_mps) {
    return false;
  }

  // Range-coincident Doppler sidelobes
  if (dr <= 1.5 * (double)range_res_m) {
    if (dv > 1.2 * (double)vel_res_mps) {
      double env_v = envelope_doppler(dv, vel_res_mps, margin_db);
      if ((double)score <= (double)trk_score * env_v) {
        return true;
      }
    }
  }

  // 2D delay-Doppler ambiguity envelope
  double env_2d = ambiguity_psf_2d(dr, dv, range_res_m, vel_res_mps, margin_db);
  if ((double)score <= (double)trk_score * env_2d) {
    return true;
  }

  return false;
}

bool is_multipath_shadow(float range_m, float vel_mps, float score,
                         float trk_range_m, float trk_vel_mps, float trk_score,
                         float range_res_m, float vel_res_mps)
{
  if (trk_score <= 0.0f || score > trk_score) {
    return false;
  }
  double dr = (double)range_m - (double)trk_range_m;
  double dv = std::abs((double)vel_mps - (double)trk_vel_mps);

  double max_delay = std::max(4.0 * (double)range_res_m, 12.0);
  double max_dv = 1.5 * (double)vel_res_mps;
  double loss_factor = std::pow(10.0, -3.0 / 10.0); // 3 dB reflection loss

  if (dr > 0.0 && dr <= max_delay && dv <= max_dv && (double)score <= (double)trk_score * loss_factor) {
    return true;
  }
  return false;
}

// -------------------------------------------------------------------------------------------------
// adaptive_clutter_map
// -------------------------------------------------------------------------------------------------
void adaptive_clutter_map::reset()
{
  power_map_.clear();
}

void adaptive_clutter_map::update(float range_m, float vel_mps, float score,
                                  float range_res_m, float vel_res_mps)
{
  float v_guard = std::min(1.0f, 1.5f * vel_res_mps);
  if (std::abs(vel_mps) > v_guard || score <= 0.0f) {
    return;
  }
  int r_bin = (range_res_m > 0.0f) ? (int)std::lround(range_m / range_res_m) : 0;
  int v_bin = (vel_res_mps > 0.0f) ? (int)std::lround(vel_mps / vel_res_mps) : 0;
  auto key = std::make_pair(r_bin, v_bin);

  auto it = power_map_.find(key);
  if (it != power_map_.end()) {
    it->second = (1.0f - alpha_) * it->second + alpha_ * score;
  } else {
    power_map_[key] = score;
  }
}

bool adaptive_clutter_map::is_static_clutter(float range_m, float vel_mps, float score,
                                             float range_res_m, float vel_res_mps) const
{
  if (score <= 0.0f) return false;

  // Direct-path baseline leakage (within 1.5 cells of excess range zero)
  if (range_m <= 1.5f * range_res_m) {
    return true;
  }

  float v_guard = std::min(1.0f, 1.5f * vel_res_mps);
  if (std::abs(vel_mps) > v_guard) {
    return false;
  }

  int r_bin = (range_res_m > 0.0f) ? (int)std::lround(range_m / range_res_m) : 0;
  int v_bin = (vel_res_mps > 0.0f) ? (int)std::lround(vel_mps / vel_res_mps) : 0;
  auto it = power_map_.find(std::make_pair(r_bin, v_bin));
  if (it != power_map_.end()) {
    float margin_lin = std::pow(10.0f, threshold_offset_db_ / 10.0f);
    return score <= it->second * margin_lin;
  }
  return false;
}

// -------------------------------------------------------------------------------------------------
// bistatic_tracklet (Stage 1)
// -------------------------------------------------------------------------------------------------
bistatic_tracklet::bistatic_tracklet(uint32_t id, double t_s, double r_m, double rr_mps,
                                     float score, double r_res_m, double rr_res_mps)
    : track_id(id), time_s(t_s), active(true), updated_this_cpi(true),
      confirmed_updates(0), total_updates(1), coast_count(0), last_score(score),
      status("tentative")
{
  x[0] = r_m;
  x[1] = rr_mps;
  x[2] = 0.0;

  double var_r  = (r_res_m > 0.0) ? (r_res_m * r_res_m / 12.0) : 1.0;
  double var_rr = (rr_res_mps > 0.0) ? (rr_res_mps * rr_res_mps / 12.0) : 1.0;
  double var_a  = 25.0 * 25.0; // initial accel variance (25 m/s^2)^2

  std::memset(p, 0, sizeof(p));
  p[0][0] = var_r;
  p[1][1] = var_rr;
  p[2][2] = var_a;
}

void bistatic_tracklet::predict(double next_t_s, double jerk_psd)
{
  double dt = next_t_s - time_s;
  if (dt <= 0.0) return;

  // Constant-jerk state transition
  x[0] += dt * x[1] + 0.5 * dt * dt * x[2];
  x[1] += dt * x[2];
  // x[2] = x[2]

  double F[3][3] = {
      {1.0, dt, 0.5 * dt * dt},
      {0.0, 1.0, dt},
      {0.0, 0.0, 1.0}
  };

  // Process noise covariance Q for constant jerk PSD
  double t2 = dt * dt, t3 = dt * t2, t4 = dt * t3, t5 = dt * t4;
  double Q[3][3] = {
      {jerk_psd * t5 / 20.0, jerk_psd * t4 / 8.0, jerk_psd * t3 / 6.0},
      {jerk_psd * t4 / 8.0,  jerk_psd * t3 / 3.0, jerk_psd * t2 / 2.0},
      {jerk_psd * t3 / 6.0,  jerk_psd * t2 / 2.0, jerk_psd * dt}
  };

  // P_new = F * P * F^T + Q
  double FP[3][3];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      FP[i][j] = F[i][0] * p[0][j] + F[i][1] * p[1][j] + F[i][2] * p[2][j];
    }
  }
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      p[i][j] = FP[i][0] * F[j][0] + FP[i][1] * F[j][1] + FP[i][2] * F[j][2] + Q[i][j];
    }
  }

  time_s = next_t_s;
  updated_this_cpi = false;
  associated_idx = -1;
}

bool bistatic_tracklet::update(double r_m, double rr_mps, float score,
                               double r_res_m, double rr_res_mps,
                               int det_idx, double gate_chi2)
{
  double R_meas[2][2] = {
      {(r_res_m > 0.0) ? (r_res_m * r_res_m / 12.0) : 1.0, 0.0},
      {0.0, (rr_res_mps > 0.0) ? (rr_res_mps * rr_res_mps / 12.0) : 1.0}
  };

  // Innovation y = z - H*x
  double y[2] = {r_m - x[0], rr_mps - x[1]};

  // Innovation covariance S = H * P * H^T + R
  double S[2][2] = {
      {p[0][0] + R_meas[0][0], p[0][1]},
      {p[1][0], p[1][1] + R_meas[1][1]}
  };

  double invS[2][2];
  if (!invert_2x2(S, invS)) {
    return false;
  }

  // Normalized innovation squared (Mahalanobis distance)
  double nis = y[0] * (invS[0][0] * y[0] + invS[0][1] * y[1]) +
               y[1] * (invS[1][0] * y[0] + invS[1][1] * y[1]);

  if (nis > gate_chi2) {
    return false;
  }

  // Kalman Gain K = P * H^T * invS (3x2)
  double K[3][2];
  for (int i = 0; i < 3; i++) {
    K[i][0] = p[i][0] * invS[0][0] + p[i][1] * invS[1][0];
    K[i][1] = p[i][0] * invS[0][1] + p[i][1] * invS[1][1];
  }

  // State update x = x + K * y
  x[0] += K[0][0] * y[0] + K[0][1] * y[1];
  x[1] += K[1][0] * y[0] + K[1][1] * y[1];
  x[2] += K[2][0] * y[0] + K[2][1] * y[1];

  // Covariance update P = (I - K*H) * P
  // (I - K*H) is 3x3: I - [K0, K1, 0]
  double I_KH[3][3] = {
      {1.0 - K[0][0], -K[0][1], 0.0},
      {-K[1][0], 1.0 - K[1][1], 0.0},
      {-K[2][0], -K[2][1], 1.0}
  };

  double new_P[3][3];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      new_P[i][j] = I_KH[i][0] * p[0][j] + I_KH[i][1] * p[1][j] + I_KH[i][2] * p[2][j];
    }
  }
  // Symmetrize
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      p[i][j] = 0.5 * (new_P[i][j] + new_P[j][i]);
    }
  }

  coast_count = 0;
  total_updates++;
  confirmed_updates++;
  updated_this_cpi = true;
  associated_idx = det_idx;
  last_score = score;
  if (confirmed_updates >= 3) {
    status = "confirmed";
  }
  return true;
}

void bistatic_tracklet::coast()
{
  coast_count++;
  updated_this_cpi = false;
  associated_idx = -1;
  if (coast_count >= 3) {
    status = "coasting";
  }
}

// -------------------------------------------------------------------------------------------------
// global_3d_track (Stage 2)
// -------------------------------------------------------------------------------------------------
global_3d_track::global_3d_track(uint32_t id, double t_s, const bistatic_geometry_3d_t& g,
                                 double r_m, double rr_mps, double az_deg, double el_deg,
                                 double r_res_m, double rr_res_mps, double az_std_deg, double el_std_deg,
                                 int det_idx)
    : track_id(id), time_s(t_s), status("confirmed"), coast_count(0),
      total_updates(1), confirmed_updates(1), updated_this_cpi(true),
      associated_idx(det_idx), nis(0.0), nis_ewma(1.0), accel_var(100.0),
      stage1_range_m(r_m), stage1_range_rate_mps(rr_mps), geom(g)
{
  std::memset(x, 0, sizeof(x));
  std::memset(p, 0, sizeof(p));

  // Closed-form 3D ray-ellipsoid birth
  double az_rad = deg2rad(az_deg);
  double el_rad = deg2rad(el_deg);
  double ce = std::cos(el_rad);
  vec3_t u(ce * std::cos(az_rad), ce * std::sin(az_rad), std::sin(el_rad));

  vec3_t a = geom.rx - geom.tx;
  double baseline = geom.baseline;
  double total_range = baseline + r_m;
  double denom = 2.0 * (total_range + a.dot(u));
  double numer = total_range * total_range - a.dot(a);

  double dist = (denom > 1e-9) ? (numer / denom) : (0.5 * r_m);
  if (dist < 0.0 || !std::isfinite(dist)) dist = 10.0;

  vec3_t pos = geom.rx + u * dist;
  x[0] = pos.x;
  x[1] = pos.y;
  x[2] = pos.z;

  // Velocity projection along bistatic gradient
  vec3_t r_tx = pos - geom.tx;
  vec3_t r_rx = pos - geom.rx;
  double d_tx = std::max(1.0, r_tx.norm());
  double d_rx = std::max(1.0, r_rx.norm());
  vec3_t grad = (r_tx / d_tx) + (r_rx / d_rx);
  double g2 = grad.dot(grad);
  vec3_t vel = (g2 > 1e-6) ? (grad * (rr_mps / g2)) : vec3_t(0, 0, 0);

  x[3] = vel.x;
  x[4] = vel.y;
  x[5] = vel.z;

  // Initial covariance: position uncertainty from range and angular sigmas
  double r_sigma = std::max(0.5, r_res_m / std::sqrt(12.0));
  double az_sigma_rad = deg2rad(std::max(0.5, az_std_deg));
  double el_sigma_rad = deg2rad(std::max(1.0, el_std_deg));

  p[0][0] = r_sigma * r_sigma + std::pow(dist * az_sigma_rad, 2.0);
  p[1][1] = r_sigma * r_sigma + std::pow(dist * az_sigma_rad, 2.0);
  p[2][2] = r_sigma * r_sigma + std::pow(dist * el_sigma_rad, 2.0);

  p[3][3] = 100.0;
  p[4][4] = 100.0;
  p[5][5] = 100.0;
}

void global_3d_track::evaluate_model(const double s[6], double out[4], bool with_angles) const
{
  vec3_t pos(s[0], s[1], s[2]);
  vec3_t vel(s[3], s[4], s[5]);

  vec3_t r_tx = pos - geom.tx;
  vec3_t r_rx = pos - geom.rx;
  double d_tx = std::max(1e-3, r_tx.norm());
  double d_rx = std::max(1e-3, r_rx.norm());

  // 1. Bistatic excess range
  out[0] = d_tx + d_rx - geom.baseline;

  // 2. Bistatic range rate
  vec3_t grad = (r_tx / d_tx) + (r_rx / d_rx);
  out[1] = grad.dot(vel);

  if (with_angles) {
    // 3. Azimuth (deg CCW from east)
    out[2] = rad2deg(std::atan2(r_rx.y, r_rx.x));
    // 4. Elevation (deg)
    double d_xy = std::hypot(r_rx.x, r_rx.y);
    out[3] = rad2deg(std::atan2(r_rx.z, std::max(1e-6, d_xy)));
  }
}

void global_3d_track::evaluate_jacobian(const double s[6], double H[4][6], bool with_angles) const
{
  std::memset(H, 0, 24 * sizeof(double));

  vec3_t pos(s[0], s[1], s[2]);
  vec3_t vel(s[3], s[4], s[5]);

  vec3_t r_tx = pos - geom.tx;
  vec3_t r_rx = pos - geom.rx;
  double d_tx = std::max(1e-3, r_tx.norm());
  double d_rx = std::max(1e-3, r_rx.norm());

  vec3_t u_tx = r_tx / d_tx;
  vec3_t u_rx = r_rx / d_rx;
  vec3_t grad = u_tx + u_rx;

  // Row 0: d(range)/d(state)
  H[0][0] = grad.x;
  H[0][1] = grad.y;
  H[0][2] = grad.z;

  // Row 1: d(range_rate)/d(state)
  H[1][3] = grad.x;
  H[1][4] = grad.y;
  H[1][5] = grad.z;

  // Curvature term: C * v
  double C[3][3] = {{0}};
  double inv_tx = 1.0 / d_tx;
  double inv_rx = 1.0 / d_rx;
  for (int i = 0; i < 3; i++) {
    double ut_i = (i == 0) ? u_tx.x : ((i == 1) ? u_tx.y : u_tx.z);
    double ur_i = (i == 0) ? u_rx.x : ((i == 1) ? u_rx.y : u_rx.z);
    for (int j = 0; j < 3; j++) {
      double delta = (i == j) ? 1.0 : 0.0;
      double ut_j = (j == 0) ? u_tx.x : ((j == 1) ? u_tx.y : u_tx.z);
      double ur_j = (j == 0) ? u_rx.x : ((j == 1) ? u_rx.y : u_rx.z);
      C[i][j] = (delta - ut_i * ut_j) * inv_tx + (delta - ur_i * ur_j) * inv_rx;
    }
  }
  H[1][0] = C[0][0] * vel.x + C[0][1] * vel.y + C[0][2] * vel.z;
  H[1][1] = C[1][0] * vel.x + C[1][1] * vel.y + C[1][2] * vel.z;
  H[1][2] = C[2][0] * vel.x + C[2][1] * vel.y + C[2][2] * vel.z;

  if (with_angles) {
    double dx = r_rx.x, dy = r_rx.y, dz = r_rx.z;
    double dxy2 = std::max(1e-6, dx * dx + dy * dy);
    double dxy = std::sqrt(dxy2);
    double d3d2 = std::max(1e-6, dxy2 + dz * dz);
    double r2d = 180.0 / M_PI;

    // Row 2: d(azimuth_deg)/d(pos)
    H[2][0] = (-dy / dxy2) * r2d;
    H[2][1] = ( dx / dxy2) * r2d;
    H[2][2] = 0.0;

    // Row 3: d(elevation_deg)/d(pos)
    H[3][0] = (-dx * dz / (d3d2 * dxy)) * r2d;
    H[3][1] = (-dy * dz / (d3d2 * dxy)) * r2d;
    H[3][2] = (dxy / d3d2) * r2d;
  }
}

void global_3d_track::predict(double next_t_s)
{
  double dt = next_t_s - time_s;
  if (dt <= 0.0) return;

  // State prediction x_new = F * x
  x[0] += dt * x[3];
  x[1] += dt * x[4];
  x[2] += dt * x[5];

  // Transition matrix F (6x6)
  double F[6][6] = {{0}};
  for (int i = 0; i < 6; i++) F[i][i] = 1.0;
  for (int i = 0; i < 3; i++) F[i][i + 3] = dt;

  // Process noise Q(dt) for 3D constant velocity with acceleration variance
  double q_pos = 0.25 * dt * dt * dt * dt * accel_var;
  double q_pos_vel = 0.5 * dt * dt * dt * accel_var;
  double q_vel = dt * dt * accel_var;

  double Q[6][6] = {{0}};
  for (int i = 0; i < 3; i++) {
    Q[i][i] = q_pos;
    Q[i][i + 3] = q_pos_vel;
    Q[i + 3][i] = q_pos_vel;
    Q[i + 3][i + 3] = q_vel;
  }

  // temp = F * P
  double temp[6][6] = {{0}};
  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < 6; j++) {
      double sum = 0.0;
      for (int k = 0; k < 6; k++) sum += F[i][k] * p[k][j];
      temp[i][j] = sum;
    }
  }

  // P_new = temp * F^T + Q
  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < 6; j++) {
      double sum = Q[i][j];
      for (int k = 0; k < 6; k++) sum += temp[i][k] * F[j][k];
      p[i][j] = sum;
    }
  }

  // Enforce exact symmetry
  for (int i = 0; i < 6; i++) {
    for (int j = i + 1; j < 6; j++) {
      double sym = 0.5 * (p[i][j] + p[j][i]);
      p[i][j] = sym;
      p[j][i] = sym;
    }
  }

  time_s = next_t_s;
  updated_this_cpi = false;
  associated_idx = -1;
}

bool global_3d_track::update(double r_m, double rr_mps, bool with_angles, double az_deg, double el_deg,
                             double r_res_m, double rr_res_mps, double az_std_deg, double el_std_deg,
                             int det_idx, double s1_r, double s1_rr)
{
  stage1_range_m = s1_r;
  stage1_range_rate_mps = s1_rr;

  bool attempt_4d = with_angles;
  bool update_success = false;

  // 1. Try 4D measurement update (Range, Range-Rate, Azimuth, Elevation)
  if (attempt_4d) {
    double z[4] = {r_m, rr_mps, az_deg, el_deg};
    double h_val[4];
    evaluate_model(x, h_val, true);

    double y[4];
    y[0] = z[0] - h_val[0];
    y[1] = z[1] - h_val[1];
    y[2] = rad2deg(wrap_pi(deg2rad(z[2] - h_val[2])));
    y[3] = rad2deg(wrap_pi(deg2rad(z[3] - h_val[3])));

    double H[4][6];
    evaluate_jacobian(x, H, true);

    double R_noise[4] = {
        std::max(0.25, std::pow(r_res_m / std::sqrt(12.0), 2.0)),
        std::max(0.25, std::pow(rr_res_mps / std::sqrt(12.0), 2.0)),
        std::max(0.5, az_std_deg * az_std_deg),
        std::max(1.0, el_std_deg * el_std_deg)
    };

    // S = H * P * H^T + R
    double S[4][4] = {{0}};
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        double sum = 0.0;
        for (int k = 0; k < 6; k++) {
          for (int m = 0; m < 6; m++) {
            sum += H[i][k] * p[k][m] * H[j][m];
          }
        }
        S[i][j] = sum + ((i == j) ? R_noise[i] : 0.0);
      }
    }

    double L[4][4];
    if (cholesky_4x4(S, L)) {
      // Innovation NIS = y^T * S^-1 * y
      double w[4];
      for (int i = 0; i < 4; i++) {
        double sum = 0.0;
        for (int k = 0; k < i; k++) sum += L[i][k] * w[k];
        w[i] = (y[i] - sum) / L[i][i];
      }
      double nis_4d = w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3];

      if (nis_4d <= CHI2_4DOF_99) {
        // Kalman Gain K = P * H^T * S^-1 (6x4)
        double K[6][4];
        for (int i = 0; i < 6; i++) {
          double b[4];
          for (int j = 0; j < 4; j++) {
            double sum = 0.0;
            for (int k = 0; k < 6; k++) sum += p[i][k] * H[j][k];
            b[j] = sum;
          }
          cholesky_solve_4x4(L, b, K[i]);
        }

        // Decoupled update: angular innovation updates position only, not velocity directly
        for (int i = 3; i < 6; i++) {
          K[i][2] = 0.0;
          K[i][3] = 0.0;
        }

        // Update state x = x + K * y
        for (int i = 0; i < 6; i++) {
          x[i] += K[i][0] * y[0] + K[i][1] * y[1] + K[i][2] * y[2] + K[i][3] * y[3];
        }

        // Joseph form covariance update: P = (I - K*H)*P*(I - K*H)^T + K*R*K^T
        double I_KH[6][6];
        for (int i = 0; i < 6; i++) {
          for (int j = 0; j < 6; j++) {
            double sum = 0.0;
            for (int k = 0; k < 4; k++) sum += K[i][k] * H[k][j];
            I_KH[i][j] = ((i == j) ? 1.0 : 0.0) - sum;
          }
        }
        double temp[6][6];
        for (int i = 0; i < 6; i++) {
          for (int j = 0; j < 6; j++) {
            double sum = 0.0;
            for (int k = 0; k < 6; k++) sum += I_KH[i][k] * p[k][j];
            temp[i][j] = sum;
          }
        }
        double new_P[6][6];
        for (int i = 0; i < 6; i++) {
          for (int j = 0; j < 6; j++) {
            double sum = 0.0;
            for (int k = 0; k < 6; k++) sum += temp[i][k] * I_KH[j][k];
            // Add K * R * K^T
            for (int m = 0; m < 4; m++) sum += K[i][m] * R_noise[m] * K[j][m];
            new_P[i][j] = sum;
          }
        }
        for (int i = 0; i < 6; i++) {
          for (int j = 0; j < 6; j++) {
            p[i][j] = 0.5 * (new_P[i][j] + new_P[j][i]);
          }
        }

        nis = nis_4d;
        update_success = true;
      }
    }
  }

  // 2. Fallback to 2D range-rate update if 4D was rejected or unavailable
  if (!update_success) {
    double z[2] = {r_m, rr_mps};
    double h_val[4];
    evaluate_model(x, h_val, false);

    double y[2] = {z[0] - h_val[0], z[1] - h_val[1]};
    double H[4][6];
    evaluate_jacobian(x, H, false);

    double R_noise[2] = {
        std::max(0.25, std::pow(r_res_m / std::sqrt(12.0), 2.0)),
        std::max(0.25, std::pow(rr_res_mps / std::sqrt(12.0), 2.0))
    };

    double S[2][2] = {{0}};
    for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 2; j++) {
        double sum = 0.0;
        for (int k = 0; k < 6; k++) {
          for (int m = 0; m < 6; m++) {
            sum += H[i][k] * p[k][m] * H[j][m];
          }
        }
        S[i][j] = sum + ((i == j) ? R_noise[i] : 0.0);
      }
    }

    double invS[2][2];
    if (invert_2x2(S, invS)) {
      double nis_2d = y[0] * (invS[0][0] * y[0] + invS[0][1] * y[1]) +
                      y[1] * (invS[1][0] * y[0] + invS[1][1] * y[1]);
      if (nis_2d <= CHI2_2DOF_99) {
        // Kalman Gain K = P * H^T * invS (6x2)
        double K[6][2];
        for (int i = 0; i < 6; i++) {
          double PH0 = 0.0, PH1 = 0.0;
          for (int k = 0; k < 6; k++) {
            PH0 += p[i][k] * H[0][k];
            PH1 += p[i][k] * H[1][k];
          }
          K[i][0] = PH0 * invS[0][0] + PH1 * invS[1][0];
          K[i][1] = PH0 * invS[0][1] + PH1 * invS[1][1];
        }

        // State update x = x + K * y
        for (int i = 0; i < 6; i++) {
          x[i] += K[i][0] * y[0] + K[i][1] * y[1];
        }

        // Covariance update
        double I_KH[6][6];
        for (int i = 0; i < 6; i++) {
          for (int j = 0; j < 6; j++) {
            double sum = K[i][0] * H[0][j] + K[i][1] * H[1][j];
            I_KH[i][j] = ((i == j) ? 1.0 : 0.0) - sum;
          }
        }
        double temp[6][6];
        for (int i = 0; i < 6; i++) {
          for (int j = 0; j < 6; j++) {
            double sum = 0.0;
            for (int k = 0; k < 6; k++) sum += I_KH[i][k] * p[k][j];
            temp[i][j] = sum;
          }
        }
        for (int i = 0; i < 6; i++) {
          for (int j = 0; j < 6; j++) {
            double sum = 0.0;
            for (int k = 0; k < 6; k++) sum += temp[i][k] * I_KH[j][k];
            for (int m = 0; m < 2; m++) sum += K[i][m] * R_noise[m] * K[j][m];
            p[i][j] = 0.5 * (sum + p[j][i]);
          }
        }

        nis = nis_2d;
        update_success = true;
      }
    }
  }

  if (!update_success) {
    coast();
    return false;
  }

  nis_ewma = 0.75 * nis_ewma + 0.25 * nis;
  coast_count = 0;
  total_updates++;
  confirmed_updates++;
  updated_this_cpi = true;
  associated_idx = det_idx;
  status = "confirmed";
  return true;
}

void global_3d_track::coast()
{
  coast_count++;
  updated_this_cpi = false;
  associated_idx = -1;
  if (coast_count >= 3) {
    status = "coasting";
  }
}

sensing_track_t global_3d_track::to_sensing_track() const
{
  sensing_track_t out;
  out.track_id = track_id;
  out.active = true;
  out.updated = updated_this_cpi;
  out.range_m = (float)stage1_range_m;
  out.range_rate_mps = (float)stage1_range_rate_mps;
  out.coast_count = coast_count;
  out.nis = (float)nis;
  out.nis_ewma = (float)nis_ewma;
  out.sigma_range_m = (float)std::sqrt(std::max(0.0, p[0][0]));

  double obs[4];
  evaluate_model(x, obs, true);

  out.azimuth_valid = true;
  out.azimuth_deg = (float)obs[2];
  out.azimuth_std_deg = (float)std::sqrt(std::max(0.01, (p[1][1] / (x[0]*x[0] + x[1]*x[1])) * (180.0/M_PI)*(180.0/M_PI)));

  out.elevation_valid = true;
  out.elevation_deg = (float)obs[3];
  out.elevation_std_deg = 2.0f;

  out.pos_valid = true;
  out.pos_x = (float)x[0];
  out.pos_y = (float)x[1];
  out.pos_z = (float)x[2];
  out.vel_x = (float)x[3];
  out.vel_y = (float)x[4];
  out.vel_z = (float)x[5];

  return out;
}

// -------------------------------------------------------------------------------------------------
// hierarchical_tracker
// -------------------------------------------------------------------------------------------------
hierarchical_tracker::hierarchical_tracker(const bistatic_geometry_3d_t& geom,
                                           const hierarchical_tracker_config_t& cfg)
    : geom_(geom), cfg_(cfg),
      clutter_map_(cfg.clutter_map_alpha, cfg.clutter_threshold_offset_db)
{
}

void hierarchical_tracker::reset()
{
  s1_tracks_.clear();
  g_tracks_.clear();
  s1_to_global_.clear();
  last_sensing_tracks_.clear();
  clutter_map_.reset();
  next_s1_id_ = 1;
  next_g_id_ = 1;
  prev_time_s_ = -1.0;
}

std::vector<bistatic_tracklet> hierarchical_tracker::get_prior_confirmed_tracklets() const
{
  std::vector<bistatic_tracklet> res;
  for (const auto& trk : s1_tracks_) {
    if (trk.confirmed_updates >= 1 || trk.status == "confirmed") {
      res.push_back(trk);
    }
  }
  return res;
}

bool hierarchical_tracker::should_reject_detection(float range_m, float vel_mps, float score,
                                                   float range_res_m, float vel_res_mps,
                                                   bool& near_track) const
{
  near_track = false;
  auto priors = get_prior_confirmed_tracklets();

  for (const auto& trk : priors) {
    double dr = std::abs((double)range_m - trk.x[0]);
    double dv = std::abs((double)vel_mps - trk.x[1]);
    double sigma_r = std::max(std::sqrt(trk.p[0][0]), (double)range_res_m);
    double sigma_v = std::max(std::sqrt(trk.p[1][1]), (double)vel_res_mps);

    double gate = std::pow(dr / (3.0 * sigma_r), 2.0) + std::pow(dv / (3.0 * sigma_v), 2.0);
    if (gate <= 1.0) {
      near_track = true;
    } else {
      if (is_aperture_sidelobe(range_m, vel_mps, score,
                               (float)trk.x[0], (float)trk.x[1], trk.last_score,
                               range_res_m, vel_res_mps, cfg_.psf_sidelobe_margin_db)) {
        return true; // reject as sidelobe
      }
      if (is_multipath_shadow(range_m, vel_mps, score,
                              (float)trk.x[0], (float)trk.x[1], trk.last_score,
                              range_res_m, vel_res_mps)) {
        return true; // reject as multipath shadow
      }
    }
  }

  // Reject static clutter in untracked space
  if (!near_track) {
    if (clutter_map_.is_static_clutter(range_m, vel_mps, score, range_res_m, vel_res_mps)) {
      return true;
    }
  }

  return false;
}

const std::vector<sensing_track_t>& hierarchical_tracker::update(
    const std::vector<sensing_detection_t>& detections,
    double time_s,
    double range_res_m,
    double vel_res_mps,
    double dwell_s)
{
  last_sensing_tracks_.clear();

  // 1. Update adaptive clutter map with current detections
  for (const auto& d : detections) {
    clutter_map_.update(d.range_m, d.vel_mps, d.snr_db, (float)range_res_m, (float)vel_res_mps);
  }

  // 2. Predict Stage 1 tracklets and Stage 2 global 3D tracks
  if (prev_time_s_ > 0.0 && time_s > prev_time_s_) {
    for (auto& s1 : s1_tracks_) {
      s1.predict(time_s, cfg_.jerk_psd);
    }
    for (auto& kv : g_tracks_) {
      kv.second.predict(time_s);
    }
  }
  prev_time_s_ = time_s;

  // 3. Stage 1 Data Association (Greedy Nearest-Neighbor on Chi-Square distance)
  std::vector<bool> det_matched(detections.size(), false);
  struct match_cand_t {
    size_t s1_idx;
    size_t det_idx;
    double dist;
  };
  std::vector<match_cand_t> candidates;

  for (size_t i = 0; i < s1_tracks_.size(); i++) {
    const auto& trk = s1_tracks_[i];
    for (size_t j = 0; j < detections.size(); j++) {
      double dr = (double)detections[j].range_m - trk.x[0];
      double dv = (double)detections[j].vel_mps - trk.x[1];
      double S00 = trk.p[0][0] + (range_res_m * range_res_m / 12.0);
      double S11 = trk.p[1][1] + (vel_res_mps * vel_res_mps / 12.0);
      double chi2 = (dr * dr / S00) + (dv * dv / S11);
      if (chi2 <= cfg_.gate_chi2_2d) {
        candidates.push_back({i, j, chi2});
      }
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const match_cand_t& a, const match_cand_t& b) { return a.dist < b.dist; });

  std::vector<bool> s1_matched(s1_tracks_.size(), false);
  for (const auto& cand : candidates) {
    if (!s1_matched[cand.s1_idx] && !det_matched[cand.det_idx]) {
      const auto& d = detections[cand.det_idx];
      s1_tracks_[cand.s1_idx].update(d.range_m, d.vel_mps, d.snr_db,
                                     range_res_m, vel_res_mps, (int)cand.det_idx, cfg_.gate_chi2_2d);
      s1_matched[cand.s1_idx] = true;
      det_matched[cand.det_idx] = true;
    }
  }

  // Coast unassigned Stage-1 tracklets
  for (size_t i = 0; i < s1_tracks_.size(); i++) {
    if (!s1_matched[i]) {
      s1_tracks_[i].coast();
    }
  }

  // Birth new Stage-1 tracklets from unassigned detections
  for (size_t j = 0; j < detections.size(); j++) {
    if (!det_matched[j]) {
      const auto& d = detections[j];
      bool near_trk = false;
      if (!should_reject_detection(d.range_m, d.vel_mps, d.snr_db, (float)range_res_m, (float)vel_res_mps, near_trk)) {
        bistatic_tracklet new_s1(next_s1_id_++, time_s, d.range_m, d.vel_mps, d.snr_db,
                                 range_res_m, vel_res_mps);
        new_s1.associated_idx = (int)j;
        s1_tracks_.push_back(new_s1);
      }
    }
  }

  // Prune dead Stage-1 tracklets
  s1_tracks_.erase(
      std::remove_if(s1_tracks_.begin(), s1_tracks_.end(),
                     [](const bistatic_tracklet& t) { return t.coast_count > 3; }),
      s1_tracks_.end());

  // 4. Stage 2 Hierarchical Association and Updates
  std::set<uint32_t> updated_global_ids;

  // Phase 4A: Direct measurement update for bound Stage-1 tracks
  for (const auto& s1 : s1_tracks_) {
    if (!s1.updated_this_cpi || s1.associated_idx < 0 || (size_t)s1.associated_idx >= detections.size()) {
      continue;
    }
    auto it = s1_to_global_.find(s1.track_id);
    if (it != s1_to_global_.end()) {
      uint32_t gid = it->second;
      auto git = g_tracks_.find(gid);
      if (git != g_tracks_.end() && updated_global_ids.find(gid) == updated_global_ids.end()) {
        const auto& d = detections[s1.associated_idx];
        git->second.update(d.range_m, d.vel_mps, d.azimuth_valid, d.azimuth_deg, d.elevation_deg,
                           range_res_m, vel_res_mps,
                           d.azimuth_std_deg > 0.0f ? d.azimuth_std_deg : 2.0,
                           d.elevation_std_deg > 0.0f ? d.elevation_std_deg : 5.0,
                           s1.associated_idx, s1.x[0], s1.x[1]);
        updated_global_ids.insert(gid);
      }
    }
  }

  // Phase 4B: Associate newly confirming or unbound Stage-1 tracks
  for (const auto& s1 : s1_tracks_) {
    if (!s1.updated_this_cpi || s1.associated_idx < 0 || (size_t)s1.associated_idx >= detections.size()) {
      continue;
    }
    auto bound_it = s1_to_global_.find(s1.track_id);
    if (bound_it != s1_to_global_.end() && updated_global_ids.find(bound_it->second) != updated_global_ids.end()) {
      continue;
    }

    bool is_confirmed_s1 = (s1.status == "confirmed" || s1.confirmed_updates >= 1 || s1.total_updates >= cfg_.confirm_updates);
    if (!is_confirmed_s1) continue;

    const auto& d = detections[s1.associated_idx];
    uint32_t best_gid = 0;
    double best_nis = std::numeric_limits<double>::infinity();

    // Check association against all existing Global 3D tracks (preserving global IDs!)
    for (auto& kv : g_tracks_) {
      if (updated_global_ids.find(kv.first) != updated_global_ids.end()) continue;

      double h_val[4];
      kv.second.evaluate_model(kv.second.x, h_val, false);
      double dr = (double)d.range_m - h_val[0];
      double dv = (double)d.vel_mps - h_val[1];
      double S00 = kv.second.p[0][0] + (range_res_m * range_res_m / 12.0);
      double S11 = kv.second.p[3][3] + (vel_res_mps * vel_res_mps / 12.0);
      double nis_2d = (dr * dr / S00) + (dv * dv / S11);

      if (nis_2d <= cfg_.gate_chi2_2d && nis_2d < best_nis) {
        best_nis = nis_2d;
        best_gid = kv.first;
      }
    }

    if (best_gid != 0) {
      // Re-bind to existing global track!
      s1_to_global_[s1.track_id] = best_gid;
      g_tracks_[best_gid].update(d.range_m, d.vel_mps, d.azimuth_valid, d.azimuth_deg, d.elevation_deg,
                                 range_res_m, vel_res_mps,
                                 d.azimuth_std_deg > 0.0f ? d.azimuth_std_deg : 2.0,
                                 d.elevation_std_deg > 0.0f ? d.elevation_std_deg : 5.0,
                                 s1.associated_idx, s1.x[0], s1.x[1]);
      updated_global_ids.insert(best_gid);
      continue;
    }

    if (bound_it != s1_to_global_.end()) continue;

    // Birth new global 3D track only if completely unbound and has valid AoA
    if (d.azimuth_valid) {
      uint32_t new_gid = next_g_id_++;
      global_3d_track new_g(new_gid, time_s, geom_, d.range_m, d.vel_mps,
                            d.azimuth_deg, d.elevation_valid ? d.elevation_deg : 0.0,
                            range_res_m, vel_res_mps,
                            d.azimuth_std_deg > 0.0f ? d.azimuth_std_deg : 2.0,
                            d.elevation_std_deg > 0.0f ? d.elevation_std_deg : 5.0,
                            s1.associated_idx);
      g_tracks_[new_gid] = new_g;
      s1_to_global_[s1.track_id] = new_gid;
      updated_global_ids.insert(new_gid);
    }
  }

  // Phase 4C: Coast unupdated global tracks & prune stale ones
  std::vector<uint32_t> dead_gids;
  for (auto& kv : g_tracks_) {
    if (updated_global_ids.find(kv.first) == updated_global_ids.end()) {
      kv.second.coast();
    }
    if (kv.second.coast_count > cfg_.max_coasts_3d) {
      dead_gids.push_back(kv.first);
    }
  }
  for (uint32_t gid : dead_gids) {
    g_tracks_.erase(gid);
  }

  // Clean mapping
  std::set<uint32_t> active_s1;
  for (const auto& s1 : s1_tracks_) active_s1.insert(s1.track_id);
  for (auto it = s1_to_global_.begin(); it != s1_to_global_.end(); ) {
    if (active_s1.find(it->first) == active_s1.end() || g_tracks_.find(it->second) == g_tracks_.end()) {
      it = s1_to_global_.erase(it);
    } else {
      ++it;
    }
  }

  // 5. Emit active confirmed tracks
  for (const auto& kv : g_tracks_) {
    if (kv.second.status == "confirmed" || kv.second.confirmed_updates >= 1) {
      last_sensing_tracks_.push_back(kv.second.to_sensing_track());
    }
  }

  return last_sensing_tracks_;
}

} // namespace nr_isac
