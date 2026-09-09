/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <cstdint>
#include <vector>

namespace nr_isac {

LocalStatistic cut_excluded_local_statistic(const std::vector<double>& likelihood,
                                             uint32_t range_bins,
                                             uint32_t doppler_bins,
                                             uint32_t range_bin,
                                             uint32_t doppler_bin,
                                             uint32_t training_range_bins = 12,
                                             uint32_t training_doppler_bins = 12,
                                             uint32_t guard_range_bins = 2,
                                             uint32_t guard_doppler_bins = 2);

double adaptive_z_threshold(double false_objects_per_cpi, uint32_t maximum_proposals);
double false_object_budget(double intensity_per_s, double represented_s,
                           double minimum = 1e-4, double maximum = 0.5);

} // namespace nr_isac
