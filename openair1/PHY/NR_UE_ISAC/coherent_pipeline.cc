/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* coherent_pipeline.cc -- CPU path; Task 10 adds the CUDA branch. */
#include "coherent_pipeline.h"
#include "coherent_ul.h"
#include "coherent_cpi_dump.h"
#include "cuda_support.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace nr_isac::coherent {
namespace {
using clk = std::chrono::steady_clock;
double ms_since(clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }
std::string jnum(double v)
{
  if (!std::isfinite(v)) return "null";
  std::ostringstream o; o.precision(7); o << v; return o.str();
}
std::string jvec(const Vec3& v) { return "[" + jnum(v.x) + "," + jnum(v.y) + "," + jnum(v.z) + "]"; }
template <class A> std::string jarr(const A& a)
{
  std::string s = "[";
  for (size_t i = 0; i < a.size(); ++i) s += (i ? "," : "") + jnum((double)a[i]);
  return s + "]";
}
std::string jbools(const std::array<bool, kCh>& a)
{
  std::string s = "[";
  for (size_t i = 0; i < a.size(); ++i) s += std::string(i ? "," : "") + (a[i] ? "true" : "false");
  return s + "]";
}
/** dB relative to the image's median positive value, rounded to 0.1 dB. */
std::vector<double> to_db(const std::vector<double>& p)
{
  std::vector<double> q; for (double v : p) if (v > 0) q.push_back(v);
  double ref = 1.0;
  if (!q.empty()) { std::nth_element(q.begin(), q.begin() + q.size() / 2, q.end()); ref = q[q.size() / 2]; }
  std::vector<double> o(p.size());
  for (size_t i = 0; i < p.size(); ++i) o[i] = std::round(100 * std::log10(std::max(p[i], 1e-30) / ref)) / 10;
  return o;
}
uint32_t rows_with_data(const CfrWindow& w)
{
  uint32_t n = 0;
  for (uint32_t r = 0; r < w.rows; ++r)
    if (std::any_of(w.observed.begin() + w.cell(r, 0), w.observed.begin() + w.cell(r, 0) + w.subcarriers, [](uint8_t o) { return o != 0; })) ++n;
  return n;
}
} // namespace

CoherentPipeline::CoherentPipeline(const CoherentConfig& cfg)
    : cfg_(cfg), reports_(cfg.out_dir, "coherent_reports"), tracks_(cfg.out_dir, "coherent_tracks"),
      coherence_(cfg.out_dir, "coherence")
{
  TrackerParams tp;
  tp.max_speed_mps = cfg.max_speed_mps;
  tp.false_object_intensity_per_s = cfg.false_object_intensity_per_s;
  tp.volume = cfg.volume;
  for (const Vec3& r : cfg.geometry.rx) tp.array_centroid = tp.array_centroid + r * (1.0 / kCh);
  tracker_ = std::make_unique<CoherentTracker>(tp);
  // Task 10: GPU range_doppler()/envelope(); CPU find_los/row-sync/detect()'s own branch-and-bound
  // fits/refine()/tracker stay on the host either way (see coherent_cuda.h's file header).
  // NR_ISAC_CUDA_DISABLE=1: force the CPU path even when a device is present, for a true same-host
  // same-run A/B (Task 10 benchmarking only; not part of any production config surface).
  if (CudaCoherent::available() && !environment_flag_enabled("NR_ISAC_CUDA_DISABLE")) {
    try { cuda_ = std::make_unique<CudaCoherent>(); std::fprintf(stderr, "SENSING: coherent pipeline: CUDA range_doppler/envelope enabled\n"); }
    catch (const std::exception& e) { std::fprintf(stderr, "SENSING: coherent pipeline: CUDA init failed (%s); falling back to CPU\n", e.what()); }
  }
  if (!cuda_ && cuda_required()) {
    cuda_fail_closed_ = true;
    std::fprintf(stderr, "SENSING: coherent pipeline: NR_ISAC_REQUIRE_CUDA=1 but no usable CUDA device; every CPI will be skipped\n");
  }
  worker_ = std::thread([this] { run(); });
  std::fprintf(stderr, "SENSING: coherent pipeline writing %s\n", reports_.path().c_str());
}

CoherentPipeline::~CoherentPipeline()
{
  { std::lock_guard<std::mutex> l(mu_); stop_ = true; }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void CoherentPipeline::submit(CfrWindow dl, std::vector<CfrWindow> ul, uint64_t seq, double t)
{
  std::unique_lock<std::mutex> l(mu_);
  if (q_.size() >= 2) { ++st_.queue_waits; cv_.wait(l, [this] { return q_.size() < 2 || stop_; }); }   // never drop
  q_.push_back(Job{std::move(dl), std::move(ul), seq, t});
  cv_.notify_all();
}

CoherentStats CoherentPipeline::stats() const { std::lock_guard<std::mutex> l(mu_); return st_; }

void CoherentPipeline::run()
{
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait(l, [this] { return !q_.empty() || stop_; });
      if (q_.empty()) return;                 // stop_ and drained
      j = std::move(q_.front()); q_.pop_front();
    }
    cv_.notify_all();
    try { process(j); }
    catch (const std::exception& e) {
      // detect() throws std::invalid_argument on an internal envelope/grid size mismatch; any other
      // stage can throw too (e.g. std::bad_alloc). Count it under the same "no coherent output this
      // CPI" bucket the axes-invalid path uses, log it, and keep draining -- one bad CPI must not stop
      // the worker.
      { std::lock_guard<std::mutex> l(mu_); ++st_.skipped; }
      std::fprintf(stderr, "SENSING: coherent CPI %llu failed: %s\n", (unsigned long long)j.seq, e.what());
    }
  }
}

void CoherentPipeline::write_coherence(uint64_t seq, double t, const Calibration& cal, const std::array<double, kCh>& los_delay_s, bool skipped)
{
  std::ostringstream co;
  co << "{\"cpi\":" << seq << ",\"t\":" << jnum(t) << ",\"phase\":" << jarr(cal.phase_rad) << ",\"phase_var\":" << jarr(cal.phase_var)
     << ",\"jitter\":" << jarr(cal.jitter_rad) << ",\"bound\":" << jarr(cal.jitter_bound_rad) << ",\"snr\":" << jarr(cal.los_snr)
     << ",\"los_found\":" << jbools(cal.los_found) << ",\"G\":" << jnum(cal.coherent_gain) << ",\"rho\":" << jnum(cal.rho)
     << ",\"af_corr_m\":" << (af_ ? jarr(af_->correction_norm_m()) : jarr(std::array<double, kCh>{}));
  std::array<double, kCh> ns{}; for (uint32_t i = 0; i < kCh; ++i) ns[i] = los_delay_s[i] * 1e9;
  co << ",\"los_delay_ns\":" << jarr(ns);      // extra key: each channel's LOS reference delay (window-absolute)
  if (skipped) co << ",\"skipped\":true";      // calibration values are the last CPI's, unchanged
  co << "}";
  coherence_.write_line(co.str());
}

void CoherentPipeline::process(Job& j)
{
  const auto t0 = clk::now();
  double tm[7] = {0};   // sync, rd, env, detect, refine, track, ul
  double tm_los = 0;    // find_los share of tm[0]
  if (!af_) af_ = std::make_unique<Autofocus>(cfg_.geometry, cfg_.survey_sigma_m, j.dl.fc_hz);
  const Geometry geo = af_->geometry();      // this CPI's focus geometry (autofocus contract)
  const Axes a = cuda_fail_closed_ ? Axes{false, "cuda required but unavailable"}
               : j.dl.valid() ? derive_axes(j.dl, cfg_.volume, geo, cfg_.max_speed_mps) : Axes{false, "invalid CFR window"};
  if (!a.valid) {
    { std::lock_guard<std::mutex> l(mu_); ++st_.skipped; }
    const CoherentStats st = stats();
    std::ostringstream rep;
    rep << "{\"cpi\":" << j.seq << ",\"t\":" << jnum(j.t) << ",\"rows\":" << j.dl.rows << ",\"stats\":{\"processed\":" << st.processed
        << ",\"skipped\":" << st.skipped << ",\"overruns\":" << st.overruns << ",\"queue_waits\":" << st.queue_waits
        << "},\"skipped_reason\":\"" << a.invalid_reason << "\",\"detections\":[],\"topview\":null,\"rd\":null}";
    reports_.write_line(rep.str());
    tracks_.write_line("{\"cpi\":" + std::to_string(j.seq) + ",\"t\":" + jnum(j.t) + ",\"tracks\":[]}");
    write_coherence(j.seq, j.t, cal_.last(), {}, true);
    return;
  }
  if (const char* dd = std::getenv("NR_ISAC_COH_DUMP")) {   // offline parity data (coherent_cpi_dump.h)
    static int dumped = 0;
    const char* nn = std::getenv("NR_ISAC_COH_DUMP_N"); const char* ev = std::getenv("NR_ISAC_COH_DUMP_EVERY");
    if (dumped < (nn ? std::atoi(nn) : 4) && j.seq % (uint64_t)std::max(1, ev ? std::atoi(ev) : 1) == 0) {
      ++dumped;
      if (!write_cpi_dump(std::string(dd) + "/cpi_" + std::to_string(j.seq) + ".bin", CpiDump{j.dl, cfg_.volume, geo, cfg_.max_speed_mps}))
        std::fprintf(stderr, "SENSING: coherent CPI dump to %s failed\n", dd);
    }
  }
  // LOS search tests n_fft bins per channel: at most one expected false LOS pick per CPI over all channels.
  const double pfa_los = std::min(0.5, 1.0 / ((double)a.n_fft * kCh));
  const SurveySigma survey{{cfg_.survey_sigma_m, cfg_.survey_sigma_m, cfg_.survey_sigma_m, cfg_.survey_sigma_m}, cfg_.survey_sigma_m};

  auto s0 = clk::now();
  // GPU path: find_los / row sync run coherent_core.cc's decisions on device-computed arrays
  // (CudaCoherent::find_los / estimate_row_sync, coherent_cuda_front.cu).
  std::array<double, kCh> geo_los{};             // survey LOS delays: picks the LOS over a stronger wall
  for (uint32_t i = 0; i < kCh; ++i) geo_los[i] = dist(geo.tx, geo.rx[i]) / kC;
  if (cuda_) cuda_->upload(j.dl);                // once per CPI: find_los, row sync and range_doppler share it
  LosEstimate L = cuda_ ? cuda_->find_los(j.dl, a, pfa_los, &geo_los) : find_los(j.dl, a, pfa_los, &geo_los);
  // Common-offset LOS referencing. Each channel's own per-CPI LOS delay jitters by 27-50 ns OTA
  // (narrow hopping allocations), and the LOS tap phase moves by 2*pi*f_alloc*delay_err with the
  // allocation centre hopping +-50 MHz: that alone made the calibration phases uniform (measured
  // 2026-09-24, jitter ~1.7 rad, G ~ 0.9). The inter-channel delays are FIXED (one X410, identical
  // cables, static geometry), so per CPI only ONE common offset (STO) is estimated, as the median over
  // channels, and each channel sits at survey + offset + its own residual (cable/survey error),
  // the cumulative median of its past residuals (static hardware: no forgetting constant).
  {
    auto med = [](std::vector<double> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
    std::array<double, kCh> res{};
    for (uint32_t i = 0; i < kCh; ++i) res[i] = los_resid_[i].empty() ? 0.0 : med(los_resid_[i]);
    std::vector<double> off;
    for (uint32_t i = 0; i < kCh; ++i) if (L.found[i]) off.push_back(L.delay_s[i] - geo_los[i] - res[i]);
    if (off.size() >= 2) {
      const double o = med(off);
      for (uint32_t i = 0; i < kCh; ++i) if (L.found[i]) {
        los_resid_[i].push_back(L.delay_s[i] - geo_los[i] - o);
        L.delay_s[i] = geo_los[i] + o + res[i];
      }
    }
  }
  tm_los = ms_since(s0);
  const RowSync rs = cuda_ ? cuda_->estimate_row_sync(j.dl, a, L) : estimate_row_sync(j.dl, a, L);
  tm[0] = ms_since(s0); s0 = clk::now();
  // L.found flows into R.los_found: envelope / detect / refine skip channels without a LOS reference.
  const RdResult R = cuda_ ? cuda_->range_doppler(j.dl, a, L, rs, true) : range_doppler(j.dl, a, L, rs);
  // Static-removal diagnostic, per channel, in dB over the channel's thermal noise: the direct path
  // before removal (LOS tap) and the strongest residue left at the direct path's range (bins 0-3),
  // just outside the zero-Doppler notch (up to 3 bins beyond it) and anywhere else tested.
  std::array<double, kCh> st_los{}, st_edge{}, st_far{};
  {
    const long dz = std::lround(-a.dopp0_hz / a.dopp_step_hz);
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!L.found[i] || !(R.noise[i] > 0) || R.rd.v.empty()) continue;
      st_los[i] = 10 * std::log10(std::max(std::norm(R.los_tap[i]) / R.noise[i], 1e-30));
      double e = 1e-30, f = 1e-30;
      for (uint32_t m = 0; m < std::min<uint32_t>(4, a.n_range); ++m)
        for (uint32_t d : a.tested_dopp) {
          const double pw = std::norm(R.rd.v[R.rd.idx(i, m, d)]) / R.noise[i];
          if (std::labs((long)d - dz) <= (long)a.notch_half_bins + 3) e = std::max(e, pw); else f = std::max(f, pw);
        }
      st_edge[i] = 10 * std::log10(e); st_far[i] = 10 * std::log10(f);
    }
  }
  tm[1] = ms_since(s0); s0 = clk::now();
  if (cuda_ && std::getenv("NR_ISAC_CUDA_DEBUG_TIMING")) { const auto gt = cuda_->last_timing();
    std::fprintf(stderr, "CUDA_TIMING rows=%u upload=%.2f build=%.2f fft=%.2f crop_norm=%.2f nudft=%.2f download=%.2f wf=%.2f\n",
                 j.dl.rows, gt.upload_ms, gt.build_ms, gt.fft_ms, gt.crop_norm_ms, gt.nudft_ms, gt.download_ms, gt.wf_ms); }
  // Calibrator SNR = SNR of R.los_tap, the mean of the n_rows rows' LOS taps: find_los's L.snr is the
  // per-row (signal+noise)/noise power ratio at the LOS bin, so the coherent R-row mean carries
  // (L.snr - 1) * n_rows.
  std::array<double, kCh> los_snr{};
  const uint32_t n_rows = rows_with_data(j.dl);
  for (uint32_t i = 0; i < kCh; ++i) los_snr[i] = L.found[i] ? std::max(L.snr[i] - 1.0, 0.0) * n_rows : 0.0;
  const Calibration cal = cal_.update(R.los_tap, L.found, los_snr);
  const Grid G = envelope_grid(cfg_.volume, a);
  std::vector<Detection> D;
  std::vector<float> E;   // CPU path only; GPU path's equivalent is cuda_->last_envelope() (see below)
  // Task 10: on GPU, cuda_->detect() runs the envelope kernel on the device and then calls the SAME
  // unmodified CPU detect() (branch-and-bound channel choice, per-candidate waveform fit, greedy
  // residual pursuit, NMS/harmonic merge) on the result -- that stage is small-N and sequential, not
  // embarrassingly parallel, and the amended brief's own method list keeps it on the host. tm[2]/tm[3]
  // split the combined call back into its GPU-envelope and CPU-detect() shares for the report.
  if (cuda_) {
    D = cuda_->detect(G, geo, detect_params(a, G, cfg_.false_object_intensity_per_s), nullptr);
    const double total = ms_since(s0); s0 = clk::now();
    tm[2] = cuda_->last_timing().envelope_ms;
    tm[3] = std::max(0.0, total - tm[2]);
  } else {
    E = envelope(R, G, geo);
    tm[2] = ms_since(s0); s0 = clk::now();
    D = detect(E, R, G, geo, detect_params(a, G, cfg_.false_object_intensity_per_s));
    tm[3] = ms_since(s0); s0 = clk::now();
  }
  if (cuda_) cuda_->refine(D, G, geo, cal, survey);
  else for (Detection& d : D) refine(d, R, G, geo, cal, survey);
  tm[4] = ms_since(s0); s0 = clk::now();
  if (cfg_.ul_enable)                          // UL illuminators (built, off by default)
    for (const CfrWindow& u : j.ul) {
      if (!u.valid()) continue;
      const Axes au = derive_axes(u, cfg_.volume, geo, cfg_.max_speed_mps); if (!au.valid) continue;
      const UeFix f = localise_ue(u, au, geo, cfg_.volume, pfa_los); if (!f.valid) continue;
      const Geometry ug = ue_geometry(geo, f);
      const LosEstimate Lu = find_los(u, au, pfa_los);
      const RdResult Ru = range_doppler(u, au, Lu, estimate_row_sync(u, au, Lu));
      const Grid Gu = envelope_grid(cfg_.volume, au);
      std::vector<Detection> Du = detect(envelope(Ru, Gu, ug), Ru, Gu, ug, detect_params(au, Gu, cfg_.false_object_intensity_per_s));
      SurveySigma us = survey; us.tx_m = std::hypot(f.sigma_m, cfg_.survey_sigma_m);
      for (Detection& d : Du) {
        refine(d, Ru, Gu, ug, cal, us);
        d.illuminator = u.session_id;
        // Illuminator position uncertainty adds (independently) to the position covariance.
        for (int k = 0; k < 3; ++k) d.pos_cov[k * 4] += f.sigma_m * f.sigma_m;
        d.pos_sigma = {std::sqrt(d.pos_cov[0]), std::sqrt(d.pos_cov[4]), std::sqrt(d.pos_cov[8])};
        D.push_back(d);
      }
    }
  tm[6] = ms_since(s0); s0 = clk::now();
  std::vector<int> assoc;
  const std::vector<Track>& T = tracker_->step(j.t, a.t_cpi_s, D, &assoc);
  for (size_t k = 0; k < D.size(); ++k)
    if (assoc[k] >= 0 && T[(size_t)assoc[k]].confirmed && D[k].illuminator == 0) af_->add(D[k], geo);
  tm[5] = ms_since(s0);
  const double total = ms_since(t0);
  const bool overrun = total > a.t_cpi_s * 1e3;
  { std::lock_guard<std::mutex> l(mu_); ++st_.processed; st_.last_ms = total; if (overrun) ++st_.overruns; }
  const CoherentStats st = stats();

  std::ostringstream rep;
  rep << "{\"cpi\":" << j.seq << ",\"t\":" << jnum(j.t) << ",\"t_cpi_s\":" << jnum(a.t_cpi_s) << ",\"b_eff_hz\":" << jnum(a.b_eff_hz)
      << ",\"range_res_m\":" << jnum(kC / a.b_eff_hz) << ",\"grid_step_m\":" << jnum(G.step) << ",\"n_voxels\":" << G.size()
      << ",\"n_dopp_tested\":" << a.tested_dopp.size() << ",\"rows\":" << j.dl.rows << ",\"gpu\":" << (cuda_ ? "true" : "false")
      << ",\"lambda_m\":" << jnum(a.lambda_m) << ",\"dopp_step_hz\":" << jnum(a.dopp_step_hz) << ",\"notch_half_bins\":" << a.notch_half_bins
      << ",\"static_db\":{\"los\":[" << jnum(st_los[0]) << "," << jnum(st_los[1]) << "," << jnum(st_los[2]) << "," << jnum(st_los[3])
      << "],\"edge\":[" << jnum(st_edge[0]) << "," << jnum(st_edge[1]) << "," << jnum(st_edge[2]) << "," << jnum(st_edge[3])
      << "],\"far\":[" << jnum(st_far[0]) << "," << jnum(st_far[1]) << "," << jnum(st_far[2]) << "," << jnum(st_far[3]) << "]}"
      << ",\"timing_ms\":{\"sync\":" << jnum(tm[0]) << ",\"los\":" << jnum(tm_los) << ",\"rd\":" << jnum(tm[1]) << ",\"env\":" << jnum(tm[2]) << ",\"detect\":" << jnum(tm[3])
      << ",\"refine\":" << jnum(tm[4]) << ",\"ul\":" << jnum(tm[6]) << ",\"track\":" << jnum(tm[5]) << ",\"total\":" << jnum(total) << "}"
      << ",\"overrun\":" << (overrun ? "true" : "false")
      << ",\"stats\":{\"processed\":" << st.processed << ",\"skipped\":" << st.skipped << ",\"overruns\":" << st.overruns
      << ",\"queue_waits\":" << st.queue_waits << "}"
      << ",\"skipped_reason\":null,\"detections\":[";
  for (size_t k = 0; k < D.size(); ++k)
    rep << (k ? "," : "") << "{\"p\":" << jvec(D[k].pos) << ",\"s\":" << jvec(D[k].pos_sigma) << ",\"cov\":" << jarr(D[k].pos_cov)
        << ",\"rr\":" << jnum(D[k].range_rate_mps) << ",\"rr_s\":" << jnum(D[k].range_rate_sigma) << ",\"snr\":" << jnum(D[k].snr)
        << ",\"fd\":" << jnum(D[k].doppler_hz) << ",\"ill\":" << D[k].illuminator << "}";
  rep << "]";
  if (std::chrono::duration<double>(clk::now() - last_image_).count() >= cfg_.monitor_period_s) {
    last_image_ = clk::now();
    const size_t nxy = (size_t)G.nx * G.ny;
    std::vector<double> top(nxy, 0.0);   // max over z and tested Doppler, y-major [iy][ix]
    // GPU path: envelope() never ran on the host, but CudaCoherent::detect() cached the same E it
    // downloaded for the CPU detect() call (same [t][voxel] layout) -- read that instead of
    // recomputing on the device.
    const std::vector<float>& Eref = cuda_ ? cuda_->last_envelope() : E;
    if (Eref.size() == a.tested_dopp.size() * G.size())
      for (size_t t = 0; t < a.tested_dopp.size(); ++t)
        for (size_t v = 0; v < G.size(); ++v) top[v % nxy] = std::max(top[v % nxy], (double)Eref[t * G.size() + v]);
    rep << ",\"topview\":{\"nx\":" << G.nx << ",\"ny\":" << G.ny << ",\"x0\":" << jnum(G.origin.x) << ",\"y0\":" << jnum(G.origin.y)
        << ",\"step\":" << jnum(G.step) << ",\"db\":" << jarr(to_db(top)) << "},\"rd\":[";
    for (uint32_t i = 0; i < kCh; ++i) {
      std::vector<double> p((size_t)a.n_range * a.n_dopp);   // range-major [m][d]
      for (uint32_t m = 0; m < a.n_range; ++m)
        for (uint32_t d = 0; d < a.n_dopp; ++d) p[(size_t)m * a.n_dopp + d] = std::norm(R.rd.v[R.rd.idx(i, m, d)]);
      rep << (i ? "," : "") << "{\"ch\":" << i << ",\"nr\":" << a.n_range << ",\"nd\":" << a.n_dopp
          << ",\"range_m_per_bin\":" << jnum(kC * a.delay_step_s) << ",\"dopp0_hz\":" << jnum(a.dopp0_hz)
          << ",\"dopp_step_hz\":" << jnum(a.dopp_step_hz) << ",\"db\":" << jarr(to_db(p)) << "}";
    }
    rep << "]";
  } else rep << ",\"topview\":null,\"rd\":null";
  rep << "}";
  reports_.write_line(rep.str());

  std::ostringstream tr;
  tr << "{\"cpi\":" << j.seq << ",\"t\":" << jnum(j.t) << ",\"tracks\":[";
  for (size_t k = 0; k < T.size(); ++k) {
    const Track& x = T[k];
    tr << (k ? "," : "") << "{\"id\":" << x.id << ",\"p\":[" << jnum(x.x[0]) << "," << jnum(x.x[1]) << "," << jnum(x.x[2])
       << "],\"v\":[" << jnum(x.x[3]) << "," << jnum(x.x[4]) << "," << jnum(x.x[5]) << "],\"s\":[" << jnum(std::sqrt(x.P[0]))
       << "," << jnum(std::sqrt(x.P[7])) << "," << jnum(std::sqrt(x.P[14])) << "],\"pe\":" << jnum(1 / (1 + std::exp(-x.llr)))
       << ",\"hits\":" << x.hits << ",\"age\":" << jnum(x.age_s) << ",\"confirmed\":" << (x.confirmed ? "true" : "false") << "}";
  }
  tr << "]}";
  tracks_.write_line(tr.str());
  write_coherence(j.seq, j.t, cal, L.delay_s, false);
}

} // namespace nr_isac::coherent
