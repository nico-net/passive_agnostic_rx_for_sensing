/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "multistatic_imm_tracker.h"
#include "explanation_evidence.h"
#include "lifecycle_evidence.h"
#ifdef NR_ISAC_LIFECYCLE_TRACE
#include "ul_probe.h" // qualification-only observer; absent from production builds
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

namespace nr_isac {
namespace {

constexpr size_t MODEL_COUNT = 3;
constexpr size_t STATE_SIZE = 9;

uint64_t saturating_add(uint64_t left, uint64_t right)
{
  return right > std::numeric_limits<uint64_t>::max() - left
      ? std::numeric_limits<uint64_t>::max() : left + right;
}

double sample_median(std::vector<double> values)
{
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 ? values[middle]
                           : 0.5 * (values[middle - 1] + values[middle]);
}

struct Observation {
  uint32_t receiver = 0;
  size_t detection = 0;
  BistaticGeometry geometry;
  Detection value;
  Matrix noise{2, 2};
  bool range_informative = false;
  bool rate_informative = false;
  double time_offset_s = 0.0;
};

Observation reflected_observation(const Observation& input, const ReflectorPlane& plane)
{
  Observation output = input;
  const auto virtualized = virtualize_reflected_detection(
      input.geometry, input.value, plane);
  output.geometry = virtualized.first;
  output.value = virtualized.second;
  return output;
}

struct MeasurementQuality {
  Matrix noise{2, 2};
  bool range_informative = false;
  bool rate_informative = false;
};

MeasurementQuality measurement_quality(const Detection& d,
                                       double range_resolution,
                                       double rate_resolution,
                                       double maximum_range,
                                       double maximum_speed)
{
  if (!(range_resolution > 0.0) || !(rate_resolution > 0.0))
    throw std::invalid_argument("multistatic measurement resolution must be positive");
  MeasurementQuality result;
  Matrix floor(2, 2);
  floor(0, 0) = std::pow(range_resolution / std::sqrt(12.0), 2.0);
  floor(1, 1) = std::pow(rate_resolution / std::sqrt(12.0), 2.0);
  // A missing curvature estimate carries no information beyond the declared surveillance
  // support. Treating it as one resolution cell was an overconfident track-injection path.
  result.noise(0, 0) = maximum_range * maximum_range;
  result.noise(1, 1) = 4.0 * maximum_speed * maximum_speed;
  if (!d.covariance_valid || d.range_rate_covariance.rows() != 2
      || d.range_rate_covariance.cols() != 2)
    return result;
  Matrix reported = symmetrized(d.range_rate_covariance);
  reported(0, 0) = std::max(reported(0, 0), floor(0, 0));
  reported(1, 1) = std::max(reported(1, 1), floor(1, 1));
  const auto eigen = eigenvalues_symmetric_2x2(reported(0, 0), reported(0, 1), reported(1, 1));
  if (!(std::isfinite(eigen[0]) && eigen[0] > 1e-12)) return result;
  const double range_support_variance = maximum_range * maximum_range / 12.0;
  const double rate_support_variance = 4.0 * maximum_speed * maximum_speed / 3.0;
  result.range_informative = reported(0, 0) < range_support_variance;
  result.rate_informative = reported(1, 1) < rate_support_variance;
  // Preserve an honest finite covariance even when it is broader than the operational support;
  // such a measurement is marked uninformative for temporal birth logic but must not be made
  // artificially more precise during established-track updates.
  result.noise = reported;
  return result;
}

std::vector<double> state6(const std::vector<double>& x)
{
  return {x[0], x[1], x[2], x[3], x[4], x[5]};
}

std::vector<double> measurement_model(const std::vector<double>& x,
                                      const BistaticGeometry& geometry)
{
  return enu_measurement_model(state6(x), geometry, false);
}

Matrix measurement_jacobian(const std::vector<double>& x, const BistaticGeometry& geometry)
{
  const Matrix six = enu_measurement_jacobian(state6(x), geometry, false);
  Matrix out(2, STATE_SIZE);
  for (size_t r = 0; r < 2; ++r)
    for (size_t c = 0; c < 6; ++c)
      out(r, c) = six(r, c);
  return out;
}

// Out-of-epoch observations evaluate geometry at acquisition time. The filter's gain,
// Joseph covariance update, model mixing and motion models remain unchanged. A zero offset
// uses the original path exactly. CV interpolation is restricted to the short evidence window.
std::vector<double> acquisition_model(const std::vector<double>& x, const Observation& observation)
{
  if (observation.time_offset_s == 0.0) return measurement_model(x, observation.geometry);
  auto at_acquisition = x;
  for (size_t axis = 0; axis < 3; ++axis)
    at_acquisition[axis] += observation.time_offset_s * x[axis + 3];
  return measurement_model(at_acquisition, observation.geometry);
}

Matrix acquisition_jacobian(const std::vector<double>& x, const Observation& observation)
{
  if (observation.time_offset_s == 0.0) return measurement_jacobian(x, observation.geometry);
  auto at_acquisition = x;
  for (size_t axis = 0; axis < 3; ++axis)
    at_acquisition[axis] += observation.time_offset_s * x[axis + 3];
  Matrix h = measurement_jacobian(at_acquisition, observation.geometry);
  for (size_t row = 0; row < 2; ++row)
    for (size_t axis = 0; axis < 3; ++axis)
      h(row, axis + 3) += observation.time_offset_s * h(row, axis);
  return h;
}

double determinant_2x2(const Matrix& value)
{
  return value(0, 0) * value(1, 1) - value(0, 1) * value(1, 0);
}

std::vector<std::pair<size_t, size_t>> minimum_cost_assignment(const Matrix& costs)
{
  if (!costs.rows() || !costs.cols()) return {};
  const bool transposed = costs.rows() > costs.cols();
  const size_t rows = transposed ? costs.cols() : costs.rows();
  const size_t columns = transposed ? costs.rows() : costs.cols();
  auto value = [&](size_t row, size_t column) {
    return transposed ? costs(column - 1, row - 1) : costs(row - 1, column - 1);
  };
  std::vector<double> row_potential(rows + 1), column_potential(columns + 1);
  std::vector<size_t> column_match(columns + 1), predecessor(columns + 1);
  for (size_t row = 1; row <= rows; ++row) {
    column_match[0] = row;
    size_t current = 0;
    std::vector<double> minimum(columns + 1, std::numeric_limits<double>::infinity());
    std::vector<uint8_t> visited(columns + 1);
    do {
      visited[current] = 1;
      const size_t active_row = column_match[current];
      double delta = std::numeric_limits<double>::infinity();
      size_t next = 0;
      for (size_t column = 1; column <= columns; ++column) if (!visited[column]) {
        const double reduced = value(active_row, column)
            - row_potential[active_row] - column_potential[column];
        if (reduced < minimum[column]) {
          minimum[column] = reduced;
          predecessor[column] = current;
        }
        if (minimum[column] < delta) {
          delta = minimum[column];
          next = column;
        }
      }
      for (size_t column = 0; column <= columns; ++column) {
        if (visited[column]) {
          row_potential[column_match[column]] += delta;
          column_potential[column] -= delta;
        } else {
          minimum[column] -= delta;
        }
      }
      current = next;
    } while (column_match[current] != 0);
    do {
      const size_t previous = predecessor[current];
      column_match[current] = column_match[previous];
      current = previous;
    } while (current);
  }
  std::vector<std::pair<size_t, size_t>> assignment;
  for (size_t column = 1; column <= columns; ++column) if (column_match[column]) {
    const size_t row = column_match[column] - 1;
    const size_t col = column - 1;
    assignment.emplace_back(transposed ? col : row, transposed ? row : col);
  }
  std::sort(assignment.begin(), assignment.end());
  return assignment;
}

Matrix outer(const std::vector<double>& value)
{
  Matrix out(value.size(), value.size());
  for (size_t r = 0; r < value.size(); ++r)
    for (size_t c = 0; c < value.size(); ++c)
      out(r, c) = value[r] * value[c];
  return out;
}

struct MotionStep {
  std::vector<double> state;
  Matrix transition{STATE_SIZE, STATE_SIZE};
  Matrix noise{STATE_SIZE, STATE_SIZE};
};

void add_integrated_jerk(Matrix& q, double dt, double psd)
{
  const double d2 = dt * dt, d3 = d2 * dt, d4 = d3 * dt, d5 = d4 * dt;
  for (size_t axis = 0; axis < 3; ++axis) {
    q(axis, axis) += psd * d5 / 20.0;
    q(axis, axis + 3) += psd * d4 / 8.0;
    q(axis + 3, axis) += psd * d4 / 8.0;
    q(axis, axis + 6) += psd * d3 / 6.0;
    q(axis + 6, axis) += psd * d3 / 6.0;
    q(axis + 3, axis + 3) += psd * d3 / 3.0;
    q(axis + 3, axis + 6) += psd * d2 / 2.0;
    q(axis + 6, axis + 3) += psd * d2 / 2.0;
    q(axis + 6, axis + 6) += psd * dt;
  }
}

MotionStep motion_step(size_t model, const std::vector<double>& x, double dt,
                       const MultistaticImmTrackerConfig& config)
{
  MotionStep out{x, Matrix::identity(STATE_SIZE), Matrix(STATE_SIZE, STATE_SIZE)};
  if (model == 0) {
    const double decay = std::exp(-dt / config.cv_acceleration_decay_s);
    for (size_t axis = 0; axis < 3; ++axis) {
      out.state[axis] += dt * x[axis + 3];
      out.state[axis + 6] *= decay;
      out.transition(axis, axis + 3) = dt;
      out.transition(axis + 6, axis + 6) = decay;
      const double d2 = dt * dt, d3 = d2 * dt;
      out.noise(axis, axis) = config.cv_acceleration_psd * d3 / 3.0;
      out.noise(axis, axis + 3) = out.noise(axis + 3, axis)
          = config.cv_acceleration_psd * d2 / 2.0;
      out.noise(axis + 3, axis + 3) = config.cv_acceleration_psd * dt;
      out.noise(axis + 6, axis + 6) = config.cv_acceleration_psd * dt;
    }
    return out;
  }

  for (size_t axis = 0; axis < 3; ++axis) {
    out.state[axis] += dt * x[axis + 3] + 0.5 * dt * dt * x[axis + 6];
    out.state[axis + 3] += dt * x[axis + 6];
    out.transition(axis, axis + 3) = dt;
    out.transition(axis, axis + 6) = 0.5 * dt * dt;
    out.transition(axis + 3, axis + 6) = dt;
  }
  add_integrated_jerk(out.noise, dt, model == 1 ? config.ca_jerk_psd
                                                 : config.turn_jerk_psd);
  if (model != 2)
    return out;

  const double horizontal_speed2 = x[3] * x[3] + x[4] * x[4];
  double omega = horizontal_speed2 > 1e-6
      ? (x[3] * x[7] - x[4] * x[6]) / horizontal_speed2 : 0.0;
  // The discrete-time Nyquist bound is numerical, not a class/motion prior.
  omega = std::clamp(omega, -PI / dt, PI / dt);
  const double angle = omega * dt, cs = std::cos(angle), sn = std::sin(angle);
  const double vx = x[3], vy = x[4], ax = x[6], ay = x[7];
  const double next_vx = cs * vx - sn * vy;
  const double next_vy = sn * vx + cs * vy;
  out.state[0] = x[0] + 0.5 * dt * (vx + next_vx);
  out.state[1] = x[1] + 0.5 * dt * (vy + next_vy);
  out.state[3] = next_vx;
  out.state[4] = next_vy;
  out.state[6] = cs * ax - sn * ay;
  out.state[7] = sn * ax + cs * ay;
  out.transition = Matrix::identity(STATE_SIZE);
  out.transition(0, 3) = 0.5 * dt * (1.0 + cs);
  out.transition(0, 4) = -0.5 * dt * sn;
  out.transition(1, 3) = 0.5 * dt * sn;
  out.transition(1, 4) = 0.5 * dt * (1.0 + cs);
  out.transition(2, 5) = dt;
  out.transition(2, 8) = 0.5 * dt * dt;
  out.transition(3, 3) = cs; out.transition(3, 4) = -sn;
  out.transition(4, 3) = sn; out.transition(4, 4) = cs;
  out.transition(5, 8) = dt;
  out.transition(6, 6) = cs; out.transition(6, 7) = -sn;
  out.transition(7, 6) = sn; out.transition(7, 7) = cs;
  return out;
}

double position_condition(const Matrix& information)
{
  const SymmetricEigen3 eigen = symmetric_eigen_3x3(information);
  if (!(eigen.values[2] > 0.0) || !(eigen.values[0] > 0.0))
    return std::numeric_limits<double>::infinity();
  return std::sqrt(eigen.values[2] / eigen.values[0]);
}

std::pair<Vec3, Matrix> fit_velocity(Vec3 position,
                                     const std::vector<Observation>& observations)
{
  Matrix information(3, 3);
  std::vector<double> rhs(3, 0.0);
  for (const Observation& observation : observations) {
    const Vec3 tx_leg = position - observation.geometry.tx;
    const Vec3 rx_leg = position - observation.geometry.rx;
    const Vec3 gradient = normalized(tx_leg) + normalized(rx_leg);
    const double weight = 1.0 / observation.noise(1, 1);
    for (size_t r = 0; r < 3; ++r) {
      rhs[r] += gradient[r] * observation.value.range_rate_mps * weight;
      for (size_t c = 0; c < 3; ++c)
        information(r, c) += gradient[r] * gradient[c] * weight;
    }
  }
  const Matrix covariance = pseudoinverse_symmetric(information, 1e-9);
  const auto value = covariance * rhs;
  return {{value[0], value[1], value[2]}, covariance};
}

bool pairwise_range_feasible(const Observation& left,
                             const Observation& right,
                             double gate_chi2)
{
  /* For one target, (excess range + Tx/Rx baseline) is its total bistatic
   * path length.  Subtracting two such measurements cancels the Tx leg, and
   * the reverse triangle inequality bounds the remainder by the distance
   * between receivers.  This is a necessary condition, not a scene prior.
   * Expand the exact bound by the declared statistical gate and the measured
   * range variances so noisy but plausible tuples continue to reach the
   * nonlinear solver. */
  const double left_path = left.value.range_m + left.geometry.baseline_m();
  const double right_path = right.value.range_m + right.geometry.baseline_m();
  const double receiver_separation = norm(left.geometry.rx - right.geometry.rx);
  const double difference_variance = left.noise(0, 0) + right.noise(0, 0);
  const double uncertainty = std::sqrt(std::max(0.0, gate_chi2 * difference_variance));
  return std::abs(left_path - right_path) <= receiver_separation + uncertainty;
}

double adaptive_chi2_2d_gate(double false_intensity, double represented_s,
                             uint64_t hypotheses)
{
  const double budget = std::clamp(false_intensity * represented_s,
                                   std::numeric_limits<double>::epsilon(),
                                   1.0 - std::numeric_limits<double>::epsilon());
  const double tail = std::clamp(budget / std::max<uint64_t>(1, hypotheses),
                                 std::numeric_limits<double>::min(),
                                 1.0 - std::numeric_limits<double>::epsilon());
  // Exact upper-tail inverse for chi-square with two degrees of freedom.
  return -2.0 * std::log(tail);
}

double adaptive_chi2_1d_gate(double false_intensity, double represented_s,
                             uint64_t hypotheses)
{
  const double budget = std::clamp(false_intensity * represented_s,
                                   std::numeric_limits<double>::epsilon(),
                                   1.0 - std::numeric_limits<double>::epsilon());
  const double x = std::log(std::max<double>(1.0,
      static_cast<double>(std::max<uint64_t>(1, hypotheses)) / budget));
  // Laurent-Massart: P[chi2_1 > 1 + 2 sqrt(x) + 2x] <= exp(-x).
  return 1.0 + 2.0 * std::sqrt(x) + 2.0 * x;
}

struct ReceiverTracklet {
  Observation previous;
  Observation current;
  double continuity_chi2 = std::numeric_limits<double>::infinity();
};

std::pair<double, Matrix> temporal_state_cost(
    const std::vector<double>& state,
    const std::vector<Observation>& previous,
    const std::vector<Observation>& current,
    double dt, std::vector<double>* rhs_out = nullptr)
{
  Matrix information(6, 6);
  std::vector<double> rhs(6, 0.0);
  double cost = 0.0;
  auto accumulate_epoch = [&](const Observation& observation, double offset) {
    std::vector<double> epoch{
        state[0] + offset * state[3], state[1] + offset * state[4],
        state[2] + offset * state[5], state[3], state[4], state[5]};
    const auto predicted = enu_measurement_model(epoch, observation.geometry, false);
    const Matrix epoch_jacobian = enu_measurement_jacobian(epoch, observation.geometry, false);
    Matrix jacobian(2, 6);
    for (size_t row = 0; row < 2; ++row)
      for (size_t axis = 0; axis < 3; ++axis) {
        jacobian(row, axis) = epoch_jacobian(row, axis);
        jacobian(row, axis + 3) = offset * epoch_jacobian(row, axis)
                                  + epoch_jacobian(row, axis + 3);
      }
    const std::vector<double> residual{
        observation.value.range_m - predicted[0],
        observation.value.range_rate_mps - predicted[1]};
    const Matrix weight = pseudoinverse_symmetric(observation.noise);
    cost += quadratic(residual, weight);
    const auto weighted_residual = weight * residual;
    const Matrix weighted_jacobian = weight * jacobian;
    for (size_t row = 0; row < 6; ++row) {
      for (size_t measurement = 0; measurement < 2; ++measurement)
        rhs[row] += jacobian(measurement, row) * weighted_residual[measurement];
      for (size_t column = 0; column < 6; ++column)
        for (size_t measurement = 0; measurement < 2; ++measurement)
          information(row, column) += jacobian(measurement, row)
                                      * weighted_jacobian(measurement, column);
    }
  };
  for (const Observation& value : current) accumulate_epoch(value, 0.0);
  for (const Observation& value : previous) accumulate_epoch(value, -dt);
  if (rhs_out) *rhs_out = std::move(rhs);
  return {cost, information};
}

struct TemporalStateFit {
  bool valid = false;
  Vec3 position;
  Vec3 velocity;
  Matrix covariance{6, 6};
  double condition = std::numeric_limits<double>::infinity();
  double normalized_rms = std::numeric_limits<double>::infinity();
};

std::vector<Vec3> algebraic_three_surface_seeds(
    const std::vector<Observation>& observations)
{
  if (observations.size() < 3) return {};
  const Vec3 transmitter = observations.front().geometry.tx;
  Matrix position_matrix(3, 3);
  std::vector<double> response(3), path_coefficient(3);
  for (size_t row = 0; row < 3; ++row) {
    if (norm(observations[row].geometry.tx - transmitter) > 1e-6) return {};
    const double path = observations[row].value.range_m
                        + observations[row].geometry.baseline_m();
    const Vec3 difference = transmitter - observations[row].geometry.rx;
    position_matrix(row, 0) = 2.0 * difference.x;
    position_matrix(row, 1) = 2.0 * difference.y;
    position_matrix(row, 2) = 2.0 * difference.z;
    path_coefficient[row] = 2.0 * path;
    response[row] = path * path + dot(transmitter, transmitter)
                    - dot(observations[row].geometry.rx,
                          observations[row].geometry.rx);
  }
  Matrix position_inverse(3, 3);
  try { position_inverse = inverse(position_matrix); }
  catch (...) { return {}; }
  const auto intercept_value = position_inverse * response;
  for (double& value : path_coefficient) value = -value;
  const auto slope_value = position_inverse * path_coefficient;
  const Vec3 intercept{intercept_value[0], intercept_value[1], intercept_value[2]};
  const Vec3 slope{slope_value[0], slope_value[1], slope_value[2]};
  const Vec3 offset = intercept - transmitter;
  const double a = 1.0 - dot(slope, slope);
  const double b = -2.0 * dot(offset, slope);
  const double c = -dot(offset, offset);
  std::vector<double> transmitter_ranges;
  const double numeric_scale = std::max({1.0, std::abs(a), std::abs(b), std::abs(c)});
  if (std::abs(a) <= std::numeric_limits<double>::epsilon() * numeric_scale) {
    if (std::abs(b) > std::numeric_limits<double>::epsilon() * numeric_scale)
      transmitter_ranges.push_back(-c / b);
  } else {
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant >= -std::numeric_limits<double>::epsilon() * numeric_scale) {
      const double root = std::sqrt(std::max(0.0, discriminant));
      transmitter_ranges.push_back((-b - root) / (2.0 * a));
      if (root > std::numeric_limits<double>::epsilon() * numeric_scale)
        transmitter_ranges.push_back((-b + root) / (2.0 * a));
    }
  }
  std::vector<Vec3> result;
  for (double transmitter_range : transmitter_ranges) {
    if (!(std::isfinite(transmitter_range) && transmitter_range > 0.0)) continue;
    const Vec3 position = intercept + transmitter_range * slope;
    if (std::isfinite(position.x) && std::isfinite(position.y)
        && std::isfinite(position.z))
      result.push_back(position);
  }
  return result;
}

TemporalStateFit fit_temporal_state(const std::vector<Observation>& previous,
                                    const std::vector<Observation>& current,
                                    double dt,
                                    const MultistaticImmTrackerConfig& config)
{
  TemporalStateFit out;
  if (previous.size() != current.size() || current.size() < 3 || !(dt > 0.0)) return out;
  const auto current_positions = algebraic_three_surface_seeds(current);
  const auto previous_positions = algebraic_three_surface_seeds(previous);
  if (current_positions.empty() || previous_positions.empty()) return out;
  std::vector<std::vector<double>> seeds;
  for (const Vec3 current_position : current_positions)
    for (const Vec3 previous_position : previous_positions) {
      const Vec3 velocity = (current_position - previous_position) / dt;
      if (norm(velocity) > config.maximum_target_speed_mps) continue;
      seeds.push_back({current_position.x, current_position.y, current_position.z,
                       velocity.x, velocity.y, velocity.z});
      const Vec3 measured_velocity = fit_velocity(current_position, current).first;
      if (norm(measured_velocity) <= config.maximum_target_speed_mps)
        seeds.push_back({current_position.x, current_position.y, current_position.z,
                         measured_velocity.x, measured_velocity.y, measured_velocity.z});
    }
  if (seeds.empty()) return out;

  const size_t scalar_measurements = 4 * current.size();
  const size_t residual_dof = scalar_measurements - 6;
  // Compare the six-state trajectory against a saturated per-measurement explanation using BIC.
  // This is data-dimensional and introduces no scene-specific residual threshold.
  const double admission_cost = residual_dof * std::log(
      static_cast<double>(scalar_measurements));
  double best_cost = std::numeric_limits<double>::infinity();
  std::vector<double> best_state;
  Matrix best_information(6, 6);
  for (std::vector<double> state : seeds) {
    double damping = std::numeric_limits<double>::epsilon();
    double previous_cost = std::numeric_limits<double>::infinity();
    for (uint32_t iteration = 0; iteration < config.birth_maximum_iterations; ++iteration) {
      std::vector<double> rhs;
      const auto [cost, information] = temporal_state_cost(
          state, previous, current, dt, &rhs);
      if (!std::isfinite(cost)) break;
      Matrix damped = information;
      for (size_t axis = 0; axis < 6; ++axis)
        damped(axis, axis) += damping * std::max(1.0, information(axis, axis));
      std::vector<double> delta;
      try { delta = inverse(damped) * rhs; }
      catch (...) { break; }
      std::vector<double> candidate = state;
      for (size_t axis = 0; axis < 6; ++axis) candidate[axis] += delta[axis];
      const double candidate_cost = temporal_state_cost(
          candidate, previous, current, dt).first;
      if (candidate_cost < cost) {
        state = std::move(candidate);
        damping = std::max(std::numeric_limits<double>::epsilon(), damping * 0.25);
        const double step_norm = std::sqrt(std::inner_product(
            delta.begin(), delta.end(), delta.begin(), 0.0));
        if (std::abs(previous_cost - candidate_cost)
                <= std::numeric_limits<double>::epsilon() * std::max(1.0, candidate_cost)
            || step_norm <= std::sqrt(std::numeric_limits<double>::epsilon()))
          break;
        previous_cost = candidate_cost;
      } else {
        damping = std::min(1.0 / std::numeric_limits<double>::epsilon(), damping * 10.0);
      }
    }
    const auto [cost, information] = temporal_state_cost(state, previous, current, dt);
    if (std::isfinite(cost) && cost < best_cost) {
      best_cost = cost;
      best_state = std::move(state);
      best_information = information;
    }
  }
  if (best_state.empty() || best_cost > admission_cost) return out;
  Matrix position_information(3, 3);
  for (size_t row = 0; row < 3; ++row)
    for (size_t column = 0; column < 3; ++column)
      position_information(row, column) = best_information(row, column);
  out.condition = position_condition(position_information);
  if (!std::isfinite(out.condition)
      || (std::isfinite(config.birth_maximum_jacobian_condition)
          && out.condition > config.birth_maximum_jacobian_condition))
    return out;
  out.position = {best_state[0], best_state[1], best_state[2]};
  out.velocity = {best_state[3], best_state[4], best_state[5]};
  if (norm(out.velocity) > config.maximum_target_speed_mps) return out;
  out.covariance = pseudoinverse_symmetric(best_information, 1e-12);
  out.normalized_rms = std::sqrt(best_cost / std::max<size_t>(1, residual_dof));
  out.valid = true;
  return out;
}

struct CandidatePredictiveScore {
  RotatingPredictiveScore score;
  uint64_t soft_queries = 0;
  uint32_t soft_folds = 0;
};

SoftEvidenceEvaluation target_soft_evidence(
    const std::array<double, 6>& state,
    const Matrix& covariance,
    const ReceiverDetectionBatch& batch,
    const ReflectorPlane* reflector)
{
  SoftEvidenceEvaluation unavailable;
  if (!batch.soft_evidence || !batch.soft_evidence->valid()
      || covariance.rows() != 6 || covariance.cols() != 6)
    return unavailable;
  try {
    std::array<double, 2> predicted;
    Matrix jacobian;
    if (reflector) {
      predicted = reflected_dl_model(state, batch.geometry, *reflector);
      Detection dummy;
      const auto virtualized = virtualize_reflected_detection(
          batch.geometry, dummy, *reflector);
      jacobian = enu_measurement_jacobian(
          std::vector<double>(state.begin(), state.end()), virtualized.first, false);
    } else {
      const auto value = enu_measurement_model(
          std::vector<double>(state.begin(), state.end()), batch.geometry, false);
      predicted = {value[0], value[1]};
      jacobian = enu_measurement_jacobian(
          std::vector<double>(state.begin(), state.end()), batch.geometry, false);
    }
    Matrix predictive = jacobian * covariance * jacobian.transposed();
    predictive(0, 0) += batch.range_resolution_m * batch.range_resolution_m / 12.0;
    predictive(1, 1) += batch.rate_resolution_mps * batch.rate_resolution_mps / 12.0;
    return evaluate_soft_evidence(*batch.soft_evidence, predicted,
                                  positive_semidefinite(symmetrized(predictive), 1e-12));
  } catch (...) {
    return unavailable;
  }
}

SoftEvidenceEvaluation target_hard_proposal_evidence(
    const std::array<double, 6>& state,
    const Matrix& covariance,
    const std::vector<Observation>& proposals,
    const MultistaticImmTrackerConfig& config,
    const ReflectorPlane* reflector)
{
  SoftEvidenceEvaluation result;
  if (proposals.empty() || covariance.rows() != 6 || covariance.cols() != 6)
    return result; // A local detector miss is no evidence either way.

  // Under clutter/new-target, proposals are uniformly distributed over the declared range/rate
  // surveillance support.  Multiplicity is the measured number of proposals in this receiver and
  // CPI.  This supplies an out-of-sample likelihood for legacy/OTA streams that do not retain the
  // unthresholded map, without an empirical gate or a scenario calibration table.
  const double support_area = config.maximum_range_m
                              * (4.0 * config.maximum_target_speed_mps);
  if (!(std::isfinite(support_area) && support_area > 0.0)) return result;
  const double log_clutter_density = std::log(static_cast<double>(proposals.size()))
                                     - std::log(support_area);
  double best_log_bayes_factor = -std::numeric_limits<double>::infinity();
  for (const Observation& original : proposals) {
    if (!(original.range_informative || original.rate_informative)) continue;
    try {
      const Observation observation = reflector
          ? reflected_observation(original, *reflector) : original;
      const std::vector<double> state_vector(state.begin(), state.end());
      const auto predicted = enu_measurement_model(
          state_vector, observation.geometry, false);
      const Matrix jacobian = enu_measurement_jacobian(
          state_vector, observation.geometry, false);
      const Matrix predictive = positive_semidefinite(symmetrized(
          jacobian * covariance * jacobian.transposed() + observation.noise), 1e-12);
      const double determinant = determinant_2x2(predictive);
      if (!(std::isfinite(determinant) && determinant > 0.0)) continue;
      const std::vector<double> residual{
          observation.value.range_m - predicted[0],
          observation.value.range_rate_mps - predicted[1]};
      const double nis = quadratic(residual, pseudoinverse_symmetric(predictive));
      if (!std::isfinite(nis)) continue;
      const double log_target_density = -std::log(2.0 * PI)
          - 0.5 * std::log(determinant) - 0.5 * nis;
      const double log_bayes_factor = log_target_density - log_clutter_density;
      if (log_bayes_factor > best_log_bayes_factor) {
        best_log_bayes_factor = log_bayes_factor;
        result.posterior_mode = original.value;
      }
    } catch (...) {}
  }
  if (!std::isfinite(best_log_bayes_factor)) return result;
  result.available = true;
  result.log_bayes_factor = best_log_bayes_factor;
  result.integrated_cells = proposals.size();
  return result;
}

SoftEvidenceEvaluation target_proposal_evidence(
    const std::array<double, 6>& state,
    const Matrix& covariance,
    const ReceiverDetectionBatch& batch,
    const std::vector<Observation>& proposals,
    const MultistaticImmTrackerConfig& config,
    const ReflectorPlane* reflector)
{
  const SoftEvidenceEvaluation soft = target_soft_evidence(
      state, covariance, batch, reflector);
  if (soft.available) return soft;
  return target_hard_proposal_evidence(
      state, covariance, proposals, config, reflector);
}

CandidatePredictiveScore score_candidate_three_plus_one(
    const std::vector<Observation>& previous,
    const std::vector<Observation>& current,
    const std::vector<ReceiverDetectionBatch>& current_batches,
    const std::vector<std::vector<Observation>>& previous_proposals,
    const std::vector<std::vector<Observation>>& current_proposals,
    const std::vector<std::shared_ptr<const SoftRangeRateEvidence>>& previous_surfaces,
    double dt,
    const MultistaticImmTrackerConfig& config,
    const ReflectorPlane* reflector)
{
  CandidatePredictiveScore result;
  std::array<const Observation*, 4> previous_by_receiver{};
  std::array<const Observation*, 4> current_by_receiver{};
  std::array<const ReceiverDetectionBatch*, 4> batch_by_receiver{};
  for (const auto& observation : previous)
    if (observation.receiver < previous_by_receiver.size())
      previous_by_receiver[observation.receiver] = &observation;
  for (const auto& observation : current)
    if (observation.receiver < current_by_receiver.size())
      current_by_receiver[observation.receiver] = &observation;
  for (const auto& batch : current_batches)
    if (batch.receiver_index < batch_by_receiver.size())
      batch_by_receiver[batch.receiver_index] = &batch;

  std::vector<std::optional<double>> folds;
  uint64_t scalar_count = 0;
  for (size_t heldout = 0; heldout < batch_by_receiver.size(); ++heldout) {
    if (!batch_by_receiver[heldout]) continue;
    std::vector<Observation> fold_previous, fold_current;
    for (size_t receiver = 0; receiver < batch_by_receiver.size(); ++receiver) {
      if (receiver == heldout || !previous_by_receiver[receiver]
          || !current_by_receiver[receiver])
        continue;
      fold_previous.push_back(reflector
          ? reflected_observation(*previous_by_receiver[receiver], *reflector)
          : *previous_by_receiver[receiver]);
      fold_current.push_back(reflector
          ? reflected_observation(*current_by_receiver[receiver], *reflector)
          : *current_by_receiver[receiver]);
    }
    if (fold_current.size() != config.birth_minimum_receivers) continue;
    TemporalStateFit fit = fit_temporal_state(
        fold_previous, fold_current, dt, config);
    if (!fit.valid) continue;
    const std::array<double, 6> current_state{
        fit.position.x, fit.position.y, fit.position.z,
        fit.velocity.x, fit.velocity.y, fit.velocity.z};
    const std::vector<Observation> no_proposals;
    const auto& current_heldout = heldout < current_proposals.size()
        ? current_proposals[heldout] : no_proposals;
    const SoftEvidenceEvaluation now = target_proposal_evidence(
        current_state, fit.covariance, *batch_by_receiver[heldout], current_heldout,
        config, reflector);
    ++result.soft_queries;
    if (!now.available) continue;
    if (batch_by_receiver[heldout]->soft_evidence
        && batch_by_receiver[heldout]->soft_evidence->valid())
      ++result.soft_folds;
    std::vector<double> temporal_evidence{now.log_bayes_factor};
    const bool previous_soft_available = heldout < previous_surfaces.size()
        && previous_surfaces[heldout] && previous_surfaces[heldout]->valid();
    const bool previous_hard_available = heldout < previous_proposals.size()
        && !previous_proposals[heldout].empty();
    if (previous_soft_available || previous_hard_available) {
      std::array<double, 6> old_state = current_state;
      for (size_t axis = 0; axis < 3; ++axis) old_state[axis] -= dt * old_state[axis + 3];
      Matrix propagation = Matrix::identity(6);
      for (size_t axis = 0; axis < 3; ++axis) propagation(axis, axis + 3) = -dt;
      const Matrix old_covariance = positive_semidefinite(symmetrized(
          propagation * fit.covariance * propagation.transposed()), 1e-12);
      ReceiverDetectionBatch old_batch = *batch_by_receiver[heldout];
      old_batch.soft_evidence = previous_soft_available
          ? previous_surfaces[heldout] : nullptr;
      const auto& previous_heldout = heldout < previous_proposals.size()
          ? previous_proposals[heldout] : no_proposals;
      const SoftEvidenceEvaluation old = target_proposal_evidence(
          old_state, old_covariance, old_batch, previous_heldout, config, reflector);
      ++result.soft_queries;
      if (old.available) temporal_evidence.push_back(old.log_bayes_factor);
    }
    // Two adjacent CPIs are a temporal validation unit. Averaging prevents their reused target
    // state from being treated as two independent likelihood products.
    folds.push_back(std::accumulate(temporal_evidence.begin(), temporal_evidence.end(), 0.0)
                    / temporal_evidence.size());
    scalar_count = std::max<uint64_t>(
        scalar_count, 2 * (fold_previous.size() + fold_current.size() + 1));
  }
  result.score = predictive_bic_from_correlated_folds(
      folds, 6, scalar_count);
  return result;
}

} // namespace

struct MultistaticImmTracker::BirthSolution {
  std::vector<Observation> observations;
  std::vector<Observation> previous_observations;
  Vec3 position;
  Vec3 velocity;
  Matrix position_covariance{3, 3};
  Matrix velocity_covariance{3, 3};
  Matrix state_covariance{6, 6};
  double condition = std::numeric_limits<double>::infinity();
  double normalized_rms = std::numeric_limits<double>::infinity();
  UlDtdDfsValidation ul_validation;
  RotatingPredictiveScore direct_predictive;
  RotatingPredictiveScore reflected_predictive;
  uint64_t reflector_id = 0;
};

struct MultistaticImmTracker::BirthHistory {
  bool valid = false;
  double time_s = 0.0;
  uint64_t sequence = 0;
  std::vector<std::vector<Observation>> observations;
  std::vector<std::shared_ptr<const SoftRangeRateEvidence>> soft_surfaces;
};

struct MultistaticImmTracker::Track {
  uint64_t id = 0;
  uint64_t birth_family = 0; // internal explanation group, not a physical identity
  MultistaticImmTrackerConfig config;
  std::array<std::vector<double>, MODEL_COUNT> model_state;
  std::array<Matrix, MODEL_COUNT> model_covariance{
      Matrix(STATE_SIZE, STATE_SIZE), Matrix(STATE_SIZE, STATE_SIZE),
      Matrix(STATE_SIZE, STATE_SIZE)};
  std::array<double, MODEL_COUNT> probability{};
  std::vector<double> state = std::vector<double>(STATE_SIZE, 0.0);
  Matrix covariance{STATE_SIZE, STATE_SIZE};
  double time_s = 0.0;
  uint64_t source_sequence = 0;
  bool updated = true;
  bool confirmed = false;
  uint32_t receiver_updates = 0;
  uint32_t coasts = 0;
  uint32_t total_updates = 1;
  uint32_t confirmed_updates = 0;
  double nis = 0.0;
  bool has_nis = false;
  double nis_ewma = 0.0;
  bool has_nis_ewma = false;
  uint64_t nis_updates = 0;
  double birth_condition = std::numeric_limits<double>::infinity();
  double last_score = 0.0;
  double existence_log_evidence = 0.0;
  double confirmation_log_threshold = std::numeric_limits<double>::infinity();
  double ul_validation_log_bayes_factor = 0.0;
  std::string ul_validation_status = "unavailable";
  double direct_predictive_bic = std::numeric_limits<double>::infinity();
  double reflected_predictive_bic = std::numeric_limits<double>::infinity();
  struct TimedEvidence {
    double time_s = 0.0;
    double log_bayes_factor = 0.0;
  };
  std::deque<TimedEvidence> direct_validation_window;
  std::array<std::deque<TimedEvidence>, 4> direct_receiver_windows;
  std::deque<TimedEvidence> direct_vs_reflected_window;
  uint64_t shared_reflector_id = 0;
  uint64_t physical_parent_track_id = 0;
  bool reflected_path = false;
  CausalOriginHypotheses origin;
  ExistenceEvidence existence;
  bool associated_object_path = false;
  uint64_t associated_parent_id = 0;
  uint64_t associated_plane_id = 0;
  // Detector precision stays in Observation::noise. These are causal object-
  // model discrepancies learned only AFTER scoring credible multi-RX updates.
  std::array<Matrix, 4> object_discrepancy{{Matrix(2,2), Matrix(2,2), Matrix(2,2), Matrix(2,2)}};
  std::array<Matrix, 4> residual_excess{{Matrix(2,2), Matrix(2,2), Matrix(2,2), Matrix(2,2)}};

  void prune_direct_evidence(double now_s)
  {
    const double oldest_s = now_s - config.maximum_propagation_s;
    auto prune = [&](auto& values) {
      while (!values.empty() && values.front().time_s < oldest_s)
        values.pop_front();
    };
    prune(direct_validation_window);
    for (auto& values : direct_receiver_windows) prune(values);
    prune(direct_vs_reflected_window);
  }

  void observe_direct_validation(const RotatingPredictiveScore& score)
  {
    prune_direct_evidence(time_s);
    if (!score.available || !std::isfinite(score.bic))
      return;
    const double log_bayes_factor = -0.5 * score.bic;
    direct_validation_window.push_back({time_s, log_bayes_factor});
  }

  void observe_direct_receiver(uint32_t receiver,
                               const SoftEvidenceEvaluation& evidence)
  {
    prune_direct_evidence(time_s);
    if (receiver >= direct_receiver_windows.size()
        || !evidence.available || !std::isfinite(evidence.log_bayes_factor))
      return;
    direct_receiver_windows[receiver].push_back({time_s, evidence.log_bayes_factor});
  }

  void observe_path_model_comparison(double direct_bic, double reflected_bic)
  {
    prune_direct_evidence(time_s);
    if (!(std::isfinite(direct_bic) && std::isfinite(reflected_bic))) return;
    // Positive values favor the direct model. BIC already charges each model's parameters and
    // reflector-bank selection; summing only across distinct CPIs is a causal sequential test.
    direct_vs_reflected_window.push_back(
        {time_s, 0.5 * (reflected_bic - direct_bic)});
  }

  uint32_t direct_supported_receiver_count() const
  {
    uint32_t count = 0;
    for (const auto& values : direct_receiver_windows) {
      // Two positive observations are the minimum temporal evidence. Negative measurements still
      // enter the aggregate sequential likelihood below, but cannot permanently poison one RX.
      if (std::count_if(values.begin(), values.end(), [](const TimedEvidence& value) {
            return value.log_bayes_factor > 0.0;
          }) >= 2)
        ++count;
    }
    return count;
  }

  uint64_t direct_validation_epoch_count() const
  { return direct_validation_window.size(); }

  uint64_t direct_support_epoch_count() const
  {
    return std::count_if(
        direct_validation_window.begin(), direct_validation_window.end(),
        [](const TimedEvidence& value) { return value.log_bayes_factor > 0.0; });
  }

  double direct_validation_mean() const
  {
    if (direct_validation_window.empty()) return 0.0;
    return std::accumulate(
        direct_validation_window.begin(), direct_validation_window.end(), 0.0,
        [](double total, const TimedEvidence& value) {
          return total + value.log_bayes_factor;
        }) / static_cast<double>(direct_validation_window.size());
  }

  double direct_vs_reflected_mean() const
  {
    if (direct_vs_reflected_window.empty())
      return std::numeric_limits<double>::infinity();
    return std::accumulate(
        direct_vs_reflected_window.begin(), direct_vs_reflected_window.end(), 0.0,
        [](double total, const TimedEvidence& value) {
          return total + value.log_bayes_factor;
        }) / static_cast<double>(direct_vs_reflected_window.size());
  }

  bool direct_emission_validated() const
  {
    // OUR ADAPTATION: no declared target class can be below ground; the receiver hardware (X410)
    // sits 1 m above ground level, so a real target's fitted height cannot be more than 1 m below
    // the receiver's own datum either way. Applied continuously (not just at birth) in case a
    // track's own state drifts below the floor after it was born.
    if (state[2] < -1.0) return false;
    if (config.evidence_mode)
      return !birth_family && existence.reportable() && !associated_object_path;
    // Two is the smallest number of distinct CPIs that constitutes temporal evidence. Each of
    // the three receivers is accumulated independently, so non-coincident local misses are
    // neutral and cannot become a global miss.
    if (direct_validation_epoch_count() < 2 || direct_support_epoch_count() < 2
        || direct_supported_receiver_count() < config.birth_minimum_receivers
        || !(direct_validation_mean() > 0.0))
      return false;
    if (direct_vs_reflected_mean() < 0.0)
      return false;
    const OriginHypothesisEvidence evidence = origin.evaluate(
        direct_vs_clutter_log_evidence());
    // Similar motion alone is not enough to suppress two real co-moving targets. Exclusivity is
    // actionable only after the independent temporal/cross-RX origin model is identifiable.
    if (evidence.ready && evidence.decision == "static_slow_multipath"
        && evidence.exclusivity_competitor_track_id != 0
        && evidence.exclusivity_log_bayes_factor > 0.0)
      return false;
    return !reflected_path;
  }

  std::array<double, 6> target_state() const
  { return {state[0], state[1], state[2], state[3], state[4], state[5]}; }

  Matrix target_covariance() const
  {
    Matrix result(6, 6);
    for (size_t row = 0; row < 6; ++row)
      for (size_t column = 0; column < 6; ++column)
        result(row, column) = covariance(row, column);
    return positive_semidefinite(symmetrized(result), 1e-12);
  }

  void apply_ul_validation(const UlDtdDfsValidation& validation)
  {
    ul_validation_status = ul_differential_decision_name(validation.decision);
    ul_validation_log_bayes_factor = validation.log_bayes_factor;
    if (!std::isfinite(validation.log_bayes_factor)) return;
    if (validation.decision == UlDifferentialDecision::support) {
      origin.apply_ul_direct_evidence(validation.log_bayes_factor);
    } else if (validation.decision == UlDifferentialDecision::contradiction) {
      origin.apply_ul_direct_evidence(validation.log_bayes_factor);
    }
  }

  double direct_vs_clutter_log_evidence() const
  { return existence_log_evidence - confirmation_log_threshold; }

  void combine()
  {
    std::fill(state.begin(), state.end(), 0.0);
    for (size_t model = 0; model < MODEL_COUNT; ++model)
      for (size_t i = 0; i < STATE_SIZE; ++i)
        state[i] += probability[model] * model_state[model][i];
    covariance = Matrix(STATE_SIZE, STATE_SIZE);
    for (size_t model = 0; model < MODEL_COUNT; ++model) {
      std::vector<double> delta(STATE_SIZE);
      for (size_t i = 0; i < STATE_SIZE; ++i)
        delta[i] = model_state[model][i] - state[i];
      covariance = covariance + probability[model]
          * (model_covariance[model] + outer(delta));
    }
    covariance = positive_semidefinite(covariance, 1e-12);
  }

  void predict(double new_time)
  {
    const double dt = new_time - time_s;
    if (dt < 0.0) throw std::invalid_argument("IMM tracker time moved backwards");
    receiver_updates = 0;
    updated = false;
    has_nis = false;
    prune_direct_evidence(new_time);
    if (dt == 0.0) return;
    std::array<double, MODEL_COUNT> predicted_probability{};
    std::array<std::vector<double>, MODEL_COUNT> mixed_state;
    std::array<Matrix, MODEL_COUNT> mixed_covariance{
        Matrix(STATE_SIZE, STATE_SIZE), Matrix(STATE_SIZE, STATE_SIZE),
        Matrix(STATE_SIZE, STATE_SIZE)};
    for (size_t target = 0; target < MODEL_COUNT; ++target) {
      for (size_t source = 0; source < MODEL_COUNT; ++source)
        predicted_probability[target] += probability[source]
            * config.model_transition[source][target];
      mixed_state[target].assign(STATE_SIZE, 0.0);
      const double denominator = std::max(predicted_probability[target], 1e-15);
      for (size_t source = 0; source < MODEL_COUNT; ++source) {
        const double weight = probability[source] * config.model_transition[source][target]
                              / denominator;
        for (size_t i = 0; i < STATE_SIZE; ++i)
          mixed_state[target][i] += weight * model_state[source][i];
      }
      for (size_t source = 0; source < MODEL_COUNT; ++source) {
        const double weight = probability[source] * config.model_transition[source][target]
                              / denominator;
        std::vector<double> delta(STATE_SIZE);
        for (size_t i = 0; i < STATE_SIZE; ++i)
          delta[i] = model_state[source][i] - mixed_state[target][i];
        mixed_covariance[target] = mixed_covariance[target]
            + weight * (model_covariance[source] + outer(delta));
      }
      const MotionStep step = motion_step(target, mixed_state[target], dt, config);
      model_state[target] = step.state;
      model_covariance[target] = positive_semidefinite(
          step.transition * mixed_covariance[target] * step.transition.transposed()
          + step.noise, 1e-12);
    }
    probability = predicted_probability;
    time_s = new_time;
    combine();
  }

  double innovation_nis(const Observation& observation) const
  {
    try {
      const auto predicted = acquisition_model(state, observation);
      const Matrix h = acquisition_jacobian(state, observation);
      const Matrix s = h * covariance * h.transposed() + observation.noise;
      return quadratic({observation.value.range_m - predicted[0],
                        observation.value.range_rate_mps - predicted[1]},
                       pseudoinverse_symmetric(s));
    } catch (...) {
      return std::numeric_limits<double>::infinity();
    }
  }

  std::optional<OriginInnovation> origin_innovation(const Observation& observation) const
  {
    try {
      const auto predicted = measurement_model(state, observation.geometry);
      const Matrix h = measurement_jacobian(state, observation.geometry);
      const Matrix s = symmetrized(h * covariance * h.transposed() + observation.noise);
      const double l00 = std::sqrt(s(0, 0));
      if (!(std::isfinite(l00) && l00 > 0.0)) return std::nullopt;
      const double l10 = s(1, 0) / l00;
      const double remaining = s(1, 1) - l10 * l10;
      if (!(std::isfinite(remaining) && remaining > 0.0)) return std::nullopt;
      const double l11 = std::sqrt(remaining);
      const double residual_range = observation.value.range_m - predicted[0];
      const double first = residual_range / l00;
      const double second = (observation.value.range_rate_mps - predicted[1] - l10 * first)
                            / l11;
      if (!(std::isfinite(first) && std::isfinite(second))) return std::nullopt;
      return OriginInnovation{observation.receiver, {first, second}};
    } catch (...) {
      return std::nullopt;
    }
  }

  void observe_origin_epoch(const std::vector<Observation>& observations)
  {
    std::vector<OriginInnovation> innovations;
    innovations.reserve(observations.size());
    for (const Observation& observation : observations) {
      const auto value = origin_innovation(observation);
      if (value) innovations.push_back(*value);
    }
    Matrix velocity_covariance(3, 3);
    for (size_t row = 0; row < 3; ++row)
      for (size_t column = 0; column < 3; ++column)
        velocity_covariance(row, column) = covariance(row + 3, column + 3);
    origin.observe_epoch(innovations, {state[3], state[4], state[5]},
                         positive_semidefinite(symmetrized(velocity_covariance), 1e-12),
                         config.birth_minimum_receivers);
  }

  bool update_measurement(const Observation& observation)
  {
    std::array<double, MODEL_COUNT> likelihood{};
    double total_probability = 0.0;
    double mixture_nis = 0.0;
    for (size_t model = 0; model < MODEL_COUNT; ++model) {
      try {
        const auto predicted = acquisition_model(model_state[model], observation);
        const std::vector<double> residual{
            observation.value.range_m - predicted[0],
            observation.value.range_rate_mps - predicted[1]};
        const Matrix h = acquisition_jacobian(model_state[model], observation);
        const Matrix s = h * model_covariance[model] * h.transposed() + observation.noise;
        const Matrix inverse_s = pseudoinverse_symmetric(s);
        const double model_nis = quadratic(residual, inverse_s);
        const double determinant = determinant_2x2(s);
        likelihood[model] = determinant > 0.0 && std::isfinite(model_nis)
            ? std::exp(-0.5 * std::min(model_nis, 1400.0))
                / (2.0 * PI * std::sqrt(determinant)) : 0.0;
        const Matrix gain = model_covariance[model] * h.transposed() * inverse_s;
        const auto correction = gain * residual;
        for (size_t i = 0; i < STATE_SIZE; ++i)
          model_state[model][i] += correction[i];
        const Matrix factor = Matrix::identity(STATE_SIZE) - gain * h;
        model_covariance[model] = positive_semidefinite(
            factor * model_covariance[model] * factor.transposed()
            + gain * observation.noise * gain.transposed(), 1e-12);
        mixture_nis += probability[model] * model_nis;
      } catch (...) {
        likelihood[model] = 0.0;
      }
      total_probability += probability[model] * likelihood[model];
    }
    if (!(total_probability > std::numeric_limits<double>::min()))
      return false;
    for (size_t model = 0; model < MODEL_COUNT; ++model)
      probability[model] = probability[model] * likelihood[model] / total_probability;
    combine();
    nis = mixture_nis;
    has_nis = true;
    ++nis_updates;
    nis_ewma += (nis - nis_ewma) / static_cast<double>(nis_updates);
    has_nis_ewma = true;
    updated = true;
    ++receiver_updates;
    last_score = std::max(last_score, observation.value.score);
    return true;
  }

  void finish_epoch(double represented_s)
  {
    // Birth already contains three receivers over two CPIs.  After that admission, a false
    // negative at any receiver is local: any statistically associated receiver refreshes the
    // existing track, while the remaining receivers only improve observability/covariance.
    if (updated && receiver_updates > 0) {
      coasts = 0;
      ++total_updates;
      if (receiver_updates >= config.birth_minimum_receivers) {
        const double false_probability = std::clamp(
            config.false_object_intensity_per_s * represented_s,
            std::numeric_limits<double>::epsilon(),
            1.0 - std::numeric_limits<double>::epsilon());
        existence_log_evidence -= std::log(false_probability);
        if (!confirmed && existence_log_evidence >= confirmation_log_threshold)
          confirmed = true;
      }
      if (confirmed) ++confirmed_updates;
    } else {
      ++coasts;
    }
  }

  TrackSnapshot snapshot(const BistaticGeometry& reporting_geometry) const
  {
    TrackSnapshot out;
    out.track_id = id;
    out.status = confirmed ? (updated ? "confirmed" : "coasting") : "tentative";
    if (config.evidence_mode) {
      out.status = existence_phase_name(existence.phase());
      out.existence_score = existence.score();
      out.existence_threshold = existence.threshold();
      out.existence_receiver_mask = existence.receiver_mask();
      if (associated_object_path) out.status = "associated_path";
    }
    out.air_time_s = time_s;
    out.has_time = true;
    out.updated = updated;
    out.position_valid = true;
    out.position_enu_m = {state[0], state[1], state[2]};
    out.velocity_enu_mps = {state[3], state[4], state[5]};
    for (size_t r = 0; r < 3; ++r)
      for (size_t c = 0; c < 3; ++c) {
        out.position_covariance(r, c) = covariance(r, c);
        out.velocity_covariance(r, c) = covariance(r + 3, c + 3);
      }
    try {
      const auto observable = measurement_model(state, reporting_geometry);
      const Matrix h = measurement_jacobian(state, reporting_geometry);
      const Matrix projected = h * covariance * h.transposed();
      out.range_m = observable[0];
      out.range_rate_mps = observable[1];
      out.sigma_range_m = std::sqrt(std::max(0.0, projected(0, 0)));
      out.sigma_rate_mps = std::sqrt(std::max(0.0, projected(1, 1)));
    } catch (...) {}
    out.range_accel_mps2 = state[6];
    out.sigma_accel_mps2 = std::sqrt(std::max(0.0, covariance(6, 6)));
    out.has_nis = has_nis; out.nis = nis;
    out.has_nis_ewma = has_nis_ewma; out.nis_ewma = nis_ewma;
    out.coast_count = coasts;
    out.confirmed_update_count = confirmed_updates;
    out.total_update_count = total_updates;
    out.source_cpi_sequence = source_sequence;
    out.imm_valid = true;
    out.imm_model_probabilities = probability;
    out.receiver_update_count = receiver_updates;
    out.geometry_condition = birth_condition;
    out.has_geometry_condition = std::isfinite(birth_condition);
    out.ul_validation_status = ul_validation_status;
    out.ul_validation_log_bayes_factor = ul_validation_log_bayes_factor;
    out.direct_predictive_bic = direct_predictive_bic;
    out.reflected_predictive_bic = reflected_predictive_bic;
    out.direct_validation_epochs = direct_validation_epoch_count();
    out.direct_support_epochs = direct_support_epoch_count();
    out.direct_supported_receivers = direct_supported_receiver_count();
    out.direct_validation_mean_log_bayes_factor = direct_validation_mean();
    out.direct_emission_validated = direct_emission_validated();
    out.shared_reflector_id = shared_reflector_id;
    out.physical_parent_track_id = physical_parent_track_id;
    if (associated_object_path) out.physical_parent_track_id = associated_parent_id;
    const OriginHypothesisEvidence origin_evidence = origin.evaluate(
        direct_vs_clutter_log_evidence());
    out.origin_hypothesis = reflected_path ? "shared_reflector_path"
                                           : origin_evidence.decision;
    out.origin_evidence_ready = reflected_path || origin_evidence.ready;
    out.origin_log_evidence = origin_evidence.log_evidence;
    out.origin_temporal_structure_log_bayes_factor =
        origin_evidence.temporal_structure_log_bayes_factor;
    out.origin_cross_rx_structure_log_bayes_factor =
        origin_evidence.cross_receiver_structure_log_bayes_factor;
    out.origin_stationary_log_bayes_factor = origin_evidence.stationary_log_bayes_factor;
    out.origin_ul_direct_log_bayes_factor = origin_evidence.ul_direct_log_bayes_factor;
    out.origin_exclusivity_log_bayes_factor = origin_evidence.exclusivity_log_bayes_factor;
    out.origin_exclusivity_competitor_track_id =
        origin_evidence.exclusivity_competitor_track_id;
    out.origin_observed_epochs = origin_evidence.observed_epochs;
    out.origin_cross_rx_epochs = origin_evidence.cross_receiver_epochs;
    const Vec3 from_rx = out.position_enu_m - reporting_geometry.rx;
    out.azimuth_deg = std::atan2(from_rx.y, from_rx.x) * 180.0 / PI;
    out.elevation_deg = std::atan2(from_rx.z, std::hypot(from_rx.x, from_rx.y)) * 180.0 / PI;
    return out;
  }
};

#include "multistatic_evidence.inc"

MultistaticImmTracker::MultistaticImmTracker(std::vector<BistaticGeometry> geometries,
                                             MultistaticImmTrackerConfig config)
    : geometries_(std::move(geometries)), config_(std::move(config)),
      birth_history_(std::make_unique<BirthHistory>())
{
  evidence_history_ = std::make_unique<EvidenceHistory>();
  if (config_.evidence_mode && geometries_.size() != 4)
    throw std::invalid_argument("experimental evidence requires exactly four surveyed receivers");
  if (config_.lifecycle_features>7 || (config_.lifecycle_features && !config_.evidence_mode))
    throw std::invalid_argument("invalid lifecycle feature mask");
  if (config_.evidence_mode > 6 || !std::isfinite(config_.evidence_window_s)
      || config_.evidence_window_s < 0.1 || config_.evidence_window_s > 0.2
      || !std::isfinite(config_.existence_threshold_scale)
      || config_.existence_threshold_scale <= 0.0 || config_.maximum_local_map_cells < 16)
    throw std::invalid_argument("invalid unified evidence configuration");
  if (geometries_.size() < config_.birth_minimum_receivers)
    throw std::invalid_argument("multistatic tracker has too few surveyed receivers");
  for (const BistaticGeometry& geometry : geometries_) geometry.validate();
  if (config_.birth_minimum_receivers != 3
      || config_.birth_minimum_receivers > geometries_.size()
      || !(config_.birth_maximum_jacobian_condition > 1.0)
      || !(config_.maximum_propagation_s > 0.0)
      || !(config_.cv_acceleration_decay_s > 0.0)
      || !(config_.maximum_target_speed_mps > 0.0)
      || !(config_.maximum_range_m > 0.0)
      || !(std::isfinite(config_.false_object_intensity_per_s)
           && config_.false_object_intensity_per_s > 0.0
           && config_.false_object_intensity_per_s * SPATIAL_CPI_DURATION_S < 1.0)
      || !(config_.cv_acceleration_psd > 0.0) || !(config_.ca_jerk_psd > 0.0)
      || !(config_.turn_jerk_psd > 0.0))
    throw std::invalid_argument("invalid multistatic IMM tracker configuration");
  double initial_sum = 0.0;
  for (double probability : config_.initial_model_probability) {
    if (!(std::isfinite(probability) && probability >= 0.0))
      throw std::invalid_argument("IMM initial probabilities must be finite and nonnegative");
    initial_sum += probability;
  }
  if (!(initial_sum > 0.0))
    throw std::invalid_argument("IMM initial probabilities have zero mass");
  for (size_t row = 0; row < MODEL_COUNT; ++row) {
    double sum = 0.0;
    for (size_t col = 0; col < MODEL_COUNT; ++col) {
      if (!(config_.model_transition[row][col] >= 0.0))
        throw std::invalid_argument("IMM transition probabilities must be nonnegative");
      sum += config_.model_transition[row][col];
    }
    if (std::abs(sum - 1.0) > 1e-9)
      throw std::invalid_argument("each IMM transition row must sum to one");
  }
}

MultistaticImmTracker::~MultistaticImmTracker() = default;

void MultistaticImmTracker::reset()
{
  *evidence_history_ = EvidenceHistory{};
  tracks_.clear();
  *birth_history_ = BirthHistory{};
  ul_validators_.clear();
  ul_session_last_seen_s_.clear();
  ul_bootstrap_history_.clear();
  ul_bootstrap_target_ids_.clear();
  ul_direct_rate_history_.clear();
  exclusivity_overlap_epochs_.clear();
  reflector_bank_.reset();
  next_id_ = 1; time_s_ = 0.0; have_time_ = false;
}

void MultistaticImmTracker::update(double time,
                                   const std::vector<ReceiverDetectionBatch>& batches,
                                   uint64_t sequence,
                                   double processing_budget_s,
                                   const std::vector<UlDifferentialReceiverBatch>& ul_batches)
{
  if (config_.evidence_mode) {
    update_evidence(time, batches, sequence, processing_budget_s, ul_batches);
    return;
  }
  using ProcessingClock = std::chrono::steady_clock;
  if (!(std::isfinite(processing_budget_s) && processing_budget_s >= 0.0
        && processing_budget_s <= SPATIAL_CPI_DURATION_S))
    throw std::invalid_argument("IMM processing budget must be within the 75 ms CPI");
  const auto processing_started = ProcessingClock::now();
  last_processing_stats_ = {};
  last_processing_stats_.processing_budget_s = processing_budget_s;
  auto finish_processing = [&] {
    last_processing_stats_.active_internal_tracks = tracks_.size();
    last_processing_stats_.reportable_tracks = std::count_if(
        tracks_.begin(), tracks_.end(),
        [](const Track& track) {
          return track.confirmed && track.direct_emission_validated();
        });
    last_processing_stats_.emission_suppressed_tracks = std::count_if(
        tracks_.begin(), tracks_.end(),
        [](const Track& track) {
          return track.confirmed && !track.direct_emission_validated();
        });
    last_processing_stats_.processing_elapsed_s = std::chrono::duration<double>(
        ProcessingClock::now() - processing_started).count();
    last_processing_stats_.processing_deadline_exhausted =
        last_processing_stats_.processing_elapsed_s > processing_budget_s;
  };
  double represented_s = SPATIAL_CPI_DURATION_S;
  if (have_time_) {
    const double dt = time - time_s_;
    if (dt < 0.0) throw std::invalid_argument("multistatic tracker time moved backwards");
    if (dt > config_.maximum_propagation_s) reset();
    else if (dt > 0.0) represented_s = dt;
  }
  have_time_ = true; time_s_ = time;
  for (Track& track : tracks_) track.predict(time);

  std::vector<std::vector<Observation>> observations(geometries_.size());
  std::vector<uint8_t> seen_receiver(geometries_.size(), 0);
  for (size_t batch_index = 0; batch_index < batches.size(); ++batch_index) {
    const auto& batch = batches[batch_index];
    if (batch.receiver_index >= geometries_.size())
      throw std::invalid_argument("receiver batch index is outside surveyed geometry");
    if (seen_receiver[batch.receiver_index]++)
      throw std::invalid_argument("one CPI contains a duplicate receiver batch");
    if (norm(batch.geometry.tx - geometries_[batch.receiver_index].tx) > 1e-6
        || norm(batch.geometry.rx - geometries_[batch.receiver_index].rx) > 1e-6)
      throw std::invalid_argument("receiver batch geometry changed after tracker construction");
    for (size_t detection = 0; detection < batch.detections.size(); ++detection) {
      const Detection& value = batch.detections[detection];
      if (!std::isfinite(value.range_m) || value.range_m < 0.0
          || !std::isfinite(value.range_rate_mps)
          || value.range_m > config_.maximum_range_m
          || std::abs(value.range_rate_mps) > 2.0 * config_.maximum_target_speed_mps
          || !std::isfinite(value.score))
        continue;
      const MeasurementQuality quality = measurement_quality(
          value, batch.range_resolution_m, batch.rate_resolution_mps,
          config_.maximum_range_m, config_.maximum_target_speed_mps);
      observations[batch.receiver_index].push_back({
          batch.receiver_index, detection, batch.geometry, value, quality.noise,
          quality.range_informative, quality.rate_informative});
    }
  }

  std::map<uint64_t, std::vector<UlDifferentialReceiverBatch>> ul_by_session;
  for (const auto& batch : ul_batches) {
    if (batch.session_id == 0) {
      ++last_processing_stats_.ul_unknown_session_batches;
      continue;
    }
    ul_by_session[batch.session_id].push_back(batch);
    ul_session_last_seen_s_[batch.session_id] = time;
  }
  // A direct PUSCH reference belongs to the protocol session, not to any candidate target. Keep
  // a bounded causal history and replace an available current sample with a robust local trend.
  // The Theil-Sen slope and median intercept tolerate isolated receiver-local CFO failures. Its
  // uncertainty is never divided by sample count because adjacent CPIs are correlated.
  for (auto& [session, session_batches] : ul_by_session) {
    auto& receiver_histories = ul_direct_rate_history_[session];
    for (auto& batch : session_batches) {
      auto& history = receiver_histories[batch.receiver_index];
      while (!history.empty()
             && time - history.front().time_s > config_.maximum_propagation_s)
        history.pop_front();
      if (!(batch.observable && batch.same_pusch_reference
            && batch.direct_path_rate_valid))
        continue; // Missing direct UL remains missing; history never fabricates a current sample.
      if (!(std::isfinite(batch.direct_path_range_rate_mps)
            && std::isfinite(batch.direct_path_rate_variance_mps2)
            && batch.direct_path_rate_variance_mps2 > 0.0)) {
        batch.direct_path_rate_valid = false;
        continue;
      }
      history.push_back({time, batch.direct_path_range_rate_mps,
                         batch.direct_path_rate_variance_mps2});
      if (history.size() < config_.birth_minimum_receivers) continue;
      std::vector<double> slopes;
      for (size_t left = 0; left < history.size(); ++left)
        for (size_t right = left + 1; right < history.size(); ++right) {
          const double dt = history[right].time_s - history[left].time_s;
          if (dt > 0.0)
            slopes.push_back((history[right].range_rate_mps
                              - history[left].range_rate_mps) / dt);
        }
      if (slopes.empty()) continue;
      const double slope = sample_median(std::move(slopes));
      std::vector<double> intercepts, variances;
      intercepts.reserve(history.size());
      variances.reserve(history.size());
      for (const UeDirectRateSample& sample : history) {
        intercepts.push_back(sample.range_rate_mps
                             + slope * (time - sample.time_s));
        variances.push_back(sample.variance_mps2);
      }
      const double estimate = sample_median(intercepts);
      std::vector<double> absolute_residuals;
      absolute_residuals.reserve(intercepts.size());
      for (double value : intercepts)
        absolute_residuals.push_back(std::abs(value - estimate));
      const double mad = sample_median(std::move(absolute_residuals));
      // 0.67448975 is the standard-normal median absolute deviation, not an empirical gate.
      const double robust_variance = std::pow(mad / 0.6744897501960817, 2.0);
      const double variance = sample_median(std::move(variances)) + robust_variance;
      if (std::isfinite(estimate) && std::isfinite(variance) && variance > 0.0) {
        batch.direct_path_range_rate_mps = estimate;
        batch.direct_path_rate_variance_mps2 = variance;
      }
    }
  }
  for (auto session = ul_session_last_seen_s_.begin();
       session != ul_session_last_seen_s_.end();) {
    if (time - session->second > config_.maximum_propagation_s) {
      ul_validators_.erase(session->first);
      ul_bootstrap_history_.erase(session->first);
      ul_bootstrap_target_ids_.erase(session->first);
      ul_direct_rate_history_.erase(session->first);
      session = ul_session_last_seen_s_.erase(session);
    } else {
      ++session;
    }
  }
  last_processing_stats_.ul_session_count = ul_by_session.size();
  std::map<uint64_t, bool> ul_established_before;
  for (const auto& [session, ignored] : ul_by_session) {
    (void)ignored;
    const auto found = ul_validators_.find(session);
    ul_established_before[session] = found != ul_validators_.end()
                                       && found->second.has_causal_ue_state();
    if (ul_established_before[session])
      ++last_processing_stats_.ul_established_session_count;
  }
  auto remaining_processing_budget = [&] {
    // UL is a separate illuminator work queue. A wall-clock cutoff inside its combinatorial loop
    // made the learned UE state depend on host load and replay order. Admit the CPI based on the
    // deterministic upstream budget, then finish the admitted UL unit atomically; measured total
    // elapsed time remains reported so deployments can size/parallelize workers honestly.
    // The caller's value is the DL/output deadline, not permission to discard the second
    // illuminator. UL is an admitted auxiliary work item and completes deterministically even when
    // the DL front end has consumed the 75 ms latency budget. Its elapsed time is still included in
    // processing diagnostics so an OTA deployment can provision/pipeline that worker honestly.
    return std::numeric_limits<double>::infinity();
  };
  auto account_ul_work = [&](const UlDtdDfsValidation& validation) {
    last_processing_stats_.ul_training_hypotheses = saturating_add(
        last_processing_stats_.ul_training_hypotheses,
        validation.training_hypotheses);
    last_processing_stats_.ul_fitted_ue_states = saturating_add(
        last_processing_stats_.ul_fitted_ue_states,
        validation.fitted_ue_states);
    if (validation.processing_deadline_exhausted)
      ++last_processing_stats_.ul_processing_deferred;
  };

  std::set<std::pair<uint32_t, size_t>> used;
  std::vector<std::vector<Observation>> assigned(tracks_.size());
  // Globally minimize normalized innovation cost independently for each receiver. A track can
  // consume one measurement from every available receiver; a receiver can update every track once.
  // The assignment has no ordering/score preference and therefore cannot let an early greedy pair
  // steal the only feasible measurement of another track.
  uint64_t association_hypotheses = 0;
  for (const auto& receiver_observations : observations) {
    const uint64_t count = receiver_observations.size() >
            std::numeric_limits<uint64_t>::max() / std::max<size_t>(1, tracks_.size())
        ? std::numeric_limits<uint64_t>::max()
        : tracks_.size() * receiver_observations.size();
    association_hypotheses = count > std::numeric_limits<uint64_t>::max()
                                      - association_hypotheses
        ? std::numeric_limits<uint64_t>::max() : association_hypotheses + count;
  }

  last_processing_stats_.association_hypotheses = association_hypotheses;
  const double association_gate = adaptive_chi2_2d_gate(
      config_.false_object_intensity_per_s, represented_s, association_hypotheses);
  for (const auto& receiver_observations : observations) {
    const double rejected_cost = association_gate
        * (tracks_.size() + receiver_observations.size() + 1.0) + 1.0;
    Matrix costs(tracks_.size(), receiver_observations.size(), rejected_cost);
    for (size_t track = 0; track < tracks_.size(); ++track)
      for (size_t detection = 0; detection < receiver_observations.size(); ++detection) {
        const double nis = tracks_[track].innovation_nis(receiver_observations[detection]);
        if (std::isfinite(nis) && nis <= association_gate)
          costs(track, detection) = nis;
      }
    for (const auto& [track, detection] : minimum_cost_assignment(costs)) {
      if (costs(track, detection) > association_gate) continue;
      const Observation& observation = receiver_observations[detection];
      assigned[track].push_back(observation);
    }
  }

  // Freeze all pre-update innovations before any receiver sequentially modifies a track. This is
  // essential: otherwise receiver ordering itself would manufacture cross-RX correlation.
  for (size_t index = 0; index < tracks_.size(); ++index)
    tracks_[index].observe_origin_epoch(assigned[index]);

  // Track exclusivity compares a shared-motion explanation with two independent velocities. The
  // three-parameter BIC penalty makes the test identifiable after more than three overlapping
  // three-receiver CPIs. Evidence is attached only to the weaker candidate; this stage reports it
  // but does not suppress either track.
  for (size_t left = 0; left < tracks_.size(); ++left) {
    if (assigned[left].size() < config_.birth_minimum_receivers) continue;
    for (size_t right = left + 1; right < tracks_.size(); ++right) {
      if (assigned[right].size() < config_.birth_minimum_receivers) continue;
      const std::pair<uint64_t, uint64_t> key{
          std::min(tracks_[left].id, tracks_[right].id),
          std::max(tracks_[left].id, tracks_[right].id)};
      const uint64_t overlap = ++exclusivity_overlap_epochs_[key];
      if (overlap <= 3) continue;
      Matrix covariance(3, 3);
      std::vector<double> difference(3);
      for (size_t row = 0; row < 3; ++row) {
        difference[row] = tracks_[left].state[row + 3] - tracks_[right].state[row + 3];
        for (size_t column = 0; column < 3; ++column)
          covariance(row, column) = tracks_[left].covariance(row + 3, column + 3)
                                    + tracks_[right].covariance(row + 3, column + 3);
      }
      try {
        const double wald = quadratic(difference, pseudoinverse_symmetric(
            positive_semidefinite(symmetrized(covariance), 1e-12), 1e-12));
        const double log_bayes_factor = 0.5 * (
            3.0 * std::log(static_cast<double>(overlap)) - wald);
        if (!(std::isfinite(log_bayes_factor) && log_bayes_factor > 0.0)) continue;
        const double left_strength = tracks_[left].direct_vs_clutter_log_evidence();
        const double right_strength = tracks_[right].direct_vs_clutter_log_evidence();
        const size_t weaker = left_strength < right_strength ? left
            : (right_strength < left_strength ? right
                                              : (tracks_[left].id > tracks_[right].id
                                                     ? left : right));
        const size_t stronger = weaker == left ? right : left;
        tracks_[weaker].origin.apply_exclusivity_evidence(
            log_bayes_factor, tracks_[stronger].id);
      } catch (...) {}
    }
  }

  // Learn a wall only from a repeated relationship between two complete causal track hypotheses.
  // A plane remains provisional after its first CPI and cannot suppress that candidate.  BIC then
  // compares the shared three-parameter reflector with a second six-state target trajectory.
  for (size_t left = 0; left < tracks_.size(); ++left) {
    if (assigned[left].size() < config_.birth_minimum_receivers
        || tracks_[left].reflected_path)
      continue;
    for (size_t right = left + 1; right < tracks_.size(); ++right) {
      if (assigned[right].size() < config_.birth_minimum_receivers
          || tracks_[right].reflected_path)
        continue;
      const double left_evidence = tracks_[left].direct_vs_clutter_log_evidence();
      const double right_evidence = tracks_[right].direct_vs_clutter_log_evidence();
      const size_t parent = left_evidence > right_evidence ? left
          : (right_evidence > left_evidence ? right
                                            : (tracks_[left].id < tracks_[right].id
                                                   ? left : right));
      const size_t path = parent == left ? right : left;
      std::vector<ReflectorPathMeasurement> measurements;
      measurements.reserve(assigned[path].size());
      for (const Observation& observation : assigned[path]) {
        ReflectorPathMeasurement value;
        value.time_s = time;
        value.receiver_index = observation.receiver;
        value.geometry = observation.geometry;
        value.detection = observation.value;
        measurements.push_back(std::move(value));
      }
      ++last_processing_stats_.reflector_pair_updates;
      const ReflectorPairDecision decision = reflector_bank_.observe_pair(
          sequence, tracks_[parent].id, tracks_[path].id,
          tracks_[parent].target_state(), tracks_[path].target_state(), measurements);
      if (decision.evidence_ready && decision.shared_reflector_preferred
          && decision.reflector_id != 0) {
        tracks_[path].reflected_path = true;
        tracks_[path].shared_reflector_id = decision.reflector_id;
        tracks_[path].physical_parent_track_id = tracks_[parent].id;
        tracks_[path].reflected_predictive_bic = decision.shared_reflector_bic;
        tracks_[path].direct_predictive_bic = decision.independent_target_bic;
      }
    }
  }
  last_processing_stats_.admitted_reflector_planes = std::count_if(
      reflector_bank_.planes().begin(), reflector_bank_.planes().end(),
      [](const ReflectorPlane& plane) { return plane.admitted; });
  // OFFLINE DIAGNOSTIC: surface the best-supported provisional plane even though it cannot be
  // admitted here (see the field comments in the header). "Best" = most supporting pairs, tie-broken
  // by lowest BIC.
  {
    const ReflectorPlane* best = nullptr;
    for (const ReflectorPlane& plane : reflector_bank_.planes()) {
      if (!best || plane.supporting_pairs > best->supporting_pairs
          || (plane.supporting_pairs == best->supporting_pairs && plane.bic < best->bic))
        best = &plane;
    }
    if (best) {
      last_processing_stats_.best_provisional_plane_valid = true;
      last_processing_stats_.best_provisional_plane_bic = best->bic;
      last_processing_stats_.best_provisional_plane_offset_m = best->offset_m;
      last_processing_stats_.best_provisional_plane_normal = {
          best->normal.x, best->normal.y, best->normal.z};
      last_processing_stats_.best_provisional_plane_supporting_pairs = best->supporting_pairs;
      last_processing_stats_.best_provisional_plane_supporting_parent_tracks =
          best->supporting_parent_tracks;
    }
  }

  // Reuse admitted planes as association hypotheses for already known physical tracks. Reflected
  // children consume their measurements but never update target kinematics or create new births.
  std::set<std::pair<uint32_t, size_t>> directly_reserved;
  for (const auto& per_track : assigned)
    for (const Observation& observation : per_track)
      directly_reserved.emplace(observation.receiver, observation.detection);
  for (size_t track_index = 0; track_index < tracks_.size(); ++track_index) {
    if (tracks_[track_index].reflected_path) continue;
    for (size_t receiver = 0; receiver < observations.size(); ++receiver) {
      const Observation* best = nullptr;
      double best_nis = std::numeric_limits<double>::infinity();
      for (const ReflectorPlane& plane : reflector_bank_.planes()) {
        if (!plane.admitted) continue;
        for (const Observation& candidate : observations[receiver]) {
          if (directly_reserved.count({candidate.receiver, candidate.detection})
              || used.count({candidate.receiver, candidate.detection}))
            continue;
          const Observation reflected = reflected_observation(candidate, plane);
          const double nis = tracks_[track_index].innovation_nis(reflected);
          if (std::isfinite(nis) && nis <= association_gate && nis < best_nis) {
            best_nis = nis;
            best = &candidate;
          }
        }
      }
      if (best) {
        used.emplace(best->receiver, best->detection);
        ++last_processing_stats_.reflected_path_assignments;
      }
    }
  }

  // A shared UE state may only be bootstrapped by an already-confirmed temporal DL track. Birth
  // candidates never fit their own nuisance UE. Receiver-held-out proposals are retained once per
  // session/CPI. The best multiplicity-corrected receiver-held-out proposal becomes a session-
  // level provisional state immediately, but it cannot affect any target. It must predict two
  // distinct later PUSCH time blocks before promotion. This tests the shared trajectory directly
  // in measurement space instead of comparing unstable per-CPI Cartesian roots along the DTD
  // ambiguity manifold.
  // This expensive nuisance search is deliberately deferred until all DL association and birth
  // work is complete, so unavailable UL can never change DL output by consuming its deadline.
  auto bootstrap_ul_sessions = [&] {
   for (const auto& [session, session_batches] : ul_by_session) {
    if (ul_established_before[session]) continue;
    std::vector<size_t> trusted;
    for (size_t index = 0; index < tracks_.size(); ++index) {
      if (!tracks_[index].confirmed
          || tracks_[index].total_updates <= 2
          || tracks_[index].direct_supported_receiver_count()
                 < config_.birth_minimum_receivers)
        continue;
      trusted.push_back(index);
    }
    if (trusted.empty()) continue;
    auto& validator = ul_validators_[session];
    struct UeProposal {
      UlDtdDfsValidation validation;
      size_t track_index = 0;
    };
    std::vector<UeProposal> ue_proposals;
    std::optional<UlDtdDfsValidation> best_validation;
    size_t inconsistent_proposals = 0;
    for (size_t index : trusted) {
      ++last_processing_stats_.ul_ue_bootstrap_attempts;
      UlDtdDfsValidation validation = validator.evaluate(
          time, tracks_[index].target_state(), tracks_[index].target_covariance(),
          session_batches, remaining_processing_budget());
      account_ul_work(validation);
      if (validation.reason == "causal_ue_bootstrap_only")
        ++last_processing_stats_.ul_ue_crossvalidated_proposals;
      else if (validation.reason == "causal_ue_bootstrap_confirmed")
        ++last_processing_stats_.ul_ue_causal_confirmations;
      else if (validation.reason == "provisional_ue_inconsistent") {
        ++last_processing_stats_.ul_ue_inconsistent_proposals;
        ++inconsistent_proposals;
      }
      else
        ++last_processing_stats_.ul_ue_bootstrap_unavailable;
      if (!validation.ue_state_valid || !validation.cross_fold_ue_consistent
          || !(validation.log_bayes_factor > 0.0))
        continue;
      // We selected one proposal after testing the measured set of confirmed DL tracks. Charge
      // that multiplicity before a target is allowed to define or confirm the one shared session.
      validation.log_bayes_factor -= std::log(static_cast<double>(trusted.size()));
      if (!(validation.log_bayes_factor > 0.0)) {
        ++last_processing_stats_.ul_ue_multiplicity_rejections;
        continue;
      }
      ue_proposals.push_back({std::move(validation), index});
    }
    if (validator.has_provisional_ue_state()) {
      if (!ue_proposals.empty()) {
        const auto best = std::max_element(
            ue_proposals.begin(), ue_proposals.end(), [](const auto& left, const auto& right) {
              return left.validation.log_bayes_factor < right.validation.log_bayes_factor;
            });
        best_validation = best->validation;
        auto& target_ids = ul_bootstrap_target_ids_[session];
        target_ids.insert(tracks_[best->track_index].id);
        best_validation->session_establishment_eligible =
            best_validation->direct_session_anchor_valid
            || target_ids.size() >= 2 || ul_by_session.size() >= 3;
      }
      if (best_validation)
        validator.commit(time, *best_validation, 0.0);
      else if (inconsistent_proposals == trusted.size()) {
        // Reset only when every available target gives complete negative evidence. One local UL
        // miss or ambiguous target is neutral and cannot destroy a session bootstrap.
        validator.reset();
        ul_bootstrap_history_[session].clear();
        ul_bootstrap_target_ids_[session].clear();
      }
    } else {
      if (ue_proposals.empty()) continue;
      const auto current = std::max_element(
          ue_proposals.begin(), ue_proposals.end(), [](const auto& left, const auto& right) {
            return left.validation.log_bayes_factor < right.validation.log_bayes_factor;
          });
      auto& history = ul_bootstrap_history_[session];
      while (!history.empty()
             && time - history.front().time_s > config_.maximum_propagation_s)
        history.pop_front();
      best_validation = current->validation;
      best_validation->reason = "causal_ue_bootstrap_only";
      auto& target_ids = ul_bootstrap_target_ids_[session];
      target_ids.insert(tracks_[current->track_index].id);
      best_validation->session_establishment_eligible =
          best_validation->direct_session_anchor_valid
          || target_ids.size() >= 2 || ul_by_session.size() >= 3;
      validator.commit(time, *best_validation, 0.0);
      history.push_back({current->validation.ue_state_time_s,
                         tracks_[current->track_index].id,
                         current->validation});
    }
    if (validator.has_causal_ue_state()) {
      ++last_processing_stats_.ul_ue_bootstrap_promotions;
      ul_bootstrap_history_[session].clear();
      ul_bootstrap_target_ids_[session].clear();
    }
   }
  };

  // Validate every existing target against every UE session that was already causal before this
  // CPI. UL updates origin/path-model evidence only; it never edits kinematics or directly retires
  // an established DL track. Missing or ambiguous UL is neutral. Contradiction remains actionable
  // for a new birth, while established paths need the shared direct-vs-reflector comparison.
  for (size_t index = 0; index < tracks_.size(); ++index) {
    std::vector<std::optional<double>> direct_dl_folds;
    std::map<uint64_t, std::vector<std::optional<double>>> reflected_dl_folds;
    for (const auto& batch : batches) {
      const std::vector<Observation> no_proposals;
      const auto& receiver_proposals = batch.receiver_index < observations.size()
          ? observations[batch.receiver_index] : no_proposals;
      const auto direct = target_proposal_evidence(
          tracks_[index].target_state(), tracks_[index].target_covariance(), batch,
          receiver_proposals, config_, nullptr);
      tracks_[index].observe_direct_receiver(batch.receiver_index, direct);
      ++last_processing_stats_.soft_map_queries;
      direct_dl_folds.push_back(direct.available
                                    ? std::optional<double>(direct.log_bayes_factor)
                                    : std::nullopt);
      for (const ReflectorPlane& plane : reflector_bank_.planes()) {
        if (!plane.admitted) continue;
        const auto reflected = target_proposal_evidence(
            tracks_[index].target_state(), tracks_[index].target_covariance(), batch,
            receiver_proposals, config_, &plane);
        ++last_processing_stats_.soft_map_queries;
        reflected_dl_folds[plane.id].push_back(
            reflected.available ? std::optional<double>(reflected.log_bayes_factor)
                                : std::nullopt);
      }
    }
    const RotatingPredictiveScore direct_dl = predictive_bic_from_correlated_folds(
        direct_dl_folds, 0, 2 * direct_dl_folds.size());
    tracks_[index].observe_direct_validation(direct_dl);
    std::map<uint64_t, RotatingPredictiveScore> reflected_dl;
    for (const auto& [plane_id, values] : reflected_dl_folds)
      reflected_dl[plane_id] = predictive_bic_from_correlated_folds(
          values, 0, 2 * values.size());

    std::vector<double> support_scores;
    std::vector<double> contradiction_scores;
    std::vector<std::optional<double>> direct_ul_model_scores;
    std::map<uint64_t, std::vector<std::optional<double>>> reflected_ul_model_scores;
    bool anchored_ul_decision = false;
    for (const auto& [session, session_batches] : ul_by_session) {
      if (!ul_established_before[session]) continue;
      auto found = ul_validators_.find(session);
      if (found == ul_validators_.end()) continue;
      ++last_processing_stats_.ul_validation_attempts;
      ++last_processing_stats_.ul_track_validation_attempts;
      const UlDtdDfsValidation validation = found->second.evaluate(
          time, tracks_[index].target_state(), tracks_[index].target_covariance(),
          session_batches, remaining_processing_budget());
      account_ul_work(validation);
      if (validation.evaluated_folds >= config_.birth_minimum_receivers
          && std::isfinite(validation.log_bayes_factor))
        direct_ul_model_scores.push_back(validation.log_bayes_factor);
      if (validation.decision == UlDifferentialDecision::support) {
        ++last_processing_stats_.ul_supported_track_updates;
        support_scores.push_back(validation.log_bayes_factor);
        anchored_ul_decision = anchored_ul_decision
                               || found->second.has_direct_session_anchor();
      } else if (validation.decision == UlDifferentialDecision::contradiction) {
        ++last_processing_stats_.ul_contradicted_track_updates;
        contradiction_scores.push_back(validation.log_bayes_factor);
        anchored_ul_decision = anchored_ul_decision
                               || found->second.has_direct_session_anchor();
      } else {
        ++last_processing_stats_.ul_unavailable_track_updates;
      }
      // The direct call above and every reflected call use the same established per-session UE
      // state. A wall is never allowed to manufacture its own UE nuisance state.
      for (const ReflectorPlane& plane : reflector_bank_.planes()) {
        if (!plane.admitted) continue;
        const UlDtdDfsValidation reflected = found->second.evaluate_reflected(
            time, tracks_[index].target_state(), tracks_[index].target_covariance(),
            session_batches, plane, remaining_processing_budget());
        if (reflected.evaluated_folds >= config_.birth_minimum_receivers
            && std::isfinite(reflected.log_bayes_factor))
          reflected_ul_model_scores[plane.id].push_back(
              reflected.log_bayes_factor);
      }
    }
    UlDtdDfsValidation aggregate;
    std::vector<double> independent_illuminator_scores = support_scores;
    independent_illuminator_scores.insert(independent_illuminator_scores.end(),
                                          contradiction_scores.begin(),
                                          contradiction_scores.end());
    if (!independent_illuminator_scores.empty()) {
      std::sort(independent_illuminator_scores.begin(),
                independent_illuminator_scores.end());
      const size_t middle = independent_illuminator_scores.size() / 2;
      aggregate.log_bayes_factor = independent_illuminator_scores.size() % 2
          ? independent_illuminator_scores[middle]
          : 0.5 * (independent_illuminator_scores[middle - 1]
                   + independent_illuminator_scores[middle]);
      // Each UE is an independent illuminator, but all sessions share the same DL target state.
      // A strict majority prevents one convenient UE from overruling contradictory illuminators;
      // a tie is ambiguous/neutral. Receiver folds inside each session were already aggregated.
      const bool actionable = anchored_ul_decision
                              || independent_illuminator_scores.size() >= 3;
      if (actionable
          && support_scores.size() > independent_illuminator_scores.size() / 2
          && aggregate.log_bayes_factor > 0.0)
        aggregate.decision = UlDifferentialDecision::support;
      else if (actionable
               && contradiction_scores.size() > independent_illuminator_scores.size() / 2
               && aggregate.log_bayes_factor < 0.0)
        aggregate.decision = UlDifferentialDecision::contradiction;
    }
    // A single target-derived, no-TA UE may be useful as a provisional nuisance model but must
    // not change direct-vs-reflected target selection. Genuine TA or three independent protocol
    // sessions provide the required target-independent anchor.
    if (!anchored_ul_decision && independent_illuminator_scores.size() < 3) {
      direct_ul_model_scores.clear();
      reflected_ul_model_scores.clear();
    }
    const RotatingPredictiveScore direct_ul = predictive_bic_from_correlated_folds(
        direct_ul_model_scores, 0, 2 * direct_ul_model_scores.size());
    tracks_[index].direct_predictive_bic = std::numeric_limits<double>::infinity();
    if (direct_dl.available || direct_ul.available) {
      double total = 0.0;
      uint32_t views = 0;
      if (direct_dl.available) total += direct_dl.bic, ++views;
      if (direct_ul.available) total += direct_ul.bic, ++views;
      tracks_[index].direct_predictive_bic = total / views;
    }
    double best_reflected_bic = std::numeric_limits<double>::infinity();
    for (const ReflectorPlane& plane : reflector_bank_.planes()) {
      if (!plane.admitted) continue;
      const auto dl = reflected_dl.find(plane.id);
      const auto ul_values = reflected_ul_model_scores.find(plane.id);
      RotatingPredictiveScore ul;
      if (ul_values != reflected_ul_model_scores.end())
        ul = predictive_bic_from_correlated_folds(
            ul_values->second, 0, 2 * ul_values->second.size());
      double total = 0.0;
      uint32_t views = 0;
      if (dl != reflected_dl.end() && dl->second.available)
        total += dl->second.bic, ++views;
      if (ul.available) total += ul.bic, ++views;
      if (!views) continue;
      // Selecting among an observed bank of walls is charged once. No receiver products or
      // repeated UE scores are multiplied.
      const double candidate_bic = total / views
          + 2.0 * std::log(static_cast<double>(
                std::max<size_t>(1, reflector_bank_.planes().size())));
      best_reflected_bic = std::min(best_reflected_bic, candidate_bic);
    }
    tracks_[index].reflected_predictive_bic = best_reflected_bic;
    tracks_[index].observe_path_model_comparison(
        tracks_[index].direct_predictive_bic,
        tracks_[index].reflected_predictive_bic);
    tracks_[index].apply_ul_validation(aggregate);
    if (tracks_[index].reflected_path) {
      // The measurements have already contributed to the shared environmental hypothesis. They
      // must not create another target or pull the physical IMM toward a virtual path.
      for (const Observation& observation : assigned[index])
        used.emplace(observation.receiver, observation.detection);
      continue;
    }
    for (const Observation& observation : assigned[index])
      if (tracks_[index].update_measurement(observation))
        used.emplace(observation.receiver, observation.detection);
  }

  for (Track& track : tracks_) {
    track.source_sequence = sequence;
    track.finish_epoch(represented_s);
  }
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
                  if (track.reflected_path
                      || track.existence_log_evidence < 0.0
                      || track.coasts * represented_s > config_.maximum_propagation_s
                      || norm(Vec3{track.state[3], track.state[4], track.state[5]})
                           > config_.maximum_target_speed_mps)
                    return true;
                  for (const BistaticGeometry& geometry : geometries_) {
                    try {
                      const auto value = measurement_model(track.state, geometry);
                      if (!std::isfinite(value[0]) || value[0] < 0.0
                          || value[0] > config_.maximum_range_m)
                        return true;
                    } catch (...) { return true; }
                  }
                  return false;
                }), tracks_.end());
  // Track IDs are never reused, so retaining pair histories after either endpoint is retired would
  // create an unbounded OTA-service memory leak under sustained clutter births.
  std::set<uint64_t> active_track_ids;
  for (const Track& track : tracks_) active_track_ids.insert(track.id);
  reflector_bank_.retain_tracks(active_track_ids);
  for (auto history = exclusivity_overlap_epochs_.begin();
       history != exclusivity_overlap_epochs_.end();) {
    if (!active_track_ids.count(history->first.first)
        || !active_track_ids.count(history->first.second))
      history = exclusivity_overlap_epochs_.erase(history);
    else
      ++history;
  }

  auto save_birth_history = [&] {
    birth_history_->valid = true;
    birth_history_->time_s = time;
    birth_history_->sequence = sequence;
    birth_history_->observations.assign(observations.size(), {});
    birth_history_->soft_surfaces.assign(observations.size(), {});
    for (const auto& batch : batches)
      if (batch.receiver_index < birth_history_->soft_surfaces.size())
        birth_history_->soft_surfaces[batch.receiver_index] = batch.soft_evidence;
    for (size_t receiver = 0; receiver < observations.size(); ++receiver)
      for (const Observation& value : observations[receiver])
        if (!used.count({value.receiver, value.detection}))
          birth_history_->observations[receiver].push_back(value);
  };

  // A single CPI can never create a 3-D track.  It only freezes unused, independently detected
  // observations for the next causal epoch.
  if (!birth_history_->valid || !(time > birth_history_->time_s)
      || time - birth_history_->time_s > config_.maximum_propagation_s) {
    save_birth_history();
    bootstrap_ul_sessions();
    finish_processing();
    return;
  }
  const double birth_dt = time - birth_history_->time_s;
  uint64_t tracklet_hypotheses = 0;
  for (size_t receiver = 0; receiver < observations.size(); ++receiver) {
    const size_t before = birth_history_->observations[receiver].size();
    const size_t now = observations[receiver].size();
    const uint64_t count = now && before > std::numeric_limits<uint64_t>::max() / now
        ? std::numeric_limits<uint64_t>::max() : before * now;
    tracklet_hypotheses = count > std::numeric_limits<uint64_t>::max()
                                   - tracklet_hypotheses
        ? std::numeric_limits<uint64_t>::max() : tracklet_hypotheses + count;
  }
  last_processing_stats_.tracklet_hypotheses = tracklet_hypotheses;
  const double tracklet_gate = adaptive_chi2_1d_gate(
      config_.false_object_intensity_per_s, birth_dt, tracklet_hypotheses);
  const double geometric_gate = adaptive_chi2_2d_gate(
      config_.false_object_intensity_per_s, birth_dt, tracklet_hypotheses);
  std::vector<std::vector<ReceiverTracklet>> tracklets(observations.size());
  for (size_t receiver = 0; receiver < observations.size(); ++receiver) {
    const auto& previous = birth_history_->observations[receiver];
    const auto& current = observations[receiver];
    const double rejected = tracklet_gate * (previous.size() + current.size() + 1.0) + 1.0;
    Matrix costs(previous.size(), current.size(), rejected);
    for (size_t before = 0; before < previous.size(); ++before)
      for (size_t now = 0; now < current.size(); ++now) {
        if (used.count({current[now].receiver, current[now].detection})) continue;
        const double range_change = current[now].value.range_m
                                    - previous[before].value.range_m;
        const bool rate_informative = current[now].rate_informative
                                      && previous[before].rate_informative;
        const double residual = rate_informative
            ? range_change - 0.5 * birth_dt * (current[now].value.range_rate_mps
                                               + previous[before].value.range_rate_mps)
            : range_change;
        const Matrix& current_noise = current[now].noise;
        const Matrix& previous_noise = previous[before].noise;
        double variance = current_noise(0, 0) + previous_noise(0, 0);
        if (rate_informative) {
          variance += 0.25 * birth_dt * birth_dt
              * (current_noise(1, 1) + previous_noise(1, 1))
              - birth_dt * current_noise(0, 1) + birth_dt * previous_noise(0, 1);
        } else {
          // With unidentifiable Doppler, marginalize the unknown bistatic rate over its declared
          // physical support. This is causal and avoids both a resolution-floor fiction and a
          // scenario-tuned continuity window.
          const double maximum_change = 2.0 * config_.maximum_target_speed_mps * birth_dt;
          variance += maximum_change * maximum_change / 3.0;
          if (std::abs(range_change) > maximum_change
                                           + std::sqrt(tracklet_gate * variance))
            continue;
        }
        if (!(variance > 0.0)) continue;
        const double chi2 = residual * residual / variance;
        if (std::isfinite(chi2) && chi2 <= tracklet_gate) costs(before, now) = chi2;
      }
    for (const auto& [before, now] : minimum_cost_assignment(costs))
      if (costs(before, now) <= tracklet_gate)
        tracklets[receiver].push_back({previous[before], current[now], costs(before, now)});
    // OUR ADAPTATION: a tracklet whose current-CPI detection is tagged as a probable micro-Doppler
    // sideband (bulk_component_iteration>=0) is dropped only when its own bulk sibling ALSO has a
    // valid tracklet this cycle AND the Doppler offset between them is statistically the same in the
    // previous CPI as in the current one. A one-off same-range-cell tag is not enough by itself --
    // this requires the offset to have PERSISTED across two consecutive, independently continuity-
    // validated frames, consistent with a genuinely rotating part (roughly constant angular rate)
    // rather than a coincidental single-CPI proximity between two unrelated detections. This is
    // deliberately more conservative than an earlier, reverted range-proximity-only version, which
    // suppressed unconditionally on a single CPI and measurably hurt classes with weak/no real
    // micro-Doppler.
    {
      std::map<int32_t, const ReceiverTracklet*> tracklet_by_current_iteration;
      for (const auto& t : tracklets[receiver])
        tracklet_by_current_iteration[static_cast<int32_t>(t.current.value.source_component_iteration)] = &t;
      std::vector<ReceiverTracklet> filtered;
      filtered.reserve(tracklets[receiver].size());
      for (const auto& t : tracklets[receiver]) {
        bool suppress = false;
        if (t.current.value.bulk_component_iteration >= 0) {
          const auto it = tracklet_by_current_iteration.find(t.current.value.bulk_component_iteration);
          if (it != tracklet_by_current_iteration.end()) {
            ++last_processing_stats_.micro_doppler_sideband_candidates;
            const ReceiverTracklet& bulk = *it->second;
            const double curr_offset = t.current.value.range_rate_mps - bulk.current.value.range_rate_mps;
            const double prev_offset = t.previous.value.range_rate_mps - bulk.previous.value.range_rate_mps;
            const double variance = t.current.noise(1, 1) + t.previous.noise(1, 1)
                                    + bulk.current.noise(1, 1) + bulk.previous.noise(1, 1);
            if (variance > 0.0) {
              const double delta = curr_offset - prev_offset;
              const double chi2 = delta * delta / variance;
              if (std::isfinite(chi2) && chi2 <= tracklet_gate) suppress = true;
            }
          }
        }
        if (suppress) ++last_processing_stats_.micro_doppler_sideband_suppressed;
        if (!suppress) filtered.push_back(t);
      }
      tracklets[receiver] = std::move(filtered);
    }
    std::sort(tracklets[receiver].begin(), tracklets[receiver].end(),
              [](const ReceiverTracklet& left, const ReceiverTracklet& right) {
      const double left_evidence = std::min(left.previous.value.score,
                                            left.current.value.score);
      const double right_evidence = std::min(right.previous.value.score,
                                             right.current.value.score);
      return std::tuple(left.continuity_chi2, -left_evidence,
                        left.current.value.range_m, left.current.value.range_rate_mps)
           < std::tuple(right.continuity_chi2, -right_evidence,
                        right.current.value.range_m, right.current.value.range_rate_mps);
    });
  }

  std::vector<size_t> usable_receivers;
  for (size_t receiver = 0; receiver < tracklets.size(); ++receiver)
    if (!tracklets[receiver].empty()) usable_receivers.push_back(receiver);
  if (usable_receivers.size() < config_.birth_minimum_receivers) {
    save_birth_history();
    bootstrap_ul_sessions();
    finish_processing();
    return;
  }

  std::vector<BirthSolution> births;
  struct ReceiverTriple { std::array<size_t, 3> receiver; };
  struct BirthQueueNode {
    size_t triple = 0;
    std::array<size_t, 3> index{};
    double continuity = 0.0;
    double negative_evidence = 0.0;
  };
  auto queue_order = [](const BirthQueueNode& left, const BirthQueueNode& right) {
    return std::tuple(left.continuity, left.negative_evidence, left.triple, left.index)
           > std::tuple(right.continuity, right.negative_evidence, right.triple, right.index);
  };
  std::vector<ReceiverTriple> triples;
  for (size_t ai = 0; ai + 2 < usable_receivers.size(); ++ai)
    for (size_t bi = ai + 1; bi + 1 < usable_receivers.size(); ++bi)
      for (size_t ci = bi + 1; ci < usable_receivers.size(); ++ci)
        triples.push_back({{usable_receivers[ai], usable_receivers[bi],
                            usable_receivers[ci]}});
  std::priority_queue<BirthQueueNode, std::vector<BirthQueueNode>, decltype(queue_order)>
      birth_queue(queue_order);
  std::vector<std::set<std::array<size_t, 3>>> queued(triples.size());
  auto enqueue = [&](size_t triple_index, std::array<size_t, 3> index) {
    const auto& triple = triples[triple_index].receiver;
    for (size_t axis = 0; axis < 3; ++axis)
      if (index[axis] >= tracklets[triple[axis]].size()) return;
    if (!queued[triple_index].insert(index).second) return;
    double continuity = 0.0, negative_evidence = 0.0;
    for (size_t axis = 0; axis < 3; ++axis) {
      const ReceiverTracklet& value = tracklets[triple[axis]][index[axis]];
      continuity += value.continuity_chi2;
      negative_evidence -= std::log(std::max(
          std::numeric_limits<double>::min(),
          std::min(value.previous.value.score, value.current.value.score)));
    }
    birth_queue.push({triple_index, index, continuity, negative_evidence});
  };
  for (size_t triple = 0; triple < triples.size(); ++triple) {
    enqueue(triple, {0, 0, 0});
    uint64_t count = 1;
    for (size_t receiver : triples[triple].receiver) {
      const uint64_t size = tracklets[receiver].size();
      count = size && count > std::numeric_limits<uint64_t>::max() / size
          ? std::numeric_limits<uint64_t>::max() : count * size;
    }
    last_processing_stats_.birth_tuple_hypotheses =
        count > std::numeric_limits<uint64_t>::max()
                    - last_processing_stats_.birth_tuple_hypotheses
        ? std::numeric_limits<uint64_t>::max()
        : last_processing_stats_.birth_tuple_hypotheses + count;
  }

  // Finish the causally admitted CPI atomically. Stopping this deterministic best-first search on
  // host wall time made track states depend on scheduler load. Runtime overruns are still measured
  // and reported; the real-time wrapper can queue/drop a later CPI without changing this CPI's
  // mathematical result.
  while (!birth_queue.empty()) {
    const BirthQueueNode node = birth_queue.top();
    birth_queue.pop();
    ++last_processing_stats_.birth_queue_pops;
    for (size_t axis = 0; axis < 3; ++axis) {
      auto neighbor = node.index;
      ++neighbor[axis];
      enqueue(node.triple, neighbor);
    }
    const auto& selected = triples[node.triple].receiver;
    const ReceiverTracklet& a = tracklets[selected[0]][node.index[0]];
    const ReceiverTracklet& b = tracklets[selected[1]][node.index[1]];
    const ReceiverTracklet& c = tracklets[selected[2]][node.index[2]];
    if (!pairwise_range_feasible(a.current, b.current, geometric_gate)
        || !pairwise_range_feasible(a.previous, b.previous, geometric_gate))
      continue;
    if (!pairwise_range_feasible(a.current, c.current, geometric_gate)
        || !pairwise_range_feasible(b.current, c.current, geometric_gate)
        || !pairwise_range_feasible(a.previous, c.previous, geometric_gate)
        || !pairwise_range_feasible(b.previous, c.previous, geometric_gate))
      continue;
    std::vector<Observation> previous{a.previous, b.previous, c.previous};
    std::vector<Observation> current{a.current, b.current, c.current};
    ++last_processing_stats_.birth_fit_attempts;
    TemporalStateFit fit = fit_temporal_state(previous, current, birth_dt, config_);
    if (!fit.valid) continue;
    for (size_t other : usable_receivers) {
      if (std::find(selected.begin(), selected.end(), other) != selected.end()) continue;
      const ReceiverTracklet* best = nullptr;
      double best_cost = std::numeric_limits<double>::infinity();
      const std::vector<double> fitted_state{
          fit.position.x, fit.position.y, fit.position.z,
          fit.velocity.x, fit.velocity.y, fit.velocity.z};
      for (const ReceiverTracklet& candidate : tracklets[other]) {
        const double cost = temporal_state_cost(
            fitted_state, {candidate.previous}, {candidate.current}, birth_dt).first;
        if (cost < best_cost) best_cost = cost, best = &candidate;
      }
      // The fourth receiver is opportunistic: absence never rejects a triple.
      if (best && best_cost <= 2.0 * geometric_gate) {
        previous.push_back(best->previous);
        current.push_back(best->current);
      }
    }
    if (current.size() > 3) {
      ++last_processing_stats_.birth_fit_attempts;
      fit = fit_temporal_state(previous, current, birth_dt, config_);
      if (!fit.valid) continue;
    }
    // Hard CLEAN objects only propose a geometry.  Validate it against the unthresholded maps,
    // rotating the held-out receiver whenever four hard paths are present.  A three-receiver birth
    // remains possible, but the fourth receiver contributes soft evidence even after a local FN.
    const CandidatePredictiveScore direct_predictive = score_candidate_three_plus_one(
        previous, current, batches, birth_history_->observations, observations,
        birth_history_->soft_surfaces,
        birth_dt, config_, nullptr);
    last_processing_stats_.soft_map_queries = saturating_add(
        last_processing_stats_.soft_map_queries, direct_predictive.soft_queries);
    last_processing_stats_.predictive_folds = saturating_add(
        last_processing_stats_.predictive_folds,
        direct_predictive.score.evaluated_folds);
    // The clutter model has zero target-state parameters and zero log likelihood ratio. BIC is
    // therefore a direct, data-dimensional decision with no tuned residual gate.
    // A retained soft map can directly reject a proposal because it contains the complete null
    // field. A thresholded held-out proposal stream supplies causal validation but is not complete
    // enough to delete the internal hypothesis at birth; it must earn reportability over later
    // CPIs instead. This distinction prevents local FNs from becoming irreversible global FNs.
    if (direct_predictive.soft_folds > 0
        && direct_predictive.score.available && !(direct_predictive.score.bic < 0.0)) {
      ++last_processing_stats_.soft_birth_rejections;
      continue;
    }

    RotatingPredictiveScore best_reflected;
    uint64_t best_reflector_id = 0;
    for (const ReflectorPlane& plane : reflector_bank_.planes()) {
      if (!plane.admitted) continue;
      const CandidatePredictiveScore reflected = score_candidate_three_plus_one(
          previous, current, batches, birth_history_->observations, observations,
          birth_history_->soft_surfaces,
          birth_dt, config_, &plane);
      last_processing_stats_.soft_map_queries = saturating_add(
          last_processing_stats_.soft_map_queries, reflected.soft_queries);
      last_processing_stats_.predictive_folds = saturating_add(
          last_processing_stats_.predictive_folds,
          reflected.score.evaluated_folds);
      if (reflected.score.available
          && (!best_reflected.available || reflected.score.bic < best_reflected.bic)) {
        best_reflected = reflected.score;
        best_reflector_id = plane.id;
      }
    }
    if (best_reflected.available
        && (!direct_predictive.score.available
            || best_reflected.bic < direct_predictive.score.bic)) {
      // The plane was learned before this proposal from other causal epochs; it is never fitted to
      // this candidate.  Keep the path as explained evidence for its physical parent, not a track.
      ++last_processing_stats_.reflected_birth_rejections;
      continue;
    }
    BirthSolution birth;
    birth.observations = std::move(current);
    birth.previous_observations = std::move(previous);
    birth.position = fit.position;
    birth.velocity = fit.velocity;
    for (size_t row = 0; row < 3; ++row)
      for (size_t column = 0; column < 3; ++column) {
        birth.position_covariance(row, column) = fit.covariance(row, column);
        birth.velocity_covariance(row, column) = fit.covariance(row + 3, column + 3);
      }
    birth.state_covariance = fit.covariance;
    birth.condition = fit.condition;
    birth.normalized_rms = fit.normalized_rms;
    birth.direct_predictive = direct_predictive.score;
    birth.reflected_predictive = best_reflected;
    birth.reflector_id = best_reflector_id;
    births.push_back(std::move(birth));
  }
  last_processing_stats_.admitted_birth_solutions = births.size();
  std::sort(births.begin(), births.end(), [](const BirthSolution& left,
                                             const BirthSolution& right) {
    return std::tuple(-static_cast<int>(left.observations.size()), left.normalized_rms,
                      left.condition)
           < std::tuple(-static_cast<int>(right.observations.size()), right.normalized_rms,
                        right.condition);
  });

  std::set<std::pair<uint32_t, size_t>> previous_used;
  for (BirthSolution birth : births) {
    bool conflict = false;
    for (const Observation& observation : birth.observations)
      conflict = conflict || used.count({observation.receiver, observation.detection});
    for (const Observation& observation : birth.previous_observations)
      conflict = conflict || previous_used.count({observation.receiver, observation.detection});
    for (const Track& track : tracks_) {
      const Vec3 existing{track.state[0], track.state[1], track.state[2]};
      Matrix combined_covariance(3, 3);
      for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
          combined_covariance(row, column) = birth.position_covariance(row, column)
                                              + track.covariance(row, column);
      const Vec3 delta = birth.position - existing;
      try {
        conflict = conflict || quadratic(
            {delta.x, delta.y, delta.z}, pseudoinverse_symmetric(combined_covariance))
            <= association_gate;
      } catch (...) {}
    }
    if (conflict) continue;

    // UL is existence evidence, not a prerequisite for forming or ranking DL geometry. Validate
    // only a birth that survived DL conflict resolution; evaluating every discarded DL tuple was
    // both wasteful and capable of exhausting the causal birth-search budget.
    if (!ul_by_session.empty()) {
      const std::array<double, 6> target_state{
          birth.position.x, birth.position.y, birth.position.z,
          birth.velocity.x, birth.velocity.y, birth.velocity.z};
      std::vector<double> support_scores;
      std::vector<double> contradiction_scores;
      std::vector<std::optional<double>> direct_model_scores;
      std::vector<std::optional<double>> reflected_model_scores;
      bool anchored_ul_decision = false;
      const ReflectorPlane* birth_reflector = nullptr;
      for (const ReflectorPlane& plane : reflector_bank_.planes())
        if (plane.id == birth.reflector_id) birth_reflector = &plane;
      for (const auto& [session, session_batches] : ul_by_session) {
        if (!ul_established_before[session]) continue;
        auto found = ul_validators_.find(session);
        if (found == ul_validators_.end()) continue;
        ++last_processing_stats_.ul_validation_attempts;
        const auto validation = found->second.evaluate(
            time, target_state, birth.state_covariance, session_batches,
            remaining_processing_budget());
        account_ul_work(validation);
        if (validation.evaluated_folds >= config_.birth_minimum_receivers
            && std::isfinite(validation.log_bayes_factor))
          direct_model_scores.push_back(validation.log_bayes_factor);
        if (validation.decision == UlDifferentialDecision::support)
          support_scores.push_back(validation.log_bayes_factor);
        else if (validation.decision == UlDifferentialDecision::contradiction)
          contradiction_scores.push_back(validation.log_bayes_factor);
        if (validation.decision != UlDifferentialDecision::unavailable)
          anchored_ul_decision = anchored_ul_decision
                                 || found->second.has_direct_session_anchor();
        if (birth_reflector) {
          const auto reflected = found->second.evaluate_reflected(
              time, target_state, birth.state_covariance, session_batches,
              *birth_reflector, remaining_processing_budget());
          if (reflected.evaluated_folds >= config_.birth_minimum_receivers
              && std::isfinite(reflected.log_bayes_factor))
            reflected_model_scores.push_back(reflected.log_bayes_factor);
        }
      }
      const size_t independent_decisions = support_scores.size()
                                           + contradiction_scores.size();
      const bool actionable_ul = anchored_ul_decision
                                 || independent_decisions >= 3;
      if (!actionable_ul) {
        direct_model_scores.clear();
        reflected_model_scores.clear();
      }
      const RotatingPredictiveScore direct_ul = predictive_bic_from_correlated_folds(
          direct_model_scores, 0, 2 * direct_model_scores.size());
      const RotatingPredictiveScore reflected_ul = predictive_bic_from_correlated_folds(
          reflected_model_scores, 0, 2 * reflected_model_scores.size());
      double direct_combined = birth.direct_predictive.bic;
      double reflected_combined = birth.reflected_predictive.bic;
      if (direct_ul.available)
        direct_combined = birth.direct_predictive.available
            ? 0.5 * (direct_combined + direct_ul.bic) : direct_ul.bic;
      if (reflected_ul.available)
        reflected_combined = birth.reflected_predictive.available
            ? 0.5 * (reflected_combined + reflected_ul.bic) : reflected_ul.bic;
      birth.direct_predictive.bic = direct_combined;
      birth.reflected_predictive.bic = reflected_combined;
      if (std::isfinite(reflected_combined)
          && (!std::isfinite(direct_combined) || reflected_combined < direct_combined)) {
        ++last_processing_stats_.reflected_birth_rejections;
        continue;
      }
      std::vector<double> independent_illuminator_scores = support_scores;
      independent_illuminator_scores.insert(independent_illuminator_scores.end(),
                                            contradiction_scores.begin(),
                                            contradiction_scores.end());
      if (actionable_ul && !independent_illuminator_scores.empty()) {
        const double score = sample_median(independent_illuminator_scores);
        birth.ul_validation.log_bayes_factor = score;
        if (support_scores.size() > independent_illuminator_scores.size() / 2
            && score > 0.0)
          birth.ul_validation.decision = UlDifferentialDecision::support;
        else if (contradiction_scores.size()
                       > independent_illuminator_scores.size() / 2
                 && score < 0.0)
          birth.ul_validation.decision = UlDifferentialDecision::contradiction;
      }
      if (birth.ul_validation.decision == UlDifferentialDecision::support) {
        // The validator already corrects selection among UL peak associations. Also correct for
        // selecting the best of the currently measured DL birth hypotheses; this multiplicity is
        // data-derived and prevents a false DL candidate from earning support merely by trying
        // many convenient nuisance-UE fits.
        birth.ul_validation.log_bayes_factor -= std::log(
            static_cast<double>(std::max<size_t>(1, births.size())));
        if (!(birth.ul_validation.log_bayes_factor > 0.0)) {
          birth.ul_validation.decision = UlDifferentialDecision::unavailable;
          birth.ul_validation.reason = "support_removed_by_dl_hypothesis_multiplicity";
        }
      }
      if (birth.ul_validation.decision == UlDifferentialDecision::support)
        ++last_processing_stats_.ul_supported_birth_solutions;
      else if (birth.ul_validation.decision == UlDifferentialDecision::contradiction) {
        ++last_processing_stats_.ul_contradicted_birth_solutions;
        // A trusted second-illuminator veto explains these measurements as non-target returns.
        // Consume both causal epochs so they cannot be recycled into a different false tuple.
        for (const Observation& observation : birth.observations)
          used.emplace(observation.receiver, observation.detection);
        for (const Observation& observation : birth.previous_observations)
          previous_used.emplace(observation.receiver, observation.detection);
        continue;
      } else {
        ++last_processing_stats_.ul_unavailable_birth_solutions;
      }
    }

    Track track;
    track.id = next_id_++;
    track.config = config_;
    track.probability = config_.initial_model_probability;
    const double probability_sum = std::accumulate(track.probability.begin(),
                                                    track.probability.end(), 0.0);
    for (double& value : track.probability) value /= probability_sum;
    track.state = {birth.position.x, birth.position.y, birth.position.z,
                   birth.velocity.x, birth.velocity.y, birth.velocity.z,
                   0.0, 0.0, 0.0};
    track.covariance = Matrix(STATE_SIZE, STATE_SIZE);
    for (size_t r = 0; r < 3; ++r)
      for (size_t c = 0; c < 3; ++c) {
        track.covariance(r, c) = birth.position_covariance(r, c);
        track.covariance(r + 3, c + 3) = birth.velocity_covariance(r, c);
      }
    for (size_t axis = 0; axis < 3; ++axis)
      track.covariance(axis + 6, axis + 6) = std::max(
          std::numeric_limits<double>::epsilon(),
          birth.velocity_covariance(axis, axis) / (birth_dt * birth_dt));
    track.covariance = positive_semidefinite(track.covariance, 1e-9);
    for (size_t model = 0; model < MODEL_COUNT; ++model) {
      track.model_state[model] = track.state;
      track.model_covariance[model] = track.covariance;
    }
    track.time_s = time;
    track.source_sequence = sequence;
    track.receiver_updates = birth.observations.size();
    track.birth_condition = birth.condition;
    const double birth_false_probability = std::clamp(
        config_.false_object_intensity_per_s * birth_dt,
        std::numeric_limits<double>::epsilon(),
        1.0 - std::numeric_limits<double>::epsilon());
    track.existence_log_evidence = -2.0 * std::log(birth_false_probability);
    if (birth.ul_validation.decision == UlDifferentialDecision::support) {
      // Positive UL identifies the origin model but does not accelerate DL confirmation. This
      // prevents a shared nuisance-state error from promoting a geometrically weak false birth;
      // positive evidence must instead persist through the ordinary causal origin history.
      track.origin.apply_ul_direct_evidence(birth.ul_validation.log_bayes_factor);
      track.ul_validation_log_bayes_factor = birth.ul_validation.log_bayes_factor;
    track.ul_validation_status = ul_differential_decision_name(
          birth.ul_validation.decision);
    }
    track.direct_predictive_bic = birth.direct_predictive.bic;
    track.reflected_predictive_bic = birth.reflected_predictive.bic;
    track.confirmation_log_threshold = std::log(
        std::max<uint64_t>(1, tracklet_hypotheses) / birth_false_probability);
    track.confirmed = track.existence_log_evidence >= track.confirmation_log_threshold;
    track.confirmed_updates = track.confirmed ? 1 : 0;
    track.total_updates = 2;
    for (const Observation& observation : birth.observations) {
      track.last_score = std::max(track.last_score, observation.value.score);
      used.emplace(observation.receiver, observation.detection);
    }
    for (const Observation& observation : birth.previous_observations)
      previous_used.emplace(observation.receiver, observation.detection);
    track.observe_origin_epoch(birth.observations);
    tracks_.push_back(std::move(track));
  }
  save_birth_history();
  bootstrap_ul_sessions();
  finish_processing();
}

std::vector<TrackSnapshot> MultistaticImmTracker::snapshots() const
{
  std::vector<TrackSnapshot> out;
  out.reserve(tracks_.size());
  for (const Track& track : tracks_)
    if (!track.birth_family) out.push_back(track.snapshot(geometries_.front()));
  return out;
}

std::vector<TrackSnapshot> MultistaticImmTracker::reportable_snapshots() const
{
  std::vector<TrackSnapshot> out;
  out.reserve(tracks_.size());
  for (const Track& track : tracks_)
    if (track.confirmed && track.direct_emission_validated())
      out.push_back(track.snapshot(geometries_.front()));
  return out;
}

TrackSnapshot MultistaticImmTracker::planning_snapshot(double time,
                                                       uint32_t receiver) const
{
  if (receiver >= geometries_.size())
    throw std::out_of_range("multistatic planning receiver is outside geometry");
  if (tracks_.empty()) {
    TrackSnapshot empty; empty.status = "uninitialized"; empty.air_time_s = time; return empty;
  }
  auto priority = [](const Track& track) {
    return std::tuple(track.confirmed ? 1 : 0, -track.coasts,
                      -trace(track.covariance));
  };
  const Track* best = nullptr;
  for (const auto& track : tracks_)
    if (!track.birth_family && !track.associated_object_path
        && (!best || priority(*best) < priority(track))) best = &track;
  if (!best) {
    TrackSnapshot empty; empty.status = "uninitialized"; empty.air_time_s = time; return empty;
  }
  Track predicted = *best;
  if (time >= predicted.time_s && time - predicted.time_s <= config_.maximum_propagation_s)
    predicted.predict(time);
  TrackSnapshot out = predicted.snapshot(geometries_[receiver]);
  out.propagated = time > best->time_s;
  out.propagation_age_s = std::max(0.0, time - best->time_s);
  return out;
}

std::vector<ConfirmedTrackView> MultistaticImmTracker::confirmed_tracks(uint32_t receiver) const
{
  if (receiver >= geometries_.size())
    throw std::out_of_range("multistatic receiver is outside geometry");
  std::vector<ConfirmedTrackView> out;
  for (const Track& track : tracks_) {
    if (!track.confirmed || track.birth_family || track.associated_object_path) continue;
    const TrackSnapshot snapshot = track.snapshot(geometries_[receiver]);
    out.push_back({track.id, snapshot.range_m, snapshot.range_rate_mps,
                   snapshot.sigma_range_m, snapshot.sigma_rate_mps, track.last_score});
  }
  return out;
}

} // namespace nr_isac
