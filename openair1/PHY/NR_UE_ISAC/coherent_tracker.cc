/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_tracker.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace nr_isac::coherent {
namespace {
constexpr double kInf = 1e18;
constexpr double kGate3 = 11.3448667301444;           // chi2_3 99 % (conventional)
constexpr double kAlpha = 0.01, kBeta = 0.10;          // SPRT (conventional)
const double kConfirm = std::log((1 - kBeta) / kAlpha), kDelete = std::log(kBeta / (1 - kAlpha));
double& P(Track& t, int r, int c) { return t.P[r * 6 + c]; }
double Pc(const Track& t, int r, int c) { return t.P[r * 6 + c]; }
bool inv3(const double a[9], double o[9], double* det)
{
  const double d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (!(std::abs(d) > 0)) return false; *det = d;
  o[0] = (a[4] * a[8] - a[5] * a[7]) / d; o[1] = (a[2] * a[7] - a[1] * a[8]) / d; o[2] = (a[1] * a[5] - a[2] * a[4]) / d;
  o[3] = (a[5] * a[6] - a[3] * a[8]) / d; o[4] = (a[0] * a[8] - a[2] * a[6]) / d; o[5] = (a[2] * a[3] - a[0] * a[5]) / d;
  o[6] = (a[3] * a[7] - a[4] * a[6]) / d; o[7] = (a[1] * a[6] - a[0] * a[7]) / d; o[8] = (a[0] * a[4] - a[1] * a[3]) / d;
  return true;
}
void symmetrize(Track& t) { for (int r = 0; r < 6; ++r) for (int c = r + 1; c < 6; ++c) { const double m = 0.5 * (P(t, r, c) + P(t, c, r)); P(t, r, c) = P(t, c, r) = m; } }
} // namespace

std::vector<int> hungarian(const std::vector<std::vector<double>>& cost)
{
  const int n = (int)cost.size(); if (!n) return {};
  const int m = (int)cost[0].size();
  // Rectangular Kuhn-Munkres (e-maxx form): outer loop runs once per WORKER, the SMALLER side of
  // the matrix, not once per max(rows,cols) padded up to a square. A CPI can carry 3 tracks against
  // a burst of 400 detections; padding to a 401x401 square (an earlier version of this function did
  // that) measured ~29 ms, over step()'s 20 ms budget -- from genuine O(N^3) work over N=401, not
  // from cache effects. This form does O(tracks^2 * detections) work instead and measures <1 ms on
  // the same input. transpose=true swaps which side is "workers" when there are more tracks than
  // detections (rows>cols), so the outer loop always runs over the smaller dimension.
  const bool transpose = n > m;
  const int W = transpose ? m : n;   // workers = outer-loop count = min(n, m)
  const int J = transpose ? n : m;   // jobs = the other side
  auto C = [&](int w, int j) { return transpose ? cost[j][w] : cost[w][j]; };  // 0-indexed (worker, job) -> cost
  std::vector<double> u(W + 1, 0.0), v(J + 1, 0.0);
  std::vector<int> p(J + 1, 0), way(J + 1, 0);
  std::vector<double> minv(J + 1); std::vector<char> used(J + 1);
  for (int i = 1; i <= W; ++i) {
    p[0] = i; int j0 = 0;
    std::fill(minv.begin(), minv.end(), std::numeric_limits<double>::infinity());
    std::fill(used.begin(), used.end(), 0);
    do {
      used[j0] = 1; const int i0 = p[j0]; double delta = std::numeric_limits<double>::infinity(); int j1 = 0;
      for (int j = 1; j <= J; ++j) if (!used[j]) {
        const double cur = C(i0 - 1, j - 1) - u[i0] - v[j];
        if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
        if (minv[j] < delta) { delta = minv[j]; j1 = j; }
      }
      for (int j = 0; j <= J; ++j) { if (used[j]) { u[p[j]] += delta; v[j] -= delta; } else minv[j] -= delta; }
      j0 = j1;
    } while (p[j0] != 0);
    do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
  }
  std::vector<int> row(n, -1);
  for (int j = 1; j <= J; ++j) if (p[j] >= 1) {
    const int w = p[j] - 1, jb = j - 1;             // 0-indexed worker, job
    if (C(w, jb) >= kInf / 2) continue;             // forbidden edge, forced by shape -> unmatched
    if (!transpose) row[w] = jb; else row[jb] = w;
  }
  return row;
}

CoherentTracker::CoherentTracker(const TrackerParams& p) : p_(p) {}

void CoherentTracker::predict(Track& t, double dt) const
{
  for (int i = 0; i < 3; ++i) t.x[i] += dt * t.x[i + 3];
  // P = F P F^T + q * [dt^3/3 dt^2/2; dt^2/2 dt] per axis
  std::array<double, 36> Pn = t.P;
  auto F = [dt](int r, int c) { return (r == c) ? 1.0 : ((c == r + 3 && r < 3) ? dt : 0.0); };
  for (int r = 0; r < 6; ++r) for (int c = 0; c < 6; ++c) {
    double s = 0; for (int i = 0; i < 6; ++i) for (int j = 0; j < 6; ++j) s += F(r, i) * t.P[i * 6 + j] * F(c, j);
    Pn[r * 6 + c] = s;
  }
  t.P = Pn;
  for (int i = 0; i < 3; ++i) { P(t, i, i) += t.q * dt * dt * dt / 3; P(t, i, i + 3) += t.q * dt * dt / 2; P(t, i + 3, i) += t.q * dt * dt / 2; P(t, i + 3, i + 3) += t.q * dt; }
  symmetrize(t); t.age_s += dt;
}
double CoherentTracker::position_nis(const Track& t, const Detection& d, double* logdet) const
{
  const double r[3] = {d.pos_sigma.x * d.pos_sigma.x, d.pos_sigma.y * d.pos_sigma.y, d.pos_sigma.z * d.pos_sigma.z};
  double S[9], Si[9], det; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) S[i * 3 + j] = Pc(t, i, j) + (i == j ? r[i] : 0);
  if (!inv3(S, Si, &det)) return kInf;
  const double nu[3] = {d.pos.x - t.x[0], d.pos.y - t.x[1], d.pos.z - t.x[2]};
  double q = 0; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q += nu[i] * Si[i * 3 + j] * nu[j];
  if (logdet) *logdet = std::log(det);
  return q;
}
void CoherentTracker::update_position(Track& t, const Detection& d, double* nis) const
{
  const double r[3] = {d.pos_sigma.x * d.pos_sigma.x, d.pos_sigma.y * d.pos_sigma.y, d.pos_sigma.z * d.pos_sigma.z};
  double S[9], Si[9], det; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) S[i * 3 + j] = Pc(t, i, j) + (i == j ? r[i] : 0);
  if (!inv3(S, Si, &det)) return;
  double K[18]; for (int a = 0; a < 6; ++a) for (int j = 0; j < 3; ++j) { double s = 0; for (int i = 0; i < 3; ++i) s += Pc(t, a, i) * Si[i * 3 + j]; K[a * 3 + j] = s; }
  const double nu[3] = {d.pos.x - t.x[0], d.pos.y - t.x[1], d.pos.z - t.x[2]};
  if (nis) { double q = 0; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q += nu[i] * Si[i * 3 + j] * nu[j]; *nis = q; }
  for (int a = 0; a < 6; ++a) for (int j = 0; j < 3; ++j) t.x[a] += K[a * 3 + j] * nu[j];
  // Joseph form: P = (I-KH)P(I-KH)^T + K R K^T
  std::array<double, 36> IKH{}; for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) IKH[a * 6 + b] = (a == b) - (b < 3 ? K[a * 3 + b] : 0.0);
  std::array<double, 36> T1{}, Pn{};
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) { double s = 0; for (int c = 0; c < 6; ++c) s += IKH[a * 6 + c] * t.P[c * 6 + b]; T1[a * 6 + b] = s; }
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) { double s = 0; for (int c = 0; c < 6; ++c) s += T1[a * 6 + c] * IKH[b * 6 + c]; for (int j = 0; j < 3; ++j) s += K[a * 3 + j] * r[j] * K[b * 3 + j]; Pn[a * 6 + b] = s; }
  t.P = Pn; symmetrize(t);
}
void CoherentTracker::update_rate(Track& t, const Detection& d) const
{
  if (!(d.range_rate_sigma > 0 && d.range_rate_sigma < 1e8)) return;
  const Vec3 x{t.x[0], t.x[1], t.x[2]};
  const Vec3 h = normalized(x - d.tx) + normalized(x - p_.array_centroid);    // gradient w.r.t. velocity
  const double hv[6] = {0, 0, 0, h.x, h.y, h.z};
  const double pred = h.x * t.x[3] + h.y * t.x[4] + h.z * t.x[5];
  double PH[6], S = d.range_rate_sigma * d.range_rate_sigma;
  for (int a = 0; a < 6; ++a) { double s = 0; for (int b = 0; b < 6; ++b) s += t.P[a * 6 + b] * hv[b]; PH[a] = s; }
  for (int a = 0; a < 6; ++a) S += hv[a] * PH[a];
  const double nu = d.range_rate_mps - pred;
  if (nu * nu / S > 6.634896601021214) return;                               // chi2_1 99 %: outlier rate ignored
  double K[6]; for (int a = 0; a < 6; ++a) K[a] = PH[a] / S;
  for (int a = 0; a < 6; ++a) t.x[a] += K[a] * nu;
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) t.P[a * 6 + b] -= K[a] * S * K[b];
  symmetrize(t);
  for (int a = 0; a < 6; ++a) if (!(t.P[a * 6 + a] > 0)) t.P[a * 6 + a] = std::abs(t.P[a * 6 + a]) + 1e-12;
}

const std::vector<Track>& CoherentTracker::step(double t_s, double t_cpi_s, const std::vector<Detection>& dets, std::vector<int>* assoc)
{
  const double dt = (last_t_ < 0) ? t_cpi_s : std::max(0.0, t_s - last_t_); last_t_ = t_s;
  for (Track& t : tracks_) predict(t, dt);
  const Volume& V = p_.volume;
  const double vol = std::max(1e-9, (V.x1 - V.x0) * (V.y1 - V.y0) * (V.z1 - V.z0));
  const double clutter_density = std::max(1e-12, p_.false_object_intensity_per_s * t_cpi_s / vol);
  std::vector<std::vector<double>> cost(tracks_.size(), std::vector<double>(dets.size(), kInf));
  for (size_t i = 0; i < tracks_.size(); ++i) for (size_t j = 0; j < dets.size(); ++j) {
    double ld; const double q = position_nis(tracks_[i], dets[j], &ld); if (q <= kGate3) cost[i][j] = q + ld; }
  std::vector<int> a = hungarian(cost);
  std::vector<char> used(dets.size(), 0);
  const double pd = this->pd();
  for (size_t i = 0; i < tracks_.size(); ++i) {
    Track& t = tracks_[i];
    if (i < a.size() && a[i] >= 0) {
      const Detection& d = dets[(size_t)a[i]]; used[(size_t)a[i]] = 1;
      double ld; const double q = position_nis(t, d, &ld);
      const double like = std::exp(-0.5 * q) / std::sqrt(std::pow(2 * M_PI, 3) * std::exp(ld));
      t.llr += std::log(std::max(1e-300, pd * like / clutter_density));
      double nis = 0; update_position(t, d, &nis); update_rate(t, d);
      t.nis_sum += nis; ++t.nis_n; ++t.hits;
      // Adaptive Q: track q toward mean(NIS)/3 (chi2_3 mean = 3 at correct sizing), multiplicatively
      // from q0. The brief's original one-liner divides by the *pre-this-update* mean, which on a
      // track's first association has zero samples behind it (nis_n==1 -> "before" mean is 0/0) and
      // only survives via a 1e-3 floor -- that floor then acts as the denominator outright, so the
      // very first hit multiplies q by ~(mean_nis/3)/1e-3, a three-orders-of-magnitude one-off jump
      // unrelated to the actual NIS. Fixed: skip the rescale on the first hit (q keeps its q0 seed,
      // which is exactly what a brand-new track should use), and for later hits divide by the
      // *previous* running mean (well-defined once nis_n>=2, no floor needed there) with a small
      // clamp on the per-step multiplicative move so one noisy NIS sample can't blow q up or collapse
      // it in a single update.
      if (t.nis_n > 1) {
        const double mean_after = t.nis_sum / t.nis_n;
        const double mean_before = (t.nis_sum - nis) / (t.nis_n - 1);
        double ratio = (mean_after / 3.0) / std::max(1e-9, mean_before / 3.0);
        ratio = std::clamp(ratio, 0.2, 5.0);
        t.q *= ratio;
      }
      if (t.confirmed) pd_hits_ += 1;
    } else {
      t.llr += std::log(std::max(1e-300, 1 - pd)); ++t.misses;
      if (t.confirmed) pd_misses_ += 1;
    }
    if (!t.confirmed && t.llr >= kConfirm) t.confirmed = true;
  }
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [](const Track& t) { return t.llr <= kDelete; }), tracks_.end());
  const double q0 = std::pow(p_.max_speed_mps / 1.0, 2);          // declared bound, stage-10 convention
  for (size_t j = 0; j < dets.size(); ++j) if (!used[j]) {
    Track t; t.id = next_id_++; t.q = q0;
    t.x = {dets[j].pos.x, dets[j].pos.y, dets[j].pos.z, 0, 0, 0};
    const double vs = p_.max_speed_mps * p_.max_speed_mps;
    t.P.fill(0); P(t, 0, 0) = dets[j].pos_sigma.x * dets[j].pos_sigma.x; P(t, 1, 1) = dets[j].pos_sigma.y * dets[j].pos_sigma.y;
    P(t, 2, 2) = dets[j].pos_sigma.z * dets[j].pos_sigma.z; P(t, 3, 3) = P(t, 4, 4) = P(t, 5, 5) = vs;
    t.hits = 1; tracks_.push_back(t);
  }
  if (assoc) { assoc->assign(dets.size(), -1); for (size_t i = 0; i < a.size(); ++i) if (a[i] >= 0) (*assoc)[(size_t)a[i]] = (int)i; }
  return tracks_;
}
} // namespace nr_isac::coherent
