/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <cstdint>
#include <vector>

namespace nr_isac {

struct FamilyAlignmentStats;

/** CUDA implementations of the allocation-family operations. Family membership is constructed
 * by the shared host code so CPU and CUDA use byte-identical keys and row ordering.
 */
FamilyAlignmentStats align_allocation_families_cuda(
    CfrWindow& window,
    bool subtract_static_reference,
    const std::vector<std::vector<uint32_t>>& family_rows);

double estimate_current_cpi_variance_cuda(
    const CfrWindow& window,
    const std::vector<std::vector<uint32_t>>& family_rows,
    uint64_t* differenced_samples);

} // namespace nr_isac
