/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_core.h"
#include "fft.h"
#include "robust_stats.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nr_isac::coherent {
namespace {
uint32_t next_pow2(uint32_t n) { uint32_t p = 1; while (p < n) p <<= 1; return p; }
double hann(double u) { return 0.5 - 0.5 * std::cos(2 * M_PI * u); }  // u in [0,1]
// Hann peak sidelobe power relative to its mainlobe (-31.47 dB): a window property, not a tuning.
constexpr double kHannPslPow = 7.1326e-4;
double baseband_hz(const CfrWindow& w, uint32_t k) { return ((double)k - w.subcarriers / 2.0) * w.scs_hz; }
// Observed [lo,hi] subcarrier span of a row; returns false when the row is empty.
bool row_span(const CfrWindow& w, uint32_t r, uint32_t* lo, uint32_t* hi)
{
  int l = -1, h = -1;
  for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) { if (l < 0) l = (int)k; h = (int)k; }
  if (l < 0) return false;
  *lo = (uint32_t)l; *hi = (uint32_t)h; return true;
}
// Centred-index inverse FFT of one row after a delay ramp and a Hann window over
// the row's observed band; amplitude normalised by the window sum so every allocation gives the
// path amplitude at its peak. `sub` (optional, per subcarrier) is subtracted after the ramp.
std::vector<cd> row_profile(const CfrWindow& w, const Axes& a, uint32_t ant, uint32_t row,
                            double ramp_delay_s, double phase_rad, const std::vector<cd>* sub = nullptr)
{
  std::vector<cd> buf(a.n_fft, cd(0, 0));
  double wsum = 0;
  uint32_t lo, hi;
  if (row_span(w, row, &lo, &hi))
    for (uint32_t k = lo; k <= hi; ++k) {
      if (!w.observed[w.cell(row, k)]) continue;
      const double u = (hi > lo) ? (double)(k - lo) / (hi - lo) : 0.5;
      const double win = hann(u); wsum += win;
      const double f = baseband_hz(w, k);
      const cd ramp = std::polar(1.0, 2 * M_PI * f * ramp_delay_s - phase_rad);
      const int q = (int)k - (int)(w.subcarriers / 2);
      const cd z = cd(w.values[w.sample(ant, row, k)]) * ramp - (sub ? (*sub)[k] : cd(0));
      buf[(size_t)((q % (int)a.n_fft + (int)a.n_fft) % (int)a.n_fft)] += z * win;
    }
  fft_inplace(buf, true);                         // inverse: sum * e^{+j...} / N
  if (wsum > 0) for (cd& v : buf) v *= (double)a.n_fft / wsum;
  return buf;
}
} // namespace

double gamma_upper_quantile(uint32_t shape, double p)
{
  auto Q = [shape](double x) { double term = 1, s = 1; for (uint32_t k = 1; k < shape; ++k) { term *= x / k; s += term; } return std::exp(-x) * s; };
  double lo = 0, hi = 1; while (Q(hi) > p) hi *= 2;
  for (int i = 0; i < 200; ++i) { const double m = 0.5 * (lo + hi); (Q(m) > p ? lo : hi) = m; }
  return 0.5 * (lo + hi);
}

Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps)
{
  Axes a;
  auto fail = [&](const char* why) { a.valid = false; a.invalid_reason = why; return a; };
  if (!w.valid() || w.antennas != kCh) return fail("window invalid or not 4 antennas");
  if (w.rows < 3) return fail("fewer than 3 rows");
  a.fc_hz = w.fc_hz; a.lambda_m = kC / w.fc_hz; a.scs_hz = w.scs_hz; a.subcarriers = w.subcarriers;
  std::vector<double> bw;
  for (uint32_t r = 0; r < w.rows; ++r) { uint32_t lo, hi; if (row_span(w, r, &lo, &hi)) bw.push_back((hi - lo + 1) * w.scs_hz); }
  if (bw.size() < 3) return fail("fewer than 3 non-empty rows");
  a.b_eff_hz = median(bw);
  if (a.b_eff_hz < 2 * w.scs_hz) return fail("median allocation narrower than 2 subcarriers");
  a.n_fft = next_pow2(w.subcarriers);
  a.delay_step_s = 1.0 / (a.n_fft * w.scs_hz);
  double max_ex = 0;
  for (double x : {vol.x0, vol.x1}) for (double y : {vol.y0, vol.y1}) for (double z : {vol.z0, vol.z1})
    for (const Vec3& rx : g.rx) max_ex = std::max(max_ex, excess_delay_s(Vec3{x, y, z}, g.tx, rx));
  a.n_range = std::min<uint32_t>(a.n_fft / 2, (uint32_t)std::ceil(max_ex / a.delay_step_s) + 4);
  const double slot = slot_duration_s(w.scs_hz);
  a.row_t_s.resize(w.rows);
  for (uint32_t r = 0; r < w.rows; ++r) a.row_t_s[r] = (w.row_time_slots[r] - w.row_time_slots[0]) * slot;
  std::vector<double> dt;
  for (uint32_t r = 1; r < w.rows; ++r) if (a.row_t_s[r] > a.row_t_s[r - 1]) dt.push_back(a.row_t_s[r] - a.row_t_s[r - 1]);
  if (dt.empty()) return fail("all rows at the same time");
  a.median_dt_s = median(dt);
  a.t_cpi_s = a.row_t_s.back() + a.median_dt_s;
  a.dopp_step_hz = 1.0 / a.t_cpi_s;
  const double span = 1.0 / (2 * a.median_dt_s);
  a.n_dopp = 2 * (uint32_t)std::ceil(span / a.dopp_step_hz);
  a.dopp0_hz = -(double)(a.n_dopp / 2) * a.dopp_step_hz;
  const double f_max = (max_speed_mps > 0) ? 2 * max_speed_mps / a.lambda_m : span;
  for (uint32_t d = 0; d < a.n_dopp; ++d) {
    const double f = a.dopp0_hz + d * a.dopp_step_hz;
    if (std::abs(f) <= a.notch_half_bins * a.dopp_step_hz) continue;
    if (std::abs(f) > f_max) continue;
    a.tested_dopp.push_back(d);
  }
  a.valid = true;
  return a;
}

LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa)
{
  LosEstimate L;
  for (uint32_t i = 0; i < kCh; ++i) {
    // Rows are combined NON-coherently: a common CFO / per-row phase rotates each row's profile, so
    // a coherent row mean cancels the LOS it is looking for (measured: 60 rows at 23 Hz CFO put the
    // estimate 20-28 bins off). The power mean is phase-blind; phase and sub-bin come from the
    // coherent refinement below.
    std::vector<double> pw(a.n_fft, 0.0);
    uint32_t R = 0;
    for (uint32_t r = 0; r < w.rows; ++r) {
      uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
      const std::vector<cd> p = row_profile(w, a, i, r, 0.0, 0.0);
      for (size_t n = 0; n < pw.size(); ++n) pw[n] += std::norm(p[n]);
      ++R;
    }
    if (R == 0) { L.found[i] = false; continue; }
    for (double& v : pw) v /= R;
    // Noise-only mean of R exponential powers is Gamma(R, mu/R): its median is mu*Q^-1(R,1/2)/R.
    const double noise = median(pw) * R / gamma_upper_quantile(R, 0.5);
    const double thr = noise * gamma_upper_quantile(R, pfa) / R;
    const size_t strongest = (size_t)(std::max_element(pw.begin(), pw.end()) - pw.begin());
    // earliest significant local maximum within n_range bins before the strongest (circular).
    // "Significant" = above the strongest path's own Hann sidelobe, with noise at its threshold
    // added IN PHASE (amplitudes add): at high SNR every leading sidelobe (-31/-41/-48 dB here)
    // clears the noise threshold alone and was picked instead of the LOS (measured 34 bins early).
    // The sidelobe is budgeted at 2x PSL power (-28.5 dB): other paths' sidelobes add to the
    // strongest's (measured: a -14 dB target 4 bins behind the LOS lifted its first sidelobe past
    // the bare PSL, 1 CPI in 40). ponytail: fixed 3 dB budget; an earlier true path more than
    // 28.5 dB below the strongest is taken for a sidelobe.
    const double lead_amp = std::sqrt(2 * kHannPslPow * pw[strongest]) + std::sqrt(thr);
    const double lead_thr = lead_amp * lead_amp;
    size_t best = strongest;
    for (uint32_t back = a.n_range; back > 0; --back) {
      const size_t n = (strongest + pw.size() - back) % pw.size();
      const size_t nm = (n + pw.size() - 1) % pw.size(), np = (n + 1) % pw.size();
      if (pw[n] > lead_thr && pw[n] >= pw[nm] && pw[n] >= pw[np]) { best = n; break; }
    }
    if (pw[best] <= thr) { L.found[i] = false; continue; }
    const size_t bm = (best + pw.size() - 1) % pw.size(), bp = (best + 1) % pw.size();
    const double y0 = std::sqrt(pw[bm]), y1 = std::sqrt(pw[best]), y2 = std::sqrt(pw[bp]);
    const double den = y0 - 2 * y1 + y2;
    const double frac = (std::abs(den) > 0) ? 0.5 * (y0 - y2) / den : 0.0;
    double bin = (double)best + std::clamp(frac, -0.5, 0.5);
    if (bin > a.n_fft / 2.0) bin -= a.n_fft;                        // negative delays wrap
    L.delay_s[i] = bin * a.delay_step_s;
    L.snr[i] = pw[best] / noise; L.found[i] = true;
  }
  // Coherent sub-bin refinement. The power mean above is phase-blind, so a moving path near the
  // LOS biases its peak (measured up to 0.27 bin from a -14 dB target 6 bins behind). With the
  // common CFO/SFO of the coarse LOS removed, the row mean is coherent: a moving path averages
  // out over its Doppler, and the hopping rows combine to the full union-band resolution.
  const RowSync s = estimate_row_sync(w, a, L);
  double d_mean = 0; for (double d : s.delay_s) d_mean += d / w.rows;
  for (uint32_t i = 0; i < kCh; ++i) {
    if (!L.found[i]) continue;
    std::vector<cd> coh(a.n_fft, cd(0));
    uint32_t R = 0;
    for (uint32_t r = 0; r < w.rows; ++r) {
      uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
      const std::vector<cd> p = row_profile(w, a, i, r, s.delay_s[r] - d_mean, s.phase_rad[r]);
      for (size_t n = 0; n < coh.size(); ++n) coh[n] += p[n];
      ++R;
    }
    const long n0 = std::lround(L.delay_s[i] / a.delay_step_s);
    const long N = (long)a.n_fft;
    auto at = [&](long n) { return std::abs(coh[(size_t)(((n % N) + N) % N)]); };
    long best = n0;
    for (long n = n0 - 2; n <= n0 + 2; ++n) if (at(n) > at(best)) best = n;
    const double y0 = at(best - 1), y1 = at(best), y2 = at(best + 1), den = y0 - 2 * y1 + y2;
    const double frac = (std::abs(den) > 0) ? 0.5 * (y0 - y2) / den : 0.0;
    L.delay_s[i] = ((double)best + std::clamp(frac, -0.5, 0.5)) * a.delay_step_s;
    L.tap[i] = coh[(size_t)(((best % N) + N) % N)] / (double)R;
  }
  return L;
}

RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& L)
{
  RowSync s; s.phase_rad.assign(w.rows, 0.0); s.delay_s.assign(w.rows, 0.0);
  std::vector<std::array<cd, kCh>> tap(w.rows);
  std::vector<cd> slope(w.rows, cd(0));
  for (uint32_t r = 0; r < w.rows; ++r)
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!L.found[i]) continue;
      cd acc = 0; double n = 0; uint32_t prev = UINT32_MAX; cd zp = 0;
      for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) {
        const cd z = cd(w.values[w.sample(i, r, k)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, k) * L.delay_s[i]);
        acc += z; n += 1;
        if (prev != UINT32_MAX && k - prev == 1) slope[r] += z * std::conj(zp);  // adjacent-subcarrier phase step
        prev = k; zp = z;
      }
      tap[r][i] = n > 0 ? acc / n : cd(0);
    }
  // Channel phase alignment against the strongest channel: the common row rotation cancels in
  // tap_i * conj(tap_ref), so this stays defined even when a CFO spins the plain row mean to ~0.
  uint32_t ref = 0;
  for (uint32_t i = 0; i < kCh; ++i) if (L.found[i] && (!L.found[ref] || L.snr[i] > L.snr[ref])) ref = i;
  std::array<cd, kCh> align{};
  for (uint32_t i = 0; i < kCh; ++i) if (L.found[i]) {
    cd x = 0; for (uint32_t r = 0; r < w.rows; ++r) x += tap[r][i] * std::conj(tap[r][ref]);
    if (std::abs(x) > 0) align[i] = std::conj(x) / std::abs(x);
  }
  // Raw per-row estimates, then a straight-line fit over row time: CFO is a linear phase
  // progression and SFO a linear delay drift. Per-row values are not used directly -- a target
  // within ~1 resolution cell of the LOS (every target of a room-sized volume) leaks into the
  // row's LOS tap at its own Doppler, so per-row sync absorbs part of the target (measured: -2.4 dB
  // and -1.2 dB target peak on the two channels whose target sits 0.7 / 4.3 bins from the LOS).
  // ponytail: a line cannot follow oscillator phase noise inside a CPI; per-row residual tracking
  // with the target Doppler protected would be the upgrade if OTA CPIs show it.
  std::vector<double> t, ph, dl; double prev_ph = 0;
  for (uint32_t r = 0; r < w.rows; ++r) {
    cd c = 0; for (uint32_t i = 0; i < kCh; ++i) if (L.found[i]) c += tap[r][i] * align[i];  // weight = LOS amplitude
    if (std::abs(c) == 0 || std::abs(slope[r]) == 0) continue;
    double p = std::arg(c);
    if (!ph.empty()) p = prev_ph + std::remainder(p - prev_ph, 2 * M_PI);                  // unwrap in row order
    prev_ph = p;
    t.push_back(a.row_t_s[r]); ph.push_back(p); dl.push_back(-std::arg(slope[r]) / (2 * M_PI * w.scs_hz));
  }
  if (t.empty()) return s;
  auto fit = [&](const std::vector<double>& y, std::vector<double>& out) {
    double mt = 0, my = 0; for (size_t n = 0; n < t.size(); ++n) { mt += t[n]; my += y[n]; }
    mt /= t.size(); my /= t.size();
    double stt = 0, sty = 0; for (size_t n = 0; n < t.size(); ++n) { stt += (t[n] - mt) * (t[n] - mt); sty += (t[n] - mt) * (y[n] - my); }
    const double b = stt > 0 ? sty / stt : 0.0;
    for (uint32_t r = 0; r < w.rows; ++r) out[r] = my + b * (a.row_t_s[r] - mt);
  };
  fit(ph, s.phase_rad); fit(dl, s.delay_s);
  return s;
}

RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s)
{
  RdResult out; out.rd.axes = a; out.rd.v.assign((size_t)kCh * a.n_range * a.n_dopp, cf(0, 0));
  std::vector<double> win(w.rows); double wsum = 0;
  for (uint32_t r = 0; r < w.rows; ++r) { win[r] = hann(a.t_cpi_s > 0 ? a.row_t_s[r] / a.row_t_s.back() : 0.5); wsum += win[r]; }
  for (uint32_t i = 0; i < kCh; ++i) {
    // Static removal PER SUBCARRIER, before the range IFFT. A range-bin mean cannot do it: when the
    // allocation hops between rows, the LOS leakage into bin m != 0 carries a row-dependent phase
    // 2*pi*q_centre(r)*m/n_fft, so it is not static in the profile domain (measured: a +350 Hz
    // LOS artifact at bin 3 beat the target on every channel). On a fixed subcarrier it IS static.
    auto derot = [&](uint32_t r, uint32_t k) {
      return cd(w.values[w.sample(i, r, k)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, k) * (L.delay_s[i] + s.delay_s[r]) - s.phase_rad[r]);
    };
    std::vector<cd> stat(w.subcarriers, cd(0)); std::vector<uint32_t> cnt(w.subcarriers, 0);
    cd los = 0; uint32_t los_rows = 0;
    for (uint32_t r = 0; r < w.rows; ++r) {
      uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
      cd acc = 0; double ws = 0;
      for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)]) {
        const cd z = derot(r, k); stat[k] += z; ++cnt[k];
        const double h = hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5); acc += z * h; ws += h;
      }
      if (ws > 0) { los += acc / ws; ++los_rows; }
    }
    for (uint32_t k = 0; k < w.subcarriers; ++k) if (cnt[k]) stat[k] /= (double)cnt[k];
    out.los_tap[i] = los_rows ? los / (double)los_rows : cd(0);    // bin-0 row mean, before static removal
    std::vector<std::vector<cd>> prof(w.rows);
    for (uint32_t r = 0; r < w.rows; ++r) {
      std::vector<cd> p = row_profile(w, a, i, r, L.delay_s[i] + s.delay_s[r], s.phase_rad[r], &stat);
      prof[r].assign(p.begin(), p.begin() + a.n_range);
    }
    for (uint32_t m = 0; m < a.n_range; ++m)
      for (uint32_t d = 0; d < a.n_dopp; ++d) {
        const double f = a.dopp0_hz + d * a.dopp_step_hz; cd acc = 0;
        for (uint32_t r = 0; r < w.rows; ++r) acc += prof[r][m] * win[r] * std::polar(1.0, -2 * M_PI * f * a.row_t_s[r]);
        out.rd.v[out.rd.idx(i, m, d)] = cf(acc / wsum);
      }
    std::vector<double> pw;
    for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d : a.tested_dopp) pw.push_back(std::norm(out.rd.v[out.rd.idx(i, m, d)]));
    out.noise[i] = pw.empty() ? 1.0 : std::max(median(pw) / std::log(2.0), std::numeric_limits<double>::min());
  }
  return out;
}

} // namespace nr_isac::coherent
