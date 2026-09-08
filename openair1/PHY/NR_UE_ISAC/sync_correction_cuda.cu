/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "sync_correction_cuda.h"

#include <cuda_runtime_api.h>
#include <cufft.h>
#include <cub/device/device_segmented_radix_sort.cuh>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cfloat>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace nr_isac {
namespace {

const char* cufft_error_name(cufftResult status)
{
  switch (status) {
    case CUFFT_SUCCESS: return "CUFFT_SUCCESS";
    case CUFFT_INVALID_PLAN: return "CUFFT_INVALID_PLAN";
    case CUFFT_ALLOC_FAILED: return "CUFFT_ALLOC_FAILED";
    case CUFFT_INVALID_TYPE: return "CUFFT_INVALID_TYPE";
    case CUFFT_INVALID_VALUE: return "CUFFT_INVALID_VALUE";
    case CUFFT_INTERNAL_ERROR: return "CUFFT_INTERNAL_ERROR";
    case CUFFT_EXEC_FAILED: return "CUFFT_EXEC_FAILED";
    case CUFFT_SETUP_FAILED: return "CUFFT_SETUP_FAILED";
    case CUFFT_INVALID_SIZE: return "CUFFT_INVALID_SIZE";
    case CUFFT_UNALIGNED_DATA: return "CUFFT_UNALIGNED_DATA";
#if CUDART_VERSION < 13000
    case CUFFT_INCOMPLETE_PARAMETER_LIST: return "CUFFT_INCOMPLETE_PARAMETER_LIST";
#endif
    case CUFFT_INVALID_DEVICE: return "CUFFT_INVALID_DEVICE";
#if CUDART_VERSION < 13000
    case CUFFT_PARSE_ERROR: return "CUFFT_PARSE_ERROR";
#endif
    case CUFFT_NO_WORKSPACE: return "CUFFT_NO_WORKSPACE";
    case CUFFT_NOT_IMPLEMENTED: return "CUFFT_NOT_IMPLEMENTED";
#if CUDART_VERSION < 13000
    case CUFFT_LICENSE_ERROR: return "CUFFT_LICENSE_ERROR";
#endif
    case CUFFT_NOT_SUPPORTED: return "CUFFT_NOT_SUPPORTED";
    default: return "CUFFT_UNKNOWN_ERROR";
  }
}

void set_error(std::string* output, const std::string& value)
{
  if (output) *output = value;
}

double median_float(std::vector<float> values)
{
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t upper = values.size() / 2;
  if (values.size() & 1) return values[upper];
  return 0.5 * (static_cast<double>(values[upper - 1]) + values[upper]);
}

int profile_halfwidth(const std::vector<float>& power, int anchor)
{
  const int n = static_cast<int>(power.size());
  const double background = median_float(power);
  const double peak = power[(anchor % n + n) % n];
  const double crossing = std::sqrt(std::max(background, std::numeric_limits<double>::min())
                                    * std::max(peak, std::numeric_limits<double>::min()));
  int result = 1;
  for (int direction : {-1, 1}) {
    int distance = 1;
    while (distance < n / 2 && power[(anchor + direction * distance + n * 2) % n] > crossing)
      ++distance;
    result = std::max(result, distance);
  }
  return std::min(result, std::max(1, n / 2 - 1));
}

__global__ void cir_power_kernel(const cufftComplex* input, float* output,
                                 size_t elements, float scale)
{
  const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index >= elements) return;
  const float real = input[index].x * scale;
  const float imag = input[index].y * scale;
  output[index] = real * real + imag * imag;
}

__global__ void row_energy_kernel(const float* power, float* energy,
                                  uint32_t rows, uint32_t fft_n)
{
  const uint32_t row = blockIdx.x;
  if (row >= rows) return;
  float sum = 0.0f;
  for (uint32_t i = threadIdx.x; i < fft_n; i += blockDim.x)
    sum += power[static_cast<size_t>(row) * fft_n + i];
  __shared__ float partial[256];
  partial[threadIdx.x] = sum;
  __syncthreads();
  for (uint32_t stride = blockDim.x / 2; stride; stride >>= 1) {
    if (threadIdx.x < stride) partial[threadIdx.x] += partial[threadIdx.x + stride];
    __syncthreads();
  }
  if (threadIdx.x == 0)
    energy[row] = fmaxf(partial[0], FLT_MIN);
}

__global__ void mean_profile_kernel(const float* power, const float* energy,
                                    float* mean_profile, uint32_t rows, uint32_t fft_n)
{
  const uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= fft_n) return;
  float sum = 0.0f;
  for (uint32_t row = 0; row < rows; ++row)
    sum += power[static_cast<size_t>(row) * fft_n + index] / energy[row];
  mean_profile[index] = sum / rows;
}

__global__ void coarse_profile_kernel(const float* mean_profile, float* coarse,
                                      uint32_t subcarriers, uint32_t oversample)
{
  const uint32_t carrier = blockIdx.x * blockDim.x + threadIdx.x;
  if (carrier >= subcarriers) return;
  float sum = 0.0f;
  for (uint32_t offset = 0; offset < oversample; ++offset)
    sum += mean_profile[static_cast<size_t>(carrier) * oversample + offset];
  coarse[carrier] = sum;
}

struct PeakCandidate {
  float power;
  int order;
};

__device__ PeakCandidate better_peak(PeakCandidate left, PeakCandidate right)
{
  if (right.power > left.power || (right.power == left.power && right.order < left.order))
    return right;
  return left;
}

__global__ void local_peak_kernel(const float* power, uint32_t rows, uint32_t fft_n,
                                  int64_t fine_center, int radius, int* peak_indices,
                                  float* peak_power, float* peak_delta)
{
  const uint32_t row = blockIdx.x;
  if (row >= rows) return;
  const int count = 2 * radius + 1;
  PeakCandidate best{-1.0f, count};
  for (int order = threadIdx.x; order < count; order += blockDim.x) {
    int64_t candidate = (fine_center - radius + order) % static_cast<int64_t>(fft_n);
    if (candidate < 0) candidate += fft_n;
    best = better_peak(best, PeakCandidate{power[static_cast<size_t>(row) * fft_n + candidate], order});
  }
  __shared__ PeakCandidate partial[256];
  partial[threadIdx.x] = best;
  __syncthreads();
  for (uint32_t stride = blockDim.x / 2; stride; stride >>= 1) {
    if (threadIdx.x < stride)
      partial[threadIdx.x] = better_peak(partial[threadIdx.x], partial[threadIdx.x + stride]);
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    int64_t peak = (fine_center - radius + partial[0].order) % static_cast<int64_t>(fft_n);
    if (peak < 0) peak += fft_n;
    const float left = power[static_cast<size_t>(row) * fft_n + (peak + fft_n - 1) % fft_n];
    const float centre = power[static_cast<size_t>(row) * fft_n + peak];
    const float right = power[static_cast<size_t>(row) * fft_n + (peak + 1) % fft_n];
    const float denominator = left - 2.0f * centre + right;
    float delta = fabsf(denominator) > FLT_MIN
                      ? 0.5f * (left - right) / denominator : 0.0f;
    delta = fminf(0.5f, fmaxf(-0.5f, delta));
    peak_indices[row] = static_cast<int>(peak);
    peak_power[row] = centre;
    peak_delta[row] = delta;
  }
}

__global__ void compact_statistics_kernel(const float* sorted_power, uint32_t rows,
                                          uint32_t fft_n, uint32_t oversample,
                                          const int* peak_indices, const float* peak_power,
                                          const float* peak_delta, double* delays,
                                          float* contrasts)
{
  const uint32_t row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= rows) return;
  const size_t base = static_cast<size_t>(row) * fft_n;
  const uint32_t upper = fft_n / 2;
  const float floor = (fft_n & 1)
                          ? sorted_power[base + upper]
                          : 0.5f * (sorted_power[base + upper - 1] + sorted_power[base + upper]);
  const int peak = peak_indices[row];
  const int signed_peak = peak <= static_cast<int>(fft_n / 2) ? peak : peak - static_cast<int>(fft_n);
  delays[row] = (static_cast<double>(signed_peak) + peak_delta[row]) / oversample;
  contrasts[row] = logf(fmaxf(peak_power[row], FLT_MIN))
                   - logf(fmaxf(floor, FLT_MIN));
}

__global__ void row_los_evidence_kernel(const cufftComplex* values,
                                        const uint8_t* observed,
                                        const double* row_delay,
                                        const cufftDoubleComplex* spatial,
                                        cufftDoubleComplex* coherent,
                                        double* energy,
                                        uint32_t* complex_count,
                                        uint32_t selected_antennas,
                                        uint32_t rows,
                                        uint32_t subcarriers)
{
  const uint32_t row = blockIdx.x;
  if (row >= rows) return;
  double coherent_real = 0.0;
  double coherent_imag = 0.0;
  double energy_sum = 0.0;
  uint32_t count = 0;
  const size_t cells = static_cast<size_t>(rows) * subcarriers;
  const size_t selected_samples = static_cast<size_t>(selected_antennas) * subcarriers;
  for (size_t selected = threadIdx.x; selected < selected_samples; selected += blockDim.x) {
    const uint32_t antenna = static_cast<uint32_t>(selected / subcarriers);
    const uint32_t carrier = static_cast<uint32_t>(selected % subcarriers);
    if (!observed[static_cast<size_t>(row) * subcarriers + carrier]) continue;
    const cufftComplex value = values[static_cast<size_t>(antenna) * cells
                                      + static_cast<size_t>(row) * subcarriers + carrier];
    const cufftDoubleComplex steering = spatial[antenna];
    // value * conj(spatial), retaining the CPU path's double accumulation.
    const double spatial_real = value.x * steering.x + value.y * steering.y;
    const double spatial_imag = value.y * steering.x - value.x * steering.y;
    const double centered = carrier - 0.5 * (subcarriers - 1.0);
    const double angle = 2.0 * PI * row_delay[row] * centered / subcarriers;
    double sine = 0.0, cosine = 0.0;
    sincos(angle, &sine, &cosine);
    coherent_real += spatial_real * cosine - spatial_imag * sine;
    coherent_imag += spatial_real * sine + spatial_imag * cosine;
    energy_sum += static_cast<double>(value.x) * value.x
                  + static_cast<double>(value.y) * value.y;
    ++count;
  }
  __shared__ double coherent_real_partial[256];
  __shared__ double coherent_imag_partial[256];
  __shared__ double energy_partial[256];
  __shared__ uint32_t count_partial[256];
  coherent_real_partial[threadIdx.x] = coherent_real;
  coherent_imag_partial[threadIdx.x] = coherent_imag;
  energy_partial[threadIdx.x] = energy_sum;
  count_partial[threadIdx.x] = count;
  __syncthreads();
  for (uint32_t stride = blockDim.x / 2; stride; stride >>= 1) {
    if (threadIdx.x < stride) {
      coherent_real_partial[threadIdx.x] += coherent_real_partial[threadIdx.x + stride];
      coherent_imag_partial[threadIdx.x] += coherent_imag_partial[threadIdx.x + stride];
      energy_partial[threadIdx.x] += energy_partial[threadIdx.x + stride];
      count_partial[threadIdx.x] += count_partial[threadIdx.x + stride];
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    coherent[row] = make_cuDoubleComplex(coherent_real_partial[0], coherent_imag_partial[0]);
    energy[row] = energy_partial[0];
    complex_count[row] = count_partial[0];
  }
}

__global__ void correction_phasor_kernel(cufftComplex* phasor,
                                         const uint8_t* observed,
                                         const double* delay,
                                         const double* applied_phase,
                                         bool apply_cpe,
                                         uint32_t rows,
                                         uint32_t subcarriers)
{
  const size_t cells = static_cast<size_t>(rows) * subcarriers;
  for (size_t cell = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       cell < cells;
       cell += static_cast<size_t>(blockDim.x) * gridDim.x) {
    if (!observed[cell]) {
      phasor[cell] = make_cuFloatComplex(0.0f, 0.0f);
      continue;
    }
    const uint32_t row = static_cast<uint32_t>(cell / subcarriers);
    const uint32_t carrier = static_cast<uint32_t>(cell % subcarriers);
    const double centered = carrier - 0.5 * (subcarriers - 1.0);
    const double angle = 2.0 * PI * delay[row] * centered / subcarriers
                         - (apply_cpe ? applied_phase[row] : 0.0);
    double sine = 0.0, cosine = 0.0;
    sincos(angle, &sine, &cosine);
    phasor[cell] = make_cuFloatComplex(static_cast<float>(cosine), static_cast<float>(sine));
  }
}

__global__ void apply_common_phasor_kernel(cufftComplex* values,
                                           const uint8_t* observed,
                                           const cufftComplex* phasor,
                                           uint32_t antennas,
                                           size_t cells)
{
  const size_t samples = static_cast<size_t>(antennas) * cells;
  for (size_t sample = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       sample < samples;
       sample += static_cast<size_t>(blockDim.x) * gridDim.x) {
    const size_t cell = sample % cells;
    if (!observed[cell]) {
      values[sample] = make_cuFloatComplex(0.0f, 0.0f);
      continue;
    }
    const cufftComplex value = values[sample];
    const cufftComplex phase = phasor[cell];
    values[sample] = make_cuFloatComplex(value.x * phase.x - value.y * phase.y,
                                         value.x * phase.y + value.y * phase.x);
  }
}

class CufftBatch {
public:
  CufftBatch() = default;
  ~CufftBatch() { reset(); }
  CufftBatch(const CufftBatch&) = delete;
  CufftBatch& operator=(const CufftBatch&) = delete;

  bool configure(uint32_t fft_n, uint32_t subcarriers, uint32_t rows, std::string* error)
  {
    if (plan_ && fft_n_ == fft_n && subcarriers_ == subcarriers && rows_ >= rows)
      return true;
    reset();
    if (!fft_n || !subcarriers || !rows || fft_n < subcarriers) {
      set_error(error, "invalid cuFFT dimensions");
      return false;
    }
    const size_t elements = static_cast<size_t>(fft_n) * rows;
    if (elements / fft_n != rows || elements > static_cast<size_t>(std::numeric_limits<int>::max())) {
      set_error(error, "cuFFT/CUB allocation size overflow");
      return false;
    }
    auto allocate = [&](void** pointer, size_t bytes, const char* label) {
      const cudaError_t status = cudaMalloc(pointer, bytes);
      if (status == cudaSuccess) return true;
      set_error(error, std::string("cudaMalloc(") + label + "): " + cudaGetErrorString(status));
      return false;
    };
    if (!allocate(reinterpret_cast<void**>(&device_), elements * sizeof(cufftComplex), "CIR")
        || !allocate(reinterpret_cast<void**>(&device_power_), elements * sizeof(float), "CIR power")
        || !allocate(reinterpret_cast<void**>(&device_sorted_), elements * sizeof(float), "sorted CIR power")
        || !allocate(reinterpret_cast<void**>(&row_energy_), rows * sizeof(float), "row energy")
        || !allocate(reinterpret_cast<void**>(&mean_profile_), fft_n * sizeof(float), "mean profile")
        || !allocate(reinterpret_cast<void**>(&coarse_profile_), subcarriers * sizeof(float), "coarse profile")
        || !allocate(reinterpret_cast<void**>(&segment_offsets_), (rows + 1) * sizeof(int), "segment offsets")
        || !allocate(reinterpret_cast<void**>(&peak_indices_), rows * sizeof(int), "peak indices")
        || !allocate(reinterpret_cast<void**>(&peak_power_), rows * sizeof(float), "peak power")
        || !allocate(reinterpret_cast<void**>(&peak_delta_), rows * sizeof(float), "peak delta")
        || !allocate(reinterpret_cast<void**>(&delays_), rows * sizeof(double), "row delays")
        || !allocate(reinterpret_cast<void**>(&contrasts_), rows * sizeof(float), "row contrasts")) {
      reset();
      return false;
    }
    cudaError_t cuda_status = cudaMemset(device_, 0, elements * sizeof(cufftComplex));
    if (cuda_status != cudaSuccess) {
      set_error(error, std::string("cudaMemset(CIR workspace): ") + cudaGetErrorString(cuda_status));
      reset();
      return false;
    }

    cufftResult cufft_status = cufftCreate(&plan_);
    if (cufft_status != CUFFT_SUCCESS) {
      set_error(error, std::string("cufftCreate: ") + cufft_error_name(cufft_status));
      reset();
      return false;
    }
    cufft_status = cufftSetAutoAllocation(plan_, 0);
    if (cufft_status != CUFFT_SUCCESS) {
      set_error(error, std::string("cufftSetAutoAllocation: ") + cufft_error_name(cufft_status));
      reset();
      return false;
    }
    int length = static_cast<int>(fft_n);
    cufft_status = cufftMakePlanMany(plan_, 1, &length, nullptr, 1, length,
                                     nullptr, 1, length, CUFFT_C2C,
                                     static_cast<int>(rows), &cufft_workspace_bytes_);
    if (cufft_status != CUFFT_SUCCESS) {
      set_error(error, std::string("cufftMakePlanMany: ") + cufft_error_name(cufft_status));
      reset();
      return false;
    }
    if (cufft_workspace_bytes_) {
      if (!allocate(&cufft_workspace_, cufft_workspace_bytes_, "cuFFT workspace")) {
        reset();
        return false;
      }
      cufft_status = cufftSetWorkArea(plan_, cufft_workspace_);
      if (cufft_status != CUFFT_SUCCESS) {
        set_error(error, std::string("cufftSetWorkArea: ") + cufft_error_name(cufft_status));
        reset();
        return false;
      }
    }

    std::vector<int> offsets(rows + 1);
    for (uint32_t row = 0; row <= rows; ++row)
      offsets[row] = static_cast<int>(static_cast<size_t>(row) * fft_n);
    cuda_status = cudaMemcpy(segment_offsets_, offsets.data(),
                             offsets.size() * sizeof(int), cudaMemcpyHostToDevice);
    if (cuda_status != cudaSuccess) {
      set_error(error, std::string("cudaMemcpy(segment offsets): ") + cudaGetErrorString(cuda_status));
      reset();
      return false;
    }
    cuda_status = cub::DeviceSegmentedRadixSort::SortKeys(
        nullptr, sort_workspace_bytes_, device_power_, device_sorted_,
        static_cast<int>(elements), static_cast<int>(rows), segment_offsets_, segment_offsets_ + 1);
    if (cuda_status != cudaSuccess) {
      set_error(error, std::string("CUB sort query: ") + cudaGetErrorString(cuda_status));
      reset();
      return false;
    }
    if (sort_workspace_bytes_ && !allocate(&sort_workspace_, sort_workspace_bytes_, "sort workspace")) {
      reset();
      return false;
    }
    try {
      compact_host_.resize(static_cast<size_t>(rows) * subcarriers);
      coarse_host_.resize(subcarriers);
      delay_host_.resize(rows);
      contrast_host_.resize(rows);
      peak_power_host_.resize(rows);
    } catch (...) {
      set_error(error, "host CUDA sync workspace allocation failed");
      reset();
      return false;
    }
    fft_n_ = fft_n;
    subcarriers_ = subcarriers;
    rows_ = rows;
    return true;
  }

  bool run(const CfrWindow& window, uint32_t oversample, CudaSyncFrontEnd& output,
           std::string* error)
  {
    const uint32_t fft_n = window.subcarriers * oversample;
    if (!configure(fft_n, window.subcarriers, window.rows, error)) return false;
    for (uint32_t row = 0; row < window.rows; ++row)
      for (uint32_t carrier = 0; carrier < window.subcarriers; ++carrier) {
        cufftComplex& destination = compact_host_[static_cast<size_t>(row) * window.subcarriers + carrier];
        if (!window.observed[window.cell(row, carrier)]) {
          destination = {0.0f, 0.0f};
        } else {
          const std::complex<float> value = window.values[window.sample(0, row, carrier)];
          destination = {value.real(), value.imag()};
        }
      }
    const size_t elements = static_cast<size_t>(window.rows) * fft_n;
    cudaError_t cuda_status = cudaMemset(device_, 0, elements * sizeof(cufftComplex));
    if (cuda_status == cudaSuccess)
      cuda_status = cudaMemcpy2D(device_, static_cast<size_t>(fft_n) * sizeof(cufftComplex),
                                 compact_host_.data(),
                                 static_cast<size_t>(window.subcarriers) * sizeof(cufftComplex),
                                 static_cast<size_t>(window.subcarriers) * sizeof(cufftComplex),
                                 window.rows, cudaMemcpyHostToDevice);
    if (cuda_status != cudaSuccess) {
      set_error(error, std::string("CUDA sync input transfer: ") + cudaGetErrorString(cuda_status));
      return false;
    }
    const cufftResult cufft_status = cufftExecC2C(plan_, device_, device_, CUFFT_INVERSE);
    if (cufft_status != CUFFT_SUCCESS) {
      set_error(error, std::string("cufftExecC2C: ") + cufft_error_name(cufft_status));
      return false;
    }
    constexpr uint32_t threads = 256;
    cir_power_kernel<<<static_cast<uint32_t>((elements + threads - 1) / threads), threads>>>(
        device_, device_power_, elements, 1.0f / fft_n);
    row_energy_kernel<<<window.rows, threads>>>(device_power_, row_energy_, window.rows, fft_n);
    mean_profile_kernel<<<(fft_n + threads - 1) / threads, threads>>>(
        device_power_, row_energy_, mean_profile_, window.rows, fft_n);
    coarse_profile_kernel<<<(window.subcarriers + threads - 1) / threads, threads>>>(
        mean_profile_, coarse_profile_, window.subcarriers, oversample);
    cuda_status = cudaMemcpy(coarse_host_.data(), coarse_profile_,
                             window.subcarriers * sizeof(float), cudaMemcpyDeviceToHost);
    if (cuda_status != cudaSuccess) {
      set_error(error, std::string("CUDA sync profile transfer: ") + cudaGetErrorString(cuda_status));
      return false;
    }
    output.anchor_unsigned = static_cast<uint32_t>(
        std::max_element(coarse_host_.begin(), coarse_host_.end()) - coarse_host_.begin());
    output.anchor = output.anchor_unsigned <= window.subcarriers / 2
                        ? static_cast<int>(output.anchor_unsigned)
                        : static_cast<int>(output.anchor_unsigned) - static_cast<int>(window.subcarriers);
    output.halfwidth = profile_halfwidth(coarse_host_, output.anchor_unsigned);
    const int radius = output.halfwidth * static_cast<int>(oversample);
    const int64_t fine_center = static_cast<int64_t>(output.anchor_unsigned) * oversample;
    local_peak_kernel<<<window.rows, threads>>>(device_power_, window.rows, fft_n,
                                                fine_center, radius, peak_indices_,
                                                peak_power_, peak_delta_);
    cuda_status = cub::DeviceSegmentedRadixSort::SortKeys(
        sort_workspace_, sort_workspace_bytes_, device_power_, device_sorted_,
        static_cast<int>(elements), static_cast<int>(window.rows),
        segment_offsets_, segment_offsets_ + 1);
    if (cuda_status != cudaSuccess) {
      set_error(error, std::string("CUB segmented row median: ") + cudaGetErrorString(cuda_status));
      return false;
    }
    compact_statistics_kernel<<<(window.rows + threads - 1) / threads, threads>>>(
        device_sorted_, window.rows, fft_n, oversample, peak_indices_, peak_power_, peak_delta_,
        delays_, contrasts_);
    cuda_status = cudaMemcpy(delay_host_.data(), delays_, window.rows * sizeof(double), cudaMemcpyDeviceToHost);
    if (cuda_status == cudaSuccess)
      cuda_status = cudaMemcpy(contrast_host_.data(), contrasts_, window.rows * sizeof(float),
                               cudaMemcpyDeviceToHost);
    if (cuda_status == cudaSuccess)
      cuda_status = cudaMemcpy(peak_power_host_.data(), peak_power_, window.rows * sizeof(float),
                               cudaMemcpyDeviceToHost);
    if (cuda_status != cudaSuccess) {
      set_error(error, std::string("CUDA sync statistic transfer: ") + cudaGetErrorString(cuda_status));
      return false;
    }
    output.delays.assign(delay_host_.begin(), delay_host_.end());
    output.contrasts.assign(contrast_host_.begin(), contrast_host_.end());
    output.peak_powers.assign(peak_power_host_.begin(), peak_power_host_.end());
    if (error) error->clear();
    return true;
  }

private:
  void reset()
  {
    if (plan_) cufftDestroy(plan_);
    if (sort_workspace_) cudaFree(sort_workspace_);
    if (cufft_workspace_) cudaFree(cufft_workspace_);
    if (contrasts_) cudaFree(contrasts_);
    if (delays_) cudaFree(delays_);
    if (peak_delta_) cudaFree(peak_delta_);
    if (peak_power_) cudaFree(peak_power_);
    if (peak_indices_) cudaFree(peak_indices_);
    if (segment_offsets_) cudaFree(segment_offsets_);
    if (coarse_profile_) cudaFree(coarse_profile_);
    if (mean_profile_) cudaFree(mean_profile_);
    if (row_energy_) cudaFree(row_energy_);
    if (device_sorted_) cudaFree(device_sorted_);
    if (device_power_) cudaFree(device_power_);
    if (device_) cudaFree(device_);
    plan_ = 0;
    device_ = nullptr;
    device_power_ = device_sorted_ = row_energy_ = mean_profile_ = coarse_profile_ = nullptr;
    segment_offsets_ = peak_indices_ = nullptr;
    peak_power_ = peak_delta_ = contrasts_ = nullptr;
    delays_ = nullptr;
    cufft_workspace_ = sort_workspace_ = nullptr;
    cufft_workspace_bytes_ = sort_workspace_bytes_ = 0;
    fft_n_ = subcarriers_ = rows_ = 0;
    compact_host_.clear();
    coarse_host_.clear();
    delay_host_.clear();
    contrast_host_.clear();
    peak_power_host_.clear();
  }

  cufftHandle plan_ = 0;
  cufftComplex* device_ = nullptr;
  float* device_power_ = nullptr;
  float* device_sorted_ = nullptr;
  float* row_energy_ = nullptr;
  float* mean_profile_ = nullptr;
  float* coarse_profile_ = nullptr;
  int* segment_offsets_ = nullptr;
  int* peak_indices_ = nullptr;
  float* peak_power_ = nullptr;
  float* peak_delta_ = nullptr;
  double* delays_ = nullptr;
  float* contrasts_ = nullptr;
  void* cufft_workspace_ = nullptr;
  void* sort_workspace_ = nullptr;
  size_t cufft_workspace_bytes_ = 0;
  size_t sort_workspace_bytes_ = 0;
  uint32_t fft_n_ = 0;
  uint32_t subcarriers_ = 0;
  uint32_t rows_ = 0;
  std::vector<cufftComplex> compact_host_;
  std::vector<float> coarse_host_;
  std::vector<double> delay_host_;
  std::vector<float> contrast_host_;
  std::vector<float> peak_power_host_;
};

double bic_value(double rss, uint32_t samples, uint32_t parameters)
{
  const double safe = std::max(rss / std::max(1u, samples), std::numeric_limits<double>::min());
  return samples * std::log(safe) + parameters * std::log(std::max(2u, samples));
}

class CorrectionWorkspace {
public:
  CorrectionWorkspace() = default;
  ~CorrectionWorkspace() { reset(); }
  CorrectionWorkspace(const CorrectionWorkspace&) = delete;
  CorrectionWorkspace& operator=(const CorrectionWorkspace&) = delete;

  bool configure(uint32_t rows, uint32_t subcarriers, uint32_t antennas, std::string* error)
  {
    if (values_ && rows_ >= rows && subcarriers_ == subcarriers && antennas_ >= antennas)
      return true;
    reset();
    if (!rows || !subcarriers || !antennas || antennas > 4) {
      set_error(error, "invalid CUDA sync-correction dimensions");
      return false;
    }
    const size_t cells = static_cast<size_t>(rows) * subcarriers;
    const size_t samples = cells * antennas;
    if (cells / rows != subcarriers || samples / cells != antennas) {
      set_error(error, "CUDA sync-correction allocation size overflow");
      return false;
    }
    auto allocate = [&](void** pointer, size_t bytes, const char* label) {
      const cudaError_t status = cudaMalloc(pointer, bytes);
      if (status == cudaSuccess) return true;
      set_error(error, std::string("cudaMalloc(") + label + "): " + cudaGetErrorString(status));
      return false;
    };
    if (!allocate(reinterpret_cast<void**>(&values_), samples * sizeof(*values_), "sync values")
        || !allocate(reinterpret_cast<void**>(&observed_), cells * sizeof(*observed_), "sync mask")
        || !allocate(reinterpret_cast<void**>(&row_delay_), rows * sizeof(*row_delay_), "LOS delay")
        || !allocate(reinterpret_cast<void**>(&delay_), rows * sizeof(*delay_), "correction delay")
        || !allocate(reinterpret_cast<void**>(&applied_phase_), rows * sizeof(*applied_phase_), "CPE")
        || !allocate(reinterpret_cast<void**>(&spatial_), 4 * sizeof(*spatial_), "LOS steering")
        || !allocate(reinterpret_cast<void**>(&coherent_), rows * sizeof(*coherent_), "LOS coherent sum")
        || !allocate(reinterpret_cast<void**>(&energy_), rows * sizeof(*energy_), "LOS energy")
        || !allocate(reinterpret_cast<void**>(&complex_count_), rows * sizeof(*complex_count_), "LOS count")
        || !allocate(reinterpret_cast<void**>(&phasor_), cells * sizeof(*phasor_), "common phasor")) {
      reset();
      return false;
    }
    try {
      corrected_host_.resize(samples);
      row_delay_host_.resize(rows);
      delay_host_.resize(rows);
      applied_phase_host_.resize(rows);
      coherent_host_.resize(rows);
      energy_host_.resize(rows);
      complex_count_host_.resize(rows);
    } catch (...) {
      set_error(error, "host CUDA sync-correction workspace allocation failed");
      reset();
      return false;
    }
    rows_ = rows;
    subcarriers_ = subcarriers;
    antennas_ = antennas;
    return true;
  }

  bool run(CfrWindow& window,
           const SyncEstimate& estimate,
           double delay_reference_bin,
           const std::optional<std::array<std::complex<double>, 4>>& los_spatial,
           std::string* error)
  {
    if (!configure(window.rows, window.subcarriers, window.antennas, error)) return false;
    const double slot_s = 1e-3 / std::max(1.0, window.scs_hz / 15000.0);
    const double constant = (estimate.sto_applied || estimate.sfo_applied)
                                ? estimate.los_bins - delay_reference_bin : 0.0;
    for (uint32_t row = 0; row < window.rows; ++row) {
      const double time = (window.row_time_slots[row] - window.row_time_slots[0]) * slot_s;
      row_delay_host_[row] = estimate.los_bins;
      delay_host_[row] = constant;
      if (estimate.sfo_applied) {
        const double drift = estimate.sfo_ppm * 1e-6 * window.subcarriers * window.scs_hz * time;
        row_delay_host_[row] += drift;
        delay_host_[row] += drift;
      }
    }
    std::array<cufftDoubleComplex, 4> spatial{};
    for (auto& value : spatial) value = make_cuDoubleComplex(1.0, 0.0);
    if (los_spatial) {
      for (uint32_t antenna = 0; antenna < window.antennas; ++antenna) {
        const double magnitude = std::abs((*los_spatial)[antenna]);
        spatial[antenna] = make_cuDoubleComplex((*los_spatial)[antenna].real() / magnitude,
                                                (*los_spatial)[antenna].imag() / magnitude);
      }
    }
    const size_t cells = static_cast<size_t>(window.rows) * window.subcarriers;
    const size_t samples = cells * window.antennas;
    static_assert(sizeof(std::complex<float>) == sizeof(cufftComplex));
    cudaError_t status = cudaMemcpy(values_, window.values.data(), samples * sizeof(*values_),
                                    cudaMemcpyHostToDevice);
    if (status == cudaSuccess)
      status = cudaMemcpy(observed_, window.observed.data(), cells * sizeof(*observed_),
                          cudaMemcpyHostToDevice);
    if (status == cudaSuccess)
      status = cudaMemcpy(row_delay_, row_delay_host_.data(), window.rows * sizeof(*row_delay_),
                          cudaMemcpyHostToDevice);
    if (status == cudaSuccess)
      status = cudaMemcpy(spatial_, spatial.data(), spatial.size() * sizeof(*spatial_),
                          cudaMemcpyHostToDevice);
    if (status != cudaSuccess) {
      set_error(error, std::string("CUDA sync-correction input transfer: ") + cudaGetErrorString(status));
      return false;
    }
    constexpr uint32_t threads = 256;
    const uint32_t selected_antennas = los_spatial ? window.antennas : 1;
    row_los_evidence_kernel<<<window.rows, threads>>>(
        values_, observed_, row_delay_, spatial_, coherent_, energy_, complex_count_,
        selected_antennas, window.rows, window.subcarriers);
    status = cudaGetLastError();
    if (status == cudaSuccess)
      status = cudaMemcpy(coherent_host_.data(), coherent_, window.rows * sizeof(*coherent_),
                          cudaMemcpyDeviceToHost);
    if (status == cudaSuccess)
      status = cudaMemcpy(energy_host_.data(), energy_, window.rows * sizeof(*energy_),
                          cudaMemcpyDeviceToHost);
    if (status == cudaSuccess)
      status = cudaMemcpy(complex_count_host_.data(), complex_count_,
                          window.rows * sizeof(*complex_count_), cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
      set_error(error, std::string("CUDA LOS-evidence transfer: ") + cudaGetErrorString(status));
      return false;
    }

    std::vector<uint32_t> usable;
    usable.reserve(window.rows);
    std::vector<double> measured_phase(window.rows, 0.0);
    std::fill_n(applied_phase_host_.begin(), window.rows, 0.0);
    for (uint32_t row = 0; row < window.rows; ++row) {
      const uint32_t count = complex_count_host_[row];
      const double energy = energy_host_[row];
      if (!count || !(energy > 0.0)) continue;
      const cufftDoubleComplex coherent = coherent_host_[row];
      const double explained = (coherent.x * coherent.x + coherent.y * coherent.y) / count;
      const double residual = std::max(energy - explained, std::numeric_limits<double>::min());
      const uint32_t real_samples = 2 * count;
      if (bic_value(residual, real_samples, 2) < bic_value(energy, real_samples, 0)) {
        usable.push_back(row);
        measured_phase[row] = std::atan2(coherent.y, coherent.x);
        applied_phase_host_[row] = measured_phase[row];
      }
    }
    if (!usable.empty()) {
      for (uint32_t row = 0; row < window.rows; ++row) {
        if (std::binary_search(usable.begin(), usable.end(), row)) continue;
        const double time = (window.row_time_slots[row] - window.row_time_slots[0]) * slot_s;
        const uint32_t nearest = *std::min_element(usable.begin(), usable.end(), [&](uint32_t left, uint32_t right) {
          const double left_time = (window.row_time_slots[left] - window.row_time_slots[0]) * slot_s;
          const double right_time = (window.row_time_slots[right] - window.row_time_slots[0]) * slot_s;
          return std::abs(left_time - time) < std::abs(right_time - time);
        });
        applied_phase_host_[row] = measured_phase[nearest];
        if (estimate.cfo_applied) {
          const double nearest_time = (window.row_time_slots[nearest] - window.row_time_slots[0]) * slot_s;
          applied_phase_host_[row] += 2.0 * PI * estimate.cfo_hz * (time - nearest_time);
        }
      }
    }
    status = cudaMemcpy(delay_, delay_host_.data(), window.rows * sizeof(*delay_),
                        cudaMemcpyHostToDevice);
    if (status == cudaSuccess)
      status = cudaMemcpy(applied_phase_, applied_phase_host_.data(),
                          window.rows * sizeof(*applied_phase_), cudaMemcpyHostToDevice);
    if (status != cudaSuccess) {
      set_error(error, std::string("CUDA correction-vector transfer: ") + cudaGetErrorString(status));
      return false;
    }
    const uint32_t cell_blocks = static_cast<uint32_t>(std::min<size_t>(65535, (cells + threads - 1) / threads));
    correction_phasor_kernel<<<cell_blocks, threads>>>(phasor_, observed_, delay_, applied_phase_,
                                                       !usable.empty(), window.rows,
                                                       window.subcarriers);
    status = cudaGetLastError();
    if (status != cudaSuccess) {
      set_error(error, std::string("CUDA correction-phasor launch: ") + cudaGetErrorString(status));
      return false;
    }
    const uint32_t sample_blocks = static_cast<uint32_t>(
        std::min<size_t>(65535, (samples + threads - 1) / threads));
    apply_common_phasor_kernel<<<sample_blocks, threads>>>(values_, observed_, phasor_,
                                                           window.antennas, cells);
    status = cudaGetLastError();
    if (status == cudaSuccess)
      status = cudaMemcpy(corrected_host_.data(), values_, samples * sizeof(*values_),
                          cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
      set_error(error, std::string("CUDA sync-correction output transfer: ") + cudaGetErrorString(status));
      return false;
    }
    std::memcpy(window.values.data(), corrected_host_.data(), samples * sizeof(*values_));
    if (error) error->clear();
    return true;
  }

private:
  void reset()
  {
    cudaFree(phasor_);
    cudaFree(complex_count_);
    cudaFree(energy_);
    cudaFree(coherent_);
    cudaFree(spatial_);
    cudaFree(applied_phase_);
    cudaFree(delay_);
    cudaFree(row_delay_);
    cudaFree(observed_);
    cudaFree(values_);
    phasor_ = nullptr;
    complex_count_ = nullptr;
    energy_ = nullptr;
    coherent_ = nullptr;
    spatial_ = nullptr;
    applied_phase_ = delay_ = row_delay_ = nullptr;
    observed_ = nullptr;
    values_ = nullptr;
    rows_ = subcarriers_ = antennas_ = 0;
    corrected_host_.clear();
    row_delay_host_.clear();
    delay_host_.clear();
    applied_phase_host_.clear();
    coherent_host_.clear();
    energy_host_.clear();
    complex_count_host_.clear();
  }

  cufftComplex* values_ = nullptr;
  uint8_t* observed_ = nullptr;
  double* row_delay_ = nullptr;
  double* delay_ = nullptr;
  double* applied_phase_ = nullptr;
  cufftDoubleComplex* spatial_ = nullptr;
  cufftDoubleComplex* coherent_ = nullptr;
  double* energy_ = nullptr;
  uint32_t* complex_count_ = nullptr;
  cufftComplex* phasor_ = nullptr;
  uint32_t rows_ = 0;
  uint32_t subcarriers_ = 0;
  uint32_t antennas_ = 0;
  std::vector<cufftComplex> corrected_host_;
  std::vector<double> row_delay_host_;
  std::vector<double> delay_host_;
  std::vector<double> applied_phase_host_;
  std::vector<cufftDoubleComplex> coherent_host_;
  std::vector<double> energy_host_;
  std::vector<uint32_t> complex_count_host_;
};

CufftBatch& shared_batch()
{
  static CufftBatch value;
  return value;
}

std::mutex& shared_batch_mutex()
{
  static std::mutex value;
  return value;
}

CorrectionWorkspace& shared_correction_workspace()
{
  static CorrectionWorkspace value;
  return value;
}

std::mutex& shared_correction_mutex()
{
  static std::mutex value;
  return value;
}

} // namespace

bool compute_sync_frontend_cuda(const CfrWindow& window,
                                uint32_t oversample,
                                CudaSyncFrontEnd& output,
                                std::string* error)
{
  if (!window.valid() || !oversample
      || window.subcarriers > std::numeric_limits<uint32_t>::max() / oversample) {
    set_error(error, "invalid CFR window or CUDA IFFT length");
    return false;
  }
  std::lock_guard<std::mutex> lock(shared_batch_mutex());
  return shared_batch().run(window, oversample, output, error);
}

bool warmup_sync_cuda(uint32_t maximum_rows, uint32_t subcarriers, std::string* error)
{
  if (maximum_rows < 3 || subcarriers < 3) {
    set_error(error, "invalid CUDA sync warmup dimensions");
    return false;
  }
  const int exponent = std::min(4, std::max(1,
      static_cast<int>(std::ceil(std::log2(std::sqrt(maximum_rows))))));
  const uint32_t oversample = 1u << exponent;
  if (subcarriers > std::numeric_limits<uint32_t>::max() / oversample) {
    set_error(error, "CUDA sync warmup IFFT length overflow");
    return false;
  }
  std::lock_guard<std::mutex> lock(shared_batch_mutex());
  return shared_batch().configure(subcarriers * oversample, subcarriers, maximum_rows, error);
}

bool apply_sync_correction_cuda(
    CfrWindow& window,
    const SyncEstimate& estimate,
    double delay_reference_bin,
    const std::optional<std::array<std::complex<double>, 4>>& los_spatial,
    std::string* error)
{
  if (!window.valid() || !std::isfinite(delay_reference_bin)
      || (los_spatial && window.antennas > los_spatial->size())) {
    set_error(error, "invalid CUDA sync-correction input");
    return false;
  }
  std::lock_guard<std::mutex> lock(shared_correction_mutex());
  return shared_correction_workspace().run(window, estimate, delay_reference_bin,
                                           los_spatial, error);
}

bool warmup_sync_correction_cuda(uint32_t maximum_rows,
                                 uint32_t subcarriers,
                                 uint32_t antennas,
                                 std::string* error)
{
  std::lock_guard<std::mutex> lock(shared_correction_mutex());
  return shared_correction_workspace().configure(maximum_rows, subcarriers, antennas, error);
}

} // namespace nr_isac
