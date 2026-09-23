/* SPDX-License-Identifier: OAI-Public-License-1.1 */
//
// Honest limit: this is a LINEAR phase model (Ĥ measured mod 2*pi), so it only converges inside
// its capture range -- the antenna error delta_i for which |z * a_i . delta_i| stays under pi,
// i.e. |delta_i| < pi / (z * |a_i|). At 3.45 GHz, |a_i| = k*|u_t-u_x| ranges up to ~2k (k =
// 2*pi*fc/c ~= 72.3 rad/m here), so the capture range is roughly pi/(3*144) ~= 7 mm in the worst
// direction and a few cm in a typical one. A tape-grade survey (sigma = 0.1 m) routinely seeds
// errors outside that range: the admission gate (step 3 below) and the per-channel accept test in
// geometry() (step 4) are what make the estimator DECLINE to act there instead of diverging --
// that is the correct, safe behaviour, not a missing feature. Pulling in a genuinely out-of-range
// error needs a coarse stage first (delay/envelope-domain refinement, unambiguous over many
// wavelengths) to shrink the error below this capture range before this linear stage can help;
// that coarse stage is future work, not attempted here.
//
// --- Sign derivation (re-check by deriving, do not guess it back) ---
// Per-channel coherent term, as produced upstream: t_i = exp(j*(-ph_true_i + ph_comp_i)), i.e.
// its phase is the survey's compensation phase minus the (unknown) true bistatic phase.
// ph(r) = k*(dist(pos,tx) + dist(pos,r) - dist(tx,r)), k = 2*pi*fc/c, evaluated at rx position r.
// Let r0 be an antenna position estimate and let the true antenna position be r0 + delta_i. Then
// to first order in delta_i:
//   ph_true_i - ph_comp_i ~= grad_r ph(r)|_{r0} . delta_i
//   grad_r dist(pos,r) = (r - pos)/|r - pos| = -u_x,  u_x = normalized(pos - r)   (rx -> target)
//   grad_r dist(tx,r)  = (r - tx)/|r - tx|   = -u_t,  u_t = normalized(tx  - r)   (rx -> tx)
//   => grad_r ph(r) = k*(-u_x - (-u_t)) = k*(u_t - u_x)
// So the raw per-channel phase, relative to r0, is
//   -ph_true_i + ph_comp_i ~= -k*(u_t - u_x).delta_i = k*(u_x - u_t).delta_i
// a_i below is set to -k*(u_t-u_x) = k*(u_x-u_t), so that fitting the observed phase against
// a_i . x makes the fitted x converge to delta_i directly (verified by the unit tests, and by the
// fact that flipping this sign makes every test regress instead of improve).
#include "coherent_autofocus.h"
#include <cmath>
namespace nr_isac::coherent {
namespace {
constexpr uint32_t kDim = 3 * kCh;

// Cholesky decomposition of a symmetric positive-definite matrix: A = L * L^T, L lower
// triangular. J_ is SPD by construction (a positive prior plus a sum of rank-1 y*y^T/var
// updates, all PSD), so this should never fail; return false defensively rather than throw.
bool cholesky(const Matrix& a, Matrix& l)
{
  const size_t n = a.rows();
  l = Matrix(n, n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j <= i; ++j) {
      double sum = a(i, j);
      for (size_t k = 0; k < j; ++k) sum -= l(i, k) * l(j, k);
      if (i == j) {
        if (!(sum > 0)) return false;
        l(i, j) = std::sqrt(sum);
      } else {
        l(i, j) = sum / l(j, j);
      }
    }
  }
  return true;
}

std::vector<double> cholesky_solve(const Matrix& l, const std::vector<double>& rhs)
{
  const size_t n = l.rows();
  std::vector<double> y(n, 0.0), x(n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    double s = rhs[i];
    for (size_t k = 0; k < i; ++k) s -= l(i, k) * y[k];
    y[i] = s / l(i, i);
  }
  for (size_t ii = 0; ii < n; ++ii) {
    const size_t i = n - 1 - ii;
    double s = y[i];
    for (size_t k = i + 1; k < n; ++k) s -= l(k, i) * x[k];
    x[i] = s / l(i, i);
  }
  return x;
}

// Full inverse via repeated Cholesky solves against the identity columns -- used to get the
// posterior covariance blocks for the admission gate. kDim=12, so this is cheap.
Matrix cholesky_inverse(const Matrix& l)
{
  const size_t n = l.rows();
  Matrix inv(n, n);
  std::vector<double> e(n, 0.0);
  for (size_t col = 0; col < n; ++col) {
    if (col > 0) e[col - 1] = 0.0;
    e[col] = 1.0;
    const std::vector<double> x = cholesky_solve(l, e);
    for (size_t r = 0; r < n; ++r) inv(r, col) = x[r];
  }
  return inv;
}
} // namespace

Autofocus::Autofocus(const Geometry& s, double sigma, double fc)
    : surveyed_(s), k_(2 * M_PI * fc / kC), J_(kDim, kDim), b_(kDim, 0.0)
{
  const double w = 1.0 / (sigma * sigma);
  for (uint32_t i = 0; i < kDim; ++i) J_(i, i) = w;
}

std::vector<double> Autofocus::solve() const
{
  Matrix l;
  if (!cholesky(J_, l)) return std::vector<double>(kDim, 0.0);
  return cholesky_solve(l, b_);
}

Geometry Autofocus::raw_geometry(const std::vector<double>& xhat) const
{
  Geometry g = surveyed_;
  for (uint32_t i = 0; i < kCh; ++i) g.rx[i] = surveyed_.rx[i] + Vec3{xhat[3 * i], xhat[3 * i + 1], xhat[3 * i + 2]};
  return g;
}

void Autofocus::add(const Detection& d)
{
  Matrix l;
  if (!cholesky(J_, l)) return;
  const std::vector<double> xhat = cholesky_solve(l, b_);
  const Matrix jinv = cholesky_inverse(l); // posterior covariance, for the admission gate

  const double var = 1.0 / (2.0 * std::max(d.snr, 1e-6));
  constexpr double kZ = 3.0; // conventional quantile for the admission gate

  const Geometry g = raw_geometry(xhat); // relinearise at the CURRENT joint estimate, ungated
  std::array<Vec3, kCh> a{};
  std::array<bool, kCh> admit{};
  uint32_t n_admit = 0;
  for (uint32_t i = 0; i < kCh; ++i) {
    if (std::abs(d.terms[i]) == 0) { admit[i] = false; continue; }
    const Vec3 ux = normalized(d.pos - g.rx[i]), ut = normalized(d.tx - g.rx[i]);
    a[i] = (ut - ux) * (-k_);
    // a_i^T P_i a_i, P_i = the 3x3 block of J^-1 for channel i (the joint posterior covariance,
    // not a per-channel-only inverse -- correlations across channels matter here).
    double a_p_a = 0.0;
    const double av[3] = {a[i].x, a[i].y, a[i].z};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        a_p_a += av[r] * jinv(3 * i + r, 3 * i + c) * av[c];
    const double capture = kZ * std::sqrt(std::max(a_p_a, 0.0) + var);
    admit[i] = capture < M_PI;
    if (admit[i]) ++n_admit;
  }
  if (n_admit < 2) return; // need at least 2 admitted channels to be worth fitting

  // ref = sum over admitted j of t_j * exp(-j*a_j.xhat_j) / |t_j|: a robust, amplitude-free
  // estimate of whatever phase all admitted channels currently agree on once each channel's own
  // best-known prediction is backed out.
  std::complex<double> ref = 0;
  for (uint32_t j = 0; j < kCh; ++j) {
    if (!admit[j]) continue;
    const double aj_xj = a[j].x * xhat[3 * j] + a[j].y * xhat[3 * j + 1] + a[j].z * xhat[3 * j + 2];
    ref += d.terms[j] * std::polar(1.0, -aj_xj) / std::abs(d.terms[j]);
  }
  if (std::abs(ref) == 0) return;

  // mean_row = (1/n_admit) * sum over admitted j of row_j, row_j = a_j placed in channel j's 3
  // columns of the kDim-wide row, 0 elsewhere.
  std::vector<double> mean_row(kDim, 0.0);
  for (uint32_t j = 0; j < kCh; ++j) {
    if (!admit[j]) continue;
    mean_row[3 * j] += a[j].x; mean_row[3 * j + 1] += a[j].y; mean_row[3 * j + 2] += a[j].z;
  }
  for (auto& v : mean_row) v /= static_cast<double>(n_admit);

  for (uint32_t i = 0; i < kCh; ++i) {
    if (!admit[i]) continue;
    const double ai_xi = a[i].x * xhat[3 * i] + a[i].y * xhat[3 * i + 1] + a[i].z * xhat[3 * i + 2];
    const double nu = std::arg(d.terms[i] * std::polar(1.0, -ai_xi) * std::conj(ref));

    std::vector<double> y(kDim, 0.0);
    for (uint32_t c = 0; c < kDim; ++c) y[c] = -mean_row[c];
    y[3 * i] += a[i].x; y[3 * i + 1] += a[i].y; y[3 * i + 2] += a[i].z;

    const double rhs = (nu + ai_xi) / var;
    for (uint32_t r = 0; r < kDim; ++r) {
      b_[r] += y[r] * rhs;
      for (uint32_t c = 0; c < kDim; ++c) J_(r, c) += y[r] * y[c] / var;
    }
  }
}

std::array<bool, kCh> Autofocus::applied() const
{
  std::array<bool, kCh> out{};
  const std::vector<double> xhat = solve();
  constexpr double kChiSq3_99 = 11.345; // chi-square, 3 dof, 1% false-alarm quantile
  for (uint32_t i = 0; i < kCh; ++i) {
    double stat = 0.0;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        stat += xhat[3 * i + r] * J_(3 * i + r, 3 * i + c) * xhat[3 * i + c];
    out[i] = stat > kChiSq3_99;
  }
  return out;
}

Geometry Autofocus::geometry() const
{
  Geometry g = surveyed_;
  const std::vector<double> xhat = solve();
  const std::array<bool, kCh> app = applied();
  for (uint32_t i = 0; i < kCh; ++i)
    if (app[i]) g.rx[i] = surveyed_.rx[i] + Vec3{xhat[3 * i], xhat[3 * i + 1], xhat[3 * i + 2]};
  return g;
}

std::array<double, kCh> Autofocus::correction_norm_m() const
{
  std::array<double, kCh> n{};
  const Geometry g = geometry();
  for (uint32_t i = 0; i < kCh; ++i) n[i] = dist(g.rx[i], surveyed_.rx[i]);
  return n;
}
} // namespace nr_isac::coherent
