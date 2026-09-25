// openair1/PHY/NR_UE_ISAC/tests/coherent_focus_test.cc  (make_window copied from coherent_core_test.cc)
#include "coherent_core.h"
#include "coherent_cuda_detect.h"   // save_detect_case (header-only): NR_ISAC_FOCUS_DUMP=<dir> records every
                                    // detect() input of these scenes for the GPU parity replay
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
static std::vector<Detection> detect_rec(const std::vector<float>& E, const RdResult& R, const Grid& G, const Geometry& g, const DetectParams& p)
{
  static int n = 0;
  if (const char* dir = std::getenv("NR_ISAC_FOCUS_DUMP")) save_detect_case(std::string(dir) + "/case_" + std::to_string(n++) + ".bin", E, R, G, g, p);
  return detect(E, R, G, g, p);
}
using nr_isac::CfrWindow; using nr_isac::slot_duration_s;

// rows x 3276 grid, antennas 4; paths: (per-channel absolute delay, amplitude, doppler); channel phase offsets
// + per-channel Doppler; migrate: the delay follows the Doppler (tau(t) = tau - f_D t / fc), as a truly
// moving path does. Off by default: the scene targets carry ONE Doppler on all four channels, which
// no velocity produces, and migrating them would make their delays geometrically inconsistent.
struct Path { std::array<double, kCh> tau; double amp; double doppler_hz; std::array<double, kCh> dopp_ch{}; bool migrate = false; };
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
  k.D = detect_rec(k.E, k.R, k.G, g, detect_params(k.a, k.G, 1.0));
  for (Detection& d : k.D) refine(d, k.R, k.G, g, k.c, survey);
  return k;
}
const Detection* nearest(const std::vector<Detection>& D, const Vec3& q, bool env)
{ const Detection* b = nullptr; for (const Detection& d : D) if (!b || dist(env ? d.pos_env : d.pos, q) < dist(env ? b->pos_env : b->pos, q)) b = &d; return b; }
} // namespace

// Brief's scene (seed 3): three targets, a below-ground mirror, t1's 2nd harmonic.
static void test_scene()
{
  const Geometry g = geo(); Volume vol;
  const Vec3 t1{5, 12, 1.2}, t2{-6, -4, 1.0}, drone{8, 8, 15}, mirror{8, 8, -15};
  const std::array<double, kCh> los = los_taus(g);
  CfrWindow w = make_window(64, {{los, 1.0, 0}, {taus(g, t1), 0.2, 45}, {taus(g, t2), 0.25, -110}, {taus(g, drone), 0.15, 70},
                                  {taus(g, mirror), -0.07, 70}, {taus(g, t1), 0.05, 90}},   // 2nd harmonic of t1
                            kPh, 0, 128, 3);
  Axes a = derive_axes(w, vol, g, 20.0); require(a.valid, "axes");
  LosEstimate L = find_los(w, a, 1e-4); RowSync s = estimate_row_sync(w, a, L); RdResult R = range_doppler(w, a, L, s);
  Calibrator cal; Calibration c{}; for (int k = 0; k < 5; ++k) c = cal.update(R.los_tap, L.found, L.snr);
  Grid G = envelope_grid(vol, a);
  require(std::abs(G.step - kC / (4 * a.b_eff_hz)) < 1e-9, "grid step derived from bandwidth");
  std::vector<float> E = envelope(R, G, g);
  require(E.size() == a.tested_dopp.size() * G.size(), "envelope sized");
  DetectParams p = detect_params(a, G, 1.0); require(p.pfa > 0 && p.pfa < 1e-3, "pfa derived from intensity");
  std::vector<Detection> D = detect_rec(E, R, G, g, p);
  // Refined at the survey sigma the pipeline passes by default (CoherentConfig::survey_sigma_m =
  // 0.1 m): at tape grade the coherent term is not trusted (rho_eff ~ 0, see test_survey_sigma).
  // Measured over seeds 1-20 of this scene: at 0.1 m all three targets lie within 3 sigma (per axis,
  // pos_cov) in 18/20 seeds; with a perfect survey (0) only in 6/20 -- the coherent lobe choice among
  // unresolved neighbours is not covered by the reported covariance (see the Task 5 report).
  for (Detection& d : D) refine(d, R, G, g, c, SurveySigma{{0.1, 0.1, 0.1, 0.1}, 0.1});
  dump("scene", D);
  auto near = [&](const Vec3& q, double tol) { for (const Detection& d : D) if (dist(d.pos, q) < tol) return true; return false; };
  const double tol = kC / (2 * a.b_eff_hz);                        // one range resolution cell
  require(near(t1, tol) && near(t2, tol) && near(drone, tol), "three targets detected within a resolution cell");
  for (const Detection& d : D) require(d.pos.z >= vol.z0 - 1e-9, "no below-ground detection (mirror rejected)");
  int t1_hits = 0; for (const Detection& d : D) if (dist(d.pos_env, t1) < 2 * G.step) ++t1_hits;
  require(t1_hits == 1, "harmonic merged into its fundamental");
  for (const Detection& d : D) require(d.refined && std::isfinite(d.pos_sigma.x) && d.pos_sigma.x > 0, "refined, finite sigma");
  // out-of-volume / out-of-axis voxels never read out of bounds (Review Focus 3): tiny n_range axis
  Axes tiny = a; tiny.n_range = 3; RdResult R3 = R; R3.rd.axes = tiny;
  R3.rd.v.assign((size_t)kCh * tiny.n_range * tiny.n_dopp, cf(0));
  std::vector<float> E3 = envelope(R3, G, g); require(E3.size() == tiny.tested_dopp.size() * G.size(), "no OOB");
}

// Item 1: a regular 37-PRB hop stride is a waveform range-Doppler ambiguity in EACH channel (a
// serrodyne ghost ~1.5-1.9 bins off at ~+250 Hz, ~0.5 of the target). It lands at a different excess
// delay on each channel, so the multi-channel envelope must detect the target and nothing else.
// noise = per-RE noise sigma: 0.01 / 0.3 / 1.0 put the target ~69 / ~40 / ~33 dB above the noise in
// the RD map (the leakage must then be predicted to ~1e-3.5 of the target's power, see detect()).
static void test_regular_hop_ghost(double noise)
{
  const Geometry g = geo(); Volume vol; const Vec3 t1{5, 12, 1.2};
  CfrWindow w = make_window(64, {{los_taus(g), 1.0, 0}, {taus(g, t1), 0.2, 45}}, kPh, 0, 128, 3, 1, true, nullptr, noise);
  Chain k = run(w, g, vol, 20.0);
  std::printf("  regular hop, noise %.2g:\n", noise); dump("regular-hop ghost", k.D);
  const Detection* d = nearest(k.D, t1, true);
  require(d && dist(d->pos_env, t1) < 2 * k.G.step, "regular hop: true target detected");
  for (const Detection& x : k.D) require(dist(x.pos_env, t1) < 2 * k.G.step, "regular hop: no detection at the ghost");
}

// Item 2: a channel whose LOS was not found has no excess-delay reference: it must not contribute.
static void test_los_missing_channel()
{
  const Geometry g = geo(); Volume vol;
  const Vec3 t1{5, 12, 1.2}, t2{-6, -4, 1.0};
  CfrWindow w = make_window(64, {{los_taus(g), 1.0, 0}, {taus(g, t1), 0.2, 45}, {taus(g, t2), 0.25, -110}}, kPh, 0, 128, 3);
  for (uint32_t r = 0; r < w.rows; ++r) for (uint32_t s = 0; s < w.subcarriers; ++s) w.values[w.sample(2, r, s)] = 0;  // dead antenna
  Chain k = run(w, g, vol, 20.0);
  require(!k.L.found[2] && k.L.found[0] && k.L.found[1] && k.L.found[3], "dead channel: no LOS found on it");
  // The channel's RD content must not matter at all: garbage there changes nothing.
  RdResult Rg = k.R; std::mt19937 rng(9); std::normal_distribution<float> n01(0, 1);
  for (uint32_t m = 0; m < Rg.rd.axes.n_range; ++m) for (uint32_t d = 0; d < Rg.rd.axes.n_dopp; ++d)
    Rg.rd.v[Rg.rd.idx(2, m, d)] = cf(1e3f * n01(rng), 1e3f * n01(rng));
  Rg.noise[2] = 1e-12;
  const std::vector<float> Eg = envelope(Rg, k.G, g);
  require(Eg == k.E, "LOS-less channel does not enter the envelope");
  std::vector<Detection> Dg = detect_rec(Eg, Rg, k.G, g, detect_params(k.a, k.G, 1.0));
  for (Detection& d : Dg) refine(d, Rg, k.G, g, k.c, SurveySigma{});
  require(Dg.size() == k.D.size(), "LOS-less channel: same detections");
  for (size_t n = 0; n < Dg.size(); ++n)
    require(dist(Dg[n].pos, k.D[n].pos) == 0 && Dg[n].snr == k.D[n].snr, "LOS-less channel: same positions and SNR");
  dump("3 channels (ch2 dead)", k.D);
  for (const Vec3& q : {t1, t2}) { const Detection* d = nearest(k.D, q, true); require(d && dist(d->pos_env, q) < 2 * k.G.step, "3-channel detection"); }
  // Gamma null scaled to the contributing channels: the snr is per contributing channel.
  const Detection* d = nearest(k.D, t2, true);
  std::printf("  3-ch t2 snr %.3g\n", d->snr);
}

// Amendment (a): the antennas are 5-10 m apart, so one target has a different bistatic Doppler on
// each channel. Here -(u_tx + u_i).v/lambda for v = (8,0,0) m/s spreads t2 over >= 2 bins (all
// channels outside the zero-Doppler notch); it must be detected with an SNR close to the same target
// with every channel at the mean Doppler: within the Hann worst-case scalloping loss, 1.42 dB, since
// the two cases sit at different sub-bin positions.
static void test_per_channel_doppler()
{
  const Geometry g = geo(); Volume vol; const Vec3 t2{-6, -4, 1.0}, v{8, 0, 0};
  const double lam = kC / 3.45e9;
  std::array<double, kCh> f{}; double fm = 0;
  for (uint32_t i = 0; i < kCh; ++i) { f[i] = -dot(normalized(t2 - g.tx) + normalized(t2 - g.rx[i]), v) / lam; fm += f[i] / kCh; }
  Path spread{taus(g, t2), 0.25, 0}, aligned{taus(g, t2), 0.25, fm};
  for (uint32_t i = 0; i < kCh; ++i) spread.dopp_ch[i] = f[i];
  Chain ks = run(make_window(64, {{los_taus(g), 1.0, 0}, spread}, kPh, 0, 128, 3), g, vol, 20.0);
  Chain ka = run(make_window(64, {{los_taus(g), 1.0, 0}, aligned}, kPh, 0, 128, 3), g, vol, 20.0);
  double fmin = f[0], fmax = f[0]; for (double x : f) { fmin = std::min(fmin, x); fmax = std::max(fmax, x); }
  std::printf("  per-channel Doppler %.1f %.1f %.1f %.1f Hz: spread %.2f bins\n", f[0], f[1], f[2], f[3], (fmax - fmin) / ks.a.dopp_step_hz);
  require((fmax - fmin) / ks.a.dopp_step_hz >= 2, "scene spreads the channels by >= 2 Doppler bins");
  dump("per-channel Doppler", ks.D); dump("aligned Doppler", ka.D);
  const Detection* ds = nearest(ks.D, t2, true); const Detection* da = nearest(ka.D, t2, true);
  require(ds && da && dist(ds->pos_env, t2) < 2 * ks.G.step && dist(da->pos_env, t2) < 2 * ka.G.step, "both detected");
  std::printf("  snr spread %.4g aligned %.4g (%.2f dB)\n", ds->snr, da->snr, 10 * std::log10(ds->snr / da->snr));
  require(std::abs(10 * std::log10(ds->snr / da->snr)) < 1.42, "per-channel Doppler: SNR within the scalloping loss of the aligned case");
}

// Amendment (b): TDD gating (rows only in 7 of every 10 slots) puts slow-time replicas of every
// target at +-1/(10 slots) = +-200 Hz: exactly one detection.
static void test_tdd_replicas()
{
  const Geometry g = geo(); Volume vol; const Vec3 t2{-6, -4, 1.0};
  std::vector<double> slots; for (uint32_t s = 0; slots.size() < 64; ++s) if (s % 10 < 7) slots.push_back(s);
  CfrWindow w = make_window(64, {{los_taus(g), 1.0, 0}, {taus(g, t2), 0.25, -110}}, kPh, 0, 128, 3, 1, false, &slots);
  Chain k = run(w, g, vol, 20.0);
  const float rep = k.R.ambiguity(0.0, 1.0 / (10 * slot_duration_s(w.scs_hz)) / k.a.dopp_step_hz);
  std::printf("  TDD slow-time PSF at +200 Hz: %.1f dB\n", 10 * std::log10(rep));
  require(rep > 0.05, "TDD gating makes a Doppler replica");
  dump("TDD", k.D);
  require(k.D.size() == 1 && dist(k.D[0].pos_env, t2) < 2 * k.G.step, "TDD: exactly one detection");
}

// Amendment (c): rho comes from the (survey-independent) LOS calibration; the target phase is not.
// A 0.1 m survey gives rho_eff ~ 0: the position is the envelope position and sigma envelope-scale.
// 2 mm keeps the coherent refinement.
// |error| <= 3 sigma per axis against truth (sigma = sqrt(diag(pos_cov))).
static void require_within_3sigma(const Detection& d, const Vec3& truth, const char* what)
{
  const Vec3 e = d.pos - truth;
  std::printf("  %s: error (%.4f %.4f %.4f) m, 3 sigma (%.4f %.4f %.4f) m\n", what, e.x, e.y, e.z,
              3 * d.pos_sigma.x, 3 * d.pos_sigma.y, 3 * d.pos_sigma.z);
  require(std::abs(e.x) <= 3 * d.pos_sigma.x && std::abs(e.y) <= 3 * d.pos_sigma.y && std::abs(e.z) <= 3 * d.pos_sigma.z, what);
}

static void test_survey_sigma()
{
  const Geometry g = geo(); Volume vol; const Vec3 t2{-6, -4, 1.0};
  Chain k = run(make_window(64, {{los_taus(g), 1.0, 0}, {taus(g, t2), 0.25, -110}}, kPh, 0, 128, 3), g, vol, 20.0);
  require(k.D.size() == 1 && k.c.rho > 0.9, "survey test: one detection, calibrated (rho ~ 1)");
  Detection coarse = k.D[0], fine = k.D[0];
  refine(coarse, k.R, k.G, g, k.c, SurveySigma{{0.1, 0.1, 0.1, 0.1}, 0.1});
  refine(fine, k.R, k.G, g, k.c, SurveySigma{{0.002, 0.002, 0.002, 0.002}, 0.002});
  std::printf("  survey 0.1 m: moved %.3g m | 2 mm: moved %.3g m\n", dist(coarse.pos, coarse.pos_env), dist(fine.pos, fine.pos_env));
  require(dist(coarse.pos, coarse.pos_env) < 1e-6, "0.1 m survey: envelope position");
  require_within_3sigma(coarse, t2, "0.1 m survey: |error| <= 3 sigma");
  require(dist(fine.pos, fine.pos_env) > 0, "2 mm survey: coherent refinement kept");
  require_within_3sigma(fine, t2, "2 mm survey: |error| <= 3 sigma");
  // The illuminator's survey error decoheres the channel pairs too (ruling: pairwise, with the tx term).
  // Here a pair's tx sensitivity is k|u(rx_i->tx) - u(rx_j->tx)| ~ 72 * 0.25 /m: 0.1 m leaves ~0.2.
  Detection txo = k.D[0], perfect = k.D[0];
  refine(txo, k.R, k.G, g, k.c, SurveySigma{{0, 0, 0, 0}, 0.1});
  refine(perfect, k.R, k.G, g, k.c, SurveySigma{});
  std::printf("  tx survey 0.1 m alone: moved %.4g m (perfect survey: %.4g m)\n", dist(txo.pos, txo.pos_env), dist(perfect.pos, perfect.pos_env));
  require(dist(txo.pos, txo.pos_env) < 0.5 * dist(perfect.pos, perfect.pos_env), "0.1 m tx survey alone: coherent weight reduced");
}

// Item 3: the coherent phases refer to the CPI's weighted mid-time, so a moving target refines to its
// mid-CPI position (perfect survey). v = 8 m/s over a ~63 ms CPI: x(0) and x(tbar) are 0.25 m apart.
static void test_mid_cpi()
{
  const Geometry g = geo(); Volume vol; const Vec3 x0{-6, -4, 1.0}, v{getenv("VX") ? atof(getenv("VX")) : 8.0, 0, 0};
  const double lam = kC / 3.45e9;
  Path p{taus(g, x0), 0.25, 0}; p.migrate = true;
  for (uint32_t i = 0; i < kCh; ++i) p.dopp_ch[i] = -dot(normalized(x0 - g.tx) + normalized(x0 - g.rx[i]), v) / lam;
  CfrWindow w = make_window(64, {{los_taus(g), 1.0, 0}, p}, kPh, 0, 128, 3);
  Chain k = run(w, g, vol, 20.0);
  require(k.D.size() == 1, "mid-CPI: one detection");
  double tw = 0, ws = 0;
  for (uint32_t r = 0; r < w.rows; ++r) { const double t = k.a.row_t_s[r], h = 0.5 - 0.5 * std::cos(2 * M_PI * t / k.a.row_t_s.back()); tw += h * t; ws += h; }
  const Vec3 xm = x0 + v * (tw / ws);
  std::printf("  mid-CPI: tbar %.4f s, |pos - x(tbar)| %.4f m, |pos - x(0)| %.4f m; envelope: |env - x(tbar)| %.4f m\n", tw / ws,
              dist(k.D[0].pos, xm), dist(k.D[0].pos, x0), dist(k.D[0].pos_env, xm));
  require(dist(k.D[0].pos, xm) < dist(k.D[0].pos, x0) / 5, "mid-CPI: at x(tbar), not x(0)");
}

// Amendment (d): hierarchical refinement cost at R = 2 m from the array centroid (a dense half-fringe
// grid over +-one envelope step would be ~1e9 points there).
static void test_refine_cost()
{
  const Geometry g = geo(); Volume vol; const Vec3 t2{-6, -4, 1.0};
  Chain k = run(make_window(64, {{los_taus(g), 1.0, 0}, {taus(g, t2), 0.25, -110}}, kPh, 0, 128, 3), g, vol, 20.0);
  Vec3 cen{}; for (const Vec3& r : g.rx) cen = cen + r * 0.25;
  Detection d = k.D.at(0); d.pos = d.pos_env = cen + Vec3{2, 0, 0};
  const int N = 20;
  const auto t0 = std::chrono::steady_clock::now();
  for (int n = 0; n < N; ++n) { Detection x = d; refine(x, k.R, k.G, g, k.c, SurveySigma{}); require(x.refined, "refined at 2 m"); }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
  std::printf("  refine at R = 2 m: %.3f ms\n", ms);
  require(ms < 5.0, "refinement at 2 m under 5 ms");
}

// An empty scene (LOS + noise only) must stay empty: the declared intensity (1/s) is 0.064 false
// objects per CPI here. Before the row-sync fixes it gave 5-9 per CPI (LOS static-removal residual
// from a grid-quantised delay reference and a spurious delay drift, spread over every Doppler bin).
static void test_empty_scene()
{
  const Geometry g = geo(); Volume vol; size_t n = 0;
  for (double noise : {0.01, 1.0}) for (uint32_t seed = 1; seed <= 5; ++seed)
    n += run(make_window(64, {{los_taus(g), 1.0, 0}}, kPh, 0, 128, seed, 1, false, nullptr, noise), g, vol, 20.0).D.size();
  std::printf("  empty scene: %zu detections in 10 CPIs\n", n);
  require(n == 0, "empty scene: no detections");
}

// Null of the per-channel max over m Doppler bins, summed over n channels.
static void test_null_quantile()
{
  for (uint32_t n : {1u, 3u, 4u}) for (double p : {0.5, 1e-3, 1e-7})
    require(std::abs(max_exp_sum_quantile(1, n, p) / gamma_upper_quantile(n, p) - 1) < 1e-3, "m=1 is Gamma(n,1)");
  std::mt19937 rng(1); std::exponential_distribution<double> ex(1.0);
  for (uint32_t m : {3u, 11u}) {
    const int N = 400000; std::vector<double> v(N);
    for (int k = 0; k < N; ++k) { double s = 0; for (int i = 0; i < 3; ++i) { double x = 0; for (uint32_t j = 0; j < m; ++j) x = std::max(x, ex(rng)); s += x; } v[k] = s; }
    std::sort(v.begin(), v.end());
    for (double p : {0.5, 1e-2}) {
      const double mc = v[(size_t)((1 - p) * N)], q = max_exp_sum_quantile(m, 3, p);
      std::printf("  null m=%u n=3 p=%g: numeric %.4f Monte Carlo %.4f\n", m, p, q, mc);
      require(std::abs(q / mc - 1) < 0.01, "max-of-m null matches Monte Carlo");
    }
  }
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  test_null_quantile();
  test_empty_scene();
  test_scene();
  test_los_missing_channel();
  test_regular_hop_ghost(0.01);
  test_regular_hop_ghost(0.3);
  test_regular_hop_ghost(1.0);
  test_per_channel_doppler();
  test_tdd_replicas();
  test_survey_sigma();
  test_mid_cpi();
  test_refine_cost();
  std::puts("coherent_focus_test: PASS");
  return 0;
}
