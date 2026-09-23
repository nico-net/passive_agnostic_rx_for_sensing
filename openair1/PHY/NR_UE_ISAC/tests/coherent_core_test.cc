// openair1/PHY/NR_UE_ISAC/tests/coherent_core_test.cc
#include "coherent_core.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;

// rows x 3276 grid, antennas 4; paths: (per-channel absolute delay, amplitude, doppler); channel phase offsets
struct Path { std::array<double, kCh> tau; double amp; double doppler_hz; };
static CfrWindow make_window(uint32_t rows, const std::vector<Path>& paths, std::array<double, kCh> ph,
                             double cfo_hz, uint32_t nprb_row, uint32_t seed, uint32_t comb = 1)
{
  CfrWindow w; w.antennas = 4; w.rows = rows; w.subcarriers = 273 * 12; w.scs_hz = 30000; w.fc_hz = 3.45e9; w.pci = 2;
  w.values.assign((size_t)4 * rows * w.subcarriers, {0, 0}); w.observed.assign((size_t)rows * w.subcarriers, 0);
  std::mt19937 rng(seed), hop(seed + 1000); std::normal_distribution<double> n01(0, 1);
  const double slot = slot_duration_s(w.scs_hz);
  for (uint32_t r = 0; r < rows; ++r) {
    w.row_time_slots.push_back(r * 2.0);          // every 2nd slot
    w.row_slot_idx.push_back(r * 2); w.row_slot_frac.push_back(0); w.row_source_mask.push_back(1u << 3);
    const double t = r * 2.0 * slot;
    // Pseudo-random hopping. A regular stride (was r*37 mod 210 PRB) makes the band centre a
    // sawtooth in time, which is a genuine range-Doppler ambiguity of the waveform: a path seen
    // ~1.6 bins off its delay is serrodyne-shifted by +176 Hz at ~0.94 of its peak, beating the
    // 0.4-bin-off-grid true Doppler (0.90 scalloping) on 2 of 4 channels. Physics, not the code.
    const uint32_t start = hop() % (273 - nprb_row + 1);
    for (uint32_t k = start * 12; k < (start + nprb_row) * 12; k += comb) {   // comb 2 = DM-RS-only row
      w.observed[w.cell(r, k)] = 1;
      const double f = ((double)k - 273 * 6) * w.scs_hz;
      for (uint32_t a = 0; a < 4; ++a) {
        std::complex<double> acc = 0;
        for (const Path& p : paths)
          acc += p.amp * std::exp(std::complex<double>(0, -2 * M_PI * ((w.fc_hz + f) * p.tau[a] - p.doppler_hz * t)));
        acc *= std::exp(std::complex<double>(0, ph[a] + 2 * M_PI * cfo_hz * t));
        acc += 0.01 * std::complex<double>(n01(rng), n01(rng));
        w.values[w.sample(a, r, k)] = std::complex<float>(acc);
      }
    }
  }
  return w;
}

int main() {
  Geometry g; g.tx = {35, 20, 6};
  g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol;
  // --- degenerate CPI (Review Focus 1)
  { CfrWindow w = make_window(1, {}, {0, 0, 0, 0}, 0, 4, 1);
    coherent::Axes a = derive_axes(w, vol, g, 10.0); require(!a.valid && !a.invalid_reason.empty(), "1-row CPI rejected with reason"); }
  // --- one LOS per channel + a moving target
  const Vec3 tgt{5, 15, 1.5};
  std::array<double, kCh> los{}, tt{};
  for (uint32_t i = 0; i < 4; ++i) { los[i] = (dist(g.tx, g.rx[i]) + 30.0) / kC; tt[i] = (dist(tgt, g.tx) + dist(tgt, g.rx[i]) + 30.0) / kC; }
  // +30 m common STO: the receiver's FFT window offset
  const double fd = 40.0;
  CfrWindow w = make_window(60, {{los, 1.0, 0.0}, {tt, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 2);
  coherent::Axes a = derive_axes(w, vol, g, 10.0);
  require(a.valid, "axes valid");
  require(std::abs(a.b_eff_hz - 64 * 12 * 30000.0) < 1, "b_eff = median observed bandwidth");
  require(a.n_dopp > 0 && a.dopp_step_hz > 0 && !a.tested_dopp.empty(), "doppler axis derived");
  // Full chain on one CPI: LOS on every channel within a quarter bin, then the target at its
  // excess-delay bin and its Doppler (so the common CFO was removed) on every channel.
  auto check_cpi = [&](const CfrWindow& win, const coherent::Axes& ax, const std::array<double, kCh>& los_exp, const char* what) {
    std::printf("  cpi: %s\n", what);
    LosEstimate L = find_los(win, ax, 1e-4);
    for (uint32_t i = 0; i < 4; ++i) {
      require(L.found[i], "LOS found");
      require(std::abs(L.delay_s[i] - los_exp[i]) < 0.25 * ax.delay_step_s + 1e-12, "LOS delay sub-bin accurate");
    }
    RowSync s = estimate_row_sync(win, ax, L);
    require(s.valid && s.phase_rad.size() == win.rows && s.delay_s.size() == win.rows, "row sync sized");
    RdResult R = range_doppler(win, ax, L, s);
    for (uint32_t i = 0; i < 4; ++i) {
      const double ex = excess_delay_s(tgt, g.tx, g.rx[i]);
      const uint32_t rb = (uint32_t)std::lround(ex / ax.delay_step_s);
      uint32_t best_r = 0, best_d = 0; float best = 0;
      for (uint32_t r = 0; r < ax.n_range; ++r) for (uint32_t d = 0; d < ax.n_dopp; ++d) {
        const float m = std::abs(R.rd.v[R.rd.idx(i, r, d)]); if (m > best) { best = m; best_r = r; best_d = d; } }
      require(std::abs((int)best_r - (int)rb) <= 1, "target at its excess delay");
      const double fbest = ax.dopp0_hz + best_d * ax.dopp_step_hz;
      require(std::abs(fbest - fd) <= ax.dopp_step_hz, "target at its doppler (CFO removed by row sync)");
    }
    return R;
  };
  check_cpi(w, a, los, "LOS + target, contiguous rows");
  // --- static wall 15 m behind every LOS: the range axis must stay referenced to the LOS, not to
  // the power centroid of all static paths
  { std::array<double, kCh> wall{};
    for (uint32_t i = 0; i < 4; ++i) wall[i] = los[i] + 15.0 / kC;
    CfrWindow w2 = make_window(60, {{los, 1.0, 0.0}, {wall, 0.5, 0.0}, {tt, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 3);
    coherent::Axes a2 = derive_axes(w2, vol, g, 10.0); require(a2.valid, "axes valid (wall)");
    RdResult R2 = check_cpi(w2, a2, los, "LOS + static wall + target");
    for (uint32_t i = 0; i < 4; ++i) require(std::abs(std::abs(R2.los_tap[i]) - 1.0) < 0.1, "|los_tap| ~ 1 with a static wall"); }
  // --- comb-2 (DM-RS-only) CPI: replica at n_fft/2 must fold, CFO must still be removed
  // A -360 m STO (late FFT window) puts every LOS at a NEGATIVE delay: its bit-identical replica
  // then sits at a positive index below n_fft/2 and wins the index-order tie, so only the fold
  // n_fft/dk_min recovers the true delay (without it this case reads ~+1925 bins).
  { std::array<double, kCh> los3{}, tt3{};
    for (uint32_t i = 0; i < 4; ++i) { los3[i] = los[i] - 360.0 / kC; tt3[i] = tt[i] - 360.0 / kC; }
    CfrWindow w3 = make_window(60, {{los3, 1.0, 0.0}, {tt3, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 4, 2);
    coherent::Axes a3 = derive_axes(w3, vol, g, 10.0); require(a3.valid, "axes valid (comb-2)");
    check_cpi(w3, a3, los3, "comb-2 rows, negative LOS delay"); }
  // --- row times must be non-decreasing; invalid axes give empty results, never a division
  { CfrWindow w4 = make_window(10, {{los, 1.0, 0.0}}, {0, 0, 0, 0}, 0, 4, 5);
    std::swap(w4.row_time_slots[3], w4.row_time_slots[4]);
    coherent::Axes a4 = derive_axes(w4, vol, g, 10.0);
    require(!a4.valid && a4.invalid_reason == "non-monotonic row times", "non-monotonic row times rejected");
    LosEstimate L4 = find_los(w4, a4, 1e-4);
    require(!L4.found[0] && !L4.found[1] && !L4.found[2] && !L4.found[3], "no LOS on invalid axes");
    require(!estimate_row_sync(w4, a4, L4).valid, "row sync invalid on invalid axes");
    require(range_doppler(w4, a4, L4, estimate_row_sync(w4, a4, L4)).rd.v.empty(), "no RD on invalid axes"); }
  // quantile helper
  const double x = gamma_upper_quantile(4, 1e-3);
  require(std::abs(std::exp(-x) * (1 + x + x * x / 2 + x * x * x / 6) - 1e-3) < 1e-9, "Q(4,x)=p");
  // large shapes (R >~ 650 rows): exp(-x) underflows a linear-domain Q. References:
  // scipy.special.gammainccinv(700, 1e-4), gammainccinv(2000, 1e-4).
  require(std::abs(gamma_upper_quantile(700, 1e-4) / 802.6984979193063 - 1) < 1e-9, "Q^-1(700,1e-4)");
  require(std::abs(gamma_upper_quantile(2000, 1e-4) / 2170.611882032139 - 1) < 1e-9, "Q^-1(2000,1e-4)");
  std::puts("coherent_core_test: PASS");
  return 0;
}
