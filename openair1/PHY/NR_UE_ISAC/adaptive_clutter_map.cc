/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "adaptive_clutter_map.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nr_isac {

AdaptiveClutterMap::AdaptiveClutterMap(double alpha, double margin, double direct_cells,
                                       double max_static_mps)
    : alpha_(alpha), margin_(margin), direct_cells_(direct_cells), max_static_mps_(max_static_mps)
{
  if (!(alpha > 0.0 && alpha <= 1.0) || margin < 1.0 || !(max_static_mps > 0.0))
    throw std::invalid_argument("invalid adaptive clutter-map configuration");
}

void AdaptiveClutterMap::reset()
{
  cells_.clear(); hits_.clear(); direct_power_ = 0.0; direct_hits_ = 0;
}

int AdaptiveClutterMap::range_bin(double range_m, double range_res_m) const
{
  if (!(range_res_m > 0.0))
    throw std::invalid_argument("range resolution must be positive");
  return std::max(0, static_cast<int>(std::llround(range_m / range_res_m)));
}

void AdaptiveClutterMap::update(double range_m, double rate_mps, double score,
                                double range_res_m, double rate_res_mps)
{
  const double guard = std::min(max_static_mps_, 1.5 * rate_res_mps);
  if (std::abs(rate_mps) > guard || !(score > 0.0))
    return;
  if (range_m <= direct_cells_ * range_res_m) {
    direct_power_ = direct_hits_ ? (1.0 - alpha_) * direct_power_ + alpha_ * score : score;
    ++direct_hits_;
  }
  const int bin = range_bin(range_m, range_res_m);
  const auto found = cells_.find(bin);
  if (found == cells_.end()) {
    cells_[bin] = score; hits_[bin] = 1;
  } else {
    found->second = (1.0 - alpha_) * found->second + alpha_ * score;
    ++hits_[bin];
  }
}

bool AdaptiveClutterMap::is_static(double range_m, double rate_mps, double score,
                                   double range_res_m, double rate_res_mps, bool update_map)
{
  if (!(score > 0.0))
    return false;
  const double guard = std::min(max_static_mps_, 1.5 * rate_res_mps);
  if (range_m <= direct_cells_ * range_res_m) {
    if (update_map && std::abs(rate_mps) <= guard)
      update(range_m, rate_mps, score, range_res_m, rate_res_mps);
    return true;
  }
  if (std::abs(rate_mps) > guard)
    return false;
  const int bin = range_bin(range_m, range_res_m);
  const auto found = cells_.find(bin);
  if (found != cells_.end()) {
    const bool clutter = score <= found->second * margin_;
    if (update_map)
      update(range_m, rate_mps, score, range_res_m, rate_res_mps);
    return clutter;
  }
  if (update_map && score <= 100.0) {
    update(range_m, rate_mps, score, range_res_m, rate_res_mps);
    return true;
  }
  return false;
}

} // namespace nr_isac
