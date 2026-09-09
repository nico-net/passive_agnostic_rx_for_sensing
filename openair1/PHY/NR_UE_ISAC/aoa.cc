/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "aoa.h"

#include "sync_correction.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace nr_isac {
namespace {

using Complex = std::complex<double>;
using ComplexMatrix = std::vector<std::vector<Complex>>;

struct FamilyKey {
  uint32_t source = 0;
  int fraction_28 = 0;
  std::string occupancy;
  bool operator<(const FamilyKey& b) const
  { return std::tie(source, fraction_28, occupancy) < std::tie(b.source, b.fraction_28, b.occupancy); }
};

std::vector<std::vector<uint32_t>> family_rows(const CfrWindow& w,
                                               const std::vector<uint8_t>& observed)
{
  std::map<FamilyKey, std::vector<uint32_t>> groups;
  for (uint32_t r = 0; r < w.rows; ++r) {
    FamilyKey key{w.row_source_mask[r], static_cast<int>(std::llround(w.row_slot_frac[r] * 28.0)), {}};
    key.occupancy.assign(reinterpret_cast<const char*>(observed.data() + (size_t)r * w.subcarriers),
                         w.subcarriers);
    groups[std::move(key)].push_back(r);
  }
  std::vector<std::vector<uint32_t>> out;
  for (auto& item : groups) out.push_back(std::move(item.second));
  return out;
}

Complex template_value(const CfrWindow& w, uint32_t row, uint32_t k,
                       double range, double rate)
{
  const double time = (w.row_time_slots[row] - w.row_time_slots[0]) * slot_duration_s(w.scs_hz);
  const double phase = -2.0 * PI * k * w.scs_hz * range / C_MPS
                       -2.0 * PI * rate * w.fc_hz * time / C_MPS;
  return {std::cos(phase), std::sin(phase)};
}

ComplexMatrix hermitian_pseudoinverse(const ComplexMatrix& input, double rcond,
                                      size_t* rank_out = nullptr,
                                      double* largest_out = nullptr)
{
  const size_t n = input.size();
  if (!n) return {};
  Matrix embedded(2 * n, 2 * n);
  for (size_t i = 0; i < n; ++i) {
    if (input[i].size() != n)
      throw std::invalid_argument("Hermitian pseudoinverse needs a square matrix");
    for (size_t j = 0; j < n; ++j) {
      const Complex value = 0.5 * (input[i][j] + std::conj(input[j][i]));
      embedded(i, j) = value.real();
      embedded(i, n + j) = -value.imag();
      embedded(n + i, j) = value.imag();
      embedded(n + i, n + j) = value.real();
    }
  }
  const SymmetricEigen eig = symmetric_eigen(embedded);
  const double largest = eig.values.empty() ? 0.0 : std::max(0.0, eig.values.back());
  const double cutoff = std::max(std::numeric_limits<double>::min(), rcond * largest);
  Matrix real_inverse(2 * n, 2 * n);
  size_t embedded_rank = 0;
  for (size_t k = 0; k < eig.values.size(); ++k) {
    if (!(eig.values[k] > cutoff)) continue;
    ++embedded_rank;
    const double reciprocal = 1.0 / eig.values[k];
    for (size_t i = 0; i < 2 * n; ++i)
      for (size_t j = 0; j < 2 * n; ++j)
        real_inverse(i, j) += reciprocal * eig.vectors(i, k) * eig.vectors(j, k);
  }
  ComplexMatrix result(n, std::vector<Complex>(n));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < n; ++j) {
      const double real = 0.5 * (real_inverse(i, j) + real_inverse(n + i, n + j));
      const double imag = 0.5 * (real_inverse(n + i, j) - real_inverse(i, n + j));
      result[i][j] = {real, imag};
    }
  if (rank_out) *rank_out = embedded_rank / 2;
  if (largest_out) *largest_out = largest;
  return result;
}

bool invert_complex(ComplexMatrix a, ComplexMatrix* inverse_out)
{
  const size_t n = a.size();
  if (!n || !inverse_out) return false;
  ComplexMatrix inverse(n, std::vector<Complex>(n));
  double scale = 0.0;
  for (size_t i = 0; i < n; ++i) {
    if (a[i].size() != n) return false;
    inverse[i][i] = 1.0;
    for (size_t j = 0; j < n; ++j) scale = std::max(scale, std::abs(a[i][j]));
  }
  const double floor = std::max(std::numeric_limits<double>::min(),
      64.0 * n * std::numeric_limits<double>::epsilon() * scale);
  for (size_t col = 0; col < n; ++col) {
    size_t pivot = col;
    for (size_t row = col + 1; row < n; ++row)
      if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
    if (!(std::abs(a[pivot][col]) > floor)) return false;
    if (pivot != col) { std::swap(a[pivot], a[col]); std::swap(inverse[pivot], inverse[col]); }
    const Complex value = a[col][col];
    for (size_t j = 0; j < n; ++j) { a[col][j] /= value; inverse[col][j] /= value; }
    for (size_t row = 0; row < n; ++row) {
      if (row == col) continue;
      const Complex factor = a[row][col];
      for (size_t j = 0; j < n; ++j) {
        a[row][j] -= factor * a[col][j];
        inverse[row][j] -= factor * inverse[col][j];
      }
    }
  }
  *inverse_out = std::move(inverse);
  return true;
}

std::vector<Complex> multiply(const ComplexMatrix& a, const std::vector<Complex>& x)
{
  std::vector<Complex> result(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    for (size_t j = 0; j < x.size(); ++j) result[i] += a[i][j] * x[j];
  return result;
}

double minimum_hermitian_eigenvalue(const ComplexMatrix& input)
{
  const size_t n = input.size();
  if (!n) return 0.0;
  Matrix embedded(2 * n, 2 * n);
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < n; ++j) {
      const Complex value = 0.5 * (input[i][j] + std::conj(input[j][i]));
      embedded(i, j) = value.real(); embedded(i, n + j) = -value.imag();
      embedded(n + i, j) = value.imag(); embedded(n + i, n + j) = value.real();
    }
  const SymmetricEigen eig = symmetric_eigen(embedded);
  return eig.values.empty() ? 0.0 : eig.values.front();
}

Matrix plane_basis(const ArrayGeometry& geometry, Vec3* null_out)
{
  Matrix gram(3, 3);
  for (size_t row = 1; row < 4; ++row) {
    const Vec3 b = geometry.positions[row] - geometry.positions[0];
    for (size_t i = 0; i < 3; ++i)
      for (size_t j = 0; j < 3; ++j) gram(i, j) += b[i] * b[j];
  }
  const SymmetricEigen eig = symmetric_eigen(gram);
  const double largest = std::max(0.0, eig.values[2]);
  const double tolerance = std::numeric_limits<double>::epsilon() * 9.0 * largest;
  if (!(eig.values[1] > tolerance) || eig.values[0] > tolerance)
    throw std::invalid_argument("array geometry is not rank-two");
  *null_out = normalized({eig.vectors(0, 0), eig.vectors(1, 0), eig.vectors(2, 0)});
  Matrix basis(3, 2);
  // SVD ordering is descending; preserve that order for covariance parity.
  for (size_t r = 0; r < 3; ++r) {
    basis(r, 0) = eig.vectors(r, 2); basis(r, 1) = eig.vectors(r, 1);
  }
  return basis;
}

Matrix baseline_matrix(const ArrayGeometry& geometry)
{
  Matrix b(3, 3);
  for (size_t r = 0; r < 3; ++r) {
    const Vec3 value = geometry.positions[r + 1] - geometry.positions[0];
    b(r, 0) = value.x; b(r, 1) = value.y; b(r, 2) = value.z;
  }
  return b;
}

bool finite_response(const std::array<Complex, 4>& z)
{
  for (Complex value : z)
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag())
        || !(std::abs(value) > std::numeric_limits<double>::min())) return false;
  return true;
}

double wrap_phase(double value)
{
  double result = std::fmod(value + PI, 2.0 * PI);
  if (result < 0.0) result += 2.0 * PI;
  return result - PI;
}

} // namespace

std::array<Complex, 4> surveyed_los_steering(const ArrayGeometry& geometry,
                                             Vec3 tx, Vec3 rx, double fc)
{
  if (!geometry.configured || !(fc > 0.0)) throw std::invalid_argument("LOS steering needs surveyed array");
  const Vec3 direction = normalized(tx - rx);
  const double wavelength = C_MPS / fc;
  std::array<Complex, 4> result;
  for (size_t a = 0; a < 4; ++a) {
    const double phase = 2.0 * PI * dot(geometry.positions[a] - geometry.positions[0], direction) / wavelength;
    result[a] = {std::cos(phase), std::sin(phase)};
  }
  return result;
}

std::array<Complex, 4> project_array_response(const CfrWindow& w,
                                              const std::vector<uint8_t>& observed,
                                              double range, double rate)
{
  if (!w.valid() || w.antennas != 4 || observed.size() != w.observed.size())
    throw std::invalid_argument("AoA projection needs a four-channel CFR window");
  std::array<Complex, 4> z{};
  for (uint32_t r = 0; r < w.rows; ++r)
    for (uint32_t k = 0; k < w.subcarriers; ++k) {
      if (!observed[w.cell(r, k)]) continue;
      const Complex steering = template_value(w, r, k, range, rate);
      for (uint32_t a = 0; a < 4; ++a)
        z[a] += static_cast<Complex>(w.values[w.sample(a, r, k)]) * std::conj(steering);
    }
  return z;
}

AoaIsolation isolate_target_response(const CfrWindow& w, const std::vector<uint8_t>& observed,
                                      double target_range, double target_rate,
                                      const std::vector<std::pair<double, double>>& nuisances,
                                      const std::array<Complex, 4>& los_spatial,
                                      double los_range)
{
  AoaIsolation out;
  if (!w.valid() || w.antennas != 4 || observed.size() != w.observed.size()) {
    out.reason = "four-channel observed CFR unavailable"; return out;
  }
  std::vector<std::pair<double, double>> pairs{{target_range, target_rate}};
  pairs.insert(pairs.end(), nuisances.begin(), nuisances.end());
  const size_t pair_count = pairs.size(), component_columns = pair_count * 4;
  ComplexMatrix component_gram(component_columns,
                               std::vector<Complex>(component_columns));
  std::vector<Complex> component_rhs(component_columns);
  const auto families = family_rows(w, observed);
  const size_t total_columns = component_columns + families.size();
  std::vector<size_t> row_family(w.rows);
  for (size_t f = 0; f < families.size(); ++f) for (uint32_t r : families[f]) row_family[r] = f;
  std::vector<double> family_norm(families.size());
  std::vector<Complex> family_rhs(families.size());
  std::vector<std::vector<Complex>> cross(families.size(),
                                          std::vector<Complex>(component_columns));
  std::array<Complex, 4> los_unit{};
  for (size_t a = 0; a < 4; ++a) {
    const double magnitude = std::abs(los_spatial[a]);
    if (!std::isfinite(magnitude) || !(magnitude > std::numeric_limits<double>::min())) {
      out.reason = "LOS nuisance steering is nonfinite or zero"; return out;
    }
    los_unit[a] = los_spatial[a] / magnitude;
  }
  double measurement_energy = 0.0;
  size_t observed_cells = 0;

  for (uint32_t r = 0; r < w.rows; ++r)
    for (uint32_t k = 0; k < w.subcarriers; ++k) {
      if (!observed[w.cell(r, k)]) continue;
      ++observed_cells;
      const size_t family = row_family[r];
      std::vector<Complex> temporal(pair_count);
      for (size_t p = 0; p < pair_count; ++p)
        temporal[p] = template_value(w, r, k, pairs[p].first, pairs[p].second);
      const double los_phase = -2.0 * PI * k * w.scs_hz * los_range / C_MPS;
      const Complex los_column{std::cos(los_phase), std::sin(los_phase)};
      family_norm[family] += 4.0 * std::norm(los_column);
      Complex combined{};
      for (size_t a = 0; a < 4; ++a) {
        const Complex y = w.values[w.sample(a, r, k)];
        measurement_energy += std::norm(y);
        combined += y * std::conj(los_unit[a]);
        for (size_t p = 0; p < pair_count; ++p) {
          const size_t i = p * 4 + a;
          component_rhs[i] += std::conj(temporal[p]) * y;
          cross[family][i] += std::conj(temporal[p]) * los_column * los_unit[a];
          for (size_t q = 0; q < pair_count; ++q)
            component_gram[i][q * 4 + a] += std::conj(temporal[p]) * temporal[q];
        }
      }
      family_rhs[family] += std::conj(los_column) * combined;
    }
  if (!observed_cells) { out.reason = "no observed samples for target isolation"; return out; }

  /* Match the Python joint normalized Gram while exploiting the fact that distinct scheduler-family
   * LOS columns are orthogonal. Eliminating that identity block leaves a small component Schur
   * system, with no scenario-tuned ridge term. */
  const double tiny = static_cast<double>(std::numeric_limits<float>::min());
  std::vector<double> component_norm(component_columns);
  for (size_t i = 0; i < component_columns; ++i)
    component_norm[i] = std::sqrt(std::max(0.0, component_gram[i][i].real()));
  ComplexMatrix schur(component_columns, std::vector<Complex>(component_columns));
  std::vector<Complex> schur_rhs(component_columns);
  for (size_t i = 0; i < component_columns; ++i) {
    const double ni = std::max(component_norm[i], tiny);
    schur_rhs[i] = component_rhs[i] / ni;
    for (size_t j = 0; j < component_columns; ++j)
      schur[i][j] = component_gram[i][j]
                    / (ni * std::max(component_norm[j], tiny));
  }
  std::vector<std::vector<Complex>> normalized_cross(
      families.size(), std::vector<Complex>(component_columns));
  size_t family_rank = 0;
  for (size_t f = 0; f < families.size(); ++f) {
    if (!(family_norm[f] > 0.0)) continue;
    ++family_rank;
    const double family_scale = std::sqrt(family_norm[f]);
    const Complex normalized_family_rhs = family_rhs[f] / family_scale;
    for (size_t i = 0; i < component_columns; ++i)
      normalized_cross[f][i] = cross[f][i]
          / (std::max(component_norm[i], tiny) * family_scale);
    for (size_t i = 0; i < component_columns; ++i) {
      schur_rhs[i] -= normalized_cross[f][i] * normalized_family_rhs;
      for (size_t j = 0; j < component_columns; ++j)
        schur[i][j] -= normalized_cross[f][i]
                       * std::conj(normalized_cross[f][j]);
    }
  }
  for (size_t i = 0; i < component_columns; ++i)
    for (size_t j = i; j < component_columns; ++j) {
      const Complex value = 0.5 * (schur[i][j] + std::conj(schur[j][i]));
      schur[i][j] = value; schur[j][i] = std::conj(value);
    }

  const double rank_rcond = std::max<size_t>(1, total_columns)
                            * static_cast<double>(std::numeric_limits<float>::epsilon());
  size_t component_rank = 0;
  ComplexMatrix normalized_inverse = hermitian_pseudoinverse(
      schur, rank_rcond, &component_rank);
  ComplexMatrix exact_inverse;
  if (invert_complex(schur, &exact_inverse)) {
    normalized_inverse = std::move(exact_inverse);
    component_rank = component_columns;
  }
  const std::vector<Complex> normalized_coefficients = multiply(normalized_inverse, schur_rhs);
  std::vector<Complex> coefficients(component_columns);
  for (size_t i = 0; i < component_columns; ++i)
    coefficients[i] = normalized_coefficients[i] / std::max(component_norm[i], tiny);
  for (size_t a = 0; a < 4; ++a) out.response[a] = coefficients[a];
  if (!finite_response(out.response)) { out.reason = "nonfinite/zero isolated response"; return out; }

  double fitted = 0.0;
  for (size_t i = 0; i < component_columns; ++i)
    fitted += std::real(std::conj(component_rhs[i]) * coefficients[i]);
  for (size_t f = 0; f < families.size(); ++f) {
    if (!(family_norm[f] > 0.0)) continue;
    Complex coupled{};
    for (size_t i = 0; i < component_columns; ++i)
      coupled += std::conj(cross[f][i]) * coefficients[i];
    const Complex family_coefficient = (family_rhs[f] - coupled) / family_norm[f];
    fitted += std::real(std::conj(family_rhs[f]) * family_coefficient);
  }
  const double residual = std::max(0.0, measurement_energy - fitted);
  out.residual_energy_fraction = measurement_energy > 0.0 ? residual / measurement_energy : 1.0;

  /* Schur-complement every other moving component as well as the LOS family block, then use the
   * minimum eigenvalue of the target's four-channel information group. A diagonal-only check can
   * falsely accept a target whose columns are duplicates of another component. */
  ComplexMatrix residual_gram(4, std::vector<Complex>(4));
  for (size_t i = 0; i < 4; ++i)
    for (size_t j = 0; j < 4; ++j) residual_gram[i][j] = schur[i][j];
  const size_t other_count = component_columns - 4;
  if (other_count) {
    ComplexMatrix other_gram(other_count, std::vector<Complex>(other_count));
    ComplexMatrix group_cross(4, std::vector<Complex>(other_count));
    for (size_t i = 0; i < other_count; ++i)
      for (size_t j = 0; j < other_count; ++j) other_gram[i][j] = schur[i + 4][j + 4];
    for (size_t i = 0; i < 4; ++i)
      for (size_t j = 0; j < other_count; ++j) group_cross[i][j] = schur[i][j + 4];
    const double other_rcond = std::max<size_t>(1, other_count + families.size())
                               * static_cast<double>(std::numeric_limits<float>::epsilon());
    size_t other_rank = 0;
    ComplexMatrix other_inverse = hermitian_pseudoinverse(other_gram, other_rcond, &other_rank);
    ComplexMatrix exact_other_inverse;
    if (invert_complex(other_gram, &exact_other_inverse))
      other_inverse = std::move(exact_other_inverse);
    for (size_t i = 0; i < 4; ++i)
      for (size_t j = 0; j < 4; ++j) {
        Complex explained{};
        for (size_t p = 0; p < other_count; ++p)
          for (size_t q = 0; q < other_count; ++q)
            explained += group_cross[i][p] * other_inverse[p][q]
                         * std::conj(group_cross[j][q]);
        residual_gram[i][j] -= explained;
      }
  }
  const double unique = std::clamp(minimum_hermitian_eigenvalue(residual_gram), 0.0, 1.0);
  out.unique_energy_fraction = unique;
  out.valid = unique > total_columns * static_cast<double>(std::numeric_limits<float>::epsilon());
  if (!out.valid) { out.reason = "target is not separable from surveyed LOS"; return out; }

  const size_t joint_rank = component_rank + family_rank;
  const size_t observed_samples = 4 * observed_cells;
  const double variance = std::max(residual / std::max<size_t>(1, observed_samples - std::min(observed_samples, joint_rank)),
      std::numeric_limits<float>::epsilon() * std::numeric_limits<float>::epsilon()
      * std::max(measurement_energy / std::max<size_t>(1, observed_samples),
                 std::numeric_limits<double>::min()));
  Matrix phase_cov(4, 4);
  for (size_t i = 0; i < 4; ++i)
    for (size_t j = 0; j < 4; ++j) {
      const Complex covariance = variance * normalized_inverse[i][j]
          / (std::max(component_norm[i], tiny) * std::max(component_norm[j], tiny));
      phase_cov(i, j) = 0.5 * std::real(covariance
                           / (out.response[i] * std::conj(out.response[j])));
    }
  Matrix difference(3, 4);
  for (size_t r = 0; r < 3; ++r) { difference(r, 0) = -1.0; difference(r, r + 1) = 1.0; }
  out.baseline_phase_covariance = positive_semidefinite(
      difference * symmetrized(phase_cov) * difference.transposed());
  out.phase_covariance_valid = true;
  return out;
}

AoaEstimate grid_free_upa_aoa(const std::array<Complex, 4>& response,
                              const ArrayGeometry& geometry, double fc,
                              const Matrix* response_covariance)
{
  AoaEstimate out;
  if (!geometry.configured || !(fc > 0.0)) { out.reason = "array geometry unavailable"; return out; }
  if (!finite_response(response)) { out.reason = "nonfinite/zero array response"; return out; }
  Vec3 null;
  Matrix basis;
  try { basis = plane_basis(geometry, &null); }
  catch (const std::exception& e) { out.reason = e.what(); return out; }
  const Matrix baselines = baseline_matrix(geometry);
  const Matrix design = baselines * basis;
  Matrix precision = Matrix::identity(3);
  if (response_covariance && response_covariance->rows() == 3)
    precision = pseudoinverse_symmetric(*response_covariance,
                                        3.0 * std::numeric_limits<double>::epsilon());
  const Matrix normal = design.transposed() * precision * design;
  const Matrix pseudo = pseudoinverse_symmetric(normal, 2.0 * std::numeric_limits<double>::epsilon())
                        * design.transposed() * precision;
  const double wavelength = C_MPS / fc;
  std::array<double, 3> measured{};
  std::array<int, 3> low{}, high{};
  for (size_t i = 0; i < 3; ++i) {
    measured[i] = std::arg(response[i + 1] * std::conj(response[0]));
    const Vec3 baseline{baselines(i, 0), baselines(i, 1), baselines(i, 2)};
    const double maximum_phase = 2.0 * PI * norm(baseline) / wavelength;
    low[i] = static_cast<int>(std::ceil((-maximum_phase - measured[i]) / (2.0 * PI)));
    high[i] = static_cast<int>(std::floor((maximum_phase - measured[i]) / (2.0 * PI)));
  }
  bool have_inside = false, have_outside = false;
  double best_inside_score = std::numeric_limits<double>::infinity();
  double best_outside_norm = std::numeric_limits<double>::infinity();
  double best_outside_score = std::numeric_limits<double>::infinity();
  Vec3 best{};
  for (int a = low[0]; a <= high[0]; ++a)
    for (int b = low[1]; b <= high[1]; ++b)
      for (int c = low[2]; c <= high[2]; ++c) {
        ++out.phase_wraps_tested;
        std::vector<double> path(3);
        const int wraps[3]{a, b, c};
        for (size_t i = 0; i < 3; ++i)
          path[i] = (measured[i] + 2.0 * PI * wraps[i]) * wavelength / (2.0 * PI);
        const auto coordinates = pseudo * path;
        Vec3 candidate;
        for (size_t r = 0; r < 3; ++r)
          candidate[r] = basis(r, 0) * coordinates[0] + basis(r, 1) * coordinates[1];
        const auto error = baselines * std::vector<double>{candidate.x, candidate.y, candidate.z};
        std::vector<double> residual(3);
        for (size_t i = 0; i < 3; ++i) residual[i] = error[i] - path[i];
        const double score = quadratic(residual, precision), candidate_norm = norm2(candidate);
        if (candidate_norm <= 1.0 && (!have_inside || score < best_inside_score)) {
          have_inside = true; best_inside_score = score; best = candidate;
        } else if (candidate_norm > 1.0
                   && (!have_outside || std::tuple(candidate_norm - 1.0, score)
                                      < std::tuple(best_outside_norm - 1.0, best_outside_score))) {
          have_outside = true; best_outside_norm = candidate_norm;
          best_outside_score = score; if (!have_inside) best = candidate;
        }
      }
  if (!have_inside && !have_outside) { out.reason = "no physically admissible phase wraps"; return out; }
  double projected_norm2 = norm2(best);
  out.visible_region_clipped = projected_norm2 > 1.0;
  if (out.visible_region_clipped) { best = best / std::sqrt(projected_norm2); projected_norm2 = 1.0; }
  const double complement = std::sqrt(std::max(0.0, 1.0 - projected_norm2));
  const Vec3 first = best + complement * null, second = best - complement * null;
  out.direction = dot(first, geometry.broadside) >= dot(second, geometry.broadside) ? first : second;
  out.direction = normalized(out.direction);
  out.azimuth_deg = std::atan2(out.direction.y, out.direction.x) * 180.0 / PI;
  out.elevation_deg = std::asin(std::clamp(out.direction.z, -1.0, 1.0)) * 180.0 / PI;
  std::array<Complex, 4> steering{{1.0, 1.0, 1.0, 1.0}};
  std::array<double, 3> residual_phase{};
  for (size_t i = 0; i < 3; ++i) {
    const Vec3 bl{baselines(i, 0), baselines(i, 1), baselines(i, 2)};
    const double expected = 2.0 * PI * dot(bl, out.direction) / wavelength;
    steering[i + 1] = std::polar(1.0, expected);
    residual_phase[i] = wrap_phase(measured[i] - expected);
  }
  Complex gain{}; for (size_t i = 0; i < 4; ++i) gain += std::conj(steering[i]) * response[i];
  gain /= 4.0;
  double residual_energy = 0.0, response_energy = 0.0;
  for (size_t i = 0; i < 4; ++i) {
    residual_energy += std::norm(response[i] - gain * steering[i]); response_energy += std::norm(response[i]);
  }
  out.relative_manifold_residual_energy = residual_energy
      / std::max(response_energy, std::numeric_limits<double>::min());
  out.phase_fit_residual_rms_rad = std::sqrt((residual_phase[0] * residual_phase[0]
      + residual_phase[1] * residual_phase[1] + residual_phase[2] * residual_phase[2]) / 3.0);

  Matrix baseline_cov(3, 3);
  if (response_covariance) baseline_cov = *response_covariance;
  else {
    const double noise = std::max(residual_energy / 2.0,
        std::numeric_limits<float>::epsilon() * std::numeric_limits<float>::epsilon()
        * std::max(response_energy, 1.0));
    std::array<double, 4> phase_variance{};
    for (size_t i = 0; i < 4; ++i)
      phase_variance[i] = noise / (2.0 * std::max(std::norm(gain * steering[i]),
                                                 std::numeric_limits<double>::min()));
    for (size_t r = 0; r < 3; ++r)
      for (size_t c = 0; c < 3; ++c)
        baseline_cov(r, c) = phase_variance[0] + (r == c ? phase_variance[r + 1] : 0.0);
  }
  const Matrix plane_pseudo = pseudoinverse_symmetric(design.transposed() * design)
                              * design.transposed();
  const Matrix plane_cov = plane_pseudo * baseline_cov * plane_pseudo.transposed()
                           * std::pow(wavelength / (2.0 * PI), 2.0);
  const auto pc = basis.transposed() * std::vector<double>{out.direction.x, out.direction.y, out.direction.z};
  const double normal_coordinate = dot(out.direction, null);
  const double normal_abs = std::abs(normal_coordinate);
  const double normal_sq = std::max(0.0, 1.0 - (pc[0] * pc[0] + pc[1] * pc[1]));
  const double normal_variance = std::max(0.0, 4.0 * quadratic(pc, plane_cov));
  const bool normal_resolved = normal_sq > 3.0 * std::sqrt(normal_variance);
  const double horizontal = std::hypot(out.direction.x, out.direction.y);
  Matrix direction_cov(3, 3), angle_cov(2, 2);
  if (!out.visible_region_clipped && normal_resolved
      && normal_abs > std::sqrt(std::numeric_limits<double>::epsilon())
      && horizontal > std::sqrt(std::numeric_limits<double>::epsilon())) {
    Matrix from_plane(3, 2);
    const double sign = normal_coordinate >= 0.0 ? 1.0 : -1.0;
    for (size_t r = 0; r < 3; ++r)
      for (size_t c = 0; c < 2; ++c)
        from_plane(r, c) = basis(r, c) - sign * null[r] * pc[c] / normal_abs;
    direction_cov = from_plane * plane_cov * from_plane.transposed();
    Matrix angle_j(2, 3);
    angle_j(0, 0) = -out.direction.y / (horizontal * horizontal);
    angle_j(0, 1) = out.direction.x / (horizontal * horizontal);
    angle_j(1, 0) = -out.direction.x * out.direction.z / horizontal;
    angle_j(1, 1) = -out.direction.y * out.direction.z / horizontal;
    angle_j(1, 2) = horizontal;
    angle_cov = angle_j * direction_cov * angle_j.transposed();
  } else {
    direction_cov = basis * plane_cov * basis.transposed();
    if (horizontal > std::sqrt(std::numeric_limits<double>::epsilon())) {
      Matrix angle_j(2, 3);
      angle_j(0, 0) = -out.direction.y / (horizontal * horizontal);
      angle_j(0, 1) = out.direction.x / (horizontal * horizontal);
      angle_j(1, 0) = -out.direction.x * out.direction.z / horizontal;
      angle_j(1, 1) = -out.direction.y * out.direction.z / horizontal;
      angle_j(1, 2) = horizontal;
      angle_cov = angle_j * direction_cov * angle_j.transposed();
      if (std::abs(dot(Vec3{angle_j(0,0),angle_j(0,1),angle_j(0,2)}, null))
          > std::sqrt(std::numeric_limits<double>::epsilon())) angle_cov(0,0)=std::max(angle_cov(0,0),PI*PI/3.0);
      if (std::abs(dot(Vec3{angle_j(1,0),angle_j(1,1),angle_j(1,2)}, null))
          > std::sqrt(std::numeric_limits<double>::epsilon())) angle_cov(1,1)=std::max(angle_cov(1,1),PI*PI/12.0);
    } else { angle_cov(0,0)=PI*PI/3.0; angle_cov(1,1)=PI*PI/12.0; }
  }
  out.direction_covariance = positive_semidefinite(direction_cov);
  out.covariance_rad2 = positive_semidefinite(angle_cov, std::numeric_limits<double>::epsilon());
  out.covariance_valid = true; out.valid = true;
  return out;
}

void attach_aoa(CfrWindow aligned, const std::vector<CleanComponent>& components,
                const Axes& axes, const PipelineConfig& config,
                std::vector<Detection>& detections, bool surveyed_los_available)
{
  if (detections.empty()) return;
  if (!config.aoa_enable || !config.array.configured || aligned.antennas != 4) {
    for (auto& d : detections) d.aoa.reason = "AoA disabled or four-channel array unavailable";
    return;
  }
  std::vector<uint8_t> selected = aoa_observed_mask(aligned, config.aoa_enable, config.aoa_ul_enable);
  if (std::none_of(selected.begin(), selected.end(), [](uint8_t x) { return x; })) {
    for (auto& d : detections) d.aoa.reason = "AoA source policy left no observed samples";
    return;
  }
  aligned.observed = selected;
  align_allocation_families(aligned, false);
  CfrWindow projected = aligned;
  subtract_allocation_family_static(projected);
  std::optional<std::array<Complex, 4>> los;
  if (surveyed_los_available)
    los = surveyed_los_steering(
        config.array, config.tx_position, config.rx_position, aligned.fc_hz);
  std::set<uint32_t> selected_iterations;
  for (const auto& d : detections) selected_iterations.insert(d.source_component_iteration);
  std::vector<std::pair<double, double>> nuisance;
  for (const auto& component : components)
    if (!selected_iterations.count(component.iteration))
      nuisance.emplace_back(component.range_bin * axes.range_res_m,
                            -(component.doppler_bin - aligned.rows / 2.0) * axes.rate_res_mps);
  for (auto& d : detections) {
    const auto direct_response = project_array_response(projected, projected.observed,
                                                         d.range_m, d.range_rate_mps);
    AoaEstimate direct = grid_free_upa_aoa(direct_response, config.array, aligned.fc_hz);
    AoaIsolation isolation;
    if (los)
      isolation = isolate_target_response(aligned, aligned.observed,
          d.range_m, d.range_rate_mps, nuisance, *los, 0.0);
    else
      isolation.reason = "surveyed illuminator geometry unavailable";
    AoaEstimate isolated;
    if (isolation.valid)
      isolated = grid_free_upa_aoa(isolation.response, config.array, aligned.fc_hz,
          isolation.phase_covariance_valid ? &isolation.baseline_phase_covariance : nullptr);
    else isolated.reason = isolation.reason;
    const bool prefer_isolated = std::abs(d.range_m) < axes.range_res_m && isolation.valid;
    AoaEstimate preferred = prefer_isolated ? isolated : direct;
    const AoaEstimate& fallback = prefer_isolated ? direct : isolated;
    const bool fallback_admissible = prefer_isolated || isolation.valid;
    if (!preferred.valid && fallback_admissible && fallback.valid) preferred = fallback;
    d.aoa = std::move(preferred); d.dwell_s = axes.dwell_s;
  }
}

} // namespace nr_isac
