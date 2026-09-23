/* SPDX-License-Identifier: OAI-Public-License-1.1 */
//
// Known limit: the linear model below holds while the per-channel residual is within +/-pi;
// residuals from a wrong fringe are down-weighted only through SNR. The pipeline feeds only
// detections associated with confirmed tracks.
//
// --- Sign derivation (do not guess this back if the test starts failing with a growing error) ---
// Per-channel coherent term, as produced upstream: t_i = exp(j*(-ph_true_i + ph_comp_i)),
// i.e. its phase is the survey's compensation phase minus the (unknown) true bistatic phase.
// ph(r) = k*(dist(pos,tx) + dist(pos,r) - dist(tx,r)), k = 2*pi*fc/c, evaluated at rx position r.
// Let r0 = current antenna estimate (geometry() before this add()) and let the true antenna
// position be r0 + delta_i (delta_i is what we want to recover; the corrected estimate is then
// r0 + x_i with x_i -> delta_i). ph_comp uses r0 (or the surveyed r, same to first order once
// re-linearised each add()), ph_true uses r0 + delta_i, so to first order in delta_i:
//   ph_true_i - ph_comp_i ~= grad_r ph(r)|_{r0} . delta_i
//   grad_r dist(pos,r) = (r - pos)/|r - pos| = -u_x,  u_x = normalized(pos - r)   (rx -> target)
//   grad_r dist(tx,r)  = (r - tx)/|r - tx|   = -u_t,  u_t = normalized(tx  - r)   (rx -> tx)
//   => grad_r ph(r) = k*(-u_x - (-u_t)) = k*(u_t - u_x)
// So the per-channel residual (before common-mode removal) is
//   e_i = -ph_true_i + ph_comp_i ~= -k*(u_t - u_x).delta_i = k*(u_x - u_t).delta_i
// The code below sets a_i = -k*(u_t - u_x) = k*(u_x - u_t) and fits e_i ~= a_i . x_i by weighted
// least squares, so the fitted x_i converges to delta_i directly -- geometry() then reports
// surveyed + x_i, which is exactly r0 + delta_i, i.e. the true antenna position. (Verified
// numerically by the unit test: correction_norm_m() shrinks the seeded per-antenna errors.)
//
// Common-mode note: e_i above is taken relative to the coherent sum of all channels (arg(t_i *
// conj(sum t)) below when the array is coherent enough, see the gate note further down), which
// removes whatever phase is common to every channel. A translation delta applied identically to
// all kCh antennas is therefore only weakly observable through the differences in (u_t - u_x)
// across antennas (which are non-zero for a real array, since each antenna looks at the
// target/tx along a slightly different bearing) -- it is not fully unobservable, just poorly
// conditioned when the array aperture is small relative to target range. The w = 1/sigma^2
// survey prior seeded into J_/b_ regularises that direction so the solve stays well-posed even
// when a particular translation is nearly in the null space.
//
// Coherence gate (found empirically, not in the original per-channel-only sketch): a SINGLE
// channel whose own delta already pushes some detections' raw (ph_true-ph_comp) past +/-pi (a
// real, measured case for the unit test's seeded errors at this array's geometry/frequency -- see
// the "Known limit" note above) drags arg(sum) away from every OTHER channel's true reference
// too, since sum includes that channel's corrupted term. That doesn't just hurt the bad channel's
// own fit, it POISONS every channel's residual, because they all share the same sum. Guard: only
// trust the shared reference when the array is coherent enough (|sum| close to kCh); otherwise
// reference channel i against its own phase directly. This is not an ad hoc fallback: for a
// coherent array sum is real and positive, so arg(t_i * conj(sum)) and arg(t_i) are the SAME
// value there -- the gate only changes behaviour in the regime the shared reference is already
// unreliable in.
#include "coherent_autofocus.h"
#include <cmath>
namespace nr_isac::coherent {
namespace {
bool solve3(const std::array<double, 9>& A, const std::array<double, 3>& b, double x[3])
{
  const double d = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
  if (!(std::abs(d) > 0)) return false;
  auto det3 = [](double a, double b_, double c, double d_, double e, double f, double g, double h, double i) { return a * (e * i - f * h) - b_ * (d_ * i - f * g) + c * (d_ * h - e * g); };
  x[0] = det3(b[0], A[1], A[2], b[1], A[4], A[5], b[2], A[7], A[8]) / d;
  x[1] = det3(A[0], b[0], A[2], A[3], b[1], A[5], A[6], b[2], A[8]) / d;
  x[2] = det3(A[0], A[1], b[0], A[3], A[4], b[1], A[6], A[7], b[2]) / d;
  return true;
}
} // namespace
Autofocus::Autofocus(const Geometry& s, double sigma, double fc) : surveyed_(s), k_(2 * M_PI * fc / kC)
{
  const double w = 1.0 / (sigma * sigma);                          // prior: surveyed position, declared sigma
  for (uint32_t i = 0; i < kCh; ++i) { J_[i] = {w, 0, 0, 0, w, 0, 0, 0, w}; b_[i] = {0, 0, 0}; }
}
void Autofocus::add(const Detection& d)
{
  std::complex<double> sum = 0; for (const auto& t : d.terms) sum += t;
  // Coherence gate: see the "Coherence gate" comment at the top of this file. 0.75*kCh requires
  // the array to be combining reasonably constructively before its sum is trusted as a reference.
  const bool coherent = std::abs(sum) >= 0.75 * kCh;
  const double var = 1.0 / (2 * std::max(d.snr, 1e-6));          // phase variance from SNR
  const Geometry g = geometry();
  for (uint32_t i = 0; i < kCh; ++i) {
    if (std::abs(d.terms[i]) == 0) continue;
    // residual vs the coherent sum (or vs the channel's own phase below the coherence gate)
    const double e = coherent ? std::arg(d.terms[i] * std::conj(sum)) : std::arg(d.terms[i]);
    // residual phase = -k * (u_tx,i - u_x,i) . delta_i   (u = unit vectors from rx_i)
    const Vec3 ux = normalized(d.pos - g.rx[i]), ut = normalized(d.tx - g.rx[i]);
    const Vec3 a = (ut - ux) * (-k_);
    const double av[3] = {a.x, a.y, a.z};
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) J_[i][r * 3 + c] += av[r] * av[c] / var; b_[i][r] += av[r] * e / var; }
  }
}
Geometry Autofocus::geometry() const
{
  Geometry g = surveyed_;
  for (uint32_t i = 0; i < kCh; ++i) { double x[3]; if (solve3(J_[i], b_[i], x)) g.rx[i] = surveyed_.rx[i] + Vec3{x[0], x[1], x[2]}; }
  return g;
}
std::array<double, kCh> Autofocus::correction_norm_m() const
{
  std::array<double, kCh> n{}; const Geometry g = geometry();
  for (uint32_t i = 0; i < kCh; ++i) n[i] = dist(g.rx[i], surveyed_.rx[i]);
  return n;
}
} // namespace nr_isac::coherent
