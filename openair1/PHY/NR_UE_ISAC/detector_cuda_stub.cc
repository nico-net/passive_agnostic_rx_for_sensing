/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "detector_cuda.h"

#include <stdexcept>

namespace nr_isac {

struct CudaDetectorBackend::Impl {};

bool detector_cuda_available() { return false; }
void detector_cuda_warmup() {}
void detector_cuda_prefer_blocking_sync() {}

CudaDetectorBackend::CudaDetectorBackend(uint32_t,
                                         uint32_t,
                                         uint32_t,
                                         uint32_t,
                                         double,
                                         double,
                                         const std::vector<double>&,
                                         const std::vector<double>&,
                                         const std::vector<double>&,
                                         const std::vector<uint8_t>&,
                                         double,
                                         bool)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

CudaDetectorBackend::~CudaDetectorBackend() = default;

void CudaDetectorBackend::reset_cpi(double,
                                    double,
                                    const std::vector<double>&,
                                    const std::vector<double>&,
                                    const std::vector<double>&,
                                    const std::vector<uint8_t>&,
                                    double)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

void CudaDetectorBackend::reset_diagnostic_cpi(double,
                                               double,
                                               const std::vector<double>&,
                                               const std::vector<double>&,
                                               const std::vector<double>&,
                                               const std::vector<uint8_t>&,
                                               double)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

std::vector<double> CudaDetectorBackend::likelihood_map(
    const std::vector<std::complex<double>>&, uint32_t)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

std::vector<double> CudaDetectorBackend::diagnostic_likelihood_map(
    const std::vector<std::complex<float>>&, uint32_t)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

void CudaDetectorBackend::begin_diagnostic_likelihood_map(
    const std::vector<std::complex<float>>&, uint32_t)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

std::vector<double> CudaDetectorBackend::finish_diagnostic_likelihood_map()
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

CudaRefinementEvaluation CudaDetectorBackend::evaluate(double, double)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

double CudaDetectorBackend::weighted_energy() const
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

double CudaDetectorBackend::unweighted_energy() const
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

std::array<double, 2> CudaDetectorBackend::subtract_component(
    double, double, const std::vector<std::complex<double>>&)
{
  throw std::runtime_error("CUDA detector support was not compiled");
}

} // namespace nr_isac
