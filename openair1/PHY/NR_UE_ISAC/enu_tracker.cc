/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "enu_tracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace nr_isac {
namespace {

std::pair<std::vector<double>, Matrix> measurement_and_noise(const Detection& d,
                                                              double range_res,
                                                              double rate_res,
                                                              bool force_2d,
                                                              bool* angles)
{
  if (!(range_res > 0.0) || !(rate_res > 0.0))
    throw std::invalid_argument("measurement resolutions must be positive");
  std::vector<double> z{d.range_m, d.range_rate_mps};
  Matrix noise(2, 2);
  noise(0, 0) = range_res * range_res / 12.0;
  noise(1, 1) = rate_res * rate_res / 12.0;
  bool use_angles = !force_2d && d.aoa.valid && d.aoa.covariance_rad2.rows() == 2
                    && d.aoa.covariance_rad2.cols() == 2;
  if (use_angles) {
    const Matrix cov = symmetrized(d.aoa.covariance_rad2);
    const auto ev = eigenvalues_symmetric_2x2(cov(0, 0), cov(0, 1), cov(1, 1));
    use_angles = std::isfinite(ev[0]) && ev[0] > 0.0;
    if (use_angles) {
      z.push_back(d.aoa.azimuth_deg * PI / 180.0);
      z.push_back(d.aoa.elevation_deg * PI / 180.0);
      Matrix expanded(4, 4);
      expanded(0, 0) = noise(0, 0); expanded(1, 1) = noise(1, 1);
      for (size_t r = 0; r < 2; ++r)
        for (size_t c = 0; c < 2; ++c) expanded(r + 2, c + 2) = cov(r, c);
      noise = expanded;
    }
  }
  if (angles) *angles = use_angles;
  return {z, noise};
}

std::tuple<std::vector<double>, Matrix, double> initial_state_covariance(
    const Detection& d, const BistaticGeometry& geometry,
    double range_res, double rate_res, double dwell)
{
  bool with_angles = false;
  auto [z, noise] = measurement_and_noise(d, range_res, rate_res, false, &with_angles);
  if (!with_angles) throw std::invalid_argument("3-D birth requires valid AoA covariance");
  auto transform = [&](const std::vector<double>& value) {
    const Vec3 position = position_from_measurement(geometry, value[0], value[2], value[3]);
    std::vector<double> probe{position.x, position.y, position.z, 0.0, 0.0, 0.0};
    const Matrix h = enu_measurement_jacobian(probe, geometry, false);
    Vec3 gradient{h(0, 0), h(0, 1), h(0, 2)};
    const double denominator = norm2(gradient);
    if (!(denominator > 0.0)) throw std::invalid_argument("range gradient is singular");
    const Vec3 velocity = gradient * (value[1] / denominator);
    return std::vector<double>{position.x, position.y, position.z,
                               velocity.x, velocity.y, velocity.z};
  };
  std::vector<double> state = transform(z);
  Matrix jac(6, 4);
  for (size_t c = 0; c < 4; ++c) {
    const double step = std::sqrt(std::max(noise(c, c), std::numeric_limits<double>::epsilon()));
    auto plus = z, minus = z; plus[c] += step; minus[c] -= step;
    const auto xp = transform(plus), xm = transform(minus);
    for (size_t r = 0; r < 6; ++r) jac(r, c) = (xp[r] - xm[r]) / (2.0 * step);
  }
  Matrix covariance = jac * noise * jac.transposed();
  const Vec3 position{state[0], state[1], state[2]};
  const double rx_distance = norm(position - geometry.rx);
  const auto angle_ev = eigenvalues_symmetric_2x2(noise(2, 2), noise(2, 3), noise(3, 3));
  const double interval = std::max(dwell, std::numeric_limits<double>::epsilon());
  const double tangential_sigma = rx_distance * std::sqrt(std::max(0.0, angle_ev[1])) / interval;
  const Matrix h = enu_measurement_jacobian(state, geometry, false);
  Vec3 g{h(0, 0), h(0, 1), h(0, 2)}; g = normalized(g);
  for (size_t r = 0; r < 3; ++r)
    for (size_t c = 0; c < 3; ++c)
      covariance(r + 3, c + 3) += ((r == c ? 1.0 : 0.0) - g[r] * g[c])
                                    * tangential_sigma * tangential_sigma;
  covariance = positive_semidefinite(covariance);
  double velocity_trace = 0.0;
  for (size_t i = 3; i < 6; ++i) velocity_trace += covariance(i, i);
  const double accel_variance = std::max(velocity_trace / (3.0 * interval * interval),
                                         std::numeric_limits<double>::epsilon());
  return {state, covariance, accel_variance};
}

} // namespace

void BistaticGeometry::validate() const
{
  if (!std::isfinite(tx.x) || !std::isfinite(tx.y) || !std::isfinite(tx.z)
      || !std::isfinite(rx.x) || !std::isfinite(rx.y) || !std::isfinite(rx.z)
      || !(baseline_m() > 0.0))
    throw std::invalid_argument("Tx/Rx positions must be finite and noncoincident");
}

double wrap_radians(double value)
{
  double result = std::fmod(value + PI, 2.0 * PI);
  if (result < 0.0) result += 2.0 * PI;
  return result - PI;
}

Vec3 direction_from_angles(double azimuth, double elevation)
{
  const double ce = std::cos(elevation);
  return {ce * std::cos(azimuth), ce * std::sin(azimuth), std::sin(elevation)};
}

Vec3 position_from_measurement(const BistaticGeometry& g, double excess,
                               double azimuth, double elevation)
{
  const Vec3 direction = direction_from_angles(azimuth, elevation);
  const Vec3 tx_from_rx = g.rx - g.tx;
  const double total = g.baseline_m() + excess;
  const double denominator = 2.0 * (total + dot(tx_from_rx, direction));
  const double numerator = total * total - norm2(tx_from_rx);
  if (!(denominator > 0.0)) throw std::invalid_argument("bearing misses bistatic ellipsoid");
  const double distance = numerator / denominator;
  if (distance < 0.0 || total - distance < 0.0 || !std::isfinite(distance))
    throw std::invalid_argument("no forward-ray bistatic intersection");
  return g.rx + direction * distance;
}

std::vector<double> enu_measurement_model(const std::vector<double>& x,
                                          const BistaticGeometry& g, bool angles)
{
  if (x.size() != 6) throw std::invalid_argument("ENU state must have six entries");
  const Vec3 p{x[0], x[1], x[2]}, v{x[3], x[4], x[5]};
  const Vec3 tx_leg = p - g.tx, rx_leg = p - g.rx;
  const double tx_range = norm(tx_leg), rx_range = norm(rx_leg);
  if (!(tx_range > 0.0) || !(rx_range > 0.0)) throw std::invalid_argument("ENU model singular at focus");
  const Vec3 gradient = tx_leg / tx_range + rx_leg / rx_range;
  std::vector<double> out{tx_range + rx_range - g.baseline_m(), dot(gradient, v)};
  if (angles) {
    out.push_back(std::atan2(rx_leg.y, rx_leg.x));
    out.push_back(std::atan2(rx_leg.z, std::hypot(rx_leg.x, rx_leg.y)));
  }
  return out;
}

Matrix enu_measurement_jacobian(const std::vector<double>& x,
                                const BistaticGeometry& g, bool angles)
{
  const Vec3 p{x[0], x[1], x[2]}, v{x[3], x[4], x[5]};
  const Vec3 tl = p - g.tx, rl = p - g.rx;
  const double tr = norm(tl), rr = norm(rl);
  if (!(tr > 0.0) || !(rr > 0.0)) throw std::invalid_argument("ENU Jacobian singular at focus");
  const Vec3 tu = tl / tr, ru = rl / rr, gradient = tu + ru;
  Matrix curvature(3, 3);
  for (size_t r = 0; r < 3; ++r)
    for (size_t c = 0; c < 3; ++c)
      curvature(r, c) = ((r == c ? 1.0 : 0.0) - tu[r] * tu[c]) / tr
                        + ((r == c ? 1.0 : 0.0) - ru[r] * ru[c]) / rr;
  const auto cv = curvature * std::vector<double>{v.x, v.y, v.z};
  Matrix h(angles ? 4 : 2, 6);
  for (size_t c = 0; c < 3; ++c) {
    h(0, c) = gradient[c]; h(1, c) = cv[c]; h(1, c + 3) = gradient[c];
  }
  if (angles) {
    const double horizontal2 = rl.x * rl.x + rl.y * rl.y;
    const double horizontal = std::sqrt(horizontal2);
    const double radius2 = horizontal2 + rl.z * rl.z;
    if (!(horizontal > std::numeric_limits<double>::min()))
      throw std::invalid_argument("angles singular on Up axis");
    h(2, 0) = -rl.y / horizontal2; h(2, 1) = rl.x / horizontal2;
    h(3, 0) = -rl.x * rl.z / (radius2 * horizontal);
    h(3, 1) = -rl.y * rl.z / (radius2 * horizontal);
    h(3, 2) = horizontal / radius2;
  }
  return h;
}

Matrix enu_transition(double dt)
{
  Matrix f = Matrix::identity(6);
  for (size_t i = 0; i < 3; ++i) f(i, i + 3) = dt;
  return f;
}

Matrix enu_process_noise(double dt, double variance)
{
  Matrix q(6, 6);
  const double pp = 0.25 * dt * dt * dt * dt * variance;
  const double pv = 0.5 * dt * dt * dt * variance;
  const double vv = dt * dt * variance;
  for (size_t i = 0; i < 3; ++i) {
    q(i, i) = pp; q(i, i + 3) = q(i + 3, i) = pv; q(i + 3, i + 3) = vv;
  }
  return q;
}

EnuTrack::EnuTrack(uint64_t id, double time, const Detection& d,
                   const BistaticGeometry& geometry, double rr, double vr, double dwell,
                   const EnuTrackerConfig& config, std::optional<size_t> index,
                   std::optional<double> stage1_range, std::optional<double> stage1_rate)
    : id_(id), geometry_(geometry), config_(config), time_s_(time), birth_time_s_(time),
      last_update_time_s_(time), associated_index_(index), stage1_range_m_(stage1_range),
      stage1_rate_mps_(stage1_rate)
{
  geometry_.validate();
  std::tie(x_, p_, acceleration_variance_) = initial_state_covariance(d, geometry_, rr, vr, dwell);
}

void EnuTrack::predict(double time)
{
  const double dt = time - time_s_;
  if (dt < 0.0) throw std::invalid_argument("air-interface time moved backwards");
  if (dt == 0.0) return;
  const Matrix f = enu_transition(dt);
  x_ = f * x_; p_ = positive_semidefinite(f * p_ * f.transposed()
                                           + enu_process_noise(dt, acceleration_variance_));
  time_s_ = time; updated_ = false; associated_index_.reset(); nis_.reset();
}

EnuInnovation EnuTrack::innovation(const Detection& d, double rr, double vr, bool force_2d) const
{
  EnuInnovation out;
  try {
    auto pair = measurement_and_noise(d, rr, vr, force_2d, &out.with_angles);
    const auto predicted = enu_measurement_model(x_, geometry_, out.with_angles);
    out.residual.resize(pair.first.size());
    for (size_t i = 0; i < out.residual.size(); ++i) out.residual[i] = pair.first[i] - predicted[i];
    if (out.with_angles) out.residual[2] = wrap_radians(out.residual[2]);
    out.jacobian = enu_measurement_jacobian(x_, geometry_, out.with_angles);
    out.noise = pair.second;
    const Matrix s = out.jacobian * p_ * out.jacobian.transposed() + out.noise;
    out.nis = quadratic(out.residual, pseudoinverse_symmetric(s));
    out.valid = std::isfinite(out.nis);
  } catch (...) { out.valid = false; }
  return out;
}

bool EnuTrack::update(const Detection& d, double rr, double vr, std::optional<size_t> index,
                      std::optional<double> stage1_range, std::optional<double> stage1_rate)
{
  EnuInnovation selected = innovation(d, rr, vr, false);
  if (!selected.valid || selected.nis > (selected.with_angles ? config_.gate_chi2_4d
                                                              : config_.gate_chi2_2d))
    selected = innovation(d, rr, vr, true);
  if (!selected.valid || selected.nis > config_.gate_chi2_2d) { coast(); return false; }
  const auto old_velocity = std::vector<double>{x_[3], x_[4], x_[5]};
  const Matrix s = selected.jacobian * p_ * selected.jacobian.transposed() + selected.noise;
  Matrix gain = p_ * selected.jacobian.transposed() * pseudoinverse_symmetric(s);
  if (selected.with_angles)
    for (size_t r = 3; r < 6; ++r)
      for (size_t c = 2; c < 4; ++c) gain(r, c) = 0.0;
  const auto dx = gain * selected.residual;
  for (size_t i = 0; i < 6; ++i) x_[i] += dx[i];
  const Matrix factor = Matrix::identity(6) - gain * selected.jacobian;
  p_ = positive_semidefinite(factor * p_ * factor.transposed()
                              + gain * selected.noise * gain.transposed());
  if (config_.adapt_acceleration_variance) {
    const double interval = std::max(time_s_ - last_update_time_s_,
                                     std::max(d.dwell_s, std::numeric_limits<double>::epsilon()));
    double velocity_change2 = 0.0;
    for (size_t i = 0; i < 3; ++i) velocity_change2 += (x_[i + 3] - old_velocity[i])
                                                         * (x_[i + 3] - old_velocity[i]);
    const double observed = velocity_change2 / (3.0 * interval * interval);
    acceleration_variance_ = std::max((1.0 - config_.adaptation_alpha) * acceleration_variance_
                                       + config_.adaptation_alpha * observed,
                                       std::numeric_limits<double>::epsilon());
  }
  last_update_time_s_ = time_s_; nis_ = selected.nis;
  nis_ewma_ = nis_ewma_ ? (1.0 - config_.adaptation_alpha) * *nis_ewma_
                          + config_.adaptation_alpha * *nis_ : *nis_;
  coasts_ = 0; ++total_updates_; ++confirmed_updates_; updated_ = true;
  associated_index_ = index; status_ = "confirmed"; recent_.push_back(1);
  if (recent_.size() > config_.confirm_window) recent_.erase(recent_.begin());
  if (stage1_range) stage1_range_m_ = stage1_range;
  if (stage1_rate) stage1_rate_mps_ = stage1_rate;
  return true;
}

void EnuTrack::coast()
{
  ++coasts_; recent_.push_back(0);
  if (recent_.size() > config_.confirm_window) recent_.erase(recent_.begin());
  updated_ = false; associated_index_.reset();
  if (coasts_ >= config_.confirm_updates) status_ = "coasting";
}

void EnuTrack::cap_birth_uncertainty(double max_velocity_variance,
                                     double max_acceleration_variance)
{
  if (!(max_velocity_variance > 0.0) || !(max_acceleration_variance > 0.0))
    throw std::invalid_argument("birth uncertainty caps must be positive");
  for (size_t i = 3; i < 6; ++i) p_(i, i) = std::min(p_(i, i), max_velocity_variance);
  p_ = symmetrized(p_);
  acceleration_variance_ = std::min(acceleration_variance_, max_acceleration_variance);
}

TrackSnapshot EnuTrack::snapshot() const
{
  TrackSnapshot s;
  s.track_id = id_; s.status = status_; s.air_time_s = time_s_; s.has_time = true;
  s.updated = updated_; s.position_valid = true;
  s.position_enu_m = {x_[0], x_[1], x_[2]}; s.velocity_enu_mps = {x_[3], x_[4], x_[5]};
  for (size_t r = 0; r < 3; ++r)
    for (size_t c = 0; c < 3; ++c) {
      s.position_covariance(r, c) = p_(r, c);
      s.velocity_covariance(r, c) = p_(r + 3, c + 3);
    }
  const auto observable = enu_measurement_model(x_, geometry_, true);
  s.range_m = stage1_range_m_.value_or(observable[0]);
  s.range_rate_mps = stage1_rate_mps_.value_or(observable[1]);
  s.azimuth_deg = observable[2] * 180.0 / PI; s.elevation_deg = observable[3] * 180.0 / PI;
  s.has_nis = nis_.has_value(); s.nis = nis_.value_or(0.0);
  s.has_nis_ewma = nis_ewma_.has_value(); s.nis_ewma = nis_ewma_.value_or(0.0);
  s.coast_count = coasts_; s.confirmed_update_count = confirmed_updates_;
  s.total_update_count = total_updates_;
  return s;
}

} // namespace nr_isac
