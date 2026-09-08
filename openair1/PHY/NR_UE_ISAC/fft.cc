/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "fft.h"

#include "pipeline_types.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace nr_isac {
namespace {

bool power_of_two(size_t n) { return n && !(n & (n - 1)); }

size_t next_power_of_two(size_t n)
{
  size_t result = 1;
  while (result < n) {
    if (result > (size_t(-1) >> 1))
      throw std::overflow_error("FFT length overflow");
    result <<= 1;
  }
  return result;
}

void radix2(std::vector<std::complex<double>>& a, bool inverse)
{
  const size_t n = a.size();
  if (!power_of_two(n))
    throw std::invalid_argument("radix-2 FFT length is not a power of two");
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(a[i], a[j]);
  }
  const double sign = inverse ? 1.0 : -1.0;
  for (size_t len = 2; len <= n; len <<= 1) {
    const double angle = sign * 2.0 * PI / static_cast<double>(len);
    const std::complex<double> root(std::cos(angle), std::sin(angle));
    for (size_t start = 0; start < n; start += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t j = 0; j < len / 2; ++j) {
        const auto u = a[start + j];
        const auto v = a[start + j + len / 2] * w;
        a[start + j] = u + v;
        a[start + j + len / 2] = u - v;
        w *= root;
      }
    }
  }
  if (inverse)
    for (auto& value : a)
      value /= static_cast<double>(n);
}

struct BluesteinPlan {
  size_t convolution_size = 0;
  std::vector<std::complex<double>> chirp;
  std::vector<std::complex<double>> kernel_spectrum;
};

const BluesteinPlan& bluestein_plan(size_t n, bool inverse)
{
  // A transform length and direction have an invariant chirp and convolution kernel.  The sensing
  // detector executes thousands of equal-length transforms per CPI; rebuilding and FFTing this
  // kernel for every row is pure setup work.  A thread-local cache preserves the original scalar
  // FFT operation order while avoiding locks in the single causal sensing worker.
  thread_local std::map<std::pair<size_t, bool>, BluesteinPlan> plans;
  const auto key = std::make_pair(n, inverse);
  const auto found = plans.find(key);
  if (found != plans.end())
    return found->second;

  BluesteinPlan plan;
  plan.convolution_size = next_power_of_two(2 * n - 1);
  plan.chirp.resize(n);
  plan.kernel_spectrum.assign(plan.convolution_size, {});
  const double sign = inverse ? 1.0 : -1.0;
  for (size_t i = 0; i < n; ++i) {
    const uint64_t square_mod = (static_cast<uint64_t>(i) * i) % (2u * static_cast<uint64_t>(n));
    const double angle = sign * PI * static_cast<double>(square_mod) / static_cast<double>(n);
    plan.chirp[i] = {std::cos(angle), std::sin(angle)};
    const std::complex<double> inverse_chirp = std::conj(plan.chirp[i]);
    plan.kernel_spectrum[i] = inverse_chirp;
    if (i != 0)
      plan.kernel_spectrum[plan.convolution_size - i] = inverse_chirp;
  }
  radix2(plan.kernel_spectrum, false);
  return plans.emplace(key, std::move(plan)).first->second;
}

void bluestein(std::vector<std::complex<double>>& values, bool inverse)
{
  const size_t n = values.size();
  const BluesteinPlan& plan = bluestein_plan(n, inverse);
  const size_t m = plan.convolution_size;
  std::vector<std::complex<double>> a(m);
  for (size_t i = 0; i < n; ++i)
    a[i] = values[i] * plan.chirp[i];
  radix2(a, false);
  for (size_t i = 0; i < m; ++i)
    a[i] *= plan.kernel_spectrum[i];
  radix2(a, true);
  for (size_t i = 0; i < n; ++i) {
    values[i] = a[i] * plan.chirp[i];
    if (inverse)
      values[i] /= static_cast<double>(n);
  }
}

} // namespace

void fft_inplace(std::vector<std::complex<double>>& values, bool inverse)
{
  if (values.empty())
    return;
  if (power_of_two(values.size()))
    radix2(values, inverse);
  else
    bluestein(values, inverse);
}

std::vector<std::complex<double>> fft(const std::vector<std::complex<double>>& values, bool inverse)
{
  auto result = values;
  fft_inplace(result, inverse);
  return result;
}

} // namespace nr_isac
