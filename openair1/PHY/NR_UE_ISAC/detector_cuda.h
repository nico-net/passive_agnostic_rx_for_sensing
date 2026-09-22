/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace nr_isac {

/** Host representation of the profile-likelihood derivatives accumulated on CUDA. */
struct CudaRefinementEvaluation {
  double objective = 0.0;
  std::array<double, 2> gradient{};
  std::array<double, 4> hessian{};
  std::vector<std::complex<double>> coherent;
};

/** Return true only when the CUDA detector was compiled and a usable device is present.
 * NR_ISAC_CUDA_DETECTOR=0 provides an explicit runtime CPU fallback for validation.
 */
bool detector_cuda_available();

/** Pay one-time CUDA context/cuFFT code-loading cost before CFR admission starts. */
void detector_cuda_warmup();

/** Per-CPI CUDA workspace for the expensive detector primitives.
 *
 * The owning detector keeps CLEAN control flow, thresholding, localization covariance, and
 * component collapse on the host. This workspace transcribes NumPy/CuPy's batched inverse FFT,
 * nonuniform slow-time coherent sum, and continuous-refinement sufficient statistics.
 */
class CudaDetectorBackend {
public:
  CudaDetectorBackend(uint32_t antennas,
                      uint32_t rows,
                      uint32_t subcarriers,
                      uint32_t range_bins,
                      double fc_hz,
                      double denominator,
                      const std::vector<double>& weights,
                      const std::vector<double>& times,
                      const std::vector<double>& rate_axis_mps,
                      const std::vector<uint8_t>& rate_allowed,
                      double rate_res_mps,
                      bool diagnostic_only = false);
  ~CudaDetectorBackend();

  CudaDetectorBackend(const CudaDetectorBackend&) = delete;
  CudaDetectorBackend& operator=(const CudaDetectorBackend&) = delete;

  /** (2026-09-22) Enable range-walk steering for this window and supply the lattice constants it
   * needs (declared physics, no fitted parameter): the range cell in metres and the slow-time
   * midpoint the map's range coordinate refers to. Disabled by default; the map is then bit-identical.
   */
  void set_range_walk(bool enabled, double range_res_m, double time_midpoint_s);

  /** Freeze the per-row range migration (in range bins) of the CLEAN proposal being refined and
   * subtracted, so refinement/subtraction use the same non-separable model the steered map found.
   * An empty vector restores the separable model.
   */
  void set_component_walk(const std::vector<double>& walk_bins);

  /** Rebind invariant CPI coordinates and clear the device residual while retaining allocations
   * and the cuFFT plan for another window with identical tensor dimensions.
   */
  void reset_cpi(double fc_hz,
                 double denominator,
                 const std::vector<double>& weights,
                 const std::vector<double>& times,
                 const std::vector<double>& rate_axis_mps,
                 const std::vector<uint8_t>& rate_allowed,
                 double rate_res_mps);

  /** Rebind only the coordinates needed by a map-only diagnostic. This deliberately avoids
   * uploading the double-precision CLEAN weights/times owned by the independent fused backend.
   */
  void reset_diagnostic_cpi(double fc_hz,
                            double denominator,
                            const std::vector<double>& weights,
                            const std::vector<double>& times,
                            const std::vector<double>& rate_axis_mps,
                            const std::vector<uint8_t>& rate_allowed,
                            double rate_res_mps);

  /** Upload the current CLEAN residual and return range-major [range][Doppler] likelihood. */
  std::vector<double> likelihood_map(const std::vector<std::complex<double>>& residual,
                                     uint32_t minimum_range_bin);

  /** (2026-09-20) Same computation as likelihood_map, but only the strongest finite positive
   * cell is located on the device and the three Doppler columns d-1, d, d+1 (each range_bins
   * long, Doppler index wrapped) are downloaded: CLEAN's proposal, its 3x3 interpolation and
   * the OS-CFAR column statistic need nothing else.  Replaces a 2 MB map transfer + host scan per
   * iteration by ~40 KB. */
  struct LikelihoodPeak {
    bool valid = false;
    uint32_t r = 0, d = 0;
    double score = 0.0;
    uint32_t range_bins = 0, rows = 0;
    std::vector<float> columns;   // 3 * range_bins: [d-1 | d | d+1]
    std::vector<float> range_slice;   // rows: all Doppler cells at range r (greatest-of CFAR column)
  };
  LikelihoodPeak likelihood_peak(const std::vector<std::complex<double>>& residual,
                                 uint32_t minimum_range_bin);

  /** Map-only complex64 input path. Observation weights isolate excluded rows on the device;
   * the CLEAN residual and its energy state are neither uploaded nor modified.
   */
  std::vector<double> diagnostic_likelihood_map(
      const std::vector<std::complex<float>>& samples,
      uint32_t minimum_range_bin);

  /** Start/finish split used to overlap the independent DL-only map with fused CLEAN. Only a
   * backend constructed with diagnostic_only=true accepts these calls.
   */
  void begin_diagnostic_likelihood_map(
      const std::vector<std::complex<float>>& samples,
      uint32_t minimum_range_bin);
  std::vector<double> finish_diagnostic_likelihood_map();

  /** Evaluate objective, gradient, Hessian, and per-antenna coherent sums at one point. */
  CudaRefinementEvaluation evaluate(double range_bin, double doppler_bin);

  /** Current residual energies; valid after the first likelihood_map upload. */
  double weighted_energy() const;
  double unweighted_energy() const;

  /** Apply the accepted sequential CLEAN component in-place and return {before, after} energy. */
  std::array<double, 2> subtract_component(
      double range_bin,
      double doppler_bin,
      const std::vector<std::complex<double>>& alpha);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nr_isac
