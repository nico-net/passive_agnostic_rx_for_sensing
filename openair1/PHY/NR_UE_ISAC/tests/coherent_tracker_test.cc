/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_tracker.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;
static Detection det(Vec3 p, double sd) { Detection d; d.pos = d.pos_env = p; d.pos_sigma = {sd, sd, sd}; d.snr = 50; d.range_rate_sigma = std::numeric_limits<double>::infinity(); d.refined = true; return d; }
// Full 6x6 positive-definiteness via Cholesky (not just the diagonal): P is PD iff every Cholesky
// pivot is strictly positive.
static bool cholesky_pd(const std::array<double, 36>& P) {
  double L[36] = {0};
  for (int i = 0; i < 6; ++i) for (int j = 0; j <= i; ++j) {
    double s = 0; for (int k = 0; k < j; ++k) s += L[i * 6 + k] * L[j * 6 + k];
    if (i == j) { const double diag = P[i * 6 + i] - s; if (!(diag > 0)) return false; L[i * 6 + i] = std::sqrt(diag); }
    else L[i * 6 + j] = (P[i * 6 + j] - s) / L[j * 6 + j];
  }
  return true;
}
// Brute-force reference for hungarian(): minimum-cost injective matching of the smaller side into
// the larger, forbidden edges (>= kInf/2) substituted with the SAME data-derived big-M hungarian()
// itself derives (M = 2*(1 + sum|finite costs|)), so the two are directly comparable in the same
// units. Exhaustive over the smaller dimension (<=6 here), cheap.
static double brute_force_min_cost(const std::vector<std::vector<double>>& cost) {
  const double kInf = 1e18;
  const int n = (int)cost.size(), m = (int)cost[0].size();
  double finite_sum = 0.0; for (const auto& row : cost) for (double c : row) if (c < kInf / 2) finite_sum += std::abs(c);
  const double M = 2.0 * (1.0 + finite_sum);
  const bool tr = n > m; const int R = tr ? m : n, Cn = tr ? n : m;
  auto at = [&](int r, int c) { return tr ? cost[c][r] : cost[r][c]; };
  std::vector<char> used_col(Cn, 0);
  double best = std::numeric_limits<double>::infinity();
  std::function<void(int, double)> rec = [&](int row, double acc) {
    if (acc >= best) return;
    if (row == R) { best = std::min(best, acc); return; }
    for (int c = 0; c < Cn; ++c) if (!used_col[c]) {
      const double e = at(row, c) < kInf / 2 ? at(row, c) : M;
      used_col[c] = 1; rec(row + 1, acc + e); used_col[c] = 0;
    }
  };
  rec(0, 0.0);
  return best;
}
static double hungarian_result_cost(const std::vector<std::vector<double>>& cost, const std::vector<int>& row) {
  const double kInf = 1e18;
  const int n = (int)cost.size(), m = (int)cost[0].size();
  double finite_sum = 0.0; for (const auto& r : cost) for (double c : r) if (c < kInf / 2) finite_sum += std::abs(c);
  const double M = 2.0 * (1.0 + finite_sum);
  const int small = std::min(n, m);
  int used_real = 0; double sum_real = 0.0;
  for (int i = 0; i < n; ++i) if (row[i] >= 0) { ++used_real; sum_real += cost[i][row[i]]; }
  return (small - used_real) * M + sum_real;
}
int main() {
  // hungarian basics
  { const double I = 1e18; auto a = hungarian({{1, 5}, {5, 1}}); require(a[0] == 0 && a[1] == 1, "diag");
    auto b = hungarian({{I, I}, {3, I}}); require(b[0] == -1 && b[1] == 0, "forbidden"); }
  TrackerParams tp; tp.max_speed_mps = 20; tp.false_object_intensity_per_s = 1.0; tp.array_centroid = {5, 5, 2};
  CoherentTracker trk(tp);
  std::mt19937 rng(9); std::normal_distribution<double> n(0, 0.3); std::uniform_real_distribution<double> u(-15, 15), uz(0, 30);
  const double T = 0.075;
  for (int k = 0; k < 120; ++k) {
    const double t = k * T;
    std::vector<Detection> D;
    D.push_back(det(Vec3{-10 + 1.2 * t + n(rng), 3 + n(rng), 1 + n(rng)}, 0.4));      // person
    D.push_back(det(Vec3{-14 + 8.0 * t + n(rng), -6 + n(rng), 0.8 + n(rng)}, 0.4));   // car
    if (k % 5) D.push_back(det(Vec3{8 - 3 * t + n(rng), 8 + 2 * t + n(rng), 15 + n(rng)}, 0.4)); // drone, P_D 0.8
    if (k % 3 == 0) D.push_back(det(Vec3{u(rng), u(rng), uz(rng)}, 0.4));              // clutter
    trk.step(t, T, D);
  }
  const auto& tr = trk.step(120 * T, T, {});
  int confirmed = 0; for (const Track& x : tr) { if (x.confirmed) ++confirmed;
    for (double v : x.x) require(std::isfinite(v), "state finite");
    require(cholesky_pd(x.P), "covariance full PD (cholesky)"); }
  require(confirmed == 3, "exactly the 3 real targets confirmed (clutter not)");
  require(trk.pd() > 0.6 && trk.pd() < 1.0, "P_D estimated online");
  // burst (Review Focus 4): 400 detections in one CPI stays fast
  std::vector<Detection> burst; for (int i = 0; i < 400; ++i) burst.push_back(det(Vec3{u(rng), u(rng), uz(rng)}, 0.4));
  const auto t0 = std::chrono::steady_clock::now(); trk.step(121 * T, T, burst);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  require(ms < 20.0, "400-detection burst under 20 ms");

  // hungarian(): brute-force optimality sweep (Critical fix 1) -- rectangular sizes 1..6, 0-75%
  // forbidden, fixed seed, a few thousand trials.
  {
    std::mt19937 hrng(42);
    std::uniform_int_distribution<int> dim(1, 6);
    std::uniform_real_distribution<double> pforbid(0.0, 0.75), val(0.0, 50.0), coin(0.0, 1.0);
    for (int trial = 0; trial < 3000; ++trial) {
      const int rn = dim(hrng), rm = dim(hrng);
      const double pf = pforbid(hrng);
      std::vector<std::vector<double>> cost(rn, std::vector<double>(rm));
      for (int i = 0; i < rn; ++i) for (int j = 0; j < rm; ++j) cost[i][j] = (coin(hrng) < pf) ? 1e18 : val(hrng);
      const auto row = hungarian(cost);
      require((int)row.size() == rn, "hungarian result size");
      const double got = hungarian_result_cost(cost, row);
      const double want = brute_force_min_cost(cost);
      require(std::abs(got - want) < 1e-6 * std::max(1.0, std::abs(want)), "hungarian result matches brute-force optimum");
    }
  }

  // assoc must be remapped to POST-erase track indices (Important fix 2): a track BEFORE an
  // associated one gets deleted (SPRT llr <= kDelete) in the same step.
  {
    TrackerParams tp2; tp2.max_speed_mps = 20; tp2.false_object_intensity_per_s = 1.0; tp2.array_centroid = {5, 5, 2};
    CoherentTracker trk2(tp2);
    const double T2 = 0.075;
    std::vector<int> assoc;
    // track0 (index 0) spawns far away and never gets another detection -> accumulates misses
    // toward deletion. track1 (index 1) spawns at the origin and keeps getting associated.
    trk2.step(0.0, T2, {det(Vec3{-100, -100, 0}, 0.4), det(Vec3{0, 0, 0}, 0.4)}, &assoc);
    bool saw_case = false;
    for (int k = 1; k < 60 && !saw_case; ++k) {
      const auto& after = trk2.step(k * T2, T2, {det(Vec3{0, 0, 0}, 0.4)}, &assoc);
      if (after.size() == 1) {
        // track0 (the pre-erase-index-0 track, BEFORE the associated track1) was just deleted.
        // assoc must point to track1's POST-erase index (0), not its stale pre-erase index (1).
        require(!assoc.empty() && assoc[0] == 0, "assoc remapped to post-erase index after mid-step deletion");
        saw_case = true;
      }
    }
    require(saw_case, "deletion-before-association case exercised");
  }

  // update_rate exercised with a real bistatic range-rate and a finite sigma (Important fix 4),
  // asserting full positive-definiteness (not just the diagonal) throughout.
  {
    TrackerParams tp3; tp3.max_speed_mps = 20; tp3.false_object_intensity_per_s = 1.0;
    const Vec3 tx{0, 0, 0}, rx{20, 0, 0}; tp3.array_centroid = rx;
    CoherentTracker trk3(tp3);
    std::mt19937 rrng(7); std::normal_distribution<double> pn(0, 0.3), rn(0, 0.05);
    const Vec3 v0{1.0, 0.5, 0.0}; const Vec3 p0{5, 10, 2};
    const double T3 = 0.075;
    for (int k = 0; k < 80; ++k) {
      const double t = k * T3;
      const Vec3 pos{p0.x + v0.x * t, p0.y + v0.y * t, p0.z + v0.z * t};
      const Vec3 h = normalized(pos - tx) + normalized(pos - rx);
      const double true_rate = h.x * v0.x + h.y * v0.y + h.z * v0.z;
      Detection d = det(Vec3{pos.x + pn(rrng), pos.y + pn(rrng), pos.z + pn(rrng)}, 0.4);
      d.tx = tx; d.range_rate_mps = true_rate + rn(rrng); d.range_rate_sigma = 0.05;
      const auto& res = trk3.step(t, T3, {d});
      require(!res.empty(), "rate-test track exists");
      for (const Track& x : res) {
        for (double xv : x.x) require(std::isfinite(xv), "rate-test state finite");
        require(cholesky_pd(x.P), "rate-test covariance full PD (cholesky)");
      }
    }
  }

  // Confirmed-track LLR clamp (SPRT restart convention): a target seen for 100 CPIs then gone is
  // deleted once its miss evidence sum(-ln(1-P_D)) reaches ln((1-b)/a) - ln(b/(1-a)), not after the
  // ~+17 per hit it banked (which would coast it for tens of seconds).
  {
    CoherentTracker k2(tp);
    std::mt19937 r2(3); std::normal_distribution<double> nn(0, 0.05);
    int k = 0;
    for (; k < 100; ++k) k2.step(k * T, T, {det(Vec3{1 + nn(r2), 2 + nn(r2), 3 + nn(r2)}, 0.1)});
    const double gap = std::log(0.9 / 0.01) - std::log(0.1 / 0.99);
    double ev = 0; int misses = 0;
    for (;; ++k) {
      ev += -std::log(1 - k2.pd());                 // step() uses the P_D estimate from before its update
      const auto& tr2 = k2.step(k * T, T, {}); ++misses;
      if (ev < gap) require(tr2.size() == 1 && tr2[0].confirmed, "confirmed track kept until the derived miss evidence");
      else { require(tr2.empty(), "departed track deleted exactly at the derived miss evidence"); break; }
    }
    std::printf("llr clamp: deleted after %d missed CPIs (gap %.3f, evidence %.3f)\n", misses, gap, ev);
    require(misses <= 5, "departed target deleted within a few CPIs");
  }
  // Long dwell (coherent_longdwell.h): (1) RateBand -- misses of a scan that does not test a track's
  // rate are not evidence: a confirmed static track survives 20 such misses, the same misses with no band
  // delete it. (2) Out-of-sequence scan: a detection time-stamped at an earlier time associates with a
  // moving track (retrodiction) and the track is returned at the latest scan's time.
  {
    TrackerParams tp; tp.max_speed_mps = 5; tp.false_object_intensity_per_s = 1;
    const double T = 0.075; std::mt19937 r3(5); std::normal_distribution<double> nn(0, 0.05);
    CoherentTracker a(tp), b(tp);
    int k = 0;
    for (; k < 40; ++k) { const Detection d = det(Vec3{3 + nn(r3), 4 + nn(r3), 1 + nn(r3)}, 0.1); a.step(k * T, T, {d}); b.step(k * T, T, {d}); }
    // Long-dwell cadence: 6 short scans (band |rate| in [1, 10] m/s, target absent: it is static) then one
    // long scan (band |rate| <= 1 m/s) that sees it. With the bands the track lives; without, the short
    // misses kill it between long hits.
    const RateBand sb{1.0, 10.0, Vec3{30, 0, 5}}, lb{0.0, 1.0, Vec3{30, 0, 5}};
    const std::vector<Track>* ta = nullptr; const std::vector<Track>* tb = nullptr;
    for (int cyc = 0; cyc < 30; ++cyc) {
      for (int m = 0; m < 6; ++m, ++k) { ta = &a.step(k * T, T, {}, nullptr, &sb); tb = &b.step(k * T, T, {}); }
      const Detection d = det(Vec3{3 + nn(r3), 4 + nn(r3), 1 + nn(r3)}, 0.1);
      ta = &a.step(k * T, T, {d}, nullptr, &lb); tb = &b.step(k * T, T, {d}); ++k;
    }
    require(ta->size() == 1 && (*ta)[0].confirmed, "rate band: a static track survives the short scans' misses between long hits");
    require(std::none_of(tb->begin(), tb->end(), [](const Track& t) { return t.hits > 40; }), "no band: the same misses delete it");
    CoherentTracker c(tp);
    for (k = 0; k <= 40; ++k) c.step(k * T, T, {det(Vec3{1.0 * k * T + nn(r3), 0, 1}, 0.1)});   // 1 m/s along x, last scan t=3.0
    std::vector<int> assoc;
    const std::vector<Track>& tc = c.step(2.6, 0.4, {det(Vec3{2.6, 0, 1}, 0.1)}, &assoc);          // out of sequence
    require(assoc.size() == 1 && assoc[0] >= 0, "out-of-sequence detection associates (retrodiction)");
    require(tc.size() == 1 && std::abs(tc[0].x[0] - 3.0) < 0.15, "track returned at the latest scan time");
    const std::vector<Track>& tc2 = c.step(3.075, T, {det(Vec3{3.075, 0, 1}, 0.1)}, &assoc);
    require(tc2.size() == 1 && assoc[0] == 0 && std::abs(tc2[0].x[0] - 3.075) < 0.15, "in-sequence scans continue after it");
    std::printf("long dwell tracker: band keeps static track (%zu, %u hits), retrodicted x %.3f\n", ta->size(), (*ta)[0].hits, tc[0].x[0]);
  }
  std::puts("coherent_tracker_test: PASS");
  return 0;
}
