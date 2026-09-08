/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace nr_isac {

inline double quantile(std::vector<double> values, double q)
{
  if (values.empty() || !(q >= 0.0 && q <= 1.0))
    throw std::invalid_argument("invalid quantile input");
  std::sort(values.begin(), values.end());
  const double index = q * static_cast<double>(values.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(index));
  const size_t hi = static_cast<size_t>(std::ceil(index));
  const double frac = index - static_cast<double>(lo);
  return values[lo] * (1.0 - frac) + values[hi] * frac;
}

inline double median(std::vector<double> values) { return quantile(std::move(values), 0.5); }

inline double median_absolute_deviation(const std::vector<double>& values, double centre)
{
  std::vector<double> deviations;
  deviations.reserve(values.size());
  for (double value : values)
    deviations.push_back(std::abs(value - centre));
  return median(std::move(deviations));
}

/** Peter J. Acklam's rational approximation to the standard-normal inverse CDF. */
inline double normal_inverse_cdf(double p)
{
  if (!(p > 0.0 && p < 1.0))
    throw std::invalid_argument("normal inverse CDF requires 0 < p < 1");
  constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                          -2.759285104469687e+02, 1.383577518672690e+02,
                          -3.066479806614716e+01, 2.506628277459239e+00};
  constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                          -1.556989798598866e+02, 6.680131188771972e+01,
                          -1.328068155288572e+01};
  constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                          -2.400758277161838e+00, -2.549732539343734e+00,
                           4.374664141464968e+00, 2.938163982698783e+00};
  constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                          2.445134137142996e+00, 3.754408661907416e+00};
  constexpr double lower = 0.02425;
  constexpr double upper = 1.0 - lower;
  if (p < lower) {
    const double q = std::sqrt(-2.0 * std::log(p));
    return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5])
           / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }
  if (p > upper) {
    const double q = std::sqrt(-2.0 * std::log(1.0 - p));
    return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5])
           / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }
  const double q = p - 0.5;
  const double r = q * q;
  return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q
         / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

} // namespace nr_isac
