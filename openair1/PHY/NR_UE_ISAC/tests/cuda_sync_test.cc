/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "sync_correction.h"
#include "sync_correction_cuda.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace nr_isac;

namespace {

CfrWindow sync_fixture()
{
  CfrWindow window;
  window.antennas = 1;
  window.rows = 8;
  window.subcarriers = 13;
  window.scs_hz = 30000.0;
  window.fc_hz = 3.5e9;
  window.values.resize(static_cast<size_t>(window.rows) * window.subcarriers);
  window.observed.assign(static_cast<size_t>(window.rows) * window.subcarriers, 1);
  window.row_time_slots.resize(window.rows);
  window.row_slot_idx.resize(window.rows);
  window.row_slot_frac.assign(window.rows, 0.0);
  window.row_source_mask.assign(window.rows, 1);
  for (uint32_t row = 0; row < window.rows; ++row) {
    window.row_time_slots[row] = row;
    window.row_slot_idx[row] = row;
    for (uint32_t carrier = 0; carrier < window.subcarriers; ++carrier) {
      const float phase = -2.0f * static_cast<float>(PI) * 2.25f * carrier / window.subcarriers
                          + 0.003f * row;
      window.values[window.sample(0, row, carrier)] =
          std::polar(1.0f + 0.001f * (carrier % 7), phase);
    }
  }
  window.observed[window.cell(2, 4)] = 0;
  return window;
}

void require_close(double actual, double expected, double tolerance, const char* label)
{
  if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > tolerance) {
    std::fprintf(stderr, "%s differs: CUDA=%.17g CPU=%.17g tolerance=%.3g\n",
                 label, actual, expected, tolerance);
    throw std::runtime_error(label);
  }
}

} // namespace

int main()
{
  const CfrWindow window = sync_fixture();
  std::string error;
  if (!warmup_sync_cuda(32, window.subcarriers, &error))
    throw std::runtime_error("CUDA sync warmup failed: " + error);
  CudaSyncFrontEnd front_end;
  if (!compute_sync_frontend_cuda(window, 4, front_end, &error))
    throw std::runtime_error("CUDA sync front end failed: " + error);
  setenv("NR_ISAC_DISABLE_CUDA_SYNC", "1", 1);
  const SyncEstimate cpu = estimate_sync(window);
  unsetenv("NR_ISAC_DISABLE_CUDA_SYNC");
  setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
  const SyncEstimate cuda = estimate_sync(window);
  unsetenv("NR_ISAC_REQUIRE_CUDA");
  if (cuda.anchor_bin != cpu.anchor_bin || cuda.anchor_halfwidth_bins != cpu.anchor_halfwidth_bins
      || cuda.admitted_rows != cpu.admitted_rows)
    throw std::runtime_error("CUDA sync discrete result differs from CPU fallback");
  require_close(cuda.los_bins, cpu.los_bins, 2e-4, "LOS delay");
  require_close(cuda.sto_bins, cpu.sto_bins, 2e-4, "STO");
  require_close(cuda.sfo_ppm, cpu.sfo_ppm, 1e-3, "SFO");
  require_close(cuda.cfo_hz, cpu.cfo_hz, 1e-3, "CFO");
  setenv("NR_ISAC_DISABLE_CUDA_SYNC", "1", 1);
  setenv("NR_ISAC_REQUIRE_CUDA", "1", 1);
  bool rejected_conflict = false;
  try {
    static_cast<void>(estimate_sync(window));
  } catch (const std::runtime_error&) {
    rejected_conflict = true;
  }
  unsetenv("NR_ISAC_DISABLE_CUDA_SYNC");
  unsetenv("NR_ISAC_REQUIRE_CUDA");
  if (!rejected_conflict)
    throw std::runtime_error("NR_ISAC_REQUIRE_CUDA did not reject a forced CPU fallback");
  return 0;
}
