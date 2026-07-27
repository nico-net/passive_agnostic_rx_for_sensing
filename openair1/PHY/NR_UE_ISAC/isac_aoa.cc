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

/*! \file openair1/PHY/NR_UE_ISAC/isac_aoa.cc
 * \brief Receive-array AoA estimation — implementation. See isac_aoa.h for the design rationale.
 */

#include "isac_aoa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/utils/LOG/log.h"
#include "common/platform_constants.h"

namespace nr_isac {

namespace {

constexpr double TWO_PI = 6.283185307179586476925286766559;

inline double wrap_pi(double a)
{
  while (a > M_PI) {
    a -= TWO_PI;
  }
  while (a < -M_PI) {
    a += TWO_PI;
  }
  return a;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Array geometry
// ---------------------------------------------------------------------------------------------

bool parse_rx_array(const std::string& spec, double boresight_deg, double fc_hz, aoa_array_t& out, bool quiet)
{
  out = aoa_array_t{};
  if (spec.empty() || fc_hz <= 0.0) {
    return false;
  }
  out.lambda_m = (double)SPEED_OF_LIGHT / fc_hz;

  const double rot = boresight_deg * M_PI / 180.0;
  const double cs = std::cos(rot), sn = std::sin(rot);
  size_t       pos = 0;
  while (pos <= spec.size()) {
    const size_t end = spec.find(';', pos);
    std::string  tok = spec.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
    pos              = (end == std::string::npos) ? spec.size() + 1 : end + 1;
    // Trim.
    size_t b = tok.find_first_not_of(" \t");
    if (b == std::string::npos) {
      continue;
    }
    tok = tok.substr(b);
    double ex = 0.0, ey = 0.0;
    if (std::sscanf(tok.c_str(), "%lf,%lf", &ex, &ey) != 2) {
      LOG_W(PHY, "SENSING: ignoring malformed rx_array element '%s' (want \"x,y\")\n", tok.c_str());
      continue;
    }
    if (out.ex.size() >= ISAC_MAX_RX_ANT) {
      LOG_W(PHY, "SENSING: rx_array truncated at %u elements\n", ISAC_MAX_RX_ANT);
      break;
    }
    out.ex.push_back((float)(ex * cs - ey * sn));
    out.ey.push_back((float)(ex * sn + ey * cs));
  }

  if (out.ex.size() < 2) {
    if (out.ex.size() == 1) {
      LOG_W(PHY, "SENSING: rx_array has a single element -- no AoA is observable\n");
    }
    out.ex.clear();
    out.ey.clear();
    return false;
  }

  // Spacing extremes: the smallest pairwise gap sets the ambiguity limit, the largest sets resolution.
  double min_gap = 1e30, max_gap = 0.0;
  for (uint32_t i = 0; i < out.size(); i++) {
    for (uint32_t j = i + 1; j < out.size(); j++) {
      const double g = std::hypot(out.ex[i] - out.ex[j], out.ey[i] - out.ey[j]);
      min_gap        = std::min(min_gap, g);
      max_gap        = std::max(max_gap, g);
    }
  }
  out.max_spacing_m = max_gap;
  out.ambiguous     = (min_gap > out.lambda_m * 0.5 + 1e-9);

  // Collinearity: max perpendicular deviation from the line through elements 0 and the farthest one.
  // A collinear array cannot distinguish a bearing from its mirror about that line (the phase depends
  // only on d·û), so the scan has to be restricted to one half-plane.
  uint32_t far = 1;
  double   fd  = 0.0;
  for (uint32_t i = 1; i < out.size(); i++) {
    const double d = std::hypot(out.ex[i] - out.ex[0], out.ey[i] - out.ey[0]);
    if (d > fd) {
      fd  = d;
      far = i;
    }
  }
  const double ax = (out.ex[far] - out.ex[0]) / std::max(fd, 1e-12);
  const double ay = (out.ey[far] - out.ey[0]) / std::max(fd, 1e-12);
  out.axis_rad    = std::atan2(ay, ax);
  double max_perp = 0.0;
  for (uint32_t i = 0; i < out.size(); i++) {
    const double dx = out.ex[i] - out.ex[0];
    const double dy = out.ey[i] - out.ey[0];
    max_perp        = std::max(max_perp, std::fabs(-dx * ay + dy * ax));
  }
  // "Non-collinear enough to resolve the mirror" is a wavelength-scale question, not a numerical one:
  // a perpendicular spread far below lambda/8 gives a phase difference under the noise.
  out.collinear = (max_perp < out.lambda_m / 8.0);

  if (quiet) {
    return true;
  }
  LOG_I(PHY,
        "SENSING: rx_array %u elements, fc=%.3fGHz, boresight=%.1fdeg, spacing %.4f-%.4fm (lambda/2=%.4fm)%s%s\n",
        out.size(), fc_hz / 1e9, boresight_deg, min_gap, max_gap, out.lambda_m / 2.0,
        out.ambiguous ? " -- SPACING AMBIGUOUS" : "",
        out.collinear ? " -- collinear (mirror ambiguity, scan restricted)" : "");
  return true;
}

// ---------------------------------------------------------------------------------------------
// Estimator
// ---------------------------------------------------------------------------------------------

aoa_estimator::aoa_estimator(const nr_isac_args_t& args_, const aoa_array_t& array_) : args(args_), array(array_)
{
  cal_.assign(array.size(), icf_t(1.0f, 0.0f));
  cal_accum_.assign(array.size(), icf_t(0.0f, 0.0f));

  // Scan window. A collinear array is restricted to the half-plane it faces so that exactly one of
  // each mirrored pair is reachable; anything else scans the full circle.
  if (array.collinear) {
    const double broadside = (args.aoa_broadside_deg > -1000.0f)
                                 ? (double)args.aoa_broadside_deg * M_PI / 180.0
                                 : array.axis_rad + M_PI / 2.0; // default: the array's own normal
    scan_lo_ = broadside - M_PI / 2.0;
    scan_hi_ = broadside + M_PI / 2.0;
  } else {
    scan_lo_ = -M_PI;
    scan_hi_ = M_PI;
  }
}

icf_t aoa_estimator::steering(uint32_t a, double theta_rad) const
{
  // Element `a` is closer to the source by d_a·û, so its phase leads by +2*pi*(d_a·û)/lambda. This
  // sign MUST match the simulator's steer_gain() in sensing_channel.c; a flipped convention produces
  // a confidently mirrored bearing rather than an obviously broken one.
  const double proj = (double)array.ex[a] * std::cos(theta_rad) + (double)array.ey[a] * std::sin(theta_rad);
  const double ph   = TWO_PI * proj / array.lambda_m;
  return icf_t((float)std::cos(ph), (float)std::sin(ph));
}

void aoa_estimator::remove_clutter(const icf_t*   h_ant,
                                   uint32_t       nof_ant,
                                   uint32_t       nof_slow,
                                   uint32_t       row_stride,
                                   uint32_t       nof_subc,
                                   const uint8_t* occ)
{
  // work_ is COMPACTED to nof_slow rows per antenna, so everything downstream uses one stride.
  const size_t src_plane = (size_t)row_stride * nof_subc;
  const size_t plane     = (size_t)nof_slow * nof_subc;
  work_.assign((size_t)nof_ant * plane, icf_t(0.0f, 0.0f));

  // Per-subcarrier slow-time mean over the OCCUPIED rows only, subtracted from those rows. This is
  // the same static/zero-Doppler suppression the main pipeline's default does; unoccupied cells are
  // left at zero so they contribute nothing to the projections below (occupied-only, no gap-fill).
  for (uint32_t a = 0; a < nof_ant; a++) {
    const icf_t* src = h_ant + (size_t)a * src_plane;
    icf_t*       dst = work_.data() + (size_t)a * plane;
    for (uint32_t c = 0; c < nof_subc; c++) {
      icf_t    sum(0.0f, 0.0f);
      uint32_t n_occ = 0;
      for (uint32_t n = 0; n < nof_slow; n++) {
        if (occ == nullptr || occ[(size_t)n * nof_subc + c]) {
          sum += src[(size_t)n * nof_subc + c];
          n_occ++;
        }
      }
      if (n_occ == 0) {
        continue;
      }
      const icf_t mean = sum * (1.0f / (float)n_occ);
      for (uint32_t n = 0; n < nof_slow; n++) {
        if (occ == nullptr || occ[(size_t)n * nof_subc + c]) {
          dst[(size_t)n * nof_subc + c] = src[(size_t)n * nof_subc + c] - mean;
        }
      }
    }
  }
}

void aoa_estimator::range_project(uint32_t nof_ant, uint32_t nof_slow, uint32_t nof_subc, uint32_t range_bin)
{
  // Inner (range) sum, matching range_doppler's inverse transform convention so bin indices agree:
  //   rowsum[a][n] = sum_c H[a][n][c] * e^{+j2*pi*c*range_bin/nof_subc}
  // work_ is the clutter-removed grid, already compacted to nof_slow rows per antenna. Unoccupied
  // cells are exactly zero there and are skipped, which is what makes this affordable on the sparse,
  // irregularly-scheduled pdsch_data occupancy this receiver actually sees.
  const size_t plane = (size_t)nof_slow * nof_subc;
  rowsum_.assign((size_t)nof_ant * nof_slow, icf_t(0.0f, 0.0f));
  const double sr = TWO_PI * (double)range_bin / (double)nof_subc;
  for (uint32_t a = 0; a < nof_ant; a++) {
    const icf_t* g = work_.data() + (size_t)a * plane;
    for (uint32_t n = 0; n < nof_slow; n++) {
      const icf_t* row = g + (size_t)n * nof_subc;
      icf_t        rs(0.0f, 0.0f);
      for (uint32_t c = 0; c < nof_subc; c++) {
        if (row[c] == icf_t(0.0f, 0.0f)) {
          continue; // unoccupied (or clutter-cancelled to nothing): contributes nothing
        }
        const double ph = sr * (double)c;
        rs += row[c] * icf_t((float)std::cos(ph), (float)std::sin(ph));
      }
      rowsum_[(size_t)a * nof_slow + n] = rs;
    }
  }
}

void aoa_estimator::eval_cell(uint32_t nof_ant, uint32_t nof_slow, double f_d_norm, std::vector<icf_t>& z) const
{
  // Outer (slow-time) sum at the row's TRUE position -- the non-uniform DFT, exact even when the
  // scheduler's row spacing wanders. The Hann weighting suppresses Doppler sidelobes from
  // neighbouring targets; being a common REAL weighting it cannot bias the inter-antenna phase
  // (see isac_aoa.h).
  z.assign(nof_ant, icf_t(0.0f, 0.0f));
  for (uint32_t a = 0; a < nof_ant; a++) {
    const icf_t* rs = rowsum_.data() + (size_t)a * nof_slow;
    icf_t        acc(0.0f, 0.0f);
    for (uint32_t n = 0; n < nof_slow; n++) {
      const double phd = -TWO_PI * f_d_norm * row_t_[n];
      acc += rs[n] * slow_win_[n] * icf_t((float)std::cos(phd), (float)std::sin(phd));
    }
    z[a] = acc;
  }
}

bool aoa_estimator::bearing_interferometry(const std::vector<icf_t>& z, double& theta_rad) const
{
  // Exact closed form for two elements: the measured phase difference is 2*pi*(d·û)/lambda, and with
  // a two-element array d·û = |d|*cos(angle between d and û), so theta = axis ± acos(...).
  if (z.size() < 2) {
    return false;
  }
  const double dphi = std::arg(z[1] * std::conj(z[0]));
  const double dx   = (double)array.ex[1] - (double)array.ex[0];
  const double dy   = (double)array.ey[1] - (double)array.ey[0];
  const double d    = std::hypot(dx, dy);
  if (d < 1e-9) {
    return false;
  }
  double c = dphi * array.lambda_m / (TWO_PI * d);
  if (c > 1.0 || c < -1.0) {
    // Outside the visible region: either an ambiguity fold or noise. Clamp to endfire rather than
    // returning NaN, and let the caller's SNR gate decide whether to trust it.
    c = std::max(-1.0, std::min(1.0, c));
  }
  const double axis  = std::atan2(dy, dx);
  const double off   = std::acos(c);
  // Two solutions, mirrored about the array axis; pick the one inside the scan window.
  const double cand1 = wrap_pi(axis + off);
  const double cand2 = wrap_pi(axis - off);
  auto in_scan = [&](double t) {
    const double rel = wrap_pi(t - 0.5 * (scan_lo_ + scan_hi_));
    return std::fabs(rel) <= 0.5 * (scan_hi_ - scan_lo_) + 1e-9;
  };
  theta_rad = in_scan(cand1) ? cand1 : cand2;
  return true;
}

bool aoa_estimator::bearing_beamscan(const std::vector<icf_t>& z, double& theta_rad) const
{
  const double step = std::max(0.05, (double)args.aoa_scan_step_deg) * M_PI / 180.0;
  const int    n    = (int)std::ceil((scan_hi_ - scan_lo_) / step);
  if (n < 3) {
    return false;
  }
  int    best_i = -1;
  double best_p = -1.0;
  std::vector<double> pw((size_t)n + 1, 0.0);
  for (int i = 0; i <= n; i++) {
    const double th = scan_lo_ + step * (double)i;
    icf_t        acc(0.0f, 0.0f);
    for (uint32_t a = 0; a < z.size(); a++) {
      acc += z[a] * std::conj(steering(a, th));
    }
    const double p = (double)std::norm(acc);
    pw[(size_t)i]  = p;
    if (p > best_p) {
      best_p = p;
      best_i = i;
    }
  }
  if (best_i < 0) {
    return false;
  }
  double th = scan_lo_ + step * (double)best_i;
  // Parabolic refinement in dB (same reasoning as the RVM's subbin_interp: a mainlobe is much closer
  // to a parabola in log domain than in linear power).
  if (best_i > 0 && best_i < n) {
    const double lm = 10.0 * std::log10(std::max(pw[(size_t)best_i - 1], 1e-30));
    const double l0 = 10.0 * std::log10(std::max(pw[(size_t)best_i], 1e-30));
    const double lp = 10.0 * std::log10(std::max(pw[(size_t)best_i + 1], 1e-30));
    const double den = lm - 2.0 * l0 + lp;
    if (std::fabs(den) > 1e-12) {
      double d = 0.5 * (lm - lp) / den;
      d        = std::max(-0.5, std::min(0.5, d));
      th += d * step;
    }
  }
  theta_rad = wrap_pi(th);
  return true;
}

bool aoa_estimator::bearing_music(const std::vector<std::vector<icf_t>>& snapshots, double& theta_rad) const
{
  const uint32_t m = array.size();
  if (m < 3 || snapshots.size() < m) {
    return false; // one source needs at least a 1-D noise subspace, and enough snapshots to see it
  }
  // Sample covariance over the neighbourhood snapshots.
  std::vector<icf_t> R((size_t)m * m, icf_t(0.0f, 0.0f));
  for (const auto& s : snapshots) {
    for (uint32_t i = 0; i < m; i++) {
      for (uint32_t j = 0; j < m; j++) {
        R[(size_t)i * m + j] += s[i] * std::conj(s[j]);
      }
    }
  }
  const float inv = 1.0f / (float)snapshots.size();
  for (auto& v : R) {
    v *= inv;
  }

  // Dominant eigenvector by power iteration: for a single source that IS the signal subspace, and the
  // noise subspace is its orthogonal complement — so MUSIC's pseudo-spectrum reduces to
  // 1 / (|a|^2 - |<a, u>|^2 / |u|^2), needing only that one eigenvector. Avoids a full Hermitian
  // eigensolver for a 4x4 problem.
  std::vector<icf_t> u(m, icf_t(1.0f, 0.0f));
  std::vector<icf_t> t(m);
  for (int it = 0; it < 50; it++) {
    for (uint32_t i = 0; i < m; i++) {
      icf_t acc(0.0f, 0.0f);
      for (uint32_t j = 0; j < m; j++) {
        acc += R[(size_t)i * m + j] * u[j];
      }
      t[i] = acc;
    }
    float nrm = 0.0f;
    for (uint32_t i = 0; i < m; i++) {
      nrm += std::norm(t[i]);
    }
    nrm = std::sqrt(nrm);
    if (nrm < 1e-20f) {
      return false;
    }
    for (uint32_t i = 0; i < m; i++) {
      u[i] = t[i] * (1.0f / nrm);
    }
  }

  const double step = std::max(0.05, (double)args.aoa_scan_step_deg) * M_PI / 180.0;
  const int    n    = (int)std::ceil((scan_hi_ - scan_lo_) / step);
  double best_p = -1.0, best_th = 0.0;
  for (int i = 0; i <= n; i++) {
    const double th = scan_lo_ + step * (double)i;
    icf_t        proj(0.0f, 0.0f);
    double       a2 = 0.0;
    for (uint32_t a = 0; a < m; a++) {
      const icf_t s = steering(a, th);
      proj += std::conj(u[a]) * s;
      a2 += (double)std::norm(s);
    }
    const double noise = std::max(a2 - (double)std::norm(proj), 1e-12);
    const double p     = 1.0 / noise;
    if (p > best_p) {
      best_p  = p;
      best_th = th;
    }
  }
  theta_rad = wrap_pi(best_th);
  return true;
}

bool aoa_estimator::estimate_bearing(const std::vector<icf_t>& z, double& theta_rad, bool& mirror) const
{
  mirror = array.collinear;
  if (args.aoa_estimator == "interferometry" || array.size() == 2) {
    return bearing_interferometry(z, theta_rad);
  }
  return bearing_beamscan(z, theta_rad);
}

double aoa_estimator::bearing_sigma(double theta_rad, double snr_lin) const
{
  // Fisher information for theta from N phasors of common linear SNR:
  //   dphi_a/dtheta = (2*pi/lambda) * (d_a . u_perp),   u_perp = (-sin th, cos th)
  //   I(theta) = 2 * SNR * sum_a (g_a - mean(g))^2      (the mean drops out: a common phase is
  //                                                      unobservable, it is absorbed by the source)
  // so sigma = 1/sqrt(I). This is a CRB, i.e. a floor -- it is reported as such, not as a measured
  // spread, and it correctly blows up at endfire where (d . u_perp) stops varying.
  const double up_x = -std::sin(theta_rad);
  const double up_y = std::cos(theta_rad);
  const double k    = TWO_PI / array.lambda_m;
  double       sum = 0.0, sum2 = 0.0;
  const uint32_t m = array.size();
  for (uint32_t a = 0; a < m; a++) {
    const double g = k * ((double)array.ex[a] * up_x + (double)array.ey[a] * up_y);
    sum += g;
    sum2 += g * g;
  }
  const double var = sum2 - sum * sum / (double)m;
  if (var <= 1e-12 || snr_lin <= 0.0) {
    return M_PI; // no information: report a full-circle uncertainty rather than a fake small number
  }
  // Clamp to a half turn. The CRB genuinely diverges as the target approaches endfire (the phase
  // stops varying with bearing there), and an unclamped value is not wrong so much as useless --
  // a live run reported sigma = 312881 deg, which is both meaningless and liable to overflow any
  // consumer that squares it into a covariance. Beyond pi the estimate carries no information at
  // all, so pi is the honest ceiling.
  return std::min(M_PI, 1.0 / std::sqrt(2.0 * snr_lin * var));
}

bool aoa_estimator::calibrate_from_los(const icf_t*             h_ant,
                                       uint32_t                 nof_ant,
                                       uint32_t                 nof_slow,
                                       uint32_t                 row_stride,
                                       uint32_t                 nof_subc,
                                       const uint8_t*           occ,
                                       const double*            row_time_slots,
                                       double                   period_slots,
                                       const nr_isac_carrier_t& carrier,
                                       double                   known_bearing_rad)
{
  if (!args.aoa_selfcal || !array.usable() || nof_ant < array.size()) {
    return false;
  }
  (void)period_slots;
  (void)carrier;

  // The direct path is static (zero Doppler) and sits at the configured nominal differential range.
  // NOTE the ordering constraint: clutter removal SUBTRACTS the per-subcarrier slow-time mean, which
  // is exactly the direct path -- so the calibration must be evaluated on the RAW grid, before
  // remove_clutter(). That is why this takes h_ant rather than reusing work_.
  const size_t src_plane = (size_t)row_stride * nof_subc;
  const size_t plane     = (size_t)nof_slow * nof_subc;
  work_.assign((size_t)nof_ant * plane, icf_t(0.0f, 0.0f));
  for (uint32_t a = 0; a < nof_ant; a++) {
    std::memcpy(work_.data() + (size_t)a * plane, h_ant + (size_t)a * src_plane, plane * sizeof(icf_t));
  }
  (void)occ;

  row_t_.assign(row_time_slots, row_time_slots + nof_slow);

  const double range_res = (double)SPEED_OF_LIGHT / ((double)nof_subc * (double)carrier.scs_hz);
  int          los_bin   = (range_res > 0.0) ? (int)std::lround((double)args.nominal_los_range_m / range_res) : 0;
  los_bin                = std::max(0, std::min((int)nof_subc - 1, los_bin));

  slow_win_.assign(nof_slow, 1.0f);
  for (uint32_t n = 0; n < nof_slow; n++) {
    slow_win_[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)std::max(1u, nof_slow - 1)));
  }
  std::vector<icf_t> z;
  range_project(nof_ant, nof_slow, nof_subc, (uint32_t)los_bin);
  eval_cell(nof_ant, nof_slow, 0.0, z);

  // The direct path's expected inter-element phases are known from the surveyed illuminator bearing;
  // whatever is left over is the receiver's own per-channel phase/gain error.
  float ref_mag = std::abs(z[0]);
  if (!(ref_mag > 0.0f)) {
    return false;
  }
  for (uint32_t a = 0; a < array.size(); a++) {
    const icf_t expect = steering(a, known_bearing_rad) * z[0] / std::abs(z[0]);
    const icf_t meas   = z[a];
    if (std::abs(meas) < 1e-20f) {
      return false;
    }
    // Correction that maps the measured channel back onto the expected one, unit modulus (gain
    // errors are deliberately NOT corrected: an amplitude taper does not bias a phase-only
    // estimator, and inverting a badly-conditioned channel's gain would amplify its noise).
    const icf_t corr = expect / meas * (std::abs(meas) / std::abs(expect));
    cal_accum_[a] += corr / std::abs(corr);
  }
  cal_updates_++;
  for (uint32_t a = 0; a < array.size(); a++) {
    const float m = std::abs(cal_accum_[a]);
    cal_[a]       = (m > 1e-20f) ? cal_accum_[a] / m : icf_t(1.0f, 0.0f);
  }
  return true;
}

void aoa_estimator::process(const icf_t*                            h_ant,
                            uint32_t                                nof_ant,
                            uint32_t                                nof_slow,
                            uint32_t                                row_stride,
                            uint32_t                                nof_subc,
                            const uint8_t*                          occ,
                            const double*                           row_time_slots,
                            double                                  period_slots,
                            const nr_isac_carrier_t&                carrier,
                            const std::vector<sensing_detection_t>& dets,
                            std::vector<aoa_estimate_t>&            out)
{
  out.assign(dets.size(), aoa_estimate_t{});
  if (!array.usable() || h_ant == nullptr || nof_ant < array.size() || nof_slow < 2 || nof_subc < 2) {
    return;
  }
  (void)carrier;

  row_t_.assign(row_time_slots, row_time_slots + nof_slow);
  slow_win_.assign(nof_slow, 1.0f);
  for (uint32_t n = 0; n < nof_slow; n++) {
    slow_win_[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)std::max(1u, nof_slow - 1)));
  }
  remove_clutter(h_ant, nof_ant, nof_slow, row_stride, nof_subc, occ);

  const uint32_t m    = array.size();
  const int      srch = (int)args.aoa_cell_search_bins;
  // Doppler bin -> cycles per slot. The main pipeline's Doppler axis is a length-nof_slow transform of
  // rows spaced `period_slots` apart, fftshifted so bin `half` is zero Doppler.
  const int    half   = (int)(nof_slow / 2);
  const double df_bin = (period_slots > 0.0) ? 1.0 / (period_slots * (double)nof_slow) : 0.0;

  std::vector<icf_t>              z, zbest;
  std::vector<std::vector<icf_t>> snaps;

  for (size_t i = 0; i < dets.size(); i++) {
    const int r0 = (int)dets[i].range_bin;
    const int d0 = (int)dets[i].doppler_bin;

    // Re-peak over a small neighbourhood. The detections come from the main pipeline's grid, which may
    // carry sync corrections and sub-bin interpolation this module deliberately does not reproduce
    // (see isac_aoa.h); a couple of bins of slack absorbs that without pretending to re-detect.
    double best_p = -1.0;
    snaps.clear();
    for (int dr = -srch; dr <= srch; dr++) {
      const int rb = r0 + dr;
      if (rb < 0 || rb >= (int)nof_subc) {
        continue;
      }
      // ONE range projection per candidate range bin, reused across every candidate Doppler bin.
      range_project(m, nof_slow, nof_subc, (uint32_t)rb);
      for (int dd = -srch; dd <= srch; dd++) {
        const int db = d0 + dd;
        if (db < 0 || db >= (int)nof_slow) {
          continue;
        }
        // Never let the re-peak wander into the direct-path guard region. MEASURED failure, not a
        // precaution: in the 2026-07-26 rx1 run two detections adjacent to the LOS skirt re-peaked
        // onto the direct-path leakage cell and reported the ILLUMINATOR's bearing (~180 deg),
        // producing the only two gross outliers in an otherwise 0.35 deg-median set. These are
        // exactly the cells CFAR itself is forbidden to detect in, so a re-peak has no business
        // preferring one. The detection's OWN cell is always admissible -- sub-bin interpolation can
        // round a legitimate detection's reported bin into the guard, and dropping it there would be
        // a regression rather than a fix.
        // Guard semantics are copied verbatim from range_doppler.cc's notch (inclusive `<=`, and the
        // wrap-around range guard at the top of the axis) so the admissible set here is EXACTLY the
        // set CFAR was allowed to detect in -- an off-by-one either way would silently re-admit the
        // cell this exists to exclude, or drop a legitimate neighbour.
        if (!(dr == 0 && dd == 0)) {
          const bool zero_range = rb <= (int)args.zero_range_guard
                                  || rb >= (int)nof_subc - 1 - (int)args.zero_range_guard;
          const bool zero_dopp  = std::abs(db - half) <= (int)args.zero_doppler_guard;
          if (zero_range || zero_dopp) {
            continue;
          }
        }
        const double fd = ((double)(db - half)) * df_bin;
        eval_cell(m, nof_slow, fd, z);
        // Apply the self-calibration before anything reads a phase from these.
        for (uint32_t a = 0; a < m; a++) {
          z[a] *= cal_[a];
        }
        snaps.push_back(z);
        const double p = (double)std::norm(z[0]);
        if (p > best_p) {
          best_p = p;
          zbest  = z;
        }
      }
    }
    if (best_p <= 0.0 || zbest.size() < m) {
      continue;
    }

    double theta = 0.0;
    bool   mirror = false;
    bool   ok     = false;
    if (args.aoa_estimator == "music") {
      ok     = bearing_music(snaps, theta);
      mirror = array.collinear;
      if (!ok) {
        ok = estimate_bearing(zbest, theta, mirror); // fall back rather than dropping the detection
      }
    } else {
      ok = estimate_bearing(zbest, theta, mirror);
    }
    if (!ok) {
      continue;
    }

    // Per-element SNR for the sigma estimate. The detection's own snr_db is the CFAR cell SNR on the
    // primary antenna's map, which is the same quantity to within the (common) processing gain.
    const double snr_lin = std::pow(10.0, (double)dets[i].snr_db / 10.0);
    if (dets[i].snr_db < args.aoa_min_snr_db) {
      continue; // too weak to trust a phase from: leave azimuth absent rather than emit noise
    }
    out[i].azimuth_deg     = (float)(wrap_pi(theta) * 180.0 / M_PI);
    out[i].azimuth_std_deg = (float)(bearing_sigma(theta, snr_lin) * 180.0 / M_PI);
    out[i].snr_db          = dets[i].snr_db;
    out[i].mirror_ambiguous = mirror;
    out[i].valid           = true;
  }
}

} // namespace nr_isac
