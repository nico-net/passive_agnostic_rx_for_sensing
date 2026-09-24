/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* Task 10: CUDA range_doppler() + envelope(). See coherent_cuda.h for the stage split rationale.
 *
 * Every formula here is a direct transcription of coherent_core.cc's CPU range_doppler()/envelope()
 * (read, not guessed, before writing this file) -- row_span/row_comb/hann/baseband_hz/dopp_half are
 * small pure functions duplicated from that file's anonymous namespace (they are not exported, and
 * coherent_core.cc itself is append-only per the Task 10 dispatch, to avoid conflicting with a
 * parallel find_los fix on another branch). Kept in double precision throughout (cuFFT Z2Z, not
 * C2C): the problem sizes here (a few hundred rows x a few thousand subcarriers) are trivial for a
 * modern GPU even at reduced fp64 throughput, and double precision makes the <1e-3 relative-error
 * parity target a near-certainty rather than something to chase with mixed precision.
 */
#include "coherent_cuda.h"

#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "robust_stats.h"

namespace nr_isac::coherent {
namespace {

using zC = cufftDoubleComplex;
constexpr double kPi = 3.14159265358979323846;

void cuda_check(cudaError_t s, const char* op) { if (s != cudaSuccess) throw std::runtime_error(std::string(op) + ": " + cudaGetErrorString(s)); }
void cufft_check(cufftResult s, const char* op) { if (s != CUFFT_SUCCESS) throw std::runtime_error(std::string(op) + " failed (cufft " + std::to_string((int)s) + ")"); }

// ---- host-side duplicates of small pure functions from coherent_core.cc's anonymous namespace
// (not exported there; see file header). Kept byte-identical to their CPU originals.
double host_hann(double u) { return 0.5 - 0.5 * std::cos(2 * kPi * u); }
bool host_row_span(const CfrWindow& w, uint32_t r, uint32_t* lo, uint32_t* hi)
{
  int l = -1, h = -1;
  for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) { if (l < 0) l = (int)k; h = (int)k; }
  if (l < 0) return false;
  *lo = (uint32_t)l; *hi = (uint32_t)h; return true;
}
uint32_t host_row_comb(const CfrWindow& w, uint32_t r)
{
  uint32_t c = 0, prev = UINT32_MAX;
  for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) {
    if (prev != UINT32_MAX && (c == 0 || k - prev < c)) c = k - prev;
    prev = k;
  }
  return c;
}
// coherent_core.cc's dopp_half()/dopp_ok(), duplicated (same reason as above).
bool host_dopp_ok(const Axes& a, long d) { return d >= 0 && d < (long)a.n_dopp && std::abs(a.dopp0_hz + d * a.dopp_step_hz) > a.notch_half_bins * a.dopp_step_hz; }
Vec3 host_unit(const Vec3& v) { const double n = norm(v); return n > 0 ? v / n : Vec3{}; }
uint32_t host_dopp_half(const Axes& a, const Geometry& geo, const std::array<bool, kCh>& used, const Vec3& x)
{
  if (!(a.v_max_mps > 0) || !(a.dopp_step_hz > 0)) return 0;
  Vec3 u[kCh]; for (uint32_t i = 0; i < kCh; ++i) u[i] = host_unit(x - geo.rx[i]);
  double m = 0;
  for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j) if (used[i] && used[j]) m = std::max(m, norm2(u[i] - u[j]));
  return (uint32_t)std::ceil(a.v_max_mps * std::sqrt(m) / (a.lambda_m * a.dopp_step_hz) - 1e-9);
}

// ---- device kernels -------------------------------------------------------------------------
__device__ inline double bb_hz(uint32_t k, uint32_t sc, double scs) { return ((double)k - sc * 0.5) * scs; }

__global__ void k_widen(const float2* in, size_t n, zC* out)
{
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < n; j += (size_t)blockDim.x * gridDim.x) { out[j].x = in[j].x; out[j].y = in[j].y; }
}

// Per (channel i, subcarrier k): mean over rows observing k of the derotated value (LOS+row-sync
// delay/phase already applied, no static subtraction yet) -- coherent_core.cc range_doppler()'s
// `stat[k]`/`cnt[k]`.
__global__ void k_stat(const zC* values, const uint8_t* observed, const double* tau, const double* phase,
                       uint32_t rows, uint32_t sc, double scs, uint32_t nch, zC* stat)
{
  for (size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x; idx < (size_t)nch * sc; idx += (size_t)blockDim.x * gridDim.x) {
    const uint32_t i = (uint32_t)(idx / sc), k = (uint32_t)(idx % sc);
    double re = 0, im = 0; uint32_t c = 0;
    const double f = bb_hz(k, sc, scs);
    for (uint32_t r = 0; r < rows; ++r) {
      if (!observed[(size_t)r * sc + k]) continue;
      const double ang = 2 * kPi * f * tau[(size_t)i * rows + r] - phase[r];
      double si, co; sincos(ang, &si, &co);
      const zC v = values[((size_t)i * rows + r) * sc + k];
      re += v.x * co - v.y * si; im += v.x * si + v.y * co;
      ++c;
    }
    stat[idx].x = c > 0 ? re / c : 0.0; stat[idx].y = c > 0 ? im / c : 0.0;
  }
}

// Per (channel i, row r): the row's own Hann-weighted derotated sum (NOT static-subtracted, NOT yet
// divided by the row's Hann weight sum) -- range_doppler()'s `los`/`los_rows` accumulator, one row's
// contribution. Host divides by wsum_row[r] (channel-independent, computed on the host) and averages
// over valid-span rows.
__global__ void k_los_row(const zC* values, const uint8_t* observed, const int2* span, const double* tau,
                          const double* phase, uint32_t rows, uint32_t sc, double scs, uint32_t nch, zC* row_los)
{
  // One block per (channel, row), threads stride the row's subcarriers, shared-memory tree reduce
  // (one thread per (i,r) was 256 threads serially looping ~3k FP64 sincos: ~11 ms).
  __shared__ double sre[256], sim[256];
  const size_t idx = blockIdx.x;
  if (idx >= (size_t)nch * rows) return;
  const uint32_t i = (uint32_t)(idx / rows), r = (uint32_t)(idx % rows);
  const int2 s = span[r];
  double re = 0, im = 0;
  if (s.y >= s.x) {
    const double ta = tau[(size_t)i * rows + r], ph = phase[r];
    for (int k = s.x + (int)threadIdx.x; k <= s.y; k += blockDim.x) {
      if (!observed[(size_t)r * sc + (uint32_t)k]) continue;
      const double u = (s.y > s.x) ? (double)(k - s.x) / (s.y - s.x) : 0.5;
      const double win = 0.5 - 0.5 * cospi(2 * u);
      const double ang = 2 * kPi * bb_hz((uint32_t)k, sc, scs) * ta - ph;
      double si, co; sincos(ang, &si, &co);
      const zC v = values[((size_t)i * rows + r) * sc + (uint32_t)k];
      re += (v.x * co - v.y * si) * win; im += (v.x * si + v.y * co) * win;
    }
  }
  sre[threadIdx.x] = re; sim[threadIdx.x] = im; __syncthreads();
  for (unsigned h = blockDim.x / 2; h > 0; h >>= 1) {
    if (threadIdx.x < h) { sre[threadIdx.x] += sre[threadIdx.x + h]; sim[threadIdx.x] += sim[threadIdx.x + h]; }
    __syncthreads();
  }
  if (threadIdx.x == 0) { row_los[idx].x = sre[0]; row_los[idx].y = sim[0]; }
}

// Build the zero-padded, centred, Hann-windowed, static-subtracted row spectrum for a batched
// inverse FFT. `spectrum` must be zeroed first (comb/gapped rows leave most of it untouched). Every
// (r,k) maps to a distinct wrapped index (n_fft = next_pow2(subcarriers) >= subcarriers, and a row's
// k range spans < subcarriers consecutive centred indices), so this is a plain write, not atomicAdd.
__global__ void k_build_spectrum(const zC* values, const uint8_t* observed, const int2* span, const double* tau,
                                 const double* phase, const zC* stat, uint32_t rows, uint32_t sc, uint32_t nfft,
                                 double scs, uint32_t nch, zC* spectrum)
{
  for (size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x; idx < (size_t)nch * rows * sc; idx += (size_t)blockDim.x * gridDim.x) {
    const uint32_t k = (uint32_t)(idx % sc); const size_t tmp = idx / sc;
    const uint32_t r = (uint32_t)(tmp % rows), i = (uint32_t)(tmp / rows);
    if (!observed[(size_t)r * sc + k]) continue;
    const int2 s = span[r]; if (s.y < s.x) continue;
    const double u = (s.y > s.x) ? (double)((long)k - s.x) / (s.y - s.x) : 0.5;
    const double win = 0.5 - 0.5 * cos(2 * kPi * u);
    const double f = bb_hz(k, sc, scs);
    const double ang = 2 * kPi * f * tau[(size_t)i * rows + r] - phase[r];
    double si, co; sincos(ang, &si, &co);
    const zC v = values[((size_t)i * rows + r) * sc + k];
    const zC st = stat[(size_t)i * sc + k];
    double dre = (v.x * co - v.y * si) - st.x, dim = (v.x * si + v.y * co) - st.y;
    dre *= win; dim *= win;
    const long q = (long)k - (long)(sc / 2);
    const long qi = ((q % (long)nfft) + (long)nfft) % (long)nfft;
    zC* out = &spectrum[((size_t)i * rows + r) * nfft + (size_t)qi];
    out->x = dre; out->y = dim;
  }
}

// cuFFT's inverse is unnormalised (no 1/N); CPU's fft_inplace(inverse=true) divides by N and the
// caller then multiplies by N/wsum, net 1/wsum -- so cuFFT's raw output needs only *1/wsum here.
__global__ void k_crop_norm(const zC* spectrum, const float* wsum_row, uint32_t rows, uint32_t nfft, uint32_t nrange,
                            long far0, int far_ok, uint32_t nch, zC* prof_near, zC* prof_far)
{
  for (size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x; idx < (size_t)nch * rows * nrange; idx += (size_t)blockDim.x * gridDim.x) {
    const uint32_t m = (uint32_t)(idx % nrange); const size_t tmp = idx / nrange;
    const uint32_t r = (uint32_t)(tmp % rows), i = (uint32_t)(tmp / rows);
    const double n = wsum_row[r] > 0 ? 1.0 / wsum_row[r] : 0.0;
    const zC v = spectrum[((size_t)i * rows + r) * nfft + m];
    prof_near[idx].x = v.x * n; prof_near[idx].y = v.y * n;
    if (far_ok) {
      const zC vf = spectrum[((size_t)i * rows + r) * nfft + (size_t)(far0 + m)];
      prof_far[idx].x = vf.x * n; prof_far[idx].y = vf.y * n;
    }
  }
}

// RD[i][m][d] = sum_r prof[i][r][m] * ed[d][r]. Reused for both the near (final RD) and far (noise
// estimate) profiles.
__global__ void k_nudft(const zC* prof, const zC* ed, uint32_t rows, uint32_t nrange, uint32_t ndopp, uint32_t nch, zC* rd)
{
  for (size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x; idx < (size_t)nch * nrange * ndopp; idx += (size_t)blockDim.x * gridDim.x) {
    const uint32_t d = (uint32_t)(idx % ndopp); const size_t tmp = idx / ndopp;
    const uint32_t m = (uint32_t)(tmp % nrange), i = (uint32_t)(tmp / nrange);
    double re = 0, im = 0;
    for (uint32_t r = 0; r < rows; ++r) {
      const zC p = prof[((size_t)i * rows + r) * nrange + m];
      const zC e = ed[(size_t)d * rows + r];
      re += p.x * e.x - p.y * e.y; im += p.x * e.y + p.y * e.x;
    }
    rd[idx].x = re; rd[idx].y = im;
  }
}

// 4-point Lagrange cubic in |RD|, same as coherent_core.cc's MagInterp/sample_rd magnitude part.
__device__ inline double mag_interp(const zC* rd, uint32_t ch, uint32_t nrange, uint32_t ndopp, double bin, uint32_t d)
{
  if (nrange == 0 || !(bin >= 0) || bin > (double)(nrange - 1)) return -1.0;
  const long b0 = (long)bin; const double t = bin - b0;
  auto magat = [&](long b) {
    b = b < 0 ? 0 : (b > (long)nrange - 1 ? (long)nrange - 1 : b);
    const zC v = rd[((size_t)ch * nrange + (uint32_t)b) * ndopp + d];
    return sqrt(v.x * v.x + v.y * v.y);
  };
  const double pm = magat(b0 - 1), p0 = magat(b0), p1 = magat(b0 + 1), p2 = magat(b0 + 2);
  const double m = -pm * t * (t - 1) * (t - 2) / 6 + p0 * (t + 1) * (t - 1) * (t - 2) / 2
                   - p1 * (t + 1) * t * (t - 2) / 2 + p2 * (t + 1) * t * (t - 1) / 6;
  return m > 0 ? m : 0.0;
}

struct GeoParams { double tx[3]; double rx[4][3]; };

// One thread per (tested Doppler index t, voxel v): per-channel sliding max of |RD|^2/noise over the
// voxel's own Doppler half-width, at the channel's own excess-delay bin -- coherent_core.cc's
// envelope(). A channel with an out-of-range bin (MagInterp::ok == false) contributes 0, same as the
// CPU `if (!mi.ok) continue;`; a non-ok Doppler bin contributes 0 to the max, same as CPU's
// `ok[d] ? mi.at(...) : 0.0` fed into sliding_max (0 can never win a max over non-negative power).
__global__ void k_envelope(const zC* rd, const float* inv_noise, const uint8_t* los_found, const uint32_t* tested_dopp,
                           uint32_t nt, const uint8_t* dopp_ok_mask, const uint32_t* dh, GeoParams geo, double origin_x,
                           double origin_y, double origin_z, double step, uint32_t nx, uint32_t ny, uint32_t nz,
                           uint32_t nrange, uint32_t ndopp, double delay_step_s, double c_mps, float* E)
{
  const size_t nv = (size_t)nx * ny * nz;
  for (size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x; idx < (size_t)nt * nv; idx += (size_t)blockDim.x * gridDim.x) {
    const size_t v = idx % nv; const uint32_t t = (uint32_t)(idx / nv);
    const long d0 = (long)tested_dopp[t]; const uint32_t h = dh[v];
    const size_t ix = v % nx, iy = (v / nx) % ny, iz = v / ((size_t)nx * ny);
    const double xx = origin_x + ix * step, xy = origin_y + iy * step, xz = origin_z + iz * step;
    float e = 0.f;
    for (uint32_t ch = 0; ch < 4; ++ch) {
      if (!los_found[ch]) continue;
      const double dtx = sqrt((xx - geo.tx[0]) * (xx - geo.tx[0]) + (xy - geo.tx[1]) * (xy - geo.tx[1]) + (xz - geo.tx[2]) * (xz - geo.tx[2]));
      const double drx = sqrt((xx - geo.rx[ch][0]) * (xx - geo.rx[ch][0]) + (xy - geo.rx[ch][1]) * (xy - geo.rx[ch][1]) + (xz - geo.rx[ch][2]) * (xz - geo.rx[ch][2]));
      const double dbase = sqrt((geo.tx[0] - geo.rx[ch][0]) * (geo.tx[0] - geo.rx[ch][0]) + (geo.tx[1] - geo.rx[ch][1]) * (geo.tx[1] - geo.rx[ch][1]) + (geo.tx[2] - geo.rx[ch][2]) * (geo.tx[2] - geo.rx[ch][2]));
      const double bin = (dtx + drx - dbase) / c_mps / delay_step_s;
      if (!(bin >= 0) || bin > (double)(nrange - 1)) continue;
      double best = 0.0;
      const long lo = d0 - (long)h, hi = d0 + (long)h;
      for (long dd = lo; dd <= hi; ++dd) {
        if (dd < 0 || dd >= (long)ndopp || !dopp_ok_mask[dd]) continue;
        const double m = mag_interp(rd, ch, nrange, ndopp, bin, (uint32_t)dd);
        const double val = m * m * (double)inv_noise[ch];
        if (val > best) best = val;
      }
      e += (float)best;
    }
    E[idx] = e;
  }
}

} // namespace

struct CudaCoherent::Impl {
  cudaStream_t stream = nullptr;
  cufftHandle plan = 0; long plan_nfft = -1; long plan_batch = -1;
  cudaEvent_t ev0{}, ev1{};

  // persistent device buffers, grown on demand, never freed per CPI (see header)
  struct Buf {
    void* p = nullptr; size_t cap = 0;
    ~Buf() { if (p) cudaFree(p); }
    void ensure(size_t bytes) {
      if (bytes <= cap) return;
      if (p) cudaFree(p);
      p = nullptr;
      cuda_check(cudaMalloc(&p, bytes), "cudaMalloc");
      cap = bytes;
    }
  };
  Buf values, values_f, observed, span, tau, phase, stat, wsum_row, row_los, spectrum, prof_near, prof_far, ed, rd, far_rd, E;
  Buf ev_tested_dopp, ev_dh, ev_dopp_ok, ev_inv_noise, ev_los_found;  // detect()'s own small persistent uploads

  // host-side state kept across range_doppler() -> detect() within one CPI
  RdResult cached;
  uint32_t cur_nch = kCh, cur_rows = 0, cur_nrange = 0, cur_ndopp = 0;
  Timing timing;
  std::vector<float> last_E;

  Impl()
  {
    cuda_check(cudaStreamCreate(&stream), "cudaStreamCreate");
    cuda_check(cudaEventCreate(&ev0), "cudaEventCreate");
    cuda_check(cudaEventCreate(&ev1), "cudaEventCreate");
  }
  ~Impl()
  {
    if (plan) cufftDestroy(plan);
    cudaEventDestroy(ev0); cudaEventDestroy(ev1);
    cudaStreamDestroy(stream);
  }
  double lap()
  {
    cuda_check(cudaEventRecord(ev1, stream), "cudaEventRecord");
    cuda_check(cudaEventSynchronize(ev1), "cudaEventSynchronize");
    float ms = 0; cuda_check(cudaEventElapsedTime(&ms, ev0, ev1), "cudaEventElapsedTime");
    std::swap(ev0, ev1);
    return (double)ms;
  }
  // Grow-only, like Buf::ensure: `rows` (hence the batch = nch*rows) changes CPI to CPI on real
  // traffic, and cufftPlan1d is expensive to create (tens of ms, measured) -- replanning on every
  // CPI was the dominant cost of the whole GPU path before this fix. A plan sized for the largest
  // batch seen so far is reused for every smaller CPI (cufftExecZ2Z then transforms some unused
  // padding past the real batch, which nothing downstream reads -- see call site). Only grows when
  // n_fft itself changes (a different window subcarrier count) or a CPI needs a bigger batch than
  // ever seen before.
  void get_plan(long nfft, long min_batch)
  {
    if (plan && plan_nfft == nfft && plan_batch >= min_batch) return;
    if (plan) { cufft_check(cufftDestroy(plan), "cufftDestroy"); plan = 0; }
    const long batch = (plan_nfft == nfft) ? std::max(plan_batch, min_batch) : min_batch;
    cufft_check(cufftPlan1d(&plan, (int)nfft, CUFFT_Z2Z, (int)batch), "cufftPlan1d");
    cufft_check(cufftSetStream(plan, stream), "cufftSetStream");
    plan_nfft = nfft; plan_batch = batch;
  }
};

bool CudaCoherent::available()
{
  int n = 0;
  return cudaGetDeviceCount(&n) == cudaSuccess && n > 0;
}

CudaCoherent::CudaCoherent() : impl_(std::make_unique<Impl>()) {}
CudaCoherent::~CudaCoherent() = default;

RdResult CudaCoherent::range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s, bool download_rd)
{
  Impl& I = *impl_;
  RdResult out; out.rd.axes = a; out.los_found = L.found;
  if (!a.valid) { I.cached = out; return out; }
  const uint32_t rows = w.rows, sc = w.subcarriers, nfft = a.n_fft, nrange = a.n_range, ndopp = a.n_dopp;
  const uint32_t nch = kCh;

  // host-side row spans, comb, and the slow-time Hann window (identical formulas to
  // coherent_core.cc's range_doppler(); see file header for why they are duplicated here).
  std::vector<int2> h_span(rows); std::vector<float> h_wsum(rows, 0.f);
  std::vector<double> h_tau((size_t)nch * rows), h_phase(rows);
  uint32_t cmax = 1;
  for (uint32_t r = 0; r < rows; ++r) {
    uint32_t lo, hi;
    const bool ok = host_row_span(w, r, &lo, &hi);
    h_span[r] = ok ? int2{(int)lo, (int)hi} : int2{1, 0};
    if (ok) { double ws = 0; for (uint32_t k = lo; k <= hi; ++k) if (w.observed[w.cell(r, k)]) ws += host_hann(hi > lo ? (double)(k - lo) / (hi - lo) : 0.5); h_wsum[r] = (float)ws; }
    cmax = std::max(cmax, host_row_comb(w, r));
    h_phase[r] = s.phase_rad.empty() ? 0.0 : s.phase_rad[r];
    for (uint32_t i = 0; i < nch; ++i) h_tau[(size_t)i * rows + r] = L.delay_s[i] + (s.delay_s.empty() ? 0.0 : s.delay_s[r]);
  }
  const long rep = (long)nfft / cmax, far0 = rep / 2 - (long)nrange / 2;
  const bool far_ok = far0 >= 2 * (long)nrange && far0 + 2 * (long)nrange <= rep;

  // Doppler phasor table ed[d][r] = win_r/wsum_total * e^{-j2pi f_d t_r} -- channel-independent, same
  // formula range_doppler() and build_waveform() both use.
  std::vector<double> win(rows); double wsum_t = 0;
  for (uint32_t r = 0; r < rows; ++r) { win[r] = host_hann(a.row_t_s.back() > 0 ? a.row_t_s[r] / a.row_t_s.back() : 0.5); wsum_t += win[r]; }
  std::vector<zC> h_ed((size_t)ndopp * rows);
  for (uint32_t d = 0; d < ndopp; ++d) for (uint32_t r = 0; r < rows; ++r) {
    const double ang = -2 * kPi * (a.dopp0_hz + d * a.dopp_step_hz) * a.row_t_s[r];
    const double sc_ = win[r] / wsum_t;
    h_ed[(size_t)d * rows + r] = zC{sc_ * std::cos(ang), sc_ * std::sin(ang)};
  }

  cuda_check(cudaEventRecord(I.ev0, I.stream), "cudaEventRecord");

  // upload
  I.values.ensure((size_t)nch * rows * sc * sizeof(zC));
  {
    // Same [antenna][row][subcarrier] layout as CfrWindow::sample(): upload the floats as-is and widen
    // on the device (a host-side widening loop cost ~9 ms/CPI).
    const size_t n = (size_t)nch * rows * sc;
    I.values_f.ensure(n * sizeof(float2));
    cuda_check(cudaMemcpyAsync(I.values_f.p, w.values.data(), n * sizeof(float2), cudaMemcpyHostToDevice, I.stream), "H2D values");
    k_widen<<<(unsigned)std::min<size_t>(65535, (n + 255) / 256), 256, 0, I.stream>>>((const float2*)I.values_f.p, n, (zC*)I.values.p);
  }
  I.observed.ensure((size_t)rows * sc * sizeof(uint8_t));
  cuda_check(cudaMemcpyAsync(I.observed.p, w.observed.data(), (size_t)rows * sc, cudaMemcpyHostToDevice, I.stream), "H2D observed");
  I.span.ensure(rows * sizeof(int2)); cuda_check(cudaMemcpyAsync(I.span.p, h_span.data(), rows * sizeof(int2), cudaMemcpyHostToDevice, I.stream), "H2D span");
  I.tau.ensure((size_t)nch * rows * sizeof(double)); cuda_check(cudaMemcpyAsync(I.tau.p, h_tau.data(), h_tau.size() * sizeof(double), cudaMemcpyHostToDevice, I.stream), "H2D tau");
  I.phase.ensure(rows * sizeof(double)); cuda_check(cudaMemcpyAsync(I.phase.p, h_phase.data(), rows * sizeof(double), cudaMemcpyHostToDevice, I.stream), "H2D phase");
  I.wsum_row.ensure(rows * sizeof(float)); cuda_check(cudaMemcpyAsync(I.wsum_row.p, h_wsum.data(), rows * sizeof(float), cudaMemcpyHostToDevice, I.stream), "H2D wsum");
  I.ed.ensure(h_ed.size() * sizeof(zC)); cuda_check(cudaMemcpyAsync(I.ed.p, h_ed.data(), h_ed.size() * sizeof(zC), cudaMemcpyHostToDevice, I.stream), "H2D ed");
  I.timing.upload_ms = I.lap();

  // stat[i][k], row_los[i][r]
  I.stat.ensure((size_t)nch * sc * sizeof(zC)); I.row_los.ensure((size_t)nch * rows * sizeof(zC));
  auto launch = [](size_t n) { dim3 t(256), b((unsigned)std::min<size_t>(65535, (n + 255) / 256)); return std::make_pair(b, t); };
  {
    auto [b, t] = launch((size_t)nch * sc);
    k_stat<<<b, t, 0, I.stream>>>((zC*)I.values.p, (uint8_t*)I.observed.p, (double*)I.tau.p, (double*)I.phase.p, rows, sc, w.scs_hz, nch, (zC*)I.stat.p);
  }
  {
    k_los_row<<<(unsigned)(nch * rows), 256, 0, I.stream>>>((zC*)I.values.p, (uint8_t*)I.observed.p, (int2*)I.span.p, (double*)I.tau.p, (double*)I.phase.p, rows, sc, w.scs_hz, nch, (zC*)I.row_los.p);
  }
  I.timing.build_ms = I.lap();

  // spectrum: zero, fill, batched inverse FFT. get_plan() first (grow-only; see its comment), then
  // size the buffer to the PLAN's batch (>= this CPI's own nch*rows) so cufftExecZ2Z never reads/
  // writes past the allocation even when the plan is left over-sized from an earlier, bigger CPI --
  // only the first nch*rows*nfft cells are zeroed/filled/read; the rest is untouched by everything
  // downstream (crop_norm/nudft index with THIS CPI's `rows`, never the plan's).
  I.get_plan(nfft, (long)nch * rows);
  I.spectrum.ensure((size_t)std::max<long>(I.plan_batch, (long)nch * rows) * nfft * sizeof(zC));
  cuda_check(cudaMemsetAsync(I.spectrum.p, 0, (size_t)nch * rows * nfft * sizeof(zC), I.stream), "memset spectrum");
  {
    auto [b, t] = launch((size_t)nch * rows * sc);
    k_build_spectrum<<<b, t, 0, I.stream>>>((zC*)I.values.p, (uint8_t*)I.observed.p, (int2*)I.span.p, (double*)I.tau.p, (double*)I.phase.p,
                                            (zC*)I.stat.p, rows, sc, nfft, w.scs_hz, nch, (zC*)I.spectrum.p);
  }
  cufft_check(cufftExecZ2Z(I.plan, (zC*)I.spectrum.p, (zC*)I.spectrum.p, CUFFT_INVERSE), "cufftExecZ2Z");
  I.timing.fft_ms = I.lap();

  // crop + normalise
  I.prof_near.ensure((size_t)nch * rows * nrange * sizeof(zC));
  if (far_ok) I.prof_far.ensure((size_t)nch * rows * nrange * sizeof(zC));
  {
    auto [b, t] = launch((size_t)nch * rows * nrange);
    k_crop_norm<<<b, t, 0, I.stream>>>((zC*)I.spectrum.p, (float*)I.wsum_row.p, rows, nfft, nrange, far0, far_ok ? 1 : 0, nch,
                                       (zC*)I.prof_near.p, far_ok ? (zC*)I.prof_far.p : nullptr);
  }
  I.timing.crop_norm_ms = I.lap();

  // NUDFT: near -> resident RD cube; far (if any) -> noise estimate
  I.rd.ensure((size_t)nch * nrange * ndopp * sizeof(zC));
  {
    auto [b, t] = launch((size_t)nch * nrange * ndopp);
    k_nudft<<<b, t, 0, I.stream>>>((zC*)I.prof_near.p, (zC*)I.ed.p, rows, nrange, ndopp, nch, (zC*)I.rd.p);
  }
  if (far_ok) {
    I.far_rd.ensure((size_t)nch * nrange * ndopp * sizeof(zC));
    auto [b, t] = launch((size_t)nch * nrange * ndopp);
    k_nudft<<<b, t, 0, I.stream>>>((zC*)I.prof_far.p, (zC*)I.ed.p, rows, nrange, ndopp, nch, (zC*)I.far_rd.p);
  }
  I.timing.nudft_ms = I.lap();

  // download RD (always -- detect()/refine() run on the CPU and need it host-side; see header),
  // los_tap, and noise.
  std::vector<zC> h_rd((size_t)nch * nrange * ndopp);
  cuda_check(cudaMemcpyAsync(h_rd.data(), I.rd.p, h_rd.size() * sizeof(zC), cudaMemcpyDeviceToHost, I.stream), "D2H rd");
  std::vector<zC> h_far;
  if (far_ok) { h_far.resize((size_t)nch * nrange * ndopp); cuda_check(cudaMemcpyAsync(h_far.data(), I.far_rd.p, h_far.size() * sizeof(zC), cudaMemcpyDeviceToHost, I.stream), "D2H far"); }
  std::vector<zC> h_row_los((size_t)nch * rows);
  cuda_check(cudaMemcpyAsync(h_row_los.data(), I.row_los.p, h_row_los.size() * sizeof(zC), cudaMemcpyDeviceToHost, I.stream), "D2H row_los");
  cuda_check(cudaStreamSynchronize(I.stream), "sync D2H");
  I.timing.download_ms = I.lap();

  out.rd.v.resize((size_t)nch * nrange * ndopp);
  for (size_t n = 0; n < out.rd.v.size(); ++n) out.rd.v[n] = cf((float)h_rd[n].x, (float)h_rd[n].y);

  for (uint32_t i = 0; i < nch; ++i) {
    cd los = 0; uint32_t los_rows = 0;
    for (uint32_t r = 0; r < rows; ++r) {
      if (h_span[r].y < h_span[r].x || !(h_wsum[r] > 0)) continue;
      const zC v = h_row_los[(size_t)i * rows + r];
      los += cd(v.x, v.y) / (double)h_wsum[r]; ++los_rows;
    }
    out.los_tap[i] = los_rows ? los / (double)los_rows : cd(0);

    std::vector<double> pw;
    if (far_ok) {
      for (uint32_t m = 0; m < nrange; ++m) for (uint32_t d : a.tested_dopp) {
        const zC v = h_far[((size_t)i * nrange + m) * ndopp + d]; pw.push_back(v.x * v.x + v.y * v.y);
      }
    } else {
      for (uint32_t m = 0; m < nrange; ++m) for (uint32_t d : a.tested_dopp) {
        const zC v = h_rd[((size_t)i * nrange + m) * ndopp + d]; pw.push_back(v.x * v.x + v.y * v.y);
      }
    }
    out.noise[i] = pw.empty() ? 1.0 : std::max(median(pw) / std::log(2.0), std::numeric_limits<double>::min());
  }

  const auto t_wf0 = std::chrono::steady_clock::now();
  out.wf = build_waveform(w, a);
  I.timing.wf_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wf0).count();
  I.timing.rd_total_ms = I.timing.upload_ms + I.timing.build_ms + I.timing.fft_ms + I.timing.crop_norm_ms
                         + I.timing.nudft_ms + I.timing.download_ms + I.timing.wf_ms;

  I.cur_nch = nch; I.cur_rows = rows; I.cur_nrange = nrange; I.cur_ndopp = ndopp;
  I.cached = out;
  (void)download_rd;   // RD is always downloaded (see header); nothing extra to do with this flag
  return out;
}

std::vector<Detection> CudaCoherent::detect(const Grid& g, const Geometry& geo, const DetectParams& p, std::vector<float>* topview_max)
{
  Impl& I = *impl_;
  const RdResult& R = I.cached;
  const Axes& a = R.rd.axes;
  if (!a.valid || R.rd.v.empty()) return {};
  const size_t nt = a.tested_dopp.size(), nv = g.size();
  if (nt == 0 || nv == 0) return {};

  std::vector<uint8_t> h_ok(a.n_dopp); for (uint32_t d = 0; d < a.n_dopp; ++d) h_ok[d] = host_dopp_ok(a, d) ? 1 : 0;
  std::vector<uint32_t> h_dh(nv); for (size_t v = 0; v < nv; ++v) h_dh[v] = host_dopp_half(a, geo, R.los_found, g.at(v));
  std::vector<uint32_t> h_t(a.tested_dopp.begin(), a.tested_dopp.end());
  std::vector<float> h_invn(kCh, 0.f); for (uint32_t i = 0; i < kCh; ++i) h_invn[i] = R.noise[i] > 0 ? (float)(1.0 / R.noise[i]) : 0.f;
  std::vector<uint8_t> h_los(kCh); for (uint32_t i = 0; i < kCh; ++i) h_los[i] = R.los_found[i] ? 1 : 0;

  cuda_check(cudaEventRecord(I.ev0, I.stream), "cudaEventRecord");
  // reuse I.rd (already resident from range_doppler()); upload only the small envelope-specific state,
  // into the SAME persistent (grown-on-demand, never-freed-per-CPI) buffers every call.
  I.ev_tested_dopp.ensure(h_t.size() * sizeof(uint32_t)); cuda_check(cudaMemcpyAsync(I.ev_tested_dopp.p, h_t.data(), h_t.size() * sizeof(uint32_t), cudaMemcpyHostToDevice, I.stream), "H2D tested_dopp");
  I.ev_dh.ensure(h_dh.size() * sizeof(uint32_t)); cuda_check(cudaMemcpyAsync(I.ev_dh.p, h_dh.data(), h_dh.size() * sizeof(uint32_t), cudaMemcpyHostToDevice, I.stream), "H2D dh");
  I.ev_dopp_ok.ensure(h_ok.size()); cuda_check(cudaMemcpyAsync(I.ev_dopp_ok.p, h_ok.data(), h_ok.size(), cudaMemcpyHostToDevice, I.stream), "H2D ok");
  I.ev_inv_noise.ensure(h_invn.size() * sizeof(float)); cuda_check(cudaMemcpyAsync(I.ev_inv_noise.p, h_invn.data(), h_invn.size() * sizeof(float), cudaMemcpyHostToDevice, I.stream), "H2D invn");
  I.ev_los_found.ensure(h_los.size()); cuda_check(cudaMemcpyAsync(I.ev_los_found.p, h_los.data(), h_los.size(), cudaMemcpyHostToDevice, I.stream), "H2D los");

  I.E.ensure(nt * nv * sizeof(float));
  GeoParams gp{}; for (int k = 0; k < 3; ++k) gp.tx[k] = geo.tx[k]; for (uint32_t i = 0; i < kCh && i < 4; ++i) for (int k = 0; k < 3; ++k) gp.rx[i][k] = geo.rx[i][k];
  {
    dim3 t(256), b((unsigned)std::min<size_t>(65535, (nt * nv + 255) / 256));
    k_envelope<<<b, t, 0, I.stream>>>((zC*)I.rd.p, (float*)I.ev_inv_noise.p, (uint8_t*)I.ev_los_found.p, (uint32_t*)I.ev_tested_dopp.p, (uint32_t)nt,
                                      (uint8_t*)I.ev_dopp_ok.p, (uint32_t*)I.ev_dh.p, gp, g.origin.x, g.origin.y, g.origin.z, g.step,
                                      g.nx, g.ny, g.nz, I.cur_nrange, I.cur_ndopp, a.delay_step_s, kC, (float*)I.E.p);
  }
  std::vector<float> h_E(nt * nv);
  cuda_check(cudaMemcpyAsync(h_E.data(), I.E.p, h_E.size() * sizeof(float), cudaMemcpyDeviceToHost, I.stream), "D2H E");
  cuda_check(cudaStreamSynchronize(I.stream), "sync detect");
  I.timing.envelope_ms = I.lap();
  I.timing.envelope_download_ms = 0;  // included above; kept separate field for report symmetry only

  (void)topview_max;  // caller reads last_envelope() instead (see coherent_cuda.h header)
  I.last_E = h_E;
  return coherent::detect(h_E, R, g, geo, p);   // unmodified CPU detect() -- see coherent_cuda.h header
}

const std::vector<float>& CudaCoherent::last_envelope() const { return impl_->last_E; }

void CudaCoherent::refine(std::vector<Detection>& dets, const Grid& g, const Geometry& geo, const Calibration& cal, const SurveySigma& survey)
{
  const RdResult& R = impl_->cached;
  for (Detection& d : dets) coherent::refine(d, R, g, geo, cal, survey);   // unmodified CPU refine()
}

CudaCoherent::Timing CudaCoherent::last_timing() const { return impl_->timing; }

} // namespace nr_isac::coherent
