/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_tracker.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;
static Detection det(Vec3 p, double sd) { Detection d; d.pos = d.pos_env = p; d.pos_sigma = {sd, sd, sd}; d.snr = 50; d.range_rate_sigma = 1e9; d.refined = true; return d; }
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
    for (int i = 0; i < 6; ++i) require(x.P[i * 6 + i] > 0, "covariance diagonal positive"); }
  require(confirmed == 3, "exactly the 3 real targets confirmed (clutter not)");
  require(trk.pd() > 0.6 && trk.pd() < 1.0, "P_D estimated online");
  // burst (Review Focus 4): 400 detections in one CPI stays fast
  std::vector<Detection> burst; for (int i = 0; i < 400; ++i) burst.push_back(det(Vec3{u(rng), u(rng), uz(rng)}, 0.4));
  const auto t0 = std::chrono::steady_clock::now(); trk.step(121 * T, T, burst);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  require(ms < 20.0, "400-detection burst under 20 ms");
  std::puts("coherent_tracker_test: PASS");
  return 0;
}
