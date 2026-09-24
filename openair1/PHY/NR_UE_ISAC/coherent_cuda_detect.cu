/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* GPU-resident coherent detect() -- see coherent_cuda_detect.h. Every formula is a transcription of
 * coherent_core.cc's detect() and its helpers (Response, fit_channel, static_term, leak_amp, choose,
 * walk, refit_all), which stay the CPU oracle. The small pure helpers are duplicated here because
 * coherent_core.cc keeps them in an anonymous namespace. */
#include "coherent_cuda_detect.h"

#include <cuda_runtime.h>
#include <cufft.h>
#include <cooperative_groups.h>
#include <cstddef>
#include <cub/device/device_segmented_sort.cuh>
#include <cub/device/device_select.cuh>
#include <cub/iterator/counting_input_iterator.cuh>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "fft.h"
#include "robust_stats.h"

namespace nr_isac::coherent {
namespace {
namespace cg = cooperative_groups;

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
void cuda_check(cudaError_t s, const char* op) { if (s != cudaSuccess) throw std::runtime_error(std::string(op) + ": " + cudaGetErrorString(s)); }

// ---- host duplicates of coherent_core.cc's anonymous-namespace helpers (kept identical) ----
Vec3 unit(const Vec3& v) { const double n = norm(v); return n > 0 ? v / n : Vec3{}; }
uint32_t n_used(const RdResult& R) { uint32_t n = 0; for (bool f : R.los_found) n += f; return n; }
uint32_t dopp_half(const Axes& a, const Geometry& geo, const std::array<bool, kCh>& used, const Vec3& x)
{
  if (!(a.v_max_mps > 0) || !(a.dopp_step_hz > 0)) return 0;
  Vec3 u[kCh]; for (uint32_t i = 0; i < kCh; ++i) u[i] = unit(x - geo.rx[i]);
  double m = 0;
  for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j) if (used[i] && used[j]) m = std::max(m, norm(u[i] - u[j]));
  return (uint32_t)std::ceil(a.v_max_mps * m / (a.lambda_m * a.dopp_step_hz) - 1e-9);
}
bool dopp_ok(const Axes& a, long d)
{
  return d >= 0 && d < (long)a.n_dopp && std::abs(a.dopp0_hz + d * a.dopp_step_hz) > a.notch_half_bins * a.dopp_step_hz;
}
cd kernel_at(const std::vector<cd>& B, long X, double x)
{
  const double u = (x + X) * RdResult::Waveform::kOvs;
  if (!(u >= 0) || u >= (double)(B.size() - 1)) return 0.0;
  const size_t u0 = (size_t)u; const double t = u - u0;
  return B[u0] * (1 - t) + B[u0 + 1] * t;
}
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
double cubic4(double pm, double p0, double p1, double p2, double t)
{
  return -pm * t * (t - 1) * (t - 2) / 6 + p0 * (t + 1) * (t - 1) * (t - 2) / 2
         - p1 * (t + 1) * t * (t - 2) / 2 + p2 * (t + 1) * t * (t - 1) / 6;
}
std::vector<double> v3(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 grad(const Geometry& geo, uint32_t i, const Vec3& x) { return unit(x - geo.tx) + unit(x - geo.rx[i]); }
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
struct Response {
  const RdResult& R; const Axes& a;
  std::vector<uint32_t> rows;
  std::vector<cd> ed, em;
  double tbar = 0;
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
  struct Ph { std::vector<cd> pp; std::vector<double> pr; double p = 0, fb = 0; };
  Ph phasors(double p, double fb) const
  {
    Ph h; h.pp.resize(rows.size()); h.pr.resize(rows.size()); h.p = p; h.fb = fb;
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
struct ChanFit {
  double p = 0, fb = 0, sp = 0, sf = 0, sA = 0, snr = 0; cd A = 0; bool ok = false;
  std::vector<Response::Ph> grid;
  std::vector<cd> M;
  std::vector<std::vector<cd>> stat;
};
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
cd path_value(const Response& rs, const ChanFit& f, uint32_t m, uint32_t d)
{
  if (!f.ok) return 0.0;
  return f.A * (rs.at(m, d, f.grid[0]) + static_cell(rs.R, f, m, d));
}
thread_local uint64_t g_j_evals = 0;
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
    ++g_j_evals;
    std::vector<cd> ac; model(p, fb, &ac);
    cd num = 0; double den = 0;
    for (size_t c = 0; c < cells.size(); ++c) { num += y[c] * std::conj(ac[c]); den += std::norm(ac[c]); }
    if (A) *A = den > 0 ? num / den : cd(0);
    return den > 0 ? std::norm(num) / den : 0.0;
  };
  double p = p0, fb = fb0;
  auto search = [&](double st) {
    double best = J(p, fb, nullptr);
    for (; st >= 0.05;) {
      double bp = p, bf = fb;
      for (int u = -1; u <= 1; ++u) for (int v = -1; v <= 1; ++v) if (u || v) {
        const double j = J(p + u * st, fb + v * st, nullptr); if (j > best) { best = j; bp = p + u * st; bf = fb + v * st; }
      }
      if (bp == p && bf == fb) st /= 2; else { p = bp; fb = bf; }
    }
    for (int it = 0; it < 20; ++it) {
      const double h = 1e-3, j0 = best;
      const double jpp = J(p + h, fb, nullptr), jpm = J(p - h, fb, nullptr), jfp = J(p, fb + h, nullptr), jfm = J(p, fb - h, nullptr);
      const double jpf = J(p + h, fb + h, nullptr), jmm = J(p - h, fb - h, nullptr);
      const double gp = (jpp - jpm) / (2 * h), gf = (jfp - jfm) / (2 * h);
      const double hpp = (jpp - 2 * j0 + jpm) / (h * h), hff = (jfp - 2 * j0 + jfm) / (h * h);
      const double hpf = (jpf + jmm - jpp - jpm - jfp - jfm + 2 * j0) / (2 * h * h);
      const double det = hpp * hff - hpf * hpf;
      if (!(hpp < 0 && det > 0)) break;
      double dp = -(hff * gp - hpf * gf) / det, df = -(hpp * gf - hpf * gp) / det;
      double jn = J(p + dp, fb + df, nullptr);
      for (int k = 0; k < 20 && !(jn >= j0); ++k) { dp /= 2; df /= 2; jn = J(p + dp, fb + df, nullptr); }
      if (!(jn >= j0)) break;
      p += dp; fb += df; best = jn;
      if (std::hypot(dp, df) < 1e-5) break;
    }
  };
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
      y[c] = cells[c].y + A * sv;
    }
    search(0.0);
  }
  f.p = p; f.fb = fb; J(p, fb, &f.A);
  const size_t N = cells.size(); const double h = 1e-3;
  std::vector<cd> a0, ap, am, fp, fm;
  model(p, fb, &a0); model(p + h, fb, &ap); model(p - h, fb, &am); model(p, fb + h, &fp); model(p, fb - h, &fm);
  std::vector<std::array<cd, 4>> Dc(N);
  for (size_t c = 0; c < N; ++c)
    Dc[c] = {f.A * (ap[c] - am[c]) / (2 * h), f.A * (fp[c] - fm[c]) / (2 * h), a0[c], cd(0, 1) * a0[c]};
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
  double rr = 0; for (size_t c = 0; c < N; ++c) rr += std::norm(y[c] - f.A * a0[c]);
  f.snr = std::norm(f.A) / std::max(R.noise[ch], N > 2 ? rr / (N - 2) : 0.0);
  const Matrix Ai = inverse(DtD), cov = Ai * DCD * Ai;
  f.sp = std::sqrt(std::max(0.0, cov(0, 0))); f.sf = std::sqrt(std::max(0.0, cov(1, 1)));
  f.sA = std::sqrt(std::max(0.0, cov(2, 2) + cov(3, 3)));
  std::vector<double> ps{p}, fs{fb};
  if (z * f.sp > 1e-6) { ps.push_back(p - z * f.sp); ps.push_back(p + z * f.sp); }
  if (z * f.sf > 1e-6) { fs.push_back(fb - z * f.sf); fs.push_back(fb + z * f.sf); }
  for (double pv : ps) for (double fv : fs) f.grid.push_back(rs.phasors(pv, fv));
  set_M(fb);
  f.stat.resize(a.n_dopp);
  f.ok = true;
  return f;
}
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


// ================================ device side ================================
struct DAxes {
  double fc_hz, lambda_m, scs_hz, delay_step_s, dopp_step_hz, dopp0_hz, v_max_mps;
  uint32_t n_range, n_dopp, notch_half_bins, n_fft;
  const uint8_t* ok;   // dopp_ok() per Doppler bin, evaluated on the host exactly as the oracle rounds it
};
struct DGeo { double tx[3]; double rx[kCh][3]; uint8_t used[kCh]; uint32_t n_used; double noise[kCh]; };
struct DGrid { double o[3]; double step; uint32_t nx, ny, nz; };

DAxes to_daxes(const Axes& a)
{
  return DAxes{a.fc_hz, a.lambda_m, a.scs_hz, a.delay_step_s, a.dopp_step_hz, a.dopp0_hz, a.v_max_mps,
               a.n_range, a.n_dopp, a.notch_half_bins, a.n_fft, nullptr};
}
DGeo to_dgeo(const Geometry& geo, const RdResult& R)
{
  DGeo g{};
  g.tx[0] = geo.tx.x; g.tx[1] = geo.tx.y; g.tx[2] = geo.tx.z;
  for (uint32_t i = 0; i < kCh; ++i) {
    g.rx[i][0] = geo.rx[i].x; g.rx[i][1] = geo.rx[i].y; g.rx[i][2] = geo.rx[i].z;
    g.used[i] = R.los_found[i]; g.n_used += R.los_found[i]; g.noise[i] = R.noise[i];
  }
  return g;
}
DGrid to_dgrid(const Grid& g) { return DGrid{{g.origin.x, g.origin.y, g.origin.z}, g.step, g.nx, g.ny, g.nz}; }

__device__ inline void d_voxel(const DGrid& g, size_t v, double x[3])
{
  const size_t ix = v % g.nx, iy = (v / g.nx) % g.ny, iz = v / ((size_t)g.nx * g.ny);
  x[0] = g.o[0] + ix * g.step; x[1] = g.o[1] + iy * g.step; x[2] = g.o[2] + iz * g.step;
}
__device__ inline double d_dist(const double a[3], const double b[3])
{
  const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return sqrt(dx * dx + dy * dy + dz * dz);
}
__device__ inline void d_unit(const double a[3], const double b[3], double u[3])  // unit(a - b)
{
  const double v[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}, n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int k = 0; k < 3; ++k) u[k] = n > 0 ? v[k] / n : 0.0;
}
__device__ inline double d_bin(const DGeo& g, const DAxes& a, const double x[3], uint32_t i)
{
  return (d_dist(x, g.tx) + d_dist(x, g.rx[i]) - d_dist(g.tx, g.rx[i])) / kC / a.delay_step_s;
}
__device__ inline bool d_dopp_ok(const DAxes& a, long d) { return d >= 0 && d < (long)a.n_dopp && a.ok[d]; }
__device__ uint32_t d_dopp_half(const DAxes& a, const DGeo& g, const double x[3])
{
  if (!(a.v_max_mps > 0) || !(a.dopp_step_hz > 0)) return 0;
  double u[kCh][3]; for (uint32_t i = 0; i < kCh; ++i) d_unit(x, g.rx[i], u[i]);
  double m = 0;
  for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j) if (g.used[i] && g.used[j]) {
    const double dx = u[i][0] - u[j][0], dy = u[i][1] - u[j][1], dz = u[i][2] - u[j][2];
    m = fmax(m, sqrt(dx * dx + dy * dy + dz * dz));
  }
  return (uint32_t)ceil(a.v_max_mps * m / (a.lambda_m * a.dopp_step_hz) - 1e-9);
}
// Cubic magnitude interpolation (coherent_core.cc's MagInterp) over a float |RD| table [ch][m][d].
struct DMag {
  uint32_t b[4]; double c[4]; bool ok;
  __device__ DMag(uint32_t n, double bin) : ok(false)
  {
    if (n == 0 || !(bin >= 0) || bin > (double)(n - 1)) return;
    const long b0 = (long)bin; const double t = bin - b0;
    for (int j = 0; j < 4; ++j) { long q = b0 - 1 + j; b[j] = (uint32_t)(q < 0 ? 0 : (q > (long)n - 1 ? (long)n - 1 : q)); }
    c[0] = -t * (t - 1) * (t - 2) / 6; c[1] = (t + 1) * (t - 1) * (t - 2) / 2;
    c[2] = -(t + 1) * t * (t - 2) / 2; c[3] = (t + 1) * t * (t - 1) / 6;
    ok = true;
  }
  __device__ double at(const float* mag, uint32_t nr, uint32_t nd, uint32_t ch, uint32_t d) const
  {
    double m = 0; for (int j = 0; j < 4; ++j) m += c[j] * (double)mag[((size_t)ch * nr + b[j]) * nd + d];
    return fmax(0.0, m);
  }
};

// Per CPI: the RD cube as float (= the host RdResult's cf values) and its magnitude |.| as float the
// way std::abs(complex<float>) computes it (glibc hypotf: the double hypotenuse rounded once).
__global__ void k_rd_float(const double2* rd, size_t n, float2* rdf, float* mag)
{
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < n; j += (size_t)blockDim.x * gridDim.x) {
    const float x = (float)rd[j].x, y = (float)rd[j].y;
    rdf[j] = make_float2(x, y);
    mag[j] = (float)sqrt((double)x * x + (double)y * y);
  }
}
__global__ void k_mag(const float2* rdf, size_t n, float* mag)
{
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < n; j += (size_t)blockDim.x * gridDim.x) {
    const float x = rdf[j].x, y = rdf[j].y;
    mag[j] = (float)sqrt((double)x * x + (double)y * y);
  }
}
__global__ void k_dh(DAxes a, DGeo g, DGrid G, size_t nv, uint32_t* dh)
{
  for (size_t v = (size_t)blockIdx.x * blockDim.x + threadIdx.x; v < nv; v += (size_t)blockDim.x * gridDim.x) {
    double x[3]; d_voxel(G, v, x); dh[v] = d_dopp_half(a, g, x);
  }
}
__device__ inline uint32_t d_m_of(const DAxes& a, const uint32_t* okc, long d, uint32_t dh)
{
  const long lo = d - (long)dh < 0 ? 0 : d - (long)dh, hi = d + (long)dh > (long)a.n_dopp - 1 ? (long)a.n_dopp - 1 : d + (long)dh;
  const uint32_t m = okc[hi + 1] - okc[lo];
  return m < 1 ? 1 : m;
}
__global__ void k_zz(DAxes a, const float* E, const uint32_t* tested, const uint32_t* okc, const uint32_t* dh, const double* nmed,
                     size_t nt, size_t nv, double* zz)
{
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < nt * nv; j += (size_t)blockDim.x * gridDim.x) {
    const size_t t = j / nv, v = j % nv;
    zz[j] = (double)E[j] / nmed[d_m_of(a, okc, tested[t], dh[v])];
  }
}
__global__ void k_median(const double* sorted, size_t nt, size_t nv, double* scale)
{
  const size_t t = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (t >= nt) return;
  const double* s = sorted + t * nv; const size_t up = nv / 2;
  scale[t] = (nv & 1) ? s[up] : 0.5 * (s[up - 1] + s[up]);
}
__global__ void k_cand(DAxes a, DGrid G, const float* E, const uint32_t* tested, const uint32_t* okc, const uint32_t* dh,
                       const double* nthr, const double* scale, size_t nt, size_t nv, uint8_t* flag)
{
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < nt * nv; j += (size_t)blockDim.x * gridDim.x) {
    const size_t t = j / nv, v = j % nv;
    uint8_t f = 0;
    const double sc = scale[t];
    if (sc > 0) {
      const double thr = sc * nthr[d_m_of(a, okc, tested[t], dh[v])];
      const float ev = E[j];
      const long NX = G.nx, NY = G.ny, NZ = G.nz;
      const long ix = v % NX, iy = (v / NX) % NY, iz = v / (NX * NY);
      if (ev > thr && G.o[2] + iz * G.step >= 0) {           // x.z < 0: ground-bounce mirror, skipped by detect()
        bool peak = true;
        for (long dt = -1; dt <= 1 && peak; ++dt) {
          const long tt = (long)t + dt;
          if (dt && (tt < 0 || tt >= (long)nt || labs((long)tested[tt] - (long)tested[t]) != 1)) continue;
          const float* En = E + (size_t)tt * nv;
          for (long dz = -1; dz <= 1 && peak; ++dz) for (long dy = -1; dy <= 1 && peak; ++dy) for (long dx = -1; dx <= 1; ++dx) {
            const long x = ix + dx, y = iy + dy, z = iz + dz;
            if ((dt || dx || dy || dz) && x >= 0 && x < NX && y >= 0 && y < NY && z >= 0 && z < NZ && En[(z * NY + y) * NX + x] > ev) { peak = false; break; }
          }
        }
        f = peak;
      }
    }
    flag[j] = f;
  }
}

// One candidate's state (detect()'s Acc) plus its choose() result.
struct DAcc { double x[3]; uint32_t d[kCh]; double pw[kCh], bin[kCh]; };
struct DPk { double v, raw; uint32_t d; };

// coherent_core.cc's choose(): per channel the local Doppler maxima of the (resid ? residual : raw)
// column inside +-dhv of d, sorted (stable insertion sort = std::sort's for <= 16 peaks), pruned by the
// bound, then the exact branch-and-bound DFS over velocity-consistent combinations. pk: this thread's
// scratch, kCh * W entries. Returns ec, or -1 (me.d/me.pw untouched then; me.bin always set).
__device__ double d_choose(const DAxes& a, const DGeo& g, DAcc& me, long d, uint32_t dhv, double thr,
                           const float* mag, const float* magc, DPk* pk, uint32_t W)
{
  const long w0 = d - (long)dhv, w1 = d + (long)dhv;
  uint32_t np[kCh] = {0, 0, 0, 0}; double pmax[kCh] = {0, 0, 0, 0};
  const uint32_t nr = a.n_range, nd = a.n_dopp;
  for (uint32_t i = 0; i < kCh; ++i) if (g.used[i]) {
    me.bin[i] = d_bin(g, a, me.x, i);
    const DMag mi(nr, me.bin[i]);
    DPk* P = pk + (size_t)i * W;
    auto colv = [&](long e, double* raw) -> double {        // col[e - w0] (-1 = not tested) and rw
      *raw = 0.0;
      if (!mi.ok || !d_dopp_ok(a, e)) return -1.0;
      const double m = mi.at(mag, nr, nd, i, (uint32_t)e); *raw = m * m / g.noise[i];
      const double mr = magc ? mi.at(magc, nr, nd, i, (uint32_t)e) : m;
      return mr * mr / g.noise[i];
    };
    double rprev = 0, rcur = 0, rnext = 0;
    double prev = -1.0, cur = colv(w0, &rcur), next = 0;
    for (long e = w0; e <= w1; ++e) {
      next = e < w1 ? colv(e + 1, &rnext) : -1.0;
      const double v = cur;
      if (v >= 0 && (e == w0 || v >= prev) && (e == w1 || v >= next)) {
        uint32_t k = np[i]++;
        while (k > 0 && P[k - 1].v < v) { P[k] = P[k - 1]; --k; }   // stable descending insertion
        P[k] = DPk{v, rcur, (uint32_t)e};
      }
      prev = cur; cur = next; rprev = rcur; rcur = rnext;
    }
    (void)rprev;
    if (np[i] == 0) {
      const long dc = d < 0 ? 0 : (d > (long)nd - 1 ? (long)nd - 1 : d);
      double raw = 0; if (dc >= w0 && dc <= w1) colv(dc, &raw);
      P[0] = DPk{0.0, raw, (uint32_t)dc}; np[i] = 1;
    }
    pmax[i] = P[0].v;
  }
  double sum_max = 0; for (uint32_t i = 0; i < kCh; ++i) sum_max += pmax[i];
  for (uint32_t i = 0; i < kCh; ++i) if (g.used[i]) {
    const double others = sum_max - pmax[i]; const DPk* P = pk + (size_t)i * W;
    while (np[i] > 1 && !(P[np[i] - 1].v + others > thr)) --np[i];
  }
  double nu[kCh] = {0, 0, 0, 0};
  if (g.n_used == kCh) {
    double gv[kCh][3];
    for (uint32_t i = 0; i < kCh; ++i) { double u1[3], u2[3]; d_unit(me.x, g.tx, u1); d_unit(me.x, g.rx[i], u2); for (int k = 0; k < 3; ++k) gv[i][k] = u1[k] + u2[k]; }
    for (uint32_t i = 0; i < kCh; ++i) {
      const double* r3[3]; uint32_t q = 0; for (uint32_t j = 0; j < kCh; ++j) if (j != i) r3[q++] = gv[j];
      const double cx = r3[1][1] * r3[2][2] - r3[1][2] * r3[2][1], cy = r3[1][2] * r3[2][0] - r3[1][0] * r3[2][2], cz = r3[1][0] * r3[2][1] - r3[1][1] * r3[2][0];
      nu[i] = ((i & 1) ? -1.0 : 1.0) * (r3[0][0] * cx + r3[0][1] * cy + r3[0][2] * cz);
    }
  }
  double nu_abs = 0; for (uint32_t i = 0; i < kCh; ++i) nu_abs += fabs(nu[i]);
  double rest[kCh + 1]; rest[kCh] = 0;
  for (int i = kCh - 1; i >= 0; --i) rest[i] = rest[i + 1] + (g.used[i] ? pmax[i] : 0.0);
  // Iterative form of the recursive DFS (same visiting order, same bound and break).
  double ec = -1; uint32_t idx[kCh] = {0, 0, 0, 0}, pick[kCh] = {0, 0, 0, 0};
  double es[kCh + 1], cs[kCh + 1]; es[0] = 0; cs[0] = 0;
  int lvl = 0; uint32_t jn[kCh + 1] = {0, 0, 0, 0, 0};   // next peak index to try per level
  const double f0 = a.dopp0_hz, fstep = a.dopp_step_hz;
  while (lvl >= 0) {
    if (lvl == (int)kCh) {
      if (!(nu_abs > 0 && fabs(cs[kCh]) > fstep * nu_abs) && es[kCh] > ec) { ec = es[kCh]; for (uint32_t i = 0; i < kCh; ++i) pick[i] = idx[i]; }
      --lvl; continue;
    }
    if (!g.used[lvl]) {
      if (jn[lvl] == 0) { jn[lvl] = 1; idx[lvl] = 0; es[lvl + 1] = es[lvl]; cs[lvl + 1] = cs[lvl]; jn[lvl + 1] = 0; ++lvl; }
      else { jn[lvl] = 0; --lvl; }
      continue;
    }
    const uint32_t j = jn[lvl]; const DPk* P = pk + (size_t)lvl * W;
    if (j < np[lvl] && es[lvl] + P[j].v + rest[lvl + 1] > fmax(ec, thr)) {
      jn[lvl] = j + 1; idx[lvl] = j;
      es[lvl + 1] = es[lvl] + P[j].v; cs[lvl + 1] = cs[lvl] + nu[lvl] * (f0 + P[j].d * fstep);
      jn[lvl + 1] = 0; ++lvl;
    } else { jn[lvl] = 0; --lvl; }
  }
  if (!(ec > thr)) return -1.0;
  for (uint32_t i = 0; i < kCh; ++i) if (g.used[i]) { const DPk& q = pk[(size_t)i * W + pick[i]]; me.pw[i] = q.raw; me.d[i] = q.d; }
  return ec;
}
__global__ void k_choose_cand(DAxes a, DGeo g, DGrid G, const uint32_t* cand, uint32_t nc, size_t nv, const uint32_t* tested,
                              const uint32_t* okc, const uint32_t* dh, const double* nthr, const double* scale, const float* mag,
                              DPk* scratch, uint32_t W, DAcc* out_me, double* out_ec, double* out_thr)
{
  const uint32_t k = blockIdx.x * blockDim.x + threadIdx.x;
  if (k >= nc) return;
  const size_t t = cand[k] / nv, v = cand[k] % nv;
  const long d = tested[t];
  const double thr = scale[t] * nthr[d_m_of(a, okc, d, dh[v])];
  DAcc me; d_voxel(G, v, me.x);
  for (uint32_t i = 0; i < kCh; ++i) { me.d[i] = 0; me.pw[i] = 0; me.bin[i] = 0; }
  out_ec[k] = d_choose(a, g, me, d, dh[v], thr, mag, nullptr, scratch + (size_t)k * kCh * W, W);
  out_me[k] = me; out_thr[k] = thr;
}

// ---------------- pursuit: path response, fitted-path tables, residual cube, leakage ----------------
constexpr int kMaxGrid = 9;
__device__ inline double2 cmul(double2 a, double2 b) { return make_double2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
__device__ inline double2 cadd(double2 a, double2 b) { return make_double2(a.x + b.x, a.y + b.y); }
__device__ inline double cabs2(double2 a) { return hypot(a.x, a.y); }
__device__ inline double2 dpolar(double th) { double s, c; sincos(th, &s, &c); return make_double2(c, s); }
__device__ inline double2 warp_sum(double2 v)
{
  for (int o = 16; o > 0; o >>= 1) { v.x += __shfl_xor_sync(0xffffffffu, v.x, o); v.y += __shfl_xor_sync(0xffffffffu, v.y, o); }
  return v;
}
// The waveform's exact path response (coherent_core.cc's Response): rows with a mask group, per row
// slow-time weight x Doppler phase ed[d][j], range phase em[m][j], and the group kernel B_g (1/kOvs bin).
struct DResp {
  uint32_t nrow, Blen; long X; double tbar;
  const double2 *ed, *em; const double *fc, *t; const int32_t* g; const double2* B;
};
__device__ inline double2 d_kernel_at(const DResp& rs, int32_t g, double x)
{
  const double u = (x + rs.X) * RdResult::Waveform::kOvs;
  if (!(u >= 0) || u >= (double)(rs.Blen - 1)) return make_double2(0, 0);
  const size_t u0 = (size_t)u; const double t = u - u0;
  const double2 b0 = rs.B[(size_t)g * rs.Blen + u0], b1 = rs.B[(size_t)g * rs.Blen + u0 + 1];
  return make_double2(b0.x * (1 - t) + b1.x * t, b0.y * (1 - t) + b1.y * t);
}
__device__ inline double2 d_term(const DResp& rs, uint32_t m, uint32_t d, uint32_t j, double2 pp, double pr)
{
  return cmul(cmul(cmul(rs.ed[(size_t)d * rs.nrow + j], rs.em[(size_t)m * rs.nrow + j]), pp), d_kernel_at(rs, rs.g[j], (double)m - pr));
}
// rs.at(m, d, h) by one warp (lanes over rows).
__device__ inline double2 d_at_warp(const DResp& rs, uint32_t m, uint32_t d, const double2* pp, const double* pr)
{
  double2 acc = make_double2(0, 0);
  for (uint32_t j = threadIdx.x & 31; j < rs.nrow; j += 32) acc = cadd(acc, d_term(rs, m, d, j, pp[j], pr[j]));
  return warp_sum(acc);
}
__global__ void k_resp(DAxes a, uint32_t nrow, const double* w, double wsum, const double* t, const double* fc, double2* ed, double2* em)
{
  const size_t ne = (size_t)a.n_dopp * nrow, nm = (size_t)a.n_range * nrow;
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < ne + nm; q += (size_t)blockDim.x * gridDim.x) {
    if (q < ne) { const size_t d = q / nrow, j = q % nrow;
      const double2 e = dpolar(-2 * M_PI * (a.dopp0_hz + d * a.dopp_step_hz) * t[j]); const double s = w[j] / wsum;
      ed[q] = make_double2(s * e.x, s * e.y); }
    else { const size_t k = q - ne, m = k / nrow, j = k % nrow; em[k] = dpolar(2 * M_PI * fc[j] * m * a.delay_step_s); }
  }
}
// One fitted unit path (coherent_core.cc's ChanFit) as the device sees it.
struct DFit {
  double p, fb, sA, absA, sp, sf, snr; double2 A; int32_t ok, ngrid, ch, pad;
  double gp[kMaxGrid], gfb[kMaxGrid];
};
// Grid phasors: per (slot, grid point, row) the path's per-row delay pr and phasor pp (Response::phasors).
__global__ void k_fit_phasors(DAxes a, DResp rs, const DFit* F, const uint32_t* slots, uint32_t ns, double2* gpp, double* gpr)
{
  const size_t per = (size_t)kMaxGrid * rs.nrow;
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < ns * per; q += (size_t)blockDim.x * gridDim.x) {
    const uint32_t s = slots[q / per]; const uint32_t gq = (uint32_t)((q % per) / rs.nrow), j = (uint32_t)(q % rs.nrow);
    const DFit& f = F[s];
    if ((int)gq >= f.ngrid) continue;
    const double fz = a.dopp0_hz + f.gfb[gq] * a.dopp_step_hz;
    const double pr = f.gp[gq] - (fz / a.fc_hz) * (rs.t[j] - rs.tbar) / a.delay_step_s;
    const size_t o = (size_t)s * per + (size_t)gq * rs.nrow + j;
    gpr[o] = pr; gpp[o] = dpolar(2 * M_PI * (fz * rs.t[j] - rs.fc[j] * pr * a.delay_step_s));
  }
}
// Direct response of each new fit's nominal path over the whole RD grid: dir[slot][m][d] = rs.at(m, d, grid[0]).
__global__ void k_fit_dir(DAxes a, DResp rs, const uint32_t* slots, uint32_t ns, const double2* gpp, const double* gpr, double2* dir)
{
  const size_t nc = (size_t)a.n_range * a.n_dopp, per = (size_t)kMaxGrid * rs.nrow;
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < ns * nc; q += (size_t)blockDim.x * gridDim.x) {
    const uint32_t s = slots[q / nc]; const uint32_t m = (uint32_t)((q % nc) / a.n_dopp), d = (uint32_t)(q % a.n_dopp);
    const double2* pp = gpp + (size_t)s * per; const double* pr = gpr + (size_t)s * per;
    double2 acc = make_double2(0, 0);
    for (uint32_t j = 0; j < rs.nrow; ++j) acc = cadd(acc, d_term(rs, m, d, j, pp[j], pr[j]));
    dir[(size_t)s * nc + (size_t)m * a.n_dopp + d] = acc;
  }
}
// Static-removal term of each new fit (coherent_core.cc's static_term), all Doppler bins: buffer fill for
// one inverse FFT per (slot, d) over the subcarriers, then the crop to the range axis.
__global__ void k_stat_phase(DAxes a, uint32_t sc, const DFit* F, const uint32_t* slots, uint32_t ns, double2* ph)
{
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < (size_t)ns * sc; q += (size_t)blockDim.x * gridDim.x) {
    const uint32_t i = (uint32_t)(q / sc), k = (uint32_t)(q % sc);
    const double fk = ((double)k - sc / 2.0) * a.scs_hz;
    ph[q] = dpolar(-2 * M_PI * fk * F[slots[i]].p * a.delay_step_s);
  }
}
__global__ void k_stat_fill(DAxes a, uint32_t sc, const float2* Q, const double2* M, const uint32_t* slots, uint32_t ns, const double2* ph, double2* buf)
{
  const size_t N = a.n_fft, per = (size_t)a.n_dopp * sc;
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < ns * per; q += (size_t)blockDim.x * gridDim.x) {
    const uint32_t i = (uint32_t)(q / per), d = (uint32_t)((q % per) / sc), k = (uint32_t)(q % sc);
    const float2 qv = Q[(size_t)d * sc + k];
    if (qv.x == 0.f && qv.y == 0.f) continue;
    const long qi = (long)k - (long)(sc / 2);
    const double2 mq = cmul(M[(size_t)slots[i] * sc + k], make_double2(qv.x, qv.y));
    buf[((size_t)i * a.n_dopp + d) * N + (size_t)(((qi % (long)N) + (long)N) % (long)N)] = cmul(mq, ph[(size_t)i * sc + k]);
  }
}
__global__ void k_stat_crop(DAxes a, const uint32_t* slots, uint32_t ns, const double2* buf, double2* stat)
{
  const size_t per = (size_t)a.n_dopp * a.n_range;
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < ns * per; q += (size_t)blockDim.x * gridDim.x) {
    const uint32_t i = (uint32_t)(q / per), d = (uint32_t)((q % per) / a.n_range), m = (uint32_t)(q % a.n_range);
    const double2 v = buf[((size_t)i * a.n_dopp + d) * a.n_fft + m];
    stat[(size_t)slots[i] * per + (size_t)d * a.n_range + m] = make_double2(-v.x, -v.y);
  }
}
// Residual cube: subtract each new fit's direct response (float, as the host cube) and refresh |.|.
__global__ void k_rebuild(DAxes a, const DFit* F, const uint32_t* slots, uint32_t ns, const double2* dir, float2* rc, float* magc)
{
  const size_t nc = (size_t)a.n_range * a.n_dopp;
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < ns * nc; q += (size_t)blockDim.x * gridDim.x) {
    const uint32_t s = slots[q / nc]; const size_t c = q % nc; const DFit& f = F[s];
    if (!f.ok) continue;
    const double2 v = cmul(f.A, dir[(size_t)s * nc + c]);
    const size_t o = (size_t)f.ch * nc + c;
    float2 r = rc[o]; r.x -= (float)v.x; r.y -= (float)v.y; rc[o] = r;
    magc[o] = (float)sqrt((double)r.x * r.x + (double)r.y * r.y);
  }
}
// Items of the pursuit (detect()'s Item) on the device; leak[i] is the running sum over accepted fits
// 0..leak_n[i]-1 at the Doppler bin leak_d[i] (the fits do not change during the pursuit, so a new
// accepted fit only appends a term, in the same order as the oracle's sum).
struct DItem { DAcc me; double thr; int32_t d; uint32_t dh, alive, pad; double leak[kCh]; uint32_t leak_d[kCh], leak_n[kCh]; };
struct DFitTabs { const DFit* F; const double2 *gpp, *dir, *stat; const double* gpr; uint32_t per, nc, sper; };
// leak_amp() of one fit at (bin, d), by one warp.
__device__ double d_leak_amp(const DAxes& a, const DResp& rs, const DFitTabs& T, uint32_t s, double bin, uint32_t d, double z)
{
  const DFit& f = T.F[s];
  if (!f.ok) return 0.0;
  const uint32_t n = a.n_range;
  if (n == 0 || !(bin >= 0) || bin > (double)(n - 1)) return 0.0;
  const long b0 = (long)bin; const double t = bin - b0;
  uint32_t b[4]; for (int j = 0; j < 4; ++j) { const long q = b0 - 1 + j; b[j] = (uint32_t)(q < 0 ? 0 : (q > (long)n - 1 ? (long)n - 1 : q)); }
  const double2* st = T.stat + (size_t)s * T.sper + (size_t)d * n;
  double best = 0;
  for (int q = 0; q < f.ngrid; ++q) {
    double v[4];
    for (int j = 0; j < 4; ++j) {
      const double2 at = q == 0 ? T.dir[(size_t)s * T.nc + (size_t)b[j] * a.n_dopp + d]
                                : d_at_warp(rs, b[j], d, T.gpp + (size_t)s * T.per + (size_t)q * rs.nrow, T.gpr + (size_t)s * T.per + (size_t)q * rs.nrow);
      v[j] = cabs2(cadd(at, st[b[j]]));
    }
    const double tt = t;
    const double c4 = -v[0] * tt * (tt - 1) * (tt - 2) / 6 + v[1] * (tt + 1) * (tt - 1) * (tt - 2) / 2
                      - v[2] * (tt + 1) * tt * (tt - 2) / 2 + v[3] * (tt + 1) * tt * (tt - 1) / 6;
    best = fmax(best, c4);
  }
  return best * (f.absA + z * f.sA);
}
// Leak of every (alive item, used channel): append the new fits, or recompute when the item's bin moved.
// fit_slot(o, ch) = o * kCh + ch. One warp per (item, channel).
__global__ void k_leak(DAxes a, DGeo g, DResp rs, DFitTabs T, DItem* items, uint32_t ni, uint32_t nacc, double z, int force)
{
  const uint32_t w = (blockIdx.x * blockDim.x + threadIdx.x) >> 5;
  if (w >= ni * kCh) return;
  DItem& it = items[w / kCh]; const uint32_t i = w % kCh;
  if (!it.alive || !g.used[i]) return;
  uint32_t from = it.leak_n[i]; double lk = it.leak[i];
  if (force || it.leak_d[i] != it.me.d[i]) { from = 0; lk = 0; }
  const double sn = sqrt(g.noise[i]);
  for (uint32_t o = from; o < nacc; ++o) lk += d_leak_amp(a, rs, T, o * kCh + i, it.me.bin[i], it.me.d[i], z) / sn;
  if ((threadIdx.x & 31) == 0) { it.leak[i] = lk; it.leak_d[i] = it.me.d[i]; it.leak_n[i] = nacc; }
}
// Re-score after an accept: NMS / harmonic merge against the accepted s, then choose() on the residual.
__global__ void k_rescore(DAxes a, DGeo g, DAcc s, double nms_r, DItem* items, uint32_t ni, const float* mag, const float* magc, DPk* scratch, uint32_t W)
{
  const uint32_t k = blockIdx.x * blockDim.x + threadIdx.x;
  if (k >= ni) return;
  DItem& it = items[k];
  if (!it.alive) return;
  const double dx = s.x[0] - it.me.x[0], dy = s.x[1] - it.me.x[1], dz = s.x[2] - it.me.x[2];
  const bool local = sqrt(fma(dz, dz, fma(dy, dy, dx * dx))) <= nms_r;
  bool same = local, harm = local; long kk = 0;
  for (uint32_t i = 0; i < kCh; ++i) if (g.used[i]) {
    const double fs = fma((double)s.d[i], a.dopp_step_hz, a.dopp0_hz), fc = fma((double)it.me.d[i], a.dopp_step_hz, a.dopp0_hz);
    same = same && labs((long)it.me.d[i] - (long)s.d[i]) <= 1;
    if (!kk) kk = lround(fc / fs);
    harm = harm && kk >= 2 && fabs(fma(-(double)kk, fs, fc)) <= a.dopp_step_hz;
  }
  if (local && (same || harm)) { it.alive = 0; return; }
  DAcc me = it.me;
  if (!(d_choose(a, g, me, it.d, it.dh, it.thr, mag, magc, scratch + (size_t)k * kCh * W, W) > it.thr)) { it.alive = 0; return; }
  it.me = me;
}

// ---------------- the per-channel path fit (coherent_core.cc's fit_channel) ----------------
// One cooperative launch fits up to kCh jobs (one per channel), B blocks per job. The optimiser's
// control flow is replicated in every block of a job (deterministic from shared results); each batch
// of J evaluations is spread over the job's blocks and closed by a grid barrier. Same evaluation
// points in the same order as the CPU, hence the same decisions.
struct FitJob { int32_t ch, own, from_slot, pad; double p0, fb0; };
constexpr int kMaxPts = 9;
struct FitArgs {
  DAxes a; DResp rs; DGeo g;
  const float2* rdf; const float2* Q; const uint8_t* mask; const uint32_t *lo, *hi; const double* trow; uint32_t nrow_all, sc;
  DFit* F; const double2 *gpp, *dir, *stat, *Mf; const double* gpr; uint32_t per, nc, sper;
  int tables;                     // 1: others from the dir/stat tables (pursuit), 0: on the fly (refit)
  const FitJob* jobs; uint32_t njobs, nacc, B;
  long hm_r, hm_d; double z; const double2* gam; long ghr, ghd;
  // scratch (per job): cells, y0 (RD minus the other paths), y (plus the static adjustment), and the
  // results of the last two batches (double-buffered: a fast block may start batch n+1 while a slow one
  // still reads batch n)
  int2* cells; double2 *y0, *y, *resA, *resa; double* resJ; float2* Mmid; uint32_t Nmax;
  int* done_iter; int* moved;
};
size_t fit_smem_bytes(uint32_t nrow, uint32_t nm, uint32_t Nmax, uint32_t nrow_all)
{
  return (size_t)(nrow + (nrow + 1) / 2 + (size_t)nm * nrow + Nmax + 4 * (size_t)Nmax + 32 * (size_t)Nmax / 2 + 2) * sizeof(double2)
         + (size_t)nrow_all * sizeof(float2);
}
__device__ inline double2 block_sum2(double2 v, double2* sh)   // blockDim.x multiple of 32, <= 1024
{
  v = warp_sum(v);
  const int w = threadIdx.x >> 5, l = threadIdx.x & 31;
  __syncthreads();
  if (l == 0) sh[w] = v;
  __syncthreads();
  double2 r = make_double2(0, 0);
  if (w == 0) { r = l < (int)(blockDim.x >> 5) ? sh[l] : make_double2(0, 0); r = warp_sum(r); if (l == 0) sh[0] = r; }
  __syncthreads();
  r = sh[0];
  __syncthreads();
  return r;
}
struct FitCtx {
  const FitArgs* A; uint32_t job, b; int N, m0; uint32_t nm;
  const int2* cells; double2 *y0, *y, *resA, *resa; double* resJ;
  double2* shp; double* shr; double2* shG; double2* sha; double2* shD; double* shP; float2* erow; double2* shred;
  int nsync, rd;   // rd: result buffer of the last completed batch
};
// J(p, fb) for point slot k: model a_c = sum_j ed[d_c][j] (em[m_c][j] pp_j K(m_c - pr_j)),
// num = sum y conj(a), den = sum |a|^2, J = |num|^2/den, A = num/den.
__device__ void fit_eval_point(FitCtx& C, double p, double fb, int k, int wb, bool want_a)
{
  const FitArgs& A = *C.A; const DAxes& a = A.a; const DResp& rs = A.rs;
  const double f = a.dopp0_hz + fb * a.dopp_step_hz;
  for (uint32_t j = threadIdx.x; j < rs.nrow; j += blockDim.x) {
    const double pr = p - (f / a.fc_hz) * (rs.t[j] - rs.tbar) / a.delay_step_s;
    C.shr[j] = pr; C.shp[j] = dpolar(2 * M_PI * (f * rs.t[j] - rs.fc[j] * pr * a.delay_step_s));
  }
  __syncthreads();
  for (uint32_t q = threadIdx.x; q < C.nm * rs.nrow; q += blockDim.x) {
    const uint32_t mi = q / rs.nrow, j = q % rs.nrow; const int m = C.m0 + (int)mi;
    if (m < 0 || m >= (int)a.n_range) continue;
    C.shG[q] = cmul(cmul(rs.em[(size_t)m * rs.nrow + j], C.shp[j]), d_kernel_at(rs, rs.g[j], (double)m - C.shr[j]));
  }
  __syncthreads();
  const int w = threadIdx.x >> 5, nw = blockDim.x >> 5;
  for (int c = w; c < C.N; c += nw) {
    const int2 cm = __ldcg(C.cells + c); const uint32_t mi = (uint32_t)(cm.x - C.m0);
    double2 acc = make_double2(0, 0);
    for (uint32_t j = threadIdx.x & 31; j < rs.nrow; j += 32) acc = cadd(acc, cmul(rs.ed[(size_t)cm.y * rs.nrow + j], C.shG[mi * rs.nrow + j]));
    acc = warp_sum(acc);
    if ((threadIdx.x & 31) == 0) C.sha[c] = acc;
  }
  __syncthreads();
  if (w == 0) {
    double2 num = make_double2(0, 0); double den = 0;
    for (int c = threadIdx.x; c < C.N; c += 32) {
      const double2 yc = __ldcg(C.y + c), ac = C.sha[c];
      num.x += yc.x * ac.x + yc.y * ac.y; num.y += yc.y * ac.x - yc.x * ac.y; den += ac.x * ac.x + ac.y * ac.y;
    }
    num = warp_sum(num);
    for (int o = 16; o > 0; o >>= 1) den += __shfl_xor_sync(0xffffffffu, den, o);
    if (threadIdx.x == 0) {
      C.resJ[wb * kMaxPts + k] = den > 0 ? (num.x * num.x + num.y * num.y) / den : 0.0;
      C.resA[wb * kMaxPts + k] = den > 0 ? make_double2(num.x / den, num.y / den) : make_double2(0, 0);
    }
  }
  if (want_a) for (int c = threadIdx.x; c < C.N; c += blockDim.x) C.resa[((size_t)wb * kMaxPts + k) * A.Nmax + c] = C.sha[c];
  __syncthreads();
}
__device__ void fit_sync(FitCtx& C) { __threadfence(); __syncthreads(); cg::this_grid().sync(); ++C.nsync; }
__device__ void fit_batch(FitCtx& C, const double* pp, const double* pf, int n, bool want_a)
{
  const int wb = C.nsync & 1;
  for (int k = (int)C.b; k < n; k += (int)C.A->B) fit_eval_point(C, pp[k], pf[k], k, wb, want_a);
  fit_sync(C);
  C.rd = wb;
}
__device__ inline double ldJ(const FitCtx& C, int k) { return __ldcg(C.resJ + C.rd * kMaxPts + k); }
__device__ inline double2 ldA(const FitCtx& C, int k) { return __ldcg(C.resA + C.rd * kMaxPts + k); }

// search(st): coarse pattern search (st >= 0.05) then Newton on central differences (h = 1e-3 bin),
// exactly coherent_core.cc's search lambda; the line search's halvings are evaluated B at a time.
__device__ double fit_search(FitCtx& C, double& p, double& fb, double st, double2& bestA)
{
  double pp[kMaxPts], pf[kMaxPts];
  double best;
  if (st >= 0.05) {
    int n = 0; pp[n] = p; pf[n++] = fb;
    for (int u = -1; u <= 1; ++u) for (int v = -1; v <= 1; ++v) if (u || v) { pp[n] = p + u * st; pf[n++] = fb + v * st; }
    fit_batch(C, pp, pf, n, false);
    best = ldJ(C, 0); bestA = ldA(C, 0);
    int off = 1;
    for (;;) {
      double bp = p, bf = fb; int q = 0;
      for (int u = -1; u <= 1; ++u) for (int v = -1; v <= 1; ++v) if (u || v) {
        const double j = ldJ(C, q + off); if (j > best) { best = j; bestA = ldA(C, q + off); bp = p + u * st; bf = fb + v * st; }
        ++q;
      }
      if (bp == p && bf == fb) st /= 2; else { p = bp; fb = bf; }
      if (!(st >= 0.05)) break;
      n = 0; for (int u = -1; u <= 1; ++u) for (int v = -1; v <= 1; ++v) if (u || v) { pp[n] = p + u * st; pf[n++] = fb + v * st; }
      fit_batch(C, pp, pf, n, false); off = 0;
    }
  } else {
    pp[0] = p; pf[0] = fb; fit_batch(C, pp, pf, 1, false);
    best = ldJ(C, 0); bestA = ldA(C, 0);
  }
  const int B = (int)C.A->B;
  for (int it = 0; it < 20; ++it) {
    const double h = 1e-3, j0 = best;
    pp[0] = p + h; pf[0] = fb; pp[1] = p - h; pf[1] = fb; pp[2] = p; pf[2] = fb + h; pp[3] = p; pf[3] = fb - h;
    pp[4] = p + h; pf[4] = fb + h; pp[5] = p - h; pf[5] = fb - h;
    fit_batch(C, pp, pf, 6, false);
    const double jpp = ldJ(C, 0), jpm = ldJ(C, 1), jfp = ldJ(C, 2), jfm = ldJ(C, 3), jpf = ldJ(C, 4), jmm = ldJ(C, 5);
    const double gp = (jpp - jpm) / (2 * h), gf = (jfp - jfm) / (2 * h);
    const double hpp = (jpp - 2 * j0 + jpm) / (h * h), hff = (jfp - 2 * j0 + jfm) / (h * h);
    const double hpf = (jpf + jmm - jpp - jpm - jfp - jfm + 2 * j0) / (2 * h * h);
    const double det = hpp * hff - hpf * hpf;
    if (!(hpp < 0 && det > 0)) break;
    double dp = -(hff * gp - hpf * gf) / det, df = -(hpp * gf - hpf * gp) / det;
    // J at dp/2^k, k = 0..20 (the CPU halves until J >= j0), evaluated min(B, kMaxPts) at a time
    double jn = 0; double2 an = make_double2(0, 0); int kk = 0;
    const int nb = B < kMaxPts ? B : kMaxPts;
    for (int base = 0; base <= 20; base += nb) {
      const int n = nb < 21 - base ? nb : 21 - base;
      double sdp = dp, sdf = df; for (int q = 0; q < base; ++q) { sdp /= 2; sdf /= 2; }
      for (int q = 0; q < n; ++q) { pp[q] = p + sdp; pf[q] = fb + sdf; sdp /= 2; sdf /= 2; }
      fit_batch(C, pp, pf, n, false);
      bool done = false;
      for (int q = 0; q < n; ++q) { jn = ldJ(C, q); an = ldA(C, q); kk = base + q; if (jn >= j0) { done = true; break; } }
      if (done) break;
    }
    for (int q = 0; q < kk; ++q) { dp /= 2; df /= 2; }
    if (!(jn >= j0)) break;
    p += dp; fb += df; best = jn; bestA = an;
    if (hypot(dp, df) < 1e-5) break;
  }
  return best;
}
// Static-removal term of a unit path at x = m - p, Doppler bin d, per-subcarrier weights M:
// sv = sum_k e^{j2pi (k - sc/2) scs x delay} M_k Q_k(d) (coherent_core.cc's static_cell is -sv).
// It only shapes a fit's data (a few % of a path): FP32 phasor recurrence over 16-subcarrier chunks,
// each restarted from an FP64 phase, FP64 accumulation across chunks.
template <class MT>
__device__ double2 d_static_sum(const FitArgs& A, double x, uint32_t d, const MT* M, double2* sh)
{
  const DAxes& a = A.a; const uint32_t sc = A.sc;
  const float2* Qd = A.Q + (size_t)d * sc;
  double2 acc = make_double2(0, 0);
  constexpr uint32_t kChunk = 16;
  const double w = 2 * M_PI * a.scs_hz * x * a.delay_step_s;
  float rs_, rc_; sincosf((float)w, &rs_, &rc_);
  for (uint32_t k0 = threadIdx.x * kChunk; k0 < sc; k0 += blockDim.x * kChunk) {
    const double2 ph0 = dpolar(w * ((double)k0 - sc / 2.0));
    float phr = (float)ph0.x, phi = (float)ph0.y; float ar = 0, ai = 0;
    const uint32_t k1 = k0 + kChunk < sc ? k0 + kChunk : sc;
    for (uint32_t k = k0; k < k1; ++k) {
      const float2 q = Qd[k];
      if (q.x != 0.f || q.y != 0.f) {
        const float mr = (float)M[k].x, mi = (float)M[k].y;
        const float tr = phr * mr - phi * mi, ti = phr * mi + phi * mr;
        ar += tr * q.x - ti * q.y; ai += tr * q.y + ti * q.x;
      }
      const float nr = phr * rc_ - phi * rs_; phi = phr * rs_ + phi * rc_; phr = nr;
    }
    acc.x += ar; acc.y += ai;
  }
  return block_sum2(acc, sh);
}
__global__ void __launch_bounds__(256) k_fit(FitArgs A)
{
  extern __shared__ double2 smem[];
  const uint32_t job = blockIdx.x / A.B, b = blockIdx.x % A.B;
  const FitJob J = A.jobs[job];
  const DAxes& a = A.a; const DResp& rs = A.rs;
  const uint32_t ch = (uint32_t)J.ch, nm = (uint32_t)(2 * A.hm_r + 1);
  FitCtx C;
  C.A = &A; C.job = job; C.b = b; C.nm = nm; C.nsync = 0; C.rd = 0;
  C.cells = A.cells + (size_t)job * A.Nmax; C.y0 = A.y0 + (size_t)job * A.Nmax; C.y = A.y + (size_t)job * A.Nmax;
  C.resA = A.resA + (size_t)job * 2 * kMaxPts; C.resJ = A.resJ + (size_t)job * 2 * kMaxPts; C.resa = A.resa + (size_t)job * 2 * kMaxPts * A.Nmax;
  C.shp = smem; C.shr = (double*)(smem + rs.nrow); C.shG = smem + rs.nrow + (rs.nrow + 1) / 2;
  C.sha = C.shG + (size_t)nm * rs.nrow; C.shD = C.sha + A.Nmax; C.shP = (double*)(C.shD + 4 * (size_t)A.Nmax);
  C.shred = (double2*)(C.shP + 32 * (size_t)A.Nmax); C.erow = (float2*)(C.shred + 2);
  __shared__ double2 shred[32];
  C.shred = shred;
  double p0 = J.p0, fb0 = J.fb0;
  if (J.from_slot) { p0 = A.F[J.own * kCh + ch].p; fb0 = A.F[J.own * kCh + ch].fb; }
  // cells: m-major, d inner, as the CPU (every thread builds the same list; block 0 publishes it)
  const long mc = lround(p0), dc = lround(fb0);
  C.m0 = (int)(mc - A.hm_r);
  int2* cellw = A.cells + (size_t)job * A.Nmax;
  int N = 0;
  for (long m = mc - A.hm_r; m <= mc + A.hm_r; ++m)
    for (long d = dc - A.hm_d; d <= dc + A.hm_d; ++d)
      if (m >= 0 && m < (long)a.n_range && d_dopp_ok(a, d)) { if (threadIdx.x == 0 && b == 0) cellw[N] = make_int2((int)m, (int)d); ++N; }
  C.N = N;
  const bool doit = N >= 4;
  fit_sync(C);   // cells published; every block has read the slot's previous p/fb
  if (doit) {
    // y0 = RD - sum of the other accepted paths on this channel (direct + static term), in the CPU's order
    for (int c = (int)b; c < N; c += (int)A.B) {
      const int2 cm = __ldcg(C.cells + c);
      const float2 r = A.rdf[((size_t)ch * a.n_range + cm.x) * a.n_dopp + cm.y];
      double2 yc = make_double2(r.x, r.y);
      for (uint32_t o = 0; o < A.nacc; ++o) {
        if ((int)o == J.own) continue;
        const uint32_t s = o * kCh + ch; const DFit f = A.F[s];
        if (!f.ok) continue;
        double2 at, st;
        if (A.tables) { at = A.dir[(size_t)s * A.nc + (size_t)cm.x * a.n_dopp + cm.y]; st = A.stat[(size_t)s * A.sper + (size_t)cm.y * a.n_range + cm.x]; }
        else {
          double2 acc = make_double2(0, 0);
          const double2* gp0 = A.gpp + (size_t)s * A.per; const double* gr0 = A.gpr + (size_t)s * A.per;
          for (uint32_t j = threadIdx.x; j < rs.nrow; j += blockDim.x) acc = cadd(acc, d_term(rs, (uint32_t)cm.x, (uint32_t)cm.y, j, gp0[j], gr0[j]));
          at = block_sum2(acc, C.shred);
          const double2 sv = d_static_sum(A, (double)cm.x - f.p, (uint32_t)cm.y, A.Mf + (size_t)s * A.sc, C.shred);
          st = make_double2(-sv.x, -sv.y);
        }
        const double2 v = cmul(f.A, cadd(at, st));
        yc.x -= v.x; yc.y -= v.y;
      }
      if (threadIdx.x == 0) { C.y0[c] = yc; C.y[c] = yc; }
    }
    fit_sync(C);
  }
  double p = p0, fb = fb0; double2 bestA = make_double2(0, 0);
  DFit out{}; out.ch = (int32_t)ch;
  if (doit) {
    fit_search(C, p, fb, 0.25, bestA);
    // M_k(f) = mean over the rows observing k of e^{j2pi f t_r} (FP32: it only shapes y)
    const double fhz = a.dopp0_hz + fb * a.dopp_step_hz;
    for (uint32_t r = threadIdx.x; r < A.nrow_all; r += blockDim.x) { const double2 e = dpolar(2 * M_PI * fhz * A.trow[r]); C.erow[r] = make_float2((float)e.x, (float)e.y); }
    __syncthreads();
    float2* Mm = A.Mmid + (size_t)blockIdx.x * A.sc;
    for (uint32_t k = threadIdx.x; k < A.sc; k += blockDim.x) {
      float mr = 0, mi = 0; uint32_t cnt = 0;
      for (uint32_t r = 0; r < A.nrow_all; ++r)
        if (k >= A.lo[r] && k <= A.hi[r] && A.mask[(size_t)r * A.sc + k]) { mr += C.erow[r].x; mi += C.erow[r].y; ++cnt; }
      Mm[k] = cnt ? make_float2(mr / cnt, mi / cnt) : make_float2(0, 0);
    }
    __syncthreads();
    for (int pass = 0; pass < 2; ++pass) {
      const double2 Av = bestA;     // = the CPU's J(p, fb, &A): same point, same y
      for (int c = (int)b; c < N; c += (int)A.B) {
        const int2 cm = __ldcg(C.cells + c);
        const double2 sv = d_static_sum(A, (double)cm.x - p, (uint32_t)cm.y, Mm, C.shred);
        if (threadIdx.x == 0) { const double2 y0c = __ldcg(C.y0 + c); C.y[c] = make_double2(y0c.x + (Av.x * sv.x - Av.y * sv.y), y0c.y + (Av.x * sv.y + Av.y * sv.x)); }
      }
      fit_sync(C);
      fit_search(C, p, fb, 0.0, bestA);
    }
    // model at (p, fb) and +-h: the sandwich covariance of (p, fb, Re A, Im A)
    const double h = 1e-3;
    double pp[5] = {p, p + h, p - h, p, p}, pf[5] = {fb, fb, fb, fb + h, fb - h};
    fit_batch(C, pp, pf, 5, true);
    if (b == 0) {
      const double2 fA = ldA(C, 0);
      const double2* ra = C.resa + (size_t)C.rd * kMaxPts * A.Nmax;
      auto av = [&](int k, int c) { return __ldcg(ra + (size_t)k * A.Nmax + c); };
      for (int c = threadIdx.x; c < N; c += blockDim.x) {
        const double2 a0 = av(0, c), ap = av(1, c), am = av(2, c), fp = av(3, c), fm = av(4, c);
        double2 d0 = cmul(fA, make_double2(ap.x - am.x, ap.y - am.y)), d1 = cmul(fA, make_double2(fp.x - fm.x, fp.y - fm.y));
        d0.x /= 2 * h; d0.y /= 2 * h; d1.x /= 2 * h; d1.y /= 2 * h;
        C.shD[4 * c + 0] = d0; C.shD[4 * c + 1] = d1; C.shD[4 * c + 2] = a0; C.shD[4 * c + 3] = make_double2(-a0.y, a0.x);
      }
      __syncthreads();
      // per cell c: DtD and DCD contributions (T = sum_e G(c,e) D_e), summed over c in order below
      const long hr = 2 * A.hm_r, hd = 2 * A.hm_d, nd = 2 * hd + 1;
      for (int c = threadIdx.x; c < N; c += blockDim.x) {
        const int2 cc = __ldcg(C.cells + c);
        double2 T[4] = {make_double2(0, 0), make_double2(0, 0), make_double2(0, 0), make_double2(0, 0)};
        for (int e = 0; e < N; ++e) {
          const int2 ce = __ldcg(C.cells + e);
          const double2 gm = A.gam[(size_t)((cc.x - ce.x + hr) * nd + (cc.y - ce.y) + hd)];
          for (int v = 0; v < 4; ++v) T[v] = cadd(T[v], cmul(gm, C.shD[4 * e + v]));
        }
        for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) {
          const double2 du = C.shD[4 * c + u], dv = C.shD[4 * c + v];
          C.shP[(size_t)c * 32 + u * 4 + v] = du.x * dv.x + du.y * dv.y;
          C.shP[(size_t)c * 32 + 16 + u * 4 + v] = du.x * T[v].x + du.y * T[v].y;
        }
      }
      __syncthreads();
      __shared__ double S[32]; __shared__ double rrs;
      if (threadIdx.x < 32) { double acc = 0; for (int c = 0; c < N; ++c) acc += C.shP[(size_t)c * 32 + threadIdx.x]; S[threadIdx.x] = acc; }
      if (threadIdx.x == 32) {
        double rr = 0;
        for (int c = 0; c < N; ++c) { const double2 yc = __ldcg(C.y + c), fa = cmul(fA, C.shD[4 * c + 2]); const double ex = yc.x - fa.x, ey = yc.y - fa.y; rr += ex * ex + ey * ey; }
        rrs = rr;
      }
      __syncthreads();
      if (threadIdx.x == 0) {
        const double nz = A.g.noise[ch];
        double DtD[4][4], DCD[4][4];
        for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) { DtD[u][v] = S[u * 4 + v]; DCD[u][v] = S[16 + u * 4 + v] * (nz / 2); }
        out.ok = 1; out.p = p; out.fb = fb; out.A = fA; out.absA = hypot(fA.x, fA.y);
        out.snr = (fA.x * fA.x + fA.y * fA.y) / fmax(nz, N > 2 ? rrs / (N - 2) : 0.0);
        // inverse(DtD) exactly as small_matrix.h (partial pivoting, relative floor 1e-12)
        double M4[4][4], Ai[4][4];
        for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) { M4[u][v] = DtD[u][v]; Ai[u][v] = u == v; }
        double scl = 0; for (int i2 = 0; i2 < 4; ++i2) scl = fmax(scl, fabs(M4[i2][i2]));
        const double flo = fmax(2.2250738585072014e-308, scl * 1e-12);
        for (int col = 0; col < 4; ++col) {
          int pv = col;
          for (int row = col + 1; row < 4; ++row) if (fabs(M4[row][col]) > fabs(M4[pv][col])) pv = row;
          if (fabs(M4[pv][col]) <= flo) { M4[col][col] += flo; pv = col; }
          if (pv != col) for (int j2 = 0; j2 < 4; ++j2) { double t1 = M4[col][j2]; M4[col][j2] = M4[pv][j2]; M4[pv][j2] = t1; t1 = Ai[col][j2]; Ai[col][j2] = Ai[pv][j2]; Ai[pv][j2] = t1; }
          const double pvv = M4[col][col];
          for (int j2 = 0; j2 < 4; ++j2) { M4[col][j2] /= pvv; Ai[col][j2] /= pvv; }
          for (int row = 0; row < 4; ++row) if (row != col) {
            const double fct = M4[row][col];
            for (int j2 = 0; j2 < 4; ++j2) { M4[row][j2] -= fct * M4[col][j2]; Ai[row][j2] -= fct * Ai[col][j2]; }
          }
        }
        double T1[4][4] = {}, cov[4][4] = {};
        for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) for (int k2 = 0; k2 < 4; ++k2) T1[u][v] += Ai[u][k2] * DCD[k2][v];
        for (int u = 0; u < 4; ++u) for (int v = 0; v < 4; ++v) for (int k2 = 0; k2 < 4; ++k2) cov[u][v] += T1[u][k2] * Ai[k2][v];
        out.sp = sqrt(fmax(0.0, cov[0][0])); out.sf = sqrt(fmax(0.0, cov[1][1])); out.sA = sqrt(fmax(0.0, cov[2][2] + cov[3][3]));
        double ps[3] = {p, 0, 0}, fs[3] = {fb, 0, 0}; int np = 1, nf = 1;
        if (A.z * out.sp > 1e-6) { ps[1] = p - A.z * out.sp; ps[2] = p + A.z * out.sp; np = 3; }
        if (A.z * out.sf > 1e-6) { fs[1] = fb - A.z * out.sf; fs[2] = fb + A.z * out.sf; nf = 3; }
        out.ngrid = 0;
        for (int i2 = 0; i2 < np; ++i2) for (int j2 = 0; j2 < nf; ++j2) { out.gp[out.ngrid] = ps[i2]; out.gfb[out.ngrid] = fs[j2]; ++out.ngrid; }
        // publish, with the refit's convergence test against the slot's previous fit
        DFit& dst = A.F[J.own * kCh + ch];
        const double scv = sqrt(fmax(1.0, (fA.x * fA.x + fA.y * fA.y) / nz / fmax(out.snr, 1e-300)));
        if (J.from_slot && (!dst.ok || fabs(out.p - dst.p) > scv * out.sp || fabs(out.fb - dst.fb) > scv * out.sf)) atomicOr(A.moved, 1);
        dst = out;
      }
    }
  } else if (b == 0 && threadIdx.x == 0) {
    A.F[J.own * kCh + ch] = out;     // ChanFit{} (not ok): fewer than 4 cells
  }
  // Wait for every job: a job done after its n-th barrier is visible to all at barrier n+1, so
  // "all done_iter < nsync" is evaluated identically by every thread of every block.
  if (b == 0 && threadIdx.x == 0) atomicExch(A.done_iter + job, C.nsync);
  for (;;) {
    bool all = true;
    for (uint32_t q = 0; q < A.njobs; ++q) if (!(((volatile int*)A.done_iter)[q] < C.nsync)) all = false;
    if (all) break;
    fit_sync(C);
  }
}
// Final set_M(fb) of the new fits (FP64: the weights of their static-removal tables).
__global__ void k_setM_phase(DAxes a, const DFit* F, const uint32_t* slots, uint32_t ns, uint32_t nrow_all, const double* trow, double2* erow)
{
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < (size_t)ns * nrow_all; q += (size_t)blockDim.x * gridDim.x) {
    const DFit& f = F[slots[q / nrow_all]];
    erow[q] = dpolar(2 * M_PI * (a.dopp0_hz + f.fb * a.dopp_step_hz) * trow[q % nrow_all]);
  }
}
__global__ void k_setM(const DFit* F, const uint32_t* slots, uint32_t ns, uint32_t sc, uint32_t nrow_all, const uint8_t* mask,
                       const uint32_t* lo, const uint32_t* hi, const double2* erow, double2* Mf)
{
  for (size_t q = (size_t)blockIdx.x * blockDim.x + threadIdx.x; q < (size_t)ns * sc; q += (size_t)blockDim.x * gridDim.x) {
    const uint32_t i = (uint32_t)(q / sc), s = slots[i], k = (uint32_t)(q % sc);
    double2 m = make_double2(0, 0); uint32_t cnt = 0;
    if (F[s].ok) {
      for (uint32_t r = 0; r < nrow_all; ++r)
        if (k >= lo[r] && k <= hi[r] && mask[(size_t)r * sc + k]) { const double2 e = erow[(size_t)i * nrow_all + r]; m.x += e.x; m.y += e.y; ++cnt; }
      if (cnt) { m.x /= (double)cnt; m.y /= (double)cnt; }
    }
    Mf[(size_t)s * sc + k] = m;
  }
}
} // namespace

struct GpuDetect::Impl {
  cudaStream_t stream = nullptr;
  GpuDetectTiming t;
  uint64_t n_calls = 0;
  // persistent device buffers, grown on demand, never freed per CPI
  struct Buf {
    void* p = nullptr; size_t cap = 0;
    ~Buf() { if (p) cudaFree(p); }
    template <class T> T* as() const { return (T*)p; }
    void ensure(size_t bytes)
    {
      if (bytes <= cap) return;
      if (p) cudaFree(p);
      p = nullptr; cap = 0;
      cuda_check(cudaMalloc(&p, bytes), "cudaMalloc");
      cap = bytes;
    }
  };
  Buf okt, rd_up, E_up, rdf, mag, rcf, magc, dh, tested, okc, nmed, nthr, zz, zz_sorted, seg_off, scale, flag, cand, ncand, cub_tmp,
      c_me, c_ec, c_thr, pk_scratch,
      r_w, r_t, r_fc, r_g, r_B, r_ed, r_em, Q, items, one, slots, F, Mf, gpp, gpr, dir, stat, st_ph, st_buf, alive,
      w_mask, w_lo, w_hi, w_trow, gam, jobs, f_cells, f_y0, f_y, f_resA, f_resa, f_resJ, f_Mmid, done_iter, moved, m_erow;
  int n_sm = 0; size_t fit_smem_set = 0;
  // Cooperative fit launch: jobs (one per channel), then each new fit's grid phasors and final M.
  void launch_fits(FitArgs fa, const std::vector<FitJob>& jb, uint32_t nrow_all, const DAxes& da, const DResp& dr, size_t per)
  {
    if (jb.empty()) return;
    const uint32_t nj = (uint32_t)jb.size();
    up(jobs, jb.data(), nj);
    const size_t smem = fit_smem_bytes(fa.rs.nrow, (uint32_t)(2 * fa.hm_r + 1), fa.Nmax, nrow_all);
    if (!n_sm) { int dev = 0; cudaGetDevice(&dev); cudaDeviceGetAttribute(&n_sm, cudaDevAttrMultiProcessorCount, dev); }
    if (smem > fit_smem_set) {
      cuda_check(cudaFuncSetAttribute(k_fit, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem), "fit smem attr");
      fit_smem_set = smem;
    }
    int per_sm = 0; cuda_check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&per_sm, k_fit, 256, smem), "fit occupancy");
    const uint32_t total = (uint32_t)std::max(1, per_sm * n_sm);
    if (total < nj) throw std::runtime_error("detect fit: device cannot co-schedule one block per job");
    fa.B = std::max<uint32_t>(1, std::min<uint32_t>(8, total / nj));
    fa.jobs = jobs.as<FitJob>(); fa.njobs = nj;
    const size_t nb = (size_t)nj * fa.B;
    f_cells.ensure((size_t)nj * fa.Nmax * sizeof(int2)); f_y0.ensure((size_t)nj * fa.Nmax * sizeof(double2)); f_y.ensure((size_t)nj * fa.Nmax * sizeof(double2));
    f_resA.ensure((size_t)nj * 2 * kMaxPts * sizeof(double2)); f_resJ.ensure((size_t)nj * 2 * kMaxPts * sizeof(double));
    f_resa.ensure((size_t)nj * 2 * kMaxPts * fa.Nmax * sizeof(double2)); f_Mmid.ensure(nb * fa.sc * sizeof(float2));
    done_iter.ensure(nj * sizeof(int));
    fa.cells = f_cells.as<int2>(); fa.y0 = f_y0.as<double2>(); fa.y = f_y.as<double2>(); fa.resA = f_resA.as<double2>(); fa.resJ = f_resJ.as<double>();
    fa.resa = f_resa.as<double2>(); fa.Mmid = f_Mmid.as<float2>(); fa.done_iter = done_iter.as<int>(); fa.moved = moved.as<int>();
    cuda_check(cudaMemsetAsync(done_iter.p, 0x7f, nj * sizeof(int), stream), "memset done_iter");
    void* args[] = {&fa};
    cuda_check(cudaLaunchCooperativeKernel((void*)k_fit, dim3((unsigned)nb), dim3(256), args, smem, stream), "fit launch");
    std::vector<uint32_t> sl(nj); for (uint32_t q = 0; q < nj; ++q) sl[q] = (uint32_t)jb[q].own * kCh + (uint32_t)jb[q].ch;
    up(slots, sl.data(), nj);
    k_fit_phasors<<<blocks(nj * per), 256, 0, stream>>>(da, dr, F.as<DFit>(), slots.as<uint32_t>(), nj, gpp.as<double2>(), gpr.as<double>());
    m_erow.ensure((size_t)nj * nrow_all * sizeof(double2));
    k_setM_phase<<<blocks((size_t)nj * nrow_all), 256, 0, stream>>>(da, F.as<DFit>(), slots.as<uint32_t>(), nj, nrow_all, w_trow.as<double>(), m_erow.as<double2>());
    k_setM<<<blocks((size_t)nj * fa.sc), 256, 0, stream>>>(F.as<DFit>(), slots.as<uint32_t>(), nj, fa.sc, nrow_all, w_mask.as<uint8_t>(), w_lo.as<uint32_t>(),
                                                          w_hi.as<uint32_t>(), m_erow.as<double2>(), Mf.as<double2>());
  }
  cufftHandle plan = 0; long plan_n = -1, plan_batch = -1;
  uint32_t slot_cap = 0;
  ~Impl() { if (plan) cufftDestroy(plan); }
  // Grow-only buffer that keeps its contents (fit tables persist across the pursuit's rounds).
  void grow_keep(Buf& b, size_t bytes)
  {
    if (bytes <= b.cap) return;
    void* np = nullptr; cuda_check(cudaMalloc(&np, bytes), "cudaMalloc");
    if (b.p) { cuda_check(cudaMemcpyAsync(np, b.p, b.cap, cudaMemcpyDeviceToDevice, stream), "D2D grow"); sync(); cudaFree(b.p); }
    b.p = np; b.cap = bytes;
  }
  void fit_slots_reserve(uint32_t n, size_t per, size_t ncc, size_t sper, uint32_t sc)
  {
    if (n <= slot_cap && F.cap >= n * sizeof(DFit)) {
      // the per-slot sizes can change CPI to CPI (rows, Doppler bins): re-check the byte sizes too
      if (gpp.cap >= n * per * sizeof(double2) && dir.cap >= n * ncc * sizeof(double2) && stat.cap >= n * sper * sizeof(double2) && Mf.cap >= (size_t)n * sc * sizeof(double2)) return;
    }
    const uint32_t cap = std::max<uint32_t>(n, 2 * slot_cap);
    grow_keep(F, cap * sizeof(DFit)); grow_keep(gpp, cap * per * sizeof(double2)); grow_keep(gpr, cap * per * sizeof(double));
    grow_keep(dir, cap * ncc * sizeof(double2)); grow_keep(stat, cap * sper * sizeof(double2)); grow_keep(Mf, (size_t)cap * sc * sizeof(double2));
    slot_cap = cap;
  }
  void set_alive(const std::vector<DItem>& it)
  {
    // only the alive flags changed on the host: rewrite them in place (strided 4-byte copies)
    if (it.empty()) return;
    cuda_check(cudaMemcpy2DAsync((char*)items.p + offsetof(DItem, alive), sizeof(DItem), (const char*)it.data() + offsetof(DItem, alive), sizeof(DItem),
                                 sizeof(uint32_t), it.size(), cudaMemcpyHostToDevice, stream), "H2D alive");
  }
  // Phasors, direct response and static-removal tables of the fits in `slots` (ns of them).
  void build_fit_tables(const DAxes& da, const DResp& dr, uint32_t ns, uint32_t sc, size_t per, size_t ncc, size_t sper)
  {
    k_fit_dir<<<blocks(ns * ncc, 128), 128, 0, stream>>>(da, dr, slots.as<uint32_t>(), ns, gpp.as<double2>(), gpr.as<double>(), dir.as<double2>());
    const long N = da.n_fft, batch = (long)ns * da.n_dopp;
    if (!plan || plan_n != N || plan_batch < batch) {
      if (plan) cufftDestroy(plan);
      plan_batch = std::max(batch, plan_n == N ? plan_batch : 0L); plan_n = N;
      if (cufftPlan1d(&plan, (int)N, CUFFT_Z2Z, (int)plan_batch) != CUFFT_SUCCESS) throw std::runtime_error("cufftPlan1d (detect static term)");
      cufftSetStream(plan, stream);
    }
    st_buf.ensure((size_t)plan_batch * N * sizeof(double2)); st_ph.ensure((size_t)ns * sc * sizeof(double2));
    cuda_check(cudaMemsetAsync(st_buf.p, 0, (size_t)batch * N * sizeof(double2), stream), "memset stat");
    k_stat_phase<<<blocks((size_t)ns * sc), 256, 0, stream>>>(da, sc, F.as<DFit>(), slots.as<uint32_t>(), ns, st_ph.as<double2>());
    k_stat_fill<<<blocks((size_t)ns * da.n_dopp * sc), 256, 0, stream>>>(da, sc, Q.as<float2>(), Mf.as<double2>(), slots.as<uint32_t>(), ns, st_ph.as<double2>(), st_buf.as<double2>());
    if (cufftExecZ2Z(plan, st_buf.as<cufftDoubleComplex>(), st_buf.as<cufftDoubleComplex>(), CUFFT_INVERSE) != CUFFT_SUCCESS) throw std::runtime_error("cufftExecZ2Z (detect static term)");
    k_stat_crop<<<blocks(ns * sper), 256, 0, stream>>>(da, slots.as<uint32_t>(), ns, st_buf.as<double2>(), stat.as<double2>());
  }
  template <class T> void up(Buf& b, const T* h, size_t n)
  {
    b.ensure(n * sizeof(T));
    if (n) cuda_check(cudaMemcpyAsync(b.p, h, n * sizeof(T), cudaMemcpyHostToDevice, stream), "H2D");
  }
  template <class T> void down(T* h, const Buf& b, size_t n)
  {
    if (n) cuda_check(cudaMemcpyAsync(h, b.p, n * sizeof(T), cudaMemcpyDeviceToHost, stream), "D2H");
  }
  void sync() { cuda_check(cudaStreamSynchronize(stream), "sync"); }
  static unsigned blocks(size_t n, unsigned t = 256) { return (unsigned)std::max<size_t>(1, std::min<size_t>(65535 * 4, (n + t - 1) / t)); }
};

GpuDetect::GpuDetect() : impl_(std::make_unique<Impl>()) {}
GpuDetect::~GpuDetect() = default;
const GpuDetectTiming& GpuDetect::timing() const { return impl_->t; }

std::vector<Detection> GpuDetect::run(const std::vector<float>& E, const RdResult& R, const Grid& g, const Geometry& geo,
                                      const DetectParams& p, const void* d_rd, const float* d_E, void* stream)
{
  Impl& I = *impl_;
  (void)d_rd; (void)d_E;
  I.stream = (cudaStream_t)stream;
  I.t = GpuDetectTiming{};
  GpuDetectTiming& T = I.t;
  const auto t_all = Clock::now();
  if (const char* dir = std::getenv("NR_ISAC_DETECT_DUMP")) {
    const char* mx = std::getenv("NR_ISAC_DETECT_DUMP_MAX");
    if (I.n_calls < (mx ? std::strtoull(mx, nullptr, 10) : 1000ull))
      save_detect_case(std::string(dir) + "/case_" + std::to_string(I.n_calls) + ".bin", E, R, g, geo, p);
  }
  ++I.n_calls;

  const Axes& a = R.rd.axes;
  std::vector<Detection> out;
  const uint32_t n = n_used(R);
  const size_t nt = a.tested_dopp.size(), nv = g.size();
  if (E.size() != nt * nv) throw std::invalid_argument("detect: envelope size does not match the grid and Doppler axis");
  if (n == 0 || nv == 0 || !(p.pfa > 0)) return out;
  auto t0 = Clock::now();
  DAxes da = to_daxes(a); const DGeo dg = to_dgeo(geo, R); const DGrid dG = to_dgrid(g);
  const size_t ncell = (size_t)kCh * a.n_range * a.n_dopp;
  // RD cube: range_doppler()'s own device buffer (double) when given, else the host cf values.
  I.rdf.ensure(ncell * sizeof(float2)); I.mag.ensure(ncell * sizeof(float));
  if (d_rd) k_rd_float<<<I.blocks(ncell), 256, 0, I.stream>>>((const double2*)d_rd, ncell, I.rdf.as<float2>(), I.mag.as<float>());
  else { I.up(I.rdf, (const float2*)R.rd.v.data(), ncell); k_mag<<<I.blocks(ncell), 256, 0, I.stream>>>(I.rdf.as<float2>(), ncell, I.mag.as<float>()); }
  if (!d_E) { I.up(I.E_up, E.data(), E.size()); d_E = I.E_up.as<float>(); }
  std::vector<uint32_t> okc(a.n_dopp + 1, 0); std::vector<uint8_t> okt(a.n_dopp);
  for (uint32_t d = 0; d < a.n_dopp; ++d) { okt[d] = dopp_ok(a, d); okc[d + 1] = okc[d] + okt[d]; }
  I.up(I.okc, okc.data(), okc.size()); I.up(I.okt, okt.data(), okt.size()); da.ok = I.okt.as<uint8_t>();
  I.up(I.tested, a.tested_dopp.data(), nt);
  I.dh.ensure(nv * sizeof(uint32_t));
  k_dh<<<I.blocks(nv), 256, 0, I.stream>>>(da, dg, dG, nv, I.dh.as<uint32_t>());
  std::vector<uint32_t> dh(nv);
  I.down(dh.data(), I.dh, nv); I.sync();
  const uint32_t dh_max = *std::max_element(dh.begin(), dh.end());
  // Null of E at (t,v): scale_t * S(m, n), m = number of searched bins (<= min(2*dh+1, n_dopp)).
  const uint32_t m_max = std::min<uint32_t>(2 * dh_max + 1, a.n_dopp) + 1;
  std::vector<double> nmed(m_max + 1, 0.0), nthr(m_max + 1, 0.0);
  for (uint32_t m = 1; m <= m_max; ++m) { nmed[m] = max_exp_sum_quantile(m, n, 0.5); nthr[m] = max_exp_sum_quantile(m, n, p.pfa); }
  I.up(I.nmed, nmed.data(), nmed.size()); I.up(I.nthr, nthr.data(), nthr.size());
  T.prep_ms = ms_since(t0); t0 = Clock::now();
  // Per-Doppler scale: median over voxels of E / median(S(m, n)) (segmented sort, exact median).
  {
    const size_t N = nt * nv;
    I.zz.ensure(N * sizeof(double)); I.zz_sorted.ensure(N * sizeof(double));
    std::vector<uint64_t> off(nt + 1); for (size_t t = 0; t <= nt; ++t) off[t] = t * nv;
    I.up(I.seg_off, off.data(), off.size());
    k_zz<<<I.blocks(N), 256, 0, I.stream>>>(da, d_E, I.tested.as<uint32_t>(), I.okc.as<uint32_t>(), I.dh.as<uint32_t>(), I.nmed.as<double>(), nt, nv, I.zz.as<double>());
    size_t tb = 0;
    cuda_check(cub::DeviceSegmentedSort::SortKeys(nullptr, tb, I.zz.as<double>(), I.zz_sorted.as<double>(), (long long)N, (long long)nt,
                                                  I.seg_off.as<uint64_t>(), I.seg_off.as<uint64_t>() + 1, I.stream), "cub sort size");
    I.cub_tmp.ensure(tb);
    cuda_check(cub::DeviceSegmentedSort::SortKeys(I.cub_tmp.p, tb, I.zz.as<double>(), I.zz_sorted.as<double>(), (long long)N, (long long)nt,
                                                  I.seg_off.as<uint64_t>(), I.seg_off.as<uint64_t>() + 1, I.stream), "cub sort");
    I.scale.ensure(nt * sizeof(double));
    k_median<<<I.blocks(nt, 64), 64, 0, I.stream>>>(I.zz_sorted.as<double>(), nt, nv, I.scale.as<double>());
  }
  T.scale_ms = ms_since(t0); t0 = Clock::now();
  // Candidates: above threshold and a local maximum over (x, y, z, Doppler); order = (t, v) as on the CPU.
  uint32_t nc = 0;
  {
    const size_t N = nt * nv;
    I.flag.ensure(N); I.cand.ensure(N * sizeof(uint32_t)); I.ncand.ensure(sizeof(uint32_t));
    k_cand<<<I.blocks(N), 256, 0, I.stream>>>(da, dG, d_E, I.tested.as<uint32_t>(), I.okc.as<uint32_t>(), I.dh.as<uint32_t>(), I.nthr.as<double>(),
                                             I.scale.as<double>(), nt, nv, I.flag.as<uint8_t>());
    size_t tb = 0; cub::CountingInputIterator<uint32_t> it(0);
    cuda_check(cub::DeviceSelect::Flagged(nullptr, tb, it, I.flag.as<uint8_t>(), I.cand.as<uint32_t>(), I.ncand.as<uint32_t>(), (int)N, I.stream), "cub select size");
    I.cub_tmp.ensure(tb);
    cuda_check(cub::DeviceSelect::Flagged(I.cub_tmp.p, tb, it, I.flag.as<uint8_t>(), I.cand.as<uint32_t>(), I.ncand.as<uint32_t>(), (int)N, I.stream), "cub select");
    I.down(&nc, I.ncand, 1); I.sync();
  }
  T.cands = nc;
  T.cand_ms = ms_since(t0); t0 = Clock::now();
  // choose() per candidate, one thread each.
  const uint32_t W = 2 * dh_max + 1;
  std::vector<DAcc> c_me(nc); std::vector<double> c_ec(nc), c_thr(nc); std::vector<uint32_t> c_idx(nc);
  if (nc) {
    I.c_me.ensure(nc * sizeof(DAcc)); I.c_ec.ensure(nc * sizeof(double)); I.c_thr.ensure(nc * sizeof(double));
    I.pk_scratch.ensure((size_t)nc * kCh * W * sizeof(DPk));
    k_choose_cand<<<I.blocks(nc, 128), 128, 0, I.stream>>>(da, dg, dG, I.cand.as<uint32_t>(), nc, nv, I.tested.as<uint32_t>(), I.okc.as<uint32_t>(),
                                                          I.dh.as<uint32_t>(), I.nthr.as<double>(), I.scale.as<double>(), I.mag.as<float>(),
                                                          I.pk_scratch.as<DPk>(), W, I.c_me.as<DAcc>(), I.c_ec.as<double>(), I.c_thr.as<double>());
    I.down(c_me.data(), I.c_me, nc); I.down(c_ec.data(), I.c_ec, nc); I.down(c_thr.data(), I.c_thr, nc); I.down(c_idx.data(), I.cand, nc);
    I.sync();
  }
  T.choose_ms = ms_since(t0); t0 = Clock::now();
  const Response rs(R);
  auto first_min = [&](bool dopp, long lim) {
    const double st = 1.0 / 8; double x = st, prev = R.ambiguity(dopp ? 0 : x, dopp ? x : 0);
    for (; x < lim; x += st) { const double v = R.ambiguity(dopp ? 0 : x + st, dopp ? x + st : 0); if (v > prev) break; prev = v; }
    return std::max(1L, (long)std::ceil(x - 1e-9));
  };
  const long hm_r = first_min(false, a.n_range), hm_d = first_min(true, a.n_dopp);
  const double z = -normal_inverse_cdf(p.pfa / 2);
  const double nms_r = 2 * g.step, res = kC / (2 * a.b_eff_hz);
  struct Acc { Vec3 x; std::array<uint32_t, kCh> d{}; std::array<double, kCh> pw{}, bin{}; };
  struct Item { Acc me; size_t t = 0, v = 0; long d = 0; double thr = 0, ec = 0; std::array<double, kCh> leak{}; bool alive = true; };
  std::vector<Item> items;
  const Vec3 hi_box = g.origin + Vec3{(g.nx - 1) * g.step, (g.ny - 1) * g.step, (g.nz - 1) * g.step};
  struct Accepted { Acc s; size_t t = 0, v = 0; long d = 0; double ec = 0; std::array<ChanFit, kCh> fit; };
  std::vector<Accepted> acc;
  auto leak_at = [&](uint32_t i, double bin, uint32_t d) {
    double lk = 0; for (Accepted& pa : acc) lk += leak_amp(rs, pa.fit[i], bin, d, z) / std::sqrt(R.noise[i]);
    return lk;
  };
  auto leak_of = [&](const Acc& me, uint32_t i) { return leak_at(i, me.bin[i], me.d[i]); };
  RdResult Rc; Rc.rd.axes = a; Rc.rd.v = R.rd.v;
  auto rebuild_residual = [&]() {
    Rc.rd.v = R.rd.v;
    for (Accepted& pa : acc) for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i] && pa.fit[i].ok)
      for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d = 0; d < a.n_dopp; ++d)
        Rc.rd.v[Rc.rd.idx(i, m, d)] -= cf(pa.fit[i].A * rs.at(m, d, pa.fit[i].grid[0]));
  };
  auto choose = [&](Acc& me, long d, uint32_t dhv, double thr, bool resid) -> double {
    const long w0 = d - (long)dhv, w1 = d + (long)dhv;
    std::array<std::vector<std::pair<double, uint32_t>>, kCh> pk; std::array<std::vector<double>, kCh> raw;
    std::array<double, kCh> pmax{};
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      me.bin[i] = excess_delay_s(me.x, geo.tx, geo.rx[i]) / a.delay_step_s;
      const MagInterp mi(a.n_range, me.bin[i]);
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
      for (const auto& qq : pk[i]) raw[i].push_back(qq.second >= (uint32_t)std::max(0L, w0) && (long)qq.second <= w1 ? rw[qq.second - w0] : 0.0);
      pmax[i] = pk[i][0].first;
    }
    double sum_max = 0; for (uint32_t i = 0; i < kCh; ++i) sum_max += pmax[i];
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      const double others = sum_max - pmax[i];
      while (pk[i].size() > 1 && !(pk[i].back().first + others > thr)) { pk[i].pop_back(); raw[i].pop_back(); }
    }
    std::array<double, kCh> nu{};
    if (n == kCh) {
      Vec3 gv[kCh]; for (uint32_t i = 0; i < kCh; ++i) gv[i] = grad(geo, i, me.x);
      for (uint32_t i = 0; i < kCh; ++i) {
        Vec3 r3[3]; uint32_t qq = 0; for (uint32_t j = 0; j < kCh; ++j) if (j != i) r3[qq++] = gv[j];
        nu[i] = ((i & 1) ? -1.0 : 1.0) * dot(r3[0], cross(r3[1], r3[2]));
      }
    }
    double nu_abs = 0; for (double v : nu) nu_abs += std::abs(v);
    std::array<double, kCh + 1> rest{};
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
          if (!(e + pk[i][j].first + rest[i + 1] > std::max(ec, thr))) break;
          idx[i] = j;
          run(i + 1, e + pk[i][j].first, cons + nu[i] * (f0 + pk[i][j].second * fstep));
        }
      }
    } dfs{pk, R.los_found, rest, nu, nu_abs, thr, a.dopp0_hz, a.dopp_step_hz, ec, idx, pick};
    dfs.run(0, 0.0, 0.0);
    if (!(ec > thr)) return -1.0;
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) { me.pw[i] = raw[i][pick[i]]; me.d[i] = pk[i][pick[i]].second; }
    return ec;
  };
  if (std::getenv("NR_ISAC_DETECT_CHECK")) {   // debug: device pre-pursuit vs the host transcription
    { std::vector<float> hm(ncell); I.down(hm.data(), I.mag, ncell); I.sync(); size_t bm = 0; double mx = 0;
      for (size_t j = 0; j < ncell; ++j) if (hm[j] != std::abs(R.rd.v[j])) { ++bm; mx = std::max(mx, (double)std::abs(hm[j] - std::abs(R.rd.v[j])) / std::abs(R.rd.v[j])); }
      std::fprintf(stderr, "CHECK mag_bad=%zu/%zu maxrel=%.3g\n", bm, ncell, mx); }
    size_t bad_dh = 0; for (size_t v = 0; v < nv; ++v) bad_dh += dh[v] != dopp_half(a, geo, R.los_found, g.at(v));
    std::vector<double> sc(nt); I.down(sc.data(), I.scale, nt); I.sync();
    size_t bad_sc = 0;
    for (size_t t = 0; t < nt; ++t) {
      std::vector<double> zz(nv);
      for (size_t v = 0; v < nv; ++v) {
        const long d = a.tested_dopp[t]; const long lo = std::max(0L, d - (long)dh[v]), hi = std::min<long>(a.n_dopp - 1, d + dh[v]);
        zz[v] = E[t * nv + v] / nmed[std::max<uint32_t>(1, okc[hi + 1] - okc[lo])];
      }
      const double m = median(std::move(zz)); if (m != sc[t]) { if (bad_sc < 3) std::fprintf(stderr, "CHECK scale t=%zu host=%.17g dev=%.17g\n", t, m, sc[t]); ++bad_sc; }
    }
    size_t bad_c = 0;
    for (uint32_t kc = 0; kc < nc; ++kc) {
      const size_t kt = c_idx[kc] / nv, kv = c_idx[kc] % nv;
      Acc me; me.x = g.at(kv);
      const double ec = choose(me, a.tested_dopp[kt], dh[kv], c_thr[kc], false);
      bool same = std::abs(ec - c_ec[kc]) <= 1e-12 * std::abs(ec);
      if (ec > c_thr[kc]) for (uint32_t i = 0; i < kCh; ++i) same = same && me.d[i] == c_me[kc].d[i] && std::abs(me.pw[i] - c_me[kc].pw[i]) <= 1e-12 * me.pw[i];
      if (!same) { if (bad_c < 5) std::fprintf(stderr, "CHECK cand %u t=%zu v=%zu host ec=%.17g d=%u,%u,%u,%u dev ec=%.17g d=%u,%u,%u,%u dh=%u\n", kc, kt, kv, ec, me.d[0], me.d[1], me.d[2], me.d[3],
                                c_ec[kc], c_me[kc].d[0], c_me[kc].d[1], c_me[kc].d[2], c_me[kc].d[3], dh[kv]); ++bad_c; }
    }
    std::fprintf(stderr, "CHECK dh_bad=%zu scale_bad=%zu choose_bad=%zu/%u\n", bad_dh, bad_sc, bad_c, nc);
  }
  std::map<std::tuple<size_t, uint32_t, uint32_t, uint32_t, uint32_t>, size_t> seen;
  for (uint32_t kc = 0; kc < nc; ++kc) {
    const size_t kt = c_idx[kc] / nv, kv = c_idx[kc] % nv; const double kthr = c_thr[kc], ec = c_ec[kc];
    if (!(ec > kthr)) continue;
    Acc me; const DAcc& dm = c_me[kc];
    me.x = Vec3{dm.x[0], dm.x[1], dm.x[2]};
    for (uint32_t i = 0; i < kCh; ++i) { me.d[i] = dm.d[i]; me.pw[i] = dm.pw[i]; me.bin[i] = dm.bin[i]; }
    const auto key = std::make_tuple(kv, me.d[0], me.d[1], me.d[2], me.d[3]);
    const auto f = seen.find(key);
    if (f != seen.end()) {
      Item& o = items[f->second];
      if (ec - kthr > o.ec - o.thr) { o.t = kt; o.d = a.tested_dopp[kt]; o.thr = kthr; o.ec = ec; }
      continue;
    }
    seen.emplace(key, items.size());
    Item it; it.me = me; it.t = kt; it.v = kv; it.d = a.tested_dopp[kt]; it.thr = kthr; it.ec = ec;
    items.push_back(it);
  }
  T.items = (uint32_t)items.size();
  T.items_ms = ms_since(t0); t0 = Clock::now();
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
    const std::array<double, kCh> bin0 = me.bin;
    auto inside = [&](const Vec3& y) {
      if (y.x < g.origin.x || y.y < g.origin.y || y.z < std::max(0.0, g.origin.z) || y.x > hi_box.x || y.y > hi_box.y || y.z > hi_box.z) return false;
      for (uint32_t i = 0; i < kCh; ++i)
        if (R.los_found[i] && std::abs(excess_delay_s(y, geo.tx, geo.rx[i]) / a.delay_step_s - bin0[i]) > hm_r) return false;
      return true;
    };
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
  auto refit_all = [&]() {
    for (int sweep = 0; sweep < 10; ++sweep) {
      ++T.sweeps;
      bool moved = false;
      for (size_t j = 0; j < acc.size(); ++j)
        for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
          std::vector<ChanFit*> oth;
          for (size_t o = 0; o < acc.size(); ++o) if (o != j && acc[o].fit[i].ok) oth.push_back(&acc[o].fit[i]);
          ChanFit& f = acc[j].fit[i];
          ChanFit nf = fit_channel(rs, i, f.p, f.fb, hm_r, hm_d, z, oth); ++T.fits;
          const double sc = nf.ok ? std::sqrt(std::max(1.0, std::norm(nf.A) / R.noise[i] / std::max(nf.snr, 1e-300))) : 1.0;
          if (nf.ok && (!f.ok || std::abs(nf.p - f.p) > sc * nf.sp || std::abs(nf.fb - f.fb) > sc * nf.sf)) moved = true;
          f = std::move(nf);
        }
      if (!moved) break;
    }
  };
  // ---- device state for the pursuit ----
  const uint32_t nrow = (uint32_t)rs.rows.size(), sc = R.wf.sc, nr = a.n_range, nd = a.n_dopp;
  const size_t ncc = (size_t)nr * nd, per = (size_t)kMaxGrid * nrow, sper = (size_t)nd * nr;
  DResp dr{};
  {
    std::vector<double> w(nrow), t(nrow), fcv(nrow); std::vector<int32_t> gg(nrow);
    for (uint32_t j = 0; j < nrow; ++j) { const uint32_t r = rs.rows[j]; w[j] = R.wf.w[r]; t[j] = a.row_t_s[r]; fcv[j] = R.wf.fc[r]; gg[j] = R.wf.grp[r]; }
    const uint32_t Blen = R.wf.B.empty() ? 0 : (uint32_t)R.wf.B[0].size();
    std::vector<double2> B((size_t)R.wf.B.size() * Blen);
    for (size_t q = 0; q < R.wf.B.size(); ++q) for (uint32_t u = 0; u < Blen; ++u) B[q * Blen + u] = make_double2(R.wf.B[q][u].real(), R.wf.B[q][u].imag());
    I.up(I.r_w, w.data(), nrow); I.up(I.r_t, t.data(), nrow); I.up(I.r_fc, fcv.data(), nrow); I.up(I.r_g, gg.data(), nrow); I.up(I.r_B, B.data(), B.size());
    I.r_ed.ensure((size_t)nd * nrow * sizeof(double2)); I.r_em.ensure((size_t)nr * nrow * sizeof(double2));
    k_resp<<<I.blocks((size_t)(nd + nr) * nrow), 256, 0, I.stream>>>(da, nrow, I.r_w.as<double>(), R.wf.wsum, I.r_t.as<double>(), I.r_fc.as<double>(),
                                                                   I.r_ed.as<double2>(), I.r_em.as<double2>());
    dr = DResp{nrow, Blen, R.wf.X, rs.tbar, I.r_ed.as<double2>(), I.r_em.as<double2>(), I.r_fc.as<double>(), I.r_t.as<double>(), I.r_g.as<int32_t>(), I.r_B.as<double2>()};
    I.up(I.Q, (const float2*)R.wf.Q.data(), R.wf.Q.size());
    I.rcf.ensure(ncell * sizeof(float2)); I.magc.ensure(ncell * sizeof(float));
    cuda_check(cudaMemcpyAsync(I.rcf.p, I.rdf.p, ncell * sizeof(float2), cudaMemcpyDeviceToDevice, I.stream), "D2D rc");
    cuda_check(cudaMemcpyAsync(I.magc.p, I.mag.p, ncell * sizeof(float), cudaMemcpyDeviceToDevice, I.stream), "D2D magc");
  }
  // fit statics: row masks/spans/times (set_M), the RD noise correlation by cell offset (sandwich covariance)
  const uint32_t nrow_all = (uint32_t)R.wf.grp.size();
  FitArgs fa{};
  {
    std::vector<double> trow(nrow_all); for (uint32_t r = 0; r < nrow_all; ++r) trow[r] = a.row_t_s[r];
    I.up(I.w_mask, R.wf.mask.data(), R.wf.mask.size()); I.up(I.w_lo, R.wf.lo.data(), nrow_all); I.up(I.w_hi, R.wf.hi.data(), nrow_all);
    I.up(I.w_trow, trow.data(), nrow_all);
    const long hr = 2 * hm_r, hd = 2 * hm_d;
    std::vector<double2> gam((size_t)(2 * hr + 1) * (2 * hd + 1));
    for (long om = -hr; om <= hr; ++om) for (long od = -hd; od <= hd; ++od) {
      const cd v = R.noise_corr((double)om, (double)od); gam[(size_t)((om + hr) * (2 * hd + 1) + od + hd)] = make_double2(v.real(), v.imag());
    }
    I.up(I.gam, gam.data(), gam.size());
    I.moved.ensure(sizeof(int));
    fa.a = da; fa.rs = dr; fa.g = dg; fa.rdf = I.rdf.as<float2>(); fa.Q = I.Q.as<float2>(); fa.mask = I.w_mask.as<uint8_t>();
    fa.lo = I.w_lo.as<uint32_t>(); fa.hi = I.w_hi.as<uint32_t>(); fa.trow = I.w_trow.as<double>(); fa.nrow_all = nrow_all; fa.sc = sc;
    fa.per = (uint32_t)per; fa.nc = (uint32_t)ncc; fa.sper = (uint32_t)sper;
    fa.hm_r = hm_r; fa.hm_d = hm_d; fa.z = z; fa.gam = I.gam.as<double2>(); fa.ghr = hr; fa.ghd = hd;
    fa.Nmax = (uint32_t)((2 * hm_r + 1) * (2 * hm_d + 1));
  }
  auto fit_ptrs = [&](FitArgs& f) {
    f.F = I.F.as<DFit>(); f.gpp = I.gpp.as<double2>(); f.gpr = I.gpr.as<double>(); f.dir = I.dir.as<double2>(); f.stat = I.stat.as<double2>(); f.Mf = I.Mf.as<double2>();
  };
  std::vector<DItem> ditems(items.size());
  auto to_ditem = [&](const Item& it, DItem* o) {
    for (int q = 0; q < 3; ++q) o->me.x[q] = q == 0 ? it.me.x.x : (q == 1 ? it.me.x.y : it.me.x.z);
    for (uint32_t i = 0; i < kCh; ++i) { o->me.d[i] = it.me.d[i]; o->me.pw[i] = it.me.pw[i]; o->me.bin[i] = it.me.bin[i]; }
    o->thr = it.thr; o->d = (int32_t)it.d; o->dh = dh[it.v]; o->alive = it.alive;
  };
  auto from_ditem = [&](const DItem& o, Item* it) {
    it->me.x = Vec3{o.me.x[0], o.me.x[1], o.me.x[2]};
    for (uint32_t i = 0; i < kCh; ++i) { it->me.d[i] = o.me.d[i]; it->me.pw[i] = o.me.pw[i]; it->me.bin[i] = o.me.bin[i]; it->leak[i] = o.leak[i]; }
    it->alive = o.alive;
  };
  for (size_t q = 0; q < items.size(); ++q) {
    DItem& o = ditems[q]; o = DItem{}; to_ditem(items[q], &o);
    for (uint32_t i = 0; i < kCh; ++i) { o.leak[i] = 0; o.leak_d[i] = o.me.d[i]; o.leak_n[i] = 0; }
  }
  I.up(I.items, ditems.data(), ditems.size());
  if (!items.empty()) I.pk_scratch.ensure(std::max<size_t>(I.pk_scratch.cap, items.size() * kCh * W * sizeof(DPk)));
  I.fit_slots_reserve(4, per, ncc, sper, sc);
  auto tabs = [&]() { return DFitTabs{I.F.as<DFit>(), I.gpp.as<double2>(), I.dir.as<double2>(), I.stat.as<double2>(), I.gpr.as<double>(), (uint32_t)per, (uint32_t)ncc, (uint32_t)sper}; };
  DItem* d_items = nullptr;
  for (;;) {
    auto tq = Clock::now();
    long pick = -1; double best = 0;
    for (size_t qi = 0; qi < items.size(); ++qi) {
      Item& it = items[qi]; if (!it.alive) continue;
      double resid = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) resid += std::pow(std::max(0.0, std::sqrt(it.me.pw[i]) - it.leak[i]), 2);
      if (resid <= it.thr) { it.alive = false; continue; }
      if (resid > best) { best = resid; pick = (long)qi; }
    }
    T.pick_ms += ms_since(tq);
    if (pick < 0) break;
    ++T.rounds;
    Item& k = items[pick]; k.alive = false;
    tq = Clock::now();
    k.ec = walk(k.me);
    {
      // Leakage of every accepted detection at the walked cells (device, one item, full sum).
      DItem one{}; to_ditem(k, &one); one.alive = 1;
      for (uint32_t i = 0; i < kCh; ++i) { one.leak[i] = 0; one.leak_d[i] = one.me.d[i]; one.leak_n[i] = 0; }
      I.up(I.one, &one, 1);
      k_leak<<<1, 32 * kCh, 0, I.stream>>>(da, dg, dr, tabs(), I.one.as<DItem>(), 1, (uint32_t)acc.size(), z, 1);
      I.down(&one, I.one, 1); I.sync();
      double resid = 0;
      for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) resid += std::pow(std::max(0.0, std::sqrt(k.me.pw[i]) - one.leak[i]), 2);
      T.walk_ms += ms_since(tq);
      if (resid <= k.thr) continue;
    }
    tq = Clock::now();
    Accepted na; na.s = k.me; na.t = k.t; na.v = k.v; na.d = k.d; na.ec = k.ec;
    const uint32_t a_idx = (uint32_t)acc.size();
    I.fit_slots_reserve((a_idx + 1) * kCh, per, ncc, sper, sc);
    std::vector<FitJob> jb;
    {
      std::vector<DFit> nf(kCh);
      for (uint32_t i = 0; i < kCh; ++i) { nf[i] = DFit{}; nf[i].ch = (int32_t)i; }
      cuda_check(cudaMemcpyAsync(I.F.as<DFit>() + (size_t)a_idx * kCh, nf.data(), kCh * sizeof(DFit), cudaMemcpyHostToDevice, I.stream), "H2D fits");
    }
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) {
      // Each channel's path starts from its own RD peak near the position-implied delay.
      double pb = k.me.bin[i], bv = -1; cd sv;
      for (double b = k.me.bin[i] - hm_r; b <= k.me.bin[i] + hm_r; b += 1.0 / 16)
        if (sample_rd(R, i, b, k.me.d[i], &sv) && std::norm(sv) > bv) { bv = std::norm(sv); pb = b; }
      jb.push_back(FitJob{(int32_t)i, (int32_t)a_idx, 0, 0, pb, (double)k.me.d[i]}); ++T.fits;
    }
    acc.push_back(std::move(na));
    {
      FitArgs f = fa; fit_ptrs(f); f.tables = 1; f.nacc = a_idx;
      I.launch_fits(f, jb, nrow_all, da, dr, per);
      I.build_fit_tables(da, dr, (uint32_t)jb.size(), sc, per, ncc, sper);
    }
    T.fit_ms += ms_since(tq); tq = Clock::now();
    k_rebuild<<<I.blocks(jb.size() * ncc), 256, 0, I.stream>>>(da, I.F.as<DFit>(), I.slots.as<uint32_t>(), (uint32_t)jb.size(), I.dir.as<double2>(), I.rcf.as<float2>(), I.magc.as<float>());
    T.rebuild_ms += ms_since(tq); tq = Clock::now();
    {
      const Acc& sa = acc.back().s; DAcc ds{};
      ds.x[0] = sa.x.x; ds.x[1] = sa.x.y; ds.x[2] = sa.x.z;
      for (uint32_t i = 0; i < kCh; ++i) { ds.d[i] = sa.d[i]; ds.pw[i] = sa.pw[i]; ds.bin[i] = sa.bin[i]; }
      // host alive flags (the pick scan and the pick itself kill items) -> device
      for (size_t q = 0; q < items.size(); ++q) ditems[q].alive = items[q].alive;
      I.set_alive(ditems);
      d_items = I.items.as<DItem>();
      const uint32_t ni = (uint32_t)items.size();
      if (ni) {
        k_rescore<<<I.blocks(ni, 64), 64, 0, I.stream>>>(da, dg, ds, nms_r, d_items, ni, I.mag.as<float>(), I.magc.as<float>(), I.pk_scratch.as<DPk>(), W);
        k_leak<<<I.blocks((size_t)ni * kCh * 32, 128), 128, 0, I.stream>>>(da, dg, dr, tabs(), d_items, ni, (uint32_t)acc.size(), z, 0);
        I.down(ditems.data(), I.items, ni); I.sync();
        for (size_t q = 0; q < items.size(); ++q) from_ditem(ditems[q], &items[q]);
      }
    }
    T.rescore_ms += ms_since(tq);
  }
  T.accepted = (uint32_t)acc.size();
  T.pursuit_ms = ms_since(t0); t0 = Clock::now();
  // Joint refit of every accepted detection, one path at a time with the others subtracted, until no
  // fitted delay or Doppler moves by more than its own standard error (at most 10 sweeps). A sweep is
  // queued without a host round trip; the host reads the convergence flag once per sweep.
  if (!acc.empty()) {
    for (int sweep = 0; sweep < 10; ++sweep) {
      ++T.sweeps;
      cuda_check(cudaMemsetAsync(I.moved.p, 0, sizeof(int), I.stream), "memset moved");
      for (size_t j = 0; j < acc.size(); ++j) {
        std::vector<FitJob> jb;
        for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) { jb.push_back(FitJob{(int32_t)i, (int32_t)j, 1, 0, 0.0, 0.0}); ++T.fits; }
        FitArgs f = fa; fit_ptrs(f); f.tables = 0; f.nacc = (uint32_t)acc.size();
        I.launch_fits(f, jb, nrow_all, da, dr, per);
      }
      int moved = 0; I.down(&moved, I.moved, 1); I.sync();
      if (!moved) break;
    }
    std::vector<DFit> hf(acc.size() * kCh);
    I.down(hf.data(), I.F, hf.size()); I.sync();
    for (size_t j = 0; j < acc.size(); ++j) for (uint32_t i = 0; i < kCh; ++i) {
      const DFit& o = hf[j * kCh + i]; ChanFit& f = acc[j].fit[i];
      f.ok = R.los_found[i] && o.ok; f.p = o.p; f.fb = o.fb; f.snr = o.snr; f.A = cd(o.A.x, o.A.y); f.sp = o.sp; f.sf = o.sf; f.sA = o.sA;
    }
  }
  T.refit_ms = ms_since(t0); t0 = Clock::now();
  for (Accepted& k : acc) {
    const Acc& s = k.s; std::array<ChanFit, kCh>& fit = k.fit;
    std::array<double, kCh> wls{};
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
    det.doppler_hz = fsum / n;
    det.range_rate_mps = -a.lambda_m * det.doppler_hz;
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
  T.finish_ms = ms_since(t0);
  T.total_ms = ms_since(t_all);
  if (std::getenv("NR_ISAC_DETECT_PROFILE"))
    std::fprintf(stderr, "DETECTPROF total=%.1f prep=%.1f scale=%.1f cand=%.1f choose=%.1f items=%.1f pursuit=%.1f [pick=%.1f walk=%.1f fit=%.1f rebuild=%.1f rescore=%.1f] refit=%.1f finish=%.1f cands=%u items=%u rounds=%u acc=%u fits=%u sweeps=%u jevals=%llu hm=%ld,%ld rows=%zu nr=%u nd=%u nt=%zu nv=%zu\n",
                 T.total_ms, T.prep_ms, T.scale_ms, T.cand_ms, T.choose_ms, T.items_ms, T.pursuit_ms, T.pick_ms, T.walk_ms, T.fit_ms, T.rebuild_ms,
                 T.rescore_ms, T.refit_ms, T.finish_ms, T.cands, T.items, T.rounds, T.accepted, T.fits, T.sweeps, (unsigned long long)g_j_evals,
                 hm_r, hm_d, rs.rows.size(), a.n_range, a.n_dopp, nt, nv);
  g_j_evals = 0;
  return out;
}

} // namespace nr_isac::coherent
