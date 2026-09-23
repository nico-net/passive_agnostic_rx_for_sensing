// openair1/PHY/NR_UE_ISAC/tests/coherent_autofocus_test.cc
#include "coherent_autofocus.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;
int main() {
  Geometry truth; truth.tx = {35, 20, 6};
  truth.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Geometry survey = truth;
  const Vec3 err[4] = {{0.02, -0.01, 0.0}, {-0.015, 0.02, 0.01}, {0.01, 0.015, -0.02}, {-0.02, -0.01, 0.015}};
  for (int i = 0; i < 4; ++i) survey.rx[i] = truth.rx[i] + err[i];
  const double fc = 3.45e9, lam = kC / fc;
  Autofocus af(survey, 0.1, fc);
  double before = 0; for (int i = 0; i < 4; ++i) before += norm(err[i]);
  std::mt19937 rng(4); std::uniform_real_distribution<double> u(-12, 12), uz(0.5, 20); std::normal_distribution<double> n(0, 0.05);
  for (int k = 0; k < 400; ++k) {
    Detection d; d.pos = {u(rng), u(rng), uz(rng)}; d.tx = truth.tx; d.snr = 100;
    // terms as the focuser would produce at the (true) voxel using the SURVEYED geometry
    for (int i = 0; i < 4; ++i) {
      const double ph_true = 2 * M_PI * (dist(d.pos, truth.tx) + dist(d.pos, truth.rx[i]) - dist(truth.tx, truth.rx[i])) / lam;
      const double ph_comp = 2 * M_PI * (dist(d.pos, survey.tx) + dist(d.pos, survey.rx[i]) - dist(survey.tx, survey.rx[i])) / lam;
      d.terms[i] = std::polar(1.0, -ph_true + ph_comp + n(rng));
    }
    af.add(d);
  }
  const Geometry g = af.geometry();
  double after = 0; for (int i = 0; i < 4; ++i) after += dist(g.rx[i], truth.rx[i]);
  require(after < 0.5 * before, "autofocus halves the survey error");
  std::puts("coherent_autofocus_test: PASS");
  return 0;
}
