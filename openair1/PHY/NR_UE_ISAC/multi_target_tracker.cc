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

#include "multi_target_tracker.h"

#include <algorithm>
#include <cmath>

namespace nr_isac {

namespace {
inline uint32_t popcount(uint32_t v) { return (uint32_t)__builtin_popcount(v); }

constexpr double DET_EWMA_ALPHA = 0.2; // same order as track_nis_ewma_alpha/SFO_EMA_ALPHA elsewhere

/// P(X >= M) for X ~ Binomial(N, p), via the forward pmf recurrence pmf(k+1) = pmf(k)*(N-k)/(k+1)*p/(1-p).
double binomial_survival(uint32_t N, double p, uint32_t M)
{
  if (M == 0) return 1.0;
  if (M > N) return 0.0;
  if (p <= 0.0) return 0.0;
  if (p >= 1.0) return 1.0;
  double sum = 0.0;
  double pmf = std::pow(1.0 - p, (double)N);
  for (uint32_t k = 0; k <= N; k++) {
    if (k >= M) sum += pmf;
    if (k < N) pmf *= ((double)(N - k) / (double)(k + 1)) * (p / (1.0 - p));
  }
  return std::min(1.0, sum);
}

/// Smallest M in [1,N] such that binomial_survival(N,p,M) <= target_pfa; N if none qualify.
uint32_t solve_confirm_m(double p, uint32_t N, double target_pfa)
{
  p = std::max(0.0, std::min(1.0, p));
  for (uint32_t M = 1; M <= N; M++) {
    if (binomial_survival(N, p, M) <= target_pfa) {
      return M;
    }
  }
  return N;
}
} // namespace

void multi_target_tracker::reset()
{
  slots_.clear();
  out_.clear();
  next_id_ = 1;
}

const std::vector<sensing_track_t>&
multi_target_tracker::update(const std::vector<sensing_detection_t>& detections, double dt_s,
                              const sensing_rvm_t* rvm)
{
  const int T = (int)slots_.size();
  const int D = (int)detections.size();

  // ---- 0. Track the measured false-alarm density (EWMA of raw detections/CPI) and derive this
  // CPI's effective M-of-N confirmation threshold. See the file/header comment: track_confirm_m==0
  // means AUTO -- solve for the smallest M such that a random/incoherent false-alarm stream would
  // confirm a spurious track within N CPIs with probability <= track_confirm_target_pfa, using the
  // association gate's area as a fraction of the range-Doppler grid area. Falls back to a fixed
  // value when no grid geometry is available yet (e.g. an offline test that doesn't pass rvm).
  mean_det_ewma_ = have_mean_det_ ? ((1.0 - DET_EWMA_ALPHA) * mean_det_ewma_ + DET_EWMA_ALPHA * (double)D)
                                  : (double)D;
  have_mean_det_ = true;

  const uint32_t N = std::max(1u, std::min(args.track_confirm_n, 32u));
  uint32_t       M;
  if (args.track_confirm_m > 0) {
    M            = args.track_confirm_m; // manual pin, bypasses auto-derivation entirely
    last_p_hit_  = -1.0;
  } else if (rvm != nullptr && rvm->range_res_m > 0.0f && rvm->vel_res_mps > 0.0f &&
             rvm->nof_range_bins > 0 && rvm->nof_doppler_bins > 0) {
    const double gate_sq   = (double)args.track_assoc_gate_sigma * (double)args.track_assoc_gate_sigma * 2.0;
    const double sr        = std::sqrt((double)args.track_r_var_m2);
    const double sv        = std::sqrt((double)args.track_rv_var_m2s2);
    const double gate_area = M_PI * gate_sq * sr * sv; // ellipse (dr/sr)^2+(dv/sv)^2 <= gate_sq
    const double grid_area = (double)rvm->nof_range_bins * (double)rvm->range_res_m *
                              (double)rvm->nof_doppler_bins * (double)rvm->vel_res_mps;
    const double p_hit     = (grid_area > 0.0) ? std::min(1.0, mean_det_ewma_ * gate_area / grid_area) : 1.0;
    last_p_hit_            = p_hit;
    M                      = solve_confirm_m(p_hit, N, (double)args.track_confirm_target_pfa);
  } else {
    M           = std::min(N, 3u); // no grid geometry known yet -- safe fixed fallback
    last_p_hit_ = -1.0;
  }
  last_confirm_m_ = M;
  const uint32_t n_mask = (N >= 32u) ? 0xFFFFFFFFu : ((1u << N) - 1u);

  // ---- 1. Global greedy association: predicted range/rate of each existing track vs. each detection.
  // Cost is a normalized (Mahalanobis-like) 2-D distance in (range, range-rate). Prediction uses each
  // track's OWN velocity propagated by dt, so tracks that momentarily share a range still separate by
  // their differing range-rate -- this is what preserves identity through a range crossing.
  const double gate_sq = (double)args.track_assoc_gate_sigma * (double)args.track_assoc_gate_sigma * 2.0;

  struct pair_t { double cost; int s; int d; };
  std::vector<pair_t> pairs;
  pairs.reserve((size_t)T * (size_t)D);
  for (int s = 0; s < T; s++) {
    const sensing_track_t& lt = slots_[(size_t)s].filter.last();
    if (!lt.active) {
      continue; // dropped filter awaiting prune; do not associate
    }
    const double dt      = (dt_s > 0.0) ? dt_s : 0.0;
    const double r_pred  = (double)lt.range_m + (double)lt.range_rate_mps * dt;
    const double v_pred  = (double)lt.range_rate_mps;
    // Gate radius floored at the baseline measurement sigma (so a fresh/tight track isn't
    // over-gated) and CEILINGED at track_assoc_gate_max_sr_m/sv_mps -- independent of how far
    // lt.sigma_range_m has grown from adaptive-q inflation during a coast run. Without the ceiling,
    // a coasting track's association gate can widen enough to scoop up unrelated scattered CFAR
    // noise as if it were a re-detection of the same target (live-confirmed 2026-07-24, see
    // defs_nr_UE_ISAC.h's track_assoc_gate_max_sr_m comment) -- this bounds the ROUTING decision
    // only; the per-track Kalman filter's own internal P/update-gate is untouched.
    const double sr = std::min((double)args.track_assoc_gate_max_sr_m,
                                std::max((double)lt.sigma_range_m, std::sqrt((double)args.track_r_var_m2)));
    const double sv = std::min((double)args.track_assoc_gate_max_sv_mps,
                                std::max(std::sqrt((double)args.track_rv_var_m2s2), 0.5));
    for (int d = 0; d < D; d++) {
      const double dr = (double)detections[(size_t)d].range_m - r_pred;
      const double dv = (double)detections[(size_t)d].vel_mps - v_pred;
      const double c  = (dr / sr) * (dr / sr) + (dv / sv) * (dv / sv);
      if (c <= gate_sq) {
        pairs.push_back({c, s, d});
      }
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const pair_t& a, const pair_t& b) { return a.cost < b.cost; });

  std::vector<int>  det_of_slot((size_t)T, -1);
  std::vector<char> det_used((size_t)D, 0);
  for (const pair_t& p : pairs) {
    if (det_of_slot[(size_t)p.s] == -1 && !det_used[(size_t)p.d]) {
      det_of_slot[(size_t)p.s] = p.d;
      det_used[(size_t)p.d]    = 1;
    }
  }

  // ---- 2. Advance each existing track with its assigned detection (or empty => coast), and update
  // its M-of-N confirmation window from whether the per-track filter actually accepted the detection.
  // N, M, n_mask were already derived in step 0 (auto or manual).
  for (int s = 0; s < T; s++) {
    slot_t& sl = slots_[(size_t)s];
    std::vector<sensing_detection_t> one;
    if (det_of_slot[(size_t)s] >= 0) {
      one.push_back(detections[(size_t)det_of_slot[(size_t)s]]);
    }
    const sensing_track_t st = sl.filter.update(one, dt_s);
    sl.age_cpis++;
    sl.hit_history = ((sl.hit_history << 1) | (st.updated ? 1u : 0u)) & n_mask;
    if (!sl.confirmed && popcount(sl.hit_history) >= M) {
      sl.confirmed = true;
    }
    // Flicker (power-continuity) metric: on an ACCEPTED detection, fold |Δsnr_dB| vs the previous
    // accepted SNR into the jitter EWMA. Real reflectors move smoothly in power; amplitude-gating
    // harmonic ghosts swing hard with the instantaneous slot occupancy. Coasts don't update it (no
    // new power sample); gross dropout is already handled by M-of-N.
    if (st.updated && det_of_slot[(size_t)s] >= 0) {
      const float snr = detections[(size_t)det_of_slot[(size_t)s]].snr_db;
      if (sl.have_snr_prev) {
        const float a = args.track_flicker_ewma_alpha;
        const float j = std::fabs(snr - sl.snr_prev_db);
        sl.snr_jitter_ewma = (sl.snr_updates == 0) ? j : ((1.0f - a) * sl.snr_jitter_ewma + a * j);
        sl.snr_updates++;
      }
      sl.snr_prev_db   = snr;
      sl.have_snr_prev = true;
    }
  }

  // ---- 3. Prune tracks the per-track filter dropped (max_coast) and tentative tracks that failed to
  // confirm within the N-CPI window. A confirmed track lives until ITS filter drops it. ----
  std::vector<slot_t> keep;
  keep.reserve(slots_.size());
  for (slot_t& sl : slots_) {
    if (!sl.filter.last().active) {
      continue; // per-track filter self-dropped (target gone past max_coast)
    }
    if (!sl.confirmed && sl.age_cpis >= N && popcount(sl.hit_history) < M) {
      continue; // tentative track never reached M-of-N -> spurious, drop
    }
    keep.push_back(std::move(sl));
  }
  slots_ = std::move(keep);

  // ---- 4. Track initiation: seed a NEW tentative track on each detection not associated to any
  // existing track, up to the max_tracks cap. A single seeding hit is not enough to be reported --
  // the M-of-N gate above decides that over subsequent CPIs. ----
  for (int d = 0; d < D; d++) {
    if (det_used[(size_t)d]) {
      continue;
    }
    if (slots_.size() >= (size_t)std::max(1u, args.track_max_tracks)) {
      break;
    }
    slots_.emplace_back(next_id_++, args);
    slot_t& sl = slots_.back();
    // dt_s = 0 -> target_tracker treats the first update as an initialisation on this detection.
    const sensing_track_t st = sl.filter.update({detections[(size_t)d]}, 0.0);
    sl.age_cpis    = 1;
    sl.hit_history = st.updated ? 1u : 0u;
    if (M <= 1u && popcount(sl.hit_history) >= M) {
      sl.confirmed = true; // confirm_m=1 => legacy "confirm on first hit"
    }
  }

  // ---- 5. Emit confirmed, active tracks with their stable ids. A confirmed track whose SNR-jitter
  // EWMA (see step 2) exceeds track_flicker_max_db -- once it has enough power samples to judge -- is
  // withheld as a flickering amplitude-gating ghost (track_flicker_reject). Report-level only: the
  // slot stays alive (keeps its association/M-of-N state) and can re-enter the report if its power
  // trajectory settles. ----
  out_.clear();
  for (const slot_t& sl : slots_) {
    if (!sl.confirmed) {
      continue;
    }
    sensing_track_t t = sl.filter.last();
    if (!t.active) {
      continue;
    }
    if (args.track_flicker_reject && sl.snr_updates >= args.track_flicker_min_updates &&
        sl.snr_jitter_ewma > args.track_flicker_max_db) {
      continue; // flickering power trajectory -> treat as a gated-harmonic ghost, don't report
    }
    t.track_id = sl.id;
    out_.push_back(t);
  }

  // ---- 6. Track-level Doppler-harmonic suppression (opt-in): drop a confirmed track whose
  // |range-rate| is a near-integer multiple (>=2) of another confirmed track's at essentially the
  // same range -- the scheduling-driven slow-time harmonic of a real target that leaked past the
  // per-CPI detection filter and confirmed. The lower-|velocity| member is the fundamental and is
  // kept. See defs_nr_UE_ISAC.h's track_harmonic_reject comment. ----
  if (args.track_harmonic_reject && out_.size() > 1) {
    const double maxk = (double)args.harmonic_max_k;
    const double tol  = (double)args.harmonic_tol;
    std::vector<sensing_track_t> kept;
    kept.reserve(out_.size());
    for (const sensing_track_t& cand : out_) {
      const double vc = std::abs((double)cand.range_rate_mps);
      bool is_harm = false;
      for (const sensing_track_t& other : out_) {
        if (&other == &cand) {
          continue;
        }
        const double vo = std::abs((double)other.range_rate_mps);
        // `other` must be the lower-|velocity| fundamental at the same range.
        if (vo < 1.0 || vc <= vo ||
            std::abs((double)cand.range_m - (double)other.range_m) > (double)args.track_harmonic_range_m) {
          continue;
        }
        const double ratio = vc / vo;
        const double krnd  = std::round(ratio);
        if (krnd >= 2.0 && krnd <= maxk && std::abs(ratio - krnd) <= tol) {
          is_harm = true;
          break;
        }
      }
      if (!is_harm) {
        kept.push_back(cand);
      }
    }
    out_.swap(kept);
  }
  return out_;
}

} // namespace nr_isac
