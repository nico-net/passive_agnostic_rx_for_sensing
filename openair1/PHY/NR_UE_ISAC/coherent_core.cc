/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_core.h"
#include "fft.h"
#include "robust_stats.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

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
  a.v_max_mps = f_max * a.lambda_m / 2;
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
  // The kernel is sampled kOvs times finer than the range bins: at integer bins the grid can
  // straddle the first null (128-PRB Hann rows: null at 5.33 bins, samples at 5 and 6 both near the
  // -31.5 dB sidelobe level, so the kernel kept falling to the NEXT null at 8 bins). A mainlobe
  // declared +-8 bins wide gave the first sidelobe (-6.4 bins) zero leakage, and it was taken for
  // the LOS on 3 of 4 channels (measured). The fine kernel at kOvs*d is the bin-d kernel exactly.
  constexpr long kOvs = 4;
  Axes af = a; af.n_fft = a.n_fft * kOvs;
  const long NF = (long)af.n_fft;
  auto wrapf = [NF](long n) { return (size_t)(((n % NF) + NF) % NF); };
  std::vector<double> kf(af.n_fft, 0.0);
  uint32_t R = 0, dk_min = 0;
  for (uint32_t r = 0; r < w.rows; ++r) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
    const std::vector<cd> k = row_profile(w, af, 0, r, 0.0, 0.0, nullptr, true);
    for (size_t n = 0; n < kf.size(); ++n) kf[n] += std::norm(k[n]);
    const uint32_t c = row_comb(w, r); if (c && (!dk_min || c < dk_min)) dk_min = c;
    ++R;
  }
  if (R == 0) return L;
  for (double& v : kf) v /= R;
  std::vector<double> kern(a.n_fft);
  for (long n = 0; n < N; ++n) kern[(size_t)n] = kf[(size_t)(kOvs * n)];
  // Delay is unambiguous only modulo n_fft/dk_min bins: fold into [-span/2, span/2), i.e. take the
  // (bit-identical, for comb-2) replica nearest 0 delay.
  const double span = (double)N / std::max(1u, dk_min);
  auto fold = [span](double bin) { return bin - span * std::round(bin / span); };
  long jn = 1;                                     // first null of the fine kernel
  while (jn < NF / 2 && kf[wrapf(jn + 1)] < kf[wrapf(jn)]) ++jn;
  const long hm = std::max(1L, jn / kOvs);         // mainlobe half-width in bins (last bin before the null)
  double e_main = 0; for (long d = -hm; d <= hm; ++d) e_main += kern[wrap(d)];
  // Leakage relative to a path's own peak at offset d (max of the fine kernel over +-1 bin: sub-bin
  // position); zero inside the mainlobe. leak_any: the same for a path ANYWHERE in a peak's
  // mainlobe (merged, unresolved).
  auto leak = [&](long d) {
    const long f = std::lround(fold((double)d));
    if (std::labs(f) <= hm) return 0.0;
    double m = 0; for (long j = kOvs * (f - 1); j <= kOvs * (f + 1); ++j) m = std::max(m, kf[wrapf(j)]);
    return m;
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
      double best_err = 0, best_score = -1; size_t best_fi = 0;
      auto score = [&](double err, size_t fi) {
        cd acc = 0; for (uint32_t r = 0; r < w.rows; ++r) acc += tap[r][i] * std::polar(1.0, 2 * M_PI * fc[r] * err) * ephase[fi][r];
        return std::abs(acc);
      };
      for (double err = -cell; err <= cell; err += step) {
        std::vector<cd> y(w.rows);
        for (uint32_t r = 0; r < w.rows; ++r) y[r] = tap[r][i] * std::polar(1.0, 2 * M_PI * fc[r] * err);
        for (size_t fi = 0; fi < fs.size(); ++fi) {
          cd acc = 0; for (uint32_t r = 0; r < w.rows; ++r) acc += y[r] * ephase[fi][r];
          if (std::abs(acc) > best_score) { best_score = std::abs(acc); best_err = err; best_fi = fi; }
        }
      }
      // The grid only FINDS the peak: applying a grid value leaves up to step/2 = 1/(16 fc_max) of
      // delay error, i.e. a random per-row hop phase of up to pi/8, which the line fit below turns into
      // a CFO error (measured: LOS-only, true CFO 0 -> -0.18 Hz, leaving the LOS 30-250x above noise at
      // range bins 0-3 across every Doppler bin after static removal, 5-9 false detections per CPI).
      // Refine: step/16 around the grid peak at its Doppler, then a parabola through the best three.
      const double fine = step / 16; double e0 = best_err;
      for (double err = best_err - step; err <= best_err + step; err += fine) {
        const double v = score(err, best_fi); if (v > best_score) { best_score = v; e0 = err; }
      }
      const double sm = score(e0 - fine, best_fi), s0 = score(e0, best_fi), sp = score(e0 + fine, best_fi), den = sm - 2 * s0 + sp;
      best_err = e0 + ((den < 0) ? std::clamp(0.5 * (sm - sp) / den, -0.5, 0.5) * fine : 0.0);
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
    // Keep the drift only if it makes the LOS flatter across each row's band (more row-coherent LOS
    // power) than no drift. The slope estimate (adjacent-subcarrier phase, sensitivity 2*pi*scs) is
    // weak: at 0 dB per RE it read 10-35 ns of drift over drift-free CPIs, leaving the LOS 4-90x above
    // noise at range bins 0-3 across every Doppler bin after static removal (1-6 false detections per
    // CPI, measured). The LOS tap's sensitivity is 2*pi*B, so a spurious drift shows here as lost LOS
    // power. Model selection on the objective the sync serves; no constant.
    auto los_pow = [&](bool drift) {
      double e = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (L.found[i])
        for (uint32_t r = 0; r < w.rows; ++r) {
          cd acc = 0;
          for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)])
            acc += cd(w.values[w.sample(i, r, k)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, k) * (L.delay_s[i] + (drift ? s.delay_s[r] : 0.0)));
          e += std::norm(acc);
        }
      return e;
    };
    if (los_pow(false) >= los_pow(true)) std::fill(s.delay_s.begin(), s.delay_s.end(), 0.0);
  }
  s.valid = true;
  return s;
}

RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s)
{
  RdResult out; out.rd.axes = a; out.los_found = L.found;
  if (!a.valid) return out;
  out.rd.v.assign((size_t)kCh * a.n_range * a.n_dopp, cf(0, 0));
  std::vector<double> win(w.rows); double wsum = 0;
  for (uint32_t r = 0; r < w.rows; ++r) { win[r] = hann(a.row_t_s.back() > 0 ? a.row_t_s[r] / a.row_t_s.back() : 0.5); wsum += win[r]; }
  // Noise window: n_range bins centred half-way between the paths [0, n_range) and their first comb
  // replica at n_fft/comb, kept n_range bins clear of both; else the in-crop median (fallback).
  uint32_t cmax = 1; for (uint32_t r = 0; r < w.rows; ++r) cmax = std::max(cmax, row_comb(w, r));
  const long rep = (long)a.n_fft / cmax, far0 = rep / 2 - (long)a.n_range / 2;
  const bool far_ok = far0 >= 2 * (long)a.n_range && far0 + 2 * (long)a.n_range <= rep;
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
    std::vector<std::vector<cd>> prof(w.rows), far(w.rows);
    for (uint32_t r = 0; r < w.rows; ++r) {
      std::vector<cd> p = row_profile(w, a, i, r, L.delay_s[i] + s.delay_s[r], s.phase_rad[r], &stat);
      prof[r].assign(p.begin(), p.begin() + a.n_range);
      if (far_ok) far[r].assign(p.begin() + far0, p.begin() + far0 + a.n_range);
    }
    for (uint32_t m = 0; m < a.n_range; ++m)
      for (uint32_t d = 0; d < a.n_dopp; ++d) {
        const double f = a.dopp0_hz + d * a.dopp_step_hz; cd acc = 0;
        for (uint32_t r = 0; r < w.rows; ++r) acc += prof[r][m] * win[r] * std::polar(1.0, -2 * M_PI * f * a.row_t_s[r]);
        out.rd.v[out.rd.idx(i, m, d)] = cf(acc / wsum);
      }
    // Noise from range bins far from every path (and every comb replica): white noise has the same
    // level there, while inside the volume's range crop a hopping allocation spreads each strong
    // target over its row mainlobe at ALL Doppler bins (random per-row phase 2*pi*fc(r)*dtau), so
    // the in-crop median measured that pedestal instead: 200x apart between channels whose targets
    // happened to cover more of the crop, which then set the envelope's channel weights (measured).
    std::vector<double> pw;
    if (far_ok) {
      for (uint32_t m = 0; m < a.n_range; ++m)
        for (uint32_t d : a.tested_dopp) {
          const double f = a.dopp0_hz + d * a.dopp_step_hz; cd acc = 0;
          for (uint32_t r = 0; r < w.rows; ++r) acc += far[r][m] * win[r] * std::polar(1.0, -2 * M_PI * f * a.row_t_s[r]);
          pw.push_back(std::norm(acc / wsum));
        }
    } else {
      for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d : a.tested_dopp) pw.push_back(std::norm(out.rd.v[out.rd.idx(i, m, d)]));
    }
    out.noise[i] = pw.empty() ? 1.0 : std::max(median(pw) / std::log(2.0), std::numeric_limits<double>::min());
  }
  // Ambiguity table (see RdResult::amb). A hopping allocation spreads a path over its row mainlobe
  // at every Doppler bin (the per-row phase 2*pi*fc(r)*dr is random across rows); a regular hop
  // stride puts a coherent ghost there instead; TDD gaps put replicas in Doppler. All of it is here.
  constexpr long O = RdResult::kAmbOvs;
  const long nr = a.n_range * O, nd = a.n_dopp * O;
  Axes ao = a; ao.n_fft = a.n_fft * O; const long NO = ao.n_fft;   // kernel at 1/O bin (zero-padded)
  std::vector<std::vector<cd>> ker(w.rows);
  for (uint32_t r = 0; r < w.rows; ++r) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
    const std::vector<cd> k = row_profile(w, ao, 0, r, 0.0, 0.0, nullptr, true);
    ker[r].resize(2 * nr + 1);
    for (long dr = -nr; dr <= nr; ++dr) ker[r][dr + nr] = k[(size_t)(((dr % NO) + NO) % NO)];
  }
  out.amb.assign((size_t)(2 * nr + 1) * (2 * nd + 1), 0.f);
  std::vector<cd> ph(w.rows);
  for (long dd = -nd; dd <= nd; ++dd) {
    for (uint32_t r = 0; r < w.rows; ++r) ph[r] = win[r] * std::polar(1.0, -2 * M_PI * ((double)dd / O) * a.dopp_step_hz * a.row_t_s[r]);
    for (long dr = -nr; dr <= nr; ++dr) {
      cd acc = 0; for (uint32_t r = 0; r < w.rows; ++r) if (!ker[r].empty()) acc += ph[r] * ker[r][dr + nr];
      out.amb[(size_t)((dr + nr) * (2 * nd + 1) + dd + nd)] = (float)(std::norm(acc) / (wsum * wsum));
    }
  }
  return out;
}

Vec3 Grid::at(size_t v) const
{
  const size_t ix = v % nx, iy = (v / nx) % ny, iz = v / ((size_t)nx * ny);
  return Vec3{origin.x + ix * step, origin.y + iy * step, origin.z + iz * step};
}

Grid envelope_grid(const Volume& vol, const Axes& a)
{
  Grid g;
  if (!(a.b_eff_hz > 0)) return g;
  g.step = kC / (4 * a.b_eff_hz); g.origin = Vec3{vol.x0, vol.y0, vol.z0};
  g.nx = (uint32_t)std::floor((vol.x1 - vol.x0) / g.step) + 1;
  g.ny = (uint32_t)std::floor((vol.y1 - vol.y0) / g.step) + 1;
  g.nz = (uint32_t)std::floor((vol.z1 - vol.z0) / g.step) + 1;
  return g;
}

namespace {
Vec3 unit(const Vec3& v) { const double n = norm(v); return n > 0 ? v / n : Vec3{}; }
// Channel ch's range axis at a fractional bin: cubic (4-point Lagrange) in magnitude, linear in
// wrapped phase between the two bracketing bins; false when outside. Not complex-linear: a row's
// profile carries a phase slope of 2*pi*f_centre(row)*delay_step per bin (~1.3 rad/bin for a
// 128-PRB row hopped 26 MHz off centre), so a complex lerp dips ~2 dB between bins. Nor linear in
// magnitude: a piecewise-linear profile peaks only AT bins, so the envelope maximum snapped to
// wherever the channels sat on integer bins (per-channel peaks read 0.13-0.46 bins off; the
// isolated-target envelope maximum 4 m off along this geometry's weak axis -- measured).
bool sample_rd(const RdResult& R, uint32_t ch, double bin, uint32_t d, cd* out)
{
  const uint32_t n = R.rd.axes.n_range;
  if (n == 0 || !(bin >= 0) || bin > (double)(n - 1)) return false;
  const uint32_t b0 = (uint32_t)bin, b1 = std::min(b0 + 1, n - 1); const double t = bin - b0;
  auto mag = [&](long b) { return (double)std::abs(R.rd.v[R.rd.idx(ch, (uint32_t)std::clamp(b, 0L, (long)n - 1), d)]); };
  const double pm = mag((long)b0 - 1), p0 = mag(b0), p1 = mag((long)b0 + 1), p2 = mag((long)b0 + 2);
  const double m = -pm * t * (t - 1) * (t - 2) / 6 + p0 * (t + 1) * (t - 1) * (t - 2) / 2
                   - p1 * (t + 1) * t * (t - 2) / 2 + p2 * (t + 1) * t * (t - 1) / 6;
  const cd v0 = R.rd.v[R.rd.idx(ch, b0, d)], v1 = R.rd.v[R.rd.idx(ch, b1, d)];
  *out = std::polar(std::max(0.0, m), std::arg(v0) + t * std::remainder(std::arg(v1) - std::arg(v0), 2 * M_PI));
  return true;
}
// Channels with a LOS reference: only those have a meaningful excess-delay axis.
uint32_t n_used(const RdResult& R) { uint32_t n = 0; for (bool f : R.los_found) n += f; return n; }
// Doppler search half-width at x (bins): a target moving at up to v_max has bistatic Doppler
// -v.(u_tx + u_i)/lambda on channel i, so channels differ by up to v_max*max|u_i - u_j|/lambda
// (u_i = unit vector rx_i -> x; 5-10 m baselines see a nearby target from very different sides).
uint32_t dopp_half(const Axes& a, const Geometry& geo, const std::array<bool, kCh>& used, const Vec3& x)
{
  if (!(a.v_max_mps > 0) || !(a.dopp_step_hz > 0)) return 0;
  Vec3 u[kCh]; for (uint32_t i = 0; i < kCh; ++i) u[i] = unit(x - geo.rx[i]);
  double m = 0;
  for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j) if (used[i] && used[j]) m = std::max(m, norm(u[i] - u[j]));
  return (uint32_t)std::ceil(a.v_max_mps * m / (a.lambda_m * a.dopp_step_hz) - 1e-9);
}
bool dopp_ok(const Axes& a, long d)  // on the axis and outside the zero-Doppler notch
{
  return d >= 0 && d < (long)a.n_dopp && std::abs(a.dopp0_hz + d * a.dopp_step_hz) > a.notch_half_bins * a.dopp_step_hz;
}
// Channel i's strongest Doppler bin within +-dh of d at excess-delay bin `bin`: power / noise.
double channel_peak(const RdResult& R, uint32_t i, double bin, long d, uint32_t dh, uint32_t* best_d)
{
  double best = 0; *best_d = (uint32_t)std::max(0L, d); cd s;
  for (long e = d - (long)dh; e <= d + (long)dh; ++e)
    if (dopp_ok(R.rd.axes, e) && sample_rd(R, i, bin, (uint32_t)e, &s) && std::norm(s) / R.noise[i] > best) {
      best = std::norm(s) / R.noise[i]; *best_d = (uint32_t)e;
    }
  return best;
}
// Survival of the n-fold sum of Y = max of m Exp(1), on a grid of step h (surv[j] = P(S >= j*h)).
struct MaxExpNull { double h = 0; std::vector<double> surv; };
const MaxExpNull& max_exp_null(uint32_t m, uint32_t n)
{
  thread_local std::map<std::pair<uint32_t, uint32_t>, MaxExpNull> cache;
  const auto key = std::make_pair(m, n);
  const auto it = cache.find(key); if (it != cache.end()) return it->second;
  MaxExpNull z; z.h = 1.0 / 64;                          // thresholds are O(10): grid error n*h/2 is negligible
  const double ymax = std::log((double)m) + 60;          // P(Y > ymax) <= m e^-ymax = e^-60
  const size_t ny = (size_t)std::ceil(ymax / z.h);
  size_t N = 1; while (N < n * ny + 1) N <<= 1;           // no wrap of the n-fold convolution
  auto F = [m](double y) { return y <= 0 ? 0.0 : std::exp(m * std::log1p(-std::exp(-y))); };
  std::vector<cd> p(N, cd(0));
  for (size_t j = 0; j < ny; ++j) p[j] = F((j + 1) * z.h) - F(j * z.h);   // exact bin probabilities
  fft_inplace(p, false);
  for (cd& v : p) v = std::pow(v, (double)n);
  fft_inplace(p, true);
  z.surv.assign(N + 1, 0.0);
  for (size_t j = N; j-- > 0;) z.surv[j] = z.surv[j + 1] + std::max(0.0, p[j].real());
  return cache.emplace(key, std::move(z)).first->second;
}
} // namespace

double max_exp_sum_quantile(uint32_t m, uint32_t n, double p)
{
  if (m == 0 || n == 0 || !(p > 0) || p >= 1) return 0.0;
  const MaxExpNull& z = max_exp_null(m, n);
  size_t j = 1; while (j < z.surv.size() && z.surv[j] > p) ++j;
  if (j >= z.surv.size()) return (double)j * z.h;
  // log-linear between j-1 and j; the sum of n bins spans [sum j_i*h, sum j_i*h + n*h): centre +n*h/2
  const double l0 = std::log(z.surv[j - 1]), l1 = std::log(std::max(z.surv[j], 1e-300));
  const double t = (l0 - std::log(p)) / (l0 - l1);
  return (j - 1 + t) * z.h + 0.5 * (n - 1) * z.h;
}

std::vector<float> envelope(const RdResult& R, const Grid& g, const Geometry& geo)
{
  const Axes& a = R.rd.axes;
  const size_t nt = a.tested_dopp.size();
  std::vector<float> E(nt * g.size(), 0.f);
  std::vector<double> col(a.n_dopp);
  for (size_t v = 0; v < g.size(); ++v) {
    const Vec3 x = g.at(v);
    const uint32_t dh = dopp_half(a, geo, R.los_found, x);
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!R.los_found[i]) continue;
      const double bin = excess_delay_s(x, geo.tx, geo.rx[i]) / a.delay_step_s;
      cd s; bool in = false; const double nz = R.noise[i];
      for (uint32_t d = 0; d < a.n_dopp; ++d) {
        col[d] = (dopp_ok(a, d) && sample_rd(R, i, bin, d, &s)) ? std::norm(s) / nz : 0.0;
        in = in || col[d] > 0;
      }
      if (!in) continue;
      for (size_t t = 0; t < nt; ++t) {                  // each channel at its own Doppler within +-dh
        const long d = a.tested_dopp[t]; double m = 0;
        for (long e = std::max(0L, d - (long)dh); e <= std::min<long>(a.n_dopp - 1, d + dh); ++e) m = std::max(m, col[e]);
        E[t * g.size() + v] += (float)m;
      }
    }
  }
  return E;
}

DetectParams detect_params(const Axes& a, const Grid& g, double intensity)
{
  DetectParams p;
  const double tested = (double)std::max<size_t>(1, g.size()) * std::max<size_t>(1, a.tested_dopp.size());
  p.pfa = std::min(0.5, intensity * a.t_cpi_s / tested);
  return p;
}

std::vector<Detection> detect(const std::vector<float>& E, const RdResult& R, const Grid& g,
                              const Geometry& geo, const DetectParams& p)
{
  const Axes& a = R.rd.axes;
  std::vector<Detection> out;
  const uint32_t n = n_used(R);
  const size_t nt = a.tested_dopp.size(), nv = g.size();
  if (n == 0 || nv == 0 || !(p.pfa > 0) || E.size() != nt * nv) return out;
  // Per voxel: the Doppler half-width, hence per (voxel, bin) the number m of searched bins.
  std::vector<uint32_t> dh(nv);
  for (size_t v = 0; v < nv; ++v) dh[v] = dopp_half(a, geo, R.los_found, g.at(v));
  std::vector<uint32_t> okc(a.n_dopp + 1, 0);
  for (uint32_t d = 0; d < a.n_dopp; ++d) okc[d + 1] = okc[d] + dopp_ok(a, d);
  auto m_of = [&](size_t t, size_t v) {
    const long d = a.tested_dopp[t];
    const long lo = std::max(0L, d - (long)dh[v]), hi = std::min<long>(a.n_dopp - 1, d + dh[v]);
    return std::max<uint32_t>(1, okc[hi + 1] - okc[lo]);
  };
  // Null of E at (t,v): scale_t * S(m, n). The scale is the per-Doppler median of E/median(S(m,n))
  // (robust; every m-component has median 1 after that normalisation). Correlated neighbouring
  // Doppler bins make the true max-of-m null lighter than the iid one: the threshold is conservative.
  std::map<uint32_t, std::pair<double, double>> q;       // m -> (median, threshold) of S(m, n)
  auto null_of = [&](uint32_t m) -> const std::pair<double, double>& {
    auto it = q.find(m);
    if (it == q.end()) it = q.emplace(m, std::make_pair(max_exp_sum_quantile(m, n, 0.5), max_exp_sum_quantile(m, n, p.pfa))).first;
    return it->second;
  };
  std::vector<double> scale(nt, 0.0);
  for (size_t t = 0; t < nt; ++t) {
    std::vector<double> z(nv);
    for (size_t v = 0; v < nv; ++v) z[v] = E[t * nv + v] / null_of(m_of(t, v)).first;
    scale[t] = median(std::move(z));
  }
  struct Cand { size_t t, v; double e, thr; };
  std::vector<Cand> c;
  const long NX = g.nx, NY = g.ny, NZ = g.nz;
  for (size_t t = 0; t < nt; ++t) {
    if (!(scale[t] > 0)) continue;
    const float* Et = &E[t * nv];
    for (size_t v = 0; v < nv; ++v) {
      const double thr = scale[t] * null_of(m_of(t, v)).second;
      if (!(Et[v] > thr)) continue;
      const long ix = v % NX, iy = (v / NX) % NY, iz = v / (NX * NY);
      bool peak = true;                                   // spatial local maximum (spec C6)
      for (long dz = -1; dz <= 1 && peak; ++dz) for (long dy = -1; dy <= 1 && peak; ++dy) for (long dx = -1; dx <= 1; ++dx) {
        const long x = ix + dx, y = iy + dy, z = iz + dz;
        if ((dx || dy || dz) && x >= 0 && x < NX && y >= 0 && y < NY && z >= 0 && z < NZ && Et[(z * NY + y) * NX + x] > Et[v]) { peak = false; break; }
      }
      if (peak) c.push_back({t, v, Et[v], thr});
    }
  }
  // pk_*: each channel's own RD peak for this detection (sub-bin delay and Doppler, power): where its
  // leakage into other cells is centred.
  struct Acc { Vec3 x; std::array<uint32_t, kCh> d{}; std::array<double, kCh> pw{}, bin{}, pk_r{}, pk_f{}, pk_p{}; };
  struct Item { Acc me; size_t t = 0, v_ = 0; long d = 0; double thr = 0, ec = 0; std::array<double, kCh> leak{}; bool alive = true; };
  std::vector<Item> items;
  const double nms_r = 2 * g.step, res = kC / (2 * a.b_eff_hz);
  // RD mainlobe half-width (bins): first minimum of the ambiguity along delay at zero Doppler offset.
  long hm_r = 1;
  while (hm_r < (long)a.n_range && R.ambiguity(hm_r + 1, 0) < R.ambiguity(hm_r, 0)) ++hm_r;
  for (const Cand& k : c) {
    const Vec3 x = g.at(k.v);
    if (x.z < 0) continue;                                              // ground-bounce mirrors
    const long d = a.tested_dopp[k.t], w0 = d - (long)dh[k.v], w1 = d + (long)dh[k.v];
    // Per channel: the local Doppler maxima inside the search window, strongest first (up to 3).
    // Independent per-channel maxima can mix different targets' energy on different channels
    // (range-only multilateration ghosts; measured 112 detections for 3 targets). A real target's
    // channel Dopplers come from ONE velocity: f_i = -g_i.v/lambda, g_i = u_tx + u_i. With 4
    // channels G (4x3) has a left null vector nu, and sum nu_i f_i = 0; each f_i is its bin to
    // +-1 bin, so the test is |sum nu_i f_i| <= step * sum |nu_i|. Fewer channels: no redundancy.
    Acc me; me.x = x;
    std::array<std::vector<std::pair<double, uint32_t>>, kCh> pk;
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      me.bin[i] = excess_delay_s(x, geo.tx, geo.rx[i]) / a.delay_step_s;
      std::vector<double> col(w1 - w0 + 1, -1.0); cd sv;
      for (long e = w0; e <= w1; ++e)
        if (dopp_ok(a, e) && sample_rd(R, i, me.bin[i], (uint32_t)e, &sv)) col[e - w0] = std::norm(sv) / R.noise[i];
      for (long e = w0; e <= w1; ++e) {
        const double v = col[e - w0];
        if (v >= 0 && (e == w0 || v >= col[e - 1 - w0]) && (e == w1 || v >= col[e + 1 - w0])) pk[i].push_back({v, (uint32_t)e});
      }
      std::sort(pk[i].begin(), pk[i].end(), [](const auto& l, const auto& r) { return l.first > r.first; });
      if (pk[i].size() > 3) pk[i].resize(3);
      if (pk[i].empty()) pk[i].push_back({0.0, (uint32_t)std::clamp(d, 0L, (long)a.n_dopp - 1)});
    }
    std::array<double, kCh> nu{};
    if (n == kCh) {
      Vec3 gv[kCh]; const Vec3 ut = unit(x - geo.tx);
      for (uint32_t i = 0; i < kCh; ++i) gv[i] = ut + unit(x - geo.rx[i]);
      for (uint32_t i = 0; i < kCh; ++i) {             // cofactor expansion: nu_i = (-1)^i det(G without row i)
        Vec3 r3[3]; uint32_t q = 0; for (uint32_t j = 0; j < kCh; ++j) if (j != i) r3[q++] = gv[j];
        nu[i] = ((i & 1) ? -1.0 : 1.0) * dot(r3[0], cross(r3[1], r3[2]));
      }
    }
    double nu_abs = 0; for (double v : nu) nu_abs += std::abs(v);
    double ec = -1; std::array<uint32_t, kCh> idx{}, pick{};
    std::array<size_t, kCh> cnt{}; for (uint32_t i = 0; i < kCh; ++i) cnt[i] = R.los_found[i] ? pk[i].size() : 1;
    for (idx[0] = 0; idx[0] < cnt[0]; ++idx[0]) for (idx[1] = 0; idx[1] < cnt[1]; ++idx[1])
      for (idx[2] = 0; idx[2] < cnt[2]; ++idx[2]) for (idx[3] = 0; idx[3] < cnt[3]; ++idx[3]) {
        double e = 0, cons = 0;
        for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
          e += pk[i][idx[i]].first; cons += nu[i] * (a.dopp0_hz + pk[i][idx[i]].second * a.dopp_step_hz);
        }
        if (nu_abs > 0 && std::abs(cons) > a.dopp_step_hz * nu_abs) continue;
        if (e > ec) { ec = e; pick = idx; }
      }
    if (!(ec > k.thr)) continue;                          // E_c <= E: the max-of-m threshold stays conservative
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) { me.pw[i] = pk[i][pick[i]].first; me.d[i] = pk[i][pick[i]].second; }
    // Sub-voxel envelope position at the chosen per-channel Dopplers (pattern search: move while
    // better, halve when the centre wins). The voxel maximum of the Doppler-free E sat 4.5 m off an
    // isolated target along this geometry's weak axis (singular value 0.49), where E is flat to
    // 0.1 dB and other targets' energy on nearby delays tipped it. Stops once a step moves a
    // bistatic path by < 0.1 range bin, below the delay interpolation's own accuracy.
    auto e_at = [&](const Vec3& y, std::array<double, kCh>* pw, std::array<double, kCh>* bn) {
      double e = 0; cd sv;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
        (*bn)[i] = excess_delay_s(y, geo.tx, geo.rx[i]) / a.delay_step_s;
        (*pw)[i] = sample_rd(R, i, (*bn)[i], me.d[i], &sv) ? std::norm(sv) / R.noise[i] : 0.0;
        e += (*pw)[i];
      }
      return e;
    };
    // The walk stays inside the candidate's own RD mainlobe on every channel (else it climbs onto
    // another target's energy at a shared per-channel Doppler: measured a 14 m walk) and in the grid.
    const Vec3 hi_box = g.origin + Vec3{(g.nx - 1) * g.step, (g.ny - 1) * g.step, (g.nz - 1) * g.step};
    std::array<double, kCh> bin0{};
    for (uint32_t i = 0; i < kCh; ++i) bin0[i] = excess_delay_s(x, geo.tx, geo.rx[i]) / a.delay_step_s;
    auto inside = [&](const Vec3& y) {
      if (y.x < g.origin.x || y.y < g.origin.y || y.z < std::max(0.0, g.origin.z) || y.x > hi_box.x || y.y > hi_box.y || y.z > hi_box.z) return false;
      for (uint32_t i = 0; i < kCh; ++i)
        if (R.los_found[i] && std::abs(excess_delay_s(y, geo.tx, geo.rx[i]) / a.delay_step_s - bin0[i]) > hm_r) return false;
      return true;
    };
    {
      std::array<double, kCh> pw, bn; double best = e_at(me.x, &me.pw, &me.bin);
      for (double st = g.step / 2; 2 * st >= 0.1 * kC * a.delay_step_s;) {
        Vec3 bx = me.x;
        for (int iz = -1; iz <= 1; ++iz) for (int iy = -1; iy <= 1; ++iy) for (int ix = -1; ix <= 1; ++ix) {
          const Vec3 y = me.x + Vec3{ix * st, iy * st, iz * st};
          if (!inside(y)) continue;
          const double e = e_at(y, &pw, &bn);
          if (e > best) { best = e; bx = y; me.pw = pw; me.bin = bn; }
        }
        if (dist(bx, me.x) == 0) st /= 2; else me.x = bx;
      }
      ec = best;
    }
    // Locate each channel's own peak: delay by cubic interpolation within the RD mainlobe of the
    // position-implied delay (the position fits all channels jointly and can sit ~1 bin off any one
    // of them along a weak axis), Doppler by a parabola through the chosen bin and its neighbours.
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      cd sv; double best = -1;
      for (double b = me.bin[i] - hm_r; b <= me.bin[i] + hm_r; b += 1.0 / (4 * RdResult::kAmbOvs))
        if (sample_rd(R, i, b, me.d[i], &sv) && std::norm(sv) > best) { best = std::norm(sv); me.pk_r[i] = b; }
      if (best < 0) { me.pk_r[i] = me.bin[i]; best = 0; }
      double y[3] = {0, best, 0};
      for (int j : {-1, 1}) if (dopp_ok(a, (long)me.d[i] + j) && sample_rd(R, i, me.pk_r[i], me.d[i] + j, &sv)) y[j + 1] = std::norm(sv);
      const double den = y[0] - 2 * y[1] + y[2];
      const double fr = (den < 0) ? std::clamp(0.5 * (y[0] - y[2]) / den, -0.5, 0.5) : 0.0;
      me.pk_f[i] = me.d[i] + fr; me.pk_p[i] = best / R.noise[i];
    }
    Item it; it.me = me; it.t = k.t; it.v_ = k.v; it.d = d; it.thr = k.thr; it.ec = ec;
    items.push_back(it);
  }
  // Greedy residual pursuit: repeatedly accept the candidate whose energy NOT explained by the
  // detections accepted so far is the largest (per-channel noise units). Strongest-raw-E
  // first let a ghost that borrowed a strong target's channels win before the weaker real target
  // it also borrowed from, which was then "explained" by the ghost (measured: drone lost 18/20).
  // Kept only if the unexplained energy exceeds the noise threshold. Per channel the observed
  // amplitude is at most noise + leakage (amplitudes), so max(0, |y_i| - |leak_i|) is at most the
  // noise amplitude and the sum of their squares at most the noise statistic: P(> thr) <= pfa, a
  // strict bound (reverse triangle inequality). Unlike a power difference it is insensitive to the
  // ~1e-4 error of a leakage predicted at a peak (above the noise threshold at 60+ dB SNR:
  // measured), and unlike a whole-statistic amplitude sum it lets a channel that IS explained (a
  // target unresolved from a stronger one there) carry no evidence instead of vetoing the others.
  for (;;) {
    long pick = -1; double best = 0;
    for (size_t q = 0; q < items.size(); ++q) {
      Item& it = items[q]; if (!it.alive) continue;
      double resid = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) resid += std::pow(std::max(0.0, std::sqrt(it.me.pw[i]) - it.leak[i]), 2);
      if (resid <= it.thr) { it.alive = false; continue; }     // leakage only grows: dead for good
      if (resid > best) { best = resid; pick = (long)q; }
    }
    if (pick < 0) break;
    Item& k = items[pick]; k.alive = false;
    const Acc& s = k.me;
    for (Item& it : items) {
      if (!it.alive) continue;
      const Acc& me = it.me;
      const bool local = dist(s.x, me.x) <= nms_r;             // NMS radius (spec C6)
      bool same = local, harm = local; long kk = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
        const double fs = a.dopp0_hz + s.d[i] * a.dopp_step_hz, fc = a.dopp0_hz + me.d[i] * a.dopp_step_hz;
        same = same && std::labs((long)me.d[i] - (long)s.d[i]) <= 1;                   // NMS: same per-channel peaks
        if (!kk) kk = std::lround(fc / fs);
        harm = harm && kk >= 2 && std::abs(fc - kk * fs) <= a.dopp_step_hz;             // k-th harmonic of a stronger one
      }
      if (local && (same || harm)) { it.alive = false; continue; }
      // Leakage of the accepted detection into the cells this candidate read (its own per-channel
      // Doppler bins): its peak power times the waveform ambiguity at the offset (range sidelobes,
      // random-hop pedestal, regular-hop ghosts, TDD/grant-gap Doppler replicas -- the slow-time
      // PSF), +-1 table cell for the estimates' error.
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
        const double dr0 = me.bin[i] - s.pk_r[i], dd0 = me.d[i] - s.pk_f[i]; float am = 0;
        for (long u = -1; u <= 1; ++u) for (long v = -1; v <= 1; ++v)
          am = std::max(am, R.ambiguity(dr0 + (double)u / RdResult::kAmbOvs, dd0 + (double)v / RdResult::kAmbOvs));
        it.leak[i] += std::sqrt(s.pk_p[i] * am);            // amplitudes: leakages add coherently at worst
      }
    }
    const Acc& me = s; const long d = k.d; const double ec = k.ec;
    Detection det; det.pos = det.pos_env = me.x; det.dopp_bin = (uint32_t)d;
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) det.chan_dopp_bin[i] = (int32_t)me.d[i];
    double fsum = 0; for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) fsum += a.dopp0_hz + me.d[i] * a.dopp_step_hz;
    det.doppler_hz = fsum / n;                                           // channel mean (centroid view)
    det.range_rate_mps = -a.lambda_m * det.doppler_hz;
    double hm = 0; for (uint32_t j = 1; j <= m_of(k.t, k.v_); ++j) hm += 1.0 / j;   // E[max of m Exp(1)]
    det.snr = std::max(1e-6, (ec / scale[k.t] - n * hm) / n);
    det.tx = geo.tx;
    const double sd = std::max(res / std::sqrt(2 * det.snr), g.step / std::sqrt(12.0));
    det.pos_sigma = Vec3{sd, sd, sd};
    const double rr = a.lambda_m * a.dopp_step_hz;
    det.range_rate_sigma = std::max(rr / std::sqrt(2 * det.snr), rr / std::sqrt(12.0));
    out.push_back(det);
  }
  return out;
}

void refine(Detection& det, const RdResult& R, const Grid& g, const Geometry& geo, const Calibration& cal,
            const std::array<double, kCh>& survey_sigma_m)
{
  const Axes& a = R.rd.axes;
  Vec3 centroid{}; for (const Vec3& r : geo.rx) centroid = centroid + r * (1.0 / kCh);
  double D = 0;
  for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j)
    if (R.los_found[i] && R.los_found[j]) D = std::max(D, dist(geo.rx[i], geo.rx[j]));
  if (!(D > 0) || !(g.step > 0)) return;                               // < 2 channels: no fringes
  const double Rng = std::max(dist(det.pos_env, centroid), g.step);
  const double fringe = a.lambda_m * Rng / (2 * D), fs = fringe / 2;
  // Each channel at its own Doppler bin: detect()'s velocity-consistent choice, else the strongest
  // within the per-channel search width.
  const uint32_t dh = dopp_half(a, geo, R.los_found, det.pos_env);
  std::array<uint32_t, kCh> db{};
  for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
    if (det.chan_dopp_bin[i] >= 0 && det.chan_dopp_bin[i] < (int32_t)a.n_dopp) db[i] = (uint32_t)det.chan_dopp_bin[i];
    else channel_peak(R, i, excess_delay_s(det.pos_env, geo.tx, geo.rx[i]) / a.delay_step_s, det.dopp_bin, dh, &db[i]);
  }
  // The LOS calibration is survey-independent, so rho cannot see survey error; the target phase
  // can: sigma_phi_i = (2pi/lambda)|u_tx,i - u_x,i| sigma_s,i (phase sensitivity to rx_i's position).
  double keep = 0; uint32_t nu = 0;
  for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
    const double sp = 2 * M_PI / a.lambda_m * norm(unit(geo.tx - geo.rx[i]) - unit(det.pos_env - geo.rx[i])) * survey_sigma_m[i];
    keep += std::exp(-0.5 * sp * sp); ++nu;
  }
  const double rho = cal.rho * (nu ? keep / nu : 0.0);
  auto coh = [&](const Vec3& x, std::array<cd, kCh>* terms) {
    cd sum = 0;
    for (uint32_t i = 0; i < kCh; ++i) {
      (*terms)[i] = 0;
      if (!R.los_found[i]) continue;
      const double ex = excess_delay_s(x, geo.tx, geo.rx[i]); cd s;
      if (!sample_rd(R, i, ex / a.delay_step_s, db[i], &s)) continue;
      (*terms)[i] = cal.coh_factor[i] * std::polar(1.0, -cal.phase_rad[i]) * s / std::sqrt(R.noise[i])
                    * std::polar(1.0, 2 * M_PI * a.fc_hz * ex);
      sum += (*terms)[i];
    }
    return std::norm(sum);
  };
  // Hierarchical search: 5^3 points at step/2 over +-one envelope step, recentre, halve, down to half
  // a fringe. ponytail: the coherent statistic has grating lobes one fringe apart across the whole
  // envelope blob; coarse levels alias them, so this finds A fringe peak near the envelope position,
  // not provably the global one -- inherent to phase-only focusing at this aperture.
  Vec3 bx = det.pos_env; std::array<cd, kCh> bt{}, t{};
  double best = coh(bx, &bt);
  for (double st = g.step / 2; ; st /= 2) {
    const Vec3 c0 = bx;
    for (int iz = -2; iz <= 2; ++iz) for (int iy = -2; iy <= 2; ++iy) for (int ix = -2; ix <= 2; ++ix) {
      const Vec3 x = c0 + Vec3{ix * st, iy * st, iz * st};
      const Vec3 o = x - det.pos_env;                       // extent: +-one envelope step (spec C5.2)
      if (x.z < 0 || std::abs(o.x) > g.step || std::abs(o.y) > g.step || std::abs(o.z) > g.step) continue;
      const double v = coh(x, &t);
      if (v > best) { best = v; bx = x; bt = t; }
    }
    if (st <= fs) break;
  }
  det.pos = bx * rho + det.pos_env * (1 - rho);
  det.terms = bt;
  const double res = kC / (2 * a.b_eff_hz);
  const double sd_env = std::max(res / std::sqrt(2 * det.snr), g.step / std::sqrt(12.0));
  const double sd_coh = std::max(fringe / std::sqrt(2 * det.snr), fs / std::sqrt(12.0));
  const double sd = rho * sd_coh + (1 - rho) * sd_env;
  det.pos_sigma = Vec3{sd, sd, sd};
  det.refined = true;
}

Calibration Calibrator::update(const std::array<cd, kCh>& tap, const std::array<bool, kCh>& found,
                               const std::array<double, kCh>& snr)
{
  Calibration c;
  const bool ref_ok = found[0] && std::abs(tap[0]) > 0;
  const cd ref = ref_ok ? std::conj(tap[0]) / std::abs(tap[0]) : cd(1);
  // Coherence of THIS CPI's LOS taps under the PREVIOUS calibration (predicted -> not tautological).
  // Only channels ALREADY seeded (calibrated before this CPI) count: an unseeded channel's s_ is a
  // placeholder, not a real reference, so it cannot evidence coherence either way.
  if (init_ && ref_ok) {
    cd sum = 0; double pow_sum = 0;
    for (uint32_t i = 0; i < kCh; ++i) if (found[i] && std::abs(tap[i]) > 0 && (i == 0 || seeded_[i])) {
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
    const bool valid_meas = found[i] && ref_ok && r > 0;
    if (!seeded_[i]) {
      p_[i] = M_PI * M_PI / 3;                            // uninformative until this channel's own first measurement
      if (!valid_meas) continue;
      // First valid measurement for this channel: the prior is not a model prediction, so its
      // "innovation" is not evidence about q -- seed s_/p_ directly from the measurement and skip
      // the innovation/q update entirely (do not count it in nq_).
      const cd z = tap[i] * ref / std::abs(tap[i]);
      s_[i] = z; p_[i] = r; q_[i] = 0; nu2_[i] = 0; nq_[i] = 0; seeded_[i] = true;
      continue;
    }
    const double p_prev = p_[i];                          // posterior BEFORE this CPI's process noise
    const double q_use = std::max(0.0, q_[i]);             // clip only at the point of use
    const double p_pred = p_prev + q_use;
    if (!valid_meas) { p_[i] = p_pred; continue; }         // predict only (no LOS)
    const cd z = tap[i] * ref / std::abs(tap[i]);
    const double nu = std::arg(z * std::conj(s_[i]));      // wrapped innovation
    const double k = p_pred / (p_pred + r);
    s_[i] *= std::polar(1.0, k * nu);
    p_[i] = (1 - k) * p_pred;
    // Mehra-style covariance matching: the unbiased per-step estimator of the TRUE q is
    // nu^2-(p_prev+r), against p_prev (the posterior BEFORE this CPI's q), not p_pred (which already
    // contains the current q estimate -- subtracting p_pred feeds q's own estimate back into itself
    // and biases it low). Accumulated WITHOUT per-sample clipping (E[max(0,X)] != max(0,E[X]); a
    // max(0,.) on each sample biases the mean up by E[(chi^2_1-1)^+] ~ 0.48); q is clamped to >=0
    // only where it is applied, in p_pred above.
    const double q_obs = nu * nu - (p_prev + r);
    q_[i] = (q_[i] * nq_[i] + q_obs) / (nq_[i] + 1);
    nu2_[i] = (nu2_[i] * nq_[i] + nu * nu) / (nq_[i] + 1);
    ++nq_[i];
    c.jitter_rad[i] = std::sqrt(nu2_[i]);                  // running innovation RMS
  }
  init_ = true;
  for (uint32_t i = 0; i < kCh; ++i) {
    c.phase_rad[i] = std::arg(s_[i]); c.phase_var[i] = p_[i];
    // One-step PREDICTIVE variance (spec Sec.2): a channel whose phase is unpredictable between CPIs
    // fades out automatically, same as a channel whose LOS tap is corrupted this CPI.
    const double q_use_i = std::max(0.0, q_[i]);
    c.coh_factor[i] = std::exp(-0.5 * std::min(p_[i] + q_use_i, 50.0));
  }
  last_ = c;
  return c;
}

} // namespace nr_isac::coherent
