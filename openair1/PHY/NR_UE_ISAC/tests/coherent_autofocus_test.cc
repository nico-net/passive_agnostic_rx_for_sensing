// openair1/PHY/NR_UE_ISAC/tests/coherent_autofocus_test.cc
#include "coherent_autofocus.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;

namespace {

struct Scene {
  Geometry truth, survey;
  Vec3 err[4];
};

// Base scene shared by all tests; (b)/(c) scale the seeded per-antenna survey errors.
Scene make_scene(double scale)
{
  Scene s;
  s.truth.tx = {35, 20, 6};
  s.truth.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  s.survey = s.truth;
  const Vec3 base_err[4] = {{0.02, -0.01, 0.0}, {-0.015, 0.02, 0.01}, {0.01, 0.015, -0.02}, {-0.02, -0.01, 0.015}};
  for (int i = 0; i < 4; ++i) { s.err[i] = base_err[i] * scale; s.survey.rx[i] = s.truth.rx[i] + s.err[i]; }
  return s;
}

// Realistic term synthesis: a random common phase per detection (the target's own unknown
// reflectivity/reference phase) and a random amplitude per detection AND channel in [1,10]
// (arbitrary per-channel SNR), on top of the geometric phase the estimator has to recover.
Autofocus run(const Scene& scene, double survey_sigma_m, int n_detections, uint32_t seed)
{
  const double fc = 3.45e9, lam = kC / fc;
  Autofocus af(scene.survey, survey_sigma_m, fc);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> upos(-12, 12), uz(0.5, 20);
  std::uniform_real_distribution<double> utheta(-M_PI, M_PI);
  std::uniform_real_distribution<double> uamp(1.0, 10.0);
  std::normal_distribution<double> noise(0.0, std::sqrt(1.0 / 200.0)); // consistent with snr=100
  for (int k = 0; k < n_detections; ++k) {
    Detection d;
    d.pos = {upos(rng), upos(rng), uz(rng)};
    d.tx = scene.truth.tx;
    d.snr = 100;
    const double theta = utheta(rng);
    for (int i = 0; i < 4; ++i) {
      const double ph_true = 2 * M_PI * (dist(d.pos, scene.truth.tx) + dist(d.pos, scene.truth.rx[i]) - dist(scene.truth.tx, scene.truth.rx[i])) / lam;
      const double ph_comp = 2 * M_PI * (dist(d.pos, scene.survey.tx) + dist(d.pos, scene.survey.rx[i]) - dist(scene.survey.tx, scene.survey.rx[i])) / lam;
      const double amp = uamp(rng);
      d.terms[i] = amp * std::polar(1.0, -ph_true + ph_comp + theta + noise(rng));
    }
    af.add(d);
  }
  return af;
}

void check_no_regression(const Scene& scene, const Geometry& g, const char* tag)
{
  for (int i = 0; i < 4; ++i) {
    const double before = dist(scene.survey.rx[i], scene.truth.rx[i]);
    const double after = dist(g.rx[i], scene.truth.rx[i]);
    if (after > before + 1e-9) {
      std::fprintf(stderr, "%s: ch%d regressed, before=%.5f after=%.5f\n", tag, i, before, after);
      require(false, "an antenna got worse");
    }
  }
}

// (a) sigma=0.03, seeded errors, 4000 realistic detections: sum improves by >=50%, and every
// antenna is at least as close to truth as the survey was.
void test_a()
{
  const Scene scene = make_scene(1.0);
  const Autofocus af = run(scene, 0.03, 4000, 1);
  const Geometry g = af.geometry();
  double before = 0, after = 0;
  for (int i = 0; i < 4; ++i) { before += dist(scene.survey.rx[i], scene.truth.rx[i]); after += dist(g.rx[i], scene.truth.rx[i]); }
  check_no_regression(scene, g, "test_a");
  require(after <= 0.5 * before, "test_a: sum should improve by >=50%");
  std::printf("test_a: PASS (before=%.4f after=%.4f)\n", before, after);
}

// (b) same, with the seeded survey errors doubled (~5 cm): same assertions.
void test_b()
{
  const Scene scene = make_scene(2.0);
  const Autofocus af = run(scene, 0.03, 4000, 2);
  const Geometry g = af.geometry();
  double before = 0, after = 0;
  for (int i = 0; i < 4; ++i) { before += dist(scene.survey.rx[i], scene.truth.rx[i]); after += dist(g.rx[i], scene.truth.rx[i]); }
  check_no_regression(scene, g, "test_b");
  require(after <= 0.5 * before, "test_b: sum should improve by >=50%");
  std::printf("test_b: PASS (before=%.4f after=%.4f)\n", before, after);
}

// (c) sigma=0.1 (tape-grade), ~5cm errors: the estimator is allowed to decline to act (capture
// range limit -- see the header comment in coherent_autofocus.cc), but must never make an antenna
// worse than the surveyed position.
void test_c()
{
  const Scene scene = make_scene(2.0);
  const Autofocus af = run(scene, 0.1, 4000, 3);
  const Geometry g = af.geometry();
  check_no_regression(scene, g, "test_c");
  double before = 0, after = 0;
  for (int i = 0; i < 4; ++i) { before += dist(scene.survey.rx[i], scene.truth.rx[i]); after += dist(g.rx[i], scene.truth.rx[i]); }
  std::printf("test_c: PASS (before=%.4f after=%.4f, no antenna regressed)\n", before, after);
}

// (d) sign check: a minimal, self-contained reproduction of the sensitivity derivation (small
// per-channel errors, safely inside the linear capture range so there is no fringe-wrap ambiguity
// to confound the check), independent of the class's admission/gating machinery. The CORRECT sign
// (a_i = -k*(u_t-u_x), matching coherent_autofocus.cc) must recover the seeded error; the flipped
// sign must not.
bool solve3(const double A[9], const double b[3], double x[3])
{
  const double d = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
  if (!(std::abs(d) > 0)) return false;
  auto det3 = [](double a, double b_, double c, double d_, double e, double f, double g, double h, double i) { return a * (e * i - f * h) - b_ * (d_ * i - f * g) + c * (d_ * h - e * g); };
  x[0] = det3(b[0], A[1], A[2], b[1], A[4], A[5], b[2], A[7], A[8]) / d;
  x[1] = det3(A[0], b[0], A[2], A[3], b[1], A[5], A[6], b[2], A[8]) / d;
  x[2] = det3(A[0], A[1], b[0], A[3], A[4], b[1], A[6], A[7], b[2]) / d;
  return true;
}

void test_d_sign_check()
{
  Geometry truth; truth.tx = {35, 20, 6};
  truth.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Geometry survey = truth;
  // Sub-mm errors: well inside the capture range at 3.45 GHz, so the raw phase never wraps and
  // this isolates the SIGN, not the estimator's wrap-handling.
  const Vec3 err[4] = {{0.0005, -0.0003, 0.0002}, {-0.0004, 0.0006, 0.0001}, {0.0003, 0.0004, -0.0005}, {-0.0006, -0.0002, 0.0003}};
  for (int i = 0; i < 4; ++i) survey.rx[i] = truth.rx[i] + err[i];
  const double fc = 3.45e9, lam = kC / fc, k = 2 * M_PI / lam;

  double result[2] = {0, 0}; // [0]=flipped-sign residual, [1]=correct-sign residual
  int idx = 0;
  for (const double sign : {1.0, -1.0}) { // 1.0 = flipped (wrong), -1.0 = correct (matches the code)
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> upos(-12, 12), uz(0.5, 20);
    double J[4][9] = {}; double b[4][3] = {};
    for (int i = 0; i < 4; ++i) J[i][0] = J[i][4] = J[i][8] = 1e-9;
    for (int kk = 0; kk < 200; ++kk) {
      const Vec3 pos = {upos(rng), upos(rng), uz(rng)};
      for (int i = 0; i < 4; ++i) {
        const double ph_true = 2 * M_PI * (dist(pos, truth.tx) + dist(pos, truth.rx[i]) - dist(truth.tx, truth.rx[i])) / lam;
        const double ph_comp = 2 * M_PI * (dist(pos, survey.tx) + dist(pos, survey.rx[i]) - dist(survey.tx, survey.rx[i])) / lam;
        const double e = -ph_true + ph_comp;
        const Vec3 uxv = normalized(pos - survey.rx[i]), utv = normalized(survey.tx - survey.rx[i]);
        const Vec3 a = (utv - uxv) * (sign * k);
        const double av[3] = {a.x, a.y, a.z};
        for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) J[i][r * 3 + c] += av[r] * av[c]; b[i][r] += av[r] * e; }
      }
    }
    double total = 0;
    for (int i = 0; i < 4; ++i) {
      double x[3] = {0, 0, 0};
      solve3(J[i], b[i], x);
      const Vec3 g = {survey.rx[i].x + x[0], survey.rx[i].y + x[1], survey.rx[i].z + x[2]};
      total += dist(g, truth.rx[i]);
    }
    result[idx++] = total;
  }
  const double before = dist(survey.rx[0], truth.rx[0]) + dist(survey.rx[1], truth.rx[1]) + dist(survey.rx[2], truth.rx[2]) + dist(survey.rx[3], truth.rx[3]);
  require(result[1] < 0.01 * before, "test_d: correct sign should recover the seeded error almost exactly");
  require(result[0] > before, "test_d: flipped sign should make things worse (sign check)");
  std::printf("test_d: PASS (before=%.5f correct-sign after=%.5f flipped-sign after=%.5f)\n", before, result[1], result[0]);
}

} // namespace

int main()
{
  test_a();
  test_b();
  test_c();
  test_d_sign_check();
  std::puts("coherent_autofocus_test: PASS");
  return 0;
}
