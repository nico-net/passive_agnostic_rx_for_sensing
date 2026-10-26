// openair1/PHY/NR_UE_ISAC/tests/coherent_focus_test.cc  (make_window copied from coherent_core_test.cc)
#include "coherent_core.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac::coherent;
using nr_isac::CfrWindow; using nr_isac::slot_duration_s;

// rows x 3276 grid, antennas 4; paths: (per-channel absolute delay, amplitude, doppler); channel phase offsets
// + per-channel Doppler; migrate: the delay follows the Doppler (tau(t) = tau - f_D t / fc), as a truly
// moving path does. Off by default: the scene targets carry ONE Doppler on all four channels, which
// no velocity produces, and migrating them would make their delays geometrically inconsistent.
struct Path {
  std::array<double, kCh> tau; double amp; double doppler_hz; std::array<double, kCh> dopp_ch{}; bool migrate = false;
  bool traj = false; Vec3 x0{}, vel{}; Vec3 tx{}; std::array<Vec3, kCh> rx{};
};
static CfrWindow make_window(uint32_t rows, const std::vector<Path>& paths, std::array<double, kCh> ph,
                             double cfo_hz, uint32_t nprb_row, uint32_t seed, uint32_t comb = 1,
                             bool regular_hop = false, const std::vector<double>* slots = nullptr, double noise = 0.01)
{
  CfrWindow w; w.antennas = 4; w.rows = rows; w.subcarriers = 273 * 12; w.scs_hz = 30000; w.fc_hz = 3.45e9; w.pci = 2;
  w.values.assign((size_t)4 * rows * w.subcarriers, {0, 0}); w.observed.assign((size_t)rows * w.subcarriers, 0);
  std::mt19937 rng(seed), hop(seed + 1000); std::normal_distribution<double> n01(0, 1);
  const double slot = slot_duration_s(w.scs_hz);
  for (uint32_t r = 0; r < rows; ++r) {
    const double ts = slots ? (*slots)[r] : r * 2.0;  // every 2nd slot unless given
    w.row_time_slots.push_back(ts);
    w.row_slot_idx.push_back((uint32_t)ts); w.row_slot_frac.push_back(0); w.row_source_mask.push_back(1u << 3);
    const double t = ts * slot;
    // Pseudo-random hopping. A regular stride (was r*37 mod 210 PRB) makes the band centre a
    // sawtooth in time, which is a genuine range-Doppler ambiguity of the waveform: a path seen
    // ~1.6 bins off its delay is serrodyne-shifted by +176 Hz at ~0.94 of its peak, beating the
    // 0.4-bin-off-grid true Doppler (0.90 scalloping) on 2 of 4 channels. Physics, not the code.
    const uint32_t start = regular_hop ? (r * 37) % (273 - nprb_row + 1) : hop() % (273 - nprb_row + 1);
    for (uint32_t k = start * 12; k < (start + nprb_row) * 12; k += comb) {   // comb 2 = DM-RS-only row
      w.observed[w.cell(r, k)] = 1;
      const double f = ((double)k - 273 * 6) * w.scs_hz;
      for (uint32_t a = 0; a < 4; ++a) {
        std::complex<double> acc = 0;
        for (const Path& p : paths) {
          if (p.traj) {
            const Vec3 xt = p.x0 + p.vel * t;
            const double tau = (dist(xt, p.tx) + dist(xt, p.rx[a]) + 30) / kC;
            acc += p.amp * std::exp(std::complex<double>(0, -2 * M_PI * (w.fc_hz + f) * tau));
            continue;
          }
          const double fd = p.doppler_hz + p.dopp_ch[a];
          acc += p.migrate ? p.amp * std::exp(std::complex<double>(0, -2 * M_PI * (w.fc_hz + f) * (p.tau[a] - fd * t / w.fc_hz)))
                           : p.amp * std::exp(std::complex<double>(0, -2 * M_PI * ((w.fc_hz + f) * p.tau[a] - fd * t)));
        }
        acc *= std::exp(std::complex<double>(0, ph[a] + 2 * M_PI * cfo_hz * t));
        acc += noise * std::complex<double>(n01(rng), n01(rng));
        w.values[w.sample(a, r, k)] = std::complex<float>(acc);
      }
    }
  }
  return w;
}

static void dump(const char* what, const std::vector<Detection>& D)
{
  std::printf("  %s: %zu detections\n", what, D.size());
  for (const Detection& d : D)
    std::printf("    env(%6.2f %6.2f %6.2f) pos(%6.2f %6.2f %6.2f) fd %7.1f snr %.3g sd (%.3g %.3g %.3g) bins %d %d %d %d\n", d.pos_env.x, d.pos_env.y,
                d.pos_env.z, d.pos.x, d.pos.y, d.pos.z, d.doppler_hz, d.snr, d.pos_sigma.x, d.pos_sigma.y, d.pos_sigma.z,
                d.chan_dopp_bin[0], d.chan_dopp_bin[1], d.chan_dopp_bin[2], d.chan_dopp_bin[3]);
}

namespace {
Geometry geo()
{
  Geometry g; g.tx = {35, 20, 6};
  g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  return g;
}
const std::array<double, kCh> kPh{0, 1.1, -2.0, 0.4};
std::array<double, kCh> taus(const Geometry& g, const Vec3& p)
{ std::array<double, kCh> t{}; for (uint32_t i = 0; i < 4; ++i) t[i] = (dist(p, g.tx) + dist(p, g.rx[i]) + 30) / kC; return t; }
std::array<double, kCh> los_taus(const Geometry& g)
{ std::array<double, kCh> t{}; for (uint32_t i = 0; i < 4; ++i) t[i] = (dist(g.tx, g.rx[i]) + 30) / kC; return t; }
// Whole chain on one window -> (R, calibration, grid, detections). survey: refine()'s survey sigma.
struct Chain { Axes a; LosEstimate L; RdResult R; Calibration c; Grid G; std::vector<float> E; std::vector<Detection> D; };
Chain run(const CfrWindow& w, const Geometry& g, const Volume& vol, double vmax, SurveySigma survey = {})
{
  Chain k; k.a = derive_axes(w, vol, g, vmax); require(k.a.valid, "axes");
  k.L = find_los(w, k.a, 1e-4); RowSync s = estimate_row_sync(w, k.a, k.L); k.R = range_doppler(w, k.a, k.L, s);
  Calibrator cal; for (int n = 0; n < 5; ++n) k.c = cal.update(k.R.los_tap, k.L.found, k.L.snr);
  k.G = envelope_grid(vol, k.a); k.E = envelope(k.R, k.G, g);
  k.D = detect(k.E, k.R, k.G, g, detect_params(k.a, k.G, 1.0));
  for (Detection& d : k.D) refine(d, k.R, k.G, g, k.c, survey);
  return k;
}
const Detection* nearest(const std::vector<Detection>& D, const Vec3& q, bool env)
{ const Detection* b = nullptr; for (const Detection& d : D) if (!b || dist(env ? d.pos_env : d.pos, q) < dist(env ? b->pos_env : b->pos, q)) b = &d; return b; }
} // namespace


int main(int argc, char** argv) {
  const Geometry g = geo(); Volume vol; const double vx = atof(argv[1]);
  const Vec3 x0{-6, -4, 1.0}, v{vx, argc > 2 ? atof(argv[2]) : 0.0, 0};
  const double lam = kC / 3.45e9;
  Path p{taus(g, x0), 0.25, 0}; p.migrate = true;
  if (getenv("TRAJ")) { p.traj = true; p.x0 = x0; p.vel = v; p.tx = g.tx; p.rx = g.rx; }
  std::array<double, kCh> f{};
  for (uint32_t i = 0; i < kCh; ++i) { f[i] = -dot(normalized(x0 - g.tx) + normalized(x0 - g.rx[i]), v) / lam; p.dopp_ch[i] = f[i]; }
  CfrWindow w = make_window(64, {{los_taus(g), 1.0, 0}, p}, kPh, 0, 128, 3);
  Chain k = run(w, g, vol, 20.0);
  const double tbar = 0.0315; const Vec3 xm = x0 + v * tbar;
  const Detection& d = k.D.at(0);
  std::printf("v=(%.1f,%.1f) refined err (%.4f %.4f %.4f) sd (%.4f %.4f %.4f) env err (%.4f %.4f %.4f)\n", v.x, v.y, d.pos.x - xm.x, d.pos.y - xm.y, d.pos.z - xm.z,
              d.pos_sigma.x, d.pos_sigma.y, d.pos_sigma.z, d.pos_env.x - xm.x, d.pos_env.y - xm.y, d.pos_env.z - xm.z);
  for (uint32_t i = 0; i < kCh; ++i) {
    const double ptrue = (taus(g, x0)[i] - f[i] * tbar / 3.45e9 - k.L.delay_s[i]) / k.a.delay_step_s;
    // phase residual at x(tbar): fitted amp advanced to tbar, calibrated, carrier-compensated
    const cd amp = d.chan_amp[i] * std::polar(1.0, 2 * M_PI * d.chan_fd_hz[i] * tbar) * std::polar(1.0, -k.c.phase_rad[i]);
    const double ph_true = std::arg(amp * std::polar(1.0, 2 * M_PI * 3.45e9 * excess_delay_s(xm, g.tx, g.rx[i])));
    const double ph_ref = std::arg(amp * std::polar(1.0, 2 * M_PI * 3.45e9 * excess_delay_s(d.pos, g.tx, g.rx[i])));
    std::printf("  ch%u fd fit %.4f true %.4f (diff %.4f Hz) | phase at x(tbar) %.4f at refined %.4f | los cal %.4f\n", i, d.chan_fd_hz[i], f[i], d.chan_fd_hz[i] - f[i], ph_true, ph_ref, k.c.phase_rad[i]);
    const Vec3 xmm = xm; const double pm = (dist(xmm, g.tx) + dist(xmm, g.rx[i]) + 30) / kC;
    std::printf("     true p(tbar) %.5f\n", (pm - k.L.delay_s[i]) / k.a.delay_step_s); (void)ptrue;
  }
}
