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

} // namespace

fft_plan* cpi_sto_tracker::plan_for(uint32_t m)
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

bool cpi_sto_tracker::estimate_row(const icf_t*         row,
                                   const uint8_t*       mask,
                                   uint32_t             nof_subc,
                                   uint32_t             comb,
                                   uint32_t             nominal_bin,
                                   los_row_estimate_t&  out)
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

  cir_.resize(m);
  plan_for(m)->run(compact_.data(), cir_.data());

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

  // bin_to_delay is invariant to a row's native comb: a comb-N row's compact CIR has M = nof_subc/N
  // samples spaced N*SCS apart in frequency, so M*(N*SCS) = nof_subc*SCS for every row -- the same
  // physical delay always lands on the same bin index regardless of source/comb.
  const double bin_to_delay_s = 1.0 / ((double)nof_subc * (double)carrier.scs_hz);
  const double range_res_m    = SPEED_OF_LIGHT * bin_to_delay_s / 2.0;
  const uint32_t nominal_bin  = (uint32_t)std::lround(NOMINAL_LOS_RANGE_M / range_res_m);

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

} // namespace nr_isac
