/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* GPU-resident coherent detect() -- see coherent_cuda_detect.h. Every formula is a transcription of
 * coherent_core.cc's detect() and its helpers (Response, fit_channel, static_term, leak_amp, choose,
 * walk, refit_all), which stay the CPU oracle. The small pure helpers are duplicated here because
 * coherent_core.cc keeps them in an anonymous namespace.
 *
 * Precision (measured on two OTA recordings, see gpu-exact-report.md): everything that feeds a decision
 * or a fit is FP64 and summed in the oracle's order where the order is sequential (rows of a path
 * response, cells of a fit's J). On real CPIs the oracle is itself rounding-determined in places (a
 * Newton step accepted on a J comparison at the rounding level, a joint refit stopped at its sweep cap,
 * a Gauss-Newton position far outside the volume): there the oracle's own source rebuilt without FMA
 * contraction, or re-run on 1-ulp-perturbed inputs, disagrees with it as often as this path does, and
 * tests/coherent_cuda_parity_test.cc classifies such outputs against that rounding ensemble. */
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
bool dopp_ok(const Axes& a, long d)
{
  return d >= 0 && d < (long)a.n_dopp && std::abs(a.dopp0_hz + d * a.dopp_step_hz) > a.notch_half_bins * a.dopp_step_hz;
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
// The rows that carry a mask group (Response in coherent_core.cc) and their weighted mid-time tbar.
struct Response {
  std::vector<uint32_t> rows;
  double tbar = 0;
  explicit Response(const RdResult& R)
  {
    const auto& wf = R.wf; const Axes& a = R.rd.axes;
    for (uint32_t r = 0; r < wf.grp.size(); ++r) if (wf.grp[r] >= 0) rows.push_back(r);
    double ws = 0; for (uint32_t r : rows) { tbar += wf.w[r] * a.row_t_s[r]; ws += wf.w[r]; }
    if (ws > 0) tbar /= ws;
  }
};
// One channel of an accepted detection's final fit (coherent_core.cc's ChanFit, the fields detect() reports).
struct ChanFit { double p = 0, fb = 0, sp = 0, sf = 0, sA = 0, snr = 0; cd A = 0; bool ok = false; };


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
// coherent_core.cc's choose(), by ONE WARP: per channel the local Doppler maxima of the (magc ? residual :
// raw) column inside +-dhv of d, sorted descending (stable: the order the oracle's per-channel lists come
// out in on every recorded candidate), pruned by the bound; then the oracle's exact branch-and-bound DFS
// over velocity-consistent combinations, run by lane 0 on the lists in shared memory. Scratch per warp:
// choose_smem(W) bytes. Returns ec (every lane) or -1 (me.d/me.pw untouched then; me.bin always set).
__host__ __device__ inline size_t choose_smem(uint32_t W) { return (size_t)W * (2 * sizeof(double) + (kCh + 1) * sizeof(DPk)) + 2 * kCh * sizeof(uint32_t); }
__device__ double w_choose(const DAxes& a, const DGeo& g, DAcc& me, long d, uint32_t dhv, double thr, const float* mag, const float* magc,
                           char* scr, uint32_t W)
{
  const uint32_t lane = threadIdx.x & 31;
  double* col = (double*)scr; double* raw = col + W; DPk* pk = (DPk*)(raw + W); DPk* tmp = pk + kCh * W; uint32_t* np = (uint32_t*)(tmp + W);
  const long w0 = d - (long)dhv, w1 = d + (long)dhv; const uint32_t nw = (uint32_t)(w1 - w0 + 1);
  const uint32_t nr = a.n_range, nd = a.n_dopp;
  double pmax[kCh] = {0, 0, 0, 0};
  for (uint32_t i = 0; i < kCh; ++i) {
    if (!g.used[i]) { if (lane == 0) np[i] = 0; continue; }
    me.bin[i] = d_bin(g, a, me.x, i);
    const DMag mi(nr, me.bin[i]);
    for (uint32_t q = lane; q < nw; q += 32) {
      const long e = w0 + (long)q; double c = -1.0, rw = 0.0;
      if (mi.ok && d_dopp_ok(a, e)) {
        const double m = mi.at(mag, nr, nd, i, (uint32_t)e); rw = m * m / g.noise[i];
        const double mr = magc ? mi.at(magc, nr, nd, i, (uint32_t)e) : m;
        c = mr * mr / g.noise[i];
      }
      col[q] = c; raw[q] = rw;
    }
    __syncwarp();
    // peaks, compacted in increasing e (ballot + prefix count)
    DPk* P = pk + (size_t)i * W; uint32_t base = 0;
    for (uint32_t q0 = 0; q0 < nw; q0 += 32) {
      const uint32_t q = q0 + lane;
      bool pkq = false;
      if (q < nw) { const double v = col[q]; pkq = v >= 0 && (q == 0 || v >= col[q - 1]) && (q == nw - 1 || v >= col[q + 1]); }
      const unsigned m = __ballot_sync(0xffffffffu, pkq);
      if (pkq) { const uint32_t o = base + __popc(m & ((1u << lane) - 1)); P[o] = DPk{col[q], raw[q], (uint32_t)(w0 + (long)q)}; }
      base += __popc(m);
    }
    __syncwarp();
    // stable descending sort: each lane ranks its peaks (#greater + #equal-before), then scatters them
    if (base > 1) {
      for (uint32_t j = lane; j < base; j += 32) tmp[j] = P[j];
      __syncwarp();
      for (uint32_t j = lane; j < base; j += 32) {
        const double v = tmp[j].v; uint32_t r = 0;
        for (uint32_t k = 0; k < base; ++k) { const double u = tmp[k].v; r += (u > v) || (u == v && k < j); }
        P[r] = tmp[j];
      }
      __syncwarp();
    }
    if (lane == 0) {
      if (base == 0) {
        const long dc = d < 0 ? 0 : (d > (long)nd - 1 ? (long)nd - 1 : d);
        P[0] = DPk{0.0, (dc >= w0 && dc <= w1) ? raw[dc - w0] : 0.0, (uint32_t)dc}; base = 1;
      }
      np[i] = base;
    }
    __syncwarp();
    pmax[i] = P[0].v;
  }
  // Pruning, velocity-consistency weights (lane 0), then the branch-and-bound over the used channels:
  // the (first, second) level pairs are shared out to the lanes in lexicographic order and every lane
  // prunes against the best energy found by ANY lane (shared); each lane keeps its lexicographically
  // first maximum, and the warp takes the lexicographically first among the maxima. That is the
  // sequential DFS's answer (it returns the lexicographically first combination of maximal energy:
  // a branch is only pruned when it cannot beat an energy already found) up to exact ties of two
  // different combinations' summed energies.
  __shared__ unsigned long long sh_best[32];   // per warp in the block (<= 32 warps)
  __shared__ unsigned sh_next[32];
  __shared__ double sh_nu[32][kCh];
  __shared__ uint32_t sh_n[32][kCh];
  const uint32_t wib = threadIdx.x >> 5;
  if (lane == 0) {
    uint32_t n[kCh]; for (uint32_t i = 0; i < kCh; ++i) n[i] = np[i];
    double sum_max = 0; for (uint32_t i = 0; i < kCh; ++i) sum_max += pmax[i];
    for (uint32_t i = 0; i < kCh; ++i) if (g.used[i]) {
      const double others = sum_max - pmax[i]; const DPk* P = pk + (size_t)i * W;
      while (n[i] > 1 && !(P[n[i] - 1].v + others > thr)) --n[i];
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
    for (uint32_t i = 0; i < kCh; ++i) { sh_nu[wib][i] = nu[i]; sh_n[wib][i] = n[i]; }
    sh_best[wib] = __double_as_longlong(thr); sh_next[wib] = 0;
  }
  __syncwarp();
  double nu[kCh], nu_abs = 0; uint32_t n[kCh];
  for (uint32_t i = 0; i < kCh; ++i) { nu[i] = sh_nu[wib][i]; n[i] = sh_n[wib][i]; nu_abs += fabs(nu[i]); }
  double rest[kCh + 1]; rest[kCh] = 0;
  for (int i = kCh - 1; i >= 0; --i) rest[i] = rest[i + 1] + (g.used[i] ? pmax[i] : 0.0);
  uint32_t L[kCh], nL = 0; for (uint32_t i = 0; i < kCh; ++i) if (g.used[i]) L[nL++] = i;
  const double f0 = a.dopp0_hz, fstep = a.dopp_step_hz;
  double my_e = -1; uint32_t my_pick[kCh] = {0, 0, 0, 0};
  volatile unsigned long long* best = &sh_best[wib];
  auto leaf = [&](double e, double c, const uint32_t* idx) {
    if (!(nu_abs > 0 && fabs(c) > fstep * nu_abs) && e > my_e && e > thr) {
      my_e = e; for (uint32_t i = 0; i < kCh; ++i) my_pick[i] = idx[i];
      atomicMax((unsigned long long*)best, (unsigned long long)__double_as_longlong(e));
    }
  };
  if (nL == 0) { if (lane == 0) { const uint32_t z[kCh] = {0, 0, 0, 0}; leaf(0.0, 0.0, z); } }
  else {
    const uint32_t n0 = n[L[0]], n1 = nL > 1 ? n[L[1]] : 1, units = n0 * n1;
    for (;;) {
      const unsigned u = lane == 0 ? atomicAdd(&sh_next[wib], 32u) : 0;   // 32 units per warp step, lane-interleaved
      const unsigned ub = __shfl_sync(0xffffffffu, u, 0);
      if (ub >= units) break;
      const unsigned my = ub + lane;
      if (my < units) {
        const uint32_t j0 = my / n1, j1 = my % n1;
        uint32_t idx[kCh] = {0, 0, 0, 0};
        const DPk* P0 = pk + (size_t)L[0] * W;
        if (P0[j0].v + rest[L[0] + 1] > __longlong_as_double((long long)*best)) {
          idx[L[0]] = j0;
          double es1 = 0.0 + P0[j0].v, cs1 = 0.0 + nu[L[0]] * (f0 + P0[j0].d * fstep);
          if (nL == 1) leaf(es1, cs1, idx);
          else {
            const DPk* P1 = pk + (size_t)L[1] * W;
            if (es1 + P1[j1].v + rest[L[1] + 1] > __longlong_as_double((long long)*best)) {
              idx[L[1]] = j1;
              const double es2 = es1 + P1[j1].v, cs2 = cs1 + nu[L[1]] * (f0 + P1[j1].d * fstep);
              if (nL == 2) leaf(es2, cs2, idx);
              else {
                // levels 2.. (at most two more): sequential DFS with the same bound and break
                double es[kCh + 1], cs[kCh + 1]; es[2] = es2; cs[2] = cs2;
                int lvl = 2; uint32_t jn[kCh + 1] = {0, 0, 0, 0, 0};
                while (lvl >= 2) {
                  if (lvl == (int)nL) { leaf(es[lvl], cs[lvl], idx); --lvl; continue; }
                  const uint32_t ch = L[lvl], j = jn[lvl]; const DPk* P = pk + (size_t)ch * W;
                  if (j < n[ch] && es[lvl] + P[j].v + rest[ch + 1] > __longlong_as_double((long long)*best)) {
                    jn[lvl] = j + 1; idx[ch] = j;
                    es[lvl + 1] = es[lvl] + P[j].v; cs[lvl + 1] = cs[lvl] + nu[ch] * (f0 + P[j].d * fstep);
                    jn[lvl + 1] = 0; ++lvl;
                  } else { jn[lvl] = 0; --lvl; }
                }
              }
            }
          }
        }
      }
    }
  }
  // warp reduction: largest energy, ties -> lexicographically first combination
  double ec = my_e; uint32_t pick[kCh]; for (uint32_t i = 0; i < kCh; ++i) pick[i] = my_pick[i];
  for (int o = 16; o > 0; o >>= 1) {
    const double oe = __shfl_xor_sync(0xffffffffu, ec, o);
    uint32_t op[kCh]; for (uint32_t i = 0; i < kCh; ++i) op[i] = __shfl_xor_sync(0xffffffffu, pick[i], o);
    bool take = oe > ec;
    if (oe == ec) { int c = 0; for (uint32_t i = 0; i < kCh && !c; ++i) c = op[i] < pick[i] ? -1 : (op[i] > pick[i] ? 1 : 0); take = c < 0; }
    if (take) { ec = oe; for (uint32_t i = 0; i < kCh; ++i) pick[i] = op[i]; }
  }
  if (lane == 0 && ec > thr) for (uint32_t i = 0; i < kCh; ++i) if (g.used[i]) { const DPk& q = pk[(size_t)i * W + pick[i]]; me.pw[i] = q.raw; me.d[i] = q.d; }
  ec = __shfl_sync(0xffffffffu, ec, 0);
  for (uint32_t i = 0; i < kCh; ++i) { me.pw[i] = __shfl_sync(0xffffffffu, me.pw[i], 0); me.d[i] = __shfl_sync(0xffffffffu, me.d[i], 0); }
  __syncwarp();
  return ec > thr ? ec : -1.0;
}
// One warp per candidate (blockDim = 32 * warps per block; dynamic smem = warps * choose_smem(W)).
__global__ void k_choose_cand(DAxes a, DGeo g, DGrid G, const uint32_t* cand, uint32_t nc, size_t nv, const uint32_t* tested,
                              const uint32_t* okc, const uint32_t* dh, const double* nthr, const double* scale, const float* mag,
                              uint32_t W, DAcc* out_me, double* out_ec, double* out_thr)
{
  extern __shared__ __align__(16) char csm[];
  const uint32_t k = (blockIdx.x * blockDim.x + threadIdx.x) >> 5;
  if (k >= nc) return;
  char* scr = csm + (threadIdx.x >> 5) * choose_smem(W);
  const size_t t = cand[k] / nv, v = cand[k] % nv;
  const long d = tested[t];
  const double thr = scale[t] * nthr[d_m_of(a, okc, d, dh[v])];
  DAcc me; d_voxel(G, v, me.x);
  for (uint32_t i = 0; i < kCh; ++i) { me.d[i] = 0; me.pw[i] = 0; me.bin[i] = 0; }
  const double ec = w_choose(a, g, me, d, dh[v], thr, mag, nullptr, scr, W);
  if ((threadIdx.x & 31) == 0) { out_ec[k] = ec; out_me[k] = me; out_thr[k] = thr; }
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
// leak_amp() of one fit at (bin, d), by one warp: the (grid point, range bin) responses are spread over
// the lanes, each a sequential row sum in the oracle's order (Response::at); shv: 4 * kMaxGrid doubles.
__device__ double d_leak_amp(const DAxes& a, const DResp& rs, const DFitTabs& T, uint32_t s, double bin, uint32_t d, double z, double* shv)
{
  const DFit& f = T.F[s];
  if (!f.ok) return 0.0;
  const uint32_t n = a.n_range, lane = threadIdx.x & 31;
  if (n == 0 || !(bin >= 0) || bin > (double)(n - 1)) return 0.0;
  const long b0 = (long)bin; const double t = bin - b0;
  uint32_t b[4]; for (int j = 0; j < 4; ++j) { const long q = b0 - 1 + j; b[j] = (uint32_t)(q < 0 ? 0 : (q > (long)n - 1 ? (long)n - 1 : q)); }
  const double2* st = T.stat + (size_t)s * T.sper + (size_t)d * n;
  for (uint32_t c = lane; c < (uint32_t)f.ngrid * 4; c += 32) {
    const uint32_t q = c / 4, jb = c % 4; double2 at;
    if (q == 0) at = T.dir[(size_t)s * T.nc + (size_t)b[jb] * a.n_dopp + d];
    else {
      const double2* pp = T.gpp + (size_t)s * T.per + (size_t)q * rs.nrow; const double* pr = T.gpr + (size_t)s * T.per + (size_t)q * rs.nrow;
      at = make_double2(0, 0);
      for (uint32_t j = 0; j < rs.nrow; ++j) at = cadd(at, d_term(rs, b[jb], d, j, pp[j], pr[j]));
    }
    shv[c] = cabs2(cadd(at, st[b[jb]]));
  }
  __syncwarp();
  double best = 0;
  for (int q = 0; q < f.ngrid; ++q) {
    const double* v = shv + 4 * q; const double tt = t;
    const double c4 = -v[0] * tt * (tt - 1) * (tt - 2) / 6 + v[1] * (tt + 1) * (tt - 1) * (tt - 2) / 2
                      - v[2] * (tt + 1) * tt * (tt - 2) / 2 + v[3] * (tt + 1) * tt * (tt - 1) / 6;
    best = fmax(best, c4);
  }
  __syncwarp();
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
  __shared__ double shv[32][4 * kMaxGrid];   // per warp of the block (blockDim <= 1024)
  const double sn = sqrt(g.noise[i]);
  for (uint32_t o = from; o < nacc; ++o) lk += d_leak_amp(a, rs, T, o * kCh + i, it.me.bin[i], it.me.d[i], z, shv[threadIdx.x >> 5]) / sn;
  if ((threadIdx.x & 31) == 0) { it.leak[i] = lk; it.leak_d[i] = it.me.d[i]; it.leak_n[i] = nacc; }
}
// Re-score after an accept, one warp per item: NMS / harmonic merge against the accepted s, then choose()
// on the residual.
__global__ void k_rescore(DAxes a, DGeo g, DAcc s, double nms_r, DItem* items, uint32_t ni, const float* mag, const float* magc, uint32_t W)
{
  extern __shared__ __align__(16) char csm[];
  const uint32_t k = (blockIdx.x * blockDim.x + threadIdx.x) >> 5;
  if (k >= ni) return;
  char* scr = csm + (threadIdx.x >> 5) * choose_smem(W);
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
  if (local && (same || harm)) { if ((threadIdx.x & 31) == 0) it.alive = 0; return; }
  DAcc me = it.me;
  const double ec = w_choose(a, g, me, it.d, it.dh, it.thr, mag, magc, scr, W);
  if ((threadIdx.x & 31) == 0) { if (!(ec > it.thr)) it.alive = 0; else it.me = me; }
}

// ---------------- the per-channel path fit (coherent_core.cc's fit_channel) ----------------
// One cooperative launch fits up to kCh jobs (one per channel), B blocks per job. The optimiser's
// control flow is replicated in every block of a job (deterministic from shared results); each batch
// of J evaluations is spread over the job's blocks and closed by a grid barrier. Same evaluation
// points in the same order as the CPU, hence the same decisions.
struct FitJob { int32_t ch, own, from_slot, pad; double p0, fb0; };
constexpr int kMaxPts = 9;
constexpr int kResaPer = 64;   // per job: 2 x kMaxPts model vectors, or the refit's N x 64 path values
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
  int2* cells; double2 *y0, *y, *resA, *resa; double* resJ; double2* Mmid; double2* Z; uint32_t Nmax;
  unsigned long long* stats;   // stats: {barriers, fits} (profiling)
  unsigned* bar; int* moved;   // bar: per job {arrival count, generation} of the job-local barrier
};
size_t fit_smem_bytes(uint32_t nrow, uint32_t nm, uint32_t Nmax, uint32_t nrow_all, uint32_t ndw)
{
  return (size_t)(nrow + (nrow + 1) / 2 + (size_t)nm * nrow + Nmax + 4 * (size_t)Nmax + 32 * (size_t)Nmax / 2 + 2) * sizeof(double2)
         + (size_t)nrow_all * sizeof(double2) + (size_t)(ndw + nm) * nrow * sizeof(double2);
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
  double2* shp; double* shr; double2* shG; double2* sha; double2* shD; double* shP; double2* erow; double2* shred;
  double2 *shED, *shEM; int d0;   // the window's Doppler (from d0) and range (from m0) row phasors ed / em
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
  // G_j(m) = em_j(m) pp_j K_j(m - pr_j), per (m, row); then per cell ONE thread sums ed_j(d) G_j(m) over the
  // rows in the oracle's order (Response::at), and one thread sums the cells (fit_channel's J). The
  // products associate differently from the oracle's ((ed * em) * pp) * K -- a few ulps per term, the
  // same size as the oracle's own FMA-contraction choices -- at a third of the FP64 work.
  for (uint32_t q = threadIdx.x; q < C.nm * rs.nrow; q += blockDim.x) {
    const uint32_t mi = q / rs.nrow, j = q % rs.nrow; const int m = C.m0 + (int)mi;
    if (m < 0 || m >= (int)a.n_range) continue;
    C.shG[q] = cmul(cmul(C.shEM[q], C.shp[j]), d_kernel_at(rs, rs.g[j], (double)m - C.shr[j]));
  }
  __syncthreads();
  for (int c = threadIdx.x; c < C.N; c += blockDim.x) {
    const int2 cm = __ldcg(C.cells + c);
    const double2* ed = C.shED + (size_t)(cm.y - C.d0) * rs.nrow; const double2* G = C.shG + (size_t)(cm.x - C.m0) * rs.nrow;
    double2 acc = make_double2(0, 0);
    for (uint32_t j = 0; j < rs.nrow; ++j) acc = cadd(acc, cmul(ed[j], G[j]));
    C.sha[c] = acc;
  }
  __syncthreads();
  if (threadIdx.x == 0) {
    double2 num = make_double2(0, 0); double den = 0;
    for (int c = 0; c < C.N; ++c) {
      const double2 yc = __ldcg(C.y + c), ac = C.sha[c];
      num.x += yc.x * ac.x + yc.y * ac.y; num.y += yc.y * ac.x - yc.x * ac.y; den += ac.x * ac.x + ac.y * ac.y;
    }
    C.resJ[wb * kMaxPts + k] = den > 0 ? (num.x * num.x + num.y * num.y) / den : 0.0;
    C.resA[wb * kMaxPts + k] = den > 0 ? make_double2(num.x / den, num.y / den) : make_double2(0, 0);
  }
  if (want_a) for (int c = threadIdx.x; c < C.N; c += blockDim.x) C.resa[((size_t)wb * kMaxPts + k) * A.Nmax + c] = C.sha[c];
  __syncthreads();
}
// Barrier among the B blocks of ONE job (a channel's fit): everything a fit exchanges between its blocks
// is job-local, and the channels are independent chains, so no grid-wide barrier is needed per batch.
// The blocks of a cooperative launch are co-resident, so spinning cannot deadlock.
__device__ void fit_sync(FitCtx& C)
{
  __syncthreads();
  if (C.A->B > 1 && threadIdx.x == 0) {
    unsigned* cnt = C.A->bar + 2 * C.job; volatile unsigned* gen = C.A->bar + 2 * C.job + 1;
    const unsigned g = *gen;
    __threadfence();
    if (atomicAdd(cnt, 1u) == C.A->B - 1) { atomicExch(cnt, 0u); __threadfence(); atomicAdd((unsigned*)gen, 1u); }
    else while (*gen == g) { }
    __threadfence();
  }
  __syncthreads();
  ++C.nsync;
}
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
// Phasor recurrence over 16-subcarrier chunks, each restarted from an exact phase.
__device__ double2 d_static_sum(const FitArgs& A, double x, uint32_t d, const double2* M, double2* sh)
{
  // FP64 throughout (a fit's data feeds decisions; FP32 here moved fits by ~1e-8 bin, which the
  // non-converging joint refit amplified to 0.1 bin -- measured on the full-band recording).
  const DAxes& a = A.a; const uint32_t sc = A.sc;
  const float2* Qd = A.Q + (size_t)d * sc;
  double2 acc = make_double2(0, 0);
  const uint32_t kChunk = (sc + blockDim.x - 1) / blockDim.x;   // one chunk per thread
  const double w = 2 * M_PI * a.scs_hz * x * a.delay_step_s;
  const double2 rot = dpolar(w);
  for (uint32_t k0 = threadIdx.x * kChunk; k0 < sc; k0 += blockDim.x * kChunk) {
    double2 ph = dpolar(w * ((double)k0 - sc / 2.0));
    const uint32_t k1 = k0 + kChunk < sc ? k0 + kChunk : sc;
    for (uint32_t k = k0; k < k1; ++k) {
      const float2 q = Qd[k];
      if (q.x != 0.f || q.y != 0.f) { const double2 mk = __ldcg(M + k); acc = cadd(acc, cmul(cmul(ph, mk), make_double2(q.x, q.y))); }
      ph = cmul(ph, rot);
    }
  }
  return block_sum2(acc, sh);
}
// DFit as written by other blocks earlier in the same (persistent) kernel: bypass L1 (not coherent across SMs).
__device__ inline DFit ld_fit(const DFit* p)
{
  static_assert(sizeof(DFit) % 8 == 0, "DFit is loaded as 8-byte words");
  DFit f; const unsigned long long* s = (const unsigned long long*)p; unsigned long long* d = (unsigned long long*)&f;
  for (int i = 0; i < (int)(sizeof(DFit) / 8); ++i) d[i] = __ldcg(s + i);
  return f;
}
__device__ void fit_ctx_init(const FitArgs& A, FitCtx& C, uint32_t job, uint32_t b, double2* smem, double2* shred)
{
  const DResp& rs = A.rs; const uint32_t nm = (uint32_t)(2 * A.hm_r + 1);
  C.A = &A; C.job = job; C.b = b; C.nm = nm; C.nsync = 0; C.rd = 0;
  C.cells = A.cells + (size_t)job * A.Nmax; C.y0 = A.y0 + (size_t)job * A.Nmax; C.y = A.y + (size_t)job * A.Nmax;
  C.resA = A.resA + (size_t)job * 2 * kMaxPts; C.resJ = A.resJ + (size_t)job * 2 * kMaxPts; C.resa = A.resa + (size_t)job * kResaPer * A.Nmax;
  C.shp = smem; C.shr = (double*)(smem + rs.nrow); C.shG = smem + rs.nrow + (rs.nrow + 1) / 2;
  C.sha = C.shG + (size_t)nm * rs.nrow; C.shD = C.sha + A.Nmax; C.shP = (double*)(C.shD + 4 * (size_t)A.Nmax);
  C.erow = (double2*)(C.shP + 32 * (size_t)A.Nmax) + 2;
  C.shED = C.erow + A.nrow_all; C.shEM = C.shED + (size_t)(2 * A.hm_d + 1) * rs.nrow;
  C.shred = shred;
}
// One channel fit (coherent_core.cc's fit_channel) by the B blocks of job C.job, ending with every block
// of the grid at the same barrier count. moved: the refit's convergence flag (OR-ed).
__device__ inline unsigned long long gtimer() { unsigned long long t; asm volatile("mov.u64 %0, %globaltimer;" : "=l"(t)); return t; }
__device__ void fit_job(const FitArgs& A, FitCtx& C, const FitJob J, int* moved)
{
  unsigned long long tph = gtimer();
  auto phase = [&](int k) { if (A.stats && C.b == 0 && threadIdx.x == 0) { const unsigned long long t = gtimer(); atomicAdd(A.stats + 4 + k, t - tph); tph = t; } };
  const uint32_t b = C.b, job = C.job;
  const DAxes& a = A.a; const DResp& rs = A.rs;
  const uint32_t ch = (uint32_t)J.ch;
  const int nsync0 = C.nsync;
  double p0 = J.p0, fb0 = J.fb0;
  if (J.from_slot) { const DFit pf0 = ld_fit(A.F + J.own * kCh + ch); p0 = pf0.p; fb0 = pf0.fb; }
  // cells: m-major, d inner, as the CPU (every thread builds the same list; block 0 publishes it)
  const long mc = lround(p0), dc = lround(fb0);
  C.m0 = (int)(mc - A.hm_r);
  int2* cellw = A.cells + (size_t)job * A.Nmax;
  int N = 0;
  for (long m = mc - A.hm_r; m <= mc + A.hm_r; ++m)
    for (long d = dc - A.hm_d; d <= dc + A.hm_d; ++d)
      if (m >= 0 && m < (long)a.n_range && d_dopp_ok(a, d)) { if (threadIdx.x == 0 && b == 0) cellw[N] = make_int2((int)m, (int)d); ++N; }
  C.N = N; C.d0 = (int)(dc - A.hm_d);
  for (uint32_t q = threadIdx.x; q < (uint32_t)(2 * A.hm_d + 1) * rs.nrow; q += blockDim.x) {
    const long d = C.d0 + (long)(q / rs.nrow); C.shED[q] = (d >= 0 && d < (long)a.n_dopp) ? rs.ed[(size_t)d * rs.nrow + q % rs.nrow] : make_double2(0, 0);
  }
  for (uint32_t q = threadIdx.x; q < C.nm * rs.nrow; q += blockDim.x) {
    const long m = C.m0 + (long)(q / rs.nrow); C.shEM[q] = (m >= 0 && m < (long)a.n_range) ? rs.em[(size_t)m * rs.nrow + q % rs.nrow] : make_double2(0, 0);
  }
  const bool doit = N >= 4;
  fit_sync(C);   // cells published; every block has read the slot's previous p/fb
  phase(8);
  if (doit) {
    // y0 = RD - sum of the other accepted paths on this channel (direct + static term)
    if (A.tables) {   // pursuit: the accepted paths' tables
    for (int c = (int)b; c < N; c += (int)A.B) {
      const int2 cm = __ldcg(C.cells + c);
      const float2 r = A.rdf[((size_t)ch * a.n_range + cm.x) * a.n_dopp + cm.y];
      double2 yc = make_double2(r.x, r.y);
      for (uint32_t o = 0; o < A.nacc; ++o) {
        if ((int)o == J.own) continue;
        const uint32_t s = o * kCh + ch; const DFit f = ld_fit(A.F + s);
        if (!f.ok) continue;
        const double2 at = A.dir[(size_t)s * A.nc + (size_t)cm.x * a.n_dopp + cm.y], st = A.stat[(size_t)s * A.sper + (size_t)cm.y * a.n_range + cm.x];
        const double2 v = cmul(f.A, cadd(at, st));
        yc.x -= v.x; yc.y -= v.y;
      }
      if (threadIdx.x == 0) { C.y0[c] = yc; C.y[c] = yc; }
    }
    } else {          // joint refit: the other paths change between fits, so on the fly
      // The static terms are linear in each path: sum_o A_o sv_o(m - p_o, d) = sum_k e^{j W m k'} Q_k(d) Z_k with
      // Z_k = sum_o A_o e^{-j W p_o k'} M^o_k (W = 2pi scs delay_step, k' = k - sc/2): ONE subcarrier sum per
      // cell instead of one per (cell, other path). Only the summation order differs from the oracle.
      __shared__ double2 shAo[64]; __shared__ double shPo[64]; __shared__ uint32_t shSo[64]; __shared__ uint32_t shNo;
      __shared__ double2 tA[64]; __shared__ double tP[64]; __shared__ int tOk[64];
      // the other paths o0..o0+63 of this channel (ok ones, in order) into shared memory, loaded in parallel
      auto stage = [&](uint32_t o0) {
        if (threadIdx.x < 64) {
          const uint32_t o = o0 + threadIdx.x; int ok = 0;
          if (o < A.nacc && (int)o != J.own) {
            const DFit* F = A.F + o * kCh + ch;
            ok = __ldcg(&F->ok); if (ok) { tA[threadIdx.x] = __ldcg(&F->A); tP[threadIdx.x] = __ldcg(&F->p); }
          }
          tOk[threadIdx.x] = ok;
        }
        __syncthreads();
        if (threadIdx.x == 0) {
          uint32_t no = 0;
          for (uint32_t q = 0; q < 64; ++q) if (tOk[q]) { shAo[no] = tA[q]; shPo[no] = tP[q]; shSo[no] = (o0 + q) * kCh + ch; ++no; }
          shNo = no;
        }
        __syncthreads();
      };
      double2* Z = A.Z + (size_t)job * A.sc;
      const double Om = 2 * M_PI * a.scs_hz * a.delay_step_s;
      for (uint32_t o0 = 0; o0 < A.nacc; o0 += 64) {
        stage(o0);
        const uint32_t kZc = (A.sc + A.B * blockDim.x - 1) / (A.B * blockDim.x) < 8 ? (A.sc + A.B * blockDim.x - 1) / (A.B * blockDim.x) : 8;
        for (uint32_t k0 = (b * blockDim.x + threadIdx.x) * kZc; k0 < A.sc; k0 += A.B * blockDim.x * kZc) {
          const uint32_t k1 = k0 + kZc < A.sc ? k0 + kZc : A.sc;
          double2 z[8];
          for (uint32_t q = 0; q < kZc; ++q) z[q] = o0 ? __ldcg(Z + k0 + (q < k1 - k0 ? q : 0)) : make_double2(0, 0);
          for (uint32_t oi = 0; oi < shNo; ++oi) {
            double2 ph = dpolar(-Om * shPo[oi] * ((double)k0 - A.sc / 2.0)); const double2 rot = dpolar(-Om * shPo[oi]);
            const double2* Mo = A.Mf + (size_t)shSo[oi] * A.sc;
            for (uint32_t k = k0; k < k1; ++k) { z[k - k0] = cadd(z[k - k0], cmul(shAo[oi], cmul(__ldcg(Mo + k), ph))); ph = cmul(ph, rot); }
          }
          for (uint32_t k = k0; k < k1; ++k) Z[k] = z[k - k0];
        }
        __syncthreads();
      }
      phase(6);
      fit_sync(C);   // Z complete (every block of the job wrote its share)
      phase(7);
      // direct responses: one thread per (cell, other path) sums the rows in the oracle's order into V, then
      // one thread per cell subtracts them in the paths' order (chunks of 64 paths)
      double2* V = C.resa;   // scratch: N x 64 values (resa holds kResaPer * Nmax)
      for (uint32_t o0 = 0; o0 < (A.nacc ? A.nacc : 1); o0 += 64) {
        if (o0 > 0 || A.nacc > 64) stage(o0);   // (the Z loop left the only chunk staged when nacc <= 64)
        for (uint32_t q = b * blockDim.x + threadIdx.x; q < (uint32_t)N * shNo; q += A.B * blockDim.x) {
          const uint32_t c = q / shNo, oi = q % shNo;
          const int2 cm = __ldcg(C.cells + c);
          const double2* ed = C.shED + (size_t)(cm.y - C.d0) * rs.nrow; const double2* em = C.shEM + (size_t)(cm.x - C.m0) * rs.nrow;
          const size_t sb = (size_t)shSo[oi] * A.per; const double2* pp = A.gpp + sb; const double* pr = A.gpr + sb;
          double2 at = make_double2(0, 0);
          for (uint32_t j = 0; j < rs.nrow; ++j)
            at = cadd(at, cmul(cmul(cmul(ed[j], em[j]), __ldcg(pp + j)), d_kernel_at(rs, rs.g[j], (double)cm.x - __ldcg(pr + j))));
          V[(size_t)c * 64 + oi] = cmul(shAo[oi], at);
        }
        fit_sync(C);
        for (int c = (int)(b * blockDim.x + threadIdx.x); c < N; c += (int)(A.B * blockDim.x)) {
          double2 yc;
          if (o0 == 0) { const int2 cm = __ldcg(C.cells + c); const float2 r = A.rdf[((size_t)ch * a.n_range + cm.x) * a.n_dopp + cm.y]; yc = make_double2(r.x, r.y); }
          else yc = C.y0[c];
          for (uint32_t oi = 0; oi < shNo; ++oi) { const double2 v = __ldcg(V + (size_t)c * 64 + oi); yc.x -= v.x; yc.y -= v.y; }
          C.y0[c] = yc;
        }
        fit_sync(C);
      }
      // plus the static terms of all other paths at once (block-wide subcarrier sum per cell)
      for (int c = (int)b; c < N; c += (int)A.B) {
        const int2 cm = __ldcg(C.cells + c);
        const double2 sv = d_static_sum(A, (double)cm.x, (uint32_t)cm.y, Z, C.shred);
        if (threadIdx.x == 0) { double2 yc = __ldcg(C.y0 + c); yc.x += sv.x; yc.y += sv.y; C.y0[c] = yc; C.y[c] = yc; }
      }
    }
    fit_sync(C);
  }
  double p = p0, fb = fb0; double2 bestA = make_double2(0, 0);
  DFit out{}; out.ch = (int32_t)ch;
  phase(0);
  if (doit) {
    fit_search(C, p, fb, 0.25, bestA);
    phase(1);
    // M_k(f) = mean over the rows observing k of e^{j2pi f t_r} (FP32: it only shapes y)
    const double fhz = a.dopp0_hz + fb * a.dopp_step_hz;
    for (uint32_t r = threadIdx.x; r < A.nrow_all; r += blockDim.x) { const double2 e = dpolar(2 * M_PI * fhz * A.trow[r]); C.erow[r] = e; }
    __syncthreads();
    double2* Mm = A.Mmid + (size_t)job * A.sc;   // shared by the job's blocks, each computes a share
    for (uint32_t k = b * blockDim.x + threadIdx.x; k < A.sc; k += A.B * blockDim.x) {
      double2 m = make_double2(0, 0); uint32_t cnt = 0;
      for (uint32_t r = 0; r < A.nrow_all; ++r)
        if (k >= A.lo[r] && k <= A.hi[r] && A.mask[(size_t)r * A.sc + k]) { m.x += C.erow[r].x; m.y += C.erow[r].y; ++cnt; }
      Mm[k] = cnt ? make_double2(m.x / (double)cnt, m.y / (double)cnt) : make_double2(0, 0);
    }
    fit_sync(C);
    phase(2);
    for (int pass = 0; pass < 2; ++pass) {
      const double2 Av = bestA;     // = the CPU's J(p, fb, &A): same point, same y
      for (int c = (int)b; c < N; c += (int)A.B) {
        const int2 cm = __ldcg(C.cells + c);
        const double2 sv = d_static_sum(A, (double)cm.x - p, (uint32_t)cm.y, Mm, C.shred);
        if (threadIdx.x == 0) { const double2 y0c = __ldcg(C.y0 + c); C.y[c] = make_double2(y0c.x + (Av.x * sv.x - Av.y * sv.y), y0c.y + (Av.x * sv.y + Av.y * sv.x)); }
      }
      fit_sync(C);
      phase(3);
      fit_search(C, p, fb, 0.0, bestA);
      phase(4);
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
        const DFit prev = ld_fit(A.F + J.own * kCh + ch);
        const double scv = sqrt(fmax(1.0, (fA.x * fA.x + fA.y * fA.y) / nz / fmax(out.snr, 1e-300)));
        if (J.from_slot && (!prev.ok || fabs(out.p - prev.p) > scv * out.sp || fabs(out.fb - prev.fb) > scv * out.sf)) atomicOr(moved, 1);
        A.F[J.own * kCh + ch] = out;
      }
    }
  } else if (b == 0 && threadIdx.x == 0) {
    A.F[J.own * kCh + ch] = out;     // ChanFit{} (not ok): fewer than 4 cells
  }
  phase(5);
  if (A.stats && b == 0 && threadIdx.x == 0) { atomicAdd(A.stats, (unsigned long long)(C.nsync - nsync0 + 1)); atomicAdd(A.stats + 1, 1ull); }
  fit_sync(C);   // the published fit is visible to every block of the job
}
__global__ void __launch_bounds__(256) k_fit(FitArgs A)
{
  extern __shared__ double2 smem[];
  __shared__ double2 shred[32];
  FitCtx C; fit_ctx_init(A, C, blockIdx.x / A.B, blockIdx.x % A.B, smem, shred);
  fit_job(A, C, A.jobs[C.job], A.moved);
}
// The joint refit (coherent_core.cc's refit_all) in ONE cooperative launch: sweeps x detections in the
// oracle's Gauss-Seidel order, the channels (independent chains: a fit only subtracts the other paths of
// its own channel) in parallel. After each fit its blocks refresh the slot's nominal phasors and M_k,
// which the next detections' fits read. moved[sweep]: the sweep's convergence flag (zeroed by the host).
__global__ void __launch_bounds__(256) k_refit(FitArgs A, int* moved, int* sweeps_done)
{
  extern __shared__ double2 smem[];
  __shared__ double2 shred[32];
  FitCtx C; fit_ctx_init(A, C, blockIdx.x / A.B, blockIdx.x % A.B, smem, shred);
  const DAxes& a = A.a; const DResp& rs = A.rs;
  const uint32_t ch = (uint32_t)A.jobs[C.job].ch;
  int sweep = 0;
  for (; sweep < 10; ++sweep) {
    for (uint32_t j = 0; j < A.nacc; ++j) {
      FitJob J = A.jobs[C.job]; J.own = (int32_t)j; J.from_slot = 1;
      fit_job(A, C, J, moved + sweep);
      // slot tables of the new fit: grid[0] phasors (k_fit_phasors) and M_k (k_setM), over the job's blocks
      const uint32_t s = j * kCh + ch; const DFit f = ld_fit(A.F + s);
      if (f.ok) {
        const double fz = a.dopp0_hz + f.gfb[0] * a.dopp_step_hz;
        for (uint32_t q = C.b * blockDim.x + threadIdx.x; q < rs.nrow; q += A.B * blockDim.x) {
          const double pr = f.gp[0] - (fz / a.fc_hz) * (rs.t[q] - rs.tbar) / a.delay_step_s;
          ((double*)A.gpr)[(size_t)s * A.per + q] = pr;
          ((double2*)A.gpp)[(size_t)s * A.per + q] = dpolar(2 * M_PI * (fz * rs.t[q] - rs.fc[q] * pr * a.delay_step_s));
        }
        const double fhz = a.dopp0_hz + f.fb * a.dopp_step_hz;
        for (uint32_t r = threadIdx.x; r < A.nrow_all; r += blockDim.x) C.erow[r] = dpolar(2 * M_PI * fhz * A.trow[r]);
        __syncthreads();
        for (uint32_t k = C.b * blockDim.x + threadIdx.x; k < A.sc; k += A.B * blockDim.x) {
          double2 m = make_double2(0, 0); uint32_t cnt = 0;
          for (uint32_t r = 0; r < A.nrow_all; ++r)
            if (k >= A.lo[r] && k <= A.hi[r] && A.mask[(size_t)r * A.sc + k]) { m.x += C.erow[r].x; m.y += C.erow[r].y; ++cnt; }
          if (cnt) { m.x /= (double)cnt; m.y /= (double)cnt; }
          ((double2*)A.Mf)[(size_t)s * A.sc + k] = m;
        }
      }
      fit_sync(C);
    }
    __threadfence(); cg::this_grid().sync();   // every channel's sweep done: its moved flag is final
    if (!__ldcg(moved + sweep)) { ++sweep; break; }
  }
  if (blockIdx.x == 0 && threadIdx.x == 0) *sweeps_done = sweep;
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
      w_mask, w_lo, w_hi, w_trow, gam, jobs, f_cells, f_y0, f_y, f_resA, f_resa, f_resJ, f_Mmid, f_Z, bar, moved, fstats, m_erow, refit_flags;
  int n_sm = 0; size_t fit_smem_set = 0, choose_smem_set = 0;
  // Warps per block for the warp-per-candidate choose kernels: as many as fit a block's shared memory (<= 4).
  uint32_t choose_warps(uint32_t W)
  {
    const size_t per = choose_smem(W);
    int dev = 0, maxsm = 0; cudaGetDevice(&dev); cudaDeviceGetAttribute(&maxsm, cudaDevAttrMaxSharedMemoryPerBlockOptin, dev);
    if (per > (size_t)maxsm) throw std::runtime_error("detect choose: Doppler search window too wide for shared memory");
    const uint32_t w = (uint32_t)std::max<size_t>(1, std::min<size_t>(4, (size_t)maxsm / per));
    if (w * per > choose_smem_set) {
      cuda_check(cudaFuncSetAttribute((const void*)k_choose_cand, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)(w * per)), "choose smem");
      cuda_check(cudaFuncSetAttribute((const void*)k_rescore, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)(w * per)), "rescore smem");
      choose_smem_set = w * per;
    }
    return w;
  }
  // Cooperative fit launch setup: jobs (one per channel), blocks per job, scratch. Returns the block count.
  size_t fit_setup(FitArgs& fa, const std::vector<FitJob>& jb, uint32_t nrow_all, const void* kern, size_t* smem_out)
  {
    const uint32_t nj = (uint32_t)jb.size();
    up(jobs, jb.data(), nj);
    const size_t smem = fit_smem_bytes(fa.rs.nrow, (uint32_t)(2 * fa.hm_r + 1), fa.Nmax, nrow_all, (uint32_t)(2 * fa.hm_d + 1));
    if (!n_sm) { int dev = 0; cudaGetDevice(&dev); cudaDeviceGetAttribute(&n_sm, cudaDevAttrMultiProcessorCount, dev); }
    if (smem > fit_smem_set) {
      cuda_check(cudaFuncSetAttribute((const void*)k_fit, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem), "fit smem attr");
      cuda_check(cudaFuncSetAttribute((const void*)k_refit, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem), "refit smem attr");
      fit_smem_set = smem;
    }
    int per_sm = 0; cuda_check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&per_sm, kern, 256, smem), "fit occupancy");
    const uint32_t total = (uint32_t)std::max(1, per_sm * n_sm);
    if (total < nj) throw std::runtime_error("detect fit: device cannot co-schedule one block per job");
    fa.B = std::max<uint32_t>(1, std::min<uint32_t>(kMaxPts, total / nj));   // a full 9-point batch in one wave
    fa.jobs = jobs.as<FitJob>(); fa.njobs = nj;
    const size_t nb = (size_t)nj * fa.B;
    f_cells.ensure((size_t)nj * fa.Nmax * sizeof(int2)); f_y0.ensure((size_t)nj * fa.Nmax * sizeof(double2)); f_y.ensure((size_t)nj * fa.Nmax * sizeof(double2));
    f_resA.ensure((size_t)nj * 2 * kMaxPts * sizeof(double2)); f_resJ.ensure((size_t)nj * 2 * kMaxPts * sizeof(double));
    f_resa.ensure((size_t)nj * kResaPer * fa.Nmax * sizeof(double2)); f_Mmid.ensure(nb * fa.sc * sizeof(double2));
    f_Z.ensure((size_t)nj * fa.sc * sizeof(double2)); fa.Z = f_Z.as<double2>();
    bar.ensure(2 * nj * sizeof(unsigned));
    fa.cells = f_cells.as<int2>(); fa.y0 = f_y0.as<double2>(); fa.y = f_y.as<double2>(); fa.resA = f_resA.as<double2>(); fa.resJ = f_resJ.as<double>();
    fa.resa = f_resa.as<double2>(); fa.Mmid = f_Mmid.as<double2>(); fa.bar = bar.as<unsigned>(); fa.moved = moved.as<int>();
    cuda_check(cudaMemsetAsync(bar.p, 0, 2 * nj * sizeof(unsigned), stream), "memset fit barrier");
    *smem_out = smem;
    return nb;
  }
  // Pursuit accept: one fit per channel, then each new fit's grid phasors and final M.
  void launch_fits(FitArgs fa, const std::vector<FitJob>& jb, uint32_t nrow_all, const DAxes& da, const DResp& dr, size_t per)
  {
    if (jb.empty()) return;
    const uint32_t nj = (uint32_t)jb.size();
    size_t smem = 0; const size_t nb = fit_setup(fa, jb, nrow_all, (const void*)k_fit, &smem);
    void* args[] = {&fa};
    cuda_check(cudaLaunchCooperativeKernel((void*)k_fit, dim3((unsigned)nb), dim3(256), args, smem, stream), "fit launch");
    fit_post(jb, nrow_all, da, dr, per, fa.sc);
  }
  // Each new fit's grid phasors and final M (from its DFit in F).
  void fit_post(const std::vector<FitJob>& jb, uint32_t nrow_all, const DAxes& da, const DResp& dr, size_t per, uint32_t sc)
  {
    const uint32_t nj = (uint32_t)jb.size();
    std::vector<uint32_t> sl(nj); for (uint32_t q = 0; q < nj; ++q) sl[q] = (uint32_t)jb[q].own * kCh + (uint32_t)jb[q].ch;
    up(slots, sl.data(), nj);
    k_fit_phasors<<<blocks(nj * per), 256, 0, stream>>>(da, dr, F.as<DFit>(), slots.as<uint32_t>(), nj, gpp.as<double2>(), gpr.as<double>());
    m_erow.ensure((size_t)nj * nrow_all * sizeof(double2));
    k_setM_phase<<<blocks((size_t)nj * nrow_all), 256, 0, stream>>>(da, F.as<DFit>(), slots.as<uint32_t>(), nj, nrow_all, w_trow.as<double>(), m_erow.as<double2>());
    k_setM<<<blocks((size_t)nj * sc), 256, 0, stream>>>(F.as<DFit>(), slots.as<uint32_t>(), nj, sc, nrow_all, w_mask.as<uint8_t>(), w_lo.as<uint32_t>(),
                                                          w_hi.as<uint32_t>(), m_erow.as<double2>(), Mf.as<double2>());
  }
  // Joint refit: all sweeps in one cooperative launch; returns the number of sweeps run.
  int launch_refit(FitArgs fa, const std::vector<FitJob>& jb, uint32_t nrow_all)
  {
    if (jb.empty()) return 0;
    size_t smem = 0; const size_t nb = fit_setup(fa, jb, nrow_all, (const void*)k_refit, &smem);
    refit_flags.ensure(11 * sizeof(int));
    cuda_check(cudaMemsetAsync(refit_flags.p, 0, 11 * sizeof(int), stream), "memset refit flags");
    int* mv = refit_flags.as<int>(); int* nsw = mv + 10;
    void* args[] = {&fa, &mv, &nsw};
    cuda_check(cudaLaunchCooperativeKernel((void*)k_refit, dim3((unsigned)nb), dim3(256), args, smem, stream), "refit launch");
    int sweeps = 0; cuda_check(cudaMemcpyAsync(&sweeps, nsw, sizeof(int), cudaMemcpyDeviceToHost, stream), "D2H sweeps");
    sync();
    return sweeps;
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
  const bool prof = std::getenv("NR_ISAC_DETECT_PROFILE") != nullptr;
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
    const uint32_t wpb = I.choose_warps(W);
    k_choose_cand<<<(unsigned)((nc + wpb - 1) / wpb), 32 * wpb, wpb * choose_smem(W), I.stream>>>(da, dg, dG, I.cand.as<uint32_t>(), nc, nv, I.tested.as<uint32_t>(), I.okc.as<uint32_t>(),
                                                          I.dh.as<uint32_t>(), I.nthr.as<double>(), I.scale.as<double>(), I.mag.as<float>(),
                                                          W, I.c_me.as<DAcc>(), I.c_ec.as<double>(), I.c_thr.as<double>());
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
    fa.stats = nullptr;
    if (prof) { I.fstats.ensure(32 * sizeof(unsigned long long)); cuda_check(cudaMemsetAsync(I.fstats.p, 0, 32 * sizeof(unsigned long long), I.stream), "memset"); fa.stats = I.fstats.as<unsigned long long>(); }
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
    if (prof) I.sync();
    T.fit_ms += ms_since(tq); tq = Clock::now();
    k_rebuild<<<I.blocks(jb.size() * ncc), 256, 0, I.stream>>>(da, I.F.as<DFit>(), I.slots.as<uint32_t>(), (uint32_t)jb.size(), I.dir.as<double2>(), I.rcf.as<float2>(), I.magc.as<float>());
    if (prof) I.sync();
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
        const uint32_t wpb = I.choose_warps(W);
        k_rescore<<<(unsigned)((ni + wpb - 1) / wpb), 32 * wpb, wpb * choose_smem(W), I.stream>>>(da, dg, ds, nms_r, d_items, ni, I.mag.as<float>(), I.magc.as<float>(), W);
        if (prof) { I.sync(); T.choose2_ms += ms_since(tq); }
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
    std::vector<FitJob> jb;
    for (uint32_t i = 0; i < kCh; ++i) if (R.los_found[i]) jb.push_back(FitJob{(int32_t)i, 0, 1, 0, 0.0, 0.0});
    FitArgs f = fa; fit_ptrs(f); f.tables = 0; f.nacc = (uint32_t)acc.size(); if (f.stats) f.stats += 16;
    T.sweeps = (uint32_t)I.launch_refit(f, jb, nrow_all);
    T.fits += T.sweeps * (uint32_t)(acc.size() * jb.size());
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
  unsigned long long fst[32] = {};
  if (prof) { cuda_check(cudaMemcpyAsync(fst, I.fstats.p, sizeof(fst), cudaMemcpyDeviceToHost, I.stream), "D2H"); I.sync();
    for (int w = 0; w < 2; ++w) std::fprintf(stderr, "FITPHASE %s fits=%llu barriers=%llu ms: y0=%.1f [cells=%.1f Z=%.1f zsync=%.1f] coarse=%.1f setM=%.1f passes=%.1f fine=%.1f cov=%.1f\n", w ? "refit" : "accept",
      fst[16 * w + 1], fst[16 * w], fst[16 * w + 4] / 1e6, fst[16 * w + 12] / 1e6, fst[16 * w + 10] / 1e6, fst[16 * w + 11] / 1e6, fst[16 * w + 5] / 1e6, fst[16 * w + 6] / 1e6, fst[16 * w + 7] / 1e6, fst[16 * w + 8] / 1e6, fst[16 * w + 9] / 1e6); }
  T.total_ms = ms_since(t_all);
  if (std::getenv("NR_ISAC_DETECT_PROFILE"))
    std::fprintf(stderr, "DETECTPROF total=%.1f prep=%.1f scale=%.1f cand=%.1f choose=%.1f items=%.1f pursuit=%.1f [pick=%.1f walk=%.1f fit=%.1f rebuild=%.1f rescore=%.1f (choose2=%.1f)] refit=%.1f finish=%.1f cands=%u items=%u rounds=%u acc=%u fits=%u sweeps=%u bpf_acc=%.1f bpf_ref=%.1f hm=%ld,%ld rows=%zu nr=%u nd=%u nt=%zu nv=%zu\n",
                 T.total_ms, T.prep_ms, T.scale_ms, T.cand_ms, T.choose_ms, T.items_ms, T.pursuit_ms, T.pick_ms, T.walk_ms, T.fit_ms, T.rebuild_ms,
                 T.rescore_ms, T.choose2_ms, T.refit_ms, T.finish_ms, T.cands, T.items, T.rounds, T.accepted, T.fits, T.sweeps,
                 fst[1] ? (double)fst[0] / fst[1] : 0.0, fst[17] ? (double)fst[16] / fst[17] : 0.0, hm_r, hm_d, rs.rows.size(), a.n_range, a.n_dopp, nt, nv);
  return out;
}

} // namespace nr_isac::coherent
