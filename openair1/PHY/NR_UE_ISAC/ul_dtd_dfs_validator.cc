/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "ul_dtd_dfs_validator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace nr_isac {
namespace {

constexpr size_t KINEMATIC_SIZE = 6;

struct UlObservation {
  uint32_t receiver = 0;
  Vec3 position;
  const Detection* detection = nullptr;
  Matrix covariance{2, 2};
  double maximum_abs_rate_mps = 0.0;
};

struct UeFit {
  std::array<double, 6> state{};
  Matrix covariance{6, 6};
  bool dfs_identifiable = false;
  double cross_fold_position_penalty = 0.0;
  double heldout_log_bayes_factor = -std::numeric_limits<double>::infinity();
};

struct FoldResult {
  bool valid = false;
  double log_bayes_factor = -std::numeric_limits<double>::infinity();
  UeFit best;
  uint64_t hypotheses = 0;
  uint64_t fits = 0;
  uint64_t valid_training_tuples = 0;
};

uint64_t saturating_add(uint64_t left, uint64_t right)
{
  return right > std::numeric_limits<uint64_t>::max() - left
      ? std::numeric_limits<uint64_t>::max() : left + right;
}

bool finite_state(const std::array<double, 6>& value)
{
  return std::all_of(value.begin(), value.end(), [](double item) {
    return std::isfinite(item);
  });
}

bool positive_covariance(const Matrix& covariance, size_t size)
{
  if (covariance.rows() != size || covariance.cols() != size) return false;
  for (size_t row = 0; row < size; ++row)
    for (size_t column = 0; column < size; ++column)
      if (!std::isfinite(covariance(row, column))) return false;
  try {
    const auto eigen = symmetric_eigen(symmetrized(covariance));
    const double largest = std::max(std::abs(eigen.values.front()),
                                    std::abs(eigen.values.back()));
    return eigen.values.front() > std::numeric_limits<double>::epsilon()
                                      * std::max(1.0, largest) * size;
  } catch (...) {
    return false;
  }
}

bool positive_position_covariance(const Matrix& covariance)
{
  if (covariance.rows() != KINEMATIC_SIZE || covariance.cols() != KINEMATIC_SIZE)
    return false;
  Matrix position(3, 3);
  for (size_t row = 0; row < 3; ++row)
    for (size_t column = 0; column < 3; ++column)
      position(row, column) = covariance(row, column);
  return positive_covariance(position, 3);
}

std::array<double, 6> propagate_state(std::array<double, 6> state, double dt)
{
  for (size_t axis = 0; axis < 3; ++axis) state[axis] += dt * state[axis + 3];
  return state;
}

Matrix propagate_covariance(const Matrix& covariance, double dt)
{
  Matrix transition = Matrix::identity(KINEMATIC_SIZE);
  for (size_t axis = 0; axis < 3; ++axis) transition(axis, axis + 3) = dt;
  return positive_semidefinite(
      transition * covariance * transition.transposed(), 1e-12);
}

Matrix numerical_jacobian(const std::array<double, 6>& target,
                          const std::array<double, 6>& ue,
                          Vec3 receiver,
                          bool with_respect_to_target)
{
  Matrix result(2, KINEMATIC_SIZE);
  const double scale = std::sqrt(std::numeric_limits<double>::epsilon());
  for (size_t column = 0; column < KINEMATIC_SIZE; ++column) {
    auto plus_target = target, minus_target = target;
    auto plus_ue = ue, minus_ue = ue;
    const double value = with_respect_to_target ? target[column] : ue[column];
    const double step = scale * std::max(1.0, std::abs(value));
    if (with_respect_to_target) {
      plus_target[column] += step; minus_target[column] -= step;
    } else {
      plus_ue[column] += step; minus_ue[column] -= step;
    }
    const auto above = ul_dtd_dfs_model(plus_target, plus_ue, receiver);
    const auto below = ul_dtd_dfs_model(minus_target, minus_ue, receiver);
    for (size_t row = 0; row < 2; ++row)
      result(row, column) = (above[row] - below[row]) / (2.0 * step);
  }
  return result;
}

double dtd_fit_cost(const std::array<double, 6>& target,
                    const std::array<UlObservation, 3>& observations,
                    Vec3 ue_position)
{
  std::array<double, 6> stationary_ue{
      ue_position.x, ue_position.y, ue_position.z, 0.0, 0.0, 0.0};
  double cost = 0.0;
  for (const auto& observation : observations) {
    const double variance = observation.covariance(0, 0);
    if (!(variance > 0.0)) return std::numeric_limits<double>::infinity();
    try {
      const double residual = observation.detection->range_m
                              - ul_dtd_dfs_model(
                                  target, stationary_ue, observation.position)[0];
      cost += residual * residual / variance;
    } catch (...) {
      return std::numeric_limits<double>::infinity();
    }
  }
  return cost;
}

std::optional<Vec3> refine_ue_position(
    const std::array<double, 6>& target,
    const std::array<UlObservation, 3>& observations,
    Vec3 initial)
{
  Vec3 state = initial;
  double cost = dtd_fit_cost(target, observations, state);
  if (!std::isfinite(cost)) return std::nullopt;
  const double epsilon = std::numeric_limits<double>::epsilon();
  // Iteration limits and convergence are derived from state dimension and machine precision;
  // they are numerical safeguards, not scene- or target-dependent gates.
  for (size_t iteration = 0; iteration < 8 * 3; ++iteration) {
    Matrix jacobian(3, 3), weight(3, 3);
    std::vector<double> residual(3);
    const std::array<double, 6> ue{
        state.x, state.y, state.z, 0.0, 0.0, 0.0};
    bool valid = true;
    for (size_t row = 0; row < observations.size(); ++row) {
      try {
        const auto predicted = ul_dtd_dfs_model(
            target, ue, observations[row].position);
        residual[row] = observations[row].detection->range_m - predicted[0];
        const Matrix local = numerical_jacobian(
            target, ue, observations[row].position, false);
        for (size_t column = 0; column < 3; ++column)
          jacobian(row, column) = local(0, column);
        weight(row, row) = 1.0 / observations[row].covariance(0, 0);
      } catch (...) {
        valid = false;
        break;
      }
    }
    if (!valid) return std::nullopt;
    const Matrix information = jacobian.transposed() * weight * jacobian;
    const auto gradient = jacobian.transposed() * weight * residual;
    double information_scale = 0.0;
    for (size_t axis = 0; axis < 3; ++axis)
      information_scale = std::max(information_scale,
                                   std::abs(information(axis, axis)));
    information_scale = std::max(information_scale, 1.0);
    double damping = epsilon * information_scale;
    bool accepted = false;
    Vec3 accepted_step;
    for (size_t attempt = 0; attempt < 8 * 3; ++attempt) {
      Matrix regularized = information;
      for (size_t axis = 0; axis < 3; ++axis)
        regularized(axis, axis) += damping;
      try {
        const auto step = inverse(regularized) * gradient;
        const Vec3 delta{step[0], step[1], step[2]};
        const Vec3 candidate = state + delta;
        const double candidate_cost = dtd_fit_cost(target, observations, candidate);
        if (std::isfinite(candidate_cost) && candidate_cost < cost) {
          state = candidate;
          cost = candidate_cost;
          accepted_step = delta;
          accepted = true;
          break;
        }
      } catch (...) {}
      damping *= 2.0;
    }
    if (!accepted) break;
    if (norm(accepted_step) <= std::sqrt(epsilon) * std::max(1.0, norm(state)))
      break;
  }
  return state;
}

std::vector<std::array<double, 6>> algebraic_ue_states(
    const std::array<double, 6>& target,
    const std::array<UlObservation, 3>& observations)
{
  const Vec3 x{target[0], target[1], target[2]};
  const Vec3 vx{target[3], target[4], target[5]};
  Matrix position_matrix(3, 3);
  std::vector<double> response(3), rho_coefficient(3);
  for (size_t row = 0; row < observations.size(); ++row) {
    const Vec3 receiver = observations[row].position;
    const double d = observations[row].detection->range_m;
    const double x_to_receiver = norm(x - receiver);
    const double q = d - x_to_receiver;
    const Vec3 difference = x - receiver;
    position_matrix(row, 0) = 2.0 * difference.x;
    position_matrix(row, 1) = 2.0 * difference.y;
    position_matrix(row, 2) = 2.0 * difference.z;
    response[row] = dot(x, x) - dot(receiver, receiver) + q * q;
    rho_coefficient[row] = -2.0 * q;
  }
  Matrix position_inverse;
  try { position_inverse = inverse(position_matrix); }
  catch (...) { return {}; }
  const auto intercept_values = position_inverse * response;
  const auto slope_values = position_inverse * rho_coefficient;
  const Vec3 intercept{intercept_values[0], intercept_values[1], intercept_values[2]};
  const Vec3 slope{slope_values[0], slope_values[1], slope_values[2]};
  const Vec3 offset = intercept - x;
  const double a = dot(slope, slope) - 1.0;
  const double b = 2.0 * dot(offset, slope);
  const double c = dot(offset, offset);
  const double numeric_scale = std::max({1.0, std::abs(a), std::abs(b), std::abs(c)});
  std::vector<double> ranges;
  if (std::abs(a) <= std::numeric_limits<double>::epsilon() * numeric_scale) {
    if (std::abs(b) > std::numeric_limits<double>::epsilon() * numeric_scale)
      ranges.push_back(-c / b);
  } else {
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant >= -std::numeric_limits<double>::epsilon() * numeric_scale) {
      const double root = std::sqrt(std::max(0.0, discriminant));
      ranges.push_back((-b - root) / (2.0 * a));
      if (root > std::numeric_limits<double>::epsilon() * numeric_scale)
        ranges.push_back((-b + root) / (2.0 * a));
    } else if (a > 0.0) {
      // No exact intersection is normal under measurement noise.  The quadratic vertex is the
      // closest algebraic point and seeds covariance-weighted nonlinear refinement below.
      ranges.push_back(-b / (2.0 * a));
    }
  }

  std::vector<std::array<double, 6>> states;
  for (double rho : ranges) {
    if (!std::isfinite(rho)) continue;
    const auto refined_position = refine_ue_position(
        target, observations, intercept + rho * slope);
    if (!refined_position) continue;
    const Vec3 t = *refined_position;
    Matrix velocity_matrix(3, 3);
    std::vector<double> velocity_response(3);
    bool physical = true;
    for (size_t row = 0; row < observations.size(); ++row) {
      const Vec3 receiver = observations[row].position;
      const double t_to_receiver = norm(t - receiver);
      if (!(t_to_receiver > 0.0)) { physical = false; break; }
      Vec3 u_xt, u_xi, u_ti;
      try {
        u_xt = normalized(x - t);
        u_xi = normalized(x - receiver);
        u_ti = normalized(t - receiver);
      } catch (...) { physical = false; break; }
      const Vec3 coefficient = -1.0 * (u_xt + u_ti);
      velocity_matrix(row, 0) = coefficient.x;
      velocity_matrix(row, 1) = coefficient.y;
      velocity_matrix(row, 2) = coefficient.z;
      velocity_response[row] = observations[row].detection->range_rate_mps
                               - dot(u_xt + u_xi, vx);
    }
    if (!physical) continue;
    std::vector<double> velocity;
    try {
      Matrix rate_weight(3, 3);
      for (size_t row = 0; row < observations.size(); ++row)
        rate_weight(row, row) = 1.0 / observations[row].covariance(1, 1);
      const Matrix velocity_information = velocity_matrix.transposed()
                                          * rate_weight * velocity_matrix;
      velocity = pseudoinverse_symmetric(velocity_information)
                 * velocity_matrix.transposed() * rate_weight * velocity_response;
    }
    catch (...) { continue; }
    std::array<double, 6> state{t.x, t.y, t.z, velocity[0], velocity[1], velocity[2]};
    if (finite_state(state)) states.push_back(state);
  }
  return states;
}

std::optional<UeFit> characterize_fit(
    const std::array<double, 6>& target,
    const std::array<UlObservation, 3>& observations,
    const std::array<double, 6>& ue)
{
  Matrix covariance(6, 6), jacobian(6, 6);
  Matrix range_covariance(3, 3), range_jacobian(3, 3);
  std::vector<double> residual(6);
  for (size_t item = 0; item < observations.size(); ++item) {
    const auto predicted = ul_dtd_dfs_model(target, ue, observations[item].position);
    residual[2 * item] = observations[item].detection->range_m - predicted[0];
    residual[2 * item + 1] = observations[item].detection->range_rate_mps - predicted[1];
    const Matrix local = numerical_jacobian(
        target, ue, observations[item].position, false);
    for (size_t row = 0; row < 2; ++row) {
      for (size_t column = 0; column < 6; ++column)
        jacobian(2 * item + row, column) = local(row, column);
      for (size_t column = 0; column < 2; ++column)
        covariance(2 * item + row, 2 * item + column)
            = observations[item].covariance(row, column);
    }
    range_covariance(item, item) = observations[item].covariance(0, 0);
    for (size_t column = 0; column < 3; ++column)
      range_jacobian(item, column) = local(0, column);
  }
  try {
    UeFit fit;
    fit.state = ue;
    // DTD alone gives three equations for the three UE-position coordinates.  Do not discard
    // that identifiable second-illuminator geometry merely because the DFS block cannot resolve
    // all three UE-velocity coordinates.  The rate dimension is added only when the complete
    // six-state Fisher matrix is numerically positive definite.
    const Matrix range_weight = pseudoinverse_symmetric(range_covariance);
    const Matrix position_information = range_jacobian.transposed()
                                        * range_weight * range_jacobian;
    if (!positive_covariance(position_information, 3)) return std::nullopt;
    const Matrix position_covariance = pseudoinverse_symmetric(position_information);
    for (size_t row = 0; row < 3; ++row)
      for (size_t column = 0; column < 3; ++column)
        fit.covariance(row, column) = position_covariance(row, column);

    const Matrix weight = pseudoinverse_symmetric(covariance);
    const Matrix information = jacobian.transposed() * weight * jacobian;
    const double cost = quadratic(residual, weight);
    // Three DTD/DFS pairs have no residual degrees of freedom. This rejects only algebraic or
    // numerical failures; held-out likelihood supplies the statistical evidence.
    if (!std::isfinite(cost)) return std::nullopt;
    const bool dfs_more_informative_than_uniform = std::all_of(
        observations.begin(), observations.end(), [](const UlObservation& observation) {
          const double uniform_width = 2.0 * observation.maximum_abs_rate_mps;
          return uniform_width > 0.0
              && observation.covariance(1, 1)
                    < uniform_width * uniform_width / (2.0 * PI * std::exp(1.0));
        });
    if (dfs_more_informative_than_uniform) {
      if (!positive_covariance(information, 6)) return std::nullopt;
      fit.covariance = pseudoinverse_symmetric(information);
      fit.dfs_identifiable = true;
    } else {
      // A DTD-only state has no asserted UE velocity. Leaving that covariance block zero is
      // intentional; consumers test dfs_identifiable before using or propagating DFS.
      for (size_t axis = 3; axis < 6; ++axis) fit.state[axis] = 0.0;
    }
    return fit;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> heldout_joint_log_bayes_factor(
    const std::array<double, 6>& target,
    const Matrix& target_covariance,
    const UeFit& fit,
    const UlDifferentialReceiverBatch& heldout,
    const Detection& detection)
{
  const auto predicted = ul_dtd_dfs_model(target, fit.state, heldout.receiver_position);
  const Matrix target_jacobian = numerical_jacobian(
      target, fit.state, heldout.receiver_position, true);
  const Matrix ue_jacobian = numerical_jacobian(
      target, fit.state, heldout.receiver_position, false);
  const double volume = heldout.maximum_differential_range_m
                        * 2.0 * heldout.maximum_abs_differential_rate_mps;
  if (!(volume > 0.0) || heldout.detections.empty()
      || !detection.covariance_valid
      || !positive_covariance(detection.range_rate_covariance, 2))
    return std::nullopt;
  double target_log_density = 0.0;
  try {
    Matrix innovation = symmetrized(detection.range_rate_covariance)
        + target_jacobian * target_covariance * target_jacobian.transposed()
        + ue_jacobian * fit.covariance * ue_jacobian.transposed();
    innovation = positive_semidefinite(innovation, 1e-12);
    const double determinant = innovation(0, 0) * innovation(1, 1)
                               - innovation(0, 1) * innovation(1, 0);
    if (!(determinant > 0.0)) return std::nullopt;
    const std::vector<double> residual{
        detection.range_m - predicted[0], detection.range_rate_mps - predicted[1]};
    const double nis = quadratic(residual, pseudoinverse_symmetric(innovation));
    if (!std::isfinite(nis)) return std::nullopt;
    target_log_density = -std::log(2.0 * PI) - 0.5 * std::log(determinant)
                         - 0.5 * nis;
  } catch (...) {
    return std::nullopt;
  }
  // The null intensity is estimated from the current held-out detection count over the declared
  // search volume.  This is target-blind and automatically charges dense multipath scenes.
  const double log_clutter_density = std::log(
      static_cast<double>(heldout.detections.size()) / volume);
  return target_log_density - log_clutter_density;
}

std::optional<double> heldout_range_log_bayes_factor(
    const std::array<double, 6>& target,
    const Matrix& target_covariance,
    const UeFit& fit,
    const UlDifferentialReceiverBatch& heldout,
    const Detection& detection)
{
  if (!(heldout.maximum_differential_range_m > 0.0)
      || heldout.detections.empty() || !detection.covariance_valid
      || !positive_covariance(detection.range_rate_covariance, 2)
      || !positive_position_covariance(fit.covariance))
    return std::nullopt;
  try {
    const auto predicted = ul_dtd_dfs_model(target, fit.state, heldout.receiver_position);
    const Matrix target_jacobian = numerical_jacobian(
        target, fit.state, heldout.receiver_position, true);
    const Matrix ue_jacobian = numerical_jacobian(
        target, fit.state, heldout.receiver_position, false);
    double variance = detection.range_rate_covariance(0, 0);
    for (size_t row = 0; row < 6; ++row)
      for (size_t column = 0; column < 6; ++column)
        variance += target_jacobian(0, row) * target_covariance(row, column)
                    * target_jacobian(0, column);
    for (size_t row = 0; row < 3; ++row)
      for (size_t column = 0; column < 3; ++column)
        variance += ue_jacobian(0, row) * fit.covariance(row, column)
                    * ue_jacobian(0, column);
    if (!(std::isfinite(variance) && variance > 0.0)) return std::nullopt;
    const double residual = detection.range_m - predicted[0];
    const double target_log_density = -0.5 * std::log(2.0 * PI * variance)
                                      -0.5 * residual * residual / variance;
    const double clutter_log_density = std::log(
        static_cast<double>(heldout.detections.size())
        / heldout.maximum_differential_range_m);
    return target_log_density - clutter_log_density;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> heldout_detection_log_bayes_factor(
    const std::array<double, 6>& target,
    const Matrix& target_covariance,
    const UeFit& fit,
    const UlDifferentialReceiverBatch& heldout,
    const Detection& detection)
{
  const auto range = heldout_range_log_bayes_factor(
      target, target_covariance, fit, heldout, detection);
  if (!range) return std::nullopt;
  if (!fit.dfs_identifiable) return range;
  const auto joint = heldout_joint_log_bayes_factor(
      target, target_covariance, fit, heldout, detection);
  // Once DFS carries more information than its measured uniform support, it is genuine negative
  // as well as positive evidence and must not be silently discarded after seeing its value.
  return joint ? joint : range;
}

double median_value(std::vector<double> values)
{
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 ? values[middle]
                           : 0.5 * (values[middle - 1] + values[middle]);
}

struct AuxiliaryEvidence {
  bool available = false;
  bool valid = true;
  double log_bayes_factor = 0.0;
};

AuxiliaryEvidence direct_path_fdoa_evidence(
    const std::array<double, 6>& ue_state,
    const Matrix& ue_covariance,
    const std::vector<const UlDifferentialReceiverBatch*>& receivers)
{
  struct Value { double residual = 0.0; double variance = 0.0; };
  std::vector<Value> values;
  const Vec3 position{ue_state[0], ue_state[1], ue_state[2]};
  const Vec3 velocity{ue_state[3], ue_state[4], ue_state[5]};
  if (!positive_covariance(ue_covariance, 6)) return {true, false, 0.0};
  for (const auto* receiver : receivers) {
    if (!receiver->direct_path_rate_valid) continue;
    AuxiliaryEvidence invalid{true, false, 0.0};
    if (!(std::isfinite(receiver->direct_path_range_rate_mps)
          && std::isfinite(receiver->direct_path_rate_variance_mps2)
          && receiver->direct_path_rate_variance_mps2 > 0.0))
      return invalid;
    const Vec3 direction = position - receiver->receiver_position;
    const double distance = norm(direction);
    if (!(std::isfinite(distance) && distance > 0.0)) return invalid;
    const Vec3 unit = direction / distance;
    const double predicted = dot(unit, velocity);
    const Vec3 position_gradient = (velocity - predicted * unit) / distance;
    const std::array<double, 6> jacobian{
        position_gradient.x, position_gradient.y, position_gradient.z,
        unit.x, unit.y, unit.z};
    double variance = receiver->direct_path_rate_variance_mps2;
    for (size_t row = 0; row < 6; ++row)
      for (size_t column = 0; column < 6; ++column)
        variance += jacobian[row] * ue_covariance(row, column) * jacobian[column];
    if (!(std::isfinite(variance) && variance > 0.0)) return invalid;
    values.push_back({receiver->direct_path_range_rate_mps - predicted,
                      variance});
  }
  // One common oscillator/CFO nuisance is identifiable only after at least three independently
  // located receivers. Cable delays are constant and never enter this FDOA comparison.
  if (values.size() < 3) return {};
  double information = 0.0, weighted_sum = 0.0;
  for (const Value& value : values) {
    information += 1.0 / value.variance;
    weighted_sum += value.residual / value.variance;
  }
  if (!(std::isfinite(information) && information > 0.0)) return {true, false, 0.0};
  const double common_offset = weighted_sum / information;
  double chi_square = 0.0;
  for (const Value& value : values)
    chi_square += std::pow(value.residual - common_offset, 2.0) / value.variance;
  const double samples = static_cast<double>(values.size());
  const double shared_bic = chi_square + std::log(samples); // one common CFO nuisance
  const double independent_bic = samples * std::log(samples); // one offset per RX
  return {true, std::isfinite(shared_bic), 0.5 * (independent_bic - shared_bic)};
}

AuxiliaryEvidence serving_range_evidence(
    const std::array<double, 6>& ue_state,
    const Matrix& ue_covariance,
    const std::vector<const UlDifferentialReceiverBatch*>& receivers)
{
  const UlDifferentialReceiverBatch* reference = nullptr;
  for (const auto* receiver : receivers) {
    if (!receiver->serving_range_valid) continue;
    if (!(std::isfinite(receiver->serving_range_m) && receiver->serving_range_m >= 0.0
          && std::isfinite(receiver->serving_range_variance_m2)
          && receiver->serving_range_variance_m2 > 0.0))
      return {true, false, 0.0};
    if (!reference) {
      reference = receiver;
      continue;
    }
    const double scale = std::max({1.0, std::abs(reference->serving_range_m),
                                   std::abs(receiver->serving_range_m)});
    if (norm(reference->serving_transmitter_position
             - receiver->serving_transmitter_position)
            > std::sqrt(std::numeric_limits<double>::epsilon()) * scale
        || std::abs(reference->serving_range_m - receiver->serving_range_m)
            > std::sqrt(std::numeric_limits<double>::epsilon()) * scale)
      return {true, false, 0.0};
  }
  if (!reference) return {};
  const Vec3 position{ue_state[0], ue_state[1], ue_state[2]};
  const Vec3 difference = position - reference->serving_transmitter_position;
  const double predicted = norm(difference);
  if (!(std::isfinite(predicted) && predicted > 0.0)
      || !positive_position_covariance(ue_covariance))
    return {true, false, 0.0};
  const Vec3 direction = difference / predicted;
  double variance = reference->serving_range_variance_m2;
  const std::array<double, 3> u{direction.x, direction.y, direction.z};
  for (size_t row = 0; row < 3; ++row)
    for (size_t column = 0; column < 3; ++column)
      variance += u[row] * ue_covariance(row, column) * u[column];
  const double support = reference->maximum_differential_range_m;
  if (!(std::isfinite(variance) && variance > 0.0
        && std::isfinite(support) && support > 0.0))
    return {true, false, 0.0};
  const double residual = reference->serving_range_m - predicted;
  const double log_target = -0.5 * (
      std::log(2.0 * PI * variance) + residual * residual / variance);
  return {true, std::isfinite(log_target), log_target + std::log(support)};
}

// Experimental qualification mask: 1=single peak-multiplicity charge,
// 2=apply existing position-disagreement penalty to full DFS fits too.
// Keep the previous behavior as an explicit control until paired validation passes.
#ifndef NR_ISAC_UL_MATH_FIXES
#define NR_ISAC_UL_MATH_FIXES 0
#endif
double causal_peak_evidence(double best,size_t count)
{
#if NR_ISAC_UL_MATH_FIXES & 1
  (void)count;
  // heldout_* already uses clutter intensity N/V. Maximizing those
  // component likelihoods has already charged this same within-RX selection.
  return best;
#else
  return best-std::log(static_cast<double>(count));
#endif
}

std::optional<UeFit> combine_ue_fits(
    const std::vector<UeFit>& fits)
{
  if (fits.size() < 3) return std::nullopt;
  const size_t dfs_fits = std::count_if(
      fits.begin(), fits.end(), [](const UeFit& fit) { return fit.dfs_identifiable; });
  if (dfs_fits >= 3) {
    try {
      Matrix full_information(6, 6);
      std::vector<double> full_rhs(6, 0.0);
      for (const UeFit& fit : fits) {
        if (!fit.dfs_identifiable || !positive_covariance(fit.covariance, 6)) continue;
        const Matrix weight = pseudoinverse_symmetric(fit.covariance);
        full_information = full_information + weight;
        const std::vector<double> state(fit.state.begin(), fit.state.end());
        const auto weighted = weight * state;
        for (size_t axis = 0; axis < 6; ++axis) full_rhs[axis] += weighted[axis];
      }
      const double inverse_count = 1.0 / static_cast<double>(dfs_fits);
      full_information = inverse_count * full_information;
      for (double& value : full_rhs) value *= inverse_count;
      if (positive_covariance(full_information, 6)) {
        UeFit combined;
        combined.covariance = pseudoinverse_symmetric(full_information);
        const auto state = combined.covariance * full_rhs;
        std::copy(state.begin(), state.end(), combined.state.begin());
        combined.dfs_identifiable = true;
#if NR_ISAC_UL_MATH_FIXES & 2
        // Equal-weight covariance intersection alone does not check whether
        // rotations estimated the same UE. Match the DTD-only branch below.
        std::vector<double> disagreement;
        for(const auto& fit:fits) {
          if(!fit.dfs_identifiable || !positive_covariance(fit.covariance,6)) continue;
          Matrix sum(3,3);
          std::vector<double> delta(3);
          for(size_t r=0;r<3;++r) {
            delta[r]=fit.state[r]-combined.state[r];
            for(size_t c=0;c<3;++c) sum(r,c)=fit.covariance(r,c)+combined.covariance(r,c);
          }
          disagreement.push_back(quadratic(delta,pseudoinverse_symmetric(sum)));
        }
        combined.cross_fold_position_penalty=0.5*median_value(disagreement);
        if(!std::isfinite(combined.cross_fold_position_penalty)) return std::nullopt;
#endif
        return combined;
      }
    } catch (...) {
      // Fall through to the identifiable DTD subspace.
    }
  }
  Matrix information(3, 3);
  std::vector<double> rhs(3, 0.0);
  try {
    for (const UeFit& fit : fits) {
      if (!positive_position_covariance(fit.covariance)) return std::nullopt;
      Matrix position_covariance(3, 3);
      for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
          position_covariance(row, column) = fit.covariance(row, column);
      const Matrix weight = pseudoinverse_symmetric(position_covariance);
      information = information + weight;
      const std::vector<double> state(fit.state.begin(), fit.state.begin() + 3);
      const auto weighted = weight * state;
      for (size_t axis = 0; axis < 3; ++axis) rhs[axis] += weighted[axis];
    }
    // Rotating folds reuse measurements and are correlated. Equal-weight covariance
    // intersection preserves the precision-weighted state while avoiding the false covariance
    // reduction that an independent-product fusion would claim.
    const double inverse_count = 1.0 / static_cast<double>(fits.size());
    information = inverse_count * information;
    for (double& value : rhs) value *= inverse_count;
    if (!positive_covariance(information, 3)) return std::nullopt;
    const Matrix covariance = pseudoinverse_symmetric(information);
    const auto mean_vector = covariance * rhs;
    UeFit combined;
    std::copy(mean_vector.begin(), mean_vector.end(), combined.state.begin());
    for (size_t row = 0; row < 3; ++row)
      for (size_t column = 0; column < 3; ++column)
        combined.covariance(row, column) = covariance(row, column);

    // Receiver rotations share measurements, so do not multiply their agreement likelihoods.
    // The median Gaussian disagreement supplies a continuous, covariance-aware penalty and
    // prevents four fold-specific UE positions from masquerading as one shared illuminator.
    std::vector<double> disagreement;
    disagreement.reserve(fits.size());
    for (const UeFit& fit : fits) {
      Matrix fit_covariance(3, 3);
      for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
          fit_covariance(row, column) = fit.covariance(row, column);
      std::vector<double> delta(3);
      for (size_t axis = 0; axis < 3; ++axis)
        delta[axis] = fit.state[axis] - combined.state[axis];
      disagreement.push_back(quadratic(
          delta, pseudoinverse_symmetric(fit_covariance + covariance)));
    }
    combined.cross_fold_position_penalty = 0.5 * median_value(disagreement);
    if (!std::isfinite(combined.cross_fold_position_penalty)) return std::nullopt;
    return combined;
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace

const char* ul_differential_decision_name(UlDifferentialDecision decision)
{
  switch (decision) {
    case UlDifferentialDecision::unavailable: return "unavailable";
    case UlDifferentialDecision::support: return "support";
    case UlDifferentialDecision::contradiction: return "contradiction";
  }
  return "invalid";
}

std::array<double, 2> ul_dtd_dfs_model(
    const std::array<double, 6>& target,
    const std::array<double, 6>& ue,
    Vec3 receiver)
{
  if (!finite_state(target) || !finite_state(ue)
      || !std::isfinite(receiver.x) || !std::isfinite(receiver.y)
      || !std::isfinite(receiver.z))
    throw std::invalid_argument("DTD/DFS model received nonfinite state");
  const Vec3 x{target[0], target[1], target[2]};
  const Vec3 vx{target[3], target[4], target[5]};
  const Vec3 t{ue[0], ue[1], ue[2]};
  const Vec3 vt{ue[3], ue[4], ue[5]};
  const Vec3 u_xt = normalized(x - t);
  const Vec3 u_xi = normalized(x - receiver);
  const Vec3 u_ti = normalized(t - receiver);
  const double d = norm(x - t) + norm(x - receiver) - norm(t - receiver);
  const double rate = dot(u_xt + u_xi, vx) + dot(-1.0 * (u_xt + u_ti), vt);
  return {d, rate};
}

std::array<double, 2> ul_dtd_dfs_reflected_model(
    const std::array<double, 6>& target,
    const std::array<double, 6>& ue,
    Vec3 receiver,
    const ReflectorPlane& plane)
{
  if (!finite_state(target) || !finite_state(ue))
    throw std::invalid_argument("reflected DTD/DFS model received nonfinite state");
  const Vec3 x{target[0], target[1], target[2]};
  const Vec3 vx{target[3], target[4], target[5]};
  const Vec3 t{ue[0], ue[1], ue[2]};
  const Vec3 vt{ue[3], ue[4], ue[5]};
  const Vec3 direct_ue_to_rx = normalized(t - receiver);
  if (plane.leg == ReflectionLeg::receive) {
    const Vec3 virtual_receiver = mirror_across_plane(receiver, plane);
    const Vec3 u_xt = normalized(x - t);
    const Vec3 u_xr = normalized(x - virtual_receiver);
    return {norm(x - t) + norm(x - virtual_receiver) - norm(t - receiver),
            dot(u_xt + u_xr, vx) + dot(-1.0 * (u_xt + direct_ue_to_rx), vt)};
  }
  const Vec3 virtual_ue = mirror_across_plane(t, plane);
  const Vec3 normal = normalized(plane.normal);
  const Vec3 virtual_velocity = vt - 2.0 * dot(normal, vt) * normal;
  const Vec3 u_xu = normalized(x - virtual_ue);
  const Vec3 u_xr = normalized(x - receiver);
  return {norm(x - virtual_ue) + norm(x - receiver) - norm(t - receiver),
          dot(u_xu + u_xr, vx) - dot(u_xu, virtual_velocity)
              - dot(direct_ue_to_rx, vt)};
}

UlDtdDfsValidation UlDtdDfsValidator::evaluate(
    double time_s,
    const std::array<double, 6>& input_target,
    const Matrix& input_target_covariance,
    const std::vector<UlDifferentialReceiverBatch>& batches,
    double processing_budget_s) const
{
  UlDtdDfsValidation result;
  using Clock = std::chrono::steady_clock;
  const auto processing_started = Clock::now();
  const bool finite_budget = std::isfinite(processing_budget_s);
  if (std::isnan(processing_budget_s)
      || (finite_budget && !(processing_budget_s > 0.0))
      || (!finite_budget && processing_budget_s < 0.0)) {
    result.processing_deadline_exhausted = true;
    result.reason = "ul_validation_processing_deadline";
    return result;
  }
  auto deadline_exhausted = [&] {
    if (!finite_budget) return false;
    if (std::chrono::duration<double>(Clock::now() - processing_started).count()
        < processing_budget_s)
      return false;
    result.processing_deadline_exhausted = true;
    result.reason = "ul_validation_processing_deadline";
    return true;
  };
  if (!std::isfinite(time_s) || !finite_state(input_target)
      || !positive_covariance(input_target_covariance, 6)) {
    result.reason = "invalid_target_hypothesis";
    return result;
  }
  std::array<const UlDifferentialReceiverBatch*, 4> receiver{};
  std::vector<const UlDifferentialReceiverBatch*> usable_receivers;
  uint64_t session_id = 0;
  std::optional<uint64_t> allocation_support_id;
  for (const auto& batch : batches) {
    if (batch.receiver_index >= receiver.size() || receiver[batch.receiver_index]) {
      result.reason = "duplicate_or_out_of_range_receiver";
      return result;
    }
    receiver[batch.receiver_index] = &batch;
    if (batch.observable) ++result.observable_receivers;
    // A CLEAN deadline censors the remaining search, not the peaks already validated.
    if (!batch.observable || !batch.same_pusch_reference
        || batch.detections.empty())
      continue;
    if (batch.session_id == 0) {
      result.reason = "unknown_pusch_session";
      return result;
    }
    if (session_id != 0 && session_id != batch.session_id) {
      result.reason = "mixed_pusch_sessions";
      return result;
    }
    session_id = batch.session_id;
    if (allocation_support_id && *allocation_support_id != batch.allocation_support_id) {
      result.reason = "mixed_pusch_allocations";
      return result;
    }
    allocation_support_id = batch.allocation_support_id;
    const auto* usable = &batch;
    if (!(std::isfinite(usable->receiver_position.x)
             && std::isfinite(usable->receiver_position.y)
             && std::isfinite(usable->receiver_position.z))
        || !(std::isfinite(usable->range_resolution_m)
             && usable->range_resolution_m > 0.0)
        || !(std::isfinite(usable->rate_resolution_mps)
             && usable->rate_resolution_mps > 0.0)
        || !(std::isfinite(usable->maximum_differential_range_m)
             && usable->maximum_differential_range_m > 0.0)
        || !(std::isfinite(usable->maximum_abs_differential_rate_mps)
             && usable->maximum_abs_differential_rate_mps > 0.0)
        || !std::isfinite(usable->maximum_differential_range_m
                          * usable->maximum_abs_differential_rate_mps)) {
      result.reason = "invalid_ul_search_support";
      return result;
    }
    for (const Detection& detection : usable->detections)
      if (!std::isfinite(detection.range_m) || detection.range_m < 0.0
          || detection.range_m > usable->maximum_differential_range_m
          || !std::isfinite(detection.range_rate_mps)
          || std::abs(detection.range_rate_mps)
                 > usable->maximum_abs_differential_rate_mps) {
        result.reason = "ul_detection_outside_declared_search_support";
        return result;
      }
    usable_receivers.push_back(usable);
  }
  if (usable_receivers.empty()) {
    result.reason = "same_pusch_measurements_unavailable";
    return result;
  }
  const double offset = usable_receivers.front()->target_time_offset_s;
  const double time_tolerance = std::sqrt(std::numeric_limits<double>::epsilon())
                                * std::max(1.0, std::abs(offset));
  for (const auto* batch : usable_receivers)
    if (!std::isfinite(batch->target_time_offset_s)
        || std::abs(batch->target_time_offset_s - offset) > time_tolerance) {
      result.reason = "receiver_pusch_times_not_common";
      return result;
    }
  const auto target = propagate_state(input_target, offset);
  const Matrix target_covariance = propagate_covariance(input_target_covariance, offset);
  result.ue_state_time_s = time_s + offset;
  if (!std::isfinite(result.ue_state_time_s)
      || (ue_state_ && result.ue_state_time_s <= ue_state_->time_s)) {
    result.reason = "ul_not_a_later_acquisition";
    return result;
  }

  // Once a UE state has been learned causally for this session, UL becomes a cheap second-
  // illuminator measurement rather than another target-specific nuisance fit.  Each receiver is
  // scored against the same propagated UE.  Taking the median and requiring support from at least
  // three receivers avoids multiplying correlated receiver scores; subtracting log(N) accounts
  // for selecting the best of N reflected peaks in a receiver using the measured multiplicity.
  if (ue_state_) {
    UeFit causal;
    try {
      causal.state = propagate_state(
          ue_state_->state, result.ue_state_time_s - ue_state_->time_s);
      causal.covariance = propagate_covariance(
          ue_state_->covariance, result.ue_state_time_s - ue_state_->time_s);
      causal.dfs_identifiable = ue_state_->dfs_identifiable;
    } catch (...) {
      result.reason = "causal_ue_propagation_failed";
      return result;
    }
    result.ue_state_valid = true;
    result.ue_velocity_identifiable = causal.dfs_identifiable;
    result.direct_session_anchor_valid = ue_state_->direct_session_anchor_valid;
    result.session_establishment_eligible = ue_state_->direct_session_anchor_valid;
    result.cross_fold_ue_consistent = true;
    result.ue_state = causal.state;
    result.ue_covariance = causal.covariance;
    std::vector<double> receiver_evidence;
    for (const auto* batch : usable_receivers) {
      if (deadline_exhausted()) return result;
      std::optional<double> best;
      for (const Detection& detection : batch->detections) {
        const auto evidence = heldout_detection_log_bayes_factor(
            target, target_covariance, causal, *batch, detection);
        if (evidence && (!best || *evidence > *best)) best = evidence;
      }
      if (!best) continue;
      const double evidence = causal_peak_evidence(*best,batch->detections.size());
      // Without search coverage, failure to find a compatible peak is not absence evidence.
      // Exclude that RX rather than letting a neutral placeholder increase the fold count.
      if (!batch->search_complete && evidence <= 0.0) continue;
      receiver_evidence.push_back(evidence);
    }
    result.evaluated_folds = receiver_evidence.size();
    result.supporting_folds = std::count_if(
        receiver_evidence.begin(), receiver_evidence.end(),
        [](double evidence) { return evidence > 0.0; });
    if (receiver_evidence.size() < 3) {
      result.reason = "fewer_than_three_causal_ul_receivers";
      return result;
    }
    result.log_bayes_factor = median_value(receiver_evidence);
    if (result.supporting_folds >= 3 && result.log_bayes_factor > 0.0) {
      if (ue_state_->established) {
        result.decision = UlDifferentialDecision::support;
        result.reason = "shared_causal_ue_support";
      } else {
        // A provisional state needs an independent later CPI before it may affect any target.
        result.reason = "causal_ue_bootstrap_confirmed";
      }
    } else if (ue_state_->established && result.log_bayes_factor < 0.0) {
      result.decision = UlDifferentialDecision::contradiction;
      result.reason = "shared_causal_ue_contradiction";
    } else {
      result.reason = ue_state_->established
          ? "shared_causal_ue_ambiguous"
          : (result.log_bayes_factor < 0.0
                 ? "provisional_ue_inconsistent" : "provisional_ue_ambiguous");
    }
    return result;
  }

  // A first nuisance estimate needs all four receivers for rotating 3+1 validation. Missing UL is
  // neutral; it is never converted into target-negative evidence.
  for (const auto* batch : receiver) {
    if (!batch || std::find(usable_receivers.begin(), usable_receivers.end(), batch)
                      == usable_receivers.end()) {
      result.reason = "four_receiver_same_pusch_holdout_unavailable";
      return result;
    }
  }

  using Association = std::array<size_t, 4>;
  using AssociationFolds = std::array<std::optional<UeFit>, 4>;
  std::map<Association, AssociationFolds> coherent_associations;
  std::vector<FoldResult> folds;
  folds.reserve(4);
  for (size_t heldout = 0; heldout < receiver.size(); ++heldout) {
    std::array<size_t, 3> training{};
    size_t cursor = 0;
    for (size_t index = 0; index < receiver.size(); ++index)
      if (index != heldout) training[cursor++] = index;
    FoldResult fold;
    for (size_t ai = 0; ai < receiver[training[0]]->detections.size(); ++ai)
      for (size_t bi = 0; bi < receiver[training[1]]->detections.size(); ++bi)
        for (size_t ci = 0; ci < receiver[training[2]]->detections.size(); ++ci) {
          if (deadline_exhausted()) return result;
          fold.hypotheses = saturating_add(fold.hypotheses, 1);
          const std::array<size_t, 3> detection_index{ai, bi, ci};
          std::array<UlObservation, 3> observations;
          bool valid = true;
          for (size_t axis = 0; axis < 3; ++axis) {
            const auto* batch = receiver[training[axis]];
            const Detection& detection = batch->detections[detection_index[axis]];
            if (!detection.covariance_valid
                || !positive_covariance(detection.range_rate_covariance, 2)
                || !std::isfinite(detection.range_m) || detection.range_m < 0.0
                || !std::isfinite(detection.range_rate_mps)) {
              valid = false; break;
            }
            observations[axis] = {
                batch->receiver_index, batch->receiver_position, &detection,
                symmetrized(detection.range_rate_covariance),
                batch->maximum_abs_differential_rate_mps};
          }
          if (!valid) continue;
          ++fold.valid_training_tuples;
          const auto states = algebraic_ue_states(target, observations);
          for (const auto& state : states) {
            auto fit = characterize_fit(target, observations, state);
            if (!fit) continue;
            ++fold.fits;
            auto preference = [&](const UeFit& candidate) {
              double prior_nis = 0.0;
              if (ue_state_) {
                try {
                  const auto prior_state = propagate_state(
                      ue_state_->state, time_s + offset - ue_state_->time_s);
                  const Matrix prior_covariance = propagate_covariance(
                      ue_state_->covariance, time_s + offset - ue_state_->time_s);
                  std::vector<double> delta(6);
                  for (size_t axis = 0; axis < 6; ++axis)
                    delta[axis] = candidate.state[axis] - prior_state[axis];
                  prior_nis = quadratic(delta, pseudoinverse_symmetric(
                      prior_covariance + candidate.covariance));
                } catch (...) { prior_nis = std::numeric_limits<double>::infinity(); }
              }
              return std::tuple(candidate.heldout_log_bayes_factor, -prior_nis);
            };
            for (size_t heldout_detection = 0;
                 heldout_detection < receiver[heldout]->detections.size();
                 ++heldout_detection) {
              if (deadline_exhausted()) return result;
              auto evidence = heldout_detection_log_bayes_factor(
                  target, target_covariance, *fit, *receiver[heldout],
                  receiver[heldout]->detections[heldout_detection]);
              if (!evidence) continue;
              UeFit candidate = *fit;
              candidate.heldout_log_bayes_factor = receiver[heldout]->search_complete
                  ? *evidence : std::max(0.0, *evidence);
              Association association{};
              for (size_t axis = 0; axis < training.size(); ++axis)
                association[training[axis]] = detection_index[axis];
              association[heldout] = heldout_detection;
              auto& coherent_fold = coherent_associations[association][heldout];
              if (!coherent_fold || preference(candidate) > preference(*coherent_fold))
                coherent_fold = candidate;
              if (!fold.valid || preference(candidate) > preference(fold.best)) {
                fold.best = candidate;
                fold.valid = true;
              }
            }
          }
        }
    result.training_hypotheses = saturating_add(
        result.training_hypotheses, fold.hypotheses);
    result.fitted_ue_states = saturating_add(result.fitted_ue_states, fold.fits);
    if (fold.valid) {
      // Training associations are nuisance hypotheses rather than independent evidence. Score
      // the best held-out prediction here; the cross-fold shared-UE model comparison below is
      // what prevents unrelated receiver-local multipath associations from being accepted.
      fold.log_bayes_factor = fold.best.heldout_log_bayes_factor;
      ++result.evaluated_folds;
      if (fold.log_bayes_factor > 0.0) ++result.supporting_folds;
    } else if (fold.valid_training_tuples > 0
               && std::all_of(receiver.begin(), receiver.end(),
                              [](const auto* batch) { return batch->search_complete; })) {
      // With a full four-receiver opportunity and valid measurement covariances, absence of any
      // identifiable physical UE fit for a training triplet is target-specific negative DTD/DFS
      // evidence. Missing or malformed measurements returned earlier as unavailable.
      fold.valid = true;
      fold.log_bayes_factor = -std::numeric_limits<double>::infinity();
      ++result.evaluated_folds;
    }
    folds.push_back(std::move(fold));
  }

  if (result.evaluated_folds < 3) {
    result.reason = "fewer_than_three_evaluable_holdout_folds";
    return result;
  }
  struct ValidatedAssociation {
    uint32_t evaluated_folds = 0;
    uint32_t supporting_folds = 0;
    double median_log_bayes_factor = -std::numeric_limits<double>::infinity();
    std::array<double, 6> ue_state{};
    Matrix ue_covariance{6, 6};
    bool ue_velocity_identifiable = false;
    bool direct_session_anchor_valid = false;
    bool auxiliary_valid = true;
    bool valid = false;
  };
  ValidatedAssociation best_association;
  uint64_t evaluable_associations = 0;
  bool have_uncorrected_coherent_support = false;
  for (const auto& [association, association_folds] : coherent_associations) {
    if (deadline_exhausted()) return result;
    (void)association;
    std::vector<double> evidence;
    std::vector<UeFit> supporting_fits;
    for (const auto& fit : association_folds) if (fit) {
      evidence.push_back(fit->heldout_log_bayes_factor);
      if (fit->heldout_log_bayes_factor > 0.0) supporting_fits.push_back(*fit);
    }
    if (evidence.size() < 3) continue;
    ++evaluable_associations;
    const auto combined = combine_ue_fits(supporting_fits);
    if (!combined) continue;
    ValidatedAssociation candidate;
    candidate.evaluated_folds = evidence.size();
    candidate.supporting_folds = supporting_fits.size();
    candidate.median_log_bayes_factor = median_value(evidence)
                                         - combined->cross_fold_position_penalty;
    candidate.ue_state = combined->state;
    candidate.ue_covariance = combined->covariance;
    candidate.ue_velocity_identifiable = combined->dfs_identifiable;
    // A single four-receiver association that survives at least three held-out rotations already
    // represents one shared nuisance state; the causal estimate above is fused conservatively.
    const bool dtd_dfs_valid = candidate.supporting_folds >= 3
                               && candidate.median_log_bayes_factor > 0.0;
    have_uncorrected_coherent_support = have_uncorrected_coherent_support || dtd_dfs_valid;
    candidate.valid = dtd_dfs_valid;
    std::vector<double> measurement_family_evidence{
        candidate.median_log_bayes_factor};
    if (candidate.valid && candidate.ue_velocity_identifiable) {
      const AuxiliaryEvidence fdoa = direct_path_fdoa_evidence(
          candidate.ue_state, candidate.ue_covariance, usable_receivers);
      if (fdoa.available) {
        candidate.auxiliary_valid = fdoa.valid;
        if (fdoa.valid)
          measurement_family_evidence.push_back(fdoa.log_bayes_factor);
      }
    }
    if (candidate.valid && candidate.auxiliary_valid) {
      const AuxiliaryEvidence timing_advance = serving_range_evidence(
          candidate.ue_state, candidate.ue_covariance, usable_receivers);
      if (timing_advance.available) {
        candidate.auxiliary_valid = candidate.auxiliary_valid && timing_advance.valid;
        if (timing_advance.valid) {
          measurement_family_evidence.push_back(
              timing_advance.log_bayes_factor);
          candidate.direct_session_anchor_valid =
              timing_advance.log_bayes_factor > 0.0;
        }
      }
    }
    // DTD/DFS, direct FDOA and TA are derived from the same PUSCH and are therefore correlated.
    // Aggregate their calibrated log-Bayes scores by the median (the mean for two families),
    // rather than multiplying them or allowing one noisy auxiliary to act as a hard gate. A
    // malformed source that asserted validity is still rejected; an absent source adds nothing.
    if (candidate.auxiliary_valid)
      candidate.median_log_bayes_factor = median_value(
          measurement_family_evidence);
    candidate.valid = candidate.valid && candidate.auxiliary_valid
                      && candidate.median_log_bayes_factor > 0.0;
    const auto preference = [](const ValidatedAssociation& item) {
      return std::tuple(item.valid, item.supporting_folds,
                        item.median_log_bayes_factor);
    };
    if (preference(candidate) > preference(best_association))
      best_association = std::move(candidate);
  }
  if (evaluable_associations > 0) {
    // Correct for selecting the best coherent receiver association. The penalty is determined by
    // the current measured hypothesis count, not by a fixed clutter or scenario parameter.
    best_association.median_log_bayes_factor -= std::log(
        static_cast<double>(evaluable_associations));
    best_association.valid = best_association.auxiliary_valid
                             && best_association.supporting_folds >= 3
                             && best_association.median_log_bayes_factor > 0.0;
  }
  if (best_association.valid) {
    result.evaluated_folds = best_association.evaluated_folds;
    result.supporting_folds = best_association.supporting_folds;
    result.log_bayes_factor = best_association.median_log_bayes_factor;
    result.cross_fold_ue_consistent = true;
    result.ue_state_valid = true;
    result.ue_velocity_identifiable = best_association.ue_velocity_identifiable;
    result.direct_session_anchor_valid =
        best_association.direct_session_anchor_valid;
    result.session_establishment_eligible = result.direct_session_anchor_valid;
    result.ue_state = best_association.ue_state;
    result.ue_covariance = best_association.ue_covariance;
    if (!ue_state_) {
      // The first target candidate must not create both its own convenient UE and positive birth
      // evidence in the same CPI. Preserve the cross-validated estimate only as the session's
      // causal bootstrap; a later independent CPI must predict consistently from that one shared
      // nuisance state before UL can support a target birth.
      result.decision = UlDifferentialDecision::unavailable;
      result.reason = "causal_ue_bootstrap_only";
    } else {
      try {
        const auto prior_state = propagate_state(
            ue_state_->state, time_s + offset - ue_state_->time_s);
        const Matrix prior_covariance = propagate_covariance(
            ue_state_->covariance, time_s + offset - ue_state_->time_s);
        std::vector<double> delta(KINEMATIC_SIZE);
        for (size_t axis = 0; axis < KINEMATIC_SIZE; ++axis)
          delta[axis] = result.ue_state[axis] - prior_state[axis];
        const double causal_nis = quadratic(
            delta, pseudoinverse_symmetric(prior_covariance + result.ue_covariance));
        if (!std::isfinite(causal_nis)) throw std::runtime_error("nonfinite causal UE NIS");
        // Penalize disagreement continuously instead of applying a tuned residual gate. The
        // zero log-Bayes decision boundary remains the same target-vs-measured-clutter test.
        result.log_bayes_factor -= 0.5 * causal_nis;
        if (result.log_bayes_factor > 0.0) {
          result.decision = UlDifferentialDecision::support;
          result.reason = "rotating_three_plus_one_causal_support";
        } else {
          result.decision = UlDifferentialDecision::unavailable;
          result.reason = "cross_validated_ue_inconsistent_with_causal_session";
        }
      } catch (...) {
        result.decision = UlDifferentialDecision::unavailable;
        result.reason = "causal_ue_consistency_not_identifiable";
      }
    }
  } else if (!have_uncorrected_coherent_support
             && std::all_of(receiver.begin(), receiver.end(),
                            [](const auto* batch) { return batch->search_complete; })
             && result.evaluated_folds == receiver.size()
             && result.supporting_folds < 3
             && (evaluable_associations == 0 || !best_association.valid)) {
    // Reject only with complete, strongly negative held-out evidence.  Weak geometry, competing
    // multipath associations, or a noisy non-common nuisance estimate is inconclusive and must
    // not turn an otherwise valid DL birth into a false negative.
    result.decision = UlDifferentialDecision::contradiction;
    result.reason = "rotating_three_plus_one_inconsistent";
  } else {
    result.reason = "rotating_three_plus_one_ambiguous";
  }
  return result;
}

UlDtdDfsValidation UlDtdDfsValidator::evaluate_reflected(
    double time_s,
    const std::array<double, 6>& input_target,
    const Matrix& input_target_covariance,
    const std::vector<UlDifferentialReceiverBatch>& batches,
    const ReflectorPlane& plane,
    double processing_budget_s) const
{
  UlDtdDfsValidation result;
  result.reason = "reflected_ul_unavailable";
  // A target candidate is never allowed to fit a private UE under a convenient wall. The direct
  // rotating 3+1 estimator must already have established this protocol session causally.
  if (!has_causal_ue_state()) {
    result.reason = "reflected_ul_requires_shared_causal_ue";
    return result;
  }
  const UlDtdDfsValidation context = evaluate(
      time_s, input_target, input_target_covariance, batches, processing_budget_s);
  if (context.processing_deadline_exhausted) return context;
  if (!context.ue_state_valid) {
    result.observable_receivers = context.observable_receivers;
    result.reason = context.reason;
    return result;
  }
  result.ue_state_valid = true;
  result.ue_velocity_identifiable = context.ue_velocity_identifiable;
  result.cross_fold_ue_consistent = true;
  result.ue_state = context.ue_state;
  result.ue_covariance = context.ue_covariance;
  result.ue_state_time_s = context.ue_state_time_s;
  result.observable_receivers = context.observable_receivers;

  std::vector<const UlDifferentialReceiverBatch*> usable;
  for (const auto& batch : batches)
    if (batch.observable && batch.same_pusch_reference
        && batch.session_id != 0 && !batch.detections.empty())
      usable.push_back(&batch);
  if (usable.size() < 3) {
    result.reason = "fewer_than_three_reflected_ul_receivers";
    return result; // missing UL is neutral
  }
  const double offset = usable.front()->target_time_offset_s;
  const auto target = propagate_state(input_target, offset);
  const Matrix target_covariance = propagate_covariance(input_target_covariance, offset);

  auto reflected_jacobian = [&](Vec3 receiver, bool target_derivative) {
    Matrix jacobian(2, KINEMATIC_SIZE);
    const double scale = std::sqrt(std::numeric_limits<double>::epsilon());
    for (size_t column = 0; column < KINEMATIC_SIZE; ++column) {
      auto target_plus = target, target_minus = target;
      auto ue_plus = context.ue_state, ue_minus = context.ue_state;
      const double value = target_derivative ? target[column] : context.ue_state[column];
      const double step = scale * std::max(1.0, std::abs(value));
      if (target_derivative) {
        target_plus[column] += step;
        target_minus[column] -= step;
      } else {
        ue_plus[column] += step;
        ue_minus[column] -= step;
      }
      const auto above = ul_dtd_dfs_reflected_model(
          target_plus, ue_plus, receiver, plane);
      const auto below = ul_dtd_dfs_reflected_model(
          target_minus, ue_minus, receiver, plane);
      for (size_t row = 0; row < 2; ++row)
        jacobian(row, column) = (above[row] - below[row]) / (2.0 * step);
    }
    return jacobian;
  };

  std::vector<double> receiver_evidence;
  for (const auto* batch : usable) {
    std::optional<double> best;
    for (const Detection& detection : batch->detections) {
      if (!detection.covariance_valid
          || !positive_covariance(detection.range_rate_covariance, 2))
        continue;
      try {
        const auto predicted = ul_dtd_dfs_reflected_model(
            target, context.ue_state, batch->receiver_position, plane);
        const Matrix target_jacobian = reflected_jacobian(
            batch->receiver_position, true);
        const Matrix ue_jacobian = reflected_jacobian(
            batch->receiver_position, false);
        double evidence = -std::numeric_limits<double>::infinity();
        if (context.ue_velocity_identifiable
            && positive_covariance(context.ue_covariance, 6)) {
          Matrix innovation = symmetrized(detection.range_rate_covariance)
              + target_jacobian * target_covariance * target_jacobian.transposed()
              + ue_jacobian * context.ue_covariance * ue_jacobian.transposed();
          innovation = positive_semidefinite(innovation, 1e-12);
          const double determinant = innovation(0, 0) * innovation(1, 1)
                                     - innovation(0, 1) * innovation(1, 0);
          const double volume = batch->maximum_differential_range_m
                                * 2.0 * batch->maximum_abs_differential_rate_mps;
          if (!(determinant > 0.0 && volume > 0.0)) continue;
          const double nis = quadratic(
              {detection.range_m - predicted[0],
               detection.range_rate_mps - predicted[1]},
              pseudoinverse_symmetric(innovation));
          const double log_target = -std::log(2.0 * PI) - 0.5 * std::log(determinant)
                                    - 0.5 * nis;
          const double log_clutter = std::log(
              static_cast<double>(batch->detections.size()) / volume);
          evidence = log_target - log_clutter;
        } else {
          // DTD may identify UE position when DFS cannot identify UE velocity. In that case DFS
          // is unavailable evidence under both direct and reflected models, never an artificial
          // zero-velocity constraint.
          double variance = detection.range_rate_covariance(0, 0);
          for (size_t row = 0; row < KINEMATIC_SIZE; ++row)
            for (size_t column = 0; column < KINEMATIC_SIZE; ++column)
              variance += target_jacobian(0, row) * target_covariance(row, column)
                          * target_jacobian(0, column);
          for (size_t row = 0; row < 3; ++row)
            for (size_t column = 0; column < 3; ++column)
              variance += ue_jacobian(0, row) * context.ue_covariance(row, column)
                          * ue_jacobian(0, column);
          if (!(std::isfinite(variance) && variance > 0.0
                && batch->maximum_differential_range_m > 0.0))
            continue;
          const double residual = detection.range_m - predicted[0];
          const double log_target = -0.5 * std::log(2.0 * PI * variance)
                                    - 0.5 * residual * residual / variance;
          const double log_clutter = std::log(
              static_cast<double>(batch->detections.size())
              / batch->maximum_differential_range_m);
          evidence = log_target - log_clutter;
        }
        if (std::isfinite(evidence) && (!best || evidence > *best)) best = evidence;
      } catch (...) {}
    }
    if (best) {
      const double evidence = causal_peak_evidence(*best,batch->detections.size());
      if (!batch->search_complete && evidence <= 0.0) continue;
      receiver_evidence.push_back(evidence);
    }
  }
  result.evaluated_folds = receiver_evidence.size();
  result.supporting_folds = std::count_if(
      receiver_evidence.begin(), receiver_evidence.end(),
      [](double value) { return value > 0.0; });
  if (receiver_evidence.size() < 3) {
    result.reason = "reflected_ul_evidence_unavailable";
    return result;
  }
  // Receiver likelihoods share the causal UE and target state. Their median is deliberately not
  // multiplied. The plane is already admitted from earlier CPIs, so it adds no candidate-local
  // nuisance fit here.
  result.log_bayes_factor = median_value(receiver_evidence);
  if (result.supporting_folds >= 3 && result.log_bayes_factor > 0.0) {
    result.decision = UlDifferentialDecision::support;
    result.reason = "shared_reflector_ul_support";
  } else if (result.log_bayes_factor < 0.0) {
    result.decision = UlDifferentialDecision::contradiction;
    result.reason = "shared_reflector_ul_contradiction";
  } else {
    result.reason = "shared_reflector_ul_ambiguous";
  }
  return result;
}

UlDtdDfsValidation UlDtdDfsValidator::propose_shared_update(
    double time_s, const std::array<double, 6>& target, const Matrix& covariance,
    const std::vector<UlDifferentialReceiverBatch>& batches) const
{
  UlDtdDfsValidation unavailable;
  unavailable.reason = "unverified_allocation_support";
  if (batches.empty() || !batches.front().allocation_support_id) return unavailable;
  for (const auto& batch : batches)
    if (batch.allocation_support_id != batches.front().allocation_support_id
        || batch.session_id != batches.front().session_id) return unavailable;
  if (ue_state_ && time_s + batches.front().target_time_offset_s <= ue_state_->time_s) {
    unavailable.reason = "shared_ue_update_not_a_later_acquisition";
    return unavailable;
  }
  // Bootstrap needs a new four-RX spatial holdout. Once a shared state exists, a later
  // three-RX time holdout can validate it without solving a new unobservable six-state fit.
  // Its uncertainty is propagated, never reduced by counting these predictions as a refit.
  std::optional<UlDtdDfsValidation> temporal;
  if (ue_state_) {
    auto prior = evaluate(time_s, target, covariance, batches);
    if (!prior.ue_state_valid || !(prior.log_bayes_factor > 0.0)
        || prior.supporting_folds < 3) return prior;
    prior.reason = ue_state_->established ? "shared_ue_temporal_validation_only"
                                         : "causal_ue_bootstrap_confirmed";
    prior.session_establishment_eligible = true;
    temporal = std::move(prior);
  }
  // Prefer a fresh four-RX estimate when it is identifiable and predicts consistently.
  UlDtdDfsValidator fresh;
  auto proposal = fresh.evaluate(time_s, target, covariance, batches);
  if (!proposal.ue_state_valid || !proposal.cross_fold_ue_consistent
      || proposal.supporting_folds < 3 || !(proposal.log_bayes_factor > 0.0))
    return temporal ? *temporal : proposal;
  if (ue_state_) {
    // A re-fit cannot erase disagreement with the shared state learned before this acquisition.
    const auto& prior = *temporal;
    proposal.log_bayes_factor = std::min(prior.log_bayes_factor, proposal.log_bayes_factor);
    proposal.reason = ue_state_->established ? "shared_ue_crossvalidated_update"
                                             : "causal_ue_bootstrap_confirmed";
    proposal.decision = ue_state_->established ? UlDifferentialDecision::support
                                              : UlDifferentialDecision::unavailable;
  }
  proposal.session_establishment_eligible = true;
  return proposal;
}

void UlDtdDfsValidator::commit(double time_s, const UlDtdDfsValidation& validation,
                               double establishment_log_threshold)
{
  if (!validation.ue_state_valid || !validation.cross_fold_ue_consistent
      || !std::isfinite(time_s)
      || !std::isfinite(validation.ue_state_time_s)
      || !finite_state(validation.ue_state)
      || (ue_state_ && validation.ue_state_time_s <= ue_state_->time_s)
      || (validation.ue_velocity_identifiable
          && !positive_covariance(validation.ue_covariance, 6))
      || !(std::isfinite(establishment_log_threshold)
           && establishment_log_threshold >= 0.0)
      || !positive_position_covariance(validation.ue_covariance))
    return;
  if (!ue_state_ && validation.reason != "causal_ue_bootstrap_only") return;
  if (ue_state_ && !ue_state_->established) {
    if (validation.reason == "causal_ue_bootstrap_confirmed") {
      // Promotion requires prediction into and agreement with a later CPI.  The validating target
      // is selected from already-confirmed DL tracks by the tracker, never from birth candidates.
      ue_state_->time_s = validation.ue_state_time_s;
      ue_state_->state = validation.ue_state;
      ue_state_->covariance = validation.ue_covariance;
      ue_state_->dfs_identifiable = validation.ue_velocity_identifiable;
      ue_state_->direct_session_anchor_valid =
          ue_state_->direct_session_anchor_valid
          || validation.direct_session_anchor_valid;
      ue_state_->cumulative_log_evidence += validation.log_bayes_factor;
      ++ue_state_->independent_time_confirmations;
      // Receiver holdout created the provisional state. It must then predict two distinct later
      // PUSCH time blocks before it may affect a target, so no single DL candidate/CPI can invent
      // a convenient UE. The zero Bayes boundary is statistical; the count is causal separation.
      ue_state_->established = ue_state_->independent_time_confirmations >= 2
                               && validation.session_establishment_eligible
                               && ue_state_->cumulative_log_evidence
                                      >= establishment_log_threshold;
    } else if (validation.reason == "provisional_ue_inconsistent") {
      // Do not let one bad algebraic root permanently own a session. A later confirmed track can
      // create a fresh, independently held-out proposal.
      ue_state_.reset();
    }
    return;
  }
  // Once established, only positive evidence may advance the shared session state.
  if (ue_state_ && validation.decision != UlDifferentialDecision::support) return;
  CausalUeState next;
  // The nuisance state is estimated at the common PUSCH midpoint, which can differ from the
  // DL candidate epoch.  Store that actual epoch so the next causal root preference propagates
  // from the correct time rather than silently absorbing UL/DL scheduling skew as UE motion.
  next.time_s = validation.ue_state_time_s;
  next.state = validation.ue_state;
  next.covariance = validation.ue_covariance;
  next.dfs_identifiable = validation.ue_velocity_identifiable;
  next.established = ue_state_.has_value();
  next.direct_session_anchor_valid = validation.direct_session_anchor_valid;
  next.independent_time_confirmations = 0;
  next.cumulative_log_evidence = validation.log_bayes_factor;
  // The state is intentionally replaced by the latest independently cross-validated estimate.
  // A fixed UE process-noise prior would be hidden motion tuning; the previous causal state is
  // used only to resolve algebraic roots during evaluate().
  ue_state_ = std::move(next);
}

void UlDtdDfsValidator::reset() { ue_state_.reset(); }

} // namespace nr_isac
