/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "coherent_types.h"
#include <vector>
namespace nr_isac::coherent {
class Autofocus {
public:
  Autofocus(const Geometry& surveyed, double survey_sigma_m, double fc_hz);
  /** Feed one detection that belongs to a CONFIRMED track. */
  void add(const Detection& d);
  /** Current refined geometry: surveyed position, with each channel's estimated correction
   *  applied only where applied() is true (otherwise that channel reports the surveyed position
   *  unchanged). */
  Geometry geometry() const;
  /** norm of the correction actually applied per channel (0 where applied() is false). */
  std::array<double, kCh> correction_norm_m() const;
  /** Per-channel runtime acceptance verdict: true where the accumulated data statistically
   *  reject the surveyed position (see the chi-square test in coherent_autofocus.cc). */
  std::array<bool, kCh> applied() const;
private:
  Geometry surveyed_;
  double k_;                    // 2*pi*fc/c
  Matrix J_;                    // 3*kCh joint information matrix
  std::vector<double> b_;       // 3*kCh joint information vector
  std::vector<double> solve() const;                                  // xhat = J^-1 b (Cholesky)
  Geometry raw_geometry(const std::vector<double>& xhat) const;        // survey + xhat, UNGATED
};
} // namespace nr_isac::coherent
