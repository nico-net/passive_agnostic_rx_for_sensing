/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <optional>

namespace nr_isac {

struct RateGate {
  std::optional<double> center_mps;
  std::optional<double> half_width_mps;
};

/** Native equivalent of clean_components_gpu + collapse_clean_multipath. */
DetectorResult detect_clean(const CfrWindow& window,
                            const PipelineConfig& config,
                            const RateGate& rate_gate = {},
                            uint32_t minimum_range_bin = 0);

} // namespace nr_isac
