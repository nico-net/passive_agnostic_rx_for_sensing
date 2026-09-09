/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace nr_isac {

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;

  double& operator[](size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
  double operator[](size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(double s, Vec3 a) { return a * s; }
inline Vec3 operator/(Vec3 a, double s) { return {a.x / s, a.y / s, a.z / s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm2(Vec3 a) { return dot(a, a); }
inline double norm(Vec3 a) { return std::sqrt(norm2(a)); }
inline Vec3 normalized(Vec3 a)
{
  const double n = norm(a);
  if (!(n > 0.0) || !std::isfinite(n))
    throw std::invalid_argument("cannot normalize zero/nonfinite vector");
  return a / n;
}

class Matrix {
public:
  Matrix() = default;
  Matrix(size_t rows, size_t cols, double value = 0.0) : rows_(rows), cols_(cols), data_(rows * cols, value) {}

  size_t rows() const { return rows_; }
  size_t cols() const { return cols_; }
  double& operator()(size_t r, size_t c) { return data_.at(r * cols_ + c); }
  double operator()(size_t r, size_t c) const { return data_.at(r * cols_ + c); }
  const std::vector<double>& data() const { return data_; }

  static Matrix identity(size_t n)
  {
    Matrix result(n, n);
    for (size_t i = 0; i < n; ++i)
      result(i, i) = 1.0;
    return result;
  }

  Matrix transposed() const
  {
    Matrix result(cols_, rows_);
    for (size_t r = 0; r < rows_; ++r)
      for (size_t c = 0; c < cols_; ++c)
        result(c, r) = (*this)(r, c);
    return result;
  }

private:
  size_t rows_ = 0;
  size_t cols_ = 0;
  std::vector<double> data_;
};

inline Matrix operator+(const Matrix& a, const Matrix& b)
{
  if (a.rows() != b.rows() || a.cols() != b.cols())
    throw std::invalid_argument("matrix add shape mismatch");
  Matrix result(a.rows(), a.cols());
  for (size_t r = 0; r < a.rows(); ++r)
    for (size_t c = 0; c < a.cols(); ++c)
      result(r, c) = a(r, c) + b(r, c);
  return result;
}

inline Matrix operator-(const Matrix& a, const Matrix& b)
{
  if (a.rows() != b.rows() || a.cols() != b.cols())
    throw std::invalid_argument("matrix subtract shape mismatch");
  Matrix result(a.rows(), a.cols());
  for (size_t r = 0; r < a.rows(); ++r)
    for (size_t c = 0; c < a.cols(); ++c)
      result(r, c) = a(r, c) - b(r, c);
  return result;
}

inline Matrix operator*(const Matrix& a, const Matrix& b)
{
  if (a.cols() != b.rows())
    throw std::invalid_argument("matrix multiply shape mismatch");
  Matrix result(a.rows(), b.cols());
  for (size_t i = 0; i < a.rows(); ++i)
    for (size_t k = 0; k < a.cols(); ++k) {
      const double av = a(i, k);
      for (size_t j = 0; j < b.cols(); ++j)
        result(i, j) += av * b(k, j);
    }
  return result;
}

inline Matrix operator*(const Matrix& a, double scalar)
{
  Matrix result(a.rows(), a.cols());
  for (size_t r = 0; r < a.rows(); ++r)
    for (size_t c = 0; c < a.cols(); ++c)
      result(r, c) = a(r, c) * scalar;
  return result;
}

inline Matrix operator*(double scalar, const Matrix& a) { return a * scalar; }

inline std::vector<double> operator*(const Matrix& a, const std::vector<double>& x)
{
  if (a.cols() != x.size())
    throw std::invalid_argument("matrix-vector shape mismatch");
  std::vector<double> result(a.rows(), 0.0);
  for (size_t r = 0; r < a.rows(); ++r)
    for (size_t c = 0; c < a.cols(); ++c)
      result[r] += a(r, c) * x[c];
  return result;
}

inline double quadratic(const std::vector<double>& x, const Matrix& a)
{
  const std::vector<double> ax = a * x;
  double value = 0.0;
  for (size_t i = 0; i < x.size(); ++i)
    value += x[i] * ax[i];
  return value;
}

inline Matrix symmetrized(const Matrix& a)
{
  if (a.rows() != a.cols())
    throw std::invalid_argument("symmetrize needs square matrix");
  return (a + a.transposed()) * 0.5;
}

inline Matrix inverse(const Matrix& input, double relative_floor = 1e-12)
{
  if (input.rows() != input.cols() || input.rows() == 0)
    throw std::invalid_argument("inverse needs nonempty square matrix");
  const size_t n = input.rows();
  Matrix a = input;
  Matrix out = Matrix::identity(n);
  double scale = 0.0;
  for (size_t i = 0; i < n; ++i)
    scale = std::max(scale, std::abs(a(i, i)));
  const double floor = std::max(std::numeric_limits<double>::min(), scale * relative_floor);
  for (size_t col = 0; col < n; ++col) {
    size_t pivot = col;
    for (size_t row = col + 1; row < n; ++row)
      if (std::abs(a(row, col)) > std::abs(a(pivot, col)))
        pivot = row;
    if (std::abs(a(pivot, col)) <= floor) {
      a(col, col) += floor;
      pivot = col;
    }
    if (std::abs(a(pivot, col)) <= std::numeric_limits<double>::min())
      throw std::runtime_error("singular matrix");
    if (pivot != col)
      for (size_t j = 0; j < n; ++j) {
        std::swap(a(col, j), a(pivot, j));
        std::swap(out(col, j), out(pivot, j));
      }
    const double p = a(col, col);
    for (size_t j = 0; j < n; ++j) {
      a(col, j) /= p;
      out(col, j) /= p;
    }
    for (size_t row = 0; row < n; ++row) {
      if (row == col)
        continue;
      const double factor = a(row, col);
      for (size_t j = 0; j < n; ++j) {
        a(row, j) -= factor * a(col, j);
        out(row, j) -= factor * out(col, j);
      }
    }
  }
  return out;
}

inline std::array<double, 2> eigenvalues_symmetric_2x2(double a, double b, double d)
{
  const double mid = 0.5 * (a + d);
  const double radius = std::hypot(0.5 * (a - d), b);
  return {mid - radius, mid + radius};
}

struct SymmetricEigen3 {
  std::array<double, 3> values{};
  Matrix vectors{3, 3}; // eigenvectors are columns, ascending eigenvalue
};

inline SymmetricEigen3 symmetric_eigen_3x3(const Matrix& input)
{
  if (input.rows() != 3 || input.cols() != 3)
    throw std::invalid_argument("3x3 eigensolver shape mismatch");
  Matrix a = symmetrized(input);
  Matrix v = Matrix::identity(3);
  for (int sweep = 0; sweep < 32; ++sweep) {
    size_t p = 0, q = 1;
    double largest = std::abs(a(0, 1));
    for (size_t i = 0; i < 3; ++i)
      for (size_t j = i + 1; j < 3; ++j)
        if (std::abs(a(i, j)) > largest) {
          largest = std::abs(a(i, j)); p = i; q = j;
        }
    if (largest <= 1e-15 * std::max(1.0, std::abs(a(0,0)) + std::abs(a(1,1)) + std::abs(a(2,2))))
      break;
    const double phi = 0.5 * std::atan2(2.0 * a(p, q), a(q, q) - a(p, p));
    const double c = std::cos(phi), s = std::sin(phi);
    for (size_t k = 0; k < 3; ++k) {
      const double apk = a(p, k), aqk = a(q, k);
      a(p, k) = c * apk - s * aqk;
      a(q, k) = s * apk + c * aqk;
    }
    for (size_t k = 0; k < 3; ++k) {
      const double akp = a(k, p), akq = a(k, q);
      a(k, p) = c * akp - s * akq;
      a(k, q) = s * akp + c * akq;
      const double vkp = v(k, p), vkq = v(k, q);
      v(k, p) = c * vkp - s * vkq;
      v(k, q) = s * vkp + c * vkq;
    }
  }
  std::array<size_t, 3> order{0, 1, 2};
  std::sort(order.begin(), order.end(), [&](size_t i, size_t j) { return a(i, i) < a(j, j); });
  SymmetricEigen3 result;
  for (size_t c = 0; c < 3; ++c) {
    result.values[c] = a(order[c], order[c]);
    for (size_t r = 0; r < 3; ++r)
      result.vectors(r, c) = v(r, order[c]);
  }
  return result;
}

inline double trace(const Matrix& a)
{
  double value = 0.0;
  for (size_t i = 0; i < std::min(a.rows(), a.cols()); ++i)
    value += a(i, i);
  return value;
}

struct SymmetricEigen {
  std::vector<double> values;
  Matrix vectors; // eigenvectors are columns, ascending eigenvalue
};

/** Deterministic Jacobi eigensolver for the small real symmetric matrices used by the trackers. */
inline SymmetricEigen symmetric_eigen(const Matrix& input)
{
  if (!input.rows() || input.rows() != input.cols())
    throw std::invalid_argument("symmetric eigensolver needs a nonempty square matrix");
  const size_t n = input.rows();
  Matrix a = symmetrized(input), v = Matrix::identity(n);
  for (size_t sweep = 0; sweep < 64 * n * n; ++sweep) {
    size_t p = 0, q = n > 1 ? 1 : 0;
    double largest = 0.0;
    for (size_t i = 0; i < n; ++i)
      for (size_t j = i + 1; j < n; ++j)
        if (std::abs(a(i, j)) > largest) largest = std::abs(a(i, j)), p = i, q = j;
    double scale = 0.0;
    for (size_t i = 0; i < n; ++i) scale = std::max(scale, std::abs(a(i, i)));
    if (largest <= 32.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, scale)) break;
    const double tau = (a(q, q) - a(p, p)) / (2.0 * a(p, q));
    const double tangent = (tau >= 0.0 ? 1.0 : -1.0)
                           / (std::abs(tau) + std::sqrt(1.0 + tau * tau));
    const double cosine = 1.0 / std::sqrt(1.0 + tangent * tangent);
    const double sine = tangent * cosine;
    for (size_t k = 0; k < n; ++k) {
      if (k == p || k == q) continue;
      const double akp = a(k, p), akq = a(k, q);
      a(k, p) = a(p, k) = cosine * akp - sine * akq;
      a(k, q) = a(q, k) = sine * akp + cosine * akq;
    }
    const double app = a(p, p), aqq = a(q, q), apq = a(p, q);
    a(p, p) = cosine * cosine * app - 2.0 * sine * cosine * apq + sine * sine * aqq;
    a(q, q) = sine * sine * app + 2.0 * sine * cosine * apq + cosine * cosine * aqq;
    a(p, q) = a(q, p) = 0.0;
    for (size_t k = 0; k < n; ++k) {
      const double vkp = v(k, p), vkq = v(k, q);
      v(k, p) = cosine * vkp - sine * vkq;
      v(k, q) = sine * vkp + cosine * vkq;
    }
  }
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t i, size_t j) { return a(i, i) < a(j, j); });
  SymmetricEigen out{{}, Matrix(n, n)};
  out.values.resize(n);
  for (size_t c = 0; c < n; ++c) {
    out.values[c] = a(order[c], order[c]);
    for (size_t r = 0; r < n; ++r) out.vectors(r, c) = v(r, order[c]);
  }
  return out;
}

inline Matrix pseudoinverse_symmetric(const Matrix& input, double rcond = 1e-12)
{
  const SymmetricEigen eig = symmetric_eigen(input);
  const double largest = eig.values.empty() ? 0.0
                                             : std::max(0.0, eig.values.back());
  const double cutoff = std::max(std::numeric_limits<double>::min(), rcond * largest);
  Matrix out(input.rows(), input.cols());
  for (size_t k = 0; k < eig.values.size(); ++k) {
    if (eig.values[k] <= cutoff) continue;
    const double reciprocal = 1.0 / eig.values[k];
    for (size_t r = 0; r < input.rows(); ++r)
      for (size_t c = 0; c < input.cols(); ++c)
        out(r, c) += reciprocal * eig.vectors(r, k) * eig.vectors(c, k);
  }
  return out;
}

inline Matrix positive_semidefinite(const Matrix& input, double floor = 0.0)
{
  const SymmetricEigen eig = symmetric_eigen(input);
  Matrix out(input.rows(), input.cols());
  for (size_t k = 0; k < eig.values.size(); ++k) {
    const double value = std::max(floor, eig.values[k]);
    for (size_t r = 0; r < input.rows(); ++r)
      for (size_t c = 0; c < input.cols(); ++c)
        out(r, c) += value * eig.vectors(r, k) * eig.vectors(c, k);
  }
  return symmetrized(out);
}

} // namespace nr_isac
