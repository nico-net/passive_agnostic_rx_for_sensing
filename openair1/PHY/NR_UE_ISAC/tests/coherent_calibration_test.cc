// openair1/PHY/NR_UE_ISAC/tests/coherent_calibration_test.cc
#include "coherent_core.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac::coherent;
static double wrap(double x) { return std::atan2(std::sin(x), std::cos(x)); }
int main() {
  std::mt19937 rng(5); std::normal_distribution<double> n(0, 1);
  const std::array<double, kCh> ph{0.0, 1.1, -2.0, 0.4};

  // --- stable phases: recovered, consistent with the filter's OWN reported uncertainty, phase_var
  // keeps averaging down over more updates, predictive coh_factor near 1, coherent gain -> ~4, rho -> ~1.
  Calibrator cal; Calibration c;
  std::array<double, kCh> var_at_40{};
  for (int k = 0; k < 200; ++k) {
    std::array<cd, kCh> tap; std::array<bool, kCh> f{true, true, true, true}; std::array<double, kCh> snr{};
    const double common = n(rng);                                   // common phase noise per CPI
    for (uint32_t i = 0; i < kCh; ++i) { tap[i] = std::polar(1.0, ph[i] + common + 0.05 * n(rng)); snr[i] = 200; }
    c = cal.update(tap, f, snr);
    if (k == 39) var_at_40 = c.phase_var;
  }
  for (uint32_t i = 1; i < kCh; ++i) {
    // Single-snapshot check in CONSISTENCY form (not a fixed rad tolerance): the filter's own reported
    // phase_var is the thing under test, so the check is that the error is plausible under it, at 3 sigma.
    require(std::abs(wrap(c.phase_rad[i] - ph[i])) <= 3 * std::sqrt(c.phase_var[i]), "phase consistent with its own posterior variance (3 sigma)");
    require(c.phase_var[i] < var_at_40[i], "phase_var keeps averaging down: update 200 < update 40");
    require(c.coh_factor[i] > 0.95, "stable: predictive coh_factor high");
  }
  require(c.coherent_gain > 3.8 && c.rho > 0.9, "coherent: G~4, rho~1");

  // --- scrambled phases each CPI: a fresh random phase every CPI. G -> ~1, rho -> ~0 (averaged over
  // the last 20 updates to denoise, same as above). The PREDICTIVE coh_factor (phase_var + process
  // noise) correctly fades a decohering channel even though its per-CPI POSTERIOR alone looks
  // confident (each update "snaps" to the fresh random measurement with high trust) -- this is what
  // supersedes R3's earlier drop of this assertion.
  Calibrator bad; Calibration b;
  std::uniform_real_distribution<double> u(-M_PI, M_PI);
  double rho_sum = 0; int rho_n = 0;
  for (int k = 0; k < 40; ++k) {
    std::array<cd, kCh> tap; std::array<bool, kCh> f{true, true, true, true}; std::array<double, kCh> snr{200, 200, 200, 200};
    for (uint32_t i = 0; i < kCh; ++i) tap[i] = std::polar(1.0, u(rng));
    b = bad.update(tap, f, snr);
    if (k >= 20) { rho_sum += b.rho; ++rho_n; }
  }
  require(rho_sum / rho_n < 0.35, "scrambled: mean rho (last 20 updates) low");
  for (uint32_t i = 1; i < kCh; ++i) require(b.coh_factor[i] < 0.5, "scrambled: channel faded (predictive coh_factor)");

  // --- LOS missing on channel 2: predicts, no NaN. For a genuinely STATIC channel (true q=0, as
  // above) the corrected covariance-matching estimator converges q_use to ~0, so a missed CPI
  // correctly does NOT grow its variance (nothing new to be uncertain about) -- that strict-growth
  // check now lives below, on the random-walk channel, where q_use is reliably positive. Here the
  // requirement is finiteness only.
  const double v_before = c.phase_var[2];
  std::array<cd, kCh> tap_miss{cd(1), std::polar(1.0, ph[1]), cd(0), std::polar(1.0, ph[3])};
  Calibration m = cal.update(tap_miss, {true, true, false, true}, {200, 200, 0, 200});
  require(std::isfinite(m.phase_rad[2]) && std::isfinite(m.coh_factor[2]) && m.phase_var[2] >= v_before, "missing LOS: predict, finite, variance never shrinks");

  // --- a channel seen late (absent for its first 5 CPIs): its first valid measurement seeds it
  // directly (s_ = z, p_ = r exactly), not blended through the uninformative prior via an "innovation".
  Calibrator late;
  for (int k = 0; k < 5; ++k) {
    std::array<cd, kCh> tap{cd(1), std::polar(1.0, 0.3), cd(0), std::polar(1.0, -0.7)};
    late.update(tap, {true, true, false, true}, {200, 200, 0, 200});
  }
  std::array<cd, kCh> tap_seed{cd(1), std::polar(1.0, 0.3), std::polar(1.0, 1.234), std::polar(1.0, -0.7)};
  Calibration seeded = late.update(tap_seed, {true, true, true, true}, {200, 200, 200, 200});
  require(std::abs(wrap(seeded.phase_rad[2] - 1.234)) < 1e-9, "late seed: s_ set exactly to the measurement");
  require(std::abs(seeded.phase_var[2] - (1.0 / (2 * 200) + 1.0 / (2 * 200))) < 1e-9, "late seed: p_ set exactly to r");

  // --- random-walk channel (true q = 1e-2): the covariance-matching estimator recovers it within a
  // factor of 2 after 500 updates. SNR=2000 (r=2.5e-4, well below q_true): the wrapped-innovation
  // estimator is measurably biased low when measurement noise is comparable to process noise (checked
  // separately, not asserted here -- this picks an SNR where the process-noise signal dominates,
  // robust across 10 sweep seeds to within [0.84, 1.14] of truth).
  {
    Calibrator rw; Calibration rc;
    std::mt19937 rng2(7); std::normal_distribution<double> nq(0, std::sqrt(1e-2));
    double walk = 0.2;
    for (int k = 0; k < 500; ++k) {
      walk += nq(rng2);
      std::array<cd, kCh> tap{cd(1), std::polar(1.0, walk), cd(0, 0), cd(0, 0)};
      rc = rw.update(tap, {true, true, false, false}, {2000, 2000, 0, 0});
    }
    const double q_est = -2 * std::log(rc.coh_factor[1]) - rc.phase_var[1];   // invert coh_factor = exp(-(p+q)/2)
    require(q_est > 1e-2 / 2 && q_est < 1e-2 * 2, "random walk: q recovered within factor 2 of truth");
    // Same fixture, one more CPI with the channel missing: here q_use IS reliably positive (unlike
    // the static channel above), so the predict step must strictly grow the variance -- the original
    // Review Focus 2 "missing LOS: predict" intent, restored where it actually applies.
    const double v_before_rw = rc.phase_var[1];
    std::array<cd, kCh> tap_miss_rw{cd(1), cd(0), cd(0, 0), cd(0, 0)};
    Calibration mrw = rw.update(tap_miss_rw, {true, false, false, false}, {2000, 0, 0, 0});
    require(std::isfinite(mrw.phase_rad[1]) && std::isfinite(mrw.coh_factor[1]) && mrw.phase_var[1] > v_before_rw,
            "missing LOS on a drifting channel: predict, no NaN, variance strictly grows");
  }

  std::puts("coherent_calibration_test: PASS");
  return 0;
}
