/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <array>
#include <vector>
#include "coherent_types.h"
#include "pipeline_types.h"

namespace nr_isac::coherent {

/** Range-Doppler cube, layout [ch][range][dopp]. */
struct RangeDoppler {
  Axes axes;
  std::vector<cf> v;
  size_t idx(uint32_t ch, uint32_t r, uint32_t d) const { return ((size_t)ch * axes.n_range + r) * axes.n_dopp + d; }
};

struct LosEstimate {
  std::array<double, kCh> delay_s{};
  std::array<cd, kCh> tap{};
  std::array<double, kCh> snr{};
  std::array<bool, kCh> found{};
};

struct RowSync {
  std::vector<double> phase_rad;
  std::vector<double> delay_s;
};

struct RdResult {
  RangeDoppler rd;
  std::array<cd, kCh> los_tap{};
  std::array<double, kCh> noise{};
};

Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps);
LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa);
RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& los);
RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& los, const RowSync& sync);
/** x with Q(shape, x) = p (regularised upper incomplete gamma), integer shape. */
double gamma_upper_quantile(uint32_t shape, double p);

} // namespace nr_isac::coherent
