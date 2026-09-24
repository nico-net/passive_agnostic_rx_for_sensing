/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_core.h"
#include "fft.h"
#include "robust_stats.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <deque>
#include <map>
#include <tuple>
#include <stdexcept>

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
  // |kernel|^2 depends only on the row's mask shape (a shift of the span is a phase ramp): one FFT per
  // distinct shape, weighted by its row count.
  std::vector<std::vector<uint8_t>> shapes; std::vector<uint32_t> first, count;
  for (uint32_t r = 0; r < w.rows; ++r) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
    std::vector<uint8_t> sh(w.observed.begin() + w.cell(r, lo), w.observed.begin() + w.cell(r, hi) + 1);
    size_t q = 0; while (q < shapes.size() && shapes[q] != sh) ++q;
    if (q == shapes.size()) { shapes.push_back(std::move(sh)); first.push_back(r); count.push_back(0); }
    ++count[q];
    const uint32_t c = row_comb(w, r); if (c && (!dk_min || c < dk_min)) dk_min = c;
    ++R;
  }
  for (size_t q = 0; q < shapes.size(); ++q) {
    const std::vector<cd> k = row_profile(w, af, 0, first[q], 0.0, 0.0, nullptr, true);
    for (size_t n = 0; n < kf.size(); ++n) kf[n] += count[q] * std::norm(k[n]);
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
  // Set when this channel's non-coherent stage could not separate an earlier path from `strongest`
  // (best == strongest below): the coherent-domain scan after `estimate_row_sync` below is the only
  // place that region gets a second look, at full-union-band resolution.
  std::array<bool, kCh> scan_early{};

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
    // The leak-budget test above can only accept an earlier path that is ALREADY its own local
    // maximum of the raw (non-coherent) power sum -- which a path closer than the mainlobe
    // half-width to a stronger, later one never is: their non-coherent sum merges into one bump
    // with no dip in between (measured: a wall +6 dB over the LOS and ~4 bins later never appears
    // in `peaks` at all, so no leakage bound could have saved it -- the spec's own case, "a wall
    // reflection ... must not bias calibration"). A candidate BEYOND the mainlobe half-width would
    // already have shown up as its own local maximum and been handled above; the algorithm is
    // structurally blind only to a second path WITHIN an accepted peak's own mainlobe (|d| <= hm).
    // That region gets resolved in the COHERENT domain instead, after `estimate_row_sync` below
    // (full-union-band resolution, not a per-row-bandwidth-limited one) -- see the scan there.
    scan_early[i] = (best == strongest);
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
  // Union-band kernel: the coherent estimator's own response to a unit-amplitude, zero-delay-error
  // static path -- built exactly like a channel's `U` below but with the data replaced by 1. Row
  // masks/weights and `s` are shared by every antenna, so this is channel-independent and built
  // once. Its peak sits at x=0 up to `s`'s own (small) per-row residual, and its PEAK VALUE is
  // generally < 1 -- a row's own per-subcarrier phase ramp at s.delay_s[r] does not telescope back
  // to the row's window sum the way the unit magnitude kernel above (kf, which fixes each row's
  // ramp at its OWN bin) does; this one genuinely reflects whatever coherence loss `s`'s row-to-row
  // residual leaves behind (real, wanted here: the leakage bound below must not assume better
  // alignment than the estimator actually achieves).
  std::vector<cd> Uk(w.subcarriers, cd(0));
  for (uint32_t r = 0; r < w.rows; ++r) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
    double ws = 0; for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)]) ws += hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5);
    for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)])
      Uk[k] += std::polar(hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5) / (ws * R),
                          2 * M_PI * baseband_hz(w, k) * s.delay_s[r] - s.phase_rad[r]);
  }
  auto kernel_at = [&](double x) {
    const cd rot = std::polar(1.0, 2 * M_PI * w.scs_hz * x * a.delay_step_s);
    cd ph = std::polar(1.0, 2 * M_PI * baseband_hz(w, 0) * x * a.delay_step_s), acc = 0;
    for (uint32_t k = 0; k < w.subcarriers; ++k, ph *= rot) if (Uk[k] != cd(0)) acc += Uk[k] * ph;
    return std::abs(acc);
  };
  // Locate the kernel's own peak near x=0 (golden section, mirroring the per-channel refinement
  // below) and normalise leakage to it -- NOT to an assumed kernel_at(0)=1, which the per-row phase
  // ramps above do not actually guarantee.
  double k0 = 0.0;
  { long k0i = 0; for (long n = -2; n <= 2; ++n) if (kernel_at(n) > kernel_at(k0i)) k0i = n;
    const double gr = 0.5 * (std::sqrt(5.0) - 1);
    double xa = k0i - 1.0, xb = k0i + 1.0, xc = xb - gr * (xb - xa), xd = xa + gr * (xb - xa);
    double fc0 = kernel_at(xc), fd0 = kernel_at(xd);
    while (xb - xa > 1e-6) {
      if (fc0 > fd0) { xb = xd; xd = xc; fd0 = fc0; xc = xb - gr * (xb - xa); fc0 = kernel_at(xc); }
      else { xa = xc; xc = xd; fc0 = fd0; xd = xa + gr * (xb - xa); fd0 = kernel_at(xd); }
    }
    k0 = 0.5 * (xa + xb);
  }
  const double kpeak = kernel_at(k0);
  // Leakage of a unit peak at integer offset d, oversampled +-1 bin at the kernel's own kOvs (a real
  // earlier path's sub-bin position is unknown ahead of time; same convention as `leak` above),
  // normalised to the kernel's OWN measured peak.
  auto leak_c = [&](long d) { double m = 0; for (long j = -kOvs; j <= kOvs; ++j) m = std::max(m, kernel_at(k0 + d + (double)j / kOvs)); return (kpeak > 0) ? m / kpeak : 1.0; };
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
    // Locate the coherent domain's own peak near n0 first (as before this fix): the non-coherent
    // estimate can be up to ~2 bins off the coherent peak (measured, see the original comment
    // below), and the earlier-path scan needs the TRUE peak position/amplitude as its "strong path"
    // reference, not the possibly-off n0 -- using n0 directly let the strong peak's own mainlobe
    // skirt at n0's neighbour read as if it exceeded its own leakage bound and falsely fired the
    // scan below on every ordinary (no-earlier-path) CPI.
    long peak = n0;
    for (long n = n0 - 2; n <= n0 + 2; ++n) if (at(n) > at(peak)) peak = n;
    long best = peak;
    if (scan_early[i]) {
      // The non-coherent stage could not separate an earlier path merged into this channel's strong
      // peak's own mainlobe (scan_early above). At full-union-band resolution the coherent profile
      // CAN: scan [peak-hm, peak-1] -- the region the non-coherent pass never resolved -- for the
      // EARLIEST local maximum that clears both this profile's own noise floor and the strong
      // peak's own leakage into it. Noise: under noise alone coh[n] is ONE complex Gaussian (R
      // independent per-row draws averaged, not R power draws summed like pw), so |coh[n]|^2 is
      // Exponential (Gamma shape 1) -- unlike the non-coherent stage's Gamma(R,.). Bonferroni over
      // the hm bins this scan actually tries -- no new constant.
      std::vector<double> cp(a.n_fft); for (size_t n = 0; n < cp.size(); ++n) cp[n] = std::norm(coh[n]);
      const double noise_c = median(cp) / gamma_upper_quantile(1, 0.5);
      const double thr_c = noise_c * gamma_upper_quantile(1, pfa / std::max<long>(1, hm));
      for (long d = -hm; d < 0; ++d) {
        const long n = peak + d;
        if (at(n) <= thr_c) continue;
        if (!(at(n) >= at(n - 1) && at(n) >= at(n + 1))) continue;
        if (at(n) <= leak_c(d) * at(peak)) continue;  // must clear the strong peak's own leakage
        best = n; break;                               // earliest first: d runs -hm -> -1
      }
    }
    // Sub-bin: maximise |coh(x)| over x continuous (golden section on [best-1, best+1]), coh(x) the
    // same coherent row mean evaluated off the FFT grid. A parabola through three magnitude samples
    // of a Hann mainlobe is biased by up to ~0.03 bin (measured), and a LOS reference off by that
    // moves every excess delay by it: 0.35 m of position along this geometry's weak axis.
    std::vector<cd> U(w.subcarriers, cd(0));
    for (uint32_t r = 0; r < w.rows; ++r) {
      uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
      double ws = 0; for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)]) ws += hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5);
      for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)])
        U[k] += cd(w.values[w.sample(i, r, k)]) * std::polar(hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5) / (ws * R),
                                                             2 * M_PI * baseband_hz(w, k) * s.delay_s[r] - s.phase_rad[r]);
    }
    auto coh_at = [&](double x) {
      const cd rot = std::polar(1.0, 2 * M_PI * w.scs_hz * x * a.delay_step_s);
      cd ph = std::polar(1.0, 2 * M_PI * baseband_hz(w, 0) * x * a.delay_step_s), acc = 0;
      for (uint32_t k = 0; k < w.subcarriers; ++k, ph *= rot) if (U[k] != cd(0)) acc += U[k] * ph;
      return acc;
    };
    const double gr = 0.5 * (std::sqrt(5.0) - 1);
    double xa = best - 1.0, xb = best + 1.0, xc = xb - gr * (xb - xa), xd = xa + gr * (xb - xa);
    double fc_ = std::abs(coh_at(xc)), fd_ = std::abs(coh_at(xd));
    while (xb - xa > 1e-6) {                               // numerical resolution, far below any CRB here
      if (fc_ > fd_) { xb = xd; xd = xc; fd_ = fc_; xc = xb - gr * (xb - xa); fc_ = std::abs(coh_at(xc)); }
      else { xa = xc; xc = xd; fc_ = fd_; xd = xa + gr * (xb - xa); fd_ = std::abs(coh_at(xd)); }
    }
    const double xm = 0.5 * (xa + xb);
    L.delay_s[i] = fold(xm) * a.delay_step_s;
    L.tap[i] = coh_at(xm);
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
  auto fit = [&](const std::vector<double>& t, const std::vector<double>& y, std::vector<double>& out) {
    double mt = 0, my = 0; for (size_t n = 0; n < t.size(); ++n) { mt += t[n]; my += y[n]; }
    mt /= t.size(); my /= t.size();
    double stt = 0, sty = 0; for (size_t n = 0; n < t.size(); ++n) { stt += (t[n] - mt) * (t[n] - mt); sty += (t[n] - mt) * (y[n] - my); }
    const double b = stt > 0 ? sty / stt : 0.0;
    for (uint32_t r = 0; r < w.rows; ++r) out[r] = my + b * (a.row_t_s[r] - mt);
  };
  std::vector<double> td, dl;
  for (uint32_t r = 0; r < w.rows; ++r)
    if (std::abs(slope[r]) > 0) { td.push_back(a.row_t_s[r]); dl.push_back(-std::arg(slope[r]) / (2 * M_PI * comb[r] * w.scs_hz)); }
  // Delay drift FIRST: the per-row LOS taps below are read at the drift-corrected delay (at 1 ppm SFO
  // a 60-row CPI drifts 7 bins, and taps read at one fixed delay decohere: coherent LOS 0.2 of its
  // amplitude and the LOS reference 0.7 bin off -- measured). Delay: DRIFT ONLY. The fit's intercept is the power-weighted centroid of all paths, not the
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
          uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
          const double tau = L.delay_s[i] + (drift ? s.delay_s[r] : 0.0);
          const cd rot = std::polar(1.0, 2 * M_PI * w.scs_hz * tau);      // phasor recurrence over k
          cd ph = std::polar(1.0, 2 * M_PI * baseband_hz(w, lo) * tau), acc = 0;
          for (uint32_t k = lo; k <= hi; ++k, ph *= rot) if (w.observed[w.cell(r, k)]) acc += cd(w.values[w.sample(i, r, k)]) * ph;
          e += std::norm(acc);
        }
      return e;
    };
    if (los_pow(false) >= los_pow(true)) std::fill(s.delay_s.begin(), s.delay_s.end(), 0.0);
  }
  if (std::any_of(s.delay_s.begin(), s.delay_s.end(), [](double v) { return v != 0; }))
    for (uint32_t r = 0; r < w.rows; ++r)
      for (uint32_t i = 0; i < kCh; ++i) {
        if (!L.found[i]) continue;
        cd acc = 0; double n = 0;
        for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) {
          acc += cd(w.values[w.sample(i, r, k)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, k) * (L.delay_s[i] + s.delay_s[r])); n += 1;
        }
        tap[r][i] = n > 0 ? acc / n : cd(0);
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
  std::vector<double> tp, ph; double prev_ph = 0;
  for (uint32_t r = 0; r < w.rows; ++r) {
    cd c = 0; for (uint32_t i = 0; i < kCh; ++i) if (L.found[i]) c += tap[r][i] * align[i];  // weight = LOS amplitude
    if (std::abs(c) > 0) {                                                                  // phase never needs a slope
      double p = std::arg(c);
      if (!ph.empty()) p = prev_ph + std::remainder(p - prev_ph, 2 * M_PI);                  // unwrap in row order
      prev_ph = p; tp.push_back(a.row_t_s[r]); ph.push_back(p);
    }
  }
  if (tp.empty()) return s;
  fit(tp, ph, s.phase_rad);
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
  std::vector<cd> ed((size_t)a.n_dopp * w.rows);     // win_r e^{-j2pi f_d t_r} / wsum, [d][r]
  for (uint32_t d = 0; d < a.n_dopp; ++d)
    for (uint32_t r = 0; r < w.rows; ++r)
      ed[(size_t)d * w.rows + r] = win[r] / wsum * std::polar(1.0, -2 * M_PI * (a.dopp0_hz + d * a.dopp_step_hz) * a.row_t_s[r]);
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
        const cd* e = &ed[(size_t)d * w.rows]; cd acc = 0;
        for (uint32_t r = 0; r < w.rows; ++r) acc += prof[r][m] * e[r];
        out.rd.v[out.rd.idx(i, m, d)] = cf(acc);
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
          const cd* e = &ed[(size_t)d * w.rows]; cd acc = 0;
          for (uint32_t r = 0; r < w.rows; ++r) acc += far[r][m] * e[r];
          pw.push_back(std::norm(acc));
        }
    } else {
      for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d : a.tested_dopp) pw.push_back(std::norm(out.rd.v[out.rd.idx(i, m, d)]));
    }
    out.noise[i] = pw.empty() ? 1.0 : std::max(median(pw) / std::log(2.0), std::numeric_limits<double>::min());
  }
  // Waveform model (RdResult::Waveform): per-row slow-time weight and span centre, kernels per mask
  // shape, and the static-removal operator. A hopping allocation spreads a path over its row
  // mainlobe at every Doppler bin (per-row phase 2*pi*fc_r*x); a regular hop stride puts a coherent
  // serrodyne ghost there; TDD gaps put Doppler replicas. The exact response of a path is all of it.
  RdResult::Waveform& m = out.wf;
  const long O = RdResult::Waveform::kOvs;
  m.w = win; m.wsum = wsum; m.sc = w.subcarriers; m.X = (long)a.n_range + 4;
  m.fc.assign(w.rows, 0.0); m.hh.assign(w.rows, 0.0); m.grp.assign(w.rows, -1);
  m.mask.assign(w.observed.begin(), w.observed.end());
  std::vector<std::vector<uint8_t>> shapes;
  std::vector<double> H(w.rows, 0.0);
  const long nx = 2 * m.X * O + 1;
  for (uint32_t r = 0; r < w.rows; ++r) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
    std::vector<uint8_t> sh(w.observed.begin() + w.cell(r, lo), w.observed.begin() + w.cell(r, hi) + 1);
    const double kc = 0.5 * (lo + hi);
    m.fc[r] = ((kc - w.subcarriers / 2.0)) * w.scs_hz;
    double h1 = 0, h2 = 0;
    for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)]) {
      const double h = hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5); h1 += h; h2 += h * h;
    }
    H[r] = h1; m.hh[r] = h1 > 0 ? h2 / (h1 * h1) : 0.0;
    int32_t g = -1;
    for (size_t q = 0; q < shapes.size(); ++q) if (shapes[q] == sh) { g = (int32_t)q; break; }
    if (g < 0) {
      g = (int32_t)shapes.size(); shapes.push_back(sh);
      std::vector<cd> B(nx), B2(nx);
      for (long u = 0; u < nx; ++u) {
        const double x = -m.X + (double)u / O;
        const cd rot = std::polar(1.0, 2 * M_PI * x / a.n_fft);
        cd ph = std::polar(1.0, -2 * M_PI * x * (kc - lo) / a.n_fft), b1 = 0, b2 = 0;   // phasor recurrence over k
        for (uint32_t k = lo; k <= hi; ++k, ph *= rot) if (w.observed[w.cell(r, k)]) {
          const double h = hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5); b1 += h * ph; b2 += h * h * ph;
        }
        B[u] = b1 / h1; B2[u] = b2 / h2;
      }
      m.B.push_back(std::move(B)); m.B2.push_back(std::move(B2));
    }
    m.grp[r] = g;
  }
  m.w2sum = 0; for (uint32_t r = 0; r < w.rows; ++r) if (m.grp[r] >= 0) m.w2sum += win[r] * win[r] * m.hh[r];
  // Static-removal operator: Q_k(d) = sum_{r observing k} w_r e^{-j2pi f_d t_r} h_rk / (H_r wsum).
  // Removing the per-subcarrier slow-time mean subtracts, from a path (A, x0, f), the RD term
  // -A sum_k e^{j2pi f_k (x - x0) delay_step} M_k(f) Q_k(d), M_k(f) = mean_{r observing k} e^{j2pi f t_r}.
  m.Q.assign((size_t)w.subcarriers * a.n_dopp, cf(0));
  m.lo.assign(w.rows, 1); m.hi.assign(w.rows, 0);
  for (uint32_t r = 0; r < w.rows; ++r) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) continue;
    m.lo[r] = lo; m.hi[r] = hi;
    for (uint32_t d = 0; d < a.n_dopp; ++d) {
      const cd e = ed[(size_t)d * w.rows + r] / H[r];
      cf* q = &m.Q[(size_t)d * w.subcarriers];
      for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)]) q[k] += cf(e * hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5));
    }
  }
  return out;
}

namespace {
cd kernel_at(const std::vector<cd>& B, long X, double x)
{
  const double u = (x + X) * RdResult::Waveform::kOvs;
  if (!(u >= 0) || u >= (double)(B.size() - 1)) return 0.0;      // beyond +-X bins: not tabulated
  const size_t u0 = (size_t)u; const double t = u - u0;
  return B[u0] * (1 - t) + B[u0 + 1] * t;
}
} // namespace

cd RdResult::ambiguity_c(double x, double dd) const
{
  const Axes& a = rd.axes; cd acc = 0;
  if (!(wf.wsum > 0)) return acc;
  for (size_t r = 0; r < wf.grp.size(); ++r) if (wf.grp[r] >= 0)
    acc += wf.w[r] * std::polar(1.0, 2 * M_PI * (wf.fc[r] * x * a.delay_step_s - dd * a.dopp_step_hz * a.row_t_s[r]))
           * kernel_at(wf.B[wf.grp[r]], wf.X, x);
  return acc / wf.wsum;
}

cd RdResult::noise_corr(double x, double dd) const
{
  const Axes& a = rd.axes; cd acc = 0;
  if (!(wf.w2sum > 0)) return acc;
  for (size_t r = 0; r < wf.grp.size(); ++r) if (wf.grp[r] >= 0)
    acc += wf.w[r] * wf.w[r] * wf.hh[r] * std::polar(1.0, 2 * M_PI * (wf.fc[r] * x * a.delay_step_s - dd * a.dopp_step_hz * a.row_t_s[r]))
           * kernel_at(wf.B2[wf.grp[r]], wf.X, x);
  return acc / wf.w2sum;
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

namespace {
double cubic4(double pm, double p0, double p1, double p2, double t)   // 4-point Lagrange on [-1, 2]
{
  return -pm * t * (t - 1) * (t - 2) / 6 + p0 * (t + 1) * (t - 1) * (t - 2) / 2
         - p1 * (t + 1) * t * (t - 2) / 2 + p2 * (t + 1) * t * (t - 1) / 6;
}
double qfunc(double x) { return 0.5 * std::erfc(x / std::sqrt(2.0)); }
// Sliding maximum of col over [d-h, d+h] (clipped to the axis) for every d: monotonic deque, O(n).
void sliding_max(const std::vector<double>& col, uint32_t h, std::vector<double>& out)
{
  const long n = (long)col.size(); out.assign(n, 0.0);
  std::deque<long> q; long next = 0;
  for (long d = 0; d < n; ++d) {
    for (const long hi = std::min(n - 1, d + (long)h); next <= hi; ++next) {
      while (!q.empty() && col[q.back()] <= col[next]) q.pop_back();
      q.push_back(next);
    }
    while (q.front() < d - (long)h) q.pop_front();
    out[d] = col[q.front()];
  }
}
std::vector<double> v3(const Vec3& v) { return {v.x, v.y, v.z}; }
// Bistatic path-length gradient of channel i at x: u(tx->x) + u(rx_i->x).
Vec3 grad(const Geometry& geo, uint32_t i, const Vec3& x) { return unit(x - geo.tx) + unit(x - geo.rx[i]); }
// Envelope position covariance (G^T W G)^-1, W_ii = 2 snr_i (B_eff/c)^2: each channel measures its
// bistatic path to c/(B_eff sqrt(2 snr_i)). False when fewer than 3 channels (not 3-D observable).
bool env_cov(const Axes& a, const Geometry& geo, const std::array<bool, kCh>& used, const Vec3& x,
             const std::array<double, kCh>& snr, Matrix* cov)
{
  Matrix F(3, 3); uint32_t n = 0;
  for (uint32_t i = 0; i < kCh; ++i) if (used[i]) {
    const std::vector<double> gi = v3(grad(geo, i, x));
    const double wi = 2 * std::max(snr[i], 1e-6) * std::pow(a.b_eff_hz / kC, 2);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) F(r, c) += wi * gi[r] * gi[c];
    ++n;
  }
  if (n < 3) return false;
  *cov = inverse(F);
  return true;
}
Matrix iso_cov(double sd) { return Matrix::identity(3) * (sd * sd); }
void set_cov(Detection& d, const Matrix& c)
{
  for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) d.pos_cov[r * 3 + k] = 0.5 * (c(r, k) + c(k, r));
  d.pos_sigma = Vec3{std::sqrt(d.pos_cov[0]), std::sqrt(d.pos_cov[4]), std::sqrt(d.pos_cov[8])};
}
// Magnitude of channel ch's RD at a fractional range bin, same cubic as sample_rd (no phase): the
// envelope needs only |RD|^2, and skipping arg()/polar() is most of its cost.
struct MagInterp {
  uint32_t b[4]; double c[4]; bool ok = false;
  MagInterp(uint32_t n, double bin)
  {
    if (n == 0 || !(bin >= 0) || bin > (double)(n - 1)) return;
    const long b0 = (long)bin; const double t = bin - b0;
    for (int j = 0; j < 4; ++j) b[j] = (uint32_t)std::clamp(b0 - 1 + j, 0L, (long)n - 1);
    c[0] = -t * (t - 1) * (t - 2) / 6; c[1] = (t + 1) * (t - 1) * (t - 2) / 2;
    c[2] = -(t + 1) * t * (t - 2) / 2; c[3] = (t + 1) * t * (t - 1) / 6;
    ok = true;
  }
  double at(const RdResult& R, uint32_t ch, uint32_t d) const
  {
    double m = 0; for (int j = 0; j < 4; ++j) m += c[j] * std::abs(R.rd.v[R.rd.idx(ch, b[j], d)]);
    return std::max(0.0, m);
  }
};

// Exact RD response of a path, on the integer RD grid (see RdResult::Waveform).
struct Response {
  const RdResult& R; const Axes& a;
  std::vector<uint32_t> rows;
  std::vector<cd> ed, em;                                // [d][r] w_r/wsum e^{-j2pi f_d t_r}, [m][r] e^{j2pi fc_r m delay}
  explicit Response(const RdResult& R_) : R(R_), a(R_.rd.axes)
  {
    const auto& wf = R.wf;
    for (uint32_t r = 0; r < wf.grp.size(); ++r) if (wf.grp[r] >= 0) rows.push_back(r);
    ed.resize((size_t)a.n_dopp * rows.size()); em.resize((size_t)a.n_range * rows.size());
    for (uint32_t d = 0; d < a.n_dopp; ++d) for (size_t j = 0; j < rows.size(); ++j)
      ed[d * rows.size() + j] = wf.w[rows[j]] / wf.wsum * std::polar(1.0, -2 * M_PI * (a.dopp0_hz + d * a.dopp_step_hz) * a.row_t_s[rows[j]]);
    for (uint32_t m = 0; m < a.n_range; ++m) for (size_t j = 0; j < rows.size(); ++j)
      em[m * rows.size() + j] = std::polar(1.0, 2 * M_PI * wf.fc[rows[j]] * m * a.delay_step_s);
    double ws = 0; for (uint32_t r : rows) { tbar += wf.w[r] * a.row_t_s[r]; ws += wf.w[r]; }
    if (ws > 0) tbar /= ws;
  }
  // A path's per-row phasors and delays. The path migrates with its own Doppler (tau-dot = -f/fc, no
  // extra parameter): its delay in row r is p_r = p - (f/fc)(t_r - tbar)/delay_step, p referring to the
  // CPI's weighted mid-time tbar. Ignoring it put an 8 m/s target's fitted position 2.2 cm off (the hop
  // sequence couples the migration's per-row phase 2pi fc_r dp_r into the delay estimate: measured).
  struct Ph { std::vector<cd> pp; std::vector<double> pr; };
  double tbar = 0;
  Ph phasors(double p, double fb) const
  {
    Ph h; h.pp.resize(rows.size()); h.pr.resize(rows.size());
    const double f = a.dopp0_hz + fb * a.dopp_step_hz;
    for (size_t j = 0; j < rows.size(); ++j) {
      h.pr[j] = p - (f / a.fc_hz) * (a.row_t_s[rows[j]] - tbar) / a.delay_step_s;
      h.pp[j] = std::polar(1.0, 2 * M_PI * (f * a.row_t_s[rows[j]] - R.wf.fc[rows[j]] * h.pr[j] * a.delay_step_s));
    }
    return h;
  }
  cd at(uint32_t m, uint32_t d, const Ph& h) const
  {
    const auto& wf = R.wf;
    const cd* e = &ed[(size_t)d * rows.size()]; const cd* f = &em[(size_t)m * rows.size()];
    cd acc = 0;
    for (size_t j = 0; j < rows.size(); ++j) acc += e[j] * f[j] * h.pp[j] * kernel_at(wf.B[wf.grp[rows[j]]], wf.X, m - h.pr[j]);
    return acc;
  }
};

// One channel of an accepted detection: the path fitted to its RD neighbourhood, and the fit's own
// error (sandwich covariance of the least-squares fit under the RD noise correlation).
struct ChanFit {
  double p = 0, fb = 0, sp = 0, sf = 0, sA = 0, snr = 0; cd A = 0; bool ok = false;
  std::vector<Response::Ph> grid;                                     // tolerance grid (p, fb) phasors; [0] nominal
  std::vector<cd> M;                                                  // M_k(f) for the static term
  std::vector<std::vector<cd>> stat;                                  // [d] -> static term per range bin (lazy)
};

const std::vector<cd>& static_term(const RdResult& R, ChanFit& f, uint32_t d);
// Static-removal term of a fitted unit path at one cell: -sum_k e^{j2pi f_k (m - p) delay} M_k Q_k(d).
cd static_cell(const RdResult& R, const ChanFit& f, uint32_t m, uint32_t d)
{
  const Axes& a = R.rd.axes; const auto& wf = R.wf;
  const double x = m - f.p;
  const cd rot = std::polar(1.0, 2 * M_PI * a.scs_hz * x * a.delay_step_s);
  cd ph = std::polar(1.0, 2 * M_PI * (-(wf.sc / 2.0)) * a.scs_hz * x * a.delay_step_s), sv = 0;
  const cf* Qd = &wf.Q[(size_t)d * wf.sc];
  for (uint32_t k = 0; k < wf.sc; ++k, ph *= rot) if (Qd[k] != cf(0)) sv += ph * f.M[k] * cd(Qd[k]);
  return -sv;
}
// A fitted path's full RD value at cell (m, d): direct response plus its static-removal term.
cd path_value(const Response& rs, const ChanFit& f, uint32_t m, uint32_t d)
{
  if (!f.ok) return 0.0;
  return f.A * (rs.at(m, d, f.grid[0]) + static_cell(rs.R, f, m, d));
}
// `others`: the other accepted paths on this channel, subtracted from the cells before fitting (the
// detections are refitted jointly, one at a time: two targets unresolved on a channel and 1-2
// Doppler bins apart otherwise bias each other's fit by 0.1 bin -- measured, t1 vs the drone).
ChanFit fit_channel(const Response& rs, uint32_t ch, double p0, double fb0, long hm_r, long hm_d, double z,
                    const std::vector<ChanFit*>& others)
{
  const RdResult& R = rs.R; const Axes& a = rs.a;
  ChanFit f;
  struct Cell { uint32_t m, d; cd y; };
  std::vector<Cell> cells;
  for (long m = std::lround(p0) - hm_r; m <= std::lround(p0) + hm_r; ++m)
    for (long d = std::lround(fb0) - hm_d; d <= std::lround(fb0) + hm_d; ++d)
      if (m >= 0 && m < (long)a.n_range && dopp_ok(a, d))
        cells.push_back({(uint32_t)m, (uint32_t)d, cd(R.rd.v[R.rd.idx(ch, (uint32_t)m, (uint32_t)d)])});
  if (cells.size() < 4) return f;
  auto model = [&](double p, double fb, std::vector<cd>* ac) {
    const Response::Ph h = rs.phasors(p, fb); ac->resize(cells.size());
    for (size_t c = 0; c < cells.size(); ++c) (*ac)[c] = rs.at(cells[c].m, cells[c].d, h);
  };
  for (Cell& c : cells) for (ChanFit* o : others) c.y -= path_value(rs, *o, c.m, c.d);
  std::vector<cd> y(cells.size()); for (size_t c = 0; c < cells.size(); ++c) y[c] = cells[c].y;
  auto J = [&](double p, double fb, cd* A) {
    std::vector<cd> ac; model(p, fb, &ac);
    cd num = 0; double den = 0;
    for (size_t c = 0; c < cells.size(); ++c) { num += y[c] * std::conj(ac[c]); den += std::norm(ac[c]); }
    if (A) *A = den > 0 ? num / den : cd(0);
    return den > 0 ? std::norm(num) / den : 0.0;
  };
  // Maximise J(p, fb): Newton steps on a central-difference gradient/Hessian (9 evaluations), halved
  // until J does not decrease, from a coarse 0.25-bin pattern search that brings it into the peak's
  // quadratic region. Stops at 1e-5 bin: numerical resolution, below any CRB this chain reaches.
  double p = p0, fb = fb0;
  auto search = [&](double st) {
    double best = J(p, fb, nullptr);
    for (; st >= 0.05;) {                                    // coarse: into the mainlobe
      double bp = p, bf = fb;
      for (int u = -1; u <= 1; ++u) for (int v = -1; v <= 1; ++v) if (u || v) {
        const double j = J(p + u * st, fb + v * st, nullptr); if (j > best) { best = j; bp = p + u * st; bf = fb + v * st; }
      }
      if (bp == p && bf == fb) st /= 2; else { p = bp; fb = bf; }
    }
    for (int it = 0; it < 20; ++it) {                        // Newton
      const double h = 1e-3, j0 = best;
      const double jpp = J(p + h, fb, nullptr), jpm = J(p - h, fb, nullptr), jfp = J(p, fb + h, nullptr), jfm = J(p, fb - h, nullptr);
      const double jpf = J(p + h, fb + h, nullptr), jmm = J(p - h, fb - h, nullptr);
      const double gp = (jpp - jpm) / (2 * h), gf = (jfp - jfm) / (2 * h);
      const double hpp = (jpp - 2 * j0 + jpm) / (h * h), hff = (jfp - 2 * j0 + jfm) / (h * h);
      const double hpf = (jpf + jmm - jpp - jpm - jfp - jfm + 2 * j0) / (2 * h * h);
      const double det = hpp * hff - hpf * hpf;
      if (!(hpp < 0 && det > 0)) break;                      // not in a concave region: keep the coarse point
      double dp = -(hff * gp - hpf * gf) / det, df = -(hpp * gf - hpf * gp) / det;
      double jn = J(p + dp, fb + df, nullptr);
      for (int k = 0; k < 20 && !(jn >= j0); ++k) { dp /= 2; df /= 2; jn = J(p + dp, fb + df, nullptr); }
      if (!(jn >= j0)) break;
      p += dp; fb += df; best = jn;
      if (std::hypot(dp, df) < 1e-5) break;
    }
  };
  // M_k(f) = mean over the rows observing k of e^{j2pi f t_r}: the path's own slow-time mean on
  // subcarrier k, which static removal subtracts (see RdResult::Waveform::Q).
  const auto& wf = R.wf;
  auto set_M = [&](double fbv) {
    const double fhz = a.dopp0_hz + fbv * a.dopp_step_hz;
    f.M.assign(wf.sc, cd(0)); std::vector<uint32_t> cnt(wf.sc, 0);
    for (uint32_t r = 0; r < wf.grp.size(); ++r) {
      const cd e = std::polar(1.0, 2 * M_PI * fhz * a.row_t_s[r]);
      const uint8_t* mk = &wf.mask[(size_t)r * wf.sc];
      for (uint32_t k = wf.lo[r]; k <= wf.hi[r]; ++k) if (mk[k]) { f.M[k] += e; ++cnt[k]; }
    }
    for (uint32_t k = 0; k < wf.sc; ++k) if (cnt[k]) f.M[k] /= (double)cnt[k];
  };
  // The static-removal term perturbs the path's own neighbourhood by a few % in amplitude (measured:
  // an isolated target fitted 0.011 bin late on every channel without it). It depends on (p, f) only
  // weakly, so: fit, subtract A * S(p, f) from the cells, refit (twice).
  search(0.25);
  set_M(fb);
  for (int pass = 0; pass < 2; ++pass) {
    cd A; J(p, fb, &A);
    for (size_t c = 0; c < cells.size(); ++c) {
      const cf* Qd = &wf.Q[(size_t)cells[c].d * wf.sc];
      const double x = cells[c].m - p;
      const cd rot = std::polar(1.0, 2 * M_PI * a.scs_hz * x * a.delay_step_s);
      cd ph = std::polar(1.0, 2 * M_PI * (-(wf.sc / 2.0)) * a.scs_hz * x * a.delay_step_s), sv = 0;
      for (uint32_t k = 0; k < wf.sc; ++k, ph *= rot) if (Qd[k] != cf(0)) sv += ph * f.M[k] * cd(Qd[k]);
      y[c] = cells[c].y + A * sv;                           // S = -sum(...): subtracting A*S adds A*sv
    }
    search(0.0);
  }
  f.p = p; f.fb = fb; J(p, fb, &f.A);
  // Sandwich covariance of theta = (p, fb, ReA, ImA): (D^T D)^-1 D^T C D (D^T D)^-1, D the real
  // Jacobian of the model on the cells, C the RD noise covariance (cells are correlated: zero-padded
  // range axis and Hann windows) -- the actual error of this (white least-squares) fit.
  const size_t N = cells.size(); const double h = 1e-3;
  std::vector<cd> a0, ap, am, fp, fm;
  model(p, fb, &a0); model(p + h, fb, &ap); model(p - h, fb, &am); model(p, fb + h, &fp); model(p, fb - h, &fm);
  std::vector<std::array<cd, 4>> Dc(N);
  for (size_t c = 0; c < N; ++c)
    Dc[c] = {f.A * (ap[c] - am[c]) / (2 * h), f.A * (fp[c] - fm[c]) / (2 * h), a0[c], cd(0, 1) * a0[c]};
  // Noise correlation by cell offset (few distinct offsets); for circular complex noise
  // Cov([Re; Im]) = (sigma^2/2) [[Re G, -Im G], [Im G, Re G]], so the real 4x4 blocks are
  // D^T D = sum_c Re(conj(D_c) D_c^T) and D^T C D = (sigma^2/2) sum_{c,e} Re(conj(D_c) G_ce D_e^T).
  const long hr = 2 * hm_r, hd = 2 * hm_d, nd = 2 * hd + 1;
  std::vector<cd> gam((size_t)(2 * hr + 1) * nd); std::vector<uint8_t> have(gam.size(), 0);
  Matrix DtD(4, 4), DCD(4, 4);
  for (size_t c = 0; c < N; ++c) {
    for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) DtD(u, v) += (std::conj(Dc[c][u]) * Dc[c][v]).real();
    for (size_t e = 0; e < N; ++e) {
      const long om = (long)cells[c].m - (long)cells[e].m, od = (long)cells[c].d - (long)cells[e].d;
      const size_t gi = (size_t)((om + hr) * nd + od + hd);
      if (!have[gi]) { gam[gi] = R.noise_corr((double)om, (double)od); have[gi] = 1; }
      for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) DCD(u, v) += (std::conj(Dc[c][u]) * gam[gi] * Dc[e][v]).real();
    }
  }
  for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) DCD(u, v) *= R.noise[ch] / 2;
  // Effective SNR for everything estimated FROM this fit (position, phase): the fit's own residual
  // over the cells, when it exceeds the noise floor, is model error (sub-bin interpolation, residual
  // sync, unmodelled weaker paths) and is as real as noise there. sigma^2 per cell = sum|r|^2/(N-2).
  // The leakage tolerance below stays noise-referenced: the residual is dominated by OTHER paths'
  // energy in the neighbourhood, which the pursuit models separately (inflating it by that removed
  // real targets -- measured, the drone).
  double rr = 0; for (size_t c = 0; c < N; ++c) rr += std::norm(y[c] - f.A * a0[c]);
  f.snr = std::norm(f.A) / std::max(R.noise[ch], N > 2 ? rr / (N - 2) : 0.0);
  const Matrix Ai = inverse(DtD), cov = Ai * DCD * Ai;
  f.sp = std::sqrt(std::max(0.0, cov(0, 0))); f.sf = std::sqrt(std::max(0.0, cov(1, 1)));
  f.sA = std::sqrt(std::max(0.0, cov(2, 2) + cov(3, 3)));
  // Tolerance grid: +-z sigma (z from the per-test false-alarm probability), collapsed when below
  // numerical resolution.
  std::vector<double> ps{p}, fs{fb};
  if (z * f.sp > 1e-6) { ps.push_back(p - z * f.sp); ps.push_back(p + z * f.sp); }
  if (z * f.sf > 1e-6) { fs.push_back(fb - z * f.sf); fs.push_back(fb + z * f.sf); }
  for (double pv : ps) for (double fv : fs) f.grid.push_back(rs.phasors(pv, fv));
  set_M(fb);
  f.stat.resize(a.n_dopp);
  f.ok = true;
  return f;
}
// Static-removal term of a fitted unit-amplitude path in Doppler bin d, per range bin:
// -sum_k e^{j2pi f_k (m - p) delay} M_k Q_k(d), one FFT over the subcarriers.
const std::vector<cd>& static_term(const RdResult& R, ChanFit& f, uint32_t d)
{
  std::vector<cd>& out = f.stat[d];
  if (!out.empty()) return out;
  const Axes& a = R.rd.axes; const auto& wf = R.wf; const long N = a.n_fft;
  std::vector<cd> buf(N, cd(0));
  for (uint32_t k = 0; k < wf.sc; ++k) {
    const cf q = wf.Q[(size_t)d * wf.sc + k];
    if (q == cf(0)) continue;
    const double fk = ((double)k - wf.sc / 2.0) * a.scs_hz;
    const long qi = (long)k - (long)(wf.sc / 2);
    buf[(size_t)(((qi % N) + N) % N)] += f.M[k] * cd(q) * std::polar(1.0, -2 * M_PI * fk * f.p * a.delay_step_s);
  }
  fft_inplace(buf, true);
  out.resize(a.n_range);
  for (uint32_t m = 0; m < a.n_range; ++m) out[m] = -buf[m] * (double)N;
  return out;
}
// Largest RD amplitude the fitted path can put into channel ch's cell (bin, d), read the way the
// detector reads it (cubic magnitude over the 4 bins around `bin`), over the fit's tolerance grid.
double leak_amp(const Response& rs, ChanFit& f, double bin, uint32_t d, double z)
{
  if (!f.ok) return 0.0;
  const uint32_t n = rs.a.n_range;
  if (n == 0 || !(bin >= 0) || bin > (double)(n - 1)) return 0.0;
  const long b0 = (long)bin; const double t = bin - b0;
  uint32_t b[4]; for (int j = 0; j < 4; ++j) b[j] = (uint32_t)std::clamp(b0 - 1 + j, 0L, (long)n - 1);
  const std::vector<cd>& st = static_term(rs.R, f, d);
  double best = 0;
  for (size_t q = 0; q < f.grid.size(); ++q) {
    double v[4];
    for (int j = 0; j < 4; ++j) v[j] = std::abs(rs.at(b[j], d, f.grid[q]) + st[b[j]]);
    best = std::max(best, cubic4(v[0], v[1], v[2], v[3], t));
  }
  return best * (std::abs(f.A) + z * f.sA);
}
} // namespace

std::vector<float> envelope(const RdResult& R, const Grid& g, const Geometry& geo)
{
  const Axes& a = R.rd.axes;
  const size_t nt = a.tested_dopp.size();
  std::vector<float> E(nt * g.size(), 0.f);
  std::vector<double> col(a.n_dopp), smax;
  std::vector<uint8_t> ok(a.n_dopp); for (uint32_t d = 0; d < a.n_dopp; ++d) ok[d] = dopp_ok(a, d);
  for (size_t v = 0; v < g.size(); ++v) {
    const Vec3 x = g.at(v);
    const uint32_t dh = dopp_half(a, geo, R.los_found, x);
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!R.los_found[i]) continue;
      const MagInterp mi(a.n_range, excess_delay_s(x, geo.tx, geo.rx[i]) / a.delay_step_s);
      if (!mi.ok) continue;
      for (uint32_t d = 0; d < a.n_dopp; ++d) { const double m = ok[d] ? mi.at(R, i, d) : 0.0; col[d] = m * m / R.noise[i]; }
      sliding_max(col, dh, smax);                         // each channel at its own Doppler within +-dh
      for (size_t t = 0; t < nt; ++t) E[t * g.size() + v] += (float)smax[a.tested_dopp[t]];
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
  if (E.size() != nt * nv) throw std::invalid_argument("detect: envelope size does not match the grid and Doppler axis");
  if (n == 0 || nv == 0 || !(p.pfa > 0)) return out;
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
  std::vector<std::pair<double, double>> q(a.n_dopp + 2, {-1.0, -1.0});   // m -> (median, threshold) of S(m, n)
  auto null_of = [&](uint32_t m) -> const std::pair<double, double>& {
    if (q[m].first < 0) q[m] = std::make_pair(max_exp_sum_quantile(m, n, 0.5), max_exp_sum_quantile(m, n, p.pfa));
    return q[m];
  };
  std::vector<double> scale(nt, 0.0);
  for (size_t t = 0; t < nt; ++t) {
    std::vector<double> zz(nv);
    for (size_t v = 0; v < nv; ++v) zz[v] = E[t * nv + v] / null_of(m_of(t, v)).first;
    scale[t] = median(std::move(zz));
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
  const Response rs(R);
  // RD mainlobe half-widths: first minimum of the waveform's own ambiguity along delay / Doppler,
  // located on a 1/8-bin grid (on the integer grid the Doppler response of these CPIs has no local
  // minimum before the axis end: the DFT step 1/T_cpi is not the Hann null spacing), rounded up.
  auto first_min = [&](bool dopp, long lim) {
    const double st = 1.0 / 8; double x = st, prev = R.ambiguity(dopp ? 0 : x, dopp ? x : 0);
    for (; x < lim; x += st) { const double v = R.ambiguity(dopp ? 0 : x + st, dopp ? x + st : 0); if (v > prev) break; prev = v; }
    return std::max(1L, (long)std::ceil(x - 1e-9));
  };
  const long hm_r = first_min(false, a.n_range), hm_d = first_min(true, a.n_dopp);
  // Tolerance multiplier of a fitted parent's parameters: the two-sided normal quantile at the
  // per-test false-alarm probability (a parent outside it is as rare as a noise false alarm).
  const double z = -normal_inverse_cdf(p.pfa / 2);
  const double nms_r = 2 * g.step, res = kC / (2 * a.b_eff_hz);
  struct Acc { Vec3 x; std::array<uint32_t, kCh> d{}; std::array<double, kCh> pw{}, bin{}; };
  struct Item { Acc me; size_t t = 0, v = 0; long d = 0; double thr = 0, ec = 0; std::array<double, kCh> leak{}; bool alive = true; };
  std::vector<Item> items;
  const Vec3 hi_box = g.origin + Vec3{(g.nx - 1) * g.step, (g.ny - 1) * g.step, (g.nz - 1) * g.step};
  struct Accepted { Acc s; size_t t = 0, v = 0; long d = 0; double ec = 0; std::array<ChanFit, kCh> fit; };
  std::vector<Accepted> acc;
  auto leak_at = [&](uint32_t i, double bin, uint32_t d) {    // accepted detections' leakage, noise-amplitude units
    double lk = 0; for (Accepted& p : acc) lk += leak_amp(rs, p.fit[i], bin, d, z) / std::sqrt(R.noise[i]);
    return lk;
  };
  auto leak_of = [&](const Acc& me, uint32_t i) { return leak_at(i, me.bin[i], me.d[i]); };
  // The RD cube with every accepted detection's fitted path (direct response) subtracted: used only
  // to CHOOSE a candidate's per-channel bins; acceptance stays on the leakage bound.
  RdResult Rc; Rc.rd.axes = a; Rc.rd.v = R.rd.v;
  auto rebuild_residual = [&]() {
    Rc.rd.v = R.rd.v;
    for (Accepted& p : acc) for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i] && p.fit[i].ok)
      for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d = 0; d < a.n_dopp; ++d)
        Rc.rd.v[Rc.rd.idx(i, m, d)] -= cf(p.fit[i].A * rs.at(m, d, p.fit[i].grid[0]));
  };
  // Per-channel Doppler bins of a candidate at me.x around envelope bin d: the best combination of
  // per-channel local maxima that is consistent with one velocity; returns its summed score (-1 if
  // none exceeds thr). me.bin/me.d/me.pw (raw power) are set.
  auto choose = [&](Acc& me, long d, uint32_t dhv, double thr, bool resid) -> double {
    // Per channel: the local Doppler maxima inside the search window. Independent per-channel maxima
    // can mix different targets' energy on different channels (range-only multilateration ghosts;
    // measured 112 detections for 3 targets). A real target's channel Dopplers come from ONE
    // velocity: f_i = -g_i.v/lambda, g_i = u_tx + u_i. With 4 channels G (4x3) has a left null vector
    // nu, and sum nu_i f_i = 0; each f_i is its bin to +-1 bin, so the test is
    // |sum nu_i f_i| <= step * sum |nu_i|. Fewer channels: no redundancy.
    const long w0 = d - (long)dhv, w1 = d + (long)dhv;
    std::array<std::vector<std::pair<double, uint32_t>>, kCh> pk; std::array<std::vector<double>, kCh> raw;
    std::array<double, kCh> pmax{};
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      me.bin[i] = excess_delay_s(me.x, geo.tx, geo.rx[i]) / a.delay_step_s;
      const MagInterp mi(a.n_range, me.bin[i]);
      // Column score: the cell's power, or (resid) its power in the RD cube with the accepted
      // detections' fitted paths subtracted (else a candidate borrows an accepted target's peak on a
      // channel where the two are unresolved in range, and a real target whose own peak is only a
      // shoulder of it there is never re-scored: measured, the drone vs t1).
      std::vector<double> col(w1 - w0 + 1, -1.0), rw(w1 - w0 + 1, 0.0);
      if (mi.ok) for (long e = w0; e <= w1; ++e) if (dopp_ok(a, e)) {
        const double m = mi.at(R, i, (uint32_t)e); rw[e - w0] = m * m / R.noise[i];
        const double mr = resid ? mi.at(Rc, i, (uint32_t)e) : m;
        col[e - w0] = mr * mr / R.noise[i];
      }
      for (long e = w0; e <= w1; ++e) {
        const double v = col[e - w0];
        if (v >= 0 && (e == w0 || v >= col[e - 1 - w0]) && (e == w1 || v >= col[e + 1 - w0])) pk[i].push_back({v, (uint32_t)e});
      }
      std::sort(pk[i].begin(), pk[i].end(), [](const auto& l, const auto& r) { return l.first > r.first; });
      if (pk[i].empty()) pk[i].push_back({0.0, (uint32_t)std::clamp(d, 0L, (long)a.n_dopp - 1)});
      for (const auto& q : pk[i]) raw[i].push_back(q.second >= (uint32_t)std::max(0L, w0) && (long)q.second <= w1 ? rw[q.second - w0] : 0.0);
      pmax[i] = pk[i][0].first;
    }
    // Exact branch and bound: a peak can only belong to a combination above the threshold if it plus
    // the other channels' maxima exceeds it; the DFS then prunes on the best combination found so far.
    double sum_max = 0; for (uint32_t i = 0; i < kCh; ++i) sum_max += pmax[i];
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      const double others = sum_max - pmax[i];
      while (pk[i].size() > 1 && !(pk[i].back().first + others > thr)) { pk[i].pop_back(); raw[i].pop_back(); }
    }
    std::array<double, kCh> nu{};
    if (n == kCh) {
      Vec3 gv[kCh]; for (uint32_t i = 0; i < kCh; ++i) gv[i] = grad(geo, i, me.x);
      for (uint32_t i = 0; i < kCh; ++i) {             // cofactor expansion: nu_i = (-1)^i det(G without row i)
        Vec3 r3[3]; uint32_t qq = 0; for (uint32_t j = 0; j < kCh; ++j) if (j != i) r3[qq++] = gv[j];
        nu[i] = ((i & 1) ? -1.0 : 1.0) * dot(r3[0], cross(r3[1], r3[2]));
      }
    }
    double nu_abs = 0; for (double v : nu) nu_abs += std::abs(v);
    std::array<double, kCh + 1> rest{};                   // sum of remaining channels' maxima (bound)
    for (int i = kCh - 1; i >= 0; --i) rest[i] = rest[i + 1] + (R.los_found[i] ? pmax[i] : 0.0);
    double ec = -1; std::array<uint32_t, kCh> idx{}, pick{};
    struct Dfs {
      const std::array<std::vector<std::pair<double, uint32_t>>, kCh>& pk; const std::array<bool, kCh>& used;
      const std::array<double, kCh + 1>& rest; const std::array<double, kCh>& nu; double nu_abs, thr, f0, fstep;
      double& ec; std::array<uint32_t, kCh>& idx; std::array<uint32_t, kCh>& pick;
      void run(uint32_t i, double e, double cons)
      {
        if (i == kCh) {
          if (!(nu_abs > 0 && std::abs(cons) > fstep * nu_abs) && e > ec) { ec = e; pick = idx; }
          return;
        }
        if (!used[i]) { idx[i] = 0; run(i + 1, e, cons); return; }
        for (uint32_t j = 0; j < pk[i].size(); ++j) {
          if (!(e + pk[i][j].first + rest[i + 1] > std::max(ec, thr))) break;   // sorted: later ones are smaller
          idx[i] = j;
          run(i + 1, e + pk[i][j].first, cons + nu[i] * (f0 + pk[i][j].second * fstep));
        }
      }
    } dfs{pk, R.los_found, rest, nu, nu_abs, thr, a.dopp0_hz, a.dopp_step_hz, ec, idx, pick};
    dfs.run(0, 0.0, 0.0);
    if (!(ec > thr)) return -1.0;                       // E_c <= E: the max-of-m threshold stays conservative
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) { me.pw[i] = raw[i][pick[i]]; me.d[i] = pk[i][pick[i]].second; }
    return ec;
  };
  std::map<std::tuple<size_t, uint32_t, uint32_t, uint32_t, uint32_t>, size_t> seen;
  for (const Cand& k : c) {
    const Vec3 x = g.at(k.v);
    if (x.z < 0) continue;                                              // ground-bounce mirrors
    const long d = a.tested_dopp[k.t];
    Acc me; me.x = x;
    const double ec = choose(me, d, dh[k.v], k.thr, false);
    if (!(ec > k.thr)) continue;

    // One item per (voxel, per-channel bins): the wide per-channel windows make many Doppler bins of
    // one voxel pick the same combination; keep the one with the largest margin over its threshold.
    const auto key = std::make_tuple(k.v, me.d[0], me.d[1], me.d[2], me.d[3]);
    const auto f = seen.find(key);
    if (f != seen.end()) {
      Item& o = items[f->second];
      if (ec - k.thr > o.ec - o.thr) { o.t = k.t; o.d = a.tested_dopp[k.t]; o.thr = k.thr; o.ec = ec; }
      continue;
    }
    seen.emplace(key, items.size());
    Item it; it.me = me; it.t = k.t; it.v = k.v; it.d = a.tested_dopp[k.t]; it.thr = k.thr; it.ec = ec;
    items.push_back(it);
  }
  // Sub-voxel envelope position at the chosen per-channel Dopplers (pattern search: move while
  // better, halve when the centre wins). The voxel maximum of the Doppler-free E sat 4.5 m off an
  // isolated target along this geometry's weak axis (singular value 0.49), where E is flat to
  // 0.1 dB and other targets' energy on nearby delays tipped it. Run only for the candidate the
  // pursuit picks (thousands of candidates per CPI; the voxel is each one's sampled representation).
  auto walk = [&](Acc& me) {
    auto e_at = [&](const Vec3& y, std::array<double, kCh>* pw, std::array<double, kCh>* bn) {
      double e = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
        (*bn)[i] = excess_delay_s(y, geo.tx, geo.rx[i]) / a.delay_step_s;
        const MagInterp mi(a.n_range, (*bn)[i]); const double m = mi.ok ? mi.at(R, i, me.d[i]) : 0.0;
        (*pw)[i] = m * m / R.noise[i]; e += (*pw)[i];
      }
      return e;
    };
    // The walk stays inside the candidate's own RD mainlobe on every channel (else it climbs onto
    // another target's energy at a shared per-channel Doppler: measured a 14 m walk) and in the grid.
    const std::array<double, kCh> bin0 = me.bin;
    auto inside = [&](const Vec3& y) {
      if (y.x < g.origin.x || y.y < g.origin.y || y.z < std::max(0.0, g.origin.z) || y.x > hi_box.x || y.y > hi_box.y || y.z > hi_box.z) return false;
      for (uint32_t i = 0; i < kCh; ++i)
        if (R.los_found[i] && std::abs(excess_delay_s(y, geo.tx, geo.rx[i]) / a.delay_step_s - bin0[i]) > hm_r) return false;
      return true;
    };
    // Stop once the step is below the envelope position's CRB along its best axis, bounded below by
    // 1/sqrt(trace F) (F = G^T W G, see env_cov): resolving the maximum more finely than the noise
    // moves it buys nothing.
    double trF = 0;
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i])
      trF += 2 * std::max(me.pw[i] - 1, 1e-6) * std::pow(a.b_eff_hz / kC, 2) * norm2(grad(geo, i, me.x));
    const double smin = trF > 0 ? 1 / std::sqrt(trF) : g.step;
    std::array<double, kCh> pw, bn; double best = e_at(me.x, &me.pw, &me.bin);
    for (double st = g.step / 2; st * std::sqrt(3.0) >= smin && st > 1e-6;) {
      Vec3 bx = me.x;
      for (int iz = -1; iz <= 1; ++iz) for (int iy = -1; iy <= 1; ++iy) for (int ix = -1; ix <= 1; ++ix) {
        const Vec3 y = me.x + Vec3{ix * st, iy * st, iz * st};
        if (!inside(y)) continue;
        const double e = e_at(y, &pw, &bn);
        if (e > best) { best = e; bx = y; me.pw = pw; me.bin = bn; }
      }
      if (dist(bx, me.x) == 0) st /= 2; else me.x = bx;
    }
    return best;
  };
  // Greedy residual pursuit: repeatedly accept the candidate whose energy NOT explained by the
  // detections accepted so far is the largest (per-channel noise units). Strongest-raw-E
  // first let a ghost that borrowed a strong target's channels win before the weaker real target
  // it also borrowed from, which was then "explained" by the ghost (measured: drone lost 18/20).
  // Kept only if the unexplained energy exceeds the noise threshold. Per channel the observed
  // amplitude is at most noise + leakage (amplitudes), so max(0, |y_i| - |leak_i|) is at most the
  // noise amplitude and the sum of their squares at most the noise statistic: P(> thr) <= pfa, a
  // strict bound (reverse triangle inequality), given leakage predicted to within the fit tolerance.
  // Leakage = the accepted detection's fitted path (per channel) through the exact waveform response
  // (range sidelobes, random-hop pedestal, regular-hop serrodyne ghosts, TDD/grant-gap Doppler
  // replicas) plus its static-removal term.
  // Joint refit of every accepted detection, one path at a time with the others subtracted, until no
  // fitted delay or Doppler moves by more than its own standard error (at most 10 sweeps).
  auto refit_all = [&]() {
    for (int sweep = 0; sweep < 10; ++sweep) {
      bool moved = false;
      for (size_t j = 0; j < acc.size(); ++j)
        for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
          std::vector<ChanFit*> oth;
          for (size_t o = 0; o < acc.size(); ++o) if (o != j && acc[o].fit[i].ok) oth.push_back(&acc[o].fit[i]);
          ChanFit& f = acc[j].fit[i];
          ChanFit nf = fit_channel(rs, i, f.p, f.fb, hm_r, hm_d, z, oth);
          // Converged when nothing moves by more than its own actual uncertainty: the noise-only
          // sigma scaled by the fit's residual (thermal SNR / effective SNR).
          const double sc = nf.ok ? std::sqrt(std::max(1.0, std::norm(nf.A) / R.noise[i] / std::max(nf.snr, 1e-300))) : 1.0;
          if (nf.ok && (!f.ok || std::abs(nf.p - f.p) > sc * nf.sp || std::abs(nf.fb - f.fb) > sc * nf.sf)) moved = true;
          f = std::move(nf);
        }
      if (!moved) break;
    }
  };
  for (;;) {
    long pick = -1; double best = 0;
    for (size_t qi = 0; qi < items.size(); ++qi) {
      Item& it = items[qi]; if (!it.alive) continue;
      double resid = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) resid += std::pow(std::max(0.0, std::sqrt(it.me.pw[i]) - it.leak[i]), 2);
      if (resid <= it.thr) { it.alive = false; continue; }
      if (resid > best) { best = resid; pick = (long)qi; }
    }
    if (pick < 0) break;
    Item& k = items[pick]; k.alive = false;
    // Walk it to its sub-voxel maximum, then re-test it there against every accepted detection's
    // leakage at its new cells.
    k.ec = walk(k.me);
    {
      double resid = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) resid += std::pow(std::max(0.0, std::sqrt(k.me.pw[i]) - leak_of(k.me, i)), 2);
      if (resid <= k.thr) continue;
    }
    // Accept. Each channel's path starts from its own RD peak near the position-implied delay (the
    // position fits all channels jointly and can sit ~1 bin off any one of them along a weak axis).
    Accepted na; na.s = k.me; na.t = k.t; na.v = k.v; na.d = k.d; na.ec = k.ec;
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      double pb = k.me.bin[i], bv = -1; cd sv;
      for (double b = k.me.bin[i] - hm_r; b <= k.me.bin[i] + hm_r; b += 1.0 / 16)
        if (sample_rd(R, i, b, k.me.d[i], &sv) && std::norm(sv) > bv) { bv = std::norm(sv); pb = b; }
      std::vector<ChanFit*> oth; for (Accepted& p : acc) if (p.fit[i].ok) oth.push_back(&p.fit[i]);
      na.fit[i] = fit_channel(rs, i, pb, k.me.d[i], hm_r, hm_d, z, oth);
    }
    acc.push_back(std::move(na));
    refit_all();
    rebuild_residual();
    const Acc& s = acc.back().s;
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
      // Re-choose the per-channel bins on the residual cube, then its leakage bound there.
      if (!(choose(it.me, it.d, dh[it.v], it.thr, true) > it.thr)) { it.alive = false; continue; }
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) it.leak[i] = leak_of(it.me, i);   // refits moved every parent
    }
  }
  for (Accepted& k : acc) {
    const Acc& s = k.s; std::array<ChanFit, kCh>& fit = k.fit;
    // Envelope position: weighted least squares of the channels' FITTED delays (Gauss-Newton from the
    // walk's maximum), with the same weights as Cov_env. The E maximum is the envelope's own estimate
    // of this, but its magnitude interpolation reads per-channel peaks 0.1-0.5 bin off, and along
    // this geometry's weak axis that put t1 1.4 m (alone) to 4 m (among the other targets) off.
    std::array<double, kCh> wls{};
    // A channel whose fitted path left the candidate's own RD mainlobe locked onto another path.
    for (uint32_t i = 0; i < kCh; ++i)
      if (R.los_found[i] && fit[i].ok && std::abs(fit[i].p - s.bin[i]) <= hm_r) wls[i] = 2 * fit[i].snr * std::pow(a.b_eff_hz / kC, 2);
    Vec3 xe = s.x;
    {
      uint32_t nf = 0; for (double wv : wls) nf += wv > 0;
      for (int it = 0; it < 20 && nf >= 3; ++it) {
        Matrix F(3, 3); std::vector<double> gr(3, 0.0);
        for (uint32_t i = 0; i < kCh; ++i) if (wls[i] > 0) {
          const Vec3 gi = grad(geo, i, xe);
          const double r = fit[i].p * a.delay_step_s * kC - (dist(xe, geo.tx) + dist(xe, geo.rx[i]) - dist(geo.tx, geo.rx[i]));
          const double gv[3] = {gi.x, gi.y, gi.z};
          for (int u = 0; u < 3; ++u) { gr[u] += wls[i] * gv[u] * r; for (int v = 0; v < 3; ++v) F(u, v) += wls[i] * gv[u] * gv[v]; }
        }
        const std::vector<double> dx = inverse(F) * gr;
        xe = xe + Vec3{dx[0], dx[1], dx[2]};
        if (std::sqrt(dx[0] * dx[0] + dx[1] * dx[1] + dx[2] * dx[2]) < 1e-9) break;
      }
      // Consistency: with 4 channels the fit has one redundant delay; its weighted residual is chi^2_1
      // when every fit is right. Beyond the z^2 quantile (the same z as the leakage tolerance) a fit
      // is contaminated by an unresolved neighbour (measured: t1 and the drone 6-9 m off on 2/20
      // seeds): keep the envelope walk's position.
      double chi2 = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (wls[i] > 0) {
        const double r = fit[i].p * a.delay_step_s * kC - (dist(xe, geo.tx) + dist(xe, geo.rx[i]) - dist(geo.tx, geo.rx[i]));
        chi2 += wls[i] * r * r;
      }
      if (nf < 3 || (nf == 4 && chi2 > z * z) || !(xe.z >= 0) || !std::isfinite(xe.x + xe.y + xe.z)) { xe = s.x; wls = {}; }
    }
    Detection det; det.pos = det.pos_env = xe; det.dopp_bin = (uint32_t)k.d;
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      det.chan_dopp_bin[i] = (int32_t)s.d[i];
      if (fit[i].ok) { det.chan_amp[i] = fit[i].A; det.chan_fd_hz[i] = a.dopp0_hz + fit[i].fb * a.dopp_step_hz; det.chan_snr[i] = fit[i].snr; }
    }
    double fsum = 0; for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) fsum += a.dopp0_hz + s.d[i] * a.dopp_step_hz;
    det.doppler_hz = fsum / n;                                           // channel mean (centroid view)
    det.range_rate_mps = -a.lambda_m * det.doppler_hz;
    // Per-channel-equivalent SNR against the thermal floor (the envelope's per-Doppler scale also
    // carries every target's hop pedestal, and differs 2x between neighbouring bins of one plateau).
    double sn = 0; for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) sn += std::max(s.pw[i] - 1, 0.0);
    det.snr = std::max(1e-6, sn / n);
    det.tx = geo.tx;
    std::array<double, kCh> snr{};
    for (uint32_t i = 0; i < kCh; ++i) snr[i] = fit[i].ok ? fit[i].snr : std::max(s.pw[i] - 1, 1e-6);
    Matrix cov;
    if (!env_cov(a, geo, R.los_found, xe, snr, &cov))
      cov = iso_cov(std::max(res / std::sqrt(2 * det.snr), g.step / std::sqrt(12.0)));
    set_cov(det, cov);
    const double rr = a.lambda_m * a.dopp_step_hz;
    det.range_rate_sigma = std::max(rr / std::sqrt(2 * det.snr), rr / std::sqrt(12.0));
    out.push_back(det);
  }
  return out;
}

void refine(Detection& det, const RdResult& R, const Grid& g, const Geometry& geo, const Calibration& cal,
            const SurveySigma& survey)
{
  const Axes& a = R.rd.axes;
  const uint32_t n = n_used(R);
  // Each channel at its own Doppler bin: detect()'s velocity-consistent choice, else the strongest
  // within the per-channel search width.
  const uint32_t dh = dopp_half(a, geo, R.los_found, det.pos_env);
  std::array<uint32_t, kCh> db{}; std::array<double, kCh> snr{};
  for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
    const double bin = excess_delay_s(det.pos_env, geo.tx, geo.rx[i]) / a.delay_step_s;
    if (det.chan_dopp_bin[i] >= 0 && det.chan_dopp_bin[i] < (int32_t)a.n_dopp) db[i] = (uint32_t)det.chan_dopp_bin[i];
    else channel_peak(R, i, bin, det.dopp_bin, dh, &db[i]);
    cd s; snr[i] = det.chan_snr[i] > 0 ? det.chan_snr[i]
                 : (sample_rd(R, i, bin, db[i], &s) ? std::max(std::norm(s) / R.noise[i] - 1, 1e-6) : 1e-6);
  }
  const double res = kC / (2 * a.b_eff_hz);
  Matrix cov_env;
  if (!env_cov(a, geo, R.los_found, det.pos_env, snr, &cov_env))
    cov_env = iso_cov(std::max(res / std::sqrt(2 * det.snr), g.step / std::sqrt(12.0)));
  det.pos = det.pos_env; set_cov(det, cov_env); det.refined = true;
  // Coherent refinement needs 3 independent phase differences (4 channels) to resolve a 3-D fringe.
  uint32_t ref = kCh; for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) { ref = i; break; }
  if (n < 4 || !(g.step > 0)) return;
  Vec3 centroid{}; for (const Vec3& r : geo.rx) centroid = centroid + r * (1.0 / kCh);
  double D = 0;
  for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j) D = std::max(D, dist(geo.rx[i], geo.rx[j]));
  const double Rng = std::max(dist(det.pos_env, centroid), g.step);
  const double fringe = a.lambda_m * Rng / (2 * D), fs = fringe / 2;
  // Phases at the CPI's weighted mid-time: the Doppler DFT's time origin is row 0, so bin f_d carries
  // 2pi (f_i - f_d) tbar of the path's own phase progression; e^{+j2pi f_d tbar} leaves the phase at tbar.
  double tw = 0, ws = 0;
  for (size_t r = 0; r < R.wf.grp.size(); ++r) if (R.wf.grp[r] >= 0) { tw += R.wf.w[r] * a.row_t_s[r]; ws += R.wf.w[r]; }
  const double tbar = ws > 0 ? tw / ws : 0.0;
  // Each channel's path phase: detect()'s fitted complex amplitude (the exact RD peak value, phase at
  // row 0) advanced by its fitted Doppler to tbar; without a fit, the RD read at the candidate delay
  // and the channel's bin, advanced by the bin frequency (the DFT at bin f_d carries 2pi (f_i - f_d) tbar).
  // The RD phase read at a fractional bin is only linearly interpolated (measured ~1 mm of bias).
  std::array<cd, kCh> amp{};
  for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
    if (std::abs(det.chan_amp[i]) > 0) amp[i] = det.chan_amp[i] * std::polar(1.0, 2 * M_PI * det.chan_fd_hz[i] * tbar);
    else {
      cd s; if (sample_rd(R, i, excess_delay_s(det.pos_env, geo.tx, geo.rx[i]) / a.delay_step_s, db[i], &s))
        amp[i] = s * std::polar(1.0, 2 * M_PI * (a.dopp0_hz + db[i] * a.dopp_step_hz) * tbar);
    }
    amp[i] *= cal.coh_factor[i] * std::polar(1.0, -cal.phase_rad[i]) / std::sqrt(R.noise[i]);
  }
  auto coh = [&](const Vec3& x, std::array<cd, kCh>* terms) {
    cd sum = 0;
    for (uint32_t i = 0; i < kCh; ++i) {
      (*terms)[i] = R.los_found[i] ? amp[i] * std::polar(1.0, 2 * M_PI * a.fc_hz * excess_delay_s(x, geo.tx, geo.rx[i])) : cd(0);
      sum += (*terms)[i];
    }
    return std::norm(sum);
  };
  // Phase-difference model against the reference channel: G_d rows g_i - g_ref, W the inverse
  // covariance of the phase differences (per-channel 1/(2 snr) + the calibration's PREDICTIVE phase
  // variance -2 ln coh_factor; the reference's term is shared by all three).
  Matrix Gd(3, 3), Cph(3, 3);
  std::array<uint32_t, 3> oth{}; uint32_t no = 0;
  for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i] && i != ref) oth[no++] = i;
  auto pvar = [&](uint32_t i) { return 1 / (2 * snr[i]) - 2 * std::log(std::max(cal.coh_factor[i], 1e-300)); };
  const Vec3 gref = grad(geo, ref, det.pos_env);
  for (uint32_t r = 0; r < 3; ++r) {
    const Vec3 gd = grad(geo, oth[r], det.pos_env) - gref;
    Gd(r, 0) = gd.x; Gd(r, 1) = gd.y; Gd(r, 2) = gd.z;
    for (uint32_t c = 0; c < 3; ++c) Cph(r, c) = pvar(ref) + (r == c ? pvar(oth[r]) : 0.0);
  }
  const Matrix W = inverse(Cph), Ei = inverse(cov_env);
  // MAP position: phase differences (wrapped) under W, plus the envelope as prior under Cov_env. The
  // plain |sum of terms|^2 is invariant to a COMMON phase, so along the mean bistatic gradient (where
  // every channel's phase moves together) its maximum is set by the terms' amplitude slopes alone
  // (measured: a lobe 2.4 cm off with every phase shifted by pi, |C| 0.2 % higher than at the truth).
  auto Q = [&](const Vec3& x, std::array<cd, kCh>* terms) {
    coh(x, terms);
    std::vector<double> dph(3), dx = v3(x - det.pos_env);
    for (uint32_t r = 0; r < 3; ++r) dph[r] = std::remainder(std::arg((*terms)[oth[r]]) - std::arg((*terms)[ref]), 2 * M_PI);
    return -0.5 * quadratic(dph, W) - 0.5 * quadratic(dx, Ei);
  };
  // Coherent covariance: (lambda/2pi)^2 (G_d^T W G_d)^-1 fused with the envelope prior the position is
  // maximised under (information adds).
  const double k2 = 2 * M_PI / a.lambda_m;
  Matrix info = Gd.transposed() * W * Gd * (k2 * k2);
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) info(r, c) += Ei(r, c);
  Matrix cov_coh = inverse(info);
  const double smin = std::sqrt(std::max(0.0, symmetric_eigen_3x3(symmetrized(cov_coh)).values[0]));
  // Hierarchical search: 5^3 points at step/2, recentre, halve, until the step is below the coherent
  // CRB along its best axis (the spec's half-fringe stop left a 1.1 cm grid on a statistic that is
  // sharp to 0.1 mm at 70 dB: measured 2.4 cm off). Each level costs 125 evaluations. The phase
  // statistic has grating lobes on the lattice lambda G_d^-1 k across the envelope blob; the prior
  // picks the one nearest the envelope, and P_lobe below carries the residual ambiguity.
  // Extent per axis: +-one envelope step (spec C5.2), narrowed to the envelope's own 3-sigma interval
  // when that is smaller (never below half a fringe).
  const Vec3 ext{std::min(g.step, std::max(fs, 3 * det.pos_sigma.x)), std::min(g.step, std::max(fs, 3 * det.pos_sigma.y)),
                 std::min(g.step, std::max(fs, 3 * det.pos_sigma.z))};
  Vec3 bx = det.pos_env; std::array<cd, kCh> bt{}, t{};
  double best = Q(bx, &bt), st_last = 0;
  for (double st = std::max({ext.x, ext.y, ext.z}) / 2; ; st /= 2) {
    st_last = st;
    const Vec3 c0 = bx;
    for (int iz = -2; iz <= 2; ++iz) for (int iy = -2; iy <= 2; ++iy) for (int ix = -2; ix <= 2; ++ix) {
      const Vec3 x = c0 + Vec3{ix * st, iy * st, iz * st};
      const Vec3 o = x - det.pos_env;
      if (x.z < 0 || std::abs(o.x) > ext.x || std::abs(o.y) > ext.y || std::abs(o.z) > ext.z) continue;
      const double v = Q(x, &t);
      if (v > best) { best = v; bx = x; bt = t; }
    }
    if (st * std::sqrt(3.0) < smin || st < 1e-6) break;
  }
  det.terms = bt;
  // Survey coherence, the form consistent with rho = (G-1)/(n-1) (a mean over channel pairs): the LOS
  // calibration is survey-independent, the target phase is not. Channel i's phase moves with rx_i by
  // k|u(rx_i->tx) - u(rx_i->x)| sigma_rx,i; a pair's difference moves with tx by k|u(rx_i->tx) - u(rx_j->tx)| sigma_tx.
  auto sphi2 = [&](uint32_t i) {
    const double s = k2 * norm(unit(geo.tx - geo.rx[i]) - unit(det.pos_env - geo.rx[i])) * survey.rx_m[i]; return s * s;
  };
  double keep = 0; uint32_t np = 0;
  for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j) if (R.los_found[i] && R.los_found[j]) {
    const double stx = k2 * norm(unit(geo.tx - geo.rx[i]) - unit(geo.tx - geo.rx[j])) * survey.tx_m;
    keep += std::exp(-0.5 * (sphi2(i) + sphi2(j) + stx * stx)); ++np;
  }
  for (int r = 0; r < 3; ++r) cov_coh(r, r) += st_last * st_last / 12;      // the search's own grid
  // Lobe ambiguity: other lobes sit at lambda G_d^-1 k, k integer. P_lobe = probability that the
  // envelope error is closer to the true lobe than to the nearest other one, 1 - 2Q(d_M/2), d_M the
  // smallest Mahalanobis length of a lobe offset under Cov_env.
  const Matrix Gi = inverse(Gd);
  double dM = INFINITY;
  for (int k0 = -1; k0 <= 1; ++k0) for (int k1 = -1; k1 <= 1; ++k1) for (int k2i = -1; k2i <= 1; ++k2i) {
    if (!k0 && !k1 && !k2i) continue;
    const std::vector<double> dx = Gi * std::vector<double>{a.lambda_m * k0, a.lambda_m * k1, a.lambda_m * k2i};
    dM = std::min(dM, std::sqrt(std::max(0.0, quadratic(dx, Ei))));
  }
  const double p_lobe = std::max(0.0, 1 - 2 * qfunc(dM / 2));
  const double rho = cal.rho * (np ? keep / np : 0.0) * p_lobe;
  det.pos = bx * rho + det.pos_env * (1 - rho);
  // Mixture covariance about the blended mean.
  const Vec3 dd = bx - det.pos_env; const double dv[3] = {dd.x, dd.y, dd.z};
  Matrix cov = cov_coh * rho + cov_env * (1 - rho);
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) cov(r, c) += rho * (1 - rho) * dv[r] * dv[c];
  set_cov(det, cov);
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
