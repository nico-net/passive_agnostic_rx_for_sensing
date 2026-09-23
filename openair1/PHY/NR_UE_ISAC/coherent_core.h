/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <array>
#include <vector>
#include "coherent_types.h"
#include "pipeline_types.h"

namespace nr_isac::coherent {

/** Range-Doppler cube, layout [ch][range][dopp]. */
struct RangeDoppler {
  Axes axes;
  std::vector<cf> v;
  size_t idx(uint32_t ch, uint32_t r, uint32_t d) const { return ((size_t)ch * axes.n_range + r) * axes.n_dopp + d; }
};

struct LosEstimate {
  std::array<double, kCh> delay_s{};
  std::array<cd, kCh> tap{};
  std::array<double, kCh> snr{};
  std::array<bool, kCh> found{};
};

/** Common per-row CFO phase and SFO delay DRIFT (zero-mean over rows; absolute delay stays
 * referenced to the LOS). valid = false when no row carried a usable LOS tap. */
struct RowSync {
  std::vector<double> phase_rad;
  std::vector<double> delay_s;
  bool valid = false;
};

struct RdResult {
  RangeDoppler rd;
  std::array<cd, kCh> los_tap{};
  std::array<double, kCh> noise{};
};

Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps);
LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa);
RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& los);
RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& los, const RowSync& sync);
/** x with Q(shape, x) = p (regularised upper incomplete gamma), integer shape. */
double gamma_upper_quantile(uint32_t shape, double p);

/** Tracks each channel's phase offset vs channel 0 across CPIs (Kalman phase filter, covariance
 * matching for process noise) and, before updating, scores this CPI's LOS taps against the
 * PREVIOUS posterior for a non-tautological coherent-gain / rho diagnostic. */
class Calibrator {
public:
  /** One CPI: los taps (LOS-referenced), found flags and LOS SNR -> posterior calibration. */
  Calibration update(const std::array<cd,kCh>& los_tap, const std::array<bool,kCh>& found,
                     const std::array<double,kCh>& los_snr);
  const Calibration& last() const { return last_; }
private:
  std::array<cd,kCh> s_{};          // unit phasor state per channel (ch0 fixed at 1)
  std::array<double,kCh> p_{};      // posterior phase variance
  std::array<double,kCh> q_{};      // process noise: RAW (unclipped, can go negative) cumulative mean
                                     // of the unbiased Mehra-style estimator nu^2-(p_prev+r); clamped
                                     // to >=0 only at the point of use, never in the accumulator.
  std::array<double,kCh> nu2_{};    // cumulative mean of nu^2 (jitter_rad = sqrt of this)
  std::array<uint32_t,kCh> nq_{};   // shared sample count for q_ and nu2_ (post-seeding innovations only)
  std::array<bool,kCh> seeded_{};   // per-channel: has this channel had its own first valid measurement
  bool init_ = false;
  Calibration last_;
};

} // namespace nr_isac::coherent
