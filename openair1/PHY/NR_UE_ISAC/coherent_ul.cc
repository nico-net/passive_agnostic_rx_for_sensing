/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_ul.h"
#include <cmath>
#include <limits>
#include <vector>

namespace nr_isac::coherent {

UeFix localise_ue(const CfrWindow& ul, const Axes& a, const Geometry& geo, const Volume& vol, double pfa)
{
  UeFix f;
  const LosEstimate L = find_los(ul, a, pfa);  // earliest significant arrival = UE direct path
  for (uint32_t i = 0; i < kCh; ++i)
    if (!L.found[i]) return f;
  f.direct_delay_s = L.delay_s;
  const double sd = a.delay_step_s / std::sqrt(12.0);  // arrival quantisation (sub-bin estimate)
  const Grid g = envelope_grid(vol, a);
  double best = std::numeric_limits<double>::infinity();
  size_t bv = 0;
  std::vector<double> cost(g.size());
  for (size_t v = 0; v < g.size(); ++v) {
    const Vec3 x = g.at(v);
    double c = 0;
    for (uint32_t i = 1; i < kCh; ++i) {
      const double meas = L.delay_s[i] - L.delay_s[0];
      const double model = (dist(x, geo.rx[i]) - dist(x, geo.rx[0])) / kC;
      c += (meas - model) * (meas - model) / (2 * sd * sd);
    }
    cost[v] = c;
    if (c < best) { best = c; bv = v; }
  }
  // sigma = RMS distance of voxels inside the chi2 +1 contour
  double s2 = 0;
  size_t n = 0;
  const Vec3 b = g.at(bv);
  for (size_t v = 0; v < g.size(); ++v)
    if (cost[v] <= best + 1.0) { s2 += norm2(g.at(v) - b); ++n; }
  f.pos = b;
  f.sigma_m = std::max(std::sqrt(s2 / std::max<size_t>(1, n)), g.step / std::sqrt(12.0));
  f.valid = true;
  return f;
}

Geometry ue_geometry(const Geometry& geo, const UeFix& ue)
{
  Geometry g = geo;
  g.tx = ue.pos;
  return g;
}

} // namespace nr_isac::coherent
