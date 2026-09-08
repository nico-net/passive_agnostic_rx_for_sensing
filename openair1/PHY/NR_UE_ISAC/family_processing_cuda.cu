/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "family_processing_cuda.h"

#include "sync_correction.h"

#include <cuda_runtime.h>
#include <thrust/device_ptr.h>
#include <thrust/sort.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace nr_isac {
namespace {

void check(cudaError_t status, const char* operation)
{
  if (status != cudaSuccess)
    throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}

struct AlignJob {
  uint32_t reference_row;
  uint32_t target_row;
  uint32_t column_offset;
  uint32_t column_count;
};

struct FamilyJob {
  uint32_t row_offset;
  uint32_t row_count;
  uint32_t column_offset;
  uint32_t column_count;
};

struct DifferenceJob {
  uint32_t left_row;
  uint32_t right_row;
  uint32_t column_offset;
  uint32_t column_count;
};

__device__ inline float2 multiply(float2 left, float2 right)
{
  return make_float2(left.x * right.x - left.y * right.y,
                     left.x * right.y + left.y * right.x);
}

__global__ void align_jobs_kernel(float2* values,
                                  const AlignJob* jobs,
                                  const uint32_t* columns,
                                  double* phases,
                                  double* weights,
                                  uint8_t* valid_jobs,
                                  uint32_t job_count,
                                  uint32_t antennas,
                                  uint32_t rows,
                                  uint32_t subcarriers)
{
  const uint32_t job_index = blockIdx.x;
  if (job_index >= job_count) return;
  const AlignJob job = jobs[job_index];
  const size_t scratch_offset = static_cast<size_t>(job_index) * subcarriers;
  __shared__ double delay_bins;
  __shared__ double gain_real;
  __shared__ double gain_imag;
  __shared__ uint32_t fit_valid;
  __shared__ uint32_t gain_valid;

  if (threadIdx.x == 0) {
    double unwrap = 0.0, previous = 0.0, weight_sum = 0.0;
    double weighted_k = 0.0, weighted_phase = 0.0;
    for (uint32_t index = 0; index < job.column_count; ++index) {
      const uint32_t subcarrier = columns[job.column_offset + index];
      const float2 reference = values[(static_cast<size_t>(job.reference_row)) * subcarriers
                                      + subcarrier];
      const float2 value = values[(static_cast<size_t>(job.target_row)) * subcarriers
                                  + subcarrier];
      const double cross_real = static_cast<double>(value.x) * reference.x
                                + static_cast<double>(value.y) * reference.y;
      const double cross_imag = static_cast<double>(value.y) * reference.x
                                - static_cast<double>(value.x) * reference.y;
      const double raw = atan2(cross_imag, cross_real);
      if (index) {
        const double step = raw - previous;
        if (step > PI) unwrap -= 2.0 * PI;
        else if (step < -PI) unwrap += 2.0 * PI;
      }
      previous = raw;
      const double phase = raw + unwrap;
      const double weight = hypot(cross_real, cross_imag);
      phases[scratch_offset + index] = phase;
      weights[scratch_offset + index] = weight;
      weight_sum += weight;
      weighted_k += weight * subcarrier;
      weighted_phase += weight * phase;
    }
    fit_valid = weight_sum > DBL_MIN;
    if (fit_valid) {
      const double mean_k = weighted_k / weight_sum;
      const double mean_phase = weighted_phase / weight_sum;
      double numerator = 0.0, denominator = 0.0;
      for (uint32_t index = 0; index < job.column_count; ++index) {
        const double x = columns[job.column_offset + index] - mean_k;
        const double weight = weights[scratch_offset + index];
        numerator += weight * x * (phases[scratch_offset + index] - mean_phase);
        denominator += weight * x * x;
      }
      fit_valid = denominator > DBL_MIN;
      if (fit_valid)
        delay_bins = -(numerator / denominator) * subcarriers / (2.0 * PI);
    }
    valid_jobs[job_index] = static_cast<uint8_t>(fit_valid);
  }
  __syncthreads();
  if (!fit_valid) return;

  for (size_t index = threadIdx.x;
       index < static_cast<size_t>(antennas) * job.column_count;
       index += blockDim.x) {
    const uint32_t antenna = static_cast<uint32_t>(index / job.column_count);
    const uint32_t column_index = static_cast<uint32_t>(index % job.column_count);
    const uint32_t subcarrier = columns[job.column_offset + column_index];
    const double angle = 2.0 * PI * delay_bins * subcarrier / subcarriers;
    double sine = 0.0, cosine = 0.0;
    sincos(angle, &sine, &cosine);
    const float2 correction = make_float2(static_cast<float>(cosine), static_cast<float>(sine));
    const size_t sample = (static_cast<size_t>(antenna) * rows + job.target_row) * subcarriers
                          + subcarrier;
    values[sample] = multiply(values[sample], correction);
  }
  __syncthreads();

  if (threadIdx.x == 0) {
    double numerator_real = 0.0, numerator_imag = 0.0, denominator = 0.0;
    for (uint32_t index = 0; index < job.column_count; ++index) {
      const uint32_t subcarrier = columns[job.column_offset + index];
      const float2 reference = values[(static_cast<size_t>(job.reference_row)) * subcarriers
                                      + subcarrier];
      const float2 value = values[(static_cast<size_t>(job.target_row)) * subcarriers
                                  + subcarrier];
      numerator_real += static_cast<double>(reference.x) * value.x
                        + static_cast<double>(reference.y) * value.y;
      numerator_imag += static_cast<double>(reference.x) * value.y
                        - static_cast<double>(reference.y) * value.x;
      denominator += static_cast<double>(reference.x) * reference.x
                     + static_cast<double>(reference.y) * reference.y;
    }
    gain_real = denominator > FLT_MIN
                    ? numerator_real / denominator : 1.0;
    gain_imag = denominator > FLT_MIN
                    ? numerator_imag / denominator : 0.0;
    gain_valid = hypot(gain_real, gain_imag) > FLT_MIN
                 && isfinite(gain_real) && isfinite(gain_imag);
  }
  __syncthreads();
  if (!gain_valid) return;
  const float gain_r = static_cast<float>(gain_real);
  const float gain_i = static_cast<float>(gain_imag);
  const float gain_denominator = gain_r * gain_r + gain_i * gain_i;
  for (size_t index = threadIdx.x;
       index < static_cast<size_t>(antennas) * job.column_count;
       index += blockDim.x) {
    const uint32_t antenna = static_cast<uint32_t>(index / job.column_count);
    const uint32_t column_index = static_cast<uint32_t>(index % job.column_count);
    const uint32_t subcarrier = columns[job.column_offset + column_index];
    const size_t sample = (static_cast<size_t>(antenna) * rows + job.target_row) * subcarriers
                          + subcarrier;
    const float2 value = values[sample];
    values[sample] = make_float2((value.x * gain_r + value.y * gain_i) / gain_denominator,
                                 (value.y * gain_r - value.x * gain_i) / gain_denominator);
  }
}

__global__ void family_static_kernel(float2* values,
                                     const FamilyJob* families,
                                     const uint32_t* family_rows,
                                     const uint32_t* columns,
                                     uint32_t family_count,
                                     uint32_t antennas,
                                     uint32_t rows,
                                     uint32_t subcarriers)
{
  const uint32_t family_index = blockIdx.x;
  if (family_index >= family_count) return;
  const FamilyJob family = families[family_index];
  const uint32_t effective_columns = family.row_count < 2 ? subcarriers : family.column_count;
  const size_t index = static_cast<size_t>(blockIdx.y) * blockDim.x + threadIdx.x;
  if (index >= static_cast<size_t>(antennas) * effective_columns) return;
  const uint32_t antenna = static_cast<uint32_t>(index / effective_columns);
  const uint32_t column_index = static_cast<uint32_t>(index % effective_columns);
  const uint32_t subcarrier = family.row_count < 2
                                  ? column_index : columns[family.column_offset + column_index];
  if (family.row_count < 2) {
    const uint32_t row = family_rows[family.row_offset];
    values[(static_cast<size_t>(antenna) * rows + row) * subcarriers + subcarrier] =
        make_float2(0.0f, 0.0f);
    return;
  }
  double mean_real = 0.0, mean_imag = 0.0;
  for (uint32_t index = 0; index < family.row_count; ++index) {
    const uint32_t row = family_rows[family.row_offset + index];
    const float2 value = values[(static_cast<size_t>(antenna) * rows + row) * subcarriers
                                + subcarrier];
    mean_real += value.x;
    mean_imag += value.y;
  }
  const float mean_r = static_cast<float>(mean_real / family.row_count);
  const float mean_i = static_cast<float>(mean_imag / family.row_count);
  for (uint32_t index = 0; index < family.row_count; ++index) {
    const uint32_t row = family_rows[family.row_offset + index];
    float2& value = values[(static_cast<size_t>(antenna) * rows + row) * subcarriers
                           + subcarrier];
    value.x -= mean_r;
    value.y -= mean_i;
  }
}

__global__ void difference_power_kernel(const float2* values,
                                        const DifferenceJob* jobs,
                                        const uint32_t* columns,
                                        double* powers,
                                        unsigned long long* valid_count,
                                        uint32_t job_count,
                                        uint32_t antennas,
                                        uint32_t rows,
                                        uint32_t subcarriers)
{
  const uint32_t job_index = blockIdx.x;
  if (job_index >= job_count) return;
  const DifferenceJob job = jobs[job_index];
  for (size_t index = static_cast<size_t>(blockIdx.y) * blockDim.x + threadIdx.x;
       index < static_cast<size_t>(antennas) * job.column_count;
       index += static_cast<size_t>(blockDim.x) * gridDim.y) {
    const uint32_t antenna = static_cast<uint32_t>(index / job.column_count);
    const uint32_t column_index = static_cast<uint32_t>(index % job.column_count);
    const uint32_t subcarrier = columns[job.column_offset + column_index];
    const float2 left = values[(static_cast<size_t>(antenna) * rows + job.left_row) * subcarriers
                               + subcarrier];
    const float2 right = values[(static_cast<size_t>(antenna) * rows + job.right_row) * subcarriers
                                + subcarrier];
    const double real = static_cast<double>(right.x) - left.x;
    const double imag = static_cast<double>(right.y) - left.y;
    const double power = real * real + imag * imag;
    if (isfinite(power) && power > 0.0) {
      const unsigned long long output = atomicAdd(valid_count, 1ULL);
      powers[output] = power;
    }
  }
}

__global__ void raw_power_kernel(const float2* values,
                                 const uint8_t* observed,
                                 double* powers,
                                 unsigned long long* valid_count,
                                 uint32_t antennas,
                                 uint32_t rows,
                                 uint32_t subcarriers,
                                 size_t samples)
{
  const size_t cells = static_cast<size_t>(rows) * subcarriers;
  for (size_t index = blockIdx.x * blockDim.x + threadIdx.x;
       index < samples;
       index += static_cast<size_t>(blockDim.x) * gridDim.x) {
    if (!observed[index % cells]) continue;
    const float2 value = values[index];
    // std::norm(complex<float>) in the host fallback rounds the sum in float before promotion.
    const double power = value.x * value.x + value.y * value.y;
    if (isfinite(power) && power > 0.0) {
      const unsigned long long output = atomicAdd(valid_count, 1ULL);
      powers[output] = power;
    }
  }
}

struct Workspace {
  float2* values = nullptr;
  uint8_t* observed = nullptr;
  AlignJob* align_jobs = nullptr;
  FamilyJob* family_jobs = nullptr;
  DifferenceJob* difference_jobs = nullptr;
  uint32_t* rows = nullptr;
  uint32_t* columns = nullptr;
  double* phases = nullptr;
  double* weights = nullptr;
  uint8_t* valid_jobs = nullptr;
  double* powers = nullptr;
  unsigned long long* valid_count = nullptr;
  std::vector<std::complex<float>> host_values;
  size_t sample_capacity = 0, cell_capacity = 0;
  uint32_t row_capacity = 0, column_capacity = 0;

  ~Workspace()
  {
    cudaFree(valid_count); cudaFree(powers); cudaFree(valid_jobs); cudaFree(weights);
    cudaFree(phases); cudaFree(columns); cudaFree(rows); cudaFree(difference_jobs);
    cudaFree(family_jobs); cudaFree(align_jobs); cudaFree(observed); cudaFree(values);
  }

  void ensure(size_t samples, size_t cells, uint32_t row_entries, uint32_t column_entries)
  {
    if (samples > sample_capacity) {
      cudaFree(values); cudaFree(powers);
      check(cudaMalloc(&values, samples * sizeof(*values)), "allocate family values");
      check(cudaMalloc(&powers, samples * sizeof(*powers)), "allocate family powers");
      sample_capacity = samples;
    }
    if (cells > cell_capacity) {
      cudaFree(observed); cudaFree(phases); cudaFree(weights);
      check(cudaMalloc(&observed, cells * sizeof(*observed)), "allocate family mask");
      check(cudaMalloc(&phases, cells * sizeof(*phases)), "allocate family phases");
      check(cudaMalloc(&weights, cells * sizeof(*weights)), "allocate family weights");
      cell_capacity = cells;
    }
    if (row_entries > row_capacity) {
      cudaFree(rows); cudaFree(align_jobs); cudaFree(family_jobs);
      cudaFree(difference_jobs); cudaFree(valid_jobs);
      check(cudaMalloc(&rows, row_entries * sizeof(*rows)), "allocate family rows");
      check(cudaMalloc(&align_jobs, row_entries * sizeof(*align_jobs)), "allocate alignment jobs");
      check(cudaMalloc(&family_jobs, row_entries * sizeof(*family_jobs)), "allocate family jobs");
      check(cudaMalloc(&difference_jobs, row_entries * sizeof(*difference_jobs)),
            "allocate difference jobs");
      check(cudaMalloc(&valid_jobs, row_entries * sizeof(*valid_jobs)),
            "allocate alignment validity");
      row_capacity = row_entries;
    }
    if (column_entries > column_capacity) {
      cudaFree(columns);
      check(cudaMalloc(&columns, column_entries * sizeof(*columns)), "allocate family columns");
      column_capacity = column_entries;
    }
    if (!valid_count)
      check(cudaMalloc(&valid_count, sizeof(*valid_count)), "allocate family valid count");
  }
};

thread_local Workspace workspace;

struct PackedFamilies {
  std::vector<uint32_t> rows;
  std::vector<uint32_t> columns;
  std::vector<FamilyJob> families;
  std::vector<AlignJob> alignments;
  std::vector<DifferenceJob> differences;
  uint64_t difference_samples = 0;
};

PackedFamilies pack(const CfrWindow& window,
                    const std::vector<std::vector<uint32_t>>& family_rows)
{
  PackedFamilies packed;
  for (const auto& rows : family_rows) {
    FamilyJob family{};
    family.row_offset = packed.rows.size();
    family.row_count = rows.size();
    packed.rows.insert(packed.rows.end(), rows.begin(), rows.end());
    family.column_offset = packed.columns.size();
    if (!rows.empty())
      for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier)
        if (window.observed[window.cell(rows.front(), subcarrier)])
          packed.columns.push_back(subcarrier);
    family.column_count = packed.columns.size() - family.column_offset;
    packed.families.push_back(family);
    if (rows.size() >= 2 && family.column_count) {
      for (size_t index = 1; index < rows.size(); ++index) {
        packed.alignments.push_back({rows.front(), rows[index], family.column_offset,
                                     family.column_count});
        packed.differences.push_back({rows[index - 1], rows[index], family.column_offset,
                                      family.column_count});
        packed.difference_samples += static_cast<uint64_t>(window.antennas)
                                     * family.column_count;
      }
    }
  }
  return packed;
}

void upload_common(const CfrWindow& window, const PackedFamilies& packed)
{
  const size_t cells = static_cast<size_t>(window.rows) * window.subcarriers;
  const size_t samples = cells * window.antennas;
  workspace.ensure(samples, cells, std::max<uint32_t>(1, packed.rows.size()),
                   std::max<uint32_t>(1, packed.columns.size()));
  static_assert(sizeof(std::complex<float>) == sizeof(float2));
  check(cudaMemcpy(workspace.values, window.values.data(), samples * sizeof(float2),
                   cudaMemcpyHostToDevice), "upload family values");
  check(cudaMemcpy(workspace.observed, window.observed.data(), cells,
                   cudaMemcpyHostToDevice), "upload family mask");
  if (!packed.rows.empty())
    check(cudaMemcpy(workspace.rows, packed.rows.data(), packed.rows.size() * sizeof(uint32_t),
                     cudaMemcpyHostToDevice), "upload family rows");
  if (!packed.columns.empty())
    check(cudaMemcpy(workspace.columns, packed.columns.data(),
                     packed.columns.size() * sizeof(uint32_t), cudaMemcpyHostToDevice),
          "upload family columns");
}

double sorted_median(uint64_t count)
{
  if (!count) throw std::invalid_argument("CPI has no positive covariance samples");
  thrust::device_ptr<double> begin(workspace.powers);
  thrust::sort(begin, begin + count);
  double middle[2]{};
  if (count & 1) {
    check(cudaMemcpy(middle, workspace.powers + count / 2, sizeof(double),
                     cudaMemcpyDeviceToHost), "download covariance median");
    return middle[0];
  }
  check(cudaMemcpy(middle, workspace.powers + count / 2 - 1, 2 * sizeof(double),
                   cudaMemcpyDeviceToHost), "download covariance median pair");
  return 0.5 * (middle[0] + middle[1]);
}

} // namespace

FamilyAlignmentStats align_allocation_families_cuda(
    CfrWindow& window,
    bool subtract_static_reference,
    const std::vector<std::vector<uint32_t>>& family_rows)
{
  const PackedFamilies packed = pack(window, family_rows);
  upload_common(window, packed);
  if (!packed.alignments.empty()) {
    check(cudaMemcpy(workspace.align_jobs, packed.alignments.data(),
                     packed.alignments.size() * sizeof(AlignJob), cudaMemcpyHostToDevice),
          "upload alignment jobs");
    align_jobs_kernel<<<packed.alignments.size(), 256>>>(
        workspace.values, workspace.align_jobs, workspace.columns, workspace.phases,
        workspace.weights, workspace.valid_jobs, packed.alignments.size(), window.antennas,
        window.rows, window.subcarriers);
    check(cudaGetLastError(), "launch allocation-family alignment");
  }
  if (subtract_static_reference && !packed.families.empty()) {
    check(cudaMemcpy(workspace.family_jobs, packed.families.data(),
                     packed.families.size() * sizeof(FamilyJob), cudaMemcpyHostToDevice),
          "upload static-family jobs");
    const uint32_t blocks_y = (window.antennas * window.subcarriers + 255) / 256;
    family_static_kernel<<<dim3(packed.families.size(), blocks_y), 256>>>(
        workspace.values, workspace.family_jobs, workspace.rows, workspace.columns,
        packed.families.size(), window.antennas, window.rows, window.subcarriers);
    check(cudaGetLastError(), "launch allocation-family static subtraction");
  }
  // Stage the result so an optional CUDA failure cannot leave a partially overwritten host
  // window for the CPU fallback to process.
  workspace.host_values.resize(window.values.size());
  check(cudaMemcpy(workspace.host_values.data(), workspace.values,
                   workspace.host_values.size() * sizeof(std::complex<float>), cudaMemcpyDeviceToHost),
        "download aligned family values");
  std::vector<uint8_t> valid(packed.alignments.size());
  if (!valid.empty())
    check(cudaMemcpy(valid.data(), workspace.valid_jobs, valid.size(), cudaMemcpyDeviceToHost),
          "download alignment validity");
  window.values.swap(workspace.host_values);
  FamilyAlignmentStats stats;
  stats.families = family_rows.size();
  for (const auto& rows : family_rows) {
    if (rows.size() < 2) ++stats.singleton_rows;
    else ++stats.repeated_families;
  }
  stats.aligned_rows = std::count(valid.begin(), valid.end(), uint8_t{1});
  return stats;
}

double estimate_current_cpi_variance_cuda(
    const CfrWindow& window,
    const std::vector<std::vector<uint32_t>>& family_rows,
    uint64_t* differenced_samples)
{
  const PackedFamilies packed = pack(window, family_rows);
  upload_common(window, packed);
  unsigned long long zero = 0;
  check(cudaMemcpy(workspace.valid_count, &zero, sizeof(zero), cudaMemcpyHostToDevice),
        "clear covariance count");
  if (!packed.differences.empty()) {
    check(cudaMemcpy(workspace.difference_jobs, packed.differences.data(),
                     packed.differences.size() * sizeof(DifferenceJob), cudaMemcpyHostToDevice),
          "upload covariance jobs");
    const uint32_t blocks_y = (window.antennas * window.subcarriers + 255) / 256;
    difference_power_kernel<<<dim3(packed.differences.size(), blocks_y), 256>>>(
        workspace.values, workspace.difference_jobs, workspace.columns, workspace.powers,
        workspace.valid_count, packed.differences.size(), window.antennas, window.rows,
        window.subcarriers);
    check(cudaGetLastError(), "launch within-family covariance");
  }
  unsigned long long valid = 0;
  check(cudaMemcpy(&valid, workspace.valid_count, sizeof(valid), cudaMemcpyDeviceToHost),
        "download covariance count");
  if (differenced_samples) *differenced_samples = packed.difference_samples;
  if (valid)
    return sorted_median(valid) / (2.0 * std::log(2.0));

  check(cudaMemcpy(workspace.valid_count, &zero, sizeof(zero), cudaMemcpyHostToDevice),
        "clear raw covariance count");
  const size_t samples = window.values.size();
  const uint32_t blocks = std::min<size_t>(65535, (samples + 255) / 256);
  raw_power_kernel<<<blocks, 256>>>(workspace.values, workspace.observed, workspace.powers,
                                   workspace.valid_count, window.antennas, window.rows,
                                   window.subcarriers, samples);
  check(cudaGetLastError(), "launch raw covariance fallback");
  check(cudaMemcpy(&valid, workspace.valid_count, sizeof(valid), cudaMemcpyDeviceToHost),
        "download raw covariance count");
  return sorted_median(valid) / std::log(2.0);
}

} // namespace nr_isac
