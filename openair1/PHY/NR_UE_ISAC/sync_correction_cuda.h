/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nr_isac {

struct CudaSyncFrontEnd {
  uint32_t anchor_unsigned = 0;
  int anchor = 0;
  int halfwidth = 0;
  std::vector<double> delays;
  std::vector<double> contrasts;
  std::vector<double> peak_powers;
};

/**
 * Run the CUDA-resident front end used by estimate_sync().
 *
 * The batched IFFT, row normalization, persistent profile, local-peak search,
 * and row medians match estimate_sync_gpu().  Only its short regression vectors
 * return to the host; statistical selection and model fitting remain in
 * sync_correction.cc.
 */
bool compute_sync_frontend_cuda(const CfrWindow& window,
                                uint32_t oversample,
                                CudaSyncFrontEnd& output,
                                std::string* error);

/** Allocate the process-wide CUDA context, buffers, and cuFFT plan before CFR admission. */
bool warmup_sync_cuda(uint32_t maximum_rows, uint32_t subcarriers, std::string* error);

} // namespace nr_isac
