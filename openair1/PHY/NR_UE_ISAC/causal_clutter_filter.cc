/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "causal_clutter_filter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace nr_isac {
namespace {

std::string family_key(const CfrWindow& window, uint32_t row)
{
  const int32_t fraction_28 = static_cast<int32_t>(
      std::llround(window.row_slot_frac[row] * 28.0));
  std::string key(sizeof(uint32_t) + sizeof(int32_t) + window.subcarriers, '\0');
  std::memcpy(key.data(), &window.row_source_mask[row], sizeof(uint32_t));
  std::memcpy(key.data() + sizeof(uint32_t), &fraction_28, sizeof(int32_t));
  std::memcpy(key.data() + sizeof(uint32_t) + sizeof(int32_t),
              window.observed.data() + window.cell(row, 0), window.subcarriers);
  return key;
}

std::map<std::string, std::vector<uint32_t>> group_families(const CfrWindow& window)
{
  std::map<std::string, std::vector<uint32_t>> result;
  for (uint32_t row = 0; row < window.rows; ++row)
    result[family_key(window, row)].push_back(row);
  return result;
}

} // namespace

CausalClutterStats CausalClutterFilter::filter(CfrWindow& window,
                                                double current_cpi_variance)
{
  if (!window.valid()) throw std::invalid_argument("invalid causal-clutter window");
  if (!(std::isfinite(current_cpi_variance) && current_cpi_variance > 0.0))
    throw std::invalid_argument("causal clutter needs positive measured CPI variance");

  CausalClutterStats stats;
  double gain_sum = 0.0;
  const auto grouped = group_families(window);
  stats.families = grouped.size();
  for (const auto& item : grouped) {
    const auto& rows = item.second;
    if (rows.empty()) continue;
    const size_t cells = static_cast<size_t>(window.antennas) * window.subcarriers;
    std::vector<std::complex<double>> current_mean(cells);
    for (uint32_t antenna = 0; antenna < window.antennas; ++antenna)
      for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
        if (!window.observed[window.cell(rows.front(), subcarrier)]) continue;
        auto& mean = current_mean[static_cast<size_t>(antenna) * window.subcarriers + subcarrier];
        for (uint32_t row : rows)
          mean += static_cast<std::complex<double>>(
              window.values[window.sample(antenna, row, subcarrier)]);
        mean /= static_cast<double>(rows.size());
      }

    auto found = families_.find(item.first);
    const bool bootstrap = found == families_.end()
                           || found->second.antennas != window.antennas
                           || found->second.subcarriers != window.subcarriers;
    if (bootstrap) {
      FamilyState state;
      state.antennas = window.antennas;
      state.subcarriers = window.subcarriers;
      state.cpis = 1;
      state.mean = current_mean;
      const double observation_variance = current_cpi_variance / rows.size();
      state.state_variance.assign(cells, observation_variance);
      state.process_variance.assign(cells, 0.0);
      families_[item.first] = std::move(state);
      ++stats.bootstrap_families;
      // The first occurrence has no past prediction.  Removing its within-CPI family mean is the
      // only causal-at-close way to prevent the direct path from consuming CLEAN iterations.
      for (uint32_t antenna = 0; antenna < window.antennas; ++antenna)
        for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
          if (!window.observed[window.cell(rows.front(), subcarrier)]) continue;
          const auto mean = static_cast<std::complex<float>>(
              current_mean[static_cast<size_t>(antenna) * window.subcarriers + subcarrier]);
          for (uint32_t row : rows)
            window.values[window.sample(antenna, row, subcarrier)] -= mean;
        }
      continue;
    }

    FamilyState& state = found->second;
    std::vector<uint32_t> columns;
    for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier)
      if (window.observed[window.cell(rows.front(), subcarrier)])
        columns.push_back(subcarrier);
    std::vector<std::complex<double>> registration(window.subcarriers, {1.0, 0.0});
    std::complex<double> gain(1.0, 0.0);
    if (columns.size() >= 2) {
      std::vector<double> phase(columns.size()), weight(columns.size());
      double unwrap = 0.0, previous_phase = 0.0;
      for (size_t index = 0; index < columns.size(); ++index) {
        const uint32_t subcarrier = columns[index];
        const std::complex<double> cross =
            current_mean[subcarrier] * std::conj(state.mean[subcarrier]);
        const double raw = std::arg(cross);
        if (index) {
          const double step = raw - previous_phase;
          if (step > PI) unwrap -= 2.0 * PI;
          else if (step < -PI) unwrap += 2.0 * PI;
        }
        previous_phase = raw;
        phase[index] = raw + unwrap;
        weight[index] = std::abs(cross);
      }
      const double weight_sum = std::accumulate(weight.begin(), weight.end(), 0.0);
      if (weight_sum > std::numeric_limits<double>::min()) {
        double mean_column = 0.0, mean_phase = 0.0;
        for (size_t index = 0; index < columns.size(); ++index) {
          mean_column += weight[index] * columns[index];
          mean_phase += weight[index] * phase[index];
        }
        mean_column /= weight_sum;
        mean_phase /= weight_sum;
        double numerator = 0.0, denominator = 0.0;
        for (size_t index = 0; index < columns.size(); ++index) {
          const double centered = columns[index] - mean_column;
          numerator += weight[index] * centered * (phase[index] - mean_phase);
          denominator += weight[index] * centered * centered;
        }
        const double slope = denominator > std::numeric_limits<double>::min()
                                 ? numerator / denominator : 0.0;
        std::complex<double> gain_numerator{};
        double gain_denominator = 0.0;
        for (uint32_t subcarrier : columns) {
          registration[subcarrier] = std::polar(
              1.0, slope * (subcarrier - mean_column));
          gain_numerator += std::conj(state.mean[subcarrier])
              * current_mean[subcarrier] * std::conj(registration[subcarrier]);
          gain_denominator += std::norm(state.mean[subcarrier]);
        }
        if (gain_denominator > std::numeric_limits<double>::min()) {
          const std::complex<double> candidate = gain_numerator / gain_denominator;
          if (std::isfinite(candidate.real()) && std::isfinite(candidate.imag())
              && std::abs(candidate) > std::numeric_limits<double>::min()) {
            gain = candidate;
            ++stats.registered_families;
          }
        }
      }
    }
    const double normalized_observation_variance = current_cpi_variance
        / rows.size() / std::max(std::norm(gain), std::numeric_limits<double>::min());
    // A changed complex mean adds two real parameters.  BIC therefore admits a stationary update
    // when the two-dimensional normalized innovation does not repay 2*log(N) complexity.
    const double bic_change_threshold = 2.0 * std::log(
        static_cast<double>(std::max<uint64_t>(2, rows.size() * (state.cpis + 1))));
    for (uint32_t antenna = 0; antenna < window.antennas; ++antenna)
      for (uint32_t subcarrier = 0; subcarrier < window.subcarriers; ++subcarrier) {
        if (!window.observed[window.cell(rows.front(), subcarrier)]) continue;
        const size_t cell = static_cast<size_t>(antenna) * window.subcarriers + subcarrier;
        const std::complex<double> prior = state.mean[cell];
        const std::complex<double> transform = gain * registration[subcarrier];
        const std::complex<double> predicted_current = transform * prior;
        for (uint32_t row : rows)
          window.values[window.sample(antenna, row, subcarrier)]
              -= static_cast<std::complex<float>>(predicted_current);
        ++stats.predicted_cells;

        const double predicted_variance = std::max(
            state.state_variance[cell] + state.process_variance[cell],
            std::numeric_limits<double>::min());
        const double innovation_variance = predicted_variance
                                           + normalized_observation_variance;
        const std::complex<double> normalized_current = current_mean[cell] / transform;
        const std::complex<double> innovation = normalized_current - prior;
        const double innovation_power = std::norm(innovation);
        const double normalized_innovation = innovation_power / innovation_variance;
        const double excess = std::max(0.0, innovation_power - state.state_variance[cell]
                                             - normalized_observation_variance);
        // A cumulative learned process variance remains causal and has no forgetting factor.
        state.process_variance[cell] +=
            (excess - state.process_variance[cell]) / static_cast<double>(state.cpis + 1);
        if (normalized_innovation <= bic_change_threshold) {
          const double gain = predicted_variance / innovation_variance;
          state.mean[cell] += gain * innovation;
          state.state_variance[cell] = std::max(
              (1.0 - gain) * predicted_variance, std::numeric_limits<double>::min());
          gain_sum += gain;
          ++stats.updated_cells;
        } else {
          // Freeze the static estimate for this CPI.  The learned process uncertainty still grows,
          // allowing genuine channel drift to be accepted later without absorbing one-off targets.
          state.state_variance[cell] = predicted_variance;
          ++stats.innovation_rejections;
        }
      }
    ++state.cpis;
  }
  if (stats.updated_cells) stats.mean_update_gain = gain_sum / stats.updated_cells;
  return stats;
}

void CausalClutterFilter::reset()
{
  families_.clear();
}

} // namespace nr_isac
