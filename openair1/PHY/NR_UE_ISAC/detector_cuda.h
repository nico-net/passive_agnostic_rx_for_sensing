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
                      double rate_res_mps);
  ~CudaDetectorBackend();

  CudaDetectorBackend(const CudaDetectorBackend&) = delete;
  CudaDetectorBackend& operator=(const CudaDetectorBackend&) = delete;

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

  /** Upload the current CLEAN residual and return range-major [range][Doppler] likelihood. */
  std::vector<double> likelihood_map(const std::vector<std::complex<double>>& residual,
                                     uint32_t minimum_range_bin);

  /** Evaluate a diagnostic map from the already-uploaded pre-CLEAN residual with alternate
   * observation weights. This does not alter the detector's weights, residual, or CLEAN state.
   */
  std::vector<double> diagnostic_likelihood_map(const std::vector<double>& weights,
                                                double denominator,
                                                uint32_t minimum_range_bin);

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
