// openair1/PHY/NR_UE_ISAC/tests/coherent_longdwell_test.cc
// Super-row construction + alignment of the long-dwell CPI (coherent_longdwell.h):
//  - a slow target (inside the short CPI's zero-Doppler notch) keeps its Doppler across ~40 short CPIs
//    whose common phase and LOS delay reference jump at random from CPI to CPI (alignment works: its
//    long-CPI peak is within 1/T_L of truth and >= 0.9 of an error-free reference run);
//  - a fast target whose Doppler folds into the slow band at the super-row rate is suppressed by the
//    block mean (anti-alias) to <= -15 dB of the slow target of equal amplitude;
//  - T_L = (c/B_eff)/(lambda f_slow), f_slow = the short CPI's lowest tested |f|.
#include "coherent_longdwell.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;

constexpr uint32_t S = 1024;
constexpr double SCS = 30000, FC = 3.45e9;
struct Tgt { double tau_s, amp, fd_hz; };

// One short CPI: rows in 7 of every 10 slots with probability 0.6, full band, a static LOS at delay 0
// plus targets; the whole CPI carries a common phase phi and a LOS delay reference error err_s (what
// the per-CPI common offset / CFO phase leave).
static CfrWindow short_cpi(uint32_t slot0, uint32_t nslots, const std::vector<Tgt>& tg, double phi, double err_s, std::mt19937& rng)
{
  CfrWindow w; w.antennas = 4; w.subcarriers = S; w.scs_hz = SCS; w.fc_hz = FC;
  std::uniform_real_distribution<double> u(0, 1); std::normal_distribution<double> n01(0, 1);
  const double slot = slot_duration_s(SCS);
  std::vector<uint32_t> sl;
  for (uint32_t n = slot0; n < slot0 + nslots; ++n) if (n % 10 < 7 && u(rng) < 0.6) sl.push_back(n);
  w.rows = (uint32_t)sl.size();
  w.values.assign((size_t)4 * w.rows * S, {0, 0}); w.observed.assign((size_t)w.rows * S, 1);
  for (uint32_t r = 0; r < w.rows; ++r) {
    w.row_time_slots.push_back(sl[r]); w.row_slot_idx.push_back(sl[r] % 20480); w.row_slot_frac.push_back(0); w.row_source_mask.push_back(1u << 3);
    const double t = sl[r] * slot;
    for (uint32_t k = 0; k < S; ++k) {
      const double f = ((double)k - S / 2.0) * SCS;
      cd h = 1.0;
      for (const Tgt& x : tg) h += x.amp * std::polar(1.0, -2 * M_PI * f * x.tau_s + 2 * M_PI * x.fd_hz * t);
      h *= std::polar(1.0, phi - 2 * M_PI * f * err_s);
      for (uint32_t a = 0; a < 4; ++a) w.values[w.sample(a, r, k)] = cf(h + 0.01 * cd(n01(rng), n01(rng)));
    }
  }
  return w;
}

// Range-gated slow-time spectrum of a long CPI: per-subcarrier mean removed (static), projected on the
// target's delay, Hann over time; returns max |Y(f)| over |f| <= fmax and its argmax.
static double spectrum_peak(const CfrWindow& w, double tau_s, double fmax, double df, double* f_at)
{
  const double slot = slot_duration_s(SCS);
  std::vector<cd> m(S, 0.0);
  for (uint32_t r = 0; r < w.rows; ++r) for (uint32_t k = 0; k < S; ++k) m[k] += cd(w.values[w.sample(0, r, k)]) / (double)w.rows;
  std::vector<cd> y(w.rows); std::vector<double> t(w.rows);
  for (uint32_t r = 0; r < w.rows; ++r) {
    cd acc = 0;
    for (uint32_t k = 0; k < S; ++k) acc += (cd(w.values[w.sample(0, r, k)]) - m[k]) * std::polar(1.0, 2 * M_PI * ((double)k - S / 2.0) * SCS * tau_s);
    y[r] = acc / (double)S; t[r] = (w.row_time_slots[r] - w.row_time_slots[0]) * slot;
  }
  double best = 0; *f_at = 0; const double T = t.back();
  for (double f = -fmax; f <= fmax; f += df) {
    cd acc = 0; double ws = 0;
    for (uint32_t r = 0; r < w.rows; ++r) { const double h = 0.5 - 0.5 * std::cos(2 * M_PI * t[r] / T); acc += h * y[r] * std::polar(1.0, -2 * M_PI * f * t[r]); ws += h; }
    if (std::abs(acc) / ws > best) { best = std::abs(acc) / ws; *f_at = f; }
  }
  return best;
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Geometry g; g.tx = {35, 20, 6}; g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol;
  const double dstep = 1.0 / (1024 * SCS);                     // one range bin
  const Tgt slow{12 * dstep, 0.1, 10.0}, fast{30 * dstep, 0.1, 300.0};
  auto run = [&](bool errors, std::vector<LongCpi>* out, double* fslow, double* beff) {
    std::mt19937 rng(7), er(99); std::uniform_real_distribution<double> u(-M_PI, M_PI), ue(-20e-9, 20e-9);
    LongDwell ld;
    for (uint32_t c = 0; c < 100; ++c) {
      const double phi = errors ? u(er) : 0.0, err = errors ? ue(er) : 0.0;
      CfrWindow w = short_cpi(c * 150, 150, {slow, fast}, phi, err, rng);
      const coherent::Axes a = derive_axes(w, vol, g, 50.0);
      require(a.valid, "short axes valid");
      LosEstimate L; L.found.fill(true);
      RowSync s; s.phase_rad.assign(w.rows, 0.0); s.delay_s.assign(w.rows, 0.0); s.valid = true;
      SuperRows sr = make_super_rows(w, a, L, s);
      require(sr.valid(), "super-rows built");
      require(sr.w.rows < w.rows, "fewer super-rows than rows");
      *fslow = sr.f_slow_hz; *beff = sr.b_eff_hz;
      sr.t_air_s = c * 0.075; sr.mid_slots = 0.5 * (w.row_time_slots.front() + w.row_time_slots.back());
      LongCpi lc; if (ld.add(std::move(sr), &lc)) out->push_back(std::move(lc));
    }
  };
  std::vector<LongCpi> A, B; double fs = 0, be = 0;
  run(true, &A, &fs, &be); run(false, &B, &fs, &be);
  fs = A[0].f_slow_hz;                                          // median over its short CPIs' first-past-notch bins
  const double TL = (kC / be) / ((kC / FC) * fs);
  std::printf("f_slow %.2f Hz  B_eff %.2f MHz  T_L %.3f s  long CPIs %zu (errors) / %zu (reference)\n", fs, be / 1e6, TL, A.size(), B.size());
  require(A.size() >= 2 && A.size() == B.size(), "long CPIs emitted at T_L/2 cadence");
  require(std::abs(A[1].t_air_s - A[0].t_air_s - A[0].cadence_s) < 0.1 * A[0].cadence_s, "cadence T_L/2");
  require(std::abs(A[0].t_l_s - TL) < 1e-9 && std::abs(A[0].cadence_s - TL / 2) < 1e-9, "T_L = (c/B)/(lambda f_slow)");
  const double df = 0.25 / TL;
  for (size_t i = 0; i < A.size(); ++i) {
    const double span = (A[i].w.row_time_slots.back() - A[i].w.row_time_slots.front()) * slot_duration_s(SCS);
    double fa, fb, ff;
    const double pa = spectrum_peak(A[i].w, slow.tau_s, fs, df, &fa), pb = spectrum_peak(B[i].w, slow.tau_s, fs, df, &fb);
    const double pf = spectrum_peak(A[i].w, fast.tau_s, fs, df, &ff);
    std::printf("  long %zu: rows %u span %.3f s  slow peak %.4f at %+.2f Hz (ref %.4f at %+.2f)  fast in-band max %.4f at %+.2f Hz (%.1f dB)\n",
                i, A[i].w.rows, span, pa, fa, pb, fb, pf, ff, 20 * std::log10(pf / pa));
    require(std::abs(span - TL) < 0.1 * TL, "long CPI spans T_L");
    require(std::abs(fa - slow.fd_hz) <= 1.0 / TL, "slow target Doppler preserved (within 1/T_L)");
    require(pa >= 0.95 * pb, "alignment: slow target >= 0.95 of the error-free reference");
    require(pf <= std::pow(10.0, -15.0 / 20) * pa, "fast target not aliased into the slow band (<= -15 dB)");
  }
  std::printf("coherent_longdwell_test: PASS\n");
  return 0;
}
