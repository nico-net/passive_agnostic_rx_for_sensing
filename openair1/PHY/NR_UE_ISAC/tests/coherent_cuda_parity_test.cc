// openair1/PHY/NR_UE_ISAC/tests/coherent_cuda_parity_test.cc
// Task 10: GPU (CudaCoherent) vs CPU (coherent_core.cc) parity on the Task 5 focus-test scene.
// SKIPs (exit 0) when this binary was not built with ENABLE_CHANNEL_SIM_CUDA or no device is present.
#include "coherent_core.h"
#include "coherent_cuda.h"
#include "coherent_cuda_detect.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;

// Path/make_window: copied verbatim from coherent_core_test.cc (kept in sync by hand -- see that
// file's own header comment; this is the same scene Task 5's focus test uses).
struct Path { std::array<double, kCh> tau; double amp; double doppler_hz; };
static CfrWindow make_window(uint32_t rows, const std::vector<Path>& paths, std::array<double, kCh> ph,
                             double cfo_hz, uint32_t nprb_row, uint32_t seed, uint32_t comb = 1, double sfo_ppm = 0)
{
  CfrWindow w; w.antennas = 4; w.rows = rows; w.subcarriers = 273 * 12; w.scs_hz = 30000; w.fc_hz = 3.45e9; w.pci = 2;
  w.values.assign((size_t)4 * rows * w.subcarriers, {0, 0}); w.observed.assign((size_t)rows * w.subcarriers, 0);
  std::mt19937 rng(seed), hop(seed + 1000); std::normal_distribution<double> n01(0, 1);
  const double slot = slot_duration_s(w.scs_hz);
  for (uint32_t r = 0; r < rows; ++r) {
    w.row_time_slots.push_back(r * 2.0);
    w.row_slot_idx.push_back(r * 2); w.row_slot_frac.push_back(0); w.row_source_mask.push_back(1u << 3);
    const double t = r * 2.0 * slot;
    const uint32_t start = hop() % (273 - nprb_row + 1);
    for (uint32_t k = start * 12; k < (start + nprb_row) * 12; k += comb) {
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

// Detection-set parity (gpu-common.md): same count; every GPU detection has a CPU one within one
// envelope step with SNR within 1 %. Returns the max position / relative-SNR mismatch for the log.
static bool same_detections(const std::vector<coherent::Detection>& Dc, const std::vector<coherent::Detection>& Dg, double step,
                            double* max_dpos, double* max_dsnr)
{
  *max_dpos = 0; *max_dsnr = 0;
  if (Dc.size() != Dg.size()) return false;
  for (const coherent::Detection& x : Dg) {
    const coherent::Detection* best = nullptr;
    for (const coherent::Detection& y : Dc) if (!best || dist(x.pos_env, y.pos_env) < dist(x.pos_env, best->pos_env)) best = &y;
    if (!best) return false;
    const double dp = dist(x.pos_env, best->pos_env), ds = std::abs(x.snr - best->snr) / std::max(best->snr, 1e-12);
    *max_dpos = std::max(*max_dpos, dp); *max_dsnr = std::max(*max_dsnr, ds);
    if (!(dp <= step && ds < 0.01)) return false;
  }
  return true;
}

// Recorded full-band OTA detect() inputs (NR_ISAC_DETECT_DUMP from a replay): CPU oracle vs GpuDetect,
// parity per case and timing percentiles. Run when NR_ISAC_DETECT_CASES=<dir> is set.
static void replay_cases(const char* dir)
{
  std::vector<std::string> files;
  if (DIR* d = opendir(dir)) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.rfind("case_", 0) == 0) files.push_back(std::string(dir) + "/" + n); } closedir(d); }
  std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });
  const char* lim = std::getenv("NR_ISAC_DETECT_CASES_MAX");
  if (lim && files.size() > std::strtoul(lim, nullptr, 10)) files.resize(std::strtoul(lim, nullptr, 10));
  GpuDetect gd; std::vector<double> tc, tg; size_t bad = 0, ndet = 0;
  const bool skip_cpu = std::getenv("NR_ISAC_DETECT_CASES_NOCPU") != nullptr;
  for (const std::string& f : files) {
    DetectCase k; require(load_detect_case(f, &k), "load case");
    using C = std::chrono::steady_clock;
    auto t0 = C::now();
    const std::vector<coherent::Detection> Dg = gd.run(k.E, k.R, k.g, k.geo, k.p);
    tg.push_back(std::chrono::duration<double, std::milli>(C::now() - t0).count());
    std::vector<coherent::Detection> Dc;
    if (!skip_cpu) { t0 = C::now(); Dc = detect(k.E, k.R, k.g, k.geo, k.p); tc.push_back(std::chrono::duration<double, std::milli>(C::now() - t0).count()); }
    double dp = 0, ds = 0; const bool ok = skip_cpu || same_detections(Dc, Dg, k.g.step, &dp, &ds);
    bad += !ok; ndet += Dg.size();
    if (!ok && std::getenv("NR_ISAC_DETECT_CASES_VERBOSE")) {
      for (const coherent::Detection& x : Dc) std::printf("  cpu pos_env=(%.3f %.3f %.3f) snr=%.4g dopp=%u\n", x.pos_env.x, x.pos_env.y, x.pos_env.z, x.snr, x.dopp_bin);
      for (const coherent::Detection& x : Dg) std::printf("  gpu pos_env=(%.3f %.3f %.3f) snr=%.4g dopp=%u\n", x.pos_env.x, x.pos_env.y, x.pos_env.z, x.snr, x.dopp_bin);
    }
    std::printf("%s: cpu=%zu gpu=%zu %s dpos=%.3g dsnr=%.3g cpu_ms=%.1f gpu_ms=%.1f\n", f.c_str(), Dc.size(), Dg.size(), ok ? "OK" : "MISMATCH",
                dp, ds, tc.empty() ? 0.0 : tc.back(), tg.back());
  }
  auto pct = [](std::vector<double> v, double q) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5))]; };
  std::printf("cases=%zu detections=%zu mismatches=%zu  cpu p50=%.1f p95=%.1f max=%.1f ms  gpu p50=%.1f p95=%.1f max=%.1f ms\n", files.size(), ndet, bad,
              pct(tc, .5), pct(tc, .95), pct(tc, 1), pct(tg, .5), pct(tg, .95), pct(tg, 1));
  require(bad == 0, "recorded-case detect parity");
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (!CudaCoherent::available()) { std::puts("coherent_cuda_parity_test: SKIP (no CUDA)"); return 0; }
  if (const char* dir = std::getenv("NR_ISAC_DETECT_CASES")) { replay_cases(dir); std::puts("coherent_cuda_parity_test: PASS"); return 0; }

  Geometry g; g.tx = {35, 20, 6};
  g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol;
  const std::array<double, kCh> ph{0, 1.1, -2.0, 0.4};
  const Vec3 t1{5, 12, 1.2}, t2{-6, -4, 1.0}, drone{8, 8, 15};
  auto taus = [&](const Vec3& p) { std::array<double, kCh> t{}; for (uint32_t i = 0; i < 4; ++i) t[i] = (dist(p, g.tx) + dist(p, g.rx[i]) + 30) / kC; return t; };
  std::array<double, kCh> los{}; for (uint32_t i = 0; i < 4; ++i) los[i] = (dist(g.tx, g.rx[i]) + 30) / kC;
  CfrWindow w = make_window(64, {{los, 1.0, 0}, {taus(t1), 0.2, 45}, {taus(t2), 0.25, -110}, {taus(drone), 0.15, 70}}, ph, 0, 128, 3);

  coherent::Axes a = derive_axes(w, vol, g, 20.0); require(a.valid, "axes");
  const LosEstimate L = find_los(w, a, 1e-4);
  const RowSync s = estimate_row_sync(w, a, L);
  const RdResult R = range_doppler(w, a, L, s);
  Calibrator cal; Calibration c{}; for (int k = 0; k < 5; ++k) c = cal.update(R.los_tap, L.found, L.snr);
  const Grid G = envelope_grid(vol, a);
  const SurveySigma survey{{0.1, 0.1, 0.1, 0.1}, 0.1};

  CudaCoherent gpu;
  const RdResult Rg = gpu.range_doppler(w, a, L, s, true);
  double m = 0, dm = 0;
  require(R.rd.v.size() == Rg.rd.v.size(), "RD size match");
  for (size_t k = 0; k < R.rd.v.size(); ++k) { m = std::max(m, (double)std::abs(R.rd.v[k])); dm = std::max(dm, (double)std::abs(R.rd.v[k] - Rg.rd.v[k])); }
  std::printf("RD parity: max|cpu|=%.6g max|diff|=%.6g rel=%.3g\n", m, dm, m > 0 ? dm / m : 0.0);
  require(m > 0 && dm / m < 1e-3, "RD parity");
  { double wm = 0, wd = 0;                         // GPU FP32 waveform kernel tables vs the CPU closed form
    require(R.wf.B.size() == Rg.wf.B.size(), "wf group count");
    for (size_t q = 0; q < R.wf.B.size(); ++q) for (size_t u = 0; u < R.wf.B[q].size(); ++u) {
      wm = std::max(wm, std::abs(R.wf.B[q][u])); wd = std::max({wd, std::abs(R.wf.B[q][u] - Rg.wf.B[q][u]), std::abs(R.wf.B2[q][u] - Rg.wf.B2[q][u])}); }
    std::printf("wf parity: groups=%zu max|B|=%.3g max|diff|=%.3g\n", R.wf.B.size(), wm, wd);
    require(wm > 0 && wd / wm < 1e-4, "waveform kernel parity"); }

  const std::vector<coherent::Detection> Dc = detect(envelope(R, G, g), R, G, g, detect_params(a, G, 1.0));
  const std::vector<coherent::Detection> Dg = gpu.detect(G, g, detect_params(a, G, 1.0), nullptr);
  std::printf("detections: cpu=%zu gpu=%zu\n", Dc.size(), Dg.size());
  {   // GpuDetect alone on the CPU chain's own inputs (isolates detect() from range_doppler/envelope)
    GpuDetect gd; const std::vector<float> E = envelope(R, G, g);
    const std::vector<coherent::Detection> D2 = gd.run(E, R, G, g, detect_params(a, G, 1.0));
    double dp = 0, ds = 0; const bool ok = same_detections(Dc, D2, G.step, &dp, &ds);
    std::printf("GpuDetect on CPU inputs: %zu detections, max dpos=%.3g dsnr=%.3g\n", D2.size(), dp, ds);
    require(ok, "GpuDetect parity on CPU inputs");
  }
  require(Dc.size() == Dg.size(), "same detection count");
  for (const coherent::Detection& x : Dg) {
    bool ok = false;
    for (const coherent::Detection& y : Dc) ok |= dist(x.pos_env, y.pos_env) <= G.step && std::abs(x.snr - y.snr) / std::max(y.snr, 1e-12) < 0.01;
    require(ok, "detection parity");
  }

  std::vector<coherent::Detection> Dc2 = Dc, Dg2 = Dg;
  for (coherent::Detection& d : Dc2) refine(d, R, G, g, c, survey);
  gpu.refine(Dg2, G, g, c, survey);
  Vec3 centroid{}; for (const Vec3& r : g.rx) centroid = centroid + r * 0.25;
  double D = 0; for (uint32_t i = 0; i < 4; ++i) for (uint32_t j = i + 1; j < 4; ++j) D = std::max(D, dist(g.rx[i], g.rx[j]));
  for (const coherent::Detection& xg : Dg2) {
    const coherent::Detection* xc = nullptr;
    for (const coherent::Detection& y : Dc2) if (dist(y.pos_env, xg.pos_env) <= G.step) xc = &y;
    require(xc != nullptr, "refine pairing");
    const double fringe = a.lambda_m * std::max(dist(xg.pos_env, centroid), G.step) / (2 * D);
    require(dist(xg.pos, xc->pos) <= fringe / 2 + 1e-9, "refine parity");
  }

  const CudaCoherent::Timing t = gpu.last_timing();
  std::printf("gpu timing (ms): upload=%.2f build=%.2f fft=%.2f crop_norm=%.2f nudft=%.2f download=%.2f wf=%.2f rd_total=%.2f envelope=%.2f\n",
              t.upload_ms, t.build_ms, t.fft_ms, t.crop_norm_ms, t.nudft_ms, t.download_ms, t.wf_ms, t.rd_total_ms, t.envelope_ms);
  std::puts("coherent_cuda_parity_test: PASS");
}
