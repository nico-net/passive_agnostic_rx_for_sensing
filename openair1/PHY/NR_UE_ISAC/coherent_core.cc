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
double baseband_hz(const CfrWindow& w, uint32_t k) { return ((double)k - w.subcarriers / 2.0) * w.scs_hz; }
// Observed [lo,hi] subcarrier span of a row; returns false when the row is empty.
bool row_span(const CfrWindow& w, uint32_t r, uint32_t* lo, uint32_t* hi)
{
  int l = -1, h = -1;
  for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) { if (l < 0) l = (int)k; h = (int)k; }
  if (l < 0) return false;
  *lo = (uint32_t)l; *hi = (uint32_t)h; return true;
}
// Smallest spacing between consecutive observed subcarriers of a row: 1 = contiguous, 2 = comb-2
// (e.g. DM-RS-only rows). 0 when the row has fewer than two observed subcarriers.
uint32_t row_comb(const CfrWindow& w, uint32_t r)
{
  uint32_t c = 0, prev = UINT32_MAX;
  for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) {
    if (prev != UINT32_MAX && (c == 0 || k - prev < c)) c = k - prev;
    prev = k;
  }
  return c;
}
// Centred-index inverse FFT of one row after a delay ramp and a Hann window over
// the row's observed band; amplitude normalised by the window sum so every allocation gives the
// path amplitude at its peak. `sub` (optional, per subcarrier) is subtracted after the ramp.
// `unit` replaces the data by 1: the row's own point-spread (kernel) from its observed mask.
std::vector<cd> row_profile(const CfrWindow& w, const Axes& a, uint32_t ant, uint32_t row,
                            double ramp_delay_s, double phase_rad, const std::vector<cd>* sub = nullptr,
                            bool unit = false)
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
      const cd z = unit ? cd(1) : cd(w.values[w.sample(ant, row, k)]) * ramp - (sub ? (*sub)[k] : cd(0));
      buf[(size_t)((q % (int)a.n_fft + (int)a.n_fft) % (int)a.n_fft)] += z * win;
    }
  fft_inplace(buf, true);                         // inverse: sum * e^{+j...} / N
  if (wsum > 0) for (cd& v : buf) v *= (double)a.n_fft / wsum;
  return buf;
}
} // namespace

double gamma_upper_quantile(uint32_t shape, double p)
{
  if (shape == 0 || p >= 1) return 0.0;
  // log Q(shape,x) = -x + log sum_{k<shape} x^k/k!, summed in the log domain (online log-sum-exp):
  // the linear form underflows exp(-x) at x ~ 745, i.e. for R >~ 650 rows.
  auto logQ = [shape](double x) {
    if (x <= 0) return 0.0;
    const double lx = std::log(x);
    double t = 0, m = 0, acc = 1;                  // term k=0: log 1 = 0
    for (uint32_t k = 1; k < shape; ++k) {
      t += lx - std::log((double)k);
      if (t > m) { acc = acc * std::exp(m - t) + 1; m = t; } else acc += std::exp(t - m);
    }
    return -x + m + std::log(acc);
  };
  const double lp = std::log(p);
  double lo = 0, hi = 1; while (logQ(hi) > lp) hi *= 2;
  for (int i = 0; i < 200 && hi - lo > 1e-13 * hi; ++i) { const double mid = 0.5 * (lo + hi); (logQ(mid) > lp ? lo : hi) = mid; }
  return 0.5 * (lo + hi);
}

Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps)
{
  Axes a;
  auto fail = [&](const char* why) { a.valid = false; a.invalid_reason = why; return a; };
  if (!w.valid() || w.antennas != kCh) return fail("window invalid or not 4 antennas");
  if (w.rows < 3) return fail("fewer than 3 rows");
  for (uint32_t r = 1; r < w.rows; ++r) if (w.row_time_slots[r] < w.row_time_slots[r - 1]) return fail("non-monotonic row times");
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
  if (!a.valid) return L;
  const long N = (long)a.n_fft;
  auto wrap = [N](long n) { return (size_t)(((n % N) + N) % N); };
  // Rows' mean point-spread (power) from the observed masks alone, and their finest comb. Taking
  // the kernel from the masks keeps the sidelobe guard valid for comb/split rows, where the Hann
  // PSL bounds nothing (a comb-2 row has a full-power replica at n_fft/2; a gapped row has raised
  // sidelobes).
  std::vector<double> kern(a.n_fft, 0.0);
  uint32_t R = 0, dk_min = 0;
  for (uint32_t r = 0; r < w.rows; ++r) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
    const std::vector<cd> k = row_profile(w, a, 0, r, 0.0, 0.0, nullptr, true);
    for (size_t n = 0; n < kern.size(); ++n) kern[n] += std::norm(k[n]);
    const uint32_t c = row_comb(w, r); if (c && (!dk_min || c < dk_min)) dk_min = c;
    ++R;
  }
  if (R == 0) return L;
  for (double& v : kern) v /= R;
  // Delay is unambiguous only modulo n_fft/dk_min bins: fold into [-span/2, span/2), i.e. take the
  // (bit-identical, for comb-2) replica nearest 0 delay.
  const double span = (double)N / std::max(1u, dk_min);
  auto fold = [span](double bin) { return bin - span * std::round(bin / span); };
  long hm = 1;                                     // mainlobe half-width: first null of the kernel
  while (hm < N / 2 && kern[wrap(hm + 1)] < kern[wrap(hm)]) ++hm;
  double e_main = 0; for (long d = -hm; d <= hm; ++d) e_main += kern[wrap(d)];
  // Leakage relative to a path's own peak at offset d (+-1 bin: sub-bin position); zero inside the
  // mainlobe. leak_any: the same for a path ANYWHERE in a peak's mainlobe (merged, unresolved).
  auto leak = [&](long d) {
    const long f = std::lround(fold((double)d));
    if (std::labs(f) <= hm) return 0.0;
    return std::max({kern[wrap(f - 1)], kern[wrap(f)], kern[wrap(f + 1)]});
  };
  auto leak_any = [&](long d) { double m = 0; for (long x = -hm; x <= hm; ++x) m = std::max(m, leak(d - x)); return m; };

  for (uint32_t i = 0; i < kCh; ++i) {
    // Rows are combined NON-coherently: a common CFO / per-row phase rotates each row's profile, so
    // a coherent row mean cancels the LOS it is looking for (measured: 60 rows at 23 Hz CFO put the
    // estimate 20-28 bins off). Phase and the final sub-bin come from the coherent refinement below.
    std::vector<double> pw(a.n_fft, 0.0);
    for (uint32_t r = 0; r < w.rows; ++r) {
      uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
      const std::vector<cd> p = row_profile(w, a, i, r, 0.0, 0.0);
      for (size_t n = 0; n < pw.size(); ++n) pw[n] += std::norm(p[n]) / R;
    }
    // Noise-only mean of R exponential powers is Gamma(R, mu/R): its median is mu*Q^-1(R,1/2)/R.
    const double noise = median(pw) * R / gamma_upper_quantile(R, 0.5);
    const double thr = noise * gamma_upper_quantile(R, pfa) / R;
    const long strongest = (long)(std::max_element(pw.begin(), pw.end()) - pw.begin());
    auto P = [&](long n) { return pw[wrap(n)]; };
    // Peaks within one unambiguous span around the strongest (a comb replica is the same path).
    std::vector<long> peaks;
    for (long d = -(long)(span / 2); d < (long)(span / 2); ++d) {
      const long n = strongest + d;
      if (P(n) > thr && P(n) >= P(n - 1) && P(n) >= P(n + 1)) peaks.push_back(n);
    }
    std::sort(peaks.begin(), peaks.end(), [&](long x, long y) { return P(x) > P(y); });
    // A peak is a PATH only if it exceeds the leakage of the stronger paths plus noise at its
    // threshold, added as AMPLITUDES (static paths and the noise sample add coherently; a power sum
    // is not a bound). Each path leaks from its peak power AND from the excess energy of its
    // mainlobe over the kernel's -- paths merged into it (measured: a -14 dB target 4-6 bins
    // behind the LOS widened the mainlobe and lifted its first sidelobe past the single-path
    // level). Without this every leading sidelobe (-31/-41/-48 dB here) clears the noise threshold
    // and was taken for the LOS (measured 12-35 bins early).
    struct Path { long n; double p, excess; };
    std::vector<Path> paths;
    for (long q : peaks) {
      double amp = std::sqrt(thr);
      for (const Path& p : paths) amp += std::sqrt(p.p * leak(q - p.n)) + std::sqrt(p.excess * leak_any(q - p.n));
      if (P(q) <= amp * amp) continue;
      double e = 0; for (long d = -hm; d <= hm; ++d) e += P(q + d);
      paths.push_back({q, P(q), std::max(0.0, e - P(q) * e_main) / e_main});
    }
    // LOS = the earliest path within n_range bins before the strongest (the spec: not the strongest,
    // a wall reflection of the gNB must not bias calibration).
    long best = strongest;
    for (const Path& p : paths) {
      const long d = std::lround(fold((double)(p.n - strongest)));
      if (d < 0 && -d <= (long)a.n_range && d < best - strongest) best = strongest + d;
    }
    if (P(best) <= thr) continue;
    const double y0 = std::sqrt(P(best - 1)), y1 = std::sqrt(P(best)), y2 = std::sqrt(P(best + 1));
    const double den = y0 - 2 * y1 + y2;
    const double frac = (std::abs(den) > 0) ? 0.5 * (y0 - y2) / den : 0.0;
    L.delay_s[i] = fold((double)wrap(best) + std::clamp(frac, -0.5, 0.5)) * a.delay_step_s;
    L.snr[i] = P(best) / noise; L.found[i] = true;
  }
  // Coherent sub-bin refinement. The power mean above is phase-blind, so a moving path near the
  // LOS biases its peak (measured up to 0.27 bin from a -14 dB target 6 bins behind). With the
  // common CFO/SFO drift removed, the row mean is coherent: a moving path averages out over its
  // Doppler, and the hopping rows combine to the full union-band resolution.
  const RowSync s = estimate_row_sync(w, a, L);
  for (uint32_t i = 0; i < kCh; ++i) {
    if (!L.found[i]) continue;
    std::vector<cd> coh(a.n_fft, cd(0));
    for (uint32_t r = 0; r < w.rows; ++r) {
      uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
      const std::vector<cd> p = row_profile(w, a, i, r, s.delay_s[r], s.phase_rad[r]);
      for (size_t n = 0; n < coh.size(); ++n) coh[n] += p[n] / (double)R;
    }
    const long n0 = std::lround(L.delay_s[i] / a.delay_step_s);
    auto at = [&](long n) { return std::abs(coh[wrap(n)]); };
    long best = n0;
    for (long n = n0 - 2; n <= n0 + 2; ++n) if (at(n) > at(best)) best = n;
    const double y0 = at(best - 1), y1 = at(best), y2 = at(best + 1), den = y0 - 2 * y1 + y2;
    const double frac = (std::abs(den) > 0) ? 0.5 * (y0 - y2) / den : 0.0;
    L.delay_s[i] = fold((double)best + std::clamp(frac, -0.5, 0.5)) * a.delay_step_s;
    L.tap[i] = coh[wrap(best)];
  }
  return L;
}

RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& L)
{
  RowSync s; s.phase_rad.assign(w.rows, 0.0); s.delay_s.assign(w.rows, 0.0);
  if (!a.valid) return s;
  std::vector<std::array<cd, kCh>> tap(w.rows);
  std::vector<cd> slope(w.rows, cd(0));
  std::vector<uint32_t> comb(w.rows, 0);
  std::vector<double> fc(w.rows, 0.0);           // row's mean observed baseband frequency
  double fc_max = 0;
  for (uint32_t r = 0; r < w.rows; ++r) {
    comb[r] = row_comb(w, r);                      // phase step over the row's own finest spacing
    double nk = 0;
    for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) { fc[r] += baseband_hz(w, k); nk += 1; }
    if (nk > 0) fc[r] /= nk;
    fc_max = std::max(fc_max, std::abs(fc[r]));
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!L.found[i]) continue;
      cd acc = 0; double n = 0; uint32_t prev = UINT32_MAX; cd zp = 0;
      for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) {
        const cd z = cd(w.values[w.sample(i, r, k)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, k) * L.delay_s[i]);
        acc += z; n += 1;
        if (prev != UINT32_MAX && k - prev == comb[r]) slope[r] += z * std::conj(zp);
        prev = k; zp = z;
      }
      tap[r][i] = n > 0 ? acc / n : cd(0);
    }
  }
  // A row's tap carries a hop-dependent phase 2*pi*fc(r)*err from any error `err` in the reference
  // delay; with the allocation hopping across the band, 1 bin of error is +-2 rad row to row and
  // the CFO fit collapses (measured: a static wall pulled the power-mean LOS 1 bin late; the fit
  // returned 3.9 Hz for a 23 Hz CFO and the coherent LOS dropped to 0.17). So per channel, the
  // residual delay is taken where the rows combine best: max over (err, f) of
  // |sum_r tap_r e^{+j2pi fc(r) err} e^{-j2pi f t_r}|, err over one row resolution cell either way,
  // stepped so the hop phase error stays <= pi/4, f over the row-rate span at half a Doppler bin.
  if (fc_max > 0) {
    const double cell = 1.0 / a.b_eff_hz, step = 1.0 / (8 * fc_max);
    const double f_half = 1.0 / (2 * a.median_dt_s), f_step = 0.5 / a.t_cpi_s;
    // e^{-j2*pi*f*t_r} depends only on (f, r), never on err or channel -- hoisted out of the err loop
    // (and the channel loop, since it doesn't depend on i either) so the joint (err,f) search stops
    // multiplying polar() evaluations by n_err (and by kCh): was O(n_err*n_f*n_rows*kCh) polar() calls,
    // now O(n_f*n_rows) once, reused by every err step and every channel. Same f sequence (same start,
    // same step, computed once instead of identically re-derived per err) -> bit-identical results.
    std::vector<double> fs; for (double f = -f_half; f <= f_half; f += f_step) fs.push_back(f);
    std::vector<std::vector<cd>> ephase(fs.size(), std::vector<cd>(w.rows));
    for (size_t fi = 0; fi < fs.size(); ++fi)
      for (uint32_t r = 0; r < w.rows; ++r) ephase[fi][r] = std::polar(1.0, -2 * M_PI * fs[fi] * a.row_t_s[r]);
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!L.found[i]) continue;
      double best_err = 0, best_score = -1;
      for (double err = -cell; err <= cell; err += step) {
        std::vector<cd> y(w.rows);
        for (uint32_t r = 0; r < w.rows; ++r) y[r] = tap[r][i] * std::polar(1.0, 2 * M_PI * fc[r] * err);
        for (size_t fi = 0; fi < fs.size(); ++fi) {
          cd acc = 0; for (uint32_t r = 0; r < w.rows; ++r) acc += y[r] * ephase[fi][r];
          if (std::abs(acc) > best_score) { best_score = std::abs(acc); best_err = err; }
        }
      }
      for (uint32_t r = 0; r < w.rows; ++r) tap[r][i] *= std::polar(1.0, 2 * M_PI * fc[r] * best_err);
    }
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
  std::vector<double> tp, ph, td, dl; double prev_ph = 0;
  for (uint32_t r = 0; r < w.rows; ++r) {
    cd c = 0; for (uint32_t i = 0; i < kCh; ++i) if (L.found[i]) c += tap[r][i] * align[i];  // weight = LOS amplitude
    if (std::abs(c) > 0) {                                                                  // phase never needs a slope
      double p = std::arg(c);
      if (!ph.empty()) p = prev_ph + std::remainder(p - prev_ph, 2 * M_PI);                  // unwrap in row order
      prev_ph = p; tp.push_back(a.row_t_s[r]); ph.push_back(p);
    }
    if (std::abs(slope[r]) > 0) { td.push_back(a.row_t_s[r]); dl.push_back(-std::arg(slope[r]) / (2 * M_PI * comb[r] * w.scs_hz)); }
  }
  if (tp.empty()) return s;
  auto fit = [&](const std::vector<double>& t, const std::vector<double>& y, std::vector<double>& out) {
    double mt = 0, my = 0; for (size_t n = 0; n < t.size(); ++n) { mt += t[n]; my += y[n]; }
    mt /= t.size(); my /= t.size();
    double stt = 0, sty = 0; for (size_t n = 0; n < t.size(); ++n) { stt += (t[n] - mt) * (t[n] - mt); sty += (t[n] - mt) * (y[n] - my); }
    const double b = stt > 0 ? sty / stt : 0.0;
    for (uint32_t r = 0; r < w.rows; ++r) out[r] = my + b * (a.row_t_s[r] - mt);
  };
  fit(tp, ph, s.phase_rad);
  // Delay: DRIFT ONLY. The fit's intercept is the power-weighted centroid of all paths, not the
  // LOS (measured: a static 0.5-amplitude wall 15 m behind moved every channel's range axis by 1.2
  // bins and dropped |los_tap| to ~0.3). Absolute delay stays referenced to find_los's LOS.
  if (td.size() >= 2) {
    fit(td, dl, s.delay_s);
    double mean = 0; for (double d : s.delay_s) mean += d / w.rows;
    for (double& d : s.delay_s) d -= mean;
  }
  s.valid = true;
  return s;
}

RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s)
{
  RdResult out; out.rd.axes = a;
  if (!a.valid) return out;
  out.rd.v.assign((size_t)kCh * a.n_range * a.n_dopp, cf(0, 0));
  std::vector<double> win(w.rows); double wsum = 0;
  for (uint32_t r = 0; r < w.rows; ++r) { win[r] = hann(a.row_t_s.back() > 0 ? a.row_t_s[r] / a.row_t_s.back() : 0.5); wsum += win[r]; }
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

Calibration Calibrator::update(const std::array<cd, kCh>& tap, const std::array<bool, kCh>& found,
                               const std::array<double, kCh>& snr)
{
  Calibration c;
  const bool ref_ok = found[0] && std::abs(tap[0]) > 0;
  const cd ref = ref_ok ? std::conj(tap[0]) / std::abs(tap[0]) : cd(1);
  // Coherence of THIS CPI's LOS taps under the PREVIOUS calibration (predicted -> not tautological).
  if (init_ && ref_ok) {
    cd sum = 0; double pow_sum = 0;
    for (uint32_t i = 0; i < kCh; ++i) if (found[i] && std::abs(tap[i]) > 0) {
      const cd unit = tap[i] * ref / std::abs(tap[i]);
      sum += unit * std::conj(s_[i]); pow_sum += 1.0;
    }
    if (pow_sum > 1) { c.coherent_gain = std::norm(sum) / pow_sum; c.rho = std::clamp((c.coherent_gain - 1) / (pow_sum - 1), 0.0, 1.0); }
  }
  for (uint32_t i = 0; i < kCh; ++i) {
    c.los_found[i] = found[i]; c.los_snr[i] = snr[i];
    if (i == 0) { s_[0] = cd(1); p_[0] = 0; continue; }
    const double r = (found[i] && ref_ok && snr[i] > 0 && snr[0] > 0) ? 1 / (2 * snr[i]) + 1 / (2 * snr[0]) : 0;
    c.jitter_bound_rad[i] = r > 0 ? std::sqrt(r) : 0;
    if (!init_) { s_[i] = cd(1); p_[i] = M_PI * M_PI / 3; q_[i] = 0; nq_[i] = 0; }   // uniform-phase prior variance
    const double p_pred = p_[i] + q_[i];
    if (!(found[i] && ref_ok && r > 0)) { p_[i] = p_pred; continue; }            // predict only (no LOS)
    const cd z = tap[i] * ref / std::abs(tap[i]);
    const double nu = std::arg(z * std::conj(s_[i]));                           // wrapped innovation
    const double k = p_pred / (p_pred + r);
    s_[i] *= std::polar(1.0, k * nu);
    p_[i] = (1 - k) * p_pred;
    // covariance matching: innovation power beyond what the model predicts -> process noise
    const double q_obs = std::max(0.0, nu * nu - (p_pred + r));
    q_[i] = (q_[i] * nq_[i] + q_obs) / (nq_[i] + 1); ++nq_[i];
    c.jitter_rad[i] = std::sqrt(nu * nu);
  }
  init_ = true;
  for (uint32_t i = 0; i < kCh; ++i) {
    c.phase_rad[i] = std::arg(s_[i]); c.phase_var[i] = p_[i];
    c.coh_factor[i] = std::exp(-0.5 * std::min(p_[i], 50.0));
  }
  if (!init_ || c.coherent_gain <= 1.0) c.rho = std::max(0.0, c.rho);
  last_ = c;
  return c;
}

} // namespace nr_isac::coherent
