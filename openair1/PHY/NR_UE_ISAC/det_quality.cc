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

/*! \file openair1/PHY/NR_UE_ISAC/det_quality.cc
 * \brief Adaptive per-detection quality — implementation. See det_quality.h for the model and the
 *        measured results. Mirrors tests/sensing_sim/proto_det_quality.py, which is where the
 *        algorithm was validated against ground truth before this was written.
 */

#include "det_quality.h"

#include <algorithm>
#include <cmath>

namespace nr_isac {

namespace {

/// Tracking rate for every online estimate. Slow enough to average several CPIs, fast enough to
/// follow a gain or traffic change within a few seconds of sensing time.
constexpr double DQ_EMA = 0.15;
/// Floor on the real/false-alarm separation, in null sigmas. Without it the two mixture components
/// can collapse onto each other on a CPI run with no real targets, after which nothing is ever
/// admitted again — a degenerate fixed point, not a tuning parameter.
constexpr double DQ_MIN_MU_R = 0.75;
/// Floor on the estimated null spread, in dB. NOT a tuning knob -- it is a statement about what an
/// SNR estimate can mean: a detection SNR is never meaningful to better than a fraction of a dB, so a
/// MAD below this is a degenerate estimate (e.g. a CPI whose detections happen to share one SNR), not
/// real information. Without it, `z = (snr - med)/mad` explodes and every posterior saturates at 0 or
/// 1 on a population that actually carries no discriminating information at all.
constexpr double DQ_MIN_MAD_DB = 0.5;

double median_of(std::vector<double>& v)
{
  if (v.empty()) {
    return 0.0;
  }
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  double m = v[mid];
  if (v.size() % 2 == 0) {
    // Even count: average the two central order statistics, so the estimate is not biased by which
    // side nth_element happened to leave the partition on.
    std::nth_element(v.begin(), v.begin() + mid - 1, v.end());
    m = 0.5 * (m + v[mid - 1]);
  }
  return m;
}

} // namespace

det_quality::det_quality(float cost_ratio)
{
  const double cr = (cost_ratio > 0.0f && std::isfinite(cost_ratio)) ? (double)cost_ratio : 1.0;
  boundary_       = (float)(cr / (1.0 + cr));
  for (uint32_t i = 0; i <= DQ_PERSIST_WINDOW; i++) {
    pc_real_[i] = 1.0; // Laplace smoothing: one pseudo-count per class per bin
    pc_fa_[i]   = 1.0;
  }
}

uint32_t det_quality::persistence_of(uint32_t range_bin, uint32_t dopp_bin,
                                     double drift_bins_per_cpi) const
{
  // MOTION-COMPENSATED. A static cell test silently penalises fast targets: a target moving several
  // range bins per CPI never matches its OWN previous detection, scores persistence 0, and is then
  // treated exactly like noise -- the opposite of the truth. The detection's own Doppler already says
  // how fast its range is changing, so where it was k CPIs ago is predictable from information
  // already in hand, with nothing to tune.
  uint32_t n = 0;
  uint32_t k = 0;
  for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
    k++;
    const double pred_r = (double)range_bin - (double)k * drift_bins_per_cpi;
    for (const cell_t& c : *it) {
      if (std::fabs((double)c.r - pred_r) <= (double)DQ_PERSIST_RANGE_BINS
          && std::abs((int)c.d - (int)dopp_bin) <= DQ_PERSIST_DOPP_BINS) {
        n++;
        break; // at most one hit per previous CPI
      }
    }
  }
  return n;
}

void det_quality::score(const std::vector<sensing_detection_t>& dets, float range_res_m,
                        double cpi_dt_s, std::vector<float>& p_real)
{
  p_real.assign(dets.size(), 0.0f);
  const size_t n = dets.size();
  if (n == 0) {
    recent_.push_back({});
    while (recent_.size() > DQ_PERSIST_WINDOW) {
      recent_.pop_front();
    }
    return;
  }

  // --- 1. robust null from THIS CPI, EMA'd across CPIs (see the header's model section) ----------
  std::vector<double> snr(n);
  for (size_t i = 0; i < n; i++) {
    snr[i] = (double)dets[i].snr_db;
  }
  std::vector<double> tmp = snr;
  const double        med_c = median_of(tmp);
  for (size_t i = 0; i < n; i++) {
    tmp[i] = std::fabs(snr[i] - med_c);
  }
  const double mad_c = std::max(median_of(tmp) * 1.4826, DQ_MIN_MAD_DB);

  if (!have_null_) {
    med_       = med_c;
    mad_       = mad_c;
    have_null_ = true;
  } else {
    med_ = (1.0 - DQ_EMA) * med_ + DQ_EMA * med_c;
    mad_ = (1.0 - DQ_EMA) * mad_ + DQ_EMA * mad_c;
  }
  const double mad = std::max(mad_, DQ_MIN_MAD_DB);

  // --- 2-6. per-detection posterior --------------------------------------------------------------
  const double mu = std::max(mu_r_, DQ_MIN_MU_R);
  const double vr = std::max(var_r_, 0.25);
  const double v0 = std::max(var_0_, 0.25);
  const double rres = (range_res_m > 0.0f) ? (double)range_res_m : 1.0;
  double pc_real_tot = 0.0, pc_fa_tot = 0.0;
  for (uint32_t i = 0; i <= DQ_PERSIST_WINDOW; i++) {
    pc_real_tot += pc_real_[i];
    pc_fa_tot += pc_fa_[i];
  }
  const double logit_prior =
      std::log(std::max(prior_, 1e-3) / std::max(1.0 - prior_, 1e-3));

  std::vector<double>   z(n);
  std::vector<uint32_t> pers(n);
  for (size_t i = 0; i < n; i++) {
    z[i] = (snr[i] - med_) / mad;
    // Where this detection was in previous CPIs, from its OWN measured range-rate.
    const double drift = (double)dets[i].vel_mps * cpi_dt_s / rres;
    pers[i]            = persistence_of(dets[i].range_bin, dets[i].doppler_bin, drift);

    // log N(z; mu, var_r) - log N(z; 0, var_0), both variances learned.
    const double ll_ratio = (-0.5 * std::log(vr) - 0.5 * (z[i] - mu) * (z[i] - mu) / vr)
                            - (-0.5 * std::log(v0) - 0.5 * z[i] * z[i] / v0);
    const double lr_pers  = std::log(pc_real_[pers[i]] / pc_real_tot)
                           - std::log(pc_fa_[pers[i]] / pc_fa_tot);
    const double logit = std::min(30.0, std::max(-30.0, ll_ratio + lr_pers + logit_prior));
    p_real[i]          = (float)(1.0 / (1.0 + std::exp(-logit)));
  }

  // --- online updates, using this CPI's own posteriors as soft labels -----------------------------
  double w_sum = 0.0, wz_sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    w_sum += (double)p_real[i];
    wz_sum += (double)p_real[i] * z[i];
  }
  // Only learn from a CPI that actually saw both classes; otherwise the estimates would chase a
  // degenerate all-real or all-false-alarm batch.
  if (w_sum > 0.5 && ((double)n - w_sum) > 0.5) {
    const double mu_obs = wz_sum / w_sum;
    double       vr_num = 0.0, v0_num = 0.0, v0_den = 0.0;
    for (size_t i = 0; i < n; i++) {
      const double w = (double)p_real[i];
      vr_num += w * (z[i] - mu_obs) * (z[i] - mu_obs);
      v0_num += (1.0 - w) * z[i] * z[i];
      v0_den += (1.0 - w);
    }
    var_0_ = (1.0 - DQ_EMA) * var_0_ + DQ_EMA * std::max(v0_num / std::max(v0_den, 1e-9), 0.25);
    // var_r >= var_0: identifiability, not tuning (see the header). Without it the EM collapses.
    var_r_ = (1.0 - DQ_EMA) * var_r_ + DQ_EMA * std::max(vr_num / w_sum, var_0_);
    mu_r_  = (1.0 - DQ_EMA) * mu_r_ + DQ_EMA * std::max(mu_obs, DQ_MIN_MU_R);
    prior_ = std::min(0.95, std::max(0.02, (1.0 - DQ_EMA) * prior_ + DQ_EMA * (w_sum / (double)n)));
    for (size_t i = 0; i < n; i++) {
      pc_real_[pers[i]] += (double)p_real[i];
      pc_fa_[pers[i]] += 1.0 - (double)p_real[i];
    }

    // P_D, receiver-side (see det_quality.h::detection_rate for why the central node cannot estimate
    // this for itself without a self-reinforcing loop). Persistence / WINDOW is a per-CPI detection
    // rate; weighting by p_real keeps false alarms -- which do not persist -- from dragging it down.
    // Only meaningful once the persistence window has actually filled, otherwise every detection
    // scores an artificially low persistence simply because there is no history yet.
    if (recent_.size() >= DQ_PERSIST_WINDOW) {
      double pd_num = 0.0;
      for (size_t i = 0; i < n; i++) {
        pd_num += (double)p_real[i] * ((double)pers[i] / (double)DQ_PERSIST_WINDOW);
      }
      const double obs = pd_num / w_sum;
      p_detect_ = (p_detect_ < 0.0) ? obs : (1.0 - DQ_EMA) * p_detect_ + DQ_EMA * obs;
    }
  }

  std::vector<cell_t> cells;
  cells.reserve(n);
  for (const sensing_detection_t& d : dets) {
    cells.push_back({d.range_bin, d.doppler_bin});
  }
  recent_.push_back(std::move(cells));
  while (recent_.size() > DQ_PERSIST_WINDOW) {
    recent_.pop_front();
  }
}

} // namespace nr_isac
