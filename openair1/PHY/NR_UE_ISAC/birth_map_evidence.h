/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "small_matrix.h"
#include <array>
#include <cstdint>
#include <vector>
namespace nr_isac {
struct ReceiverDetectionBatch;
struct LocalMapEvidence {
  bool available = false;
  double score = 0.0;
  double visibility = 0.0;
  uint64_t lookups = 0;
  double unconditioned_score = 0.0;
};
// A previously supported object explaining the same RF component. Coordinates
// and support are in range/range-rate units. Not a power to subtract from a map.
struct MapExplanation {
  std::array<double, 2> measurement{};
  Matrix covariance{2, 2};
};
// Local, read-only hypothesis query. Does not create detections or mutate CLEAN.
LocalMapEvidence local_map_evidence(const ReceiverDetectionBatch& batch,
    const std::array<double, 6>& state, const Matrix& covariance, uint32_t maximum_cells,
    const std::vector<MapExplanation>& explanations = {});
}
