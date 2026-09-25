// openair1/PHY/NR_UE_ISAC/tests/coherent_core_test.cc
#include "coherent_core.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;

// rows x 3276 grid, antennas 4; paths: (per-channel absolute delay, amplitude, doppler); channel phase offsets
struct Path { std::array<double, kCh> tau; double amp; double doppler_hz; };
static CfrWindow make_window(uint32_t rows, const std::vector<Path>& paths, std::array<double, kCh> ph,
                             double cfo_hz, uint32_t nprb_row, uint32_t seed, uint32_t comb = 1, double sfo_ppm = 0)
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
          acc += p.amp * std::exp(std::complex<double>(0, -2 * M_PI * ((w.fc_hz + f) * p.tau[a] - p.doppler_hz * t + f * sfo_ppm * 1e-6 * t)));
        acc *= std::exp(std::complex<double>(0, ph[a] + 2 * M_PI * cfo_hz * t));
        acc += 0.01 * std::complex<double>(n01(rng), n01(rng));
        w.values[w.sample(a, r, k)] = std::complex<float>(acc);
      }
    }
  }
  return w;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Geometry g; g.tx = {35, 20, 6};
  g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol;
  std::array<double, kCh> geo{}; for (uint32_t i = 0; i < 4; ++i) geo[i] = dist(g.tx, g.rx[i]) / kC;   // survey LOS delays
  // --- degenerate CPI (Review Focus 1)
  { CfrWindow w = make_window(1, {}, {0, 0, 0, 0}, 0, 4, 1);
    coherent::Axes a = derive_axes(w, vol, g, 10.0); require(!a.valid && !a.invalid_reason.empty(), "1-row CPI rejected with reason"); }
  // --- one LOS per channel + a moving target
  const Vec3 tgt{5, 15, 1.5};
  std::array<double, kCh> los{}, tt{};
  for (uint32_t i = 0; i < 4; ++i) { los[i] = (dist(g.tx, g.rx[i]) + 30.0) / kC; tt[i] = (dist(tgt, g.tx) + dist(tgt, g.rx[i]) + 30.0) / kC; }
  // +30 m common STO: the receiver's FFT window offset
  // --- build_waveform's closed-form (Dirichlet) kernel == the direct Hann-weighted sum, contiguous and comb-2
  for (uint32_t v : {0u, 1u, 2u}) {                // contiguous, comb-2, PRB-periodic gapped mask
    const uint32_t comb = v == 1 ? 2 : 1;
    CfrWindow wk = make_window(8, {{los, 1.0, 0.0}}, {0, 0, 0, 0}, 0, 16, 7, comb);
    if (v == 2) {                                  // 0x0c3 per PRB (REs 0,1,6,7), plus a CSI-RS-like hole
      uint32_t first = 0; while (!wk.observed[wk.cell(0, first)]) ++first;
      for (uint32_t k = first; k < wk.subcarriers; ++k)
        if (wk.observed[wk.cell(0, k)] && (!((0x0c3u >> ((k - first) % 12)) & 1u) || (k - first) / 12 == 5)) wk.observed[wk.cell(0, k)] = 0;
    }
    coherent::Axes ak = derive_axes(wk, vol, g, 10.0); require(ak.valid, "axes valid (kernel)");
    const RdResult::Waveform m = build_waveform(wk, ak);
    uint32_t lo = 0, hi = 0; while (!wk.observed[wk.cell(0, lo)]) ++lo;
    for (hi = wk.subcarriers - 1; !wk.observed[wk.cell(0, hi)]; --hi) {}
    const double kc = 0.5 * (lo + hi); double h1 = 0, h2 = 0, err = 0;
    auto hn = [&](uint32_t k) { return 0.5 - 0.5 * std::cos(2 * M_PI * (double)(k - lo) / (hi - lo)); };
    for (uint32_t k = lo; k <= hi; ++k) if (wk.observed[wk.cell(0, k)]) { h1 += hn(k); h2 += hn(k) * hn(k); }
    const std::vector<cd>& B = m.B[m.grp[0]]; const std::vector<cd>& B2 = m.B2[m.grp[0]];
    for (size_t u = 0; u < B.size(); u += 7) {
      const double x = -m.X + (double)u / RdResult::Waveform::kOvs; cd b1 = 0, b2 = 0;
      for (uint32_t k = lo; k <= hi; ++k) if (wk.observed[wk.cell(0, k)])
        { const cd e = std::polar(1.0, 2 * M_PI * x * (k - kc) / ak.n_fft); b1 += hn(k) * e; b2 += hn(k) * hn(k) * e; }
      err = std::max({err, std::abs(B[u] - b1 / h1), std::abs(B2[u] - b2 / h2)});
    }
    std::printf("  waveform kernel variant %u: closed form vs direct max err %.2e\n", v, err);
    require(err < 1e-9, "closed-form waveform kernel matches the direct sum");
  }
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
    LosEstimate L = find_los(win, ax, 1e-4, &geo);
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
  // --- REGRESSION (coherent-domain early-path resolution): a wall +6 dB stronger than the LOS and
  // ~32 ns (~4 bins) later, merged into the LOS's own mainlobe by a 64-PRB row's kernel (half-width
  // ~10 bins here) -- the non-coherent power-sum stage cannot resolve them as separate peaks and
  // locks onto the wall (measured pre-fix: calibration phase off by ~2.4 rad, targets 20-30 m off).
  // find_los must resolve the true, earlier, weaker LOS in the coherent (full-union-band) domain.
  { std::array<double, kCh> wall6{};
    for (uint32_t i = 0; i < 4; ++i) wall6[i] = los[i] + 32e-9;
    CfrWindow w7 = make_window(60, {{los, 1.0, 0.0}, {wall6, 2.0, 0.0}, {tt, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 7);
    coherent::Axes a7 = derive_axes(w7, vol, g, 10.0); require(a7.valid, "axes valid (strong late wall)");
    check_cpi(w7, a7, los, "LOS earlier + wall +6 dB/~4 bins later + target"); }
  // --- no earlier path: LOS alone (strongest AND earliest) + a WEAKER later wall must not perturb
  // the LOS lock -- the coherent scan's noise/leakage thresholds must reject everything in
  // [n0-hm, n0-1] here and fall back to the ordinary (unperturbed) LOS bin.
  { std::array<double, kCh> wall7{};
    for (uint32_t i = 0; i < 4; ++i) wall7[i] = los[i] + 40e-9;
    CfrWindow w8 = make_window(60, {{los, 1.0, 0.0}, {wall7, 0.5, 0.0}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 9);
    coherent::Axes a8 = derive_axes(w8, vol, g, 10.0); require(a8.valid, "axes valid (weak late wall)");
    LosEstimate L8 = find_los(w8, a8, 1e-4, &geo);
    for (uint32_t i = 0; i < 4; ++i)
      require(L8.found[i] && std::abs(L8.delay_s[i] - los[i]) < 0.25 * a8.delay_step_s, "no earlier path: LOS not falsely moved"); }
  // --- find_los timing on a 64-row CPI (75 ms real-time budget)
  { CfrWindow wt = make_window(64, {{los, 1.0, 0.0}, {tt, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 10);
    coherent::Axes at = derive_axes(wt, vol, g, 10.0); require(at.valid, "axes valid (timing)");
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kReps = 5;
    for (int i = 0; i < kReps; ++i) { LosEstimate Lt = find_los(wt, at, 1e-4); require(Lt.found[0], "timing run found LOS"); }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;
    std::printf("  find_los timing: %.2f ms/call (64-row CPI, 4 channels)\n", ms); }
  // --- comb-2 (DM-RS-only) CPI: replica at n_fft/2 must fold, CFO must still be removed
  // A -360 m STO (late FFT window) puts every LOS at a NEGATIVE delay: its bit-identical replica
  // then sits at a positive index below n_fft/2 and wins the index-order tie, so only the fold
  // n_fft/dk_min recovers the true delay (without it this case reads ~+1925 bins).
  { std::array<double, kCh> los3{}, tt3{};
    for (uint32_t i = 0; i < 4; ++i) { los3[i] = los[i] - 360.0 / kC; tt3[i] = tt[i] - 360.0 / kC; }
    CfrWindow w3 = make_window(60, {{los3, 1.0, 0.0}, {tt3, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 4, 2);
    coherent::Axes a3 = derive_axes(w3, vol, g, 10.0); require(a3.valid, "axes valid (comb-2)");
    check_cpi(w3, a3, los3, "comb-2 rows, negative LOS delay"); }
  // --- 128-PRB rows: at integer bins the kernel straddles its first null (5.33 bins, samples at 5
  // and 6 both near the -31.5 dB sidelobe), which once declared the mainlobe +-8 bins wide and took
  // the first sidelobe (-6.4 bins) for the LOS on 3 of 4 channels.
  { const Vec3 t1{5, 12, 1.2}, t2{-6, -4, 1.0}, dr{8, 8, 15};
    auto tp = [&](const Vec3& q) { std::array<double, kCh> t{}; for (uint32_t i = 0; i < 4; ++i) t[i] = (dist(q, g.tx) + dist(q, g.rx[i]) + 30.0) / kC; return t; };
    CfrWindow w5 = make_window(64, {{los, 1.0, 0.0}, {tp(t1), 0.2, 45}, {tp(t2), 0.25, -110}, {tp(dr), 0.15, 70}}, {0, 1.1, -2.0, 0.4}, 0, 128, 3);
    coherent::Axes a5 = derive_axes(w5, vol, g, 20.0); require(a5.valid, "axes valid (128 PRB)");
    LosEstimate L5 = find_los(w5, a5, 1e-4);
    for (uint32_t i = 0; i < 4; ++i)
      require(L5.found[i] && std::abs(L5.delay_s[i] - los[i]) < 0.25 * a5.delay_step_s, "128-PRB rows: LOS, not its first sidelobe"); }
  // --- SFO: a sample-clock offset ramps every path's delay (baseband phase only) by sfo*t. The row
  // sync's drift must be KEPT (the model selection that rejects spurious drifts must not reject a real
  // one) and the target must stay at its excess-delay bin and Doppler. 1 ppm = 59 ns (7 bins) here.
  { const double sfo = 1.0;
    CfrWindow w6 = make_window(60, {{los, 1.0, 0.0}, {tt, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 6, 1, sfo);
    coherent::Axes a6 = derive_axes(w6, vol, g, 10.0); require(a6.valid, "axes valid (SFO)");
    double tm = 0; for (double t : a6.row_t_s) tm += t / a6.row_t_s.size();
    std::array<double, kCh> los6{}; for (uint32_t i = 0; i < 4; ++i) los6[i] = los[i] + sfo * 1e-6 * tm;   // LOS at the mean row time
    LosEstimate L6 = find_los(w6, a6, 1e-4); RowSync s6 = estimate_row_sync(w6, a6, L6);
    const double drift = s6.delay_s.back() - s6.delay_s.front(), want = sfo * 1e-6 * a6.row_t_s.back();
    std::printf("  cpi: SFO %.1f ppm: drift %.2f ns (true %.2f ns)\n", sfo, drift * 1e9, want * 1e9);
    require(s6.valid && std::abs(drift / want - 1) < 0.1, "SFO: row-sync drift kept, within 10 %");
    check_cpi(w6, a6, los6, "SFO 1 ppm"); }
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
  // --- notch-leakage whitening on a SPARSE CPI: 40 slots of which only 14 carry a row, irregularly (DL
  // grant gaps). Its Doppler pedestal is too high for the range-bin median floor, so the floor of every
  // tested cell is predicted from the notch energy spread by the waveform ambiguity: the leakage of a
  // drifting in-notch wall must stop exceeding the per-cell threshold. (Movers in such CPIs can be masked
  // by their own notch pedestal -- documented limit in coherent_core.cc.)
  { std::array<double, kCh> wall{}; for (uint32_t i = 0; i < 4; ++i) wall[i] = los[i] + 15.0 / kC;
    const double step = 1.0 / (40 * 2 * slot_duration_s(30000));
    const uint32_t keep[] = {0, 1, 2, 5, 9, 10, 14, 20, 21, 27, 30, 33, 38, 39};
    CfrWindow w = make_window(40, {{los, 1.0, 0.0}, {wall, 0.5, 0.5 * step}}, {0, 1.1, -2.0, 0.4}, 0, 64, 21);
    for (uint32_t r = 0; r < w.rows; ++r)
      if (std::find(std::begin(keep), std::end(keep), r) == std::end(keep))
        for (uint32_t k = 0; k < w.subcarriers; ++k) w.observed[w.cell(r, k)] = 0;
    const coherent::Axes a = derive_axes(w, vol, g, 50.0);
    require(a.valid && a.tested_dopp.size() > 8, "sparse CPI axes with room outside the notch");
    const LosEstimate L = find_los(w, a, 1e-4, &geo);
    RdResult R = range_doppler(w, a, L, estimate_row_sync(w, a, L));
    const double pfa = detect_params(a, envelope_grid(vol, a), 1.0).pfa, thr = -std::log(pfa);
    auto count = [&]() { int n = 0; for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i])
      for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d : a.tested_dopp) n += std::norm(R.rd.v[R.rd.idx(i, m, d)]) / R.noise[i] > thr; return n; };
    const int before = count(); WhitenInfo wi; whiten_range_clutter(R, pfa, &wi); const int after = count();
    std::printf("  sparse CPI: ped %.3f (bound %.3f) mode %d; clutter cells over threshold %d -> %d\n", wi.ped, wi.bound, wi.mode, before, after);
    require(wi.mode == 2 && before > 0 && after == 0, "sparse CPI: notch leakage whitened below the per-cell threshold");
  }
  std::puts("coherent_core_test: PASS");
  return 0;
}
