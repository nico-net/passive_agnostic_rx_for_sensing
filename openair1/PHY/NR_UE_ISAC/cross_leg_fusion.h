/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <vector>

namespace nr_isac {

struct CrossLegFusionConfig {
  double angle_gate_chi2 = 9.21034037197618;
  double minimum_cross_leg_stddev_deg = 3.0;
};

struct CrossLegFusionDiagnostics {
  bool ul_motion_active = false;
  uint32_t candidate_specific_matches = 0;
  uint32_t suppressed_dl_candidates = 0;
  std::string mode;
  bool runtime_ul_tx_position_used = false;
};

struct CrossLegFusionResult {
  std::vector<Detection> dl_measurements;
  std::vector<AoaEstimate> auxiliary_ul_aoa;
  CrossLegFusionDiagnostics diagnostics;
};

/** Pair DL and UL only in the shared receive-bearing domain. UL bistatic range/rate is never
 * inserted into the DL geometry because the passive receiver does not know UE position.
 */
CrossLegFusionResult confirm_and_fuse_dl_with_ul(
    const std::vector<Detection>& dl_measurements,
    const std::vector<Detection>& ul_measurements,
    bool use_ul_aoa,
    CrossLegFusionConfig config = {});

} // namespace nr_isac
