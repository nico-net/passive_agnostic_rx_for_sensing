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

// Minimum same-comb rows observed before the ISI baseline is trusted enough to flag anomalies;
// below this every row in that comb group is accepted (building up the baseline).
constexpr uint32_t SFO_ISI_MIN_GROUP_ROWS = 3;

// Size of the sliding window of recent accepted same-comb rows the ISI baseline is computed over
// (see comb_stats_t's comment for why this replaced a whole-CPI cumulative mean). Large enough to
// average out row-to-row noise, small enough to track legitimate slow drift in the metric as the
// tracking window walks across the compact CIR over a long CPI.
constexpr uint32_t SFO_ISI_LOCAL_WINDOW = 20;

// A row is excluded as ISI-contaminated if its out-of-window CIR energy exceeds its comb group's
// local mean by more than this many standard deviations. Tuned (not guessed) from Phase 6a
// measurement: out-of-window energy is dominated by ordinary windowed-DFT spectral leakage, whose
// level swings ~3-4 orders of magnitude as a function of the row's OWN sub-bin fractional position
// alone (near-zero frac -> near-zero leakage; frac near 0.5-0.9 -> much higher), entirely independent
// of any real contamination -- confirmed via a standalone clean-signal measurement (see gap-analysis
// doc section 12). A local-neighbor sigma threshold cannot distinguish this benign, frac-driven
// swing from genuine ISI without normalizing by the row's own frac first (not implemented here --
// flagged as follow-up work). SFO_ISI_SIGMA is set high enough that this ordinary swing does not
// trigger false exclusions on a clean synthetic signal (empirically verified), while still able to
// catch a genuinely extreme (many-sigma-beyond-the-local-mix) contamination event.
constexpr double SFO_ISI_SIGMA = 30.0;

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

// Phase 4: number of successful (detection-found) CPIs averaged to establish the LOS baseline.
// "First several CPIs" per the task text; not tied to any particular CPI duration.
constexpr uint32_t LOS_BASELINE_INIT_CPIS = 5;

// Phase 4: a detection must be within this many bins of the current (candidate) baseline on BOTH
// axes to be considered "the" LOS detection this CPI. Wide enough for residual jitter/sub-bin
// quantization, narrow enough not to grab an unrelated target sharing the LOS's general vicinity.
constexpr uint32_t LOS_MATCH_MAX_RANGE_BINS   = 5;
constexpr uint32_t LOS_MATCH_MAX_DOPPLER_BINS = 5;

// Phase 4: leaky-integrator gains for the closed-loop bias. A PURE integrator risks unbounded
// windup if the baseline match degrades for several consecutive CPIs (e.g. a real target
// transiting the LOS's own range/Doppler cell); the leak term keeps the bias state bounded while
// still accumulating a persistent correction over many CPIs. Starting defaults; tune in Phase 6a
// against the self-test harness's known-injected residual (this is the PLL/integrator bandwidth
// choice the Constraints section asks to be stated explicitly, not picked silently).
constexpr double LOS_BIAS_KI   = 0.3;
constexpr double LOS_BIAS_LEAK = 0.02;

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

const std::vector<float>& row_cir_builder::hann_for(uint32_t m)
{
  for (auto& kv : hann_cache_) {
    if (kv.first == m) {
      return kv.second;
    }
  }
  std::vector<float> w(m);
  const uint32_t     denom = (m > 1) ? (m - 1) : 1;
  for (uint32_t i = 0; i < m; i++) {
    w[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)i / (float)denom));
  }
  hann_cache_.emplace_back(m, std::move(w));
  return hann_cache_.back().second;
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

  // Hann-window before the IFFT (same formula range_doppler.cc already uses for its own range/
  // Doppler axes). Discovered empirically in Phase 6a testing, not assumed: an un-windowed compact
  // CIR is a raw Dirichlet-kernel (rectangular-spectrum) mainlobe, and 3-point parabolic
  // interpolation of that shape has severe bias (measured up to ~0.2 bin error approaching a 0.5-bin
  // true offset) -- windowing shapes the mainlobe much closer to parabolic, which is exactly why
  // range_doppler's own main DSP path already windows both its axes. This does not shift the peak's
  // location (a window is a real, symmetric taper -- it broadens the mainlobe, it does not move it),
  // only the accuracy of sub-bin extraction from it. See docs/NR_UE_ISAC_sync_gap_analysis.md
  // section 12 for the measured before/after bias figures.
  const std::vector<float>& win = hann_for(m);
  for (uint32_t i = 0; i < m; i++) {
    compact_[i] *= win[i];
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
  comb_stats_.push_back(comb_stats_t{comb, {}});
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

      // Sliding-window, per-comb ISI check using only the last SFO_ISI_LOCAL_WINDOW ACCEPTED rows
      // of the same comb ("neighboring rows", not the whole CPI since it started -- see comb_stats_t's
      // comment for why a whole-CPI cumulative baseline over-triggered in testing).
      comb_stats_t& st = stats_for(row_comb[r]);
      bool          isi = false;
      if (st.recent.size() >= SFO_ISI_MIN_GROUP_ROWS) {
        double mean = 0.0;
        for (double v : st.recent) {
          mean += v;
        }
        mean /= (double)st.recent.size();
        double m2 = 0.0;
        for (double v : st.recent) {
          m2 += (v - mean) * (v - mean);
        }
        const double std_dev = std::sqrt(m2 / (double)st.recent.size());
        if (std_dev > 0.0 && out_energy > mean + SFO_ISI_SIGMA * std_dev) {
          isi = true;
          // Phase 5 instrumentation: per-row exclusion detail (which row, why). LOG_D since this can
          // fire multiple times per CPI; the LOG_I summary below covers the per-CPI count.
          LOG_D(PHY,
                "SENSING: sync(SFO) row=%u comb=%u excluded (ISI): out_win_energy=%.3e > local_mean=%.3e + "
                "%.1f*stddev=%.3e (n_local=%zu)\n",
                r, row_comb[r], out_energy, mean, SFO_ISI_SIGMA, std_dev, st.recent.size());
        }
      }
      rowinfo.excluded_isi = isi;

      if (!isi) {
        st.recent.push_back(out_energy);
        if (st.recent.size() > SFO_ISI_LOCAL_WINDOW) {
          st.recent.pop_front();
        }
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

los_residual_t los_baseline_tracker::update_residual(const std::vector<sensing_detection_t>& detections,
                                                     const sensing_rvm_t& rvm, double fc_hz)
{
  los_residual_t out;

  if (rvm.nof_range_bins == 0 || rvm.nof_doppler_bins == 0) {
    return out;
  }

  // Reference to match against this CPI: the established baseline once set, otherwise the running
  // init average (or, before any init CPI has succeeded, a physically-motivated initial guess --
  // the same nominal LOS range Phase 1/3 seed from, and zero Doppler since the direct path is
  // static once fully corrected -- NOT an arbitrary universal constant asserted as truth).
  uint32_t guess_range_bin, guess_doppler_bin;
  if (baseline_set_) {
    guess_range_bin   = baseline_range_bin_;
    guess_doppler_bin = baseline_doppler_bin_;
  } else if (init_count_ > 0) {
    guess_range_bin   = (uint32_t)(init_range_bin_sum_ / init_count_);
    guess_doppler_bin = (uint32_t)(init_doppler_bin_sum_ / init_count_);
  } else {
    // Physically-motivated initial guess, not an arbitrary constant: the same nominal LOS range
    // Phase 1/3 seed from (rvm.range_res_m already encodes this CPI's actual bin spacing), and
    // zero Doppler since the direct path is static once fully corrected.
    guess_range_bin   = (uint32_t)std::lround(NOMINAL_LOS_RANGE_M / (double)rvm.range_res_m);
    guess_doppler_bin = rvm.nof_doppler_bins / 2;
  }

  const sensing_detection_t* best     = nullptr;
  uint32_t                   best_dist = UINT32_MAX;
  for (const auto& d : detections) {
    const uint32_t dr = (d.range_bin > guess_range_bin) ? (d.range_bin - guess_range_bin) : (guess_range_bin - d.range_bin);
    const uint32_t dd = (d.doppler_bin > guess_doppler_bin) ? (d.doppler_bin - guess_doppler_bin)
                                                             : (guess_doppler_bin - d.doppler_bin);
    if (dr > LOS_MATCH_MAX_RANGE_BINS || dd > LOS_MATCH_MAX_DOPPLER_BINS) {
      continue;
    }
    const uint32_t dist = dr + dd;
    if (dist < best_dist) {
      best_dist = dist;
      best      = &d;
    }
  }

  if (best == nullptr) {
    out.baseline_established = baseline_set_;
    return out; // no detection near the (candidate) baseline this CPI -- nothing to measure
  }

  out.detection_found = true;
  out.range_bin        = best->range_bin;
  out.doppler_bin       = best->doppler_bin;

  if (!baseline_set_) {
    // Still accumulating the init average.
    init_count_++;
    init_range_sum_ += best->range_m;
    init_vel_sum_ += best->vel_mps;
    init_range_bin_sum_ += best->range_bin;
    init_doppler_bin_sum_ += best->doppler_bin;

    if (init_count_ >= LOS_BASELINE_INIT_CPIS) {
      baseline_range_m_     = init_range_sum_ / (double)init_count_;
      baseline_vel_mps_     = init_vel_sum_ / (double)init_count_;
      baseline_range_bin_   = (uint32_t)std::lround((double)init_range_bin_sum_ / (double)init_count_);
      baseline_doppler_bin_ = (uint32_t)std::lround((double)init_doppler_bin_sum_ / (double)init_count_);
      baseline_set_         = true;
      LOG_I(PHY,
            "SENSING: sync(LOS baseline) established over %u CPIs: range_bin=%u (%.2f m) doppler_bin=%u "
            "(%.3f m/s)\n",
            init_count_, baseline_range_bin_, baseline_range_m_, baseline_doppler_bin_, baseline_vel_mps_);
    }
    out.baseline_established = baseline_set_;
    // Report against the running average even before finalisation, for Phase 5 convergence logging.
    out.range_residual_m = best->range_m - (init_range_sum_ / (double)init_count_);
    out.vel_residual_mps = best->vel_mps - (init_vel_sum_ / (double)init_count_);
    out.delay_bias_s     = delay_bias_s_;
    out.cfo_bias_hz      = cfo_bias_hz_;
    return out;
  }

  // Baseline established: this is the real residual, and the closed-loop bias update.
  out.baseline_established = true;
  out.range_residual_m     = best->range_m - baseline_range_m_;
  out.vel_residual_mps     = best->vel_mps - baseline_vel_mps_;

  const double delay_residual_s = 2.0 * out.range_residual_m / SPEED_OF_LIGHT; // matches range_doppler.cc's
                                                                                // round-trip range_m = c*tau/2
  const double cfo_residual_hz  = 2.0 * out.vel_residual_mps * fc_hz / SPEED_OF_LIGHT; // matches vel_res_mps's
                                                                                        // own fc-based derivation

  delay_bias_s_ = (1.0 - LOS_BIAS_LEAK) * delay_bias_s_ + LOS_BIAS_KI * delay_residual_s;
  cfo_bias_hz_  = (1.0 - LOS_BIAS_LEAK) * cfo_bias_hz_ + LOS_BIAS_KI * cfo_residual_hz;

  out.delay_bias_s = delay_bias_s_;
  out.cfo_bias_hz  = cfo_bias_hz_;

  LOG_I(PHY,
        "SENSING: sync(LOS residual) range=%u (%.2f m, resid=%.3f m) doppler=%u (resid=%.4f m/s) -> "
        "bias(delay=%.3e s, cfo=%.3f Hz)\n",
        out.range_bin, best->range_m, out.range_residual_m, out.doppler_bin, out.vel_residual_mps, delay_bias_s_,
        cfo_bias_hz_);

  return out;
}

void los_baseline_tracker::apply_bias_correction(icf_t* h_cpi, const uint8_t* occ_all, uint32_t cpi_rows,
                                                 uint32_t nof_subc, const double* row_time_slots,
                                                 const nr_isac_carrier_t& carrier) const
{
  if (!baseline_set_ || (delay_bias_s_ == 0.0 && cfo_bias_hz_ == 0.0)) {
    return; // no established baseline yet, or the integrator hasn't accumulated any correction
  }

  const double slots_per_sf = std::max(1.0, (double)carrier.scs_hz / 15000.0);
  const double slot_dur_s   = 1e-3 / slots_per_sf;

  // Same two correction primitives as Phases 1-3: a linear-in-k frequency ramp cancels the
  // accumulated delay bias (identical sign convention to Phase 1/3's "+j2*pi*k*SCS*tau" cancel
  // formula), and a uniform per-row rotation, scaled by that row's own elapsed time, cancels the
  // accumulated CFO-like bias (identical sign convention to Phase 2's "-observed phase" cancel).
  const double delay_phase_per_subc = 2.0 * M_PI * (double)carrier.scs_hz * delay_bias_s_;

  for (uint32_t r = 0; r < cpi_rows; r++) {
    const double time_s   = row_time_slots[r] * slot_dur_s;
    const double cfo_phase = 2.0 * M_PI * cfo_bias_hz_ * time_s;
    const icf_t  cfo_rot((float)std::cos(-cfo_phase), (float)std::sin(-cfo_phase));

    icf_t*         row_ptr = &h_cpi[(size_t)r * nof_subc];
    const uint8_t* mask    = &occ_all[(size_t)r * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      if (!mask[c]) {
        continue;
      }
      const double delay_phase = delay_phase_per_subc * (double)c;
      const icf_t  delay_rot((float)std::cos(delay_phase), (float)std::sin(delay_phase));
      row_ptr[c] = row_ptr[c] * delay_rot * cfo_rot;
    }
  }
}

} // namespace nr_isac
