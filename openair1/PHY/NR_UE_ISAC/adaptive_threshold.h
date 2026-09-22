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
                                             uint32_t guard_doppler_bins = 2,
                                             // OUR ADAPTATION (stage-6 fix, 2026-09-18): training
                                             // cells that are known NOT to be noise samples.
                                             // excluded_doppler_bin: the zero-Doppler bin, where
                                             // the post-stage-3 static residual lives (UINT32_MAX
                                             // = none).  exclude_cut_range_bin: drop the CUT's own
                                             // range bin so its Doppler sidelobes do not train it.
                                             uint32_t excluded_doppler_bin = UINT32_MAX,
                                             bool exclude_cut_range_bin = false);

double adaptive_z_threshold(double false_objects_per_cpi, uint32_t maximum_proposals);
double false_object_budget(double intensity_per_s, double represented_s,
                           double minimum = 1e-4, double maximum = 0.5);

} // namespace nr_isac
