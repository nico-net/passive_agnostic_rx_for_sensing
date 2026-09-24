/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <array>
#include <cmath>
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
  std::array<double, kCh> noise{};    // thermal: from range bins far from every path
  std::array<bool, kCh> los_found{};  // = LosEstimate::found; channels without a LOS reference are
                                      // skipped by the envelope, detection and refinement
  /** Waveform ambiguity |sum_r w_r K_r(dr) e^{-j2pi dd t_r}|^2 / |sum_r w_r|^2 (K_r = row r's
   * unit-peak range kernel from its mask) for dr in [-n_range, n_range], dd in [-n_dopp, n_dopp] bins,
   * tabulated at 1/kAmbOvs bin; the same for every channel (shared masks and row times). A path's
   * leakage into the RD cell (dr, dd) away is its peak power times this. 0 outside the table. */
  static constexpr long kAmbOvs = 4;
  std::vector<float> amb;
  float ambiguity(double dr, double dd) const
  {
    const long nr = rd.axes.n_range * kAmbOvs, nd = rd.axes.n_dopp * kAmbOvs;
    const long r = std::lround(dr * kAmbOvs), d = std::lround(dd * kAmbOvs);
    if (amb.empty() || r < -nr || r > nr || d < -nd || d > nd) return 0.f;
    return amb[(size_t)((r + nr) * (2 * nd + 1) + d + nd)];
  }
};

/** Envelope voxel grid over the volume. Voxel v = (iz*ny + iy)*nx + ix. */
struct Grid {
  Vec3 origin; double step = 0; uint32_t nx = 0, ny = 0, nz = 0;
  size_t size() const { return (size_t)nx * ny * nz; }
  Vec3 at(size_t v) const;
};
struct DetectParams { double pfa = 0; };  // per-voxel-per-Doppler false-alarm probability (derived)

Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps);
LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa);
RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& los);
RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& los, const RowSync& sync);
/** step = c/(4*b_eff) over the volume. */
Grid envelope_grid(const Volume& vol, const Axes& a);
/** E[t*g.size()+v] for t over a.tested_dopp: sum over LOS-found channels of max over the voxel's
 * per-channel Doppler window (+-v_max*max|u_i-u_j|/(lambda*step) bins around d, notch excluded) of
 * |RD_i(dtau_i(x), d')|^2/noise_i: under noise a sum of n_used maxima of m Exp(1). */
std::vector<float> envelope(const RdResult& R, const Grid& g, const Geometry& geo);
/** pfa = false_object_intensity_per_s * T_cpi / (n_voxels * n_dopp_tested). */
DetectParams detect_params(const Axes& a, const Grid& g, double false_object_intensity_per_s);
/** Per-Doppler median-scaled CFAR on that null (max_exp_sum_quantile), spatial local maxima,
 * velocity-consistent per-channel Doppler bins, greedy residual pursuit against the accepted
 * detections' ambiguity leakage, NMS / harmonic merge, z < 0 rejected. */
std::vector<Detection> detect(const std::vector<float>& E, const RdResult& R, const Grid& g,
                              const Geometry& geo, const DetectParams& p);
/** Coherent refinement (hierarchical, down to half a fringe) within +-one envelope step. pos = rho_eff
 * blend, rho_eff = cal.rho * mean_i exp(-sigma_phi_i^2/2), sigma_phi_i = (2pi/lambda)|u_tx,i - u_x,i| *
 * survey_sigma_m[i]: the LOS calibration cannot see survey error, the target phase can. */
void refine(Detection& det, const RdResult& R, const Grid& g, const Geometry& geo, const Calibration& cal,
            const std::array<double, kCh>& survey_sigma_m);
/** x with P(S > x) = p, S = sum of n iid Y, Y = max of m iid Exp(1) (CDF (1-e^-y)^m): the envelope's
 * per-voxel null when each channel takes its max over m Doppler bins. m = 1 gives Gamma(n,1). */
double max_exp_sum_quantile(uint32_t m, uint32_t n, double p);
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
