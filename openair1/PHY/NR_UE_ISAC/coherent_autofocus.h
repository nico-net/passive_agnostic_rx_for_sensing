/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "coherent_types.h"
#include <vector>
namespace nr_isac::coherent {
class Autofocus {
public:
  Autofocus(const Geometry& surveyed, double survey_sigma_m, double fc_hz);
  /** Feed one detection that belongs to a CONFIRMED track.
   *
   *  CONTRACT: focus_geometry is the geometry the CALLER used to focus/reconstruct this
   *  detection's terms -- i.e. whatever geometry() last returned when the CPI this detection
   *  came from was formed. It is NOT assumed to be the surveyed geometry: the normal pipeline
   *  fetches geometry() once per CPI, focuses with it, and only then calls add() on the
   *  resulting detections, so terms routinely already carry this estimator's own prior
   *  correction baked in. Passing surveyed unchanged (e.g. `af.geometry()` before anything has
   *  ever been applied) is the degenerate case and exactly what "terms formed with the surveyed
   *  geometry" means in the unit tests. See the focus-geometry-contract derivation at the top of
   *  coherent_autofocus.cc for why this must be passed explicitly. */
  void add(const Detection& d, const Geometry& focus_geometry);
  /** Current refined geometry: surveyed position, with each channel's estimated correction
   *  applied only where applied() is true (otherwise that channel reports the surveyed position
   *  unchanged). This is exactly what a caller should pass back in as focus_geometry for
   *  detections formed after fetching it. */
  Geometry geometry() const;
  /** norm of the correction actually applied per channel (0 where applied() is false). */
  std::array<double, kCh> correction_norm_m() const;
  /** Per-channel runtime acceptance verdict: true where the accumulated data statistically
   *  reject the surveyed position, using the MARGINAL posterior covariance (the conservative
   *  choice -- see coherent_autofocus.cc). */
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
