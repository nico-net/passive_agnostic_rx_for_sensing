/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include "coherent_types.h"
namespace nr_isac::coherent {
class Autofocus {
public:
  Autofocus(const Geometry& surveyed, double survey_sigma_m, double fc_hz);
  /** Feed one detection that belongs to a CONFIRMED track. */
  void add(const Detection& d);
  /** Current refined geometry (surveyed + estimated per-antenna corrections). */
  Geometry geometry() const;
  std::array<double, kCh> correction_norm_m() const;
private:
  Geometry surveyed_; double k_;                 // 2*pi*fc/c
  std::array<std::array<double, 9>, kCh> J_{};   // information matrices
  std::array<std::array<double, 3>, kCh> b_{};   // information vectors
};
} // namespace nr_isac::coherent
