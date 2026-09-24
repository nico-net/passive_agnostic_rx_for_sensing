/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "coherent_core.h"
#include "coherent_types.h"

namespace nr_isac::coherent {

struct UeFix {
  bool valid = false;
  Vec3 pos;
  double sigma_m = 0;
  std::array<double, kCh> direct_delay_s{};
};

/** UE position from its direct-path arrival per channel (TDOA, grid search over the volume). */
UeFix localise_ue(const CfrWindow& ul, const Axes& a, const Geometry& geo, const Volume& vol, double pfa);
/** Geometry for focusing with the UE as illuminator. */
Geometry ue_geometry(const Geometry& geo, const UeFix& ue);

} // namespace nr_isac::coherent
