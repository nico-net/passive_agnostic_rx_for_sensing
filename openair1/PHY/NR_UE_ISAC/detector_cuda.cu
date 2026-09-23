/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "detector_cuda.h"

#include "pipeline_types.h"

#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace nr_isac {
namespace {

void cuda_check(cudaError_t status, const char* operation)
{
  if (status != cudaSuccess)
    throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}

void cufft_check(cufftResult status, const char* operation)
{
  if (status != CUFFT_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed (cuFFT status "
                             + std::to_string(static_cast<int>(status)) + ")");
}

__device__ inline cufftDoubleComplex zadd(cufftDoubleComplex a, cufftDoubleComplex b)
{
  return make_cuDoubleComplex(a.x + b.x, a.y + b.y);
}

__device__ inline cufftDoubleComplex zmul(cufftDoubleComplex a, cufftDoubleComplex b)
{
  return make_cuDoubleComplex(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

__global__ void project_weighted(const cufftDoubleComplex* residual,
                                 cufftComplex* projected,
                                 const float* weights,
                                 size_t cells,
                                 size_t samples)
{
  for (size_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < samples;
       index += static_cast<size_t>(blockDim.x) * gridDim.x) {
    const float weight = weights[index % cells];
    projected[index].x = static_cast<float>(residual[index].x) * weight;
    projected[index].y = static_cast<float>(residual[index].y) * weight;
  }
}

__global__ void weight_projected(cufftComplex* projected,
                                 const float* weights,
                                 size_t cells,
                                 size_t samples)
{
  for (size_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < samples;
       index += static_cast<size_t>(blockDim.x) * gridDim.x) {
    const float weight = weights[index % cells];
    projected[index].x *= weight;
    projected[index].y *= weight;
  }
}

// (2026-09-22) RANGE-WALK STEERING (OUR ADAPTATION, NR_ISAC_RANGE_WALK=1).
// A point target with bistatic range rate v is at range r0 + v*(t - t_mid) at row time t, i.e. its
// energy migrates over v*T/dR range cells during a dwell T. At 75 ms this is < 0.2 cells and is
// irrelevant; at the 0.3 s accumulated dwells a car at 10-17 m/s walks 1-1.7 cells and the coherent
// sum smears. The matched filter for hypothesis (q, d) therefore reads the projected range profile
// of each row at q + v_d*(t_row - t_mid)/dR (linear interpolation on the IFFT lattice), which is the
// standard range-walk migration correction; v_d is the SAME hypothesis the slow-time steering uses,
// so no new search dimension and no extra transform is introduced -- one extra lattice read per row.
__device__ __forceinline__ cufftComplex projected_at(const cufftComplex* projected,
                                                     size_t base, uint32_t subcarriers,
                                                     double index)
{
  const double wrapped = index - floor(index / subcarriers) * subcarriers;
  const uint32_t q0 = static_cast<uint32_t>(wrapped);
  const float frac = static_cast<float>(wrapped - q0);
  const uint32_t q1 = (q0 + 1u) % subcarriers;
  const cufftComplex a = projected[base + q0];
  const cufftComplex b = projected[base + q1];
  return make_cuFloatComplex(a.x + frac * (b.x - a.x), a.y + frac * (b.y - a.y));
}

__global__ void likelihood_kernel(const cufftComplex* projected,
                                  const cufftComplex* slow_steering,
                                  const uint8_t* rate_allowed,
                                  float* likelihood,
                                  uint32_t antennas,
                                  uint32_t rows,
                                  uint32_t subcarriers,
                                  uint32_t range_bins,
                                  uint32_t minimum_range_bin,
                                  float denominator,
                                  const double* times,
                                  double time_midpoint_s,
                                  double rate_res_mps,
                                  double range_res_m,
                                  uint8_t range_walk)
{
  const size_t outputs = static_cast<size_t>(range_bins) * rows;
  for (size_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < outputs;
       index += static_cast<size_t>(blockDim.x) * gridDim.x) {
    const uint32_t range_bin = static_cast<uint32_t>(index / rows);
    const uint32_t doppler_bin = static_cast<uint32_t>(index % rows);
    if (range_bin < minimum_range_bin || !rate_allowed[doppler_bin] || !(denominator > 0.0)) {
      likelihood[index] = -INFINITY;
      continue;
    }
    float power = 0.0f;
    // v_d matches the reported detection rate: -(d - rows/2) * rate_res_mps.
    const double walk_rate = range_walk && range_res_m > 0.0
        ? -(static_cast<double>(doppler_bin) - static_cast<double>(rows / 2)) * rate_res_mps
        : 0.0;
    for (uint32_t antenna = 0; antenna < antennas; ++antenna) {
      cufftComplex coherent = make_cuFloatComplex(0.0f, 0.0f);
      for (uint32_t row = 0; row < rows; ++row) {
        const auto steering = slow_steering[static_cast<size_t>(doppler_bin) * rows + row];
        const size_t base = (static_cast<size_t>(antenna) * rows + row) * subcarriers;
        const size_t sample = base + range_bin;
        const auto value = range_walk
            ? projected_at(projected, base, subcarriers,
                           static_cast<double>(range_bin)
                               + walk_rate * (times[row] - time_midpoint_s) / range_res_m)
            : projected[sample];
        coherent.x += value.x * steering.x - value.y * steering.y;
        coherent.y += value.x * steering.y + value.y * steering.x;
      }
      power += coherent.x * coherent.x + coherent.y * coherent.y;
    }
    const float score = power / denominator;
    likelihood[index] = isfinite(score) && score > 0.0f ? score : -INFINITY;
  }
}

__global__ void refinement_templates(cufftDoubleComplex* range_steering,
                                     cufftDoubleComplex* doppler_steering,
                                     const double* times,
                                     uint32_t rows,
                                     uint32_t subcarriers,
                                     double fc_hz,
                                     double rate_res_mps,
                                     double range_bin,
                                     double doppler_bin)
{
  for (uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < subcarriers;
       index += blockDim.x * gridDim.x) {
    const double phase = -2.0 * PI * index * range_bin / subcarriers;
    double sine = 0.0, cosine = 0.0;
    sincos(phase, &sine, &cosine);
    range_steering[index] = make_cuDoubleComplex(cosine, sine);
  }
  for (uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < rows;
       index += blockDim.x * gridDim.x) {
    const double slope = 2.0 * PI * rate_res_mps * fc_hz * times[index] / C_MPS;
    const double phase = slope * (doppler_bin - static_cast<int>(rows / 2));
    double sine = 0.0, cosine = 0.0;
    sincos(phase, &sine, &cosine);
    doppler_steering[index] = make_cuDoubleComplex(cosine, sine);
  }
}

// (2026-09-22) Walk-consistent component model. When range-walk steering is active the CLEAN
// component is not separable in (subcarrier, row): the target sits at range_bin + walk[row], so its
// template carries the extra factor exp(-j2*pi*k*walk[row]/K). Detection, refinement and
// subtraction must use the SAME model or CLEAN removes a component it did not find. walk[] is
// frozen at the proposal's coarse Doppler bin, so the refinement gradients stay exact.
__device__ __forceinline__ cufftDoubleComplex walk_factor(const double* walk_bins, uint32_t row,
                                                          uint32_t subcarrier, uint32_t subcarriers)
{
  if (!walk_bins) return make_cuDoubleComplex(1.0, 0.0);
  const double phase = -2.0 * PI * subcarrier * walk_bins[row] / subcarriers;
  double sine = 0.0, cosine = 0.0;
  sincos(phase, &sine, &cosine);
  return make_cuDoubleComplex(cosine, sine);
}

constexpr uint32_t REFINEMENT_BLOCK_SIZE = 256;
constexpr uint32_t REFINEMENT_BLOCKS_PER_ANTENNA = 32;
constexpr uint32_t REFINEMENT_TERMS = 6;

__global__ void refinement_partials(const cufftDoubleComplex* residual,
                                    const double* weights,
                                    const double* times,
                                    const cufftDoubleComplex* range_steering,
                                    const cufftDoubleComplex* doppler_steering,
                                    double* partials,
                                    uint32_t antennas,
                                    uint32_t rows,
                                    uint32_t subcarriers,
                                    double fc_hz,
                                    double rate_res_mps,
                                    double range_bin,
                                    double doppler_bin,
                                    const double* walk_bins)
{
  const uint32_t antenna = blockIdx.x / REFINEMENT_BLOCKS_PER_ANTENNA;
  const uint32_t antenna_block = blockIdx.x % REFINEMENT_BLOCKS_PER_ANTENNA;
  if (antenna >= antennas) return;
  const size_t cells = static_cast<size_t>(rows) * subcarriers;
  cufftDoubleComplex sums[REFINEMENT_TERMS];
#pragma unroll
  for (uint32_t term = 0; term < REFINEMENT_TERMS; ++term)
    sums[term] = make_cuDoubleComplex(0.0, 0.0);

  const size_t first = static_cast<size_t>(antenna_block) * blockDim.x + threadIdx.x;
  const size_t stride = static_cast<size_t>(REFINEMENT_BLOCKS_PER_ANTENNA) * blockDim.x;
  for (size_t cell = first; cell < cells; cell += stride) {
    const uint32_t row = static_cast<uint32_t>(cell / subcarriers);
    const uint32_t subcarrier = static_cast<uint32_t>(cell % subcarriers);
    const double sr = -2.0 * PI * subcarrier / subcarriers;
    const double sd = 2.0 * PI * rate_res_mps * fc_hz * times[row] / C_MPS;
    const auto steering = zmul(zmul(range_steering[subcarrier], doppler_steering[row]),
                               walk_factor(walk_bins, row, subcarrier, subcarriers));
    const auto value = residual[static_cast<size_t>(antenna) * cells + cell];
    const double weight = weights[cell];
    // residual * conj(steering), with the observation weight applied exactly once.
    const cufftDoubleComplex projected = make_cuDoubleComplex(
        weight * (value.x * steering.x + value.y * steering.y),
        weight * (value.y * steering.x - value.x * steering.y));
    sums[0] = zadd(sums[0], projected);
    sums[1] = zadd(sums[1], make_cuDoubleComplex(projected.y * sr, -projected.x * sr));
    sums[2] = zadd(sums[2], make_cuDoubleComplex(projected.y * sd, -projected.x * sd));
    sums[3] = zadd(sums[3], make_cuDoubleComplex(-projected.x * sr * sr,
                                                 -projected.y * sr * sr));
    sums[4] = zadd(sums[4], make_cuDoubleComplex(-projected.x * sd * sd,
                                                 -projected.y * sd * sd));
    sums[5] = zadd(sums[5], make_cuDoubleComplex(-projected.x * sr * sd,
                                                 -projected.y * sr * sd));
  }

  __shared__ cufftDoubleComplex shared[REFINEMENT_TERMS][REFINEMENT_BLOCK_SIZE];
#pragma unroll
  for (uint32_t term = 0; term < REFINEMENT_TERMS; ++term)
    shared[term][threadIdx.x] = sums[term];
  __syncthreads();
  for (uint32_t width = blockDim.x / 2; width > 0; width >>= 1) {
    if (threadIdx.x < width) {
#pragma unroll
      for (uint32_t term = 0; term < REFINEMENT_TERMS; ++term)
        shared[term][threadIdx.x] = zadd(shared[term][threadIdx.x],
                                         shared[term][threadIdx.x + width]);
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    const size_t output = (static_cast<size_t>(antenna) * REFINEMENT_BLOCKS_PER_ANTENNA
                           + antenna_block) * REFINEMENT_TERMS * 2;
#pragma unroll
    for (uint32_t term = 0; term < REFINEMENT_TERMS; ++term) {
      partials[output + 2 * term] = shared[term][0].x;
      partials[output + 2 * term + 1] = shared[term][0].y;
    }
  }
}

__global__ void subtract_component_kernel(cufftDoubleComplex* residual,
                                          const cufftDoubleComplex* range_steering,
                                          const cufftDoubleComplex* doppler_steering,
                                          const cufftDoubleComplex* alpha,
                                          uint32_t rows,
                                          uint32_t subcarriers,
                                          size_t cells,
                                          size_t samples,
                                          const double* walk_bins)
{
  for (size_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < samples;
       index += static_cast<size_t>(blockDim.x) * gridDim.x) {
    const uint32_t antenna = static_cast<uint32_t>(index / cells);
    const size_t cell = index % cells;
    const uint32_t row = static_cast<uint32_t>(cell / subcarriers);
    const uint32_t subcarrier = static_cast<uint32_t>(cell % subcarriers);
    const auto steering = zmul(zmul(range_steering[subcarrier], doppler_steering[row]),
                               walk_factor(walk_bins, row, subcarrier, subcarriers));
    const auto fitted = zmul(alpha[antenna], steering);
    residual[index].x -= fitted.x;
    residual[index].y -= fitted.y;
  }
}

constexpr uint32_t ENERGY_BLOCK_SIZE = 256;
constexpr uint32_t ENERGY_BLOCKS = 256;

__global__ void energy_partials(const cufftDoubleComplex* residual,
                                const double* weights,
                                double* partials,
                                size_t cells,
                                size_t samples)
{
  double weighted = 0.0;
  double unweighted = 0.0;
  for (size_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < samples;
       index += static_cast<size_t>(blockDim.x) * gridDim.x) {
    const auto value = residual[index];
    const double energy = value.x * value.x + value.y * value.y;
    unweighted += energy;
    weighted += weights[index % cells] * energy;
  }
  __shared__ double weighted_shared[ENERGY_BLOCK_SIZE];
  __shared__ double unweighted_shared[ENERGY_BLOCK_SIZE];
  weighted_shared[threadIdx.x] = weighted;
  unweighted_shared[threadIdx.x] = unweighted;
  __syncthreads();
  for (uint32_t width = blockDim.x / 2; width > 0; width >>= 1) {
    if (threadIdx.x < width) {
      weighted_shared[threadIdx.x] += weighted_shared[threadIdx.x + width];
      unweighted_shared[threadIdx.x] += unweighted_shared[threadIdx.x + width];
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    partials[2 * blockIdx.x] = weighted_shared[0];
    partials[2 * blockIdx.x + 1] = unweighted_shared[0];
  }
}

} // namespace

struct CudaDetectorBackend::Impl {
  uint32_t antennas = 0;
  uint32_t rows = 0;
  uint32_t subcarriers = 0;
  uint32_t range_bins = 0;
  double fc_hz = 0.0;
  double denominator = 0.0;
  double rate_res_mps = 0.0;
  double range_res_m = 0.0;
  double time_midpoint_s = 0.0;
  uint8_t range_walk = 0;
  double* component_walk = nullptr;     // device, rows entries; null while the model is separable
  bool component_walk_set = false;
  size_t cells = 0;
  size_t samples = 0;
  cufftHandle range_plan{};
  bool plan_valid = false;
  bool diagnostic_only = false;
  bool diagnostic_pending = false;
  cudaStream_t stream = nullptr;
  cudaStream_t diagnostic_stream = nullptr;
  bool residual_loaded = false;
  cufftDoubleComplex* residual = nullptr;
  cufftComplex* projected = nullptr;
  double* weights = nullptr;
  float* weights_float = nullptr;
  double* times = nullptr;
  cufftComplex* slow_steering = nullptr;
  cufftDoubleComplex* refinement_range_steering = nullptr;
  cufftDoubleComplex* refinement_doppler_steering = nullptr;
  uint8_t* rate_allowed = nullptr;
  float* likelihood = nullptr;
  double* refinement = nullptr;
  cufftDoubleComplex* alpha = nullptr;
  double* energy = nullptr;
  cufftComplex* pinned_samples = nullptr;
  float* pinned_weights = nullptr;
  cufftComplex* pinned_slow_steering = nullptr;
  uint8_t* pinned_rate_allowed = nullptr;
  float* pinned_likelihood = nullptr;
  std::vector<float> host_likelihood;
  float* argmax_values = nullptr; unsigned int* argmax_indices = nullptr;
  std::vector<float> host_argmax_values; std::vector<unsigned int> host_argmax_indices;
  std::vector<double> host_refinement;
  std::array<double, ENERGY_BLOCKS * 2> host_energy{};
  double current_weighted_energy = 0.0;
  double current_unweighted_energy = 0.0;

  ~Impl()
  {
    if (stream) cudaStreamSynchronize(stream);
    if (diagnostic_stream) cudaStreamSynchronize(diagnostic_stream);
    if (plan_valid) cufftDestroy(range_plan);
    if (stream) cudaStreamDestroy(stream);
    if (diagnostic_stream) cudaStreamDestroy(diagnostic_stream);
    cudaFreeHost(pinned_likelihood);
    cudaFreeHost(pinned_rate_allowed);
    cudaFreeHost(pinned_slow_steering);
    cudaFreeHost(pinned_weights);
    cudaFreeHost(pinned_samples);
    cudaFree(refinement);
    cudaFree(energy);
    cudaFree(alpha);
    cudaFree(likelihood); cudaFree(argmax_values); cudaFree(argmax_indices);
    cudaFree(rate_allowed);
    cudaFree(component_walk);
    cudaFree(refinement_doppler_steering);
    cudaFree(refinement_range_steering);
    cudaFree(slow_steering);
    cudaFree(times);
    cudaFree(weights_float);
    cudaFree(weights);
    cudaFree(projected);
    cudaFree(residual);
  }
};

bool detector_cuda_available()
{
  const char* choice = std::getenv("NR_ISAC_CUDA_DETECTOR");
  if (choice && (!std::strcmp(choice, "0") || !std::strcmp(choice, "off")
                 || !std::strcmp(choice, "false")))
    return false;
  int devices = 0;
  const cudaError_t status = cudaGetDeviceCount(&devices);
  if (status != cudaSuccess) {
    cudaGetLastError();
    return false;
  }
  return devices > 0;
}

void detector_cuda_prefer_blocking_sync()
{
  if (detector_cuda_available())
    cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
}

void detector_cuda_warmup()
{
  static std::once_flag warmed;
  std::call_once(warmed, []() {
    cuda_check(cudaFree(nullptr), "initialize CUDA detector context");
    cudaFuncAttributes attributes{};
    cuda_check(cudaFuncGetAttributes(&attributes, project_weighted), "load weighting kernel");
    cuda_check(cudaFuncGetAttributes(&attributes, weight_projected),
               "load diagnostic weighting kernel");
    cuda_check(cudaFuncGetAttributes(&attributes, likelihood_kernel), "load likelihood kernel");
    cuda_check(cudaFuncGetAttributes(&attributes, refinement_templates), "load refinement-template kernel");
    cuda_check(cudaFuncGetAttributes(&attributes, refinement_partials), "load refinement kernel");
    cuda_check(cudaFuncGetAttributes(&attributes, subtract_component_kernel), "load subtraction kernel");
    cuda_check(cudaFuncGetAttributes(&attributes, energy_partials), "load energy kernel");

    constexpr int length = 3276;
    constexpr int batch = 4 * 192;
    cufftComplex* workspace = nullptr;
    cufftHandle plan{};
    cuda_check(cudaMalloc(&workspace, static_cast<size_t>(length) * batch * sizeof(*workspace)),
               "allocate detector warmup workspace");
    try {
      cuda_check(cudaMemset(workspace, 0,
                            static_cast<size_t>(length) * batch * sizeof(*workspace)),
                 "clear detector warmup workspace");
      int dimensions[] = {length};
      cufft_check(cufftPlanMany(&plan, 1, dimensions, nullptr, 1, length, nullptr, 1, length,
                               CUFFT_C2C, batch),
                  "plan detector warmup transform");
      cufft_check(cufftExecC2C(plan, workspace, workspace, CUFFT_INVERSE),
                  "execute detector warmup transform");
      cuda_check(cudaDeviceSynchronize(), "synchronize detector warmup");
    } catch (...) {
      if (plan) cufftDestroy(plan);
      cudaFree(workspace);
      throw;
    }
    cufftDestroy(plan);
    cudaFree(workspace);
  });
}

CudaDetectorBackend::CudaDetectorBackend(uint32_t antennas,
                                         uint32_t rows,
                                         uint32_t subcarriers,
                                         uint32_t range_bins,
                                         double fc_hz,
                                         double denominator,
                                         const std::vector<double>& host_weights,
                                         const std::vector<double>& host_times,
                                         const std::vector<double>& host_rates,
                                         const std::vector<uint8_t>& host_rate_allowed,
                                         double rate_res_mps,
                                         bool diagnostic_only)
    : impl_(std::make_unique<Impl>())
{
  if (!antennas || !rows || !subcarriers || !range_bins || range_bins > subcarriers
      || host_weights.size() != static_cast<size_t>(rows) * subcarriers
      || host_times.size() != rows || host_rates.size() != rows
      || host_rate_allowed.size() != rows)
    throw std::invalid_argument("invalid CUDA detector dimensions");
  auto& p = *impl_;
  p.antennas = antennas;
  p.rows = rows;
  p.subcarriers = subcarriers;
  p.range_bins = range_bins;
  p.diagnostic_only = diagnostic_only;
  p.cells = static_cast<size_t>(rows) * subcarriers;
  p.samples = p.cells * antennas;
  if (!diagnostic_only) {
    p.host_likelihood.resize(static_cast<size_t>(range_bins) * rows);
    p.host_refinement.resize(static_cast<size_t>(antennas) * REFINEMENT_BLOCKS_PER_ANTENNA
                             * REFINEMENT_TERMS * 2);
    cuda_check(cudaMalloc(&p.residual, p.samples * sizeof(*p.residual)), "cudaMalloc residual");
  }
  cuda_check(cudaMalloc(&p.projected, p.samples * sizeof(*p.projected)), "cudaMalloc FFT workspace");
  if (!diagnostic_only)
    cuda_check(cudaMalloc(&p.weights, p.cells * sizeof(*p.weights)), "cudaMalloc weights");
  cuda_check(cudaMalloc(&p.weights_float, p.cells * sizeof(*p.weights_float)),
             "cudaMalloc float weights");
  if (!diagnostic_only)
    cuda_check(cudaMalloc(&p.times, rows * sizeof(*p.times)), "cudaMalloc times");
  cuda_check(cudaMalloc(&p.slow_steering,
                        static_cast<size_t>(rows) * rows * sizeof(*p.slow_steering)),
             "cudaMalloc slow-time steering");
  if (!diagnostic_only) {
    cuda_check(cudaMalloc(&p.refinement_range_steering,
                          subcarriers * sizeof(*p.refinement_range_steering)),
               "cudaMalloc refinement range steering");
    cuda_check(cudaMalloc(&p.refinement_doppler_steering,
                          rows * sizeof(*p.refinement_doppler_steering)),
               "cudaMalloc refinement Doppler steering");
  }
  cuda_check(cudaMalloc(&p.rate_allowed, rows * sizeof(*p.rate_allowed)), "cudaMalloc rate mask");
  cuda_check(cudaMalloc(&p.likelihood,
                        static_cast<size_t>(range_bins) * rows * sizeof(*p.likelihood)),
             "cudaMalloc likelihood");
  if (!diagnostic_only) {
    cuda_check(cudaStreamCreateWithFlags(&p.stream, cudaStreamNonBlocking),
               "create detector CUDA stream");
    cuda_check(cudaMalloc(&p.refinement, p.host_refinement.size() * sizeof(*p.refinement)),
               "cudaMalloc refinement partials");
    cuda_check(cudaMalloc(&p.alpha, antennas * sizeof(*p.alpha)), "cudaMalloc CLEAN amplitudes");
    cuda_check(cudaMalloc(&p.energy, p.host_energy.size() * sizeof(*p.energy)),
               "cudaMalloc energy partials");
  } else {
    cuda_check(cudaStreamCreateWithFlags(&p.diagnostic_stream, cudaStreamNonBlocking),
               "create diagnostic CUDA stream");
    cuda_check(cudaMallocHost(&p.pinned_samples, p.samples * sizeof(*p.pinned_samples)),
               "allocate pinned diagnostic samples");
    cuda_check(cudaMallocHost(&p.pinned_weights, p.cells * sizeof(*p.pinned_weights)),
               "allocate pinned diagnostic weights");
    cuda_check(cudaMallocHost(&p.pinned_slow_steering,
                              static_cast<size_t>(rows) * rows * sizeof(*p.pinned_slow_steering)),
               "allocate pinned diagnostic steering");
    cuda_check(cudaMallocHost(&p.pinned_rate_allowed, rows * sizeof(*p.pinned_rate_allowed)),
               "allocate pinned diagnostic rate mask");
    cuda_check(cudaMallocHost(&p.pinned_likelihood,
                              static_cast<size_t>(range_bins) * rows * sizeof(*p.pinned_likelihood)),
               "allocate pinned diagnostic likelihood");
  }
  int dimensions[] = {static_cast<int>(subcarriers)};
  cufft_check(cufftPlanMany(&p.range_plan, 1, dimensions,
                           nullptr, 1, static_cast<int>(subcarriers),
                           nullptr, 1, static_cast<int>(subcarriers),
                           CUFFT_C2C, static_cast<int>(antennas * rows)),
              "cufftPlanMany detector range transform");
  p.plan_valid = true;
  if (diagnostic_only) {
    cufft_check(cufftSetStream(p.range_plan, p.diagnostic_stream),
                "bind diagnostic cuFFT stream");
    reset_diagnostic_cpi(fc_hz, denominator, host_weights, host_times, host_rates,
                         host_rate_allowed, rate_res_mps);
  } else {
    cufft_check(cufftSetStream(p.range_plan, p.stream),
                "bind detector cuFFT nonblocking stream");
    reset_cpi(fc_hz, denominator, host_weights, host_times, host_rates, host_rate_allowed,
              rate_res_mps);
  }
}

CudaDetectorBackend::~CudaDetectorBackend() = default;

void CudaDetectorBackend::set_range_walk(bool enabled, double range_res_m, double time_midpoint_s)
{
  auto& p = *impl_;
  p.range_walk = enabled ? 1u : 0u;
  p.range_res_m = range_res_m;
  p.time_midpoint_s = time_midpoint_s;
  if (!enabled) p.component_walk_set = false;
}

void CudaDetectorBackend::set_component_walk(const std::vector<double>& walk_bins)
{
  auto& p = *impl_;
  if (walk_bins.empty()) { p.component_walk_set = false; return; }
  if (walk_bins.size() != p.rows)
    throw std::invalid_argument("CLEAN component walk shape mismatch");
  if (!p.component_walk)
    cuda_check(cudaMalloc(&p.component_walk, p.rows * sizeof(double)), "cudaMalloc component walk");
  cuda_check(cudaMemcpyAsync(p.component_walk, walk_bins.data(), p.rows * sizeof(double),
                             cudaMemcpyHostToDevice, p.stream), "upload component walk");
  cuda_check(cudaStreamSynchronize(p.stream), "synchronize component walk upload");
  p.component_walk_set = true;
}

void CudaDetectorBackend::reset_cpi(double fc_hz,
                                    double denominator,
                                    const std::vector<double>& host_weights,
                                    const std::vector<double>& host_times,
                                    const std::vector<double>& host_rates,
                                    const std::vector<uint8_t>& host_rate_allowed,
                                    double rate_res_mps)
{
  auto& p = *impl_;
  if (p.diagnostic_only)
    throw std::logic_error("full detector reset requested from diagnostic-only backend");
  if (host_weights.size() != p.cells || host_times.size() != p.rows
      || host_rates.size() != p.rows || host_rate_allowed.size() != p.rows)
    throw std::invalid_argument("CUDA detector reset shape mismatch");
  p.fc_hz = fc_hz;
  p.denominator = denominator * p.antennas;
  p.rate_res_mps = rate_res_mps;
  p.residual_loaded = false;
  p.current_weighted_energy = 0.0;
  p.current_unweighted_energy = 0.0;
  cuda_check(cudaMemcpyAsync(p.weights, host_weights.data(), p.cells * sizeof(*p.weights),
                             cudaMemcpyHostToDevice, p.stream),
             "upload detector weights");
  std::vector<float> float_weights(host_weights.begin(), host_weights.end());
  cuda_check(cudaMemcpyAsync(p.weights_float, float_weights.data(),
                        p.cells * sizeof(*p.weights_float), cudaMemcpyHostToDevice, p.stream),
             "upload float detector weights");
  cuda_check(cudaMemcpyAsync(p.times, host_times.data(), p.rows * sizeof(*p.times),
                             cudaMemcpyHostToDevice, p.stream),
             "upload detector times");
  std::vector<cufftComplex> steering(static_cast<size_t>(p.rows) * p.rows);
  for (uint32_t doppler = 0; doppler < p.rows; ++doppler)
    for (uint32_t row = 0; row < p.rows; ++row) {
      const double phase = 2.0 * PI * host_rates[doppler] * fc_hz * host_times[row] / C_MPS;
      steering[static_cast<size_t>(doppler) * p.rows + row] = {
          static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
  cuda_check(cudaMemcpyAsync(p.slow_steering, steering.data(),
                        steering.size() * sizeof(*p.slow_steering), cudaMemcpyHostToDevice,
                        p.stream),
             "upload detector slow-time steering");
  cuda_check(cudaMemcpyAsync(p.rate_allowed, host_rate_allowed.data(),
                        p.rows * sizeof(*p.rate_allowed), cudaMemcpyHostToDevice, p.stream),
             "upload detector rate mask");
  cuda_check(cudaStreamSynchronize(p.stream), "synchronize detector CPI coordinates");
}

void CudaDetectorBackend::reset_diagnostic_cpi(
    double fc_hz,
    double denominator,
    const std::vector<double>& host_weights,
    const std::vector<double>& host_times,
    const std::vector<double>& host_rates,
    const std::vector<uint8_t>& host_rate_allowed,
    double rate_res_mps)
{
  auto& p = *impl_;
  if (!p.diagnostic_only)
    throw std::logic_error("diagnostic reset requested from full detector backend");
  if (p.diagnostic_pending)
    throw std::logic_error("diagnostic reset requested while a map is pending");
  if (host_weights.size() != p.cells || host_times.size() != p.rows
      || host_rates.size() != p.rows || host_rate_allowed.size() != p.rows)
    throw std::invalid_argument("CUDA diagnostic reset shape mismatch");
  p.fc_hz = fc_hz;
  p.denominator = denominator * p.antennas;
  p.rate_res_mps = rate_res_mps;
  p.residual_loaded = false;
  for (size_t cell = 0; cell < p.cells; ++cell)
    p.pinned_weights[cell] = static_cast<float>(host_weights[cell]);
  for (uint32_t doppler = 0; doppler < p.rows; ++doppler)
    for (uint32_t row = 0; row < p.rows; ++row) {
      const double phase = 2.0 * PI * host_rates[doppler] * fc_hz * host_times[row] / C_MPS;
      p.pinned_slow_steering[static_cast<size_t>(doppler) * p.rows + row] = {
          static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
  std::memcpy(p.pinned_rate_allowed, host_rate_allowed.data(), p.rows);
  try {
    cuda_check(cudaMemcpyAsync(p.weights_float, p.pinned_weights,
                               p.cells * sizeof(*p.weights_float), cudaMemcpyHostToDevice,
                               p.diagnostic_stream),
               "upload diagnostic weights");
    cuda_check(cudaMemcpyAsync(p.slow_steering, p.pinned_slow_steering,
                               static_cast<size_t>(p.rows) * p.rows * sizeof(*p.slow_steering),
                               cudaMemcpyHostToDevice, p.diagnostic_stream),
               "upload diagnostic slow-time steering");
    cuda_check(cudaMemcpyAsync(p.rate_allowed, p.pinned_rate_allowed,
                               p.rows * sizeof(*p.rate_allowed), cudaMemcpyHostToDevice,
                               p.diagnostic_stream),
               "upload diagnostic rate mask");
  } catch (...) {
    cudaStreamSynchronize(p.diagnostic_stream);
    throw;
  }
}


// (2026-09-20) Block-wise argmax of finite positive cells; the host reduces the per-block results.
__global__ void argmax_blocks_kernel(const float* __restrict__ values, size_t count,
                                     float* __restrict__ block_values, unsigned int* __restrict__ block_indices)
{
  __shared__ float sv[256];
  __shared__ unsigned int si[256];
  float best = -1.0f; unsigned int best_index = 0u;
  for (size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += (size_t)gridDim.x * blockDim.x) {
    const float v = values[i];
    if (isfinite(v) && v > 0.0f && (v > best || (v == best && i < best_index))) { best = v; best_index = (unsigned int)i; }
  }
  sv[threadIdx.x] = best; si[threadIdx.x] = best_index;
  __syncthreads();
  for (unsigned int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
    if (threadIdx.x < stride) {
      const float ov = sv[threadIdx.x + stride]; const unsigned int oi = si[threadIdx.x + stride];
      if (ov > sv[threadIdx.x] || (ov == sv[threadIdx.x] && oi < si[threadIdx.x])) { sv[threadIdx.x] = ov; si[threadIdx.x] = oi; }
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) { block_values[blockIdx.x] = sv[0]; block_indices[blockIdx.x] = si[0]; }
}

template<typename DetectorState>
std::vector<double> finish_likelihood_map(DetectorState& p, uint32_t minimum_range_bin,
                                          cudaStream_t stream)
{
  cufft_check(cufftExecC2C(p.range_plan, p.projected, p.projected, CUFFT_INVERSE),
              "execute detector range transform");
  const size_t map_cells = static_cast<size_t>(p.range_bins) * p.rows;
  const uint32_t map_blocks = static_cast<uint32_t>(
      std::min<size_t>(65535, (map_cells + 127) / 128));
  likelihood_kernel<<<map_blocks, 128, 0, stream>>>(p.projected, p.slow_steering, p.rate_allowed,
                                        p.likelihood, p.antennas, p.rows, p.subcarriers,
                                        p.range_bins, minimum_range_bin,
                                        static_cast<float>(p.denominator),
                                        p.times, p.time_midpoint_s, p.rate_res_mps,
                                        p.range_res_m, p.range_walk);
  cuda_check(cudaGetLastError(), "launch detector likelihood kernel");
  cuda_check(cudaMemcpyAsync(p.host_likelihood.data(), p.likelihood,
                             map_cells * sizeof(float), cudaMemcpyDeviceToHost, stream),
             "download detector likelihood");
  cuda_check(cudaStreamSynchronize(stream), "synchronize detector likelihood");
  return std::vector<double>(p.host_likelihood.begin(), p.host_likelihood.end());
}

std::vector<double> CudaDetectorBackend::likelihood_map(
    const std::vector<std::complex<double>>& host_residual, uint32_t minimum_range_bin)
{
  auto& p = *impl_;
  if (p.diagnostic_only)
    throw std::logic_error("CLEAN likelihood requested from diagnostic-only backend");
  if (host_residual.size() != p.samples || minimum_range_bin >= p.range_bins)
    throw std::invalid_argument("CUDA detector residual shape mismatch");
  if (!p.residual_loaded) {
    static_assert(sizeof(std::complex<double>) == sizeof(cufftDoubleComplex));
    cuda_check(cudaMemcpyAsync(p.residual, host_residual.data(),
                               p.samples * sizeof(*p.residual), cudaMemcpyHostToDevice,
                               p.stream), "upload detector residual");
    energy_partials<<<ENERGY_BLOCKS, ENERGY_BLOCK_SIZE, 0, p.stream>>>(
        p.residual, p.weights, p.energy, p.cells, p.samples);
    cuda_check(cudaGetLastError(), "launch initial detector-energy kernel");
    cuda_check(cudaMemcpyAsync(p.host_energy.data(), p.energy,
                          p.host_energy.size() * sizeof(double), cudaMemcpyDeviceToHost,
                          p.stream),
               "download initial detector energy");
    cuda_check(cudaStreamSynchronize(p.stream), "synchronize initial detector energy");
    p.current_weighted_energy = 0.0;
    p.current_unweighted_energy = 0.0;
    for (uint32_t block = 0; block < ENERGY_BLOCKS; ++block) {
      p.current_weighted_energy += p.host_energy[2 * block];
      p.current_unweighted_energy += p.host_energy[2 * block + 1];
    }
    p.residual_loaded = true;
  }
  const uint32_t blocks = static_cast<uint32_t>(std::min<size_t>(65535, (p.samples + 255) / 256));
  project_weighted<<<blocks, 256, 0, p.stream>>>(
      p.residual, p.projected, p.weights_float, p.cells, p.samples);
  cuda_check(cudaGetLastError(), "launch detector weighting kernel");
  return finish_likelihood_map(p, minimum_range_bin, p.stream);
}

CudaDetectorBackend::LikelihoodPeak CudaDetectorBackend::likelihood_peak(
    const std::vector<std::complex<double>>& host_residual, uint32_t minimum_range_bin)
{
  auto& p = *impl_;
  if (p.diagnostic_only)
    throw std::logic_error("CLEAN likelihood requested from diagnostic-only backend");
  if (host_residual.size() != p.samples || minimum_range_bin >= p.range_bins)
    throw std::invalid_argument("CUDA detector residual shape mismatch");
  if (!p.residual_loaded) {
    static_assert(sizeof(std::complex<double>) == sizeof(cufftDoubleComplex));
    cuda_check(cudaMemcpyAsync(p.residual, host_residual.data(),
                               p.samples * sizeof(*p.residual), cudaMemcpyHostToDevice,
                               p.stream), "upload detector residual");
    energy_partials<<<ENERGY_BLOCKS, ENERGY_BLOCK_SIZE, 0, p.stream>>>(
        p.residual, p.weights, p.energy, p.cells, p.samples);
    cuda_check(cudaGetLastError(), "launch initial detector-energy kernel");
    cuda_check(cudaMemcpyAsync(p.host_energy.data(), p.energy,
                          p.host_energy.size() * sizeof(double), cudaMemcpyDeviceToHost,
                          p.stream),
               "download initial detector energy");
    cuda_check(cudaStreamSynchronize(p.stream), "synchronize initial detector energy");
    p.current_weighted_energy = 0.0;
    p.current_unweighted_energy = 0.0;
    for (uint32_t block = 0; block < ENERGY_BLOCKS; ++block) {
      p.current_weighted_energy += p.host_energy[2 * block];
      p.current_unweighted_energy += p.host_energy[2 * block + 1];
    }
    p.residual_loaded = true;
  }
  const uint32_t blocks = static_cast<uint32_t>(std::min<size_t>(65535, (p.samples + 255) / 256));
  project_weighted<<<blocks, 256, 0, p.stream>>>(
      p.residual, p.projected, p.weights_float, p.cells, p.samples);
  cuda_check(cudaGetLastError(), "launch detector weighting kernel");
  {
    cufft_check(cufftExecC2C(p.range_plan, p.projected, p.projected, CUFFT_INVERSE),
                "execute detector range transform");
    const size_t map_cells = static_cast<size_t>(p.range_bins) * p.rows;
    const uint32_t map_blocks = static_cast<uint32_t>(std::min<size_t>(65535, (map_cells + 127) / 128));
    likelihood_kernel<<<map_blocks, 128, 0, p.stream>>>(p.projected, p.slow_steering, p.rate_allowed,
                                          p.likelihood, p.antennas, p.rows, p.subcarriers,
                                          p.range_bins, minimum_range_bin,
                                          static_cast<float>(p.denominator),
                                          p.times, p.time_midpoint_s, p.rate_res_mps,
                                          p.range_res_m, p.range_walk);
    cuda_check(cudaGetLastError(), "launch detector likelihood kernel");
    constexpr uint32_t kArgBlocks = 512;
    if (!p.argmax_values) {
      cuda_check(cudaMalloc(&p.argmax_values, kArgBlocks * sizeof(float)), "cudaMalloc argmax values");
      cuda_check(cudaMalloc(&p.argmax_indices, kArgBlocks * sizeof(unsigned int)), "cudaMalloc argmax indices");
      p.host_argmax_values.resize(kArgBlocks); p.host_argmax_indices.resize(kArgBlocks);
    }
    argmax_blocks_kernel<<<kArgBlocks, 256, 0, p.stream>>>(p.likelihood, map_cells, p.argmax_values, p.argmax_indices);
    cuda_check(cudaGetLastError(), "launch detector argmax kernel");
    cuda_check(cudaMemcpyAsync(p.host_argmax_values.data(), p.argmax_values, kArgBlocks * sizeof(float), cudaMemcpyDeviceToHost, p.stream), "download argmax values");
    cuda_check(cudaMemcpyAsync(p.host_argmax_indices.data(), p.argmax_indices, kArgBlocks * sizeof(unsigned int), cudaMemcpyDeviceToHost, p.stream), "download argmax indices");
    cuda_check(cudaStreamSynchronize(p.stream), "synchronize detector argmax");
    LikelihoodPeak out; out.range_bins = p.range_bins; out.rows = p.rows;
    float best = -1.0f; size_t best_index = 0;
    for (uint32_t b = 0; b < kArgBlocks; ++b) {
      const float v = p.host_argmax_values[b]; const size_t i = p.host_argmax_indices[b];
      if (v > 0.0f && (v > best || (v == best && i < best_index))) { best = v; best_index = i; }
    }
    if (!(best > 0.0f)) return out;
    out.valid = true; out.score = static_cast<double>(best);
    out.r = static_cast<uint32_t>(best_index / p.rows); out.d = static_cast<uint32_t>(best_index % p.rows);
    out.columns.resize(3 * static_cast<size_t>(p.range_bins));
    for (int j = -1; j <= 1; ++j) {
      const uint32_t dd = static_cast<uint32_t>(((static_cast<int64_t>(out.d) + j) % static_cast<int64_t>(p.rows) + p.rows) % p.rows);
      cuda_check(cudaMemcpy2DAsync(out.columns.data() + static_cast<size_t>(j + 1) * p.range_bins, sizeof(float),
                                   p.likelihood + dd, static_cast<size_t>(p.rows) * sizeof(float), sizeof(float), p.range_bins,
                                   cudaMemcpyDeviceToHost, p.stream), "download peak Doppler columns");
    }
    out.range_slice.resize(p.rows);
    cuda_check(cudaMemcpyAsync(out.range_slice.data(), p.likelihood + static_cast<size_t>(out.r) * p.rows,
                               static_cast<size_t>(p.rows) * sizeof(float), cudaMemcpyDeviceToHost, p.stream),
               "download peak range slice");
    cuda_check(cudaStreamSynchronize(p.stream), "synchronize peak columns");
    return out;
  }
}

std::vector<double> CudaDetectorBackend::diagnostic_likelihood_map(
    const std::vector<std::complex<float>>& host_samples,
    uint32_t minimum_range_bin)
{
  begin_diagnostic_likelihood_map(host_samples, minimum_range_bin);
  return finish_diagnostic_likelihood_map();
}

void CudaDetectorBackend::begin_diagnostic_likelihood_map(
    const std::vector<std::complex<float>>& host_samples,
    uint32_t minimum_range_bin)
{
  auto& p = *impl_;
  if (!p.diagnostic_only)
    throw std::logic_error("diagnostic map requested from full detector backend");
  if (p.diagnostic_pending)
    throw std::logic_error("a diagnostic map is already pending");
  if (host_samples.size() != p.samples || minimum_range_bin >= p.range_bins)
    throw std::invalid_argument("CUDA diagnostic samples shape mismatch");
  static_assert(sizeof(std::complex<float>) == sizeof(cufftComplex));
  std::memcpy(p.pinned_samples, host_samples.data(), p.samples * sizeof(*p.pinned_samples));
  p.diagnostic_pending = true;
  try {
    cuda_check(cudaMemcpyAsync(p.projected, p.pinned_samples,
                               p.samples * sizeof(*p.projected), cudaMemcpyHostToDevice,
                               p.diagnostic_stream),
               "upload diagnostic complex64 samples");
    const uint32_t blocks = static_cast<uint32_t>(
        std::min<size_t>(65535, (p.samples + 255) / 256));
    weight_projected<<<blocks, 256, 0, p.diagnostic_stream>>>(
        p.projected, p.weights_float, p.cells, p.samples);
    cuda_check(cudaGetLastError(), "launch diagnostic weighting kernel");
    cufft_check(cufftExecC2C(p.range_plan, p.projected, p.projected, CUFFT_INVERSE),
                "execute diagnostic range transform");
    const size_t map_cells = static_cast<size_t>(p.range_bins) * p.rows;
    const uint32_t map_blocks = static_cast<uint32_t>(
        std::min<size_t>(65535, (map_cells + 127) / 128));
    likelihood_kernel<<<map_blocks, 128, 0, p.diagnostic_stream>>>(
        p.projected, p.slow_steering, p.rate_allowed, p.likelihood, p.antennas,
        p.rows, p.subcarriers, p.range_bins, minimum_range_bin,
        static_cast<float>(p.denominator), p.times, p.time_midpoint_s, p.rate_res_mps,
        p.range_res_m, p.range_walk);
    cuda_check(cudaGetLastError(), "launch diagnostic likelihood kernel");
    cuda_check(cudaMemcpyAsync(p.pinned_likelihood, p.likelihood,
                               map_cells * sizeof(*p.pinned_likelihood), cudaMemcpyDeviceToHost,
                               p.diagnostic_stream),
               "download diagnostic likelihood");
  } catch (...) {
    cudaStreamSynchronize(p.diagnostic_stream);
    p.diagnostic_pending = false;
    throw;
  }
}

std::vector<double> CudaDetectorBackend::finish_diagnostic_likelihood_map()
{
  auto& p = *impl_;
  if (!p.diagnostic_only || !p.diagnostic_pending)
    throw std::logic_error("no diagnostic map is pending");
  const cudaError_t status = cudaStreamSynchronize(p.diagnostic_stream);
  p.diagnostic_pending = false;
  cuda_check(status, "synchronize diagnostic likelihood");
  const size_t map_cells = static_cast<size_t>(p.range_bins) * p.rows;
  return std::vector<double>(p.pinned_likelihood, p.pinned_likelihood + map_cells);
}

CudaRefinementEvaluation CudaDetectorBackend::evaluate(double range_bin, double doppler_bin)
{
  auto& p = *impl_;
  if (!p.residual_loaded)
    throw std::logic_error("CUDA refinement requested before residual upload");
  const uint32_t template_blocks = std::min<uint32_t>(128, (p.subcarriers + 255) / 256);
  refinement_templates<<<template_blocks, 256, 0, p.stream>>>(
      p.refinement_range_steering, p.refinement_doppler_steering, p.times, p.rows,
      p.subcarriers, p.fc_hz, p.rate_res_mps, range_bin, doppler_bin);
  cuda_check(cudaGetLastError(), "launch detector refinement-template kernel");
  const uint32_t blocks = p.antennas * REFINEMENT_BLOCKS_PER_ANTENNA;
  refinement_partials<<<blocks, REFINEMENT_BLOCK_SIZE, 0, p.stream>>>(
      p.residual, p.weights, p.times, p.refinement_range_steering,
      p.refinement_doppler_steering, p.refinement, p.antennas, p.rows, p.subcarriers,
      p.fc_hz, p.rate_res_mps, range_bin, doppler_bin,
      p.component_walk_set ? p.component_walk : nullptr);
  cuda_check(cudaGetLastError(), "launch detector refinement kernel");
  cuda_check(cudaMemcpyAsync(p.host_refinement.data(), p.refinement,
                        p.host_refinement.size() * sizeof(double), cudaMemcpyDeviceToHost,
                        p.stream),
             "download detector refinement statistics");
  cuda_check(cudaStreamSynchronize(p.stream), "synchronize detector refinement");

  CudaRefinementEvaluation result;
  result.coherent.resize(p.antennas);
  for (uint32_t antenna = 0; antenna < p.antennas; ++antenna) {
    std::complex<double> terms[REFINEMENT_TERMS]{};
    for (uint32_t block = 0; block < REFINEMENT_BLOCKS_PER_ANTENNA; ++block) {
      const size_t base = (static_cast<size_t>(antenna) * REFINEMENT_BLOCKS_PER_ANTENNA + block)
                          * REFINEMENT_TERMS * 2;
      for (uint32_t term = 0; term < REFINEMENT_TERMS; ++term)
        terms[term] += std::complex<double>(p.host_refinement[base + 2 * term],
                                           p.host_refinement[base + 2 * term + 1]);
    }
    const auto coherent = terms[0];
    const auto first_range = terms[1];
    const auto first_doppler = terms[2];
    result.coherent[antenna] = coherent;
    result.objective += std::norm(coherent);
    result.gradient[0] += 2.0 * std::real(std::conj(coherent) * first_range);
    result.gradient[1] += 2.0 * std::real(std::conj(coherent) * first_doppler);
    result.hessian[0] += 2.0 * std::real(std::conj(first_range) * first_range
                                        + std::conj(coherent) * terms[3]);
    const double cross = 2.0 * std::real(std::conj(first_doppler) * first_range
                                         + std::conj(coherent) * terms[5]);
    result.hessian[1] += cross;
    result.hessian[2] += cross;
    result.hessian[3] += 2.0 * std::real(std::conj(first_doppler) * first_doppler
                                        + std::conj(coherent) * terms[4]);
  }
  return result;
}

double CudaDetectorBackend::weighted_energy() const
{
  if (!impl_->residual_loaded) throw std::logic_error("CUDA detector residual is not loaded");
  return impl_->current_weighted_energy;
}

double CudaDetectorBackend::unweighted_energy() const
{
  if (!impl_->residual_loaded) throw std::logic_error("CUDA detector residual is not loaded");
  return impl_->current_unweighted_energy;
}

std::array<double, 2> CudaDetectorBackend::subtract_component(
    double range_bin,
    double doppler_bin,
    const std::vector<std::complex<double>>& host_alpha)
{
  auto& p = *impl_;
  if (!p.residual_loaded || host_alpha.size() != p.antennas)
    throw std::invalid_argument("CUDA CLEAN amplitude shape mismatch");
  std::vector<cufftDoubleComplex> alpha(p.antennas);
  for (uint32_t antenna = 0; antenna < p.antennas; ++antenna)
    alpha[antenna] = {host_alpha[antenna].real(), host_alpha[antenna].imag()};
  cuda_check(cudaMemcpyAsync(p.alpha, alpha.data(), alpha.size() * sizeof(*p.alpha),
                        cudaMemcpyHostToDevice, p.stream), "upload CLEAN amplitudes");
  const uint32_t template_blocks = std::min<uint32_t>(128, (p.subcarriers + 255) / 256);
  refinement_templates<<<template_blocks, 256, 0, p.stream>>>(
      p.refinement_range_steering, p.refinement_doppler_steering, p.times, p.rows,
      p.subcarriers, p.fc_hz, p.rate_res_mps, range_bin, doppler_bin);
  cuda_check(cudaGetLastError(), "launch accepted-component steering kernel");
  const uint32_t blocks = static_cast<uint32_t>(std::min<size_t>(65535, (p.samples + 255) / 256));
  subtract_component_kernel<<<blocks, 256, 0, p.stream>>>(
      p.residual, p.refinement_range_steering, p.refinement_doppler_steering, p.alpha,
      p.rows, p.subcarriers, p.cells, p.samples,
      p.component_walk_set ? p.component_walk : nullptr);
  cuda_check(cudaGetLastError(), "launch CLEAN subtraction kernel");
  energy_partials<<<ENERGY_BLOCKS, ENERGY_BLOCK_SIZE, 0, p.stream>>>(
      p.residual, p.weights, p.energy, p.cells, p.samples);
  cuda_check(cudaGetLastError(), "launch detector-energy kernel");
  cuda_check(cudaMemcpyAsync(p.host_energy.data(), p.energy,
                        p.host_energy.size() * sizeof(double), cudaMemcpyDeviceToHost,
                        p.stream),
             "download detector energy");
  cuda_check(cudaStreamSynchronize(p.stream), "synchronize detector subtraction");
  const double before = p.current_weighted_energy;
  p.current_weighted_energy = 0.0;
  p.current_unweighted_energy = 0.0;
  for (uint32_t block = 0; block < ENERGY_BLOCKS; ++block) {
    p.current_weighted_energy += p.host_energy[2 * block];
    p.current_unweighted_energy += p.host_energy[2 * block + 1];
  }
  return {before, p.current_weighted_energy};
}

} // namespace nr_isac
