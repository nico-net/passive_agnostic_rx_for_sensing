/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_cuda.h"

#include <stdexcept>

namespace nr_isac::coherent {

struct CudaCoherent::Impl {};

bool CudaCoherent::available() { return false; }

CudaCoherent::CudaCoherent() { throw std::runtime_error("coherent CUDA path was not compiled (ENABLE_CHANNEL_SIM_CUDA=OFF)"); }
CudaCoherent::~CudaCoherent() = default;

void CudaCoherent::upload(const CfrWindow&) { throw std::runtime_error("coherent CUDA path was not compiled"); }
LosEstimate CudaCoherent::find_los(const CfrWindow&, const Axes&, double, const std::array<double, kCh>*)
{ throw std::runtime_error("coherent CUDA path was not compiled"); }
RowSync CudaCoherent::estimate_row_sync(const CfrWindow&, const Axes&, const LosEstimate&)
{ throw std::runtime_error("coherent CUDA path was not compiled"); }

RdResult CudaCoherent::range_doppler(const CfrWindow&, const Axes&, const LosEstimate&, const RowSync&, bool)
{ throw std::runtime_error("coherent CUDA path was not compiled"); }

std::vector<Detection> CudaCoherent::detect(const Grid&, const Geometry&, const DetectParams&, std::vector<float>*)
{ throw std::runtime_error("coherent CUDA path was not compiled"); }

const std::vector<float>& CudaCoherent::last_envelope() const
{ static const std::vector<float> empty; return empty; }

void CudaCoherent::refine(std::vector<Detection>&, const Grid&, const Geometry&, const Calibration&, const SurveySigma&)
{ throw std::runtime_error("coherent CUDA path was not compiled"); }

CudaCoherent::Timing CudaCoherent::last_timing() const { return {}; }

} // namespace nr_isac::coherent
