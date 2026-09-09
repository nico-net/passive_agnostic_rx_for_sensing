/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "adaptive_threshold.h"

#include "robust_stats.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace nr_isac {

LocalStatistic cut_excluded_local_statistic(const std::vector<double>& surface,
                                             uint32_t nr,
                                             uint32_t nd,
                                             uint32_t cut_r,
                                             uint32_t cut_d,
                                             uint32_t train_r,
                                             uint32_t train_d,
                                             uint32_t guard_r,
                                             uint32_t guard_d)
{
  if (!nr || !nd || surface.size() != static_cast<size_t>(nr) * nd || cut_r >= nr || cut_d >= nd)
    throw std::invalid_argument("local CUT map/index mismatch");
  if (guard_r > train_r || guard_d > train_d)
    throw std::invalid_argument("CUT guard exceeds training radius");
  std::vector<double> values;
  const uint32_t lo_r = cut_r > train_r ? cut_r - train_r : 0;
  const uint32_t hi_r = std::min(nr - 1, cut_r + train_r);
  values.reserve(static_cast<size_t>(hi_r - lo_r + 1) * (2 * train_d + 1));
  for (uint32_t r = lo_r; r <= hi_r; ++r) {
    for (int64_t offset = -static_cast<int64_t>(train_d); offset <= static_cast<int64_t>(train_d); ++offset) {
      int64_t wrapped = (static_cast<int64_t>(cut_d) + offset) % static_cast<int64_t>(nd);
      if (wrapped < 0)
        wrapped += nd;
      const uint32_t d = static_cast<uint32_t>(wrapped);
      const uint32_t circular = std::min((d > cut_d ? d - cut_d : cut_d - d),
                                         nd - (d > cut_d ? d - cut_d : cut_d - d));
      if ((r > cut_r ? r - cut_r : cut_r - r) <= guard_r && circular <= guard_d)
        continue;
      const double value = surface[(size_t)r * nd + d];
      if (std::isfinite(value) && value > 0.0)
        values.push_back(value);
    }
  }
  LocalStatistic result;
  if (values.size() < 16) {
    values.clear();
    for (double value : surface)
      if (std::isfinite(value) && value > 0.0)
        values.push_back(value);
    result.fallback_global = true;
  }
  if (values.empty())
    return result;
  std::vector<double> log_values;
  log_values.reserve(values.size());
  for (double value : values)
    log_values.push_back(std::log(std::max(value, std::numeric_limits<double>::min())));
  result.log_background_median = median(log_values);
  result.log_background_mad = median_absolute_deviation(log_values, result.log_background_median);
  result.log_background_sigma = 1.4826 * result.log_background_mad;
  if (!(result.log_background_sigma > 0.0))
    result.log_background_sigma = (quantile(log_values, 0.75) - quantile(log_values, 0.25)) / 1.349;
  result.log_background_sigma = std::max(result.log_background_sigma, 1e-12);
  result.cut_score = surface[(size_t)cut_r * nd + cut_d];
  result.valid = std::isfinite(result.cut_score) && result.cut_score > 0.0;
  if (result.valid)
    result.z = (std::log(result.cut_score) - result.log_background_median) / result.log_background_sigma;
  result.training_cells = static_cast<uint32_t>(values.size());
  return result;
}

double adaptive_z_threshold(double budget, uint32_t proposals)
{
  if (!(budget > 0.0 && budget < 1.0) || proposals == 0)
    throw std::invalid_argument("invalid adaptive false-object design");
  const double tail = std::min(0.499999999999, std::max(1e-12, budget / proposals));
  return normal_inverse_cdf(1.0 - tail);
}

double false_object_budget(double intensity, double represented, double minimum, double maximum)
{
  if (!(intensity > 0.0) || !(represented > 0.0) || !(minimum > 0.0 && minimum <= maximum && maximum < 1.0))
    throw std::invalid_argument("invalid false-object budget input");
  return std::min(maximum, std::max(minimum, intensity * represented));
}

} // namespace nr_isac
