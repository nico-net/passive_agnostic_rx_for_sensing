/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* GPU front of the coherent per-CPI chain -- see coherent_cuda_front.h. Every array is a transcription of
 * the CPU loop it replaces in coherent_core.cc (find_los, estimate_row_sync, build_waveform); the
 * decisions stay in coherent_core.cc. FP64 wherever a result feeds a delay/phase estimate; FP32 only for
 * the waveform tables (as the Task 10 k_wf) and the static-removal operator Q (the CPU accumulates Q in
 * FP32 too). Parity: tests/coherent_cuda_parity_test.cc. */
#include "coherent_cuda_front.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace nr_isac::coherent {
namespace {

using zC = cufftDoubleComplex;
constexpr double kPi = 3.14159265358979323846;

void ck(cudaError_t s, const char* op) { if (s != cudaSuccess) throw std::runtime_error(std::string(op) + ": " + cudaGetErrorString(s)); }
void ckf(cufftResult s, const char* op) { if (s != CUFFT_SUCCESS) throw std::runtime_error(std::string(op) + " failed (cufft " + std::to_string((int)s) + ")"); }
unsigned nblk(size_t n) { return (unsigned)std::max<size_t>(1, std::min<size_t>(65535, (n + 255) / 256)); }
#define GRID_STRIDE(j, n) for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < (n); j += (size_t)blockDim.x * gridDim.x)

__device__ inline double bb_hz(uint32_t k, uint32_t sc, double scs) { return ((double)k - sc * 0.5) * scs; }
__device__ inline size_t wrapc(uint32_t k, uint32_t sc, uint32_t n)   // centred index (k - sc/2) mod n
{ const long q = (long)k - (long)(sc / 2); return (size_t)(((q % (long)n) + (long)n) % (long)n); }

__global__ void k_widen(const float2* in, size_t n, zC* out) { GRID_STRIDE(j, n) { out[j].x = in[j].x; out[j].y = in[j].y; } }

// Per row: Hann weight table hw[r][k] (0 where not observed) and its sums -- coherent_core.cc's hann() over
// the row's observed span (weight 1 for a single-subcarrier row).
__global__ void k_hw(const uint8_t* obs, const int2* span, uint32_t sc, double* hw, double* h1, double* h2, float* wsf)
{
  __shared__ double s1[256], s2[256];
  const uint32_t r = blockIdx.x; const int2 s = span[r];
  double a1 = 0, a2 = 0;
  for (uint32_t k = threadIdx.x; k < sc; k += blockDim.x) {
    double h = 0;
    if (obs[(size_t)r * sc + k]) { h = s.y > s.x ? 0.5 - 0.5 * cospi(2.0 * ((double)((long)k - s.x) / (double)(s.y - s.x))) : 1.0; a1 += h; a2 += h * h; }
    hw[(size_t)r * sc + k] = h;
  }
  s1[threadIdx.x] = a1; s2[threadIdx.x] = a2; __syncthreads();
  for (unsigned h = blockDim.x / 2; h > 0; h >>= 1) { if (threadIdx.x < h) { s1[threadIdx.x] += s1[threadIdx.x + h]; s2[threadIdx.x] += s2[threadIdx.x + h]; } __syncthreads(); }
  if (threadIdx.x == 0) { h1[r] = s1[0]; h2[r] = s2[0]; wsf[r] = (float)s1[0]; }
}

// Row spectra for a batched inverse FFT: batch b = (channel, row) (row_of_b: rows given explicitly, channel
// unused -- the unit kernels), Hann-weighted data (or the weight alone, `unit`) at the centred index mod nout.
__global__ void k_scatter(const zC* values, const double* hw, const uint32_t* row_of_b, uint32_t nb, uint32_t rows, uint32_t sc,
                          uint32_t nout, int unit, zC* out)
{
  GRID_STRIDE(j, (size_t)nb * sc) {
    const uint32_t b = (uint32_t)(j / sc), k = (uint32_t)(j % sc);
    const uint32_t r = row_of_b ? row_of_b[b] : b % rows, i = row_of_b ? 0 : b / rows;
    const double h = hw[(size_t)r * sc + k];
    if (h == 0) continue;
    zC z;
    if (unit) { z.x = h; z.y = 0; } else { const zC v = values[((size_t)i * rows + r) * sc + k]; z.x = v.x * h; z.y = v.y * h; }
    out[(size_t)b * nout + wrapc(k, sc, nout)] = z;
  }
}

// out[c][j] = sum_{b < nb} wgt[c*nb+b] |X[c*nb+b][j]|^2.
__global__ void k_powsum(const zC* X, const double* wgt, uint32_t nc, uint32_t nb, uint32_t n, double* out)
{
  GRID_STRIDE(idx, (size_t)nc * n) {
    const uint32_t c = (uint32_t)(idx / n), j = (uint32_t)(idx % n);
    double acc = 0;
    for (uint32_t b = 0; b < nb; ++b) {
      const double g = wgt[(size_t)c * nb + b]; if (g == 0) continue;
      const zC v = X[((size_t)c * nb + b) * n + j]; acc += g * (v.x * v.x + v.y * v.y);
    }
    out[idx] = acc;
  }
}

// Coherent row-mean spectrum per (channel, subcarrier): sum over rows observing k of H h_r(k)/(ws_r R)
// e^{j(2pi f_k tau_r - phi_r)} (find_los's U), or of h_r(k)/(ws_r R) alone (`unit`: its Uk). Written both
// in subcarrier order (U) and at the centred index mod nout (FFT input).
__global__ void k_colsum(const zC* values, const uint8_t* obs, const double* hw, const double* h1, const double* tau, const double* phase,
                         uint32_t rows, uint32_t sc, double scs, double R, uint32_t nch, int unit, uint32_t nout, zC* U, zC* out)
{
  GRID_STRIDE(idx, (size_t)nch * sc) {
    const uint32_t i = (uint32_t)(idx / sc), k = (uint32_t)(idx % sc);
    const double f = bb_hz(k, sc, scs);
    double re = 0, im = 0;
    for (uint32_t r = 0; r < rows; ++r) {
      if (!obs[(size_t)r * sc + k]) continue;
      const double g = hw[(size_t)r * sc + k] / (h1[r] * R);
      if (unit) { re += g; continue; }
      double si, co; sincos(2 * kPi * f * tau[r] - phase[r], &si, &co);
      const zC v = values[((size_t)i * rows + r) * sc + k];
      const double pr = g * co, pi = g * si;
      re += v.x * pr - v.y * pi; im += v.x * pi + v.y * pr;
    }
    U[idx].x = re; U[idx].y = im;
    zC* o = &out[(size_t)i * nout + wrapc(k, sc, nout)]; o->x = re; o->y = im;
  }
}

__global__ void k_abs(const zC* X, size_t n, double* out) { GRID_STRIDE(j, n) out[j] = sqrt(X[j].x * X[j].x + X[j].y * X[j].y); }

// One block per (channel, row): sum over observed k of H e^{j2pi f_k tau}; each thread a contiguous chunk
// with a phasor recurrence (2 sincos per chunk). Optional slope: sum of H(k) conj(H(k-c)) over pairs one
// comb apart (the row's finest spacing, so k-c is the previous observed subcarrier), times e^{j2pi c scs tau}.
__global__ void k_row_sums(const zC* values, const uint8_t* obs, const int2* span, const uint32_t* comb, const double* tau,
                           uint32_t rows, uint32_t sc, double scs, uint32_t found, int want_slope, zC* acc_out, zC* slope_out)
{
  __shared__ double sh[4][256];
  const uint32_t b = blockIdx.x, i = b / rows, r = b % rows;
  const int2 s = span[r];
  double ar = 0, ai = 0, sr = 0, si_ = 0;
  const double ta = tau[b];
  const uint32_t c = comb[r];
  if (((found >> i) & 1u) && s.y >= s.x) {
    const uint32_t width = (uint32_t)(s.y - s.x + 1), chunk = (width + blockDim.x - 1) / blockDim.x;
    const uint32_t k0 = (uint32_t)s.x + threadIdx.x * chunk, k1 = min((uint32_t)s.y + 1, k0 + chunk);
    if (k0 < k1) {
      double pr, pi, qr, qi;
      sincos(2 * kPi * bb_hz(k0, sc, scs) * ta, &pi, &pr);
      sincos(2 * kPi * scs * ta, &qi, &qr);
      const zC* v = values + ((size_t)i * rows + r) * sc;
      const uint8_t* o = obs + (size_t)r * sc;
      for (uint32_t k = k0; k < k1; ++k) {
        if (o[k]) {
          ar += v[k].x * pr - v[k].y * pi; ai += v[k].x * pi + v[k].y * pr;
          if (want_slope && c > 0 && k >= (uint32_t)s.x + c && o[k - c]) {
            const zC u = v[k - c];
            sr += v[k].x * u.x + v[k].y * u.y; si_ += v[k].y * u.x - v[k].x * u.y;
          }
        }
        const double t = pr * qr - pi * qi; pi = pr * qi + pi * qr; pr = t;
      }
    }
  }
  sh[0][threadIdx.x] = ar; sh[1][threadIdx.x] = ai; sh[2][threadIdx.x] = sr; sh[3][threadIdx.x] = si_; __syncthreads();
  for (unsigned h = blockDim.x / 2; h > 0; h >>= 1) {
    if (threadIdx.x < h) for (int q = 0; q < 4; ++q) sh[q][threadIdx.x] += sh[q][threadIdx.x + h];
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    acc_out[b].x = sh[0][0]; acc_out[b].y = sh[1][0];
    if (want_slope) {
      double er, ei; sincos(2 * kPi * (double)c * scs * ta, &ei, &er);
      slope_out[b].x = sh[2][0] * er - sh[3][0] * ei; slope_out[b].y = sh[2][0] * ei + sh[3][0] * er;
    }
  }
}

// ed[d][r] = (win_r / wsum) e^{-j2pi f_d t_r}.
__global__ void k_ed(const double* wr, const double* t, uint32_t rows, uint32_t ndopp, double f0, double df, zC* ed)
{
  GRID_STRIDE(j, (size_t)ndopp * rows) {
    const uint32_t d = (uint32_t)(j / rows), r = (uint32_t)(j % rows);
    double s, c; sincos(-2 * kPi * (f0 + d * df) * t[r], &s, &c);
    ed[j].x = wr[r] * c; ed[j].y = wr[r] * s;
  }
}

// Waveform kernel tables per mask-shape group (representative row rep[g]): B[u] = sum_k h_k e^{j th_u (k - kc)},
// B2 with h_k^2, th_u = 2pi(-X + u/O)/n_fft -- the Task 10 FP32 k_wf, reading the weights from hw.
__global__ void k_wfB(const double* hw, const uint8_t* obs, const int2* span, const uint32_t* rep, uint32_t sc, uint32_t ng, uint32_t nx,
                      float X, float O, float nfft, float2* B, float2* B2)
{
  GRID_STRIDE(j, (size_t)ng * nx) {
    const uint32_t g = (uint32_t)(j / nx), u = (uint32_t)(j % nx), r = rep[g];
    const int2 s = span[r]; const double kc = 0.5 * (s.x + s.y);
    const float th = 2.f * (float)kPi * (-X + (float)u / O) / nfft;
    float br = 0, bi = 0, cr = 0, ci = 0;
    for (int k = s.x; k <= s.y; ++k) {
      if (!obs[(size_t)r * sc + k]) continue;
      float sn, cs; sincosf(th * (float)((double)k - kc), &sn, &cs);
      const float h = (float)hw[(size_t)r * sc + k];
      br += h * cs; bi += h * sn; cr += h * h * cs; ci += h * h * sn;
    }
    B[j] = make_float2(br, bi); B2[j] = make_float2(cr, ci);
  }
}

// e_over_h[d][r] = ed[d][r] / H_r (FP32), 0 for rows without weight.
__global__ void k_eoh(const zC* ed, const double* h1, uint32_t rows, uint32_t ndopp, float2* out)
{
  GRID_STRIDE(j, (size_t)ndopp * rows) {
    const uint32_t r = (uint32_t)(j % rows);
    out[j] = h1[r] > 0 ? make_float2((float)(ed[j].x / h1[r]), (float)(ed[j].y / h1[r])) : make_float2(0.f, 0.f);
  }
}

// Static-removal operator Q[d][k] = sum_r (ed[d][r]/H_r) h_r(k), FP32 accumulation in row order (as the CPU).
__global__ void k_Q(const float2* eoh, const double* hw, uint32_t rows, uint32_t sc, uint32_t ndopp, float2* Q)
{
  GRID_STRIDE(j, (size_t)ndopp * sc) {
    const uint32_t d = (uint32_t)(j / sc), k = (uint32_t)(j % sc);
    float re = 0, im = 0;
    for (uint32_t r = 0; r < rows; ++r) {
      const float h = (float)hw[(size_t)r * sc + k]; if (h == 0.f) continue;
      const float2 e = eoh[(size_t)d * rows + r]; re += e.x * h; im += e.y * h;
    }
    Q[j] = make_float2(re, im);
  }
}

// ---- find_los's final sub-bin refinement (los_refine): golden section on |coh(x)|, coh(x) = sum_k U[k]
// e^{j2pi f_k x delay_step} = sum_k U[k] e^{j2pi (2k - sc) x / (2 n_fft)}. One WARP per channel: every lane
// runs the same scalar search (FP64) on warp-broadcast sums, so no block barriers are involved. FP32 terms:
// the phase's integer part is reduced exactly in integers, phasor recurrences re-anchor every 8 steps;
// lane partials summed in FP64. |coh| to ~1e-6: argmax within ~1e-3 bin, tap ~1e-4 (bounds 0.01 bin, 1e-3).
__device__ inline void phasor2(long long q2, long long xi, float xf, long long n2, float* c, float* s)   // e^{j2pi q2 (xi+xf)/n2}
{
  const long long m = (q2 * xi) % n2;
  sincospif(2.f * ((float)m / (float)n2 + (float)q2 * xf / (float)n2), s, c);
}
__device__ inline double wsum(double v) { for (int o = 16; o > 0; o >>= 1) v += __shfl_down_sync(0xffffffffu, v, o); return __shfl_sync(0xffffffffu, v, 0); }
// {Re, Im} of sum_k S[k] e^{j2pi (2k - sc) x / n2}, identical in every lane. S in shared memory; lane l
// takes k = l + 32 j in four independent phasor chains (j mod 4, each stepped by 128 subcarriers and
// re-anchored exactly every 8 steps) so the dependent FP32 latency overlaps.
__device__ void dtft(const float2* S, uint32_t sc, long long n2, double x, double* re, double* im)
{
  const uint32_t lane = threadIdx.x & 31;
  const long long xi = llrint(x); const float xf = (float)(x - (double)xi);
  float qr, qi; phasor2(256, xi, xf, n2, &qr, &qi);
  float ar[4] = {0, 0, 0, 0}, ai[4] = {0, 0, 0, 0}, pr[4], pi[4];
  const uint32_t nj = (sc + 31 - lane) / 32;                // this lane's terms
  for (uint32_t j0 = 0; j0 < nj; j0 += 4) {
    if ((j0 & 31u) == 0)
#pragma unroll
      for (int c = 0; c < 4; ++c) phasor2(2LL * (lane + 32 * (j0 + c)) - sc, xi, xf, n2, &pr[c], &pi[c]);
#pragma unroll
    for (int c = 0; c < 4; ++c) {
      if (j0 + c < nj) { const float2 u = S[lane + 32 * (j0 + c)]; ar[c] += u.x * pr[c] - u.y * pi[c]; ai[c] += u.x * pi[c] + u.y * pr[c]; }
      const float t = pr[c] * qr - pi[c] * qi; pi[c] = pr[c] * qi + pi[c] * qr; pr[c] = t;
    }
  }
  *re = wsum((double)ar[0] + ar[1] + ar[2] + ar[3]); *im = wsum((double)ai[0] + ai[1] + ai[2] + ai[3]);
}
template <class F> __device__ double golden_argmax(F f, double x0)   // coherent_core.cc find_los's argmax
{
  const double gr = 0.5 * (sqrt(5.0) - 1);
  double xa = x0 - 1.0, xb = x0 + 1.0, xc = xb - gr * (xb - xa), xd = xa + gr * (xb - xa);
  double fc_ = f(xc), fd_ = f(xd);
  while (xb - xa > 1e-6) {
    if (fc_ > fd_) { xb = xd; xd = xc; fd_ = fc_; xc = xb - gr * (xb - xa); fc_ = f(xc); }
    else { xa = xc; xc = xd; fc_ = fd_; xd = xa + gr * (xb - xa); fd_ = f(xd); }
  }
  return 0.5 * (xa + xb);
}
// in[i] = {best, peak} (bins); out[i] = {x, Re tap, Im tap}. Same fixed-point two-path fit as the CPU.
__global__ void k_los_refine(const float2* U4, const float2* Ukg, uint32_t sc, long long n2, const long2* in, uint32_t found, double* out)
{
  extern __shared__ float2 smem[];                           // U of this channel, then Uk
  const uint32_t i = blockIdx.x;
  if (!((found >> i) & 1u)) return;
  float2 *U = smem, *Uk = smem + sc;
  for (uint32_t k = threadIdx.x; k < sc; k += 32) { U[k] = U4[(size_t)i * sc + k]; Uk[k] = Ukg[k]; }
  __syncwarp();
  const long best = in[i].x, peak = in[i].y;
  // coh(x) - c kc(x - xr): the other path's union-kernel response removed (c = 0: plain coh)
  auto val = [&](double x, double cr, double ci, double xr, double* re, double* im) {
    dtft(U, sc, n2, x, re, im);
    if (cr != 0 || ci != 0) { double kr, ki; dtft(Uk, sc, n2, x - xr, &kr, &ki); *re -= cr * kr - ci * ki; *im -= cr * ki + ci * kr; }
  };
  auto mag = [&](double cr, double ci, double xr) { return [=](double x) { double re, im; val(x, cr, ci, xr, &re, &im); return hypot(re, im); }; };
  double xm = golden_argmax(mag(0, 0, 0), (double)best), tr, ti;
  val(xm, 0, 0, 0, &tr, &ti);
  if (best != peak) {
    double xs = golden_argmax(mag(0, 0, 0), (double)peak), ar, ai;
    val(xs, 0, 0, 0, &ar, &ai);
    for (int it = 0; it < 8; ++it) {
      const double xm0 = xm, xs0 = xs;
      xm = golden_argmax(mag(ar, ai, xs), xm); val(xm, ar, ai, xs, &tr, &ti);
      xs = golden_argmax(mag(tr, ti, xm), xs); val(xs, tr, ti, xm, &ar, &ai);
      if (fabs(xm - xm0) < 1e-4 && fabs(xs - xs0) < 1e-4) break;
    }
  }
  if (threadIdx.x == 0) { out[3 * i] = xm; out[3 * i + 1] = tr; out[3 * i + 2] = ti; }
}
__global__ void k_narrow(const zC* in, size_t n, float2* out) { GRID_STRIDE(j, n) out[j] = make_float2((float)in[j].x, (float)in[j].y); }

double host_hann(double u) { return 0.5 - 0.5 * std::cos(2 * M_PI * u); }
struct Lap {                                        // adds the scope's wall time to one Timing field
  double& acc; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~Lap() { acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
};
} // namespace

CudaFront::Buf::~Buf() { if (p) cudaFree(p); }
void CudaFront::Buf::ensure(size_t bytes)
{
  if (bytes <= cap) return;
  if (p) cudaFree(p);
  p = nullptr; cap = 0;
  ck(cudaMalloc(&p, bytes), "cudaMalloc");
  cap = bytes;
}

CudaFront::CudaFront(cudaStream_t stream) : st_(stream) {}
CudaFront::~CudaFront() { for (Plan* p : {&p_rows_, &p_kern_, &p_fine_, &p_coh_}) if (p->h) cufftDestroy(p->h); }

cufftHandle CudaFront::plan(Plan& p, long n, long batch)
{
  if (p.h && p.n == n && p.batch >= batch) return p.h;
  const long b = (p.h && p.n == n) ? std::max(p.batch, batch) : batch;
  if (p.h) { ckf(cufftDestroy(p.h), "cufftDestroy"); p.h = 0; }
  ckf(cufftPlan1d(&p.h, (int)n, CUFFT_Z2Z, (int)b), "cufftPlan1d");
  ckf(cufftSetStream(p.h, st_), "cufftSetStream");
  p.n = n; p.batch = b;
  return p.h;
}
void CudaFront::sync() { ck(cudaStreamSynchronize(st_), "cudaStreamSynchronize"); }

const cufftDoubleComplex* CudaFront::d_values() const { return values_.as<zC>(); }
const uint8_t* CudaFront::d_observed() const { return observed_.as<uint8_t>(); }
const int2* CudaFront::d_span() const { return span_.as<int2>(); }
const float* CudaFront::d_wsum_f() const { return wsum_f_.as<float>(); }

bool CudaFront::bound(const CfrWindow& w) const
{
  if (bound_ptr_ != (const void*)w.values.data() || bound_rows_ != w.rows || bound_sc_ != w.subcarriers || w.values.empty()) return false;
  const size_t n = w.values.size();
  return w.values[0] == bound_fp_[0] && w.values[n / 2] == bound_fp_[1] && w.values[n - 1] == bound_fp_[2];
}

void CudaFront::upload(const CfrWindow& w)
{
  tm_ = Timing{};
  Lap lap_{tm_.upload};
  const uint32_t rows = w.rows, sc = w.subcarriers, nch = w.antennas;
  if (!w.valid() || nch != kCh) throw std::invalid_argument("CudaFront::upload: invalid window");
  scs_hz_ = w.scs_hz;
  const size_t n = (size_t)nch * rows * sc;
  values_f_.ensure(n * sizeof(float2)); values_.ensure(n * sizeof(zC));
  ck(cudaMemcpyAsync(values_f_.p, w.values.data(), n * sizeof(float2), cudaMemcpyHostToDevice, st_), "H2D values");
  k_widen<<<nblk(n), 256, 0, st_>>>(values_f_.as<float2>(), n, values_.as<zC>());
  observed_.ensure((size_t)rows * sc);
  ck(cudaMemcpyAsync(observed_.p, w.observed.data(), (size_t)rows * sc, cudaMemcpyHostToDevice, st_), "H2D observed");

  // Host metadata, one pass per row (coherent_core.cc's row_span / row_comb / shape groups).
  Meta& m = m_;
  m.rows = rows; m.sc = sc;
  m.lo.assign(rows, 1); m.hi.assign(rows, 0); m.comb.assign(rows, 0); m.grp.assign(rows, -1); m.first.clear();
  m.fc.assign(rows, 0.0); m.nk.assign(rows, 0.0);
  std::unordered_multimap<size_t, uint32_t> shapes;   // hash of the span's mask bytes -> group
  std::vector<int2> span(rows);
  for (uint32_t r = 0; r < rows; ++r) {
    const uint8_t* o = w.observed.data() + w.cell(r, 0);
    int l = -1, h = -1; uint32_t c = 0, prev = UINT32_MAX;
    double fs = 0, nk = 0;
    for (uint32_t k = 0; k < sc; ++k) if (o[k]) {
      fs += ((double)k - sc / 2.0) * w.scs_hz; nk += 1;   // estimate_row_sync's fc / nk, same order
      if (l < 0) l = (int)k;
      h = (int)k;
      if (prev != UINT32_MAX && (c == 0 || k - prev < c)) c = k - prev;
      prev = k;
    }
    m.comb[r] = c; m.nk[r] = nk; m.fc[r] = nk > 0 ? fs / nk : fs;
    span[r] = int2{1, 0};
    if (l < 0) continue;
    m.lo[r] = l; m.hi[r] = h; span[r] = int2{l, h};
    const std::string_view key((const char*)o + l, (size_t)(h - l + 1));
    const size_t hs = std::hash<std::string_view>{}(key);
    auto rg = shapes.equal_range(hs);
    for (auto it = rg.first; it != rg.second && m.grp[r] < 0; ++it) {
      const uint32_t f = m.first[it->second];
      if ((uint32_t)(m.hi[f] - m.lo[f] + 1) == key.size() && std::memcmp(w.observed.data() + w.cell(f, m.lo[f]), key.data(), key.size()) == 0)
        m.grp[r] = (int32_t)it->second;
    }
    if (m.grp[r] < 0) { m.grp[r] = (int32_t)m.first.size(); shapes.emplace(hs, (uint32_t)m.first.size()); m.first.push_back(r); }
  }
  span_.ensure(rows * sizeof(int2)); comb_.ensure(rows * sizeof(uint32_t));
  ck(cudaMemcpyAsync(span_.p, span.data(), rows * sizeof(int2), cudaMemcpyHostToDevice, st_), "H2D span");
  ck(cudaMemcpyAsync(comb_.p, m.comb.data(), rows * sizeof(uint32_t), cudaMemcpyHostToDevice, st_), "H2D comb");
  hw_.ensure((size_t)rows * sc * sizeof(double)); h1_.ensure(rows * sizeof(double)); h2_.ensure(rows * sizeof(double)); wsum_f_.ensure(rows * sizeof(float));
  k_hw<<<rows, 256, 0, st_>>>(observed_.as<uint8_t>(), span_.as<int2>(), sc, hw_.as<double>(), h1_.as<double>(), h2_.as<double>(), wsum_f_.as<float>());
  m.h1.resize(rows); m.h2.resize(rows);
  ck(cudaMemcpyAsync(m.h1.data(), h1_.p, rows * sizeof(double), cudaMemcpyDeviceToHost, st_), "D2H h1");
  ck(cudaMemcpyAsync(m.h2.data(), h2_.p, rows * sizeof(double), cudaMemcpyDeviceToHost, st_), "D2H h2");
  sync();
  bound_ptr_ = w.values.data(); bound_rows_ = rows; bound_sc_ = sc;
  bound_fp_[0] = w.values[0]; bound_fp_[1] = w.values[n / 2]; bound_fp_[2] = w.values[n - 1];
}

void CudaFront::los_kernel(const Axes& a, uint32_t ovs, const std::vector<uint32_t>& first, const std::vector<uint32_t>& count,
                           std::vector<double>& kf)
{
  Lap lap_{tm_.kernel};
  const uint32_t NF = a.n_fft * ovs, G = (uint32_t)first.size(), sc = m_.sc;
  kf.assign(NF, 0.0);
  if (!G) return;
  const cufftHandle h = plan(p_kern_, NF, G);
  spec_k_.ensure((size_t)p_kern_.batch * NF * sizeof(zC));
  ck(cudaMemsetAsync(spec_k_.p, 0, (size_t)G * NF * sizeof(zC), st_), "memset");
  std::vector<double> wgt(G);
  for (uint32_t g = 0; g < G; ++g) { const double h1 = m_.h1[first[g]]; wgt[g] = h1 > 0 ? count[g] / (h1 * h1) : 0.0; }
  bidx_.ensure(G * sizeof(uint32_t)); wgt_.ensure(G * sizeof(double));
  ck(cudaMemcpyAsync(bidx_.p, first.data(), G * sizeof(uint32_t), cudaMemcpyHostToDevice, st_), "H2D first");
  ck(cudaMemcpyAsync(wgt_.p, wgt.data(), G * sizeof(double), cudaMemcpyHostToDevice, st_), "H2D wgt");
  k_scatter<<<nblk((size_t)G * sc), 256, 0, st_>>>(nullptr, hw_.as<double>(), bidx_.as<uint32_t>(), G, m_.rows, sc, NF, 1, spec_k_.as<zC>());
  ckf(cufftExecZ2Z(h, spec_k_.as<zC>(), spec_k_.as<zC>(), CUFFT_INVERSE), "cufftExecZ2Z kern");
  out_d_.ensure(NF * sizeof(double));
  k_powsum<<<nblk(NF), 256, 0, st_>>>(spec_k_.as<zC>(), wgt_.as<double>(), 1, G, NF, out_d_.as<double>());
  ck(cudaMemcpyAsync(kf.data(), out_d_.p, NF * sizeof(double), cudaMemcpyDeviceToHost, st_), "D2H kf");
  sync();
}

void CudaFront::los_noncoherent(const Axes& a, uint32_t R, std::array<std::vector<double>, kCh>& pw)
{
  Lap lap_{tm_.noncoh};
  const uint32_t N = a.n_fft, rows = m_.rows, sc = m_.sc, B = kCh * rows;
  const cufftHandle h = plan(p_rows_, N, B);
  spec_.ensure((size_t)p_rows_.batch * N * sizeof(zC));
  ck(cudaMemsetAsync(spec_.p, 0, (size_t)B * N * sizeof(zC), st_), "memset");
  k_scatter<<<nblk((size_t)B * sc), 256, 0, st_>>>(values_.as<zC>(), hw_.as<double>(), nullptr, B, rows, sc, N, 0, spec_.as<zC>());
  ckf(cufftExecZ2Z(h, spec_.as<zC>(), spec_.as<zC>(), CUFFT_INVERSE), "cufftExecZ2Z rows");
  std::vector<double> wgt(B);
  for (uint32_t b = 0; b < B; ++b) { const double h1 = m_.h1[b % rows]; wgt[b] = h1 > 0 ? 1.0 / (h1 * h1 * R) : 0.0; }
  wgt_.ensure(B * sizeof(double));
  ck(cudaMemcpyAsync(wgt_.p, wgt.data(), B * sizeof(double), cudaMemcpyHostToDevice, st_), "H2D wgt");
  out_d_.ensure((size_t)kCh * N * sizeof(double));
  k_powsum<<<nblk((size_t)kCh * N), 256, 0, st_>>>(spec_.as<zC>(), wgt_.as<double>(), kCh, rows, N, out_d_.as<double>());
  for (uint32_t i = 0; i < kCh; ++i) {
    pw[i].resize(N);
    ck(cudaMemcpyAsync(pw[i].data(), out_d_.as<double>() + (size_t)i * N, N * sizeof(double), cudaMemcpyDeviceToHost, st_), "D2H pw");
  }
  sync();
}

void CudaFront::los_union_kernel(const Axes& a, uint32_t R, uint32_t ovs, std::vector<cd>& Uk, std::vector<double>& kmag)
{
  Lap lap_{tm_.union_k};
  const uint32_t NKF = a.n_fft * ovs, sc = m_.sc;
  const cufftHandle h = plan(p_fine_, NKF, 1);
  spec_f_.ensure((size_t)p_fine_.batch * NKF * sizeof(zC));
  ck(cudaMemsetAsync(spec_f_.p, 0, (size_t)NKF * sizeof(zC), st_), "memset");
  uk_.ensure(sc * sizeof(zC)); ukf_.ensure(sc * sizeof(float2));
  k_colsum<<<nblk(sc), 256, 0, st_>>>(nullptr, observed_.as<uint8_t>(), hw_.as<double>(), h1_.as<double>(), nullptr, nullptr, m_.rows, sc,
                                      scs_hz_, (double)R, 1, 1, NKF, uk_.as<zC>(), spec_f_.as<zC>());
  k_narrow<<<nblk(sc), 256, 0, st_>>>(uk_.as<zC>(), sc, ukf_.as<float2>());
  ckf(cufftExecZ2Z(h, spec_f_.as<zC>(), spec_f_.as<zC>(), CUFFT_INVERSE), "cufftExecZ2Z fine");
  out_d_.ensure(NKF * sizeof(double));
  k_abs<<<nblk(NKF), 256, 0, st_>>>(spec_f_.as<zC>(), NKF, out_d_.as<double>());
  Uk.resize(sc); kmag.resize(NKF);
  ck(cudaMemcpyAsync(Uk.data(), uk_.p, sc * sizeof(zC), cudaMemcpyDeviceToHost, st_), "D2H Uk");
  ck(cudaMemcpyAsync(kmag.data(), out_d_.p, NKF * sizeof(double), cudaMemcpyDeviceToHost, st_), "D2H kmag");
  sync();
}

void CudaFront::los_coherent(const Axes& a, uint32_t R, const RowSync& s, std::array<std::vector<cd>, kCh>& coh)
{
  Lap lap_{tm_.coh};
  const uint32_t N = a.n_fft, rows = m_.rows, sc = m_.sc;
  const cufftHandle h = plan(p_coh_, N, kCh);
  spec_c_.ensure((size_t)p_coh_.batch * N * sizeof(zC));
  ck(cudaMemsetAsync(spec_c_.p, 0, (size_t)kCh * N * sizeof(zC), st_), "memset");
  tau_.ensure(kCh * rows * sizeof(double)); phase_.ensure(rows * sizeof(double));
  ck(cudaMemcpyAsync(tau_.p, s.delay_s.data(), rows * sizeof(double), cudaMemcpyHostToDevice, st_), "H2D delay");
  ck(cudaMemcpyAsync(phase_.p, s.phase_rad.data(), rows * sizeof(double), cudaMemcpyHostToDevice, st_), "H2D phase");
  uc_.ensure((size_t)kCh * sc * sizeof(zC)); ucf_.ensure((size_t)kCh * sc * sizeof(float2));
  k_colsum<<<nblk((size_t)kCh * sc), 256, 0, st_>>>(values_.as<zC>(), observed_.as<uint8_t>(), hw_.as<double>(), h1_.as<double>(), tau_.as<double>(),
                                                    phase_.as<double>(), rows, sc, scs_hz_, (double)R, kCh, 0, N, uc_.as<zC>(), spec_c_.as<zC>());
  k_narrow<<<nblk((size_t)kCh * sc), 256, 0, st_>>>(uc_.as<zC>(), (size_t)kCh * sc, ucf_.as<float2>());
  ckf(cufftExecZ2Z(h, spec_c_.as<zC>(), spec_c_.as<zC>(), CUFFT_INVERSE), "cufftExecZ2Z coh");
  for (uint32_t i = 0; i < kCh; ++i) {
    coh[i].resize(N);
    ck(cudaMemcpyAsync(coh[i].data(), spec_c_.as<zC>() + (size_t)i * N, N * sizeof(zC), cudaMemcpyDeviceToHost, st_), "D2H coh");
  }
  sync();
}

void CudaFront::row_sums(const std::array<double, kCh>& delay, const std::vector<double>* drift, const std::array<bool, kCh>& found,
                         std::vector<std::array<cd, kCh>>& acc, std::vector<cd>* slope)
{
  Lap lap_{tm_.row_sums};
  ++tm_.row_sums_calls;
  const uint32_t rows = m_.rows, sc = m_.sc;
  std::vector<double> tau((size_t)kCh * rows);
  uint32_t fm = 0;
  for (uint32_t i = 0; i < kCh; ++i) {
    fm |= found[i] ? 1u << i : 0u;
    for (uint32_t r = 0; r < rows; ++r) tau[(size_t)i * rows + r] = delay[i] + (drift ? (*drift)[r] : 0.0);
  }
  tau_.ensure(tau.size() * sizeof(double)); acc_.ensure(tau.size() * sizeof(zC)); slope_.ensure(tau.size() * sizeof(zC));
  ck(cudaMemcpyAsync(tau_.p, tau.data(), tau.size() * sizeof(double), cudaMemcpyHostToDevice, st_), "H2D tau");
  k_row_sums<<<kCh * rows, 256, 0, st_>>>(values_.as<zC>(), observed_.as<uint8_t>(), span_.as<int2>(), comb_.as<uint32_t>(), tau_.as<double>(),
                                          rows, sc, scs_hz_, fm, slope ? 1 : 0, acc_.as<zC>(), slope_.as<zC>());
  std::vector<cd> ha(tau.size()), hs(slope ? tau.size() : 0);
  ck(cudaMemcpyAsync(ha.data(), acc_.p, ha.size() * sizeof(zC), cudaMemcpyDeviceToHost, st_), "D2H acc");
  if (slope) ck(cudaMemcpyAsync(hs.data(), slope_.p, hs.size() * sizeof(zC), cudaMemcpyDeviceToHost, st_), "D2H slope");
  sync();
  acc.assign(rows, std::array<cd, kCh>{});
  for (uint32_t r = 0; r < rows; ++r)
    for (uint32_t i = 0; i < kCh; ++i) if (found[i]) acc[r][i] = ha[(size_t)i * rows + r];
  if (slope) {
    slope->assign(rows, cd(0));
    for (uint32_t r = 0; r < rows; ++r)
      for (uint32_t i = 0; i < kCh; ++i) if (found[i]) (*slope)[r] += hs[(size_t)i * rows + r];
  }
}

void CudaFront::row_info(std::vector<uint32_t>& comb, std::vector<double>& fc_mean, std::vector<double>& n_obs)
{
  comb = m_.comb; fc_mean = m_.fc; n_obs = m_.nk;
}

void CudaFront::los_refine(const Axes& a, const std::array<long, kCh>& best, const std::array<long, kCh>& peak,
                           const std::array<bool, kCh>& found, std::array<double, kCh>& x, std::array<cd, kCh>& tap)
{
  Lap lap_{tm_.refine};
  uint32_t fm = 0; long2 in[kCh]; double out[3 * kCh] = {};
  for (uint32_t i = 0; i < kCh; ++i) { fm |= found[i] ? 1u << i : 0u; in[i] = make_long2(best[i], peak[i]); }
  ref_in_.ensure(sizeof(in)); ref_out_.ensure(sizeof(out));
  ck(cudaMemcpyAsync(ref_in_.p, in, sizeof(in), cudaMemcpyHostToDevice, st_), "H2D refine");
  const size_t shm = 2 * (size_t)m_.sc * sizeof(float2);
  ck(cudaFuncSetAttribute(k_los_refine, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)shm), "refine smem");
  k_los_refine<<<kCh, 32, shm, st_>>>(ucf_.as<float2>(), ukf_.as<float2>(), m_.sc, 2LL * a.n_fft, ref_in_.as<long2>(), fm, ref_out_.as<double>());
  ck(cudaMemcpyAsync(out, ref_out_.p, sizeof(out), cudaMemcpyDeviceToHost, st_), "D2H refine");
  sync();
  for (uint32_t i = 0; i < kCh; ++i) if (found[i]) { x[i] = out[3 * i]; tap[i] = cd(out[3 * i + 1], out[3 * i + 2]); }
}

const cufftDoubleComplex* CudaFront::ed(const Axes& a)
{
  Lap lap_{tm_.ed};
  const uint32_t rows = m_.rows;
  std::vector<double> wr(rows); double wsum = 0;
  for (uint32_t r = 0; r < rows; ++r) { wr[r] = host_hann(a.row_t_s.back() > 0 ? a.row_t_s[r] / a.row_t_s.back() : 0.5); wsum += wr[r]; }
  for (double& v : wr) v /= wsum;
  wrow_.ensure(rows * sizeof(double)); rowt_.ensure(rows * sizeof(double)); ed_.ensure((size_t)a.n_dopp * rows * sizeof(zC));
  ck(cudaMemcpyAsync(wrow_.p, wr.data(), rows * sizeof(double), cudaMemcpyHostToDevice, st_), "H2D win");
  ck(cudaMemcpyAsync(rowt_.p, a.row_t_s.data(), rows * sizeof(double), cudaMemcpyHostToDevice, st_), "H2D row_t");
  k_ed<<<nblk((size_t)a.n_dopp * rows), 256, 0, st_>>>(wrow_.as<double>(), rowt_.as<double>(), rows, a.n_dopp, a.dopp0_hz, a.dopp_step_hz, ed_.as<zC>());
  sync();   // wr (host) is released on return
  return ed_.as<zC>();
}

RdResult::Waveform CudaFront::waveform(const CfrWindow& w, const Axes& a)
{
  Lap lap_{tm_.waveform};
  RdResult::Waveform m;
  if (!a.valid) return m;
  const uint32_t rows = m_.rows, sc = m_.sc;
  const zC* d_ed = ed(a);
  m.w.resize(rows); m.wsum = 0;
  for (uint32_t r = 0; r < rows; ++r) { m.w[r] = host_hann(a.row_t_s.back() > 0 ? a.row_t_s[r] / a.row_t_s.back() : 0.5); m.wsum += m.w[r]; }
  const long O = RdResult::Waveform::kOvs;
  m.sc = sc; m.X = (long)a.n_range + 4;
  m.fc.assign(rows, 0.0); m.hh.assign(rows, 0.0); m.grp.assign(rows, -1);
  m.mask.assign(w.observed.begin(), w.observed.end());
  m.lo.assign(rows, 1); m.hi.assign(rows, 0);
  for (uint32_t r = 0; r < rows; ++r) {
    if (m_.grp[r] < 0) continue;
    const double kc = 0.5 * (m_.lo[r] + m_.hi[r]);
    m.fc[r] = ((kc - sc / 2.0)) * w.scs_hz;
    m.hh[r] = m_.h1[r] > 0 ? m_.h2[r] / (m_.h1[r] * m_.h1[r]) : 0.0;
    m.grp[r] = m_.grp[r]; m.lo[r] = (uint32_t)m_.lo[r]; m.hi[r] = (uint32_t)m_.hi[r];
  }
  m.w2sum = 0; for (uint32_t r = 0; r < rows; ++r) if (m.grp[r] >= 0) m.w2sum += m.w[r] * m.w[r] * m.hh[r];

  const uint32_t ng = (uint32_t)m_.first.size(), nx = (uint32_t)(2 * m.X * O + 1);
  std::vector<float2> hB((size_t)ng * nx), hB2((size_t)ng * nx);
  if (ng) {
    wf_rep_.ensure(ng * sizeof(uint32_t)); wf_B_.ensure(hB.size() * sizeof(float2)); wf_B2_.ensure(hB.size() * sizeof(float2));
    ck(cudaMemcpyAsync(wf_rep_.p, m_.first.data(), ng * sizeof(uint32_t), cudaMemcpyHostToDevice, st_), "H2D rep");
    k_wfB<<<nblk(hB.size()), 256, 0, st_>>>(hw_.as<double>(), observed_.as<uint8_t>(), span_.as<int2>(), wf_rep_.as<uint32_t>(), sc, ng, nx,
                                            (float)m.X, (float)O, (float)a.n_fft, wf_B_.as<float2>(), wf_B2_.as<float2>());
    ck(cudaMemcpyAsync(hB.data(), wf_B_.p, hB.size() * sizeof(float2), cudaMemcpyDeviceToHost, st_), "D2H B");
    ck(cudaMemcpyAsync(hB2.data(), wf_B2_.p, hB2.size() * sizeof(float2), cudaMemcpyDeviceToHost, st_), "D2H B2");
  }
  const size_t nq = (size_t)a.n_dopp * sc;
  e_over_h_.ensure((size_t)a.n_dopp * rows * sizeof(float2)); wf_Q_.ensure(nq * sizeof(float2));
  k_eoh<<<nblk((size_t)a.n_dopp * rows), 256, 0, st_>>>(d_ed, h1_.as<double>(), rows, a.n_dopp, e_over_h_.as<float2>());
  k_Q<<<nblk(nq), 256, 0, st_>>>(e_over_h_.as<float2>(), hw_.as<double>(), rows, sc, a.n_dopp, wf_Q_.as<float2>());
  m.Q.resize(nq);
  ck(cudaMemcpyAsync(m.Q.data(), wf_Q_.p, nq * sizeof(float2), cudaMemcpyDeviceToHost, st_), "D2H Q");
  sync();
  m.B.resize(ng); m.B2.resize(ng);
  for (uint32_t g = 0; g < ng; ++g) {
    const uint32_t r = m_.first[g];
    m.B[g].resize(nx); m.B2[g].resize(nx);
    for (uint32_t u = 0; u < nx; ++u) {
      const float2 b = hB[(size_t)g * nx + u], b2 = hB2[(size_t)g * nx + u];
      m.B[g][u] = cd(b.x, b.y) / m_.h1[r]; m.B2[g][u] = cd(b2.x, b2.y) / m_.h2[r];
    }
  }
  return m;
}

} // namespace nr_isac::coherent
