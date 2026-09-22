/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "origin_hypothesis.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace nr_isac {
namespace {

constexpr double LOG_TWO_PI = 1.8378770664093454835606594728112352797;

double median(std::vector<double> values)
{
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 ? values[middle]
                           : 0.5 * (values[middle - 1] + values[middle]);
}

double numeric_variance_floor(double scale)
{
  return std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(scale));
}

} // namespace

struct CausalOriginHypotheses::ScalarAutoregression {
  bool have_previous = false;
  double previous = 0.0;
  uint64_t pairs = 0;
  double sum_x = 0.0, sum_y = 0.0;
  double sum_xx = 0.0, sum_yy = 0.0, sum_xy = 0.0;

  void add(double value)
  {
    if (!std::isfinite(value)) return;
    if (have_previous) {
      ++pairs;
      sum_x += previous;
      sum_y += value;
      sum_xx += previous * previous;
      sum_yy += value * value;
      sum_xy += previous * value;
    }
    previous = value;
    have_previous = true;
  }

  bool identifiable() const { return pairs > 3; }

  double structured_log_bayes_factor() const
  {
    if (!identifiable()) return 0.0;
    const double n = static_cast<double>(pairs);
    const double centered_xx = sum_xx - sum_x * sum_x / n;
    const double centered_xy = sum_xy - sum_x * sum_y / n;
    if (!(centered_xx > numeric_variance_floor(sum_xx))) return 0.0;
    // The alternative is a stationary AR(1) residual process. The [-1,1] restriction is the
    // mathematical stationarity domain, not a tuned motion or scene threshold.
    const double rho = std::clamp(centered_xy / centered_xx, -1.0, 1.0);
    const double intercept = (sum_y - rho * sum_x) / n;
    double sse = sum_yy + n * intercept * intercept + rho * rho * sum_xx
                 - 2.0 * intercept * sum_y - 2.0 * rho * sum_xy
                 + 2.0 * intercept * rho * sum_x;
    sse = std::max(sse, numeric_variance_floor(sum_yy));
    const double independent_sse = std::max(
        sum_yy - sum_y * sum_y / n, numeric_variance_floor(sum_yy));
    // Both hypotheses fit the same marginal mean/variance. The comparison therefore measures
    // lag dependence only; detector covariance scale error cannot masquerade as multipath.
    const double direct_bic = n * (LOG_TWO_PI + std::log(independent_sse / n) + 1.0)
                              + 2.0 * std::log(n);
    // Structured model adds only the lag coefficient (three parameters total).
    const double structured_bic = n * (LOG_TWO_PI + std::log(sse / n) + 1.0)
                                  + 3.0 * std::log(n);
    return 0.5 * (direct_bic - structured_bic);
  }
};

struct CausalOriginHypotheses::BivariateMoments {
  uint64_t count = 0;
  double sum_x = 0.0, sum_y = 0.0;
  double sum_xx = 0.0, sum_yy = 0.0, sum_xy = 0.0;

  void add(double x, double y)
  {
    if (!(std::isfinite(x) && std::isfinite(y))) return;
    ++count;
    sum_x += x; sum_y += y;
    sum_xx += x * x; sum_yy += y * y; sum_xy += x * y;
  }

  bool identifiable() const { return count > 5; }

  double structured_log_bayes_factor() const
  {
    if (!identifiable()) return 0.0;
    const double n = static_cast<double>(count);
    const double mean_x = sum_x / n, mean_y = sum_y / n;
    const double var_x = std::max(sum_xx / n - mean_x * mean_x,
                                  numeric_variance_floor(sum_xx / n));
    const double var_y = std::max(sum_yy / n - mean_y * mean_y,
                                  numeric_variance_floor(sum_yy / n));
    const double covariance = sum_xy / n - mean_x * mean_y;
    const double determinant = std::max(
        var_x * var_y - covariance * covariance,
        numeric_variance_floor(var_x * var_y));
    // Independence and correlation models fit identical marginal means and variances. Their sole
    // difference is the cross-covariance parameter, so covariance miscalibration is not evidence.
    const double direct_bic = n * (
        2.0 * LOG_TWO_PI + std::log(var_x * var_y) + 2.0) + 4.0 * std::log(n);
    // The alternative bivariate Gaussian adds one covariance entry.
    const double structured_bic = n * (2.0 * LOG_TWO_PI + std::log(determinant) + 2.0)
                                  + 5.0 * std::log(n);
    return 0.5 * (direct_bic - structured_bic);
  }
};

CausalOriginHypotheses::~CausalOriginHypotheses() = default;

CausalOriginHypotheses::CausalOriginHypotheses(const CausalOriginHypotheses& other)
    : observed_epochs_(other.observed_epochs_),
      cross_receiver_epochs_(other.cross_receiver_epochs_),
      velocity_mps_(other.velocity_mps_),
      velocity_covariance_(other.velocity_covariance_),
      have_velocity_(other.have_velocity_),
      ul_direct_log_bayes_factor_(other.ul_direct_log_bayes_factor_),
      have_ul_evidence_(other.have_ul_evidence_),
      exclusivity_log_bayes_factor_(other.exclusivity_log_bayes_factor_),
      exclusivity_competitor_track_id_(other.exclusivity_competitor_track_id_),
      have_exclusivity_evidence_(other.have_exclusivity_evidence_)
{
  for (size_t dimension = 0; dimension < temporal_.size(); ++dimension)
    if (other.temporal_[dimension])
      temporal_[dimension] = std::make_shared<ScalarAutoregression>(
          *other.temporal_[dimension]);
  for (const auto& [key, value] : other.cross_receiver_)
    if (value)
      cross_receiver_[key] = std::make_shared<BivariateMoments>(*value);
}

CausalOriginHypotheses& CausalOriginHypotheses::operator=(
    const CausalOriginHypotheses& other)
{
  if (this == &other) return *this;
  CausalOriginHypotheses copy(other);
  *this = std::move(copy);
  return *this;
}

void CausalOriginHypotheses::observe_epoch(
    const std::vector<OriginInnovation>& innovations,
    const std::array<double, 3>& velocity_mps,
    const Matrix& velocity_covariance,
    uint32_t cross_receiver_support)
{
  if (innovations.empty()) return; // A missing receiver/CPI is absence of evidence.
  if (velocity_covariance.rows() != 3 || velocity_covariance.cols() != 3)
    throw std::invalid_argument("origin hypothesis velocity covariance must be 3x3");
  std::map<uint32_t, std::array<double, 2>> by_receiver;
  std::array<double, 2> epoch_mean{};
  size_t admitted = 0;
  for (const OriginInnovation& innovation : innovations) {
    if (!(std::isfinite(innovation.whitened[0])
          && std::isfinite(innovation.whitened[1])))
      continue;
    by_receiver[innovation.receiver_index] = innovation.whitened;
    epoch_mean[0] += innovation.whitened[0];
    epoch_mean[1] += innovation.whitened[1];
    ++admitted;
  }
  if (!admitted) return;
  ++observed_epochs_;
  if (by_receiver.size() >= cross_receiver_support) ++cross_receiver_epochs_;
  for (size_t dimension = 0; dimension < 2; ++dimension) {
    epoch_mean[dimension] /= static_cast<double>(admitted);
    if (!temporal_[dimension]) temporal_[dimension] = std::make_shared<ScalarAutoregression>();
    temporal_[dimension]->add(epoch_mean[dimension]);
  }
  for (auto left = by_receiver.begin(); left != by_receiver.end(); ++left) {
    for (auto right = std::next(left); right != by_receiver.end(); ++right) {
      for (uint32_t dimension = 0; dimension < 2; ++dimension) {
        const std::array<uint32_t, 3> key{left->first, right->first, dimension};
        auto& moments = cross_receiver_[key];
        if (!moments) moments = std::make_shared<BivariateMoments>();
        moments->add(left->second[dimension], right->second[dimension]);
      }
    }
  }
  velocity_mps_ = velocity_mps;
  velocity_covariance_ = velocity_covariance;
  have_velocity_ = true;
}

void CausalOriginHypotheses::apply_ul_direct_evidence(double log_bayes_factor)
{
  if (!std::isfinite(log_bayes_factor)) return;
  ul_direct_log_bayes_factor_ += log_bayes_factor;
  have_ul_evidence_ = true;
}

void CausalOriginHypotheses::apply_exclusivity_evidence(
    double log_bayes_factor, uint64_t competitor_track_id)
{
  if (!(std::isfinite(log_bayes_factor) && log_bayes_factor > 0.0)
      || competitor_track_id == 0)
    return;
  if (!have_exclusivity_evidence_
      || log_bayes_factor > exclusivity_log_bayes_factor_) {
    exclusivity_log_bayes_factor_ = log_bayes_factor;
    exclusivity_competitor_track_id_ = competitor_track_id;
    have_exclusivity_evidence_ = true;
  }
}

OriginHypothesisEvidence CausalOriginHypotheses::evaluate(
    double direct_vs_clutter_log_evidence) const
{
  OriginHypothesisEvidence out;
  out.observed_epochs = observed_epochs_;
  out.cross_receiver_epochs = cross_receiver_epochs_;
  out.ul_direct_log_bayes_factor = ul_direct_log_bayes_factor_;
  out.exclusivity_log_bayes_factor = exclusivity_log_bayes_factor_;
  out.exclusivity_competitor_track_id = exclusivity_competitor_track_id_;

  std::vector<double> temporal_scores;
  for (const auto& value : temporal_)
    if (value && value->identifiable())
      temporal_scores.push_back(value->structured_log_bayes_factor());
  out.temporal_structure_log_bayes_factor = median(temporal_scores);

  std::vector<double> receiver_scores;
  for (const auto& [key, value] : cross_receiver_) {
    (void)key;
    if (value && value->identifiable())
      receiver_scores.push_back(value->structured_log_bayes_factor());
  }
  out.cross_receiver_structure_log_bayes_factor = median(receiver_scores);

  bool stationary_identifiable = false;
  if (have_velocity_ && observed_epochs_ > 3) {
    try {
      const std::vector<double> velocity{
          velocity_mps_[0], velocity_mps_[1], velocity_mps_[2]};
      const double wald = quadratic(velocity,
                                    pseudoinverse_symmetric(velocity_covariance_, 1e-12));
      if (std::isfinite(wald)) {
        const double n = static_cast<double>(observed_epochs_);
        // Static motion uses three fewer free velocity parameters than unconstrained motion.
        out.stationary_log_bayes_factor = 0.5 * (3.0 * std::log(n) - wald);
        stationary_identifiable = true;
      }
    } catch (...) {}
  }

  std::vector<double> multipath_features;
  if (!temporal_scores.empty())
    multipath_features.push_back(out.temporal_structure_log_bayes_factor);
  if (!receiver_scores.empty())
    multipath_features.push_back(out.cross_receiver_structure_log_bayes_factor);
  if (stationary_identifiable)
    multipath_features.push_back(out.stationary_log_bayes_factor);
  if (have_exclusivity_evidence_)
    multipath_features.push_back(exclusivity_log_bayes_factor_);
  // Staticity, temporal correlation, cross-RX correlation, and duplicate motion share the same
  // DL track and are conservatively aggregated by their median. UL is generated by an independent
  // illuminator, so its log likelihood ratio may then be combined once. Missing UL remains zero.
  const double multipath_vs_direct = median(multipath_features)
                                      - ul_direct_log_bayes_factor_;
  out.log_evidence = {direct_vs_clutter_log_evidence,
                      direct_vs_clutter_log_evidence + multipath_vs_direct,
                      0.0};
  out.ready = stationary_identifiable
              && (!temporal_scores.empty() || !receiver_scores.empty());
  const size_t winner = static_cast<size_t>(std::distance(
      out.log_evidence.begin(), std::max_element(out.log_evidence.begin(),
                                                 out.log_evidence.end())));
  out.decision = winner == 0 ? "direct_target"
                 : (winner == 1 ? "static_slow_multipath" : "clutter_or_new");
  return out;
}

} // namespace nr_isac
