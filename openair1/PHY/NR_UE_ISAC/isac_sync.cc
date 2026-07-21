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

#include "isac_sync.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

extern "C" {
#include "common/utils/LOG/log.h"
}

namespace nr_isac {

namespace {

constexpr double SPEED_OF_LIGHT = 299792458.0;

// Matches the diagnosed fixed group-delay LOS tap (~bin 6 @ 8.16 m/bin on the fused 51-PRB grid,
// per CLAUDE.md's resolved LOS/CFO artifact note). Used only to seed the per-row peak search window
// -- range_res is invariant to a row's native comb (see process()), so this nominal bin is the same
// integer for every row regardless of source/comb.
constexpr double NOMINAL_LOS_RANGE_M = 49.0;

// Half-width (in bins) of the window around the nominal bin searched for each row's own peak.
constexpr uint32_t SEARCH_HALFWIN_BINS = 4;

// Shortest compacted CIR (in samples) trusted for a windowed peak search + parabolic fit. Below this
// a row is skipped rather than risking a peak search with no real margin around the nominal bin.
constexpr uint32_t MIN_CIR_LEN = 8;

// Minimum valid rows required to attempt a CPI-wide line fit / STO-vs-SFO classification.
constexpr uint32_t MIN_VALID_ROWS = 8;

// Total drift over the CPI's time span, in range bins, below which the per-row sub-bin delay is
// classified "constant" (fine STO -- safe to null). At/above this a real SFO is leaking through and
// must NOT be corrected here (Phase 3's job) -- ties to the acceptance criterion's "fraction of one
// range bin" residual tolerance.
constexpr double DRIFT_BIN_THRESHOLD = 0.25;

// Phase 2 (CFO): minimum valid LOS-tap rows to attempt the unwrap + line fit. Lower than Phase 1's
// MIN_VALID_ROWS since a phase-vs-time line has only 2 unknowns (slope, intercept) and each row
// contributes one scalar (vs. Phase 1 needing search-window margin per row).
constexpr uint32_t CFO_MIN_VALID_ROWS = 4;

// Alpha-beta tracker gains for the slow cross-CPI CFO estimate (diagnostic; not fed into this
// phase's own correction). dt is 1 CPI tick, not elapsed seconds -- see cpi_cfo_tracker::process().
// Starting defaults; tune in Phase 6a's sweep against a known injected CFO/drift rate.
constexpr double CFO_ALPHA = 0.3;
constexpr double CFO_BETA  = 0.05;

// Phase 3 (SFO): half-width (in bins) of the per-row tracking window, re-centered on the PREVIOUS
// accepted row's own peak rather than a fixed nominal bin (see cpi_sfo_tracker's class comment).
// Wider than Phase 1's SEARCH_HALFWIN_BINS since it must additionally tolerate row-to-row peak
// wobble/noise on top of genuine per-step drift, with no "known stable location" prior beyond the
// very first row.
constexpr uint32_t SFO_TRACK_HALFWIN_BINS = 10;

// Minimum {time, delay} pairs (post-ISI-exclusion) to attempt the SFO line fit.
constexpr uint32_t SFO_MIN_VALID_ROWS = 8;

// Minimum same-comb rows observed before the running ISI baseline is trusted enough to flag
// anomalies; below this every row in that comb group is accepted (building up the baseline).
constexpr uint32_t SFO_ISI_MIN_GROUP_ROWS = 3;

// A row is excluded as ISI-contaminated if its out-of-window CIR energy exceeds its comb group's
// running mean by more than this many standard deviations. Starting default; tune in Phase 6a.
constexpr double SFO_ISI_SIGMA = 4.0;

// This build's actual RF sample rate (USRP B210, per CLAUDE.md's live-testbed config) -- used only
// to additionally express the fitted (dimensionless) ppm figure as an absolute Hz clock error for
// human-readable logging; the ppm value itself does not depend on this constant.
constexpr double SAMPLE_RATE_HZ = 23.04e6;

// Nominal LOS bin, shared between Phase 1 (fixed-window search) and Phase 3 (seeds its first row's
// tracking window from the same location). See NOMINAL_LOS_RANGE_M above for why this is the same
// integer regardless of a row's native comb.
uint32_t nominal_los_bin(uint32_t nof_subc, double scs_hz)
{
  if (nof_subc == 0 || scs_hz <= 0.0) {
    return 0;
  }
  const double bin_to_delay_s = 1.0 / ((double)nof_subc * scs_hz);
  const double range_res_m    = SPEED_OF_LIGHT * bin_to_delay_s / 2.0;
  return (uint32_t)std::lround(NOMINAL_LOS_RANGE_M / range_res_m);
}

} // namespace

fft_plan* row_cir_builder::plan_for(uint32_t m)
{
  for (auto& kv : plan_cache_) {
    if (kv.first == m) {
      return kv.second.get();
    }
  }
  plan_cache_.emplace_back(m, std::unique_ptr<fft_plan>(new fft_plan(m, true /* inverse, matches
                                                                        range_doppler's range IFFT */)));
  return plan_cache_.back().second.get();
}

bool row_cir_builder::build(const icf_t* row, const uint8_t* mask, uint32_t nof_subc, uint32_t comb,
                            std::vector<icf_t>& cir)
{
  if (comb == 0) {
    return false;
  }

  int first = -1, last = -1;
  for (uint32_t c = 0; c < nof_subc; c++) {
    if (mask[c]) {
      if (first < 0) {
        first = (int)c;
      }
      last = (int)c;
    }
  }
  if (first < 0) {
    return false;
  }

  const uint32_t m = (uint32_t)((last - first) / (int)comb) + 1;
  if (m < MIN_CIR_LEN) {
    return false;
  }

  // Gather the row's real samples at their native comb stride. If a nominal position isn't actually
  // occupied (a rare same-slot multi-source merge where two combs overlap inconsistently), bail on
  // this row rather than IFFT a signal that isn't truly uniformly sampled.
  compact_.resize(m);
  for (uint32_t i = 0; i < m; i++) {
    const uint32_t c = (uint32_t)first + i * comb;
    if (c >= nof_subc || !mask[c]) {
      return false;
    }
    compact_[i] = row[c];
  }

  cir.resize(m);
  plan_for(m)->run(compact_.data(), cir.data());
  return true;
}

bool cpi_sto_tracker::estimate_row(const icf_t*         row,
                                   const uint8_t*       mask,
                                   uint32_t             nof_subc,
                                   uint32_t             comb,
                                   uint32_t             nominal_bin,
                                   los_row_estimate_t&  out)
{
  if (!cir_builder_.build(row, mask, nof_subc, comb, cir_)) {
    return false;
  }
  const uint32_t m = (uint32_t)cir_.size();

  if (m < 2 * SEARCH_HALFWIN_BINS + 3) {
    return false; // too short to window-search around the nominal bin with parabola margin
  }
  uint32_t lo = (nominal_bin > SEARCH_HALFWIN_BINS) ? nominal_bin - SEARCH_HALFWIN_BINS : 1;
  uint32_t hi = std::min(nominal_bin + SEARCH_HALFWIN_BINS, m - 2);
  if (lo < 1) {
    lo = 1;
  }
  if (hi <= lo) {
    return false;
  }

  uint32_t peak     = lo;
  double   peak_pow = std::norm(cir_[lo]);
  for (uint32_t b = lo + 1; b <= hi; b++) {
    const double p = std::norm(cir_[b]);
    if (p > peak_pow) {
      peak_pow = p;
      peak     = b;
    }
  }

  // Parabolic interpolation across the 3 power samples straddling the peak for a sub-bin estimate.
  const double y_m1  = std::norm(cir_[peak - 1]);
  const double y0    = std::norm(cir_[peak]);
  const double y_p1  = std::norm(cir_[peak + 1]);
  const double denom = y_m1 - 2.0 * y0 + y_p1;
  double       delta = 0.0;
  if (denom != 0.0) {
    delta = std::clamp(0.5 * (y_m1 - y_p1) / denom, -0.5, 0.5);
  }

  out.peak_bin = peak;
  out.frac_bin = delta;
  out.peak_val = cir_[peak]; // pre-correction complex sample; Phase 2 reads its phase for CFO/CPE
  return true;
}

sto_fit_result_t cpi_sto_tracker::process(icf_t*                   h_cpi,
                                          const uint8_t*           occ_all,
                                          uint32_t                 cpi_rows,
                                          uint32_t                 nof_subc,
                                          const uint32_t*          row_comb,
                                          const double*            row_time_slots,
                                          const nr_isac_carrier_t& carrier)
{
  sto_fit_result_t fit;
  rows_.assign(cpi_rows, los_row_estimate_t());

  if (cpi_rows == 0 || nof_subc == 0 || carrier.scs_hz == 0) {
    return fit;
  }

  const uint32_t nominal_bin = nominal_los_bin(nof_subc, carrier.scs_hz);

  const double slots_per_sf = std::max(1.0, (double)carrier.scs_hz / 15000.0);
  const double slot_dur_s   = 1e-3 / slots_per_sf;

  for (uint32_t r = 0; r < cpi_rows; r++) {
    los_row_estimate_t est;
    est.row    = r;
    est.time_s = row_time_slots[r] * slot_dur_s;
    const icf_t*   row  = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask = &occ_all[(size_t)r * nof_subc];
    est.valid           = estimate_row(row, mask, nof_subc, row_comb[r], nominal_bin, est);
    rows_[r]            = est;
  }

  // Least-squares fit of sub-bin delay vs. each row's own absolute time (irregular spacing).
  double   sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  double   tmin = 0.0, tmax = 0.0;
  bool     have_range = false;
  uint32_t n          = 0;
  for (const auto& e : rows_) {
    if (!e.valid) {
      continue;
    }
    sx += e.time_s;
    sy += e.frac_bin;
    sxx += e.time_s * e.time_s;
    sxy += e.time_s * e.frac_bin;
    n++;
    if (!have_range) {
      tmin = tmax  = e.time_s;
      have_range   = true;
    } else {
      tmin = std::min(tmin, e.time_s);
      tmax = std::max(tmax, e.time_s);
    }
  }
  fit.n_valid = n;

  if (n < MIN_VALID_ROWS) {
    LOG_I(PHY, "SENSING: sync(STO) CPI has only %u valid LOS rows (need >=%u) -- skipping fit/correction\n", n,
          MIN_VALID_ROWS);
    return fit;
  }

  const double denom = (double)n * sxx - sx * sx;
  double       slope = 0.0;
  if (std::abs(denom) > 1e-30) {
    slope = ((double)n * sxy - sx * sy) / denom;
  }
  fit.slope_bins_per_s = slope;
  fit.mean_frac_bin    = sy / (double)n;
  fit.drift_bins_cpi   = std::abs(slope * (tmax - tmin));
  fit.is_constant      = fit.drift_bins_cpi < DRIFT_BIN_THRESHOLD;

  uint32_t pk_min = UINT32_MAX, pk_max = 0;
  for (const auto& e : rows_) {
    if (e.valid) {
      pk_min = std::min(pk_min, e.peak_bin);
      pk_max = std::max(pk_max, e.peak_bin);
    }
  }

  if (fit.is_constant) {
    apply_correction(h_cpi, occ_all, cpi_rows, nof_subc, fit.mean_frac_bin);
    LOG_I(PHY,
          "SENSING: sync(STO) n_valid=%u peak_bin=[%u..%u] mean_frac=%.3f bins drift=%.3f bins/CPI (<%.2f) -> "
          "corrected fine STO\n",
          n, pk_min, pk_max, fit.mean_frac_bin, fit.drift_bins_cpi, DRIFT_BIN_THRESHOLD);
  } else {
    LOG_I(PHY,
          "SENSING: sync(STO) n_valid=%u peak_bin=[%u..%u] mean_frac=%.3f bins drift=%.3f bins/CPI (>=%.2f) -> "
          "SFO leaking through, deferring correction (Phase 3), no STO correction applied\n",
          n, pk_min, pk_max, fit.mean_frac_bin, fit.drift_bins_cpi, DRIFT_BIN_THRESHOLD);
  }

  return fit;
}

void cpi_sto_tracker::apply_correction(icf_t*         h_cpi,
                                       const uint8_t* occ_all,
                                       uint32_t       cpi_rows,
                                       uint32_t       nof_subc,
                                       double         mean_frac_bin)
{
  // Null only the sub-bin (fractional) component of the LOS delay -- not the row's integer CIR bin,
  // which sits at the already-diagnosed ~49 m group-delay artifact and is out of scope here (see
  // docs/NR_UE_ISAC_sync_gap_analysis.md). A common frequency-domain phase ramp applied identically
  // to every row is equivalent to, and cheaper than, a fractional-delay time-domain resample when
  // only a sub-bin shift is being removed.
  const double phase_per_subc = 2.0 * M_PI * mean_frac_bin / (double)nof_subc;
  for (uint32_t r = 0; r < cpi_rows; r++) {
    icf_t*         row  = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (!mask[c]) {
        continue;
      }
      const double phase = phase_per_subc * (double)c;
      row[c] *= icf_t((float)std::cos(phase), (float)std::sin(phase));
    }
  }
}

cfo_fit_result_t cpi_cfo_tracker::process(icf_t* h_cpi, const uint8_t* occ_all, uint32_t nof_subc,
                                          const std::vector<los_row_estimate_t>& rows)
{
  cfo_fit_result_t fit;

  // Sequential phase unwrap across valid rows, in time order (rows are already time-ordered by
  // construction -- see sensing_engine.cc's absolute-slot-indexed row accumulation).
  std::vector<uint32_t> valid_idx;
  std::vector<double>   raw_phase;
  std::vector<double>   unwrapped;
  double prev_unwrapped = 0.0;
  double prev_raw       = 0.0;
  bool   have_prev      = false;
  for (const auto& e : rows) {
    if (!e.valid) {
      continue;
    }
    const double raw = std::atan2((double)e.peak_val.imag(), (double)e.peak_val.real());
    double       u;
    if (!have_prev) {
      u          = raw;
      have_prev  = true;
    } else {
      double delta = raw - prev_raw;
      delta -= std::round(delta / (2.0 * M_PI)) * 2.0 * M_PI; // wrap into (-pi, pi]
      u = prev_unwrapped + delta;
    }
    valid_idx.push_back(e.row);
    raw_phase.push_back(raw);
    unwrapped.push_back(u);
    prev_unwrapped = u;
    prev_raw       = raw;
  }

  fit.n_valid = (uint32_t)valid_idx.size();
  if (fit.n_valid < CFO_MIN_VALID_ROWS) {
    LOG_I(PHY, "SENSING: sync(CFO) CPI has only %u valid LOS-phase rows (need >=%u) -- skipping fit/correction\n",
          fit.n_valid, CFO_MIN_VALID_ROWS);
    return fit;
  }

  // Least-squares fit of unwrapped phase vs. each row's own absolute time (irregular spacing).
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  const double n = (double)valid_idx.size();
  for (size_t i = 0; i < valid_idx.size(); i++) {
    const double x = rows[valid_idx[i]].time_s;
    const double y = unwrapped[i];
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  const double denom = n * sxx - sx * sx;
  double       slope = 0.0, intercept = 0.0;
  if (std::abs(denom) > 1e-30) {
    slope     = (n * sxy - sx * sy) / denom;
    intercept = (sy - slope * sx) / n;
  } else {
    intercept = sy / n;
  }
  fit.cfo_hz = slope / (2.0 * M_PI);

  double sq_resid = 0.0;
  for (size_t i = 0; i < valid_idx.size(); i++) {
    const double predicted = slope * rows[valid_idx[i]].time_s + intercept;
    const double resid     = unwrapped[i] - predicted;
    sq_resid += resid * resid;
  }
  fit.residual_phase_rms_rad = std::sqrt(sq_resid / n);

  // Slow cross-CPI alpha-beta tracker (diagnostic only -- the correction below uses each row's own
  // raw observed phase, not this filtered value; Phase 4's closed loop is where a filtered/fed-back
  // estimate would actually be consumed). dt is taken as 1 CPI tick, not elapsed seconds: simple and
  // sufficient per the task, and sidesteps needing an absolute across-CPI clock (cpi_row_time resets
  // every CPI by design).
  if (!state_init_) {
    cfo_state_hz_        = fit.cfo_hz;
    cfo_rate_hz_per_cpi_ = 0.0;
    state_init_          = true;
  } else {
    const double predicted = cfo_state_hz_ + cfo_rate_hz_per_cpi_;
    const double residual  = fit.cfo_hz - predicted;
    cfo_state_hz_          = predicted + CFO_ALPHA * residual;
    cfo_rate_hz_per_cpi_   = cfo_rate_hz_per_cpi_ + CFO_BETA * residual;
  }
  fit.cfo_hz_filtered     = cfo_state_hz_;
  fit.cfo_rate_hz_per_cpi = cfo_rate_hz_per_cpi_;

  // De-rotate every valid row's occupied subcarriers by its own raw observed LOS-tap phase -- a
  // uniform (not per-subcarrier) rotation, since CFO/CPE is common to every subcarrier of a row.
  for (size_t i = 0; i < valid_idx.size(); i++) {
    const uint32_t r   = valid_idx[i];
    const icf_t    rot((float)std::cos(-raw_phase[i]), (float)std::sin(-raw_phase[i]));
    icf_t*         row  = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (mask[c]) {
        row[c] *= rot;
      }
    }
  }

  LOG_I(PHY,
        "SENSING: sync(CFO) n_valid=%u cfo=%.2f Hz (filtered=%.2f Hz, rate=%.3f Hz/CPI) resid_phase_rms=%.3f rad "
        "-> CPE-corrected %u rows\n",
        fit.n_valid, fit.cfo_hz, fit.cfo_hz_filtered, fit.cfo_rate_hz_per_cpi, fit.residual_phase_rms_rad,
        fit.n_valid);

  return fit;
}

cpi_sfo_tracker::comb_stats_t& cpi_sfo_tracker::stats_for(uint32_t comb)
{
  for (auto& s : comb_stats_) {
    if (s.comb == comb) {
      return s;
    }
  }
  comb_stats_.push_back(comb_stats_t{comb, 0, 0.0, 0.0});
  return comb_stats_.back();
}

bool cpi_sfo_tracker::track_row(const icf_t* row, const uint8_t* mask, uint32_t nof_subc, uint32_t comb,
                                uint32_t anchor_bin, uint32_t& out_peak, double& out_frac,
                                double& out_win_energy_per_bin)
{
  if (!cir_builder_.build(row, mask, nof_subc, comb, cir_)) {
    return false;
  }
  const uint32_t m = (uint32_t)cir_.size();
  if (m < 2 * SFO_TRACK_HALFWIN_BINS + 3) {
    return false; // too short to window-search with parabola margin
  }

  uint32_t lo = (anchor_bin > SFO_TRACK_HALFWIN_BINS) ? anchor_bin - SFO_TRACK_HALFWIN_BINS : 1;
  uint32_t hi = std::min(anchor_bin + SFO_TRACK_HALFWIN_BINS, m - 2);
  if (lo < 1) {
    lo = 1;
  }
  if (hi <= lo) {
    return false; // anchor has walked outside this row's (shorter) CIR span
  }

  uint32_t peak     = lo;
  double   peak_pow = std::norm(cir_[lo]);
  for (uint32_t b = lo + 1; b <= hi; b++) {
    const double p = std::norm(cir_[b]);
    if (p > peak_pow) {
      peak_pow = p;
      peak     = b;
    }
  }

  const double y_m1  = std::norm(cir_[peak - 1]);
  const double y0    = std::norm(cir_[peak]);
  const double y_p1  = std::norm(cir_[peak + 1]);
  const double denom = y_m1 - 2.0 * y0 + y_p1;
  double       delta = 0.0;
  if (denom != 0.0) {
    delta = std::clamp(0.5 * (y_m1 - y_p1) / denom, -0.5, 0.5);
  }

  // Out-of-window energy: average CIR power outside [lo, hi] -- the ISI/contamination indicator.
  // Compared only within this row's own comb group (see process()), since M (and hence per-bin
  // noise floor) varies with comb.
  double   out_energy_sum = 0.0;
  uint32_t out_count      = 0;
  for (uint32_t b = 0; b < m; b++) {
    if (b >= lo && b <= hi) {
      continue;
    }
    out_energy_sum += std::norm(cir_[b]);
    out_count++;
  }
  out_win_energy_per_bin = (out_count > 0) ? (out_energy_sum / (double)out_count) : 0.0;

  out_peak = peak;
  out_frac = delta;
  return true;
}

sfo_fit_result_t cpi_sfo_tracker::process(icf_t*                   h_cpi,
                                          const uint8_t*           occ_all,
                                          uint32_t                 cpi_rows,
                                          uint32_t                 nof_subc,
                                          const uint32_t*          row_comb,
                                          const double*            row_time_slots,
                                          const nr_isac_carrier_t& carrier)
{
  sfo_fit_result_t fit;
  rows_.clear();
  comb_stats_.clear();

  if (cpi_rows == 0 || nof_subc == 0 || carrier.scs_hz == 0) {
    return fit;
  }

  const double bin_to_delay_s = 1.0 / ((double)nof_subc * (double)carrier.scs_hz);
  const double slots_per_sf   = std::max(1.0, (double)carrier.scs_hz / 15000.0);
  const double slot_dur_s     = 1e-3 / slots_per_sf;

  // Sequential tracking walk: each row's search window is re-centered on the PREVIOUS ACCEPTED
  // row's own peak (not a fixed nominal bin), so cumulative drift across the CPI can be arbitrarily
  // large as long as the per-step drift between consecutive valid, non-ISI-excluded rows stays
  // within SFO_TRACK_HALFWIN_BINS. The very first row seeds from the same nominal bin Phase 1 uses.
  uint32_t anchor = nominal_los_bin(nof_subc, carrier.scs_hz);

  rows_.reserve(cpi_rows);
  for (uint32_t r = 0; r < cpi_rows; r++) {
    sfo_row_t rowinfo;
    rowinfo.row    = r;
    rowinfo.time_s = row_time_slots[r] * slot_dur_s;
    rowinfo.comb   = row_comb[r];

    const icf_t*   row_ptr = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask    = &occ_all[(size_t)r * nof_subc];

    uint32_t peak = 0;
    double   frac = 0.0, out_energy = 0.0;
    rowinfo.valid = track_row(row_ptr, mask, nof_subc, row_comb[r], anchor, peak, frac, out_energy);

    if (rowinfo.valid) {
      rowinfo.tau_s                 = ((double)peak + frac) * bin_to_delay_s;
      rowinfo.out_win_energy_per_bin = out_energy;

      // Streaming, per-comb ISI check using ONLY the running baseline accumulated so far (causal
      // within this walk), so an anomalous row can't inflate the very baseline used to judge it.
      comb_stats_t& st = stats_for(row_comb[r]);
      bool          isi = false;
      if (st.n >= SFO_ISI_MIN_GROUP_ROWS) {
        const double mean    = st.mean;
        const double std_dev = std::sqrt(st.m2 / (double)st.n);
        if (std_dev > 0.0 && out_energy > mean + SFO_ISI_SIGMA * std_dev) {
          isi = true;
        }
      }
      rowinfo.excluded_isi = isi;

      if (!isi) {
        // Welford online update of this comb group's mean/variance.
        st.n++;
        const double delta1 = out_energy - st.mean;
        st.mean += delta1 / (double)st.n;
        const double delta2 = out_energy - st.mean;
        st.m2 += delta1 * delta2;

        anchor = peak; // advance the walk only on accepted (non-anomalous) rows
      }
    }

    rows_.push_back(rowinfo);
  }

  uint32_t n_candidate = 0, n_excluded = 0;
  for (const auto& e : rows_) {
    if (e.valid) {
      n_candidate++;
      if (e.excluded_isi) {
        n_excluded++;
      }
    }
  }
  fit.n_candidate    = n_candidate;
  fit.n_excluded_isi = n_excluded;

  // Least-squares fit of {time, delay} over surviving (valid, non-ISI-excluded) rows.
  double   sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  uint32_t n  = 0;
  for (const auto& e : rows_) {
    if (!e.valid || e.excluded_isi) {
      continue;
    }
    sx += e.time_s;
    sy += e.tau_s;
    sxx += e.time_s * e.time_s;
    sxy += e.time_s * e.tau_s;
    n++;
  }
  fit.n_fit = n;

  if (n < SFO_MIN_VALID_ROWS) {
    LOG_I(PHY,
          "SENSING: sync(SFO) CPI candidates=%u excluded_isi=%u fit_rows=%u (need >=%u) -- skipping fit/correction\n",
          n_candidate, n_excluded, n, SFO_MIN_VALID_ROWS);
    return fit;
  }

  const double denom = (double)n * sxx - sx * sx;
  double       slope = 0.0;
  if (std::abs(denom) > 1e-30) {
    slope = ((double)n * sxy - sx * sy) / denom;
  }

  // slope is delay-drift rate in seconds-of-delay per second-elapsed == the dimensionless fractional
  // sample-clock error (ppm/1e6) directly -- no further unit conversion needed beyond x1e6.
  fit.sfo_ppm               = slope * 1.0e6;
  fit.sample_clock_error_hz = fit.sfo_ppm * 1.0e-6 * SAMPLE_RATE_HZ;
  fit.corrected             = true;

  // Correct EVERY row (not just fit-contributing ones) using the fit's continuous prediction at
  // that row's own time, anchored so the correction is zero at CPI start (t=0): tau_correct(row) =
  // slope * time_s[row] only -- the fit's intercept (its value at t=0, i.e. wherever the absolute
  // delay level sits, including the diagnosed ~49 m / bin-6 group delay) is deliberately excluded,
  // mirroring Phase 1's fractional-only correction boundary.
  for (uint32_t r = 0; r < cpi_rows; r++) {
    const double   tau_correct = slope * (row_time_slots[r] * slot_dur_s);
    const double   phase_per_subc = 2.0 * M_PI * (double)carrier.scs_hz * tau_correct;
    icf_t*         row_ptr = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask    = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (!mask[c]) {
        continue;
      }
      const double phase = phase_per_subc * (double)c;
      row_ptr[c] *= icf_t((float)std::cos(phase), (float)std::sin(phase));
    }
  }

  LOG_I(PHY,
        "SENSING: sync(SFO) candidates=%u excluded_isi=%u fit_rows=%u sfo=%.4f ppm (%.3f Hz @ %.2f MHz sample "
        "rate) -> corrected %u rows\n",
        n_candidate, n_excluded, n, fit.sfo_ppm, fit.sample_clock_error_hz, SAMPLE_RATE_HZ / 1e6, cpi_rows);

  return fit;
}

} // namespace nr_isac
