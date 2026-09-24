/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_cuda_detect.h"

#include <stdexcept>

namespace nr_isac::coherent {

struct GpuDetect::Impl { GpuDetectTiming t; };
GpuDetect::GpuDetect() { throw std::runtime_error("coherent CUDA detect was not compiled (ENABLE_CHANNEL_SIM_CUDA=OFF)"); }
GpuDetect::~GpuDetect() = default;
std::vector<Detection> GpuDetect::run(const std::vector<float>&, const RdResult&, const Grid&, const Geometry&, const DetectParams&,
                                      const void*, const float*, void*)
{ throw std::runtime_error("coherent CUDA detect was not compiled"); }
const GpuDetectTiming& GpuDetect::timing() const { return impl_->t; }

} // namespace nr_isac::coherent
