/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <cstdint>
#include <unordered_map>

namespace nr_isac {

class AdaptiveClutterMap {
public:
  AdaptiveClutterMap(double alpha = 0.15, double margin = 1.5,
                     double direct_cells = 1.5, double max_static_mps = 1.0);
  void reset();
  void update(double range_m, double rate_mps, double score,
              double range_res_m, double rate_res_mps);
  bool is_static(double range_m, double rate_mps, double score,
                 double range_res_m, double rate_res_mps, bool update_map = true);

private:
  int range_bin(double range_m, double range_res_m) const;
  double alpha_;
  double margin_;
  double direct_cells_;
  double max_static_mps_;
  std::unordered_map<int, double> cells_;
  std::unordered_map<int, uint32_t> hits_;
  double direct_power_ = 0.0;
  uint32_t direct_hits_ = 0;
};

} // namespace nr_isac
