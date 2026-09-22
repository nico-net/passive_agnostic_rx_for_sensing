/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include "pipeline_types.h"

#include <complex>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nr_isac {

struct CausalClutterStats {
  uint32_t families = 0;
  uint32_t bootstrap_families = 0;
  uint32_t registered_families = 0;
  uint64_t predicted_cells = 0;
  uint64_t updated_cells = 0;
  uint64_t innovation_rejections = 0;
  double mean_update_gain = 0.0;
};

/** Causal, receiver-local static-channel predictor.
 *
 * Allocation families are never mixed.  A CPI is filtered with the state frozen at the end of
 * the preceding CPI; only after filtering is the state updated from the current family mean.
 * The update gain is obtained from learned state/process uncertainty and the measured current-CPI
 * variance.  The stationary/change decision uses the BIC penalty implied by the number of samples,
 * so this class has no scenario, class, range, speed, or manually selected forgetting constants.
 */
class CausalClutterFilter {
public:
  CausalClutterStats filter(CfrWindow& window, double current_cpi_variance);
  void reset();

private:
  struct FamilyState {
    uint32_t antennas = 0;
    uint32_t subcarriers = 0;
    uint64_t cpis = 0;
    std::vector<std::complex<double>> mean;
    std::vector<double> state_variance;
    std::vector<double> process_variance;
  };
  std::map<std::string, FamilyState> families_;
};

} // namespace nr_isac
