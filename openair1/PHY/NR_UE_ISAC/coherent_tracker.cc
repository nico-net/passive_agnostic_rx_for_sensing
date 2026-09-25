/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_tracker.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
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
std::array<double, 9> cov_of(const Detection& d)
{
  std::array<double, 9> c = d.pos_cov;
  if (!(c[0] > 0 && c[4] > 0 && c[8] > 0)) c = {d.pos_sigma.x * d.pos_sigma.x, 0, 0, 0, d.pos_sigma.y * d.pos_sigma.y, 0, 0, 0, d.pos_sigma.z * d.pos_sigma.z};
  return c;
}
/** N(x; mu, A + B), the 3-D Gaussian with the two covariances summed. 0 if singular. */
double gauss3(const Vec3& x, const Vec3& mu, const std::array<double, 9>& A, const std::array<double, 9>& B)
{
  double S[9], Si[9], det;
  for (int i = 0; i < 9; ++i) S[i] = A[i] + B[i];
  if (!inv3(S, Si, &det) || !(det > 0)) return 0;
  const double nu[3] = {x.x - mu.x, x.y - mu.y, x.z - mu.z};
  double q = 0; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q += nu[i] * Si[i * 3 + j] * nu[j];
  return std::exp(-0.5 * q) / std::sqrt(std::pow(2 * M_PI, 3) * det);
}
} // namespace

std::vector<int> hungarian(const std::vector<std::vector<double>>& cost)
{
  const int n = (int)cost.size(); if (!n) return {};
  const int m = (int)cost[0].size();
  // Rectangular Kuhn-Munkres (e-maxx form): outer loop runs once per WORKER, the SMALLER side of
  // the matrix, not once per max(rows,cols) padded up to a square -- O(tracks^2 * detections)
  // instead of O(detections^3) for a typical few-tracks/many-detections CPI.
  //
  // Forbidden entries (>= kInf/2, the convention callers use) are NOT fed to the algorithm as the
  // raw 1e18 sentinel: ulp(1e18) is ~128, so once the u/v potentials accumulate sums of that
  // magnitude every real (small) cost difference underneath gets rounded away -- corrupting the
  // assignment whenever any row or column is entirely forbidden, which for a tracker is every CPI
  // (any track with no detection inside its gate). Measured in review: 4131/200000 random trials
  // suboptimal with the raw sentinel. Fixed with a data-derived big-M: large enough that trading a
  // real edge for a forbidden one can never pay off (bounded by the sum of all finite costs), but
  // small enough to stay well inside double precision.
  double finite_sum = 0.0;
  for (const auto& row : cost) for (double c : row) if (c < kInf / 2) finite_sum += std::abs(c);
  const double M = 2.0 * (1.0 + finite_sum);
  const bool transpose = n > m;
  const int W = transpose ? m : n;   // workers = outer-loop count = min(n, m)
  const int J = transpose ? n : m;   // jobs = the other side
  auto Craw = [&](int w, int j) { return transpose ? cost[j][w] : cost[w][j]; };            // 0-indexed (worker, job) -> ORIGINAL cost
  auto C = [&](int w, int j) { const double c = Craw(w, j); return c >= kInf / 2 ? M : c; }; // big-M substituted, algorithm-internal only
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
    if (Craw(w, jb) >= kInf / 2) continue;          // forbidden edge, tested against the ORIGINAL cost -> unmatched
    if (!transpose) row[w] = jb; else row[jb] = w;
  }
  return row;
}

CoherentTracker::CoherentTracker(const TrackerParams& p) : p_(p)
{
  if (!(p_.max_speed_mps > 0)) throw std::invalid_argument("TrackerParams.max_speed_mps must be > 0");
  if (!(p_.false_object_intensity_per_s > 0)) throw std::invalid_argument("TrackerParams.false_object_intensity_per_s must be > 0");
}

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
  if (!std::isfinite(d.range_rate_sigma) || d.range_rate_sigma <= 0) return;   // no rate measurement
  const Vec3 x{t.x[0], t.x[1], t.x[2]};
  // Unit vectors from each end (tx, rx) toward the target: h is d/dx of the bistatic path length
  // |x-tx| + |x-rx|, i.e. the gradient of predicted range-rate w.r.t. target velocity.
  const Vec3 h = normalized(x - d.tx) + normalized(x - p_.array_centroid);
  const double hv[6] = {0, 0, 0, h.x, h.y, h.z};
  const double pred = h.x * t.x[3] + h.y * t.x[4] + h.z * t.x[5];
  const double r_var = d.range_rate_sigma * d.range_rate_sigma;
  double PH[6], S = r_var;
  for (int a = 0; a < 6; ++a) { double s = 0; for (int b = 0; b < 6; ++b) s += t.P[a * 6 + b] * hv[b]; PH[a] = s; }
  for (int a = 0; a < 6; ++a) S += hv[a] * PH[a];
  const double nu = d.range_rate_mps - pred;
  if (nu * nu / S > 6.634896601021214) return;                               // chi2_1 99 %: outlier rate ignored
  double K[6]; for (int a = 0; a < 6; ++a) K[a] = PH[a] / S;
  for (int a = 0; a < 6; ++a) t.x[a] += K[a] * nu;
  // Joseph form (scalar measurement): P = (I-K h^T) P (I-K h^T)^T + K sigma^2 K^T. Same reasoning
  // as update_position's Joseph form: guarantees PSD under roundoff, so no post-hoc diagonal
  // repair is needed (or correct) the way the plain P -= K S K^T form required.
  std::array<double, 36> IKH{}; for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) IKH[a * 6 + b] = (a == b) - K[a] * hv[b];
  std::array<double, 36> T1{}, Pn{};
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) { double s = 0; for (int c = 0; c < 6; ++c) s += IKH[a * 6 + c] * t.P[c * 6 + b]; T1[a * 6 + b] = s; }
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) { double s = 0; for (int c = 0; c < 6; ++c) s += T1[a * 6 + c] * IKH[b * 6 + c]; s += K[a] * r_var * K[b]; Pn[a * 6 + b] = s; }
  t.P = Pn; symmetrize(t);
}

const std::vector<Track>& CoherentTracker::step(double t_s, double t_cpi_s, const std::vector<Detection>& dets, std::vector<int>* assoc)
{
  const double dt = (last_t_ < 0) ? t_cpi_s : std::max(0.0, t_s - last_t_); last_t_ = t_s;
  for (Track& t : tracks_) predict(t, dt);
  const Volume& V = p_.volume;
  const double vol = std::max(1e-9, (V.x1 - V.x0) * (V.y1 - V.y0) * (V.z1 - V.z0));
  // Clutter intensity is MEASURED, not assumed, and it is SPATIAL: a kernel density of the past
  // in-volume detections no confirmed track claimed, each kernel being that detection's own
  // position covariance (plus the evaluated detection's), so the bandwidth is the measurement
  // uncertainty and nothing is tuned. The declared intensity enters as one pseudo-event spread
  // uniformly over the volume (same Laplace convention as pd()). A receiver whose false detections
  // pile up in one region (the direct-path cell) would otherwise over-credit every chance
  // association there and confirm ghosts.
  // ponytail: whole-epoch history, O(history) per association; window/grid it if epochs run for hours.
  const size_t h0 = clutter_hist_.size();
  auto clutter_density = [&](const Detection& d, uint64_t self) {
    const std::array<double, 9> Cd = cov_of(d);
    double k = 0;
    // A track's own tentative trail is not clutter against itself (else a real target suppresses
    // its own confirmation); every other unclaimed detection is.
    for (size_t i = 0; i < h0; ++i) if (clutter_hist_[i].owner != self) k += gauss3(d.pos, clutter_hist_[i].pos, clutter_hist_[i].cov, Cd);
    return std::max(1e-300, (k + 1.0 / vol) / (clutter_t_ + 1.0 / p_.false_object_intensity_per_s) * t_cpi_s);
  };
  const double q0 = p_.max_speed_mps * p_.max_speed_mps;          // declared bound, stage-10 convention
  std::vector<std::vector<double>> cost(tracks_.size(), std::vector<double>(dets.size(), kInf));
  for (size_t i = 0; i < tracks_.size(); ++i) for (size_t j = 0; j < dets.size(); ++j) {
    double ld; const double q = position_nis(tracks_[i], dets[j], &ld); if (q <= kGate3) cost[i][j] = q + ld; }
  std::vector<int> a = hungarian(cost);
  std::vector<char> used(dets.size(), 0);
  // owner[j]: id of the track detection j feeds (0 none); claimed = fed a CONFIRMED track.
  std::vector<uint64_t> owner(dets.size(), 0); std::vector<char> claimed(dets.size(), 0);
  for (size_t i = 0; i < a.size() && i < tracks_.size(); ++i) if (a[i] >= 0) { owner[(size_t)a[i]] = tracks_[i].id; claimed[(size_t)a[i]] = tracks_[i].confirmed; }
  const double pd = this->pd();
  for (size_t i = 0; i < tracks_.size(); ++i) {
    Track& t = tracks_[i];
    if (i < a.size() && a[i] >= 0) {
      const Detection& d = dets[(size_t)a[i]]; used[(size_t)a[i]] = 1;
      double ld; const double q = position_nis(t, d, &ld);
      const double like = std::exp(-0.5 * q) / std::sqrt(std::pow(2 * M_PI, 3) * std::exp(ld));
      t.llr += std::log(std::max(1e-300, pd * like / clutter_density(d, t.id)));
      double nis = 0; update_position(t, d, &nis); update_rate(t, d);
      t.nis_sum += nis; ++t.nis_n; ++t.hits;
      // Covariance matching (Mehra): q tracks the running mean NIS toward its theoretical value of
      // 3 (chi2_3 mean at correct sizing), scaled from the declared q0 seed. Absolute, not
      // incremental -- no clamp, no first-hit special case needed, since nis_n>=1 is guaranteed
      // right after a hit (++t.nis_n above).
      t.q = q0 * (t.nis_sum / t.nis_n) / 3.0;
      if (t.confirmed) pd_hits_ += 1;
    } else {
      t.llr += std::log(std::max(1e-300, 1 - pd)); ++t.misses;
      if (t.confirmed) pd_misses_ += 1;
    }
    // The volume floor is the ground: a hard physical constraint (estimate projection onto z >= z0,
    // no velocity into it). The other faces only bound surveillance, so they are not projected.
    if (t.x[2] < V.z0) { t.x[2] = V.z0; t.x[5] = std::max(0.0, t.x[5]); }
    if (!t.confirmed && t.llr >= kConfirm) t.confirmed = true;
    // SPRT restart convention: a confirmed track's evidence is capped at the confirm threshold, so a
    // departed target is deleted after exactly kConfirm - kDelete of miss evidence, not after all the
    // hits it banked (100 hits at ~+5 each would coast ~40 s).
    if (t.confirmed) t.llr = std::min(t.llr, kConfirm);
  }
  // Snapshot assoc against PRE-erase indices (matches hungarian's `a`, whose domain is exactly
  // tracks_'s index space here, since erase()/new-track push haven't run yet), then remap those
  // indices to post-erase positions once tracks_ is compacted below. Building assoc only AFTER
  // erase() (against post-erase tracks_.size() but pre-erase indices in `a`) is wrong whenever any
  // track -- in particular one BEFORE the associated one -- gets deleted in the same step.
  std::vector<int> pre_erase_assoc;
  if (assoc) { pre_erase_assoc.assign(dets.size(), -1); for (size_t i = 0; i < a.size(); ++i) if (a[i] >= 0) pre_erase_assoc[(size_t)a[i]] = (int)i; }
  std::vector<int> old_to_new(tracks_.size(), -1);
  { int nxt = 0; for (size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].llr > kDelete) old_to_new[i] = nxt++; }
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [](const Track& t) { return t.llr <= kDelete; }), tracks_.end());
  if (assoc) { assoc->assign(dets.size(), -1); for (size_t j = 0; j < pre_erase_assoc.size(); ++j) if (pre_erase_assoc[j] >= 0) (*assoc)[j] = old_to_new[(size_t)pre_erase_assoc[j]]; }
  for (size_t j = 0; j < dets.size(); ++j) if (!used[j]) {
    Track t; t.id = next_id_++; t.q = q0;
    t.x = {dets[j].pos.x, dets[j].pos.y, dets[j].pos.z, 0, 0, 0};
    t.P.fill(0); P(t, 0, 0) = dets[j].pos_sigma.x * dets[j].pos_sigma.x; P(t, 1, 1) = dets[j].pos_sigma.y * dets[j].pos_sigma.y;
    P(t, 2, 2) = dets[j].pos_sigma.z * dets[j].pos_sigma.z; P(t, 3, 3) = P(t, 4, 4) = P(t, 5, 5) = q0;
    t.hits = 1; owner[j] = t.id; tracks_.push_back(t);
  }
  for (size_t j = 0; j < dets.size(); ++j) {
    const Vec3& x = dets[j].pos;
    if (!claimed[j] && x.x >= V.x0 && x.x <= V.x1 && x.y >= V.y0 && x.y <= V.y1 && x.z >= V.z0 && x.z <= V.z1)
      clutter_hist_.push_back({x, cov_of(dets[j]), owner[j]});
  }
  clutter_t_ += t_cpi_s;
  return tracks_;
}
} // namespace nr_isac::coherent
