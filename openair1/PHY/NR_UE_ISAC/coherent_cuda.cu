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
#include "coherent_cuda_front.h"
#include "coherent_cuda_detect.h"

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
// (not exported there; see file header): coherent_core.cc's dopp_half()/dopp_ok().
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

// Per (channel i, subcarrier k): mean over rows observing k of the derotated value (LOS+row-sync
// delay/phase already applied, no static subtraction yet) -- coherent_core.cc range_doppler()'s
// `stat[k]`/`cnt[k]`.
__global__ void k_stat(const zC* values, const uint8_t* observed, const double* tau, const double* phase, const double* amp,
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
      const zC v0 = values[((size_t)i * rows + r) * sc + k]; const zC v{v0.x * amp[r], v0.y * amp[r]};
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
                          const double* phase, const double* amp, uint32_t rows, uint32_t sc, double scs, uint32_t nch, zC* row_los)
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
      const zC v0 = values[((size_t)i * rows + r) * sc + (uint32_t)k]; const zC v{v0.x * amp[r], v0.y * amp[r]};
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
                                 const double* phase, const double* amp, const zC* stat, uint32_t rows, uint32_t sc, uint32_t nfft,
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
    const zC v0 = values[((size_t)i * rows + r) * sc + k]; const zC v{v0.x * amp[r], v0.y * amp[r]};
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
struct GeoParams { double tx[3]; double rx[4][3]; };

// One thread per (tested Doppler index t, voxel v): per-channel sliding max of |RD|^2/noise over the
// voxel's own Doppler half-width, at the channel's own excess-delay bin -- coherent_core.cc's
// envelope(). A channel with an out-of-range bin (MagInterp::ok == false) contributes 0, same as the
// CPU `if (!mi.ok) continue;`; a non-ok Doppler bin contributes 0 to the max, same as CPU's
// `ok[d] ? mi.at(...) : 0.0` fed into sliding_max (0 can never win a max over non-negative power).
// Per CPI, once: |RD| as FP32 (the envelope loop below then never touches FP64 -- consumer GPUs run
// FP64 at 1/64 rate, and the FP64 envelope cost 83 ms p50 / 1.15 s p95 per CPI OTA at 64k voxels).
__global__ void k_mag(const zC* rd, size_t n, float* mag)
{
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < n; j += (size_t)blockDim.x * gridDim.x)
    mag[j] = (float)sqrt(rd[j].x * rd[j].x + rd[j].y * rd[j].y);
}
// Per (voxel, channel), once: the voxel's excess-delay range bin (FP64 geometry, stored FP32); -1 when
// the channel has no LOS or the bin is off the range axis (contributes 0, as the CPU envelope()).
__global__ void k_vbin(const uint8_t* los_found, GeoParams geo, double origin_x, double origin_y, double origin_z, double step,
                       uint32_t nx, uint32_t ny, uint32_t nz, uint32_t nrange, double delay_step_s, double c_mps, float* vbin)
{
  const size_t nv = (size_t)nx * ny * nz;
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < nv * 4; j += (size_t)blockDim.x * gridDim.x) {
    const size_t v = j / 4; const uint32_t ch = (uint32_t)(j % 4);
    const size_t ix = v % nx, iy = (v / nx) % ny, iz = v / ((size_t)nx * ny);
    const double xx = origin_x + ix * step, xy = origin_y + iy * step, xz = origin_z + iz * step;
    const double dtx = sqrt((xx - geo.tx[0]) * (xx - geo.tx[0]) + (xy - geo.tx[1]) * (xy - geo.tx[1]) + (xz - geo.tx[2]) * (xz - geo.tx[2]));
    const double drx = sqrt((xx - geo.rx[ch][0]) * (xx - geo.rx[ch][0]) + (xy - geo.rx[ch][1]) * (xy - geo.rx[ch][1]) + (xz - geo.rx[ch][2]) * (xz - geo.rx[ch][2]));
    const double dbase = sqrt((geo.tx[0] - geo.rx[ch][0]) * (geo.tx[0] - geo.rx[ch][0]) + (geo.tx[1] - geo.rx[ch][1]) * (geo.tx[1] - geo.rx[ch][1]) + (geo.tx[2] - geo.rx[ch][2]) * (geo.tx[2] - geo.rx[ch][2]));
    const double bin = (dtx + drx - dbase) / c_mps / delay_step_s;
    vbin[j] = (los_found[ch] && bin >= 0 && bin <= (double)(nrange - 1)) ? (float)bin : -1.f;
  }
}

// One BLOCK per voxel: build each channel's fractional-bin-interpolated, notch-masked magnitude
// column ONCE over the full Doppler axis (staged in shared memory), then every one of the ~nt tested
// Doppler indices scans its own +-dh(v) window out of that shared column -- coherent_core.cc's
// envelope() already amortises exactly this way (its sliding_max() builds `col[d]` once per channel
// before sliding a window over it). The prior kernel (one thread per (t,voxel)) redid the full 4-tap
// interpolation plus 4 global-memory loads for every tested Doppler index, up to 2*dh+1 times each --
// on the full-band OTA CPI (nt~146, dh up to ~60) that is >100x redundant global traffic and FLOPs
// per voxel per channel. `mag`'s layout is Doppler-contiguous per (channel, range bin), so the
// strided build loop below is coalesced. `dopp_ok_mask[dd] ? m : 0` is folded into `vals[]` at build
// time instead of re-tested per query. Squaring only the final window max (not every element) is the
// same simplification the CPU makes implicitly (max of squares of a non-negative sequence == square
// of its max).
__global__ void k_envelope(const float* mag, const float* inv_noise, const float* vbin, const uint32_t* tested_dopp,
                           uint32_t nt, const uint8_t* dopp_ok_mask, const uint32_t* dh, size_t nv,
                           uint32_t nrange, uint32_t ndopp, float* E)
{
  extern __shared__ float smem[];
  float* const vals = smem;          // [ndopp]: current channel's interpolated/masked Doppler column
  float* const eacc = smem + ndopp;  // [nt]: running cross-channel sum for this voxel

  for (size_t v = blockIdx.x; v < nv; v += gridDim.x) {
    for (uint32_t t = threadIdx.x; t < nt; t += blockDim.x) eacc[t] = 0.f;
    __syncthreads();
    const long h = (long)dh[v];
    for (uint32_t ch = 0; ch < 4; ++ch) {
      const float bin = vbin[v * 4 + ch];   // same value for every thread in the block: uniform branch
      if (bin < 0.f) continue;
      const long b0 = (long)bin; const float u = bin - (float)b0;
      const float cm = -u * (u - 1) * (u - 2) / 6, c0 = (u + 1) * (u - 1) * (u - 2) / 2, c1 = -(u + 1) * u * (u - 2) / 2, c2 = (u + 1) * u * (u - 1) / 6;
      auto row = [&](long b) { b = b < 0 ? 0 : (b > (long)nrange - 1 ? (long)nrange - 1 : b); return mag + ((size_t)ch * nrange + (size_t)b) * ndopp; };
      const float *rm = row(b0 - 1), *r0 = row(b0), *r1 = row(b0 + 1), *r2 = row(b0 + 2);
      for (uint32_t d = threadIdx.x; d < ndopp; d += blockDim.x)
        vals[d] = dopp_ok_mask[d] ? fmaxf(cm * rm[d] + c0 * r0[d] + c1 * r1[d] + c2 * r2[d], 0.f) : 0.f;
      __syncthreads();
      const float invn = inv_noise[ch];
      for (uint32_t t = threadIdx.x; t < nt; t += blockDim.x) {
        const long d0 = (long)tested_dopp[t];
        const long lo = d0 - h < 0 ? 0 : d0 - h, hi = d0 + h > (long)ndopp - 1 ? (long)ndopp - 1 : d0 + h;
        float best = 0.f;
        for (long dd = lo; dd <= hi; ++dd) best = fmaxf(best, vals[dd] * vals[dd]);
        eacc[t] += best * invn;
      }
      __syncthreads();   // every read of vals[] for this channel must finish before the next one overwrites it
    }
    for (uint32_t t = threadIdx.x; t < nt; t += blockDim.x) E[(size_t)t * nv + v] = eacc[t];
    __syncthreads();     // all E writes done before the grid-stride loop reuses eacc[]/vals[] for the next voxel
  }
}

} // namespace

namespace {
__global__ void k_scale_rd(zC* rd, const float* g, uint32_t nrange, uint32_t ndopp, size_t n)
{
  for (size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x; j < n; j += (size_t)blockDim.x * gridDim.x) {
    const float f = g[j / ndopp];            // [ch][range] factor; rd is [ch][range][dopp]
    rd[j].x *= f; rd[j].y *= f;
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
  Buf whiten, ramp, wf_off, wf_hw, wf_st, wf_B, wf_B2, mag, vbin, values, values_f, observed, span, tau, phase, stat, wsum_row, row_los, spectrum, prof_near, prof_far, ed, rd, far_rd, E;
  Buf ev_tested_dopp, ev_dh, ev_dopp_ok, ev_inv_noise, ev_los_found;  // detect()'s own small persistent uploads

  // host-side state kept across range_doppler() -> detect() within one CPI
  RdResult cached;
  std::unique_ptr<CudaFront> front;   // device-resident CPI shared by find_los / row sync / range_doppler
  uint32_t cur_nch = kCh, cur_rows = 0, cur_nrange = 0, cur_ndopp = 0;
  size_t ev_smem_raised = 0;   // largest k_envelope dynamic-shared-mem opt-in granted so far (see detect())
  Timing timing;
  std::vector<float> last_E;
  // Grow-only, contents never read: GpuDetect::run() size-checks its `E` argument even when it takes
  // the device envelope directly (d_E != null) and never touches `E`'s contents in that case -- see
  // detect() below. Passing this instead of downloading avoids the D2H on every CPI that doesn't need
  // a host copy, without editing coherent_cuda_detect.cu (owned by another agent).
  std::vector<float> ev_dummy_E;
  GpuDetect det;   // GPU-resident detect() (coherent_cuda_detect.cu); opt-in via NR_ISAC_CUDA_DETECT_GPU=1 (CPU oracle default)

  Impl()
  {
    cuda_check(cudaStreamCreate(&stream), "cudaStreamCreate");
    cuda_check(cudaEventCreate(&ev0), "cudaEventCreate");
    cuda_check(cudaEventCreate(&ev1), "cudaEventCreate");
    front = std::make_unique<CudaFront>(stream);
  }
  ~Impl()
  {
    front.reset();
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

void CudaCoherent::upload(const CfrWindow& w) { impl_->front->upload(w); }
LosEstimate CudaCoherent::find_los(const CfrWindow& w, const Axes& a, double pfa, const std::array<double, kCh>* geo_los_s)
{
  CudaFront& F = *impl_->front;
  if (!F.bound(w)) F.upload(w);
  return coherent::find_los(w, a, pfa, geo_los_s, &F);
}
RowSync CudaCoherent::estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& L)
{
  CudaFront& F = *impl_->front;
  if (!F.bound(w)) F.upload(w);
  return coherent::estimate_row_sync(w, a, L, &F);
}

RdResult CudaCoherent::range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s, bool download_rd)
{
  Impl& I = *impl_;
  RdResult out; out.rd.axes = a; out.los_found = L.found;
  if (!a.valid) { I.cached = out; return out; }
  const uint32_t rows = w.rows, sc = w.subcarriers, nfft = a.n_fft, nrange = a.n_range, ndopp = a.n_dopp;
  const uint32_t nch = kCh;

  // The CPI and its row metadata (spans, combs, Hann sums) are device-resident in the front
  // (coherent_cuda_front.cu), uploaded once per CPI; only the per-row delay/phase goes up here.
  CudaFront& F = *I.front;
  const auto t_up0 = std::chrono::steady_clock::now();
  if (!F.bound(w)) F.upload(w);
  const CudaFront::Meta& M = F.meta();
  std::vector<double> h_tau((size_t)nch * rows), h_phase(rows), h_amp(rows, 1.0);
  uint32_t cmax = 1;
  for (uint32_t r = 0; r < rows; ++r) {
    cmax = std::max(cmax, M.comb[r]);
    h_phase[r] = s.phase_rad.empty() ? 0.0 : s.phase_rad[r];
    if (!s.amp.empty()) h_amp[r] = s.amp[r];
    for (uint32_t i = 0; i < nch; ++i) h_tau[(size_t)i * rows + r] = L.delay_s[i] + (s.delay_s.empty() ? 0.0 : s.delay_s[r]);
  }
  const long rep = (long)nfft / cmax, far0 = rep / 2 - (long)nrange / 2;
  const bool far_ok = far0 >= 2 * (long)nrange && far0 + 2 * (long)nrange <= rep;

  // Doppler phasor table ed[d][r] = win_r/wsum_total * e^{-j2pi f_d t_r}, built on the device.
  const zC* d_ed = F.ed(w, a);
  const zC* d_values = F.d_values(); const uint8_t* d_obs = F.d_observed(); const int2* d_span = F.d_span(); const float* d_wsum = F.d_wsum_f();

  cuda_check(cudaEventRecord(I.ev0, I.stream), "cudaEventRecord");

  // upload (per-row delay/phase only)
  I.tau.ensure((size_t)nch * rows * sizeof(double)); cuda_check(cudaMemcpyAsync(I.tau.p, h_tau.data(), h_tau.size() * sizeof(double), cudaMemcpyHostToDevice, I.stream), "H2D tau");
  I.phase.ensure(rows * sizeof(double)); cuda_check(cudaMemcpyAsync(I.phase.p, h_phase.data(), rows * sizeof(double), cudaMemcpyHostToDevice, I.stream), "H2D phase");
  I.ramp.ensure(rows * sizeof(double)); cuda_check(cudaMemcpyAsync(I.ramp.p, h_amp.data(), rows * sizeof(double), cudaMemcpyHostToDevice, I.stream), "H2D amp");
  I.lap();
  I.timing.upload_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_up0).count();

  // stat[i][k], row_los[i][r]
  I.stat.ensure((size_t)nch * sc * sizeof(zC)); I.row_los.ensure((size_t)nch * rows * sizeof(zC));
  auto launch = [](size_t n) { dim3 t(256), b((unsigned)std::min<size_t>(65535, (n + 255) / 256)); return std::make_pair(b, t); };
  {
    auto [b, t] = launch((size_t)nch * sc);
    k_stat<<<b, t, 0, I.stream>>>(d_values, d_obs, (double*)I.tau.p, (double*)I.phase.p, (double*)I.ramp.p, rows, sc, w.scs_hz, nch, (zC*)I.stat.p);
  }
  {
    k_los_row<<<(unsigned)(nch * rows), 256, 0, I.stream>>>(d_values, d_obs, d_span, (double*)I.tau.p, (double*)I.phase.p, (double*)I.ramp.p, rows, sc, w.scs_hz, nch, (zC*)I.row_los.p);
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
    k_build_spectrum<<<b, t, 0, I.stream>>>(d_values, d_obs, d_span, (double*)I.tau.p, (double*)I.phase.p, (double*)I.ramp.p,
                                            (zC*)I.stat.p, rows, sc, nfft, w.scs_hz, nch, (zC*)I.spectrum.p);
  }
  cufft_check(cufftExecZ2Z(I.plan, (zC*)I.spectrum.p, (zC*)I.spectrum.p, CUFFT_INVERSE), "cufftExecZ2Z");
  I.timing.fft_ms = I.lap();

  // crop + normalise
  I.prof_near.ensure((size_t)nch * rows * nrange * sizeof(zC));
  if (far_ok) I.prof_far.ensure((size_t)nch * rows * nrange * sizeof(zC));
  {
    auto [b, t] = launch((size_t)nch * rows * nrange);
    k_crop_norm<<<b, t, 0, I.stream>>>((zC*)I.spectrum.p, d_wsum, rows, nfft, nrange, far0, far_ok ? 1 : 0, nch,
                                       (zC*)I.prof_near.p, far_ok ? (zC*)I.prof_far.p : nullptr);
  }
  I.timing.crop_norm_ms = I.lap();

  // NUDFT: near -> resident RD cube; far (if any) -> noise estimate
  I.rd.ensure((size_t)nch * nrange * ndopp * sizeof(zC));
  {
    auto [b, t] = launch((size_t)nch * nrange * ndopp);
    k_nudft<<<b, t, 0, I.stream>>>((zC*)I.prof_near.p, d_ed, rows, nrange, ndopp, nch, (zC*)I.rd.p);
  }
  if (far_ok) {
    I.far_rd.ensure((size_t)nch * nrange * ndopp * sizeof(zC));
    auto [b, t] = launch((size_t)nch * nrange * ndopp);
    k_nudft<<<b, t, 0, I.stream>>>((zC*)I.prof_far.p, d_ed, rows, nrange, ndopp, nch, (zC*)I.far_rd.p);
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
      if (M.grp[r] < 0 || !(M.h1[r] > 0)) continue;
      const zC v = h_row_los[(size_t)i * rows + r];
      los += cd(v.x, v.y) / M.h1[r]; ++los_rows;
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
  out.wf = F.waveform(w, a);                        // B/B2 and the static-removal operator Q on the device
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
    const size_t nm = (size_t)I.cur_nch * I.cur_nrange * I.cur_ndopp;
    I.mag.ensure(nm * sizeof(float)); I.vbin.ensure(nv * 4 * sizeof(float));
    k_mag<<<(unsigned)std::min<size_t>(65535, (nm + 255) / 256), 256, 0, I.stream>>>((zC*)I.rd.p, nm, (float*)I.mag.p);
    k_vbin<<<(unsigned)std::min<size_t>(65535, (nv * 4 + 255) / 256), 256, 0, I.stream>>>((uint8_t*)I.ev_los_found.p, gp, g.origin.x, g.origin.y, g.origin.z, g.step,
                                                                                     g.nx, g.ny, g.nz, I.cur_nrange, a.delay_step_s, kC, (float*)I.vbin.p);
    // One block per voxel (see k_envelope); shared mem = this CPI's own Doppler-axis size + tested-bin
    // count (derived, not tuned). Opt in to a bigger-than-default dynamic shared mem block ONLY if this
    // CPI's own axes need it (RTX 4060 Ti / Ada supports up to 100 KB opt-in vs the 48 KB static default).
    const int ev_threads = 128;
    const unsigned ev_blocks = (unsigned)std::min<size_t>(65535, std::max<size_t>(1, nv));
    const size_t ev_smem = ((size_t)I.cur_ndopp + nt) * sizeof(float);
    if (ev_smem > 48 * 1024 && ev_smem > I.ev_smem_raised) {
      int dev = 0, max_optin = 0;
      cuda_check(cudaGetDevice(&dev), "cudaGetDevice");
      cuda_check(cudaDeviceGetAttribute(&max_optin, cudaDevAttrMaxSharedMemoryPerBlockOptin, dev), "cudaDeviceGetAttribute");
      if ((size_t)max_optin < ev_smem) throw std::runtime_error("k_envelope: CPI Doppler axis exceeds device shared-mem opt-in limit");
      cuda_check(cudaFuncSetAttribute(k_envelope, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)ev_smem), "cudaFuncSetAttribute smem");
      I.ev_smem_raised = ev_smem;
    }
    k_envelope<<<ev_blocks, ev_threads, ev_smem, I.stream>>>((float*)I.mag.p, (float*)I.ev_inv_noise.p, (float*)I.vbin.p, (uint32_t*)I.ev_tested_dopp.p, (uint32_t)nt,
                                      (uint8_t*)I.ev_dopp_ok.p, (uint32_t*)I.ev_dh.p, nv, I.cur_nrange, I.cur_ndopp, (float*)I.E.p);
  }
  // Host-side E is needed for the CPU-oracle path (currently the default -- see below) and for the
  // caller's periodic topview image (coherent_pipeline.cc passes a non-null `topview_max` exactly on
  // the CPI that image is due); GpuDetect::run() below takes the DEVICE envelope pointer directly and
  // never reads its `E` argument when one is supplied, so skipping this D2H when neither applies
  // changes nothing it consumes.
  const bool use_gpu_detect = std::getenv("NR_ISAC_CUDA_DETECT_GPU") != nullptr;
  const bool need_host_E = !use_gpu_detect || topview_max != nullptr;
  std::vector<float> h_E;
  if (need_host_E) {
    h_E.resize(nt * nv);
    cuda_check(cudaMemcpyAsync(h_E.data(), I.E.p, h_E.size() * sizeof(float), cudaMemcpyDeviceToHost, I.stream), "D2H E");
  } else if (I.ev_dummy_E.size() != nt * nv) {
    I.ev_dummy_E.resize(nt * nv);   // grow-only in steady state (see Impl::ev_dummy_E); contents unused
  }
  cuda_check(cudaStreamSynchronize(I.stream), "sync detect");
  I.timing.envelope_ms = I.lap();
  I.timing.envelope_download_ms = 0;  // included above; kept separate field for report symmetry only

  if (need_host_E) I.last_E = h_E;    // last_envelope(): topview builder (or the CPU-oracle path)
  // GpuDetect is opt-in until it matches the CPU oracle on real data: on 40 detect() inputs dumped from
  // the full-band OTA recording it made different decisions on 12 (FP32 magnitudes flip near-tied
  // pursuit choices; the clean parity scene has no ties). The user's rule is no accuracy loss.
  // (Inherited from c32469c8b5, unrelated to this file's envelope-kernel speed-up -- kept verbatim.)
  if (!use_gpu_detect) return coherent::detect(h_E, R, g, geo, p);   // CPU oracle (default)
  return I.det.run(need_host_E ? h_E : I.ev_dummy_E, R, g, geo, p, I.rd.p, (const float*)I.E.p, I.stream);
}

void CudaCoherent::scale_rd(const std::vector<float>& g)
{
  Impl& I = *impl_;
  if (g.size() != (size_t)I.cur_nch * I.cur_nrange || I.cur_ndopp == 0) return;
  I.whiten.ensure(g.size() * sizeof(float));
  cuda_check(cudaMemcpyAsync(I.whiten.p, g.data(), g.size() * sizeof(float), cudaMemcpyHostToDevice, I.stream), "H2D whiten");
  const size_t n = (size_t)I.cur_nch * I.cur_nrange * I.cur_ndopp;
  k_scale_rd<<<(unsigned)std::min<size_t>(65535, (n + 255) / 256), 256, 0, I.stream>>>((zC*)I.rd.p, (const float*)I.whiten.p, I.cur_nrange, I.cur_ndopp, n);
  cuda_check(cudaStreamSynchronize(I.stream), "sync whiten");
  for (size_t j = 0; j < I.cached.rd.v.size(); ++j) I.cached.rd.v[j] *= g[j / I.cur_ndopp];   // keep detect()'s host copy in step
}

const std::vector<float>& CudaCoherent::last_envelope() const { return impl_->last_E; }

void CudaCoherent::refine(std::vector<Detection>& dets, const Grid& g, const Geometry& geo, const Calibration& cal, const SurveySigma& survey)
{
  const RdResult& R = impl_->cached;
  for (Detection& d : dets) coherent::refine(d, R, g, geo, cal, survey);   // unmodified CPU refine()
}

CudaCoherent::Timing CudaCoherent::last_timing() const
{
  Timing t = impl_->timing;
  const CudaFront::Timing& f = impl_->front->timing();
  t.f_upload_ms = f.upload; t.f_kernel_ms = f.kernel; t.f_noncoh_ms = f.noncoh; t.f_union_ms = f.union_k; t.f_coh_ms = f.coh; t.f_refine_ms = f.refine;
  t.f_rowsums_ms = f.row_sums; t.f_ed_ms = f.ed; t.f_wf_ms = f.waveform; t.f_rowsums_calls = f.row_sums_calls;
  return t;
}

} // namespace nr_isac::coherent
