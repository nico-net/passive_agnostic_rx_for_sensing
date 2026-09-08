/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <optional>
#include <utility>

namespace nr_isac {

SyncEstimate estimate_sync(const CfrWindow& window);

/** Apply selected STO/SFO and per-row measured LOS phase identically to every antenna. */
void apply_sync_correction(CfrWindow& window,
                           const SyncEstimate& estimate,
                           double delay_reference_bin,
                           const std::optional<std::array<std::complex<double>, 4>>& los_spatial_steering);

class IndependentClockTracker {
public:
  SyncEstimate update(const SyncEstimate& measurement, double represented_time_s,
                      uint32_t subcarriers, double scs_hz);
  void reset();

private:
  bool have_delay_ = false;
  double delay_ = 0.0;
  double delay_variance_ = 0.0;
  uint32_t delay_updates_ = 0;
  std::optional<double> last_time_s_;
};

struct FamilyAlignmentStats {
  uint32_t families = 0;
  uint32_t repeated_families = 0;
  uint32_t aligned_rows = 0;
  uint32_t singleton_rows = 0;
};

/** Python align_allocation_families_gpu, with identical family keys and per-antenna mean removal. */
FamilyAlignmentStats align_allocation_families(CfrWindow& window, bool subtract_static_reference);
/** Align two independent CFR views using one CUDA batch when available. */
std::pair<FamilyAlignmentStats, FamilyAlignmentStats> align_allocation_families_pair(
    CfrWindow& fused, CfrWindow& dl_only, bool subtract_static_reference);
void subtract_allocation_family_static(CfrWindow& window);

/** Robust homoscedastic current-CPI variance used by the Python diagonal-weight path. */
double estimate_current_cpi_variance(const CfrWindow& window,
                                     uint32_t* family_count = nullptr,
                                     uint64_t* differenced_samples = nullptr);

/** AoA source policy. Dimensions/timestamps stay unchanged; only mask rows are disabled. */
std::vector<uint8_t> aoa_observed_mask(const CfrWindow& window, bool aoa_enable, bool aoa_ul_enable);

} // namespace nr_isac
