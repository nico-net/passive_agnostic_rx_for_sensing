/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* coherent_longdwell.cc -- long-dwell slow-target CPI, see coherent_longdwell.h. */
#include "coherent_longdwell.h"
#include "coherent_cuda.h"
#include "coherent_cpi_dump.h"
#include "fft.h"
#include "robust_stats.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace nr_isac::coherent {
namespace {
using clk = std::chrono::steady_clock;
double ms_since(clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }
double baseband_hz(uint32_t S, double scs, uint32_t k) { return ((double)k - S / 2.0) * scs; }
} // namespace

SuperRows make_super_rows(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s)
{
  SuperRows o;
  if (!a.valid || a.tested_dopp.empty() || !w.valid() || w.antennas != kCh || s.phase_rad.size() != w.rows
      || s.delay_s.size() != w.rows) return o;
  // The first Doppler bin past the short CPI's notch (bins are integer multiples of dopp_step). Not the
  // min over tested_dopp: derive_axes's floating-point notch test flips the edge bin on one ulp.
  const double fs = (a.notch_half_bins + 1) * a.dopp_step_hz;
  const uint32_t S = w.subcarriers;
  const double slot = slot_duration_s(w.scs_hz), Ts = 1.0 / (4 * fs);
  std::vector<uint32_t> rows;
  for (uint32_t r = 0; r < w.rows; ++r) {
    if (s.bad.size() == w.rows && s.bad[r]) continue;
    if (std::any_of(w.observed.begin() + w.cell(r, 0), w.observed.begin() + w.cell(r, 0) + S, [](uint8_t v) { return v != 0; }))
      rows.push_back(r);
  }
  if (rows.empty()) return o;
  // Equal blocks of length <= T_s over the CPI's own span.
  const double t_first = w.row_time_slots[rows.front()] * slot, span = w.row_time_slots[rows.back()] * slot - t_first;
  const uint32_t nb = std::max<uint32_t>(1, (uint32_t)std::ceil(span / Ts));
  const double Tb = span / nb;
  struct Blk { std::vector<cd> acc; std::vector<uint32_t> cnt; double tsum = 0; uint32_t n = 0, slot_idx = 0, mask = 0; };
  std::vector<Blk> B(nb);
  o.sum.assign((size_t)kCh * S, cd(0)); o.cnt.assign(S, 0);
  for (uint32_t r : rows) {
    const double t = w.row_time_slots[r] * slot - t_first;
    const uint32_t b = Tb > 0 ? std::min(nb - 1, (uint32_t)std::floor(t / Tb)) : 0;
    Blk& k_ = B[b];
    if (k_.acc.empty()) { k_.acc.assign((size_t)kCh * S, cd(0)); k_.cnt.assign(S, 0); k_.slot_idx = w.row_slot_idx[r]; }
    k_.tsum += w.row_time_slots[r]; ++k_.n; k_.mask |= w.row_source_mask[r];
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t k = 0; k < S; ++k) if (w.observed[w.cell(r, k)]) { lo = std::min(lo, k); hi = k; ++k_.cnt[k]; ++o.cnt[k]; }
    const double amp = s.amp.empty() ? 1.0 : s.amp[r];
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!L.found[i]) continue;
      // range_doppler()'s derotation: amp * z * e^{j(2pi f_k (L.delay + s.delay) - s.phase)}, as a recurrence over k
      const double tau = L.delay_s[i] + s.delay_s[r];
      cd ph = std::polar(amp, 2 * M_PI * baseband_hz(S, w.scs_hz, lo) * tau - s.phase_rad[r]);
      const cd rot = std::polar(1.0, 2 * M_PI * w.scs_hz * tau);
      for (uint32_t k = lo; k <= hi; ++k, ph *= rot) if (w.observed[w.cell(r, k)]) {
        const cd z = cd(w.values[w.sample(i, r, k)]) * ph;
        k_.acc[(size_t)i * S + k] += z; o.sum[(size_t)i * S + k] += z;
      }
    }
  }
  uint32_t nr = 0; for (const Blk& b : B) nr += b.n > 0;
  CfrWindow& v = o.w;
  v.session_id = w.session_id; v.antennas = kCh; v.rows = nr; v.subcarriers = S; v.scs_hz = w.scs_hz; v.fc_hz = w.fc_hz;
  v.pci = w.pci; v.start_utc_ns = w.start_utc_ns;
  v.values.assign((size_t)kCh * nr * S, cf(0)); v.observed.assign((size_t)nr * S, 0);
  uint32_t q = 0;
  for (const Blk& b : B) {
    if (!b.n) continue;
    v.row_time_slots.push_back(b.tsum / b.n); v.row_slot_idx.push_back(b.slot_idx); v.row_slot_frac.push_back(0.0);
    v.row_source_mask.push_back(b.mask);
    for (uint32_t k = 0; k < S; ++k) if (b.cnt[k]) {
      v.observed[v.cell(q, k)] = 1;
      for (uint32_t i = 0; i < kCh; ++i) v.values[v.sample(i, q, k)] = cf(b.acc[(size_t)i * S + k] / (double)b.cnt[k]);
    }
    ++q;
  }
  // The long CPI's range cell is its OWN (derive_axes: median row bandwidth over its rows, here the
  // super-rows, whose union allocations are wider than the short CPI's rows): T_L bounds migration by it.
  std::vector<double> bw;
  for (uint32_t r = 0; r < nr; ++r) {
    int lo = -1, hi = -1;
    for (uint32_t k = 0; k < S; ++k) if (v.observed[v.cell(r, k)]) { if (lo < 0) lo = (int)k; hi = (int)k; }
    if (lo >= 0) bw.push_back((hi - lo + 1) * w.scs_hz);
  }
  o.found = L.found; o.f_slow_hz = fs; o.b_eff_hz = median(bw);
  o.t0_slots = v.row_time_slots.front(); o.t1_slots = v.row_time_slots.back();
  return o;
}

void LongDwell::ref_add(const SuperRows& s, int sign)
{
  const uint32_t S = s.w.subcarriers;
  if (ref_sum_.size() != (size_t)kCh * S) { ref_sum_.assign((size_t)kCh * S, cd(0)); ref_cnt_.assign((size_t)kCh * S, 0); }
  for (uint32_t i = 0; i < kCh; ++i) if (s.found[i])
    for (uint32_t k = 0; k < S; ++k) if (s.cnt[k]) {
      ref_sum_[(size_t)i * S + k] += (double)sign * s.sum[(size_t)i * S + k];
      if (sign > 0) ref_cnt_[(size_t)i * S + k] += s.cnt[k]; else ref_cnt_[(size_t)i * S + k] -= s.cnt[k];
    }
}

bool LongDwell::add(SuperRows sr, LongCpi* out)
{
  last_delay_s = 0; last_gain = 1;
  if (!sr.valid()) return false;
  const uint32_t S = sr.w.subcarriers;
  const double slot = slot_duration_s(sr.w.scs_hz), lambda = kC / sr.w.fc_hz;
  // T_L over a set of short CPIs: the median short CPI's first bin past its notch and the median
  // super-row bandwidth. Median, not max: OTA a few short CPIs close after 3 rows / 12 ms (a notch of
  // +-160 Hz), and a max let one of them turn a whole long CPI into a 0.2 s, +-16 m/s one (measured).
  auto dwell = [&](const std::deque<SuperRows>& q, double* fs_out, double* b_out) {
    std::vector<double> be, fv;
    for (const SuperRows& e : q) { be.push_back(e.b_eff_hz); fv.push_back(e.f_slow_hz); }
    *fs_out = median(fv); *b_out = median(be);
    return (kC / *b_out) / (lambda * *fs_out);
  };
  if (!q_.empty()) {
    const SuperRows& b = q_.back();
    if (b.w.subcarriers != S || b.w.scs_hz != sr.w.scs_hz || b.w.fc_hz != sr.w.fc_hz) reset();
    else { double fs, be; if ((sr.t0_slots - b.t1_slots) * slot > dwell(q_, &fs, &be)) reset(); }   // a gap longer than a dwell: no coherence across it
  }
  // Align this short CPI to the running static profile S: one common delay (the per-CPI common LOS
  // offset jitter) then one complex gain (the per-CPI CFO phase / level), over every channel and
  // subcarrier. Delay = the peak of the static cross-correlation c(d) = |sum_k X_k e^{j2pi f_k d}|,
  // X_k = sum_i conj(S_ik) sum_ik: the static self-match dominates it, so moving targets (always at
  // positive excess delay) cannot pull it. (An adjacent-subcarrier phase-slope estimate is their
  // power-weighted centroid: measured +0.5 ns per short CPI of bias, a random walk of the reference to
  // +10 ns in 4 s on the chain scene.)
  if (!ref_sum_.empty()) {
    std::vector<cd> X(S, cd(0)); bool any = false;
    for (uint32_t i = 0; i < kCh; ++i) if (sr.found[i])
      for (uint32_t k = 0; k < S; ++k) {
        const size_t x = (size_t)i * S + k;
        if (sr.cnt[k] && ref_cnt_[x]) { X[k] += std::conj(ref_sum_[x] / (double)ref_cnt_[x]) * sr.sum[x]; any = true; }
      }
    if (any) {
      uint32_t n = 1; while (n < S) n <<= 1;
      const double step = 1.0 / (n * sr.w.scs_hz);                 // IFFT bin (s)
      std::vector<cd> buf(n, cd(0));
      for (uint32_t k = 0; k < S; ++k) buf[(k + n - S / 2) % n] = X[k];
      fft_inplace(buf, true);                                       // buf[m] ~ c(m step), circular
      uint32_t pk = 0; for (uint32_t m = 1; m < n; ++m) if (std::abs(buf[m]) > std::abs(buf[pk])) pk = m;
      auto c = [&](double d) {
        const cd rot = std::polar(1.0, 2 * M_PI * sr.w.scs_hz * d); cd ph = std::polar(1.0, 2 * M_PI * baseband_hz(S, sr.w.scs_hz, 0) * d), acc = 0;
        for (uint32_t k = 0; k < S; ++k, ph *= rot) acc += X[k] * ph;
        return std::abs(acc);
      };
      // golden section on [peak-1, peak+1] bins, to numerical resolution
      const double gr = 0.5 * (std::sqrt(5.0) - 1), d0 = ((pk < n / 2) ? (double)pk : (double)pk - n) * step;
      double xa = d0 - step, xb = d0 + step, xc = xb - gr * (xb - xa), xd = xa + gr * (xb - xa), fc_ = c(xc), fd_ = c(xd);
      while (xb - xa > 1e-6 * step) {
        if (fc_ > fd_) { xb = xd; xd = xc; fd_ = fc_; xc = xb - gr * (xb - xa); fc_ = c(xc); }
        else { xa = xc; xc = xd; fc_ = fd_; xd = xa + gr * (xb - xa); fd_ = c(xd); }
      }
      const double dr = 0.5 * (xa + xb);
      std::vector<cd> ramp(S);
      for (uint32_t k = 0; k < S; ++k) ramp[k] = std::polar(1.0, 2 * M_PI * baseband_hz(S, sr.w.scs_hz, k) * dr);
      cd num = 0; double den = 0;                                  // LS gain, weight cnt (sum = cnt * mean)
      for (uint32_t i = 0; i < kCh; ++i) if (sr.found[i]) for (uint32_t k = 0; k < S; ++k) {
        const size_t x = (size_t)i * S + k; if (!sr.cnt[k] || !ref_cnt_[x]) continue;
        const cd ref = ref_sum_[x] / (double)ref_cnt_[x];
        num += std::conj(ref) * sr.sum[x] * ramp[k]; den += std::norm(ref) * sr.cnt[k];
      }
      // (Measured, not adopted: a per-SUPER-ROW common gain and a per-CHANNEL per-CPI gain on top of this
      // left the OTA empty-room long false rate unchanged or worse, 1.38 -> 1.53 / 1.49 per CPI.)
      if (den > 0 && std::abs(num) > 0) {
        const cd g = num / den;
        last_delay_s = dr; last_gain = g; sr.align_delay_s = dr;
        for (uint32_t k = 0; k < S; ++k) ramp[k] /= g;
        for (uint32_t i = 0; i < kCh; ++i) for (uint32_t k = 0; k < S; ++k) sr.sum[(size_t)i * S + k] *= ramp[k];
        for (uint32_t i = 0; i < kCh; ++i) for (uint32_t r = 0; r < sr.w.rows; ++r) for (uint32_t k = 0; k < S; ++k)
          if (sr.w.observed[sr.w.cell(r, k)]) sr.w.values[sr.w.sample(i, r, k)] = cf(cd(sr.w.values[sr.w.sample(i, r, k)]) * ramp[k]);
      }
    }
  }
  ref_add(sr, +1);
  if (start_slots_ < 0) start_slots_ = sr.t0_slots;
  q_.push_back(std::move(sr));
  double fs, be; const double TL = dwell(q_, &fs, &be);
  const double t_end = q_.back().t1_slots;
  if ((t_end - start_slots_) * slot < TL) return false;
  // Emit the super-rows within [t_end - T_L, t_end].
  const double from = t_end - TL / slot;
  LongCpi& c = *out; c = LongCpi{};
  CfrWindow& v = c.w; const CfrWindow& f = q_.front().w;
  v.session_id = f.session_id; v.antennas = kCh; v.subcarriers = S; v.scs_hz = f.scs_hz; v.fc_hz = f.fc_hz; v.pci = f.pci;
  c.found.fill(true);
  std::vector<std::pair<const SuperRows*, uint32_t>> sel;
  for (const SuperRows& e : q_) {
    bool used = false;
    for (uint32_t r = 0; r < e.w.rows; ++r) if (e.w.row_time_slots[r] >= from) { sel.push_back({&e, r}); used = true; }
    if (used) { if (!c.n_short) v.start_utc_ns = e.w.start_utc_ns; ++c.n_short; for (uint32_t i = 0; i < kCh; ++i) c.found[i] = c.found[i] && e.found[i]; }
  }
  // Consecutive short CPIs can interleave by a few slots (rows from parallel lanes close into the next
  // window): order the super-rows by time.
  std::stable_sort(sel.begin(), sel.end(), [](const auto& x, const auto& y) { return x.first->w.row_time_slots[x.second] < y.first->w.row_time_slots[y.second]; });
  v.rows = (uint32_t)sel.size();
  v.values.assign((size_t)kCh * v.rows * S, cf(0)); v.observed.assign((size_t)v.rows * S, 0);
  for (uint32_t q = 0; q < v.rows; ++q) {
    const CfrWindow& e = sel[q].first->w; const uint32_t r = sel[q].second;
    v.row_time_slots.push_back(e.row_time_slots[r]); v.row_slot_idx.push_back(e.row_slot_idx[r]);
    v.row_slot_frac.push_back(e.row_slot_frac[r]); v.row_source_mask.push_back(e.row_source_mask[r]);
    std::copy(e.observed.begin() + e.cell(r, 0), e.observed.begin() + e.cell(r, 0) + S, v.observed.begin() + v.cell(q, 0));
    for (uint32_t i = 0; i < kCh; ++i)
      std::copy(e.values.begin() + e.sample(i, r, 0), e.values.begin() + e.sample(i, r, 0) + S, v.values.begin() + v.sample(i, q, 0));
  }
  // Re-centre on the LOS: CPI j's LOS sat at its own reference error e_j and was moved to the profile's
  // e_ref = e_j - dr_j; the e_j are zero-mean, so e_ref ~ -mean(dr_j) over the contributing CPIs.
  {
    double md = 0; uint32_t n = 0;
    for (const SuperRows& e : q_) if (e.t1_slots >= from) { md += e.align_delay_s; ++n; }
    if (n) md /= n;
    std::vector<cf> ramp(S);
    for (uint32_t k = 0; k < S; ++k) ramp[k] = cf(std::polar(1.0, -2 * M_PI * baseband_hz(S, v.scs_hz, k) * md));
    for (uint32_t i = 0; i < kCh; ++i) for (uint32_t q = 0; q < v.rows; ++q) for (uint32_t k = 0; k < S; ++k) v.values[v.sample(i, q, k)] *= ramp[k];
  }
  c.f_slow_hz = fs; c.t_l_s = TL; c.cadence_s = TL / 2; c.b_eff_hz = be;
  const SuperRows& last = q_.back();
  c.t_air_s = last.t_air_s + (0.5 * (v.row_time_slots.front() + v.row_time_slots.back()) - last.mid_slots) * slot;
  // 50 % overlap: the next long CPI starts half a dwell before this one ended.
  start_slots_ = t_end - 0.5 * TL / slot;
  while (!q_.empty() && q_.front().t1_slots < start_slots_) { ref_add(q_.front(), -1); q_.pop_front(); }
  return true;
}

LongDwellRunner::LongDwellRunner(const CoherentConfig& cfg, bool use_cuda, Sink sink)
    : cfg_(cfg), use_cuda_(use_cuda), sink_(std::move(sink))
{
  worker_ = std::thread([this] { run(); });
}

LongDwellRunner::~LongDwellRunner()
{
  { std::lock_guard<std::mutex> l(mu_); stop_ = true; }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void LongDwellRunner::push(CfrWindow&& w, const Axes& a, const LosEstimate& L, const RowSync& s, double t, const Geometry& geo,
                           const Calibration& cal)
{
  std::lock_guard<std::mutex> l(mu_);
  q_.push_back(Job{std::move(w), a, L, s, t, geo, cal});
  qmax_ = std::max(qmax_, q_.size());
  cv_.notify_all();
}

void LongDwellRunner::run()
{
  if (use_cuda_) {    // own device context objects on this thread (per-thread default stream)
    try { cuda_ = std::make_unique<CudaCoherent>(); }
    catch (const std::exception& e) { std::fprintf(stderr, "SENSING: long dwell: CUDA init failed (%s); CPU chain\n", e.what()); }
  }
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait(l, [this] { return !q_.empty() || stop_; });
      if (q_.empty()) return;
      j = std::move(q_.front()); q_.pop_front();
    }
    try { process(j); }
    catch (const std::exception& e) { std::fprintf(stderr, "SENSING: long-dwell CPI failed: %s\n", e.what()); }
  }
}

void LongDwellRunner::process(Job& j)
{
  auto t0 = clk::now(); const auto tstart = t0;
  SuperRows sr = make_super_rows(j.w, j.a, j.L, j.s);
  sr.t_air_s = j.t; if (j.w.rows) sr.mid_slots = 0.5 * (j.w.row_time_slots.front() + j.w.row_time_slots.back());
  LongResult r;
  if (!ld_.add(std::move(sr), &r.cpi)) return;
  r.tm_build = ms_since(t0); t0 = clk::now();
  { std::lock_guard<std::mutex> l(mu_); r.queue_max = qmax_; }
  const LongCpi& c = r.cpi;
  const Geometry& geo = j.geo;
  if (const char* dd = std::getenv("NR_ISAC_LONG_DUMP")) {   // offline diagnostics: every long window (coherent_cpi_dump.h)
    static int n = 0;
    write_cpi_dump(std::string(dd) + "/long_" + std::to_string(n++) + ".bin", CpiDump{c.w, cfg_.volume, geo, c.f_slow_hz * (kC / c.w.fc_hz) / 2});
  }
  // Tested |f| <= f_slow: derive_axes tests |f| <= 2 v_max / lambda.
  r.a = derive_axes(c.w, cfg_.volume, geo, c.f_slow_hz * (kC / c.w.fc_hz) / 2);
  // A long CPI must carry at least as many super-rows as the Doppler bins it is built to test, 2 f_slow T_L
  // (unknowns): OTA, windows around traffic gaps held 10-34 rows and put 5-19 ghosts each (measured). A
  // full window carries ~2x (T_s = 1/(4 f_slow)).
  if (r.a.valid && c.w.rows < 2 * c.f_slow_hz * c.t_l_s) { r.a.valid = false; r.a.invalid_reason = "fewer super-rows than tested Doppler bins"; }
  if (r.a.valid) {
    const Axes& a = r.a;
    LosEstimate L0; L0.found = c.found;                       // rows are LOS-referenced already: delay 0
    RowSync s0; s0.phase_rad.assign(c.w.rows, 0.0); s0.delay_s.assign(c.w.rows, 0.0); s0.valid = true;
    RdResult R = cuda_ ? cuda_->range_doppler(c.w, a, L0, s0, true) : range_doppler(c.w, a, L0, s0);
    r.G = envelope_grid(cfg_.volume, a);
    // Expected false objects per long CPI = intensity x cadence (not x T_L): at 50 % overlap the long
    // CPIs then add the declared intensity per second, not twice it.
    const DetectParams P = detect_params(a, r.G, cfg_.false_object_intensity_per_s * c.cadence_s / a.t_cpi_s);
    { const std::vector<float> wg = whiten_range_clutter(R, P.pfa); if (cuda_) cuda_->scale_rd(wg); }
    r.tm_rd = ms_since(t0); t0 = clk::now();
    if (cuda_) {
      r.D = cuda_->detect(r.G, geo, P, nullptr);
      r.tm_env = cuda_->last_timing().envelope_ms; r.tm_detect = std::max(0.0, ms_since(t0) - r.tm_env);
    } else {
      const std::vector<float> E = envelope(R, r.G, geo);
      r.tm_env = ms_since(t0); t0 = clk::now();
      r.D = detect(E, R, r.G, geo, P);
      r.tm_detect = ms_since(t0);
    }
    t0 = clk::now();
    const SurveySigma survey{{cfg_.survey_sigma_m, cfg_.survey_sigma_m, cfg_.survey_sigma_m, cfg_.survey_sigma_m}, cfg_.survey_sigma_m};
    if (cuda_) cuda_->refine(r.D, r.G, geo, j.cal, survey);
    else for (Detection& d : r.D) refine(d, R, r.G, geo, j.cal, survey);
    // The long CPI owns |rate| <= lambda f_slow only: a fit outside it is a faster mover seen through the
    // band edge (the short CPI's job), not a slow target.
    const double rho = a.lambda_m * c.f_slow_hz;
    r.D.erase(std::remove_if(r.D.begin(), r.D.end(), [&](const Detection& d) { return !(std::abs(d.range_rate_mps) <= rho); }), r.D.end());
    r.tm_refine = ms_since(t0);
  }
  r.tm_total = ms_since(tstart);
  sink_(r);
}

} // namespace nr_isac::coherent
