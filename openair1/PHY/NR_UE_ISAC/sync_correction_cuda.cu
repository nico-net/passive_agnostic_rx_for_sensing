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
    case CUFFT_INCOMPLETE_PARAMETER_LIST: return "CUFFT_INCOMPLETE_PARAMETER_LIST";
    case CUFFT_INVALID_DEVICE: return "CUFFT_INVALID_DEVICE";
    case CUFFT_PARSE_ERROR: return "CUFFT_PARSE_ERROR";
    case CUFFT_NO_WORKSPACE: return "CUFFT_NO_WORKSPACE";
    case CUFFT_NOT_IMPLEMENTED: return "CUFFT_NOT_IMPLEMENTED";
    case CUFFT_LICENSE_ERROR: return "CUFFT_LICENSE_ERROR";
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

} // namespace nr_isac
