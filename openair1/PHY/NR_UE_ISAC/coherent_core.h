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
  std::vector<double> amp;        // per-row amplitude correction (1/|common gain|); empty = 1
  std::vector<uint8_t> bad;       // per-row outlier flag (static residual inconsistent with the other rows); empty = none
  bool valid = false;
};

struct RdResult {
  RangeDoppler rd;
  std::array<cd, kCh> los_tap{};
  std::array<double, kCh> noise{};    // thermal: from range bins far from every path
  std::array<bool, kCh> los_found{};  // = LosEstimate::found; channels without a LOS reference are
                                      // skipped by the envelope, detection and refinement
  /** Waveform model shared by every channel (same masks and row times), for exact leakage
   * prediction. A row's range kernel is K_r(x) = e^{j2pi fc_r x delay_step} B_g(x): B_g depends only
   * on the row's mask shape relative to its span centre (group g), tabulated at 1/kOvs bin. */
  struct Waveform {
    static constexpr long kOvs = 32;
    std::vector<double> w;                   // slow-time Hann weight per row
    double wsum = 0, w2sum = 0;              // sum w, sum w^2 * hh
    std::vector<double> fc, hh;              // row centre baseband Hz; sum(h^2)/(sum h)^2
    std::vector<int32_t> grp;                // row -> group, -1 = empty row
    std::vector<std::vector<cd>> B, B2;      // per group, x in [-X, X] bins: kernel (h) and noise kernel (h^2)
    long X = 0;
    uint32_t sc = 0;                         // window subcarriers
    std::vector<uint8_t> mask;               // [row][subcarrier]
    std::vector<uint32_t> lo, hi;            // row span (observed subcarriers), empty rows: lo > hi
    std::vector<cf> Q;                       // [dopp][subcarrier]: static-removal operator, see .cc
  } wf;
  /** Complex ambiguity of a unit path: RD response x range bins and dd Doppler bins away from it
   * (dd = (f_cell - f_path)/dopp_step). 1 at (0,0). */
  cd ambiguity_c(double x, double dd) const;
  /** Noise correlation between RD cells (x, dd) apart; 1 at (0,0). */
  cd noise_corr(double x, double dd) const;
  float ambiguity(double x, double dd) const { return (float)std::norm(ambiguity_c(x, dd)); }
};

/** Envelope voxel grid over the volume. Voxel v = (iz*ny + iy)*nx + ix. */
struct Grid {
  Vec3 origin; double step = 0; uint32_t nx = 0, ny = 0, nz = 0;
  size_t size() const { return (size_t)nx * ny * nz; }
  Vec3 at(size_t v) const;
};
struct DetectParams { double pfa = 0; };  // per-voxel-per-Doppler false-alarm probability (derived)

/** Accelerator for the O(rows x subcarriers) array stages of find_los()/estimate_row_sync(), bound to
 * ONE CfrWindow (the GPU path, coherent_cuda_front.cu). It only produces the arrays the CPU loops do;
 * every decision stays in the shared code. nullptr = the CPU loops (the oracle). */
struct FrontOps {
  virtual ~FrontOps() = default;
  /** kf[n] = sum_g count[g] |unit row profile of row first[g] on an ovs*n_fft grid|^2 (before /R). */
  virtual void los_kernel(const Axes& a, uint32_t ovs, const std::vector<uint32_t>& first, const std::vector<uint32_t>& count,
                          std::vector<double>& kf) = 0;
  /** pw[i][n] = mean over the R non-empty rows of |row_profile(i, r, 0, 0)[n]|^2. */
  virtual void los_noncoherent(const Axes& a, uint32_t R, std::array<std::vector<double>, kCh>& pw) = 0;
  /** Union-band unit kernel Uk[k] = sum_r obs h_r(k)/(ws_r R), and kmag[n] = |its unnormalised inverse
   * FFT on an ovs*n_fft grid|. */
  virtual void los_union_kernel(const Axes& a, uint32_t R, uint32_t ovs, std::vector<cd>& Uk, std::vector<double>& kmag) = 0;
  /** Per channel: coh[i] = the unnormalised inverse FFT (n_fft, centred index) of find_los's coherent
   * row-mean spectrum U[i][k] = sum_r obs H h_r(k)/(ws_r R) e^{j(2pi f_k s.delay[r] - s.phase[r])} (U is
   * kept for los_refine()). */
  virtual void los_coherent(const Axes& a, uint32_t R, const RowSync& s, std::array<std::vector<cd>, kCh>& coh) = 0;
  /** acc[r][i] = sum over row r's observed k of H_i(r,k) e^{j2pi f_k (delay[i] + drift[r])} for found
   * channels (others 0). slope (optional): [r] = sum over found i and observed k with k - comb_r observed
   * of z(k) conj(z(k - comb_r)), z the same derotated value. */
  virtual void row_sums(const std::array<double, kCh>& delay, const std::vector<double>* drift, const std::array<bool, kCh>& found,
                        std::vector<std::array<cd, kCh>>& acc, std::vector<cd>* slope) = 0;
  /** Per row: finest subcarrier spacing (row_comb), mean observed baseband frequency and observed count. */
  virtual void row_info(std::vector<uint32_t>& comb, std::vector<double>& fc_mean, std::vector<double>& n_obs) = 0;
  /** find_los's final sub-bin stage for every found channel, on the U / Uk of the last los_coherent() /
   * los_union_kernel(): golden-section argmax of |coh(x)| on [best-1, best+1] and, when best != peak, the
   * alternating two-path fit against the path at peak. x in bins (unfolded), tap = coh at x. */
  virtual void los_refine(const Axes& a, const std::array<long, kCh>& best, const std::array<long, kCh>& peak,
                          const std::array<bool, kCh>& found, std::array<double, kCh>& x, std::array<cd, kCh>& tap) = 0;
};

Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps);
/** geo_los_s (optional): each channel's geometric LOS delay |tx-rx_i|/c from the survey; when given, the
 * LOS candidate consistent with one common offset across channels wins over a stronger wall. */
LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa, const std::array<double, kCh>* geo_los_s = nullptr,
                     FrontOps* ops = nullptr);
RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& los, FrontOps* ops = nullptr);
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
/** Declared survey accuracy (1-sigma, m) of each antenna and of the illuminator. */
struct SurveySigma { std::array<double, kCh> rx_m{}; double tx_m = 0; };
/** Coherent refinement, hierarchical down to half a fringe within +-one envelope step, phases referred
 * to the CPI's weighted mid-time. pos = rho_eff blend with rho_eff = cal.rho * survey coherence *
 * P_lobe; pos_cov = rho_eff Cov_coh + (1-rho_eff) Cov_env + rho_eff(1-rho_eff) dd^T (mixture). */
void refine(Detection& det, const RdResult& R, const Grid& g, const Geometry& geo, const Calibration& cal,
            const SurveySigma& survey);
/** x with P(S > x) = p, S = sum of n iid Y, Y = max of m iid Exp(1) (CDF (1-e^-y)^m): the envelope's
 * per-voxel null when each channel takes its max over m Doppler bins. m = 1 gives Gamma(n,1). */
double max_exp_sum_quantile(uint32_t m, uint32_t n, double p);
/** x with Q(shape, x) = p (regularised upper incomplete gamma), integer shape. */
double gamma_upper_quantile(uint32_t shape, double p);

/** Clutter-limited CFAR in range. Per (channel, range bin) the floor is the median over the tested Doppler
 * bins (robust: a mover occupies a few) converted to an exponential mean; where it exceeds the channel's
 * thermal noise the whole range bin is scaled to thermal (amplitude sqrt(noise/floor)), so every cell is
 * unit-mean under "nothing moving" and the designed false-alarm rate holds at clutter-limited ranges too
 * (OTA empty room: broadband static residue 30-46 dB above thermal at the direct path's ranges made
 * ~10 detections per CPI). A target at the same range as that residue is judged against it -- physics.
 * Skipped (all factors 1) when the waveform's own Doppler pedestal could hide a mover under the floor
 * (pi_ped * -ln(pfa_cell) >= 1, e.g. narrow hopping rows). Returns the factors [ch][range] (1 = none). */
std::vector<float> whiten_range_clutter(RdResult& R, double pfa_cell);

/** Slow-time weight of each row: the CPI's Hann taper at the row time times the row's own subcarrier
 * weight sum (its Hann taper over the observed subcarriers). Coherent (matched-filter) integration weights
 * every subcarrier sample equally, so a row counts in proportion to what it observed: a 2-PRB row weighed as
 * much as a 273-PRB row made the narrow grants' noise and offsets dominate the static residue (OTA, a 5 ms
 * TDD-periodic comb at +-200 Hz). 0 for an empty row. */
std::vector<double> slow_time_weights(const CfrWindow& w, const Axes& a);

/** Task 10 (GPU path): the RdResult::Waveform half of range_doppler() (ambiguity/leakage model,
 * independent of the RD cube itself), factored out as a standalone function so a GPU-computed RD
 * cube can get a CPU-built `wf` without paying for range_doppler()'s own (CPU) RD.v computation.
 * Bit-identical to the wf that range_doppler(w, a, los, sync) would have produced for the same
 * (w, a) -- range_doppler()'s wf-building code never reads `los`/`sync`, only row masks/times. */
RdResult::Waveform build_waveform(const CfrWindow& w, const Axes& a, bool kernels = true);

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
