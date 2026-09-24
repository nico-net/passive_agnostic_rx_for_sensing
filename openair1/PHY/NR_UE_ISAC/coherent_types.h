/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>
#include "small_matrix.h"

namespace nr_isac::coherent {

using nr_isac::Vec3;  // re-exported so `using namespace nr_isac::coherent` also finds it
using cd = std::complex<double>;
using cf = std::complex<float>;
constexpr double kC = 299792458.0;
constexpr uint32_t kCh = 4;

struct Volume { double x0 = -15, x1 = 15, y0 = -15, y1 = 15, z0 = 0, z1 = 30; };
struct Geometry { std::array<Vec3, kCh> rx{}; Vec3 tx{}; };

struct CoherentConfig {
  bool enable = false;
  bool ul_enable = false;
  Volume volume;
  double survey_sigma_m = 0.1;               // declared tape accuracy (input, not tuned)
  double max_speed_mps = 0.0;                // = PipelineConfig::maximum_target_speed_mps
  double false_object_intensity_per_s = 0.0; // = PipelineConfig::false_object_intensity_per_s
  Geometry geometry;                         // surveyed: spatial_rx_positions + tx_pos_*
  std::string out_dir;                       // directory of report_path
  double monitor_period_s = 0.5;             // = rvm_period_s
};

/** Per-CPI axes, all derived from the CPI's own allocation and row times. */
struct Axes {
  bool valid = false;
  std::string invalid_reason;
  double fc_hz = 0, lambda_m = 0, scs_hz = 0;
  uint32_t subcarriers = 0;  // window grid width
  uint32_t n_fft = 0;        // zero-padded range FFT length
  double delay_step_s = 0;   // 1/(n_fft*scs)
  uint32_t n_range = 0;      // cropped range bins (bin 0 = own LOS)
  double b_eff_hz = 0;       // median observed bandwidth per row
  uint32_t n_dopp = 0;       // Doppler bins
  double dopp_step_hz = 0, dopp0_hz = 0;  // bin d frequency = dopp0_hz + d*dopp_step_hz
  double t_cpi_s = 0, median_dt_s = 0;
  uint32_t notch_half_bins = 2;           // Hann mainlobe half-width (window property)
  double v_max_mps = 0;                   // declared max speed (else the Doppler span's): sets the
                                          // per-channel Doppler search width of the envelope
  std::vector<uint32_t> tested_dopp;      // bins outside the notch and within 2*v_max/lambda
  std::vector<double> row_t_s;            // row times relative to the first row
};

struct Calibration {
  std::array<double, kCh> phase_rad{};      // posterior per-channel phase (ch0 = 0)
  std::array<double, kCh> phase_var{};      // posterior phase variance (rad^2)
  std::array<double, kCh> coh_factor{};     // exp(-(phase_var + process_noise)/2): the one-step
                                             // PREDICTIVE phase variance, so an unpredictable
                                             // (decohering) channel fades even when its own posterior
                                             // looks confident.
  std::array<double, kCh> los_snr{};        // linear
  std::array<double, kCh> jitter_rad{};     // running innovation RMS: sqrt(cumulative mean of nu^2)
  std::array<double, kCh> jitter_bound_rad{}; // 1/sqrt(2*SNR)
  std::array<bool, kCh> los_found{};
  double coherent_gain = 1.0;               // predicted-phase LOS coherent gain in [0,n_used], n_used =
                                             // channels found+updated this CPI AND already seeded
  double rho = 0.0;                         // (G-1)/(n_used-1) clipped to [0,1]
};

struct Detection {
  Vec3 pos;                 // final position (rho-blended)
  Vec3 pos_env;             // envelope-pass position
  Vec3 pos_sigma;           // 1-sigma per axis
  double doppler_hz = 0;
  double range_rate_mps = 0;        // bistatic path-length rate = -lambda*doppler
  double range_rate_sigma = 0;
  double snr = 0;                   // linear, per channel equivalent
  uint32_t dopp_bin = 0;
  std::array<int32_t, kCh> chan_dopp_bin{-1, -1, -1, -1};  // each channel's own (velocity-consistent)
                                                            // Doppler bin; -1 = not set
  bool refined = false;
  std::array<cd, kCh> terms{};      // per-channel coherent terms at pos (autofocus input)
  uint64_t illuminator = 0;         // 0 = gNB, else UL session id
  Vec3 tx;                          // illuminator position used
};

struct Track {
  uint64_t id = 0;
  std::array<double, 6> x{};        // x y z vx vy vz
  std::array<double, 36> P{};       // row-major 6x6
  double llr = 0;                   // SPRT log-likelihood ratio
  double q = 0;                     // white-acceleration PSD (adaptive)
  double nis_sum = 0; uint32_t nis_n = 0;
  uint32_t hits = 0, misses = 0;
  double age_s = 0;
  bool confirmed = false;
};

inline double dist(const Vec3& a, const Vec3& b) { return norm(a - b); }
/** Excess bistatic delay of x on the channel at rx, relative to that channel's direct path (s). */
inline double excess_delay_s(const Vec3& x, const Vec3& tx, const Vec3& rx)
{
  return (dist(x, tx) + dist(x, rx) - dist(tx, rx)) / kC;
}
bool parse_volume(const std::string& text, Volume* out);

} // namespace nr_isac::coherent
