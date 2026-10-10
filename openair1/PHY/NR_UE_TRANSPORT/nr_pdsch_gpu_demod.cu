/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU-DEMOD module side (part of libpdsch_gpu.so): bit-exact nr_rx_pdsch() demodulation of one passive PDSCH grant,
 * all symbols in one launch set. See nr_pdsch_gpu_demod.h. The integer arithmetic is nr_pdsch_gpu_demod.cuh (the
 * same functions the host test checks against the CPU). The ISAC_RX_MRC_ALIGN rotation is double precision with
 * the operations, operand order and fused multiply-adds GCC 13 -O2 (aarch64, -ffp-contract=fast) emits for the CPU
 * source (read from the disassembly of nr_dlsch_demodulation.c.o); this file is compiled with --fmad=false so nvcc
 * fuses nothing on its own. */
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_gpu_demod.cuh"
#include <cuda_runtime.h>
#include "PHY/TOOLS/nr_gpu_budget_cuda.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace nrdm {
constexpr int CTX = 8;
constexpr int NSYM = NR_GPU_DEMOD_NSYM;
constexpr int EXT = NR_GPU_DEMOD_MAX_EXT;
constexpr int MAXCH = (EXT + 47) / 48; /* ALIGN=2 chunks per symbol */

struct Shared { /* mapped: written by the GPU, read by the host after the stream completes */
  nrd_decision d;
  int32_t avg[16], median[16];
  int64_t pw[NSYM][4];
  int ref[NSYM];
  long long kap_sz[NSYM][4], kap_sm[NSYM][4];
  nr_gpu_evm_sym_t evm[NSYM];
};
struct Ctx {
  cudaStream_t s;
  nrd_c16 *y, *h; /* device memory, [14][4][EXT] and [14][16][EXT] */
  double2 *cs;       /* ALIGN: per (symbol, branch, chunk) rotation, nan = none */
  double2 *tm;       /* ALIGN: per (symbol, branch, RE) correlation terms */
  double2 *xs, *sd; /* EVM: per-RE zero-forcing solution and decisions [14][2][EXT] */
  double *part, *sum; /* EVM block partials [14][EVM_NCH][EVM_P] and per-symbol sums [14][EVM_P] */
  nrd_c16 *hy, *hh;   /* pinned host copies of y/h for the CPU EVM fallback (ISAC_PDSCH_GPU_EXACT_DEMOD_EVM=0) */
  cudaEvent_t ev[13];
  cudaStream_t s2;           /* EVM chain, concurrent with compensation/MMSE/LLR */
  cudaEvent_t fork, join; /* ISAC_PDSCH_GPU_EXACT_DEMOD_PROF=1: per-kernel GPU time */
  Shared *sh;  /* pinned host copy, filled by one DMA at the end */
  Shared *dsh; /* device copy the kernels use (host-pinned memory is uncached for the GPU) */
};
static Ctx g_ctx[CTX];
static std::atomic<unsigned> g_busy{0};
static int g_ok, g_prof;
static std::atomic<unsigned long long> g_prof_ns[13], g_prof_n;

static int acquire()
{
  for (;;) {
    unsigned m = g_busy.load();
    for (int i = 0; i < CTX; i++)
      if (!(m >> i & 1u) && g_busy.compare_exchange_strong(m, m | 1u << i))
        return i;
    std::this_thread::yield();
  }
}

/* ---- gather: nr_dlsch_extract_rbs() with the CPU-made index map ------------------------------------------- */
__global__ void k_gather(const nr_gpu_demod_job_t j, nrd_c16 *y, nrd_c16 *h)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const nrd_c16 *gy = (const nrd_c16 *)j.geo_y + (size_t)m * j.ext;
  const nrd_c16 *gh = (const nrd_c16 *)j.geo_h + (size_t)m * j.ext;
  const nrd_c16 *rx = (const nrd_c16 *)j.rxdataF;
  const nrd_c16 *est = (const nrd_c16 *)j.est;
  const int nre = j.nre[m];
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < j.ext; i += gridDim.x * blockDim.x) {
    const bool in = (int)i < nre;
    const nrd_c16 py = in ? gy[i] : nrd_c16{0, 0}, ph = in ? gh[i] : nrd_c16{0, 1};
    for (int a = 0; a < j.nbRx; a++) {
      nrd_c16 v = {0, 0};
      if (in && py.i)
        v = rx[(size_t)a * j.rx_stride + (size_t)m * j.N + (uint16_t)py.r];
      y[((size_t)m * NR_GPU_DEMOD_MAX_RX + a) * EXT + i] = v;
    }
    for (int r = 0; r < j.nl * j.nbRx; r++) {
      nrd_c16 v = {0, 0};
      if (in)
        v = est[(size_t)r * j.est_stride + (size_t)(ph.i - 1) * j.N + (uint16_t)ph.r];
      h[((size_t)m * 16 + r) * EXT + i] = v;
    }
  }
}

/* ---- channel level of the first data symbol, median (nl > 1), branch decision, output shift ---------------- */
__global__ void k_level(const nr_gpu_demod_job_t j, const nrd_c16 *h, Shared *sh)
{
  __shared__ uint32_t lane[256][4];
  __shared__ long long mx[256], mn[256];
  const int f = j.first_symbol, t = threadIdx.x;
  const uint32_t len = j.nre[f];
  const int x = nrd_factor2(len);
  const int rows = j.nl * j.nbRx;
  for (int r = blockIdx.x; r < rows; r += gridDim.x) {
    const nrd_c16 *row = h + ((size_t)f * 16 + r) * EXT;
    uint32_t l4[4] = {0, 0, 0, 0};
    for (uint32_t g = t; g < (len >> 2); g += blockDim.x)
      for (int k = 0; k < 4; k++)
        l4[k] += nrd_level_term(row[4 * g + k], x);
    for (int k = 0; k < 4; k++)
      lane[t][k] = l4[k];
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
      if (t < s)
        for (int k = 0; k < 4; k++)
          lane[t][k] += lane[t + s][k];
      __syncthreads();
    }
    if (t == 0)
      sh->avg[r] = nrd_level_finish(lane[0], len);
    __syncthreads();
    if (j.nl > 1) {
      long long a = sh->avg[r], b = sh->avg[r];
      for (uint32_t g = t; g < (len >> 2); g += blockDim.x) {
        const long long v = nrd_median_term(row + 4 * g);
        a = v > a ? v : a;
        b = v < b ? v : b;
      }
      mx[t] = a;
      mn[t] = b;
      __syncthreads();
      for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (t < s) {
          mx[t] = mx[t + s] > mx[t] ? mx[t + s] : mx[t];
          mn[t] = mn[t + s] < mn[t] ? mn[t + s] : mn[t];
        }
        __syncthreads();
      }
      if (t == 0)
        sh->median[r] = (int32_t)((mx[0] + mn[0]) >> 1);
      __syncthreads();
    }
  }
}
__global__ void k_decide(const nr_gpu_demod_job_t j, Shared *sh)
{
  nrd_decide(j.nl, j.nbRx, j.mode, j.thr_coef, j.l2_delta, sh->avg, sh->median, &sh->d);
}

/* ---- mode-3 branch silencing (every symbol), then the per-symbol branch powers and reference of the alignment */
__global__ void k_mask_pw(const nr_gpu_demod_job_t j, nrd_c16 *h, Shared *sh)
{
  __shared__ long long red[256];
  const int m = blockIdx.x, t = threadIdx.x;
  if (!(j.sym_mask >> m & 1))
    return;
  const nrd_decision d = sh->d;
  if (j.nl == 1 && d.zero_mask)
    for (int a = 0; a < j.nbRx; a++)
      if (d.zero_mask >> a & 1)
        for (uint32_t i = t; i < j.ext; i += blockDim.x)
          h[((size_t)m * 16 + a) * EXT + i] = nrd_c16{0, 0};
  const int nre = j.nre[m];
  if (!(j.nl == 1 && d.mrc_nb_rx > 1 && nre >= 12 && j.align)) {
    if (t == 0)
      sh->ref[m] = -1;
    return;
  }
  __syncthreads();
  for (int a = d.mrc_rx_index; a < d.mrc_rx_index + d.mrc_nb_rx; a++) {
    const nrd_c16 *row = h + ((size_t)m * 16 + a) * EXT;
    long long s = 0;
    for (int i = t; i < nre; i += blockDim.x)
      s += (long long)row[i].r * row[i].r + (long long)row[i].i * row[i].i;
    red[t] = s;
    __syncthreads();
    for (int k = blockDim.x / 2; k > 0; k >>= 1) {
      if (t < k)
        red[t] += red[t + k];
      __syncthreads();
    }
    if (t == 0)
      sh->pw[m][a] = red[0];
    __syncthreads();
  }
  if (t == 0) {
    int ref = -1;
    for (int a = d.mrc_rx_index; a < d.mrc_rx_index + d.mrc_nb_rx; a++)
      if (sh->pw[m][a] > 0 && (ref < 0 || sh->pw[m][a] > sh->pw[m][ref]))
        ref = a;
    sh->ref[m] = ref;
  }
}

/* ---- ISAC_RX_MRC_ALIGN, phase 1: one thread per (symbol, chunk, branch) forms the chunk's cross-correlation sequentially in
 * RE order with GCC's FMA pattern (exactly the CPU's sums) and the rotation (cos, sin); phase 2 rotates every RE. --------- */
__global__ void k_align_terms(const nr_gpu_demod_job_t j, const nrd_c16 *y, const nrd_c16 *h, const Shared *sh, double2 *tm)
{
  /* the per-RE terms of the CPU's cr/ci sums, one thread per (RE, branch): t_r = fma(uar, urr, uai*uri), t_i = fma(uai, urr, -(uar*uri)) */
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const int ref = sh->ref[m];
  if (ref < 0)
    return;
  const nrd_decision d = sh->d;
  const uint32_t nre = j.nre[m];
  const nrd_c16 *hr_row = h + ((size_t)m * 16 + ref) * EXT;
  const nrd_c16 *yr_row = y + ((size_t)m * NR_GPU_DEMOD_MAX_RX + ref) * EXT;
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < nre; i += gridDim.x * blockDim.x) {
    const nrd_c16 hr = hr_row[i], yr = yr_row[i];
    const double urr = (double)hr.r * yr.r + (double)hr.i * yr.i, uri = (double)hr.r * yr.i - (double)hr.i * yr.r;
    for (int w = 0; w < d.mrc_nb_rx && w < 4; w++) {
      const int a = d.mrc_rx_index + w;
      if (a == ref)
        continue;
      const nrd_c16 ha = h[((size_t)m * 16 + a) * EXT + i], ya = y[((size_t)m * NR_GPU_DEMOD_MAX_RX + a) * EXT + i];
      /* products of int16 are exact in double, and so are these sums (|.| < 2^31) */
      const double uar = (double)ha.r * ya.r + (double)ha.i * ya.i, uai = (double)ha.r * ya.i - (double)ha.i * ya.r;
      tm[((size_t)m * 4 + w) * EXT + i] = make_double2(__fma_rn(uar, urr, __dmul_rn(uai, uri)),   /* fmadd d, uar, urr, (uai*uri) */
                                                       __fma_rn(uai, urr, -__dmul_rn(uar, uri))); /* fnmsub d, uai, urr, (uar*uri) */
    }
  }
}
__global__ void k_align_cs(const nr_gpu_demod_job_t j, const Shared *sh, const double2 *tm, double2 *cs)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const int ref = sh->ref[m];
  if (ref < 0)
    return;
  const nrd_decision d = sh->d;
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  const int a = d.mrc_rx_index + (idx & 3), c = idx >> 2;
  if (c >= MAXCH)
    return;
  double2 *out = cs + ((size_t)m * 4 + (idx & 3)) * MAXCH + c;
  *out = make_double2(nan(""), 0.0); /* nan = leave this chunk alone */
  if ((idx & 3) >= d.mrc_nb_rx || a == ref || sh->pw[m][a] <= 0)
    return;
  const uint32_t nre = j.nre[m];
  const uint32_t chunk = j.align >= 2 ? 48u : nre;
  const uint32_t c0 = (uint32_t)c * chunk;
  if (c0 >= nre)
    return;
  const uint32_t c1 = c0 + chunk < nre ? c0 + chunk : nre;
  const double2 *T = tm + ((size_t)m * 4 + (idx & 3)) * EXT;
  double cr = 0.0, ci = 0.0;
  for (uint32_t i = c0; i < c1; i++) { /* the CPU's sequential fadd, RE order */
    const double2 q = T[i];
    cr = __dadd_rn(cr, q.x);
    ci = __dadd_rn(ci, q.y);
  }
  const double mag = __dsqrt_rn(__fma_rn(cr, cr, __dmul_rn(ci, ci))); /* fmul ci*ci; fmadd cr,cr; fsqrt */
  if (mag > 0.0)
    *out = make_double2(__ddiv_rn(cr, mag), __ddiv_rn(ci, mag));
}
__global__ void k_align_rot(const nr_gpu_demod_job_t j, nrd_c16 *h, const Shared *sh, const double2 *cs)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1) || sh->ref[m] < 0)
    return;
  const nrd_decision d = sh->d;
  const uint32_t nre = j.nre[m];
  const uint32_t chunk = j.align >= 2 ? 48u : nre;
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < j.ext; i += gridDim.x * blockDim.x) {
    /* ALIGN=1 (one chunk) rotates the whole row; ALIGN=2 the chunk's REs only */
    const uint32_t c = chunk == nre ? 0 : i / chunk;
    if (chunk != nre && i >= nre)
      continue;
    for (int w = 0; w < d.mrc_nb_rx && w < 4; w++) {
      const double2 q = cs[((size_t)m * 4 + w) * MAXCH + c];
      if (isnan(q.x))
        continue;
      nrd_c16 *row = h + ((size_t)m * 16 + d.mrc_rx_index + w) * EXT;
      const nrd_c16 v = row[i];
      const double hr2 = __fma_rn((double)v.r, q.x, -__dmul_rn((double)v.i, q.y)); /* fnmsub d, h.r, c, (h.i*sn) */
      const double hi2 = __fma_rn((double)v.r, q.y, __dmul_rn((double)v.i, q.x));  /* fmadd d, h.r, sn, (h.i*c) */
      row[i] = nrd_c16{(int16_t)llround(hr2), (int16_t)llround(hi2)};          /* lround, then (int16_t) wraps */
    }
  }
}

/* ---- nr_channel_compensation() over the whole extraction row, per layer ------------------------------------ */
__global__ void k_comp(const nr_gpu_demod_job_t j, const nrd_c16 *y, const nrd_c16 *h, const Shared *sh)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const nrd_decision d = sh->d;
  const int shift = d.log2_maxh;
  const nrd_c16 *rxF = y + ((size_t)m * NR_GPU_DEMOD_MAX_RX + d.mrc_rx_index) * EXT;
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < j.ext; i += gridDim.x * blockDim.x)
    for (int l = 0; l < j.nl; l++) {
      const nrd_c16 *chF = h + ((size_t)m * 16 + l * j.nbRx + d.mrc_rx_index) * EXT;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf;
      nrd_comp_re(d.mrc_nb_rx, EXT, i, j.qm, shift, rxF, chF, (nrd_c16 *)j.comp + row, (nrd_c16 *)j.mag + row,
                  (nrd_c16 *)j.magb + row, (nrd_c16 *)j.magr + row);
      if (j.wide_mrc && j.nl == 1 && j.qm == 2 && i < j.nre[m]) {
        /* nr_rx_pdsch(): the CRC-validated recovery's full-precision matched-filter sum, one output clamp */
        long long re = 0, im = 0;
        for (int a = 0; a < d.mrc_nb_rx; a++) {
          const nrd_c16 hh = chF[(size_t)a * EXT + i], yy = rxF[(size_t)a * EXT + i];
          re += (long long)hh.r * yy.r + (long long)hh.i * yy.i;
          im += (long long)hh.r * yy.i - (long long)hh.i * yy.r;
        }
        re >>= shift;
        im >>= shift;
        ((nrd_c16 *)j.comp)[row + i] = nrd_c16{(int16_t)(re > 32767 ? 32767 : re < -32768 ? -32768 : re),
                                               (int16_t)(im > 32767 ? 32767 : im < -32768 ? -32768 : im)};
      }
    }
}

/* ---- per-symbol, per-layer kappa sums (EVM armed, QAM): sum |Re z| + |Im z| and sum 2 mag (exact integers) ---- */
__global__ void k_kappa(const nr_gpu_demod_job_t j, Shared *sh)
{
  __shared__ long long rz[256], rm[256];
  const int m = blockIdx.x, t = threadIdx.x;
  if (!(j.sym_mask >> m & 1))
    return;
  for (int l = 0; l < j.nl; l++) {
    const nrd_c16 *z = (const nrd_c16 *)j.comp + ((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf;
    const nrd_c16 *ma = (const nrd_c16 *)j.mag + ((size_t)m * NR_GPU_DEMOD_MAX_NL + (j.mag_per_layer ? l : 0)) * j.buf;
    long long a = 0, b = 0;
    for (int i = t; i < j.nre[m]; i += blockDim.x) {
      a += (z[i].r < 0 ? -(int)z[i].r : z[i].r) + (z[i].i < 0 ? -(int)z[i].i : z[i].i);
      b += 2 * (long long)ma[i].r;
    }
    rz[t] = a;
    rm[t] = b;
    __syncthreads();
    for (int k = blockDim.x / 2; k > 0; k >>= 1) {
      if (t < k) {
        rz[t] += rz[t + k];
        rm[t] += rm[t + k];
      }
      __syncthreads();
    }
    if (t == 0) {
      sh->kap_sz[m][l] = rz[0];
      sh->kap_sm[m][l] = rm[0];
    }
    __syncthreads();
  }
}

/* ---- QAM-FIX ISAC_PDSCH_MMSE_MODE=2 (nr_dlsch_mmse, Nl = 2): per RE, G = sum_a sat(conj(h_r) h_c >> shift) in
 * int16 (mult_cpx_conj_vector + nr_a_sum_b), then the double Gauss-Jordan [G | H^H y | I] of the CPU source.
 * Complex products follow GCC's lowering (re = fma(L.r, R.r, -(L.i R.i)), im = fma(L.r, R.i, (L.i R.r)));
 * division is libgcc 13's __divdc3 (ported from its aarch64 object code); cabs of integer components is the
 * correctly rounded sqrt of the exact sum (glibc's cabs agrees on 2e7 random integer pairs), else hypot. */
struct dcx {
  double r, i;
};
__device__ __forceinline__ dcx cmul(dcx L, dcx R)
{
  return dcx{__fma_rn(L.r, R.r, -__dmul_rn(L.i, R.i)), __fma_rn(L.r, R.i, __dmul_rn(L.i, R.r))};
}
__device__ __forceinline__ double cabs_c(dcx x)
{
  const double lim = 67108864.0; /* 2^26: the squares and their sum are exact */
  if (x.r == trunc(x.r) && x.i == trunc(x.i) && fabs(x.r) < lim && fabs(x.i) < lim)
    return __dsqrt_rn(__dadd_rn(__dmul_rn(x.r, x.r), __dmul_rn(x.i, x.i)));
  return hypot(x.r, x.i);
}
__device__ dcx divdc3(double a, double b, double c, double d)
{
  const double RBIG = __longlong_as_double(0x7fdfffffffffffffLL), RMIN2 = __longlong_as_double(0x3cb0000000000000LL);
  const double RMIN = __longlong_as_double(0x0010000000000000LL), RMAX2 = __longlong_as_double(0x7c9fffffffffffffLL);
  const double SC = __longlong_as_double(0x4330000000000000LL);
  double x, y;
  if (fabs(c) < fabs(d)) {
    if (fabs(d) >= RBIG) {
      a = __dmul_rn(a, 0.5); b = __dmul_rn(b, 0.5); c = __dmul_rn(c, 0.5); d = __dmul_rn(d, 0.5);
    }
    if (fabs(d) < RMIN2 || (fabs(a) < RMIN && fabs(b) < RMAX2 && fabs(d) < RMAX2) || (fabs(b) < RMIN && fabs(a) < RMAX2 && fabs(d) < RMAX2)) {
      a = __dmul_rn(a, SC); b = __dmul_rn(b, SC); c = __dmul_rn(c, SC); d = __dmul_rn(d, SC);
    }
    const double ratio = __ddiv_rn(c, d), denom = __fma_rn(c, ratio, d);
    if (fabs(ratio) > RMIN) {
      x = __ddiv_rn(__fma_rn(a, ratio, b), denom);
      y = __ddiv_rn(__fma_rn(b, ratio, -a), denom);
    } else {
      x = __ddiv_rn(__fma_rn(__ddiv_rn(a, d), c, b), denom);
      y = __ddiv_rn(__fma_rn(__ddiv_rn(b, d), c, -a), denom);
    }
  } else {
    if (fabs(c) >= RBIG) {
      a = __dmul_rn(a, 0.5); b = __dmul_rn(b, 0.5); c = __dmul_rn(c, 0.5); d = __dmul_rn(d, 0.5);
    }
    if (fabs(c) < RMIN2 || (fabs(a) < RMIN && fabs(b) < RMAX2 && fabs(c) < RMAX2) || (fabs(b) < RMIN && fabs(a) < RMAX2 && fabs(c) < RMAX2)) {
      a = __dmul_rn(a, SC); b = __dmul_rn(b, SC); c = __dmul_rn(c, SC); d = __dmul_rn(d, SC);
    }
    const double ratio = __ddiv_rn(d, c), denom = __fma_rn(d, ratio, c);
    if (fabs(ratio) > RMIN) {
      x = __ddiv_rn(__fma_rn(b, ratio, a), denom);
      y = __ddiv_rn(__fma_rn(-a, ratio, b), denom);
    } else {
      x = __ddiv_rn(__fma_rn(__ddiv_rn(b, c), d, a), denom);
      y = __ddiv_rn(__fma_rn(-__ddiv_rn(a, c), d, b), denom);
    }
  }
  return dcx{x, y};
}
__device__ __forceinline__ int16_t qf_s16(double v)
{
  return (int16_t)(v > 32767.0 ? 32767 : v < -32768.0 ? -32768 : (long long)llround(v));
}
__global__ void k_mmse2(const nr_gpu_demod_job_t j, const nrd_c16 *h, const Shared *sh)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const int shift = sh->d.log2_maxh, nb = j.nbRx;
  const int16_t qa = j.qm == 4 ? NRD_QAM16_n1 : j.qm == 6 ? NRD_QAM64_n1 : j.qm == 8 ? NRD_QAM256_n1 : 0;
  const int16_t qb = j.qm == 6 ? NRD_QAM64_n2 : j.qm == 8 ? NRD_QAM256_n2 : 0;
  const int16_t qr = j.qm == 8 ? NRD_QAM256_n3 : 0;
  const uint32_t nre = j.nre[m];
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < nre; i += gridDim.x * blockDim.x) {
    dcx M[2][5];
#pragma unroll
    for (int r = 0; r < 2; r++) {
#pragma unroll
      for (int c = 0; c < 2; c++) {
        nrd_c16 g = {0, 0};
        for (int a = 0; a < nb; a++) {
          const nrd_c16 hr = h[((size_t)m * 16 + r * nb + a) * EXT + i], hc = h[((size_t)m * 16 + c * nb + a) * EXT + i];
          const nrd_c16 p = nrd_cpx_mult_conj(hr, hc, shift);
          g = a == 0 ? p : nrd_c16{nrd_adds(g.r, p.r), nrd_adds(g.i, p.i)};
        }
        M[r][c] = dcx{(double)g.r, (double)g.i};
        M[r][3 + c] = dcx{r == c ? 1.0 : 0.0, 0.0};
      }
      const nrd_c16 b = ((const nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i];
      M[r][2] = dcx{(double)b.r, (double)b.i};
    }
    /* the CPU's Gauss-Jordan for nl = 2, unrolled with static indices (no local-memory spills): k = 0 pivots between
     * the two rows, k = 1 has a single candidate; same operations, same order */
    bool singular = false;
    {
      const bool sw = cabs_c(M[1][0]) > cabs_c(M[0][0]);
      if (cabs_c(sw ? M[1][0] : M[0][0]) < 1e-9)
        singular = true;
      if (!singular) {
        if (sw) {
#pragma unroll
          for (int c = 0; c < 5; c++) {
            const dcx t = M[0][c];
            M[0][c] = M[1][c];
            M[1][c] = t;
          }
        }
        const dcx inv = divdc3(1.0, 0.0, M[0][0].r, M[0][0].i);
#pragma unroll
        for (int c = 0; c < 5; c++)
          M[0][c] = cmul(M[0][c], inv);
        const dcx f = M[1][0];
        if (!(f.r == 0.0 && f.i == 0.0)) {
#pragma unroll
          for (int c = 0; c < 5; c++) {
            const dcx t = cmul(M[0][c], f);
            M[1][c] = dcx{__dsub_rn(M[1][c].r, t.r), __dsub_rn(M[1][c].i, t.i)};
          }
        }
        if (cabs_c(M[1][1]) < 1e-9)
          singular = true;
        if (!singular) {
          const dcx inv1 = divdc3(1.0, 0.0, M[1][1].r, M[1][1].i);
#pragma unroll
          for (int c = 0; c < 5; c++)
            M[1][c] = cmul(M[1][c], inv1);
          const dcx f1 = M[0][1];
          if (!(f1.r == 0.0 && f1.i == 0.0)) {
#pragma unroll
            for (int c = 0; c < 5; c++) {
              const dcx t = cmul(M[1][c], f1);
              M[0][c] = dcx{__dsub_rn(M[0][c].r, t.r), __dsub_rn(M[0][c].i, t.i)};
            }
          }
        }
      }
    }
#pragma unroll
    for (int r = 0; r < 2; r++) {
      const double dinv = singular ? 0.0 : M[r][3 + r].r;
      const double w = dinv > 1e-12 ? __ddiv_rn(1.0, dinv) : 0.0;
      const double orr = w > 0.0 ? __dmul_rn(M[r][2].r, w) : 0.0, oi = w > 0.0 ? __dmul_rn(M[r][2].i, w) : 0.0;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i;
      ((nrd_c16 *)j.comp)[row] = nrd_c16{qf_s16(orr), qf_s16(oi)};
      const int16_t ma = qf_s16(__dmul_rn(__dmul_rn(w, (double)qa), 1.0 / 32768.0)); /* == / 32768.0 exactly */
      const int16_t mb = qf_s16(__dmul_rn(__dmul_rn(w, (double)qb), 1.0 / 32768.0));
      const int16_t mr = qf_s16(__dmul_rn(__dmul_rn(w, (double)qr), 1.0 / 32768.0));
      ((nrd_c16 *)j.mag)[row] = nrd_c16{ma, ma};
      ((nrd_c16 *)j.magb)[row] = nrd_c16{mb, mb};
      ((nrd_c16 *)j.magr)[row] = nrd_c16{mr, mr};
    }
  }
}


/* ---- DECGPU: nr_dlsch_mmse() for Nl = 3, 4 (ISAC_PDSCH_GPU_EXACT_DEMOD_NL34=1). One thread per RE. --------------- */
/* QAM-FIX mode 2: the generic Gauss-Jordan of the CPU source with the operation order of the Nl = 2 kernel above
 * (same compiled loop): pivot by cabs, complex scaling cmul(M[k][c], inv), elimination M[r][c] - cmul(M[k][c], f). */
template <int NL> __global__ void k_mmse2n(const nr_gpu_demod_job_t j, const nrd_c16 *h, const Shared *sh)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  constexpr int NC = 2 * NL + 1;
  const int shift = sh->d.log2_maxh, nb = j.nbRx;
  const int16_t qa = j.qm == 4 ? NRD_QAM16_n1 : j.qm == 6 ? NRD_QAM64_n1 : j.qm == 8 ? NRD_QAM256_n1 : 0;
  const int16_t qb = j.qm == 6 ? NRD_QAM64_n2 : j.qm == 8 ? NRD_QAM256_n2 : 0;
  const int16_t qr = j.qm == 8 ? NRD_QAM256_n3 : 0;
  const uint32_t nre = j.nre[m];
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < nre; i += gridDim.x * blockDim.x) {
    dcx M[NL][NC];
    for (int r = 0; r < NL; r++) {
      for (int c = 0; c < NL; c++) {
        nrd_c16 g = {0, 0};
        for (int a = 0; a < nb; a++) {
          const nrd_c16 hr = h[((size_t)m * 16 + r * nb + a) * EXT + i], hc = h[((size_t)m * 16 + c * nb + a) * EXT + i];
          const nrd_c16 p = nrd_cpx_mult_conj(hr, hc, shift);
          g = a == 0 ? p : nrd_c16{nrd_adds(g.r, p.r), nrd_adds(g.i, p.i)};
        }
        M[r][c] = dcx{(double)g.r, (double)g.i};
        M[r][NL + 1 + c] = dcx{r == c ? 1.0 : 0.0, 0.0};
      }
      const nrd_c16 b = ((const nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i];
      M[r][NL] = dcx{(double)b.r, (double)b.i};
    }
    bool singular = false;
    for (int k = 0; k < NL && !singular; k++) {
      int piv = k;
      for (int r = k + 1; r < NL; r++)
        if (cabs_c(M[r][k]) > cabs_c(M[piv][k]))
          piv = r;
      if (cabs_c(M[piv][k]) < 1e-9) {
        singular = true;
        break;
      }
      if (piv != k)
        for (int c = 0; c < NC; c++) {
          const dcx t = M[k][c];
          M[k][c] = M[piv][c];
          M[piv][c] = t;
        }
      const dcx inv = divdc3(1.0, 0.0, M[k][k].r, M[k][k].i);
      for (int c = 0; c < NC; c++)
        M[k][c] = cmul(M[k][c], inv);
      for (int r = 0; r < NL; r++) {
        if (r == k)
          continue;
        const dcx f = M[r][k];
        if (f.r == 0.0 && f.i == 0.0)
          continue;
        for (int c = 0; c < NC; c++) {
          const dcx t = cmul(M[k][c], f);
          M[r][c] = dcx{__dsub_rn(M[r][c].r, t.r), __dsub_rn(M[r][c].i, t.i)};
        }
      }
    }
    for (int r = 0; r < NL; r++) {
      const double dinv = singular ? 0.0 : M[r][NL + 1 + r].r;
      const double w = dinv > 1e-12 ? __ddiv_rn(1.0, dinv) : 0.0;
      const double orr = w > 0.0 ? __dmul_rn(M[r][NL].r, w) : 0.0, oi = w > 0.0 ? __dmul_rn(M[r][NL].i, w) : 0.0;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + r) * j.buf + i;
      ((nrd_c16 *)j.comp)[row] = nrd_c16{qf_s16(orr), qf_s16(oi)};
      const int16_t ma = qf_s16(__dmul_rn(__dmul_rn(w, (double)qa), 1.0 / 32768.0));
      const int16_t mb = qf_s16(__dmul_rn(__dmul_rn(w, (double)qb), 1.0 / 32768.0));
      const int16_t mr = qf_s16(__dmul_rn(__dmul_rn(w, (double)qr), 1.0 / 32768.0));
      ((nrd_c16 *)j.mag)[row] = nrd_c16{ma, ma};
      ((nrd_c16 *)j.magb)[row] = nrd_c16{mb, mb};
      ((nrd_c16 *)j.magr)[row] = nrd_c16{mr, mr};
    }
  }
}

/* stock fixed-point MMSE (mode 0): mult_complex_vectors(in1, in2, s) = conj-free complex product, madd lanes, low 16
 * bits of re >> s and im >> s (srai / slli + blend, no saturation); nr_determin()'s cofactor recursion along index 0;
 * adds_epi16 sums; sign_epi16 negation. A[x][y] is the CPU's conjH_H_elements[0][x][y]. */
__device__ __forceinline__ nrd_c16 mcv(nrd_c16 a, nrd_c16 b, int s)
{
  const int32_t re = nrd_madd(a.r, b.r, a.i, nrd_neg_wrap(b.i)), im = nrd_madd(a.r, b.i, a.i, b.r);
  const int16_t ore = (int16_t)nrd_srai32(re, s);
  const int16_t oim = 16 - s >= 32 || 16 - s < 0 ? 0 : (int16_t)(((uint32_t)im << (16 - s)) >> 16);
  return nrd_c16{ore, oim};
}
template <int S> struct Det {
  /* a[x][y] for x, y < S given as index lists into the full matrix */
  __device__ static nrd_c16 run(const nrd_c16 (&A)[4][4], const int *xs, const int *ys, int sign, int s0)
  {
    nrd_c16 acc = {0, 0};
    for (int rtx = 0; rtx < S; rtx++) {
      int sx[S - 1 > 0 ? S - 1 : 1], sy[S - 1 > 0 ? S - 1 : 1], k = 0;
      for (int c = 1; c < S; c++) /* sub_matrix[cidx][ridx] = a44[cc[cidx]][rr[ridx]], cc skips ctx = 0, rr skips rtx */
        sx[c - 1] = xs[c];
      for (int r = 0; r < S; r++)
        if (r != rtx)
          sy[k++] = ys[r];
      const nrd_c16 t = Det<S - 1>::run(A, sx, sy, ((rtx & 1) ? -1 : 1) * sign, s0);
      const nrd_c16 p = mcv(A[xs[0]][ys[rtx]], t, s0);
      acc = rtx == 0 ? p : nrd_c16{nrd_adds(acc.r, p.r), nrd_adds(acc.i, p.i)};
    }
    return acc;
  }
};
template <> struct Det<1> {
  __device__ static nrd_c16 run(const nrd_c16 (&A)[4][4], const int *xs, const int *ys, int sign, int)
  {
    const nrd_c16 a = A[xs[0]][ys[0]];
    return sign < 0 ? nrd_c16{nrd_neg_wrap(a.r), nrd_neg_wrap(a.i)} : a;
  }
};
template <int NL> __global__ void k_mmse0n(const nr_gpu_demod_job_t j, const nrd_c16 *h, const Shared *sh)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const int shift = sh->d.log2_maxh, s0 = shift - 1, nb = j.nbRx;
  const uint32_t nre = j.nre[m];
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < nre; i += gridDim.x * blockDim.x) {
    nrd_c16 A[4][4];
    for (int r = 0; r < NL; r++)
      for (int c = 0; c < NL; c++) { /* conjH_H_elements[a][c][r] = conj(h_r) h_c >> shift, summed over a */
        nrd_c16 g = {0, 0};
        for (int a = 0; a < nb; a++) {
          const nrd_c16 p = nrd_cpx_mult_conj(h[((size_t)m * 16 + r * nb + a) * EXT + i], h[((size_t)m * 16 + c * nb + a) * EXT + i], shift);
          g = a == 0 ? p : nrd_c16{nrd_adds(g.r, p.r), nrd_adds(g.i, p.i)};
        }
        A[c][r] = g;
      }
    if (j.nvar != 0)
      for (int p = 0; p < NL; p++) { /* add_epi32 of nvar >> 3 on the (re | im << 16) word */
        const uint32_t w = ((uint32_t)(uint16_t)A[p][p].r | (uint32_t)(uint16_t)A[p][p].i << 16) + (j.nvar >> 3);
        A[p][p] = nrd_c16{(int16_t)(uint16_t)(w & 0xffff), (int16_t)(uint16_t)(w >> 16)};
      }
    int all[4] = {0, 1, 2, 3};
    const nrd_c16 det = Det<NL>::run(A, all, all, 1, s0);
    nrd_c16 inv[NL][NL]; /* inv_H_h_H[rtx][ctx] */
    for (int rtx = 0; rtx < NL; rtx++)
      for (int ctx = 0; ctx < NL; ctx++) {
        int cc[NL - 1], rr[NL - 1], k = 0, q = 0;
        for (int x = 0; x < NL; x++)
          if (x != ctx)
            cc[k++] = x;
        for (int y = 0; y < NL; y++)
          if (y != rtx)
            rr[q++] = y;
        inv[rtx][ctx] = Det<NL - 1>::run(A, cc, rr, ((rtx & 1) ? -1 : 1) * ((ctx & 1) ? -1 : 1), s0);
      }
    nrd_c16 y[NL];
    for (int l = 0; l < NL; l++)
      y[l] = ((const nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf + i];
    for (int rtx = 0; rtx < NL; rtx++) {
      nrd_c16 z = {0, 0};
      for (int ctx = 0; ctx < NL; ctx++) {
        const nrd_c16 p = mcv(inv[ctx][rtx], y[ctx], s0);
        z = nrd_c16{nrd_adds(z.r, p.r), nrd_adds(z.i, p.i)};
      }
      ((nrd_c16 *)j.comp)[((size_t)m * NR_GPU_DEMOD_MAX_NL + rtx) * j.buf + i] = z;
    }
    if (j.qm > 2) { /* thresholds from det: (det.r, det.r) * QAM amplitude, mulhrs, layer 0's rows */
      const int16_t qa = j.qm == 4 ? NRD_QAM16_n1 : j.qm == 6 ? NRD_QAM64_n1 : NRD_QAM256_n1;
      const int16_t qb = j.qm == 6 ? NRD_QAM64_n2 : j.qm == 8 ? NRD_QAM256_n2 : 0;
      const int16_t qr = j.qm == 8 ? NRD_QAM256_n3 : 0;
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL) * j.buf + i;
      const int16_t a = nrd_mulhrs(det.r, qa), b = nrd_mulhrs(det.r, qb), c = nrd_mulhrs(det.r, qr);
      ((nrd_c16 *)j.mag)[row] = nrd_c16{a, a};
      ((nrd_c16 *)j.magb)[row] = nrd_c16{b, b};
      ((nrd_c16 *)j.magr)[row] = nrd_c16{c, c};
    }
  }
}

/* ---- ISAC_PDSCH_EVM_GRANT on the GPU (nr_rx_pdsch()'s EVM block, Nl <= 2): one block per symbol. Per-RE terms
 * follow the CPU formulas (the 1-layer solve x = b / G is the CPU's exact IEEE division); the sums are block
 * reductions, so the accumulator agrees with the CPU's sequential sums to rounding, not bit for bit. The decoder's
 * inputs (LLRs) never depend on it. ---------------------------------------------------------------------------- */
template <typename T> __device__ T block_sum(T v, T *red)
{
  const int t = threadIdx.x;
  red[t] = v;
  __syncthreads();
  for (int k = blockDim.x / 2; k > 0; k >>= 1) {
    if (t < k)
      red[t] += red[t + k];
    __syncthreads();
  }
  const T r = red[0];
  __syncthreads();
  return r;
}
/* block reduction of K partials per thread; the sums land in thread 0's v[] (warp shuffles, then the warps) */
template <typename T, int K> __device__ void block_vsum(T (&v)[K], T *sh)
{
  const int t = threadIdx.x, lane = t & 31, w = t >> 5, nw = blockDim.x >> 5;
  for (int k = 0; k < K; k++)
    for (int off = 16; off > 0; off >>= 1)
      v[k] += __shfl_down_sync(0xffffffffu, v[k], off);
  if (lane == 0)
    for (int k = 0; k < K; k++)
      sh[w * K + k] = v[k];
  __syncthreads();
  if (t == 0)
    for (int k = 0; k < K; k++) {
      T a = sh[k];
      for (int q = 1; q < nw; q++)
        a += sh[q * K + k];
      v[k] = a;
    }
  __syncthreads();
}
constexpr int EVM_CH = 240;                              /* REs per EVM block: 20 RBs, so an RB never straddles */
constexpr int EVM_NCH = (EXT + EVM_CH - 1) / EVM_CH;     /* blocks per symbol */
constexpr int EVM_P = 80;                                /* partial slots per (symbol, block): pass 1 0-2, pass 2 3-53, pass 3 54-73 */
struct EvmCtx {
  int nl, nbRx, nb, a0, a1, lmax;
  double ideal_pow;
};
__device__ __forceinline__ bool evm_ctx(const nr_gpu_demod_job_t &j, const Shared *sh, int m, EvmCtx &c)
{
  if (!(j.sym_mask >> m & 1) || !(j.nl >= 1 && j.nl <= 2 && j.nre[m] >= 12 && j.nre[m] <= j.ext))
    return false;
  const nrd_decision d = sh->d;
  c.nl = j.nl;
  c.nbRx = j.nbRx;
  c.nb = j.nbRx < 4 ? j.nbRx : 4;
  c.a0 = j.nl == 1 ? d.mrc_rx_index : 0;
  c.a1 = j.nl == 1 ? d.mrc_rx_index + d.mrc_nb_rx : j.nbRx;
  c.lmax = (1 << (j.qm / 2)) - 1;
  double ms = 0.0;
  int nlev = 0;
  for (int l = 1; l <= c.lmax; l += 2) {
    ms += (double)l * l;
    nlev++;
  }
  c.ideal_pow = 2.0 * ms / nlev;
  return true;
}
template <typename T> __device__ __forceinline__ void evm_slice(T x, T scale, int lmax, T &v, T &s)
{
  v = x * scale;
  s = (T)2 * floor(v * (T)0.5) + (T)1; /* v * 0.5 == v / 2.0 exactly; 2 floor + 1 is exact */
  s = s > (T)lmax ? (T)lmax : s < (T)-lmax ? (T)-lmax : s;
}
/* Per-RE math type of the GPU EVM: T = float (default, ISAC_PDSCH_GPU_EXACT_DEMOD_EVM=1; the GB10 has ~1/64-rate FP64)
 * or double (=2, the CPU's formulas in double). Sums are always accumulated in double. Diagnostic only: the decoder's
 * LLRs never depend on it. */
/* block sums of K float terms per thread: float warp shuffles, then the 8 warp partials in double (fixed order) */
template <int K> __device__ void block_colsum(const float (&v0)[K], float *col, double *out)
{
  const int t = threadIdx.x, lane = t & 31, w = t >> 5, nw = blockDim.x >> 5;
  float v[K];
#pragma unroll
  for (int k = 0; k < K; k++) {
    v[k] = v0[k];
#pragma unroll
    for (int off = 16; off > 0; off >>= 1)
      v[k] += __shfl_down_sync(0xffffffffu, v[k], off);
  }
  if (lane == 0)
#pragma unroll
    for (int k = 0; k < K; k++)
      col[w * K + k] = v[k];
  __syncthreads();
  if (t < K) {
    double a = 0.0;
    for (int q = 0; q < nw; q++)
      a += (double)col[q * K + t];
    out[t] = a;
  }
  __syncthreads();
}
template <typename T> struct cxT {
  T r, i;
};
template <typename T> __device__ __forceinline__ cxT<T> cmulT(cxT<T> a, cxT<T> b)
{
  return cxT<T>{a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
}
/* pass 1: per-RE zero forcing; block partials {pw0, pw1, nv} */
template <typename T>
__global__ void k_evm1(const nr_gpu_demod_job_t j, const nrd_c16 *y, const nrd_c16 *h, double2 *xs, const Shared *sh, double *part, int erase)
{
  __shared__ double shd[8 * 3];
  const int m = blockIdx.y, t = threadIdx.x;
  EvmCtx c;
  if (!evm_ctx(j, sh, m, c))
    return;
  const uint32_t nre = j.nre[m], i = blockIdx.x * EVM_CH + t;
  if (blockIdx.x * EVM_CH >= nre)
    return;
  const nrd_c16 *Y = y + (size_t)m * NR_GPU_DEMOD_MAX_RX * EXT;
  const nrd_c16 *H = h + (size_t)m * 16 * EXT;
  double2 *X = xs + (size_t)m * 2 * EXT;
  double v[3] = {0.0, 0.0, 0.0};
  if (t < EVM_CH && i < nre) {
    const int nl = c.nl, nbRx = c.nbRx;
    long long g[2][3][2] = {{{0}}};
    for (int a = c.a0; a < c.a1; a++) {
      const nrd_c16 yy = Y[(size_t)a * EXT + i];
#pragma unroll
      for (int l = 0; l < 2; l++) {
        if (l >= nl)
          break;
        const nrd_c16 hl = H[((size_t)l * nbRx + a) * EXT + i];
#pragma unroll
        for (int k = 0; k < 2; k++) {
          if (k >= nl)
            break;
          const nrd_c16 hk = H[((size_t)k * nbRx + a) * EXT + i];
          g[l][k][0] += (long long)hl.r * hk.r + (long long)hl.i * hk.i; /* conj(h_l) h_k: exact integers */
          g[l][k][1] += (long long)hl.r * hk.i - (long long)hl.i * hk.r;
        }
        g[l][2][0] += (long long)hl.r * yy.r + (long long)hl.i * yy.i;
        g[l][2][1] += (long long)hl.r * yy.i - (long long)hl.i * yy.r;
      }
    }
    bool ok = true;
    if (erase) {
      bool z = true;
      for (int a = c.a0; a < c.a1 && z; a++)
        z = Y[(size_t)a * EXT + i].r == 0 && Y[(size_t)a * EXT + i].i == 0;
      ok = !z;
    }
    double2 x[2] = {make_double2(0, 0), make_double2(0, 0)};
    if (nl == 1) {
      const double g00 = (double)g[0][0][0];
      ok = ok && g00 > 1e-9;
      if (ok) { /* __divdc3(a, b, c, 0) == (a / c, b / c) */
        if (sizeof(T) == sizeof(double))
          x[0] = make_double2(__ddiv_rn((double)g[0][2][0], g00), __ddiv_rn((double)g[0][2][1], g00));
        else
          x[0] = make_double2((double)((float)g[0][2][0] / (float)g00), (double)((float)g[0][2][1] / (float)g00));
      }
    } else if (sizeof(T) == sizeof(double)) { /* the CPU's elimination in double */
      dcx G[2][3];
      for (int l = 0; l < 2; l++)
        for (int k = 0; k < 3; k++)
          G[l][k] = dcx{(double)g[l][k][0], (double)g[l][k][1]};
      for (int cc = 0; cc < 2 && ok; cc++) {
        int piv = cc;
        for (int r = cc + 1; r < 2; r++)
          if (cabs_c(G[r][cc]) > cabs_c(G[piv][cc]))
            piv = r;
        if (cabs_c(G[piv][cc]) <= 1e-9) {
          ok = false;
          break;
        }
        if (piv != cc)
          for (int k = 0; k < 3; k++) {
            const dcx tt = G[cc][k];
            G[cc][k] = G[piv][k];
            G[piv][k] = tt;
          }
        for (int r = 0; r < 2; r++) {
          if (r == cc)
            continue;
          const dcx f = divdc3(G[r][cc].r, G[r][cc].i, G[cc][cc].r, G[cc][cc].i);
          for (int k = cc; k < 3; k++) {
            const dcx p = cmul(G[cc][k], f);
            G[r][k] = dcx{__dsub_rn(G[r][k].r, p.r), __dsub_rn(G[r][k].i, p.i)};
          }
        }
      }
      for (int l = 0; l < 2 && ok; l++) {
        const dcx q = divdc3(G[l][2].r, G[l][2].i, G[l][l].r, G[l][l].i);
        x[l] = make_double2(q.r, q.i);
      }
    } else { /* 2 x 2 Hermitian solve in float (Cramer): x = adj(G) b / det(G) */
      const float g00 = (float)g[0][0][0], g11 = (float)g[1][1][0];
      const cxT<float> g01 = {(float)g[0][1][0], (float)g[0][1][1]}, g10 = {(float)g[1][0][0], (float)g[1][0][1]};
      const cxT<float> b0 = {(float)g[0][2][0], (float)g[0][2][1]}, b1 = {(float)g[1][2][0], (float)g[1][2][1]};
      const cxT<float> p = cmulT(g01, g10);
      const float det = g00 * g11 - p.r; /* real for a Hermitian G */
      const float mx = fmaxf(fabsf(g00), fabsf(g11));
      ok = ok && mx > 1e-9f && fabsf(det) > 1e-7f * mx * mx;
      if (ok) {
        const cxT<float> u = cmulT(g01, b1), w = cmulT(g10, b0);
        x[0] = make_double2((double)((g11 * b0.r - u.r) / det), (double)((g11 * b0.i - u.i) / det));
        x[1] = make_double2((double)((g00 * b1.r - w.r) / det), (double)((g00 * b1.i - w.i) / det));
      }
    }
    for (int l = 0; l < nl; l++) {
      if (!ok) {
        X[(size_t)l * EXT + i] = make_double2(nan(""), nan(""));
        continue;
      }
      X[(size_t)l * EXT + i] = x[l];
      v[l] = sizeof(T) == sizeof(double) ? __fma_rn(x[l].x, x[l].x, __dmul_rn(x[l].y, x[l].y))
                                         : (double)((float)x[l].x * (float)x[l].x + (float)x[l].y * (float)x[l].y);
    }
    v[2] = ok ? 1.0 : 0.0;
  }
  block_vsum(v, shd);
  if (t == 0)
    for (int k = 0; k < 3; k++)
      part[((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + k] = v[k];
}
/* fold the block partials of slots [o, o+n) per symbol into sum[m][0..n) (block order) */
__global__ void k_evm_fold(const nr_gpu_demod_job_t j, const double *part, int o, int n, double *sum)
{
  const int m = blockIdx.x, k = threadIdx.x;
  if (k >= n || !(j.sym_mask >> m & 1))
    return;
  const int nch = (j.nre[m] + EVM_CH - 1) / EVM_CH;
  double a = 0.0;
  for (int b = 0; b < nch && b < EVM_NCH; b++)
    a += part[((size_t)m * EVM_NCH + b) * EVM_P + o + k];
  sum[m * EVM_P + o + k] = a;
}
/* pass 2: decisions, error and LS-fit partials; per-(RE mod 12) and per-RB error sums in RE order from shared memory */
template <typename T>
__global__ void k_evm2(const nr_gpu_demod_job_t j, const nrd_c16 *y, const nrd_c16 *h, const double2 *xs, double2 *sd, Shared *sh,
                       double *part, const double *sum)
{
  __shared__ double shd[8 * 27];
  __shared__ float colf[8 * 27];
  __shared__ float s_e[2][EVM_CH];  /* e2 / ideal per (layer, RE of the block), -1 = no term */
  __shared__ double s_sc[2];
  const int m = blockIdx.y, t = threadIdx.x;
  EvmCtx c;
  nr_gpu_evm_sym_t *E = &sh->evm[m];
  if (!evm_ctx(j, sh, m, c))
    return;
  const uint32_t nre = j.nre[m], i = blockIdx.x * EVM_CH + t;
  if (blockIdx.x * EVM_CH >= nre)
    return;
  const double *S = sum + m * EVM_P;
  const unsigned nv = (unsigned)S[2];
  if (!(nv >= 12 && S[0] > 0.0 && (c.nl < 2 || S[1] > 0.0)))
    return;
  if (t == 0) {
    s_sc[0] = sqrt(c.ideal_pow / (S[0] / nv));
    s_sc[1] = c.nl > 1 ? sqrt(c.ideal_pow / (S[1] / nv)) : 0.0;
  }
  __syncthreads();
  const T scale[2] = {(T)s_sc[0], (T)s_sc[1]};
  const nrd_c16 *Y = y + (size_t)m * NR_GPU_DEMOD_MAX_RX * EXT;
  const nrd_c16 *H = h + (size_t)m * 16 * EXT;
  const double2 *X = xs + (size_t)m * 2 * EXT;
  double2 *D = sd + (size_t)m * 2 * EXT;
  const int lmax = c.lmax, nl = c.nl;
  const T inv_ideal = (T)1 / (T)c.ideal_pow;
  /* v: err_l[2] qr[2] qi[2] qd[2] qx[2] cr[4] ci[4] P[4] Y[4] nterm */
  T v[27];
#pragma unroll
  for (int k = 0; k < 27; k++)
    v[k] = 0;
  if (t < EVM_CH) {
    s_e[0][t] = -1.f;
    s_e[1][t] = -1.f;
  }
  if (t < EVM_CH && i < nre && !isnan(X[i].x)) {
    T sr[2] = {0, 0}, sqv[2] = {0, 0};
#pragma unroll
    for (int l = 0; l < 2; l++) {
      if (l >= nl)
        break;
      const double2 x = X[(size_t)l * EXT + i];
      T vi, vq, si, sq;
      evm_slice<T>((T)x.x, scale[l], lmax, vi, si);
      evm_slice<T>((T)x.y, scale[l], lmax, vq, sq);
      const T e2 = (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
      v[l] += e2;
      s_e[l][t] = (float)(e2 * inv_ideal);
      v[2 + l] += vi * si + vq * sq;
      v[4 + l] += vq * si - vi * sq;
      v[6 + l] += si * si + sq * sq;
      v[8 + l] += vi * vi + vq * vq;
      sr[l] = si / scale[l];
      sqv[l] = sq / scale[l];
      D[(size_t)l * EXT + i] = make_double2((double)sr[l], (double)sqv[l]);
      v[26] += (T)1;
    }
#pragma unroll
    for (int a = 0; a < 4; a++) {
      if (a >= c.nb)
        break;
      const nrd_c16 yy = Y[(size_t)a * EXT + i];
      T hsr = 0, hsi = 0;
#pragma unroll
      for (int l = 0; l < 2; l++) {
        if (l >= nl)
          break;
        const nrd_c16 hh = H[((size_t)l * c.nbRx + a) * EXT + i];
        hsr += (T)hh.r * sr[l] - (T)hh.i * sqv[l];
        hsi += (T)hh.r * sqv[l] + (T)hh.i * sr[l];
      }
      v[10 + a] += (T)yy.r * hsr + (T)yy.i * hsi;
      v[14 + a] += (T)yy.i * hsr - (T)yy.r * hsi;
      v[18 + a] += hsr * hsr + hsi * hsi;
      v[22 + a] += (T)yy.r * (T)yy.r + (T)yy.i * (T)yy.i;
    }
  }
  __syncthreads();
  /* per RE index mod 12 (this block starts at a multiple of 12): thread k sums its REs in order, layers in order */
  if (t < 12) {
    float acc = 0.f;
    unsigned n = 0;
    for (int q = t; q < EVM_CH; q += 12)
      for (int l = 0; l < nl; l++)
        if (s_e[l][q] >= 0.f) {
          acc += s_e[l][q];
          n++;
        }
    part[((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + 30 + t] = acc;
    part[((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + 42 + t] = n;
  }
  /* per-RB map (symbols 0, 1, 3; layer 0): the CPU's own float sequence for that RB */
  if ((m == 0 || m == 1 || m == 3) && t < EVM_CH / 12) {
    const uint32_t rb = blockIdx.x * (EVM_CH / 12) + t;
    float acc = 0.f;
    unsigned n = 0;
    for (int q = t * 12; q < t * 12 + 12; q++)
      if (s_e[0][q] >= 0.f) {
        acc += s_e[0][q];
        n++;
      }
    if (rb < 276 && rb * 12 < nre) {
      E->err_rb[rb] = acc;
      E->n_rb[rb] = (uint16_t)n;
    }
  }
  double *dst = part + ((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + 3;
  if (sizeof(T) == sizeof(float)) {
    block_colsum(*(const float(*)[27])v, colf, dst);
  } else {
    double dv[27];
#pragma unroll
    for (int k = 0; k < 27; k++)
      dv[k] = (double)v[k];
    block_vsum(dv, shd);
    if (t == 0)
      for (int k = 0; k < 27; k++)
        dst[k] = dv[k];
  }
}
/* pass 3: cross-branch correlation of the LS residuals (needs the symbol's alpha_a from pass 2) */
template <typename T>
__global__ void k_evm3(const nr_gpu_demod_job_t j, const nrd_c16 *y, const nrd_c16 *h, const double2 *xs, const double2 *sd,
                       const Shared *sh, double *part, const double *sum)
{
  __shared__ double shd[8 * 20];
  __shared__ float colf[8 * 20];
  __shared__ double s_ar[4][2];
  const int m = blockIdx.y, t = threadIdx.x;
  EvmCtx c;
  if (!evm_ctx(j, sh, m, c))
    return;
  const uint32_t nre = j.nre[m], i = blockIdx.x * EVM_CH + t;
  if (blockIdx.x * EVM_CH >= nre)
    return;
  const double *S = sum + m * EVM_P;
  const unsigned nv = (unsigned)S[2];
  if (!(nv >= 12 && S[0] > 0.0 && (c.nl < 2 || S[1] > 0.0)))
    return;
  if (t < c.nb) {
    const double P = S[3 + 18 + t];
    s_ar[t][0] = P > 0.0 ? S[3 + 10 + t] / P : 0.0;
    s_ar[t][1] = P > 0.0 ? S[3 + 14 + t] / P : 0.0;
  }
  __syncthreads();
  const nrd_c16 *Y = y + (size_t)m * NR_GPU_DEMOD_MAX_RX * EXT;
  const nrd_c16 *H = h + (size_t)m * 16 * EXT;
  const double2 *X = xs + (size_t)m * 2 * EXT;
  const double2 *D = sd + (size_t)m * 2 * EXT;
  T v[20];
#pragma unroll
  for (int k = 0; k < 20; k++)
    v[k] = 0;
  if (t < EVM_CH && i < nre && !isnan(X[i].x)) {
    cxT<T> e[4];
#pragma unroll
    for (int a = 0; a < 4; a++) {
      e[a] = cxT<T>{0, 0};
      if (a >= c.nb)
        continue;
      cxT<T> hs = {0, 0};
#pragma unroll
      for (int l = 0; l < 2; l++) {
        if (l >= c.nl)
          break;
        const nrd_c16 hh = H[((size_t)l * c.nbRx + a) * EXT + i];
        const double2 dd = D[(size_t)l * EXT + i];
        const cxT<T> p = cmulT(cxT<T>{(T)hh.r, (T)hh.i}, cxT<T>{(T)dd.x, (T)dd.y});
        hs.r += p.r;
        hs.i += p.i;
      }
      const cxT<T> q = cmulT(cxT<T>{(T)s_ar[a][0], (T)s_ar[a][1]}, hs);
      const nrd_c16 yy = Y[(size_t)a * EXT + i];
      e[a] = cxT<T>{(T)yy.r - q.r, (T)yy.i - q.i};
    }
    int k = 0;
#pragma unroll
    for (int a = 0; a < 4; a++)
#pragma unroll
      for (int b = a; b < 4; b++, k++) {
        if (a >= c.nb || b >= c.nb)
          continue;
        const cxT<T> p = cmulT(e[a], cxT<T>{e[b].r, -e[b].i});
        v[k] = p.r;
        v[10 + k] = p.i;
      }
  }
  double *dst = part + ((size_t)m * EVM_NCH + blockIdx.x) * EVM_P + 54;
  if (sizeof(T) == sizeof(float)) {
    block_colsum(*(const float(*)[20])v, colf, dst);
  } else {
    double dv[20];
#pragma unroll
    for (int k = 0; k < 20; k++)
      dv[k] = (double)v[k];
    block_vsum(dv, shd);
    if (t == 0)
      for (int k = 0; k < 20; k++)
        dst[k] = dv[k];
  }
}
__global__ void k_evm_zero(Shared *sh)
{
  for (int rb = threadIdx.x; rb < 276; rb += blockDim.x) {
    sh->evm[blockIdx.x].err_rb[rb] = 0.f;
    sh->evm[blockIdx.x].n_rb[rb] = 0;
  }
}
/* export the symbol's sums for the host fold */
__global__ void k_evm_out(const nr_gpu_demod_job_t j, Shared *sh, const double *sum)
{
  const int m = blockIdx.x;
  nr_gpu_evm_sym_t *E = &sh->evm[m];
  EvmCtx c;
  if (threadIdx.x != 0)
    return;
  E->valid = 0;
  if (!evm_ctx(j, sh, m, c))
    return;
  const double *S = sum + m * EVM_P;
  const unsigned nv = (unsigned)S[2];
  if (!(nv >= 12 && S[0] > 0.0 && (c.nl < 2 || S[1] > 0.0)))
    return;
  const double *V = S + 3;
  for (int l = 0; l < c.nl; l++) {
    E->err_l[l] = V[l];
    E->qr[l] = V[2 + l];
    E->qi[l] = V[4 + l];
    E->qd[l] = V[6 + l];
    E->qx[l] = V[8 + l];
  }
  for (int a = 0; a < c.nb; a++) {
    E->cr[a] = V[10 + a];
    E->ci[a] = V[14 + a];
    E->P[a] = V[18 + a];
    E->Y[a] = V[22 + a];
  }
  for (int k = 0; k < 12; k++) {
    E->err_k[k] = (float)S[30 + k];
    E->n_k[k] = (uint32_t)S[42 + k];
  }
  int k = 0;
  for (int a = 0; a < c.nb; a++)
    for (int b = a; b < c.nb; b++, k++) {
      E->Cr[a][b] = S[54 + k];
      E->Ci[a][b] = S[64 + k];
    }
  E->nv = nv;
  E->nterm = (uint32_t)V[26];
  E->ideal_pow = c.ideal_pow;
  E->nb = c.nb;
  E->rbmap = m == 0 || m == 1 || m == 3;
  E->row = m == 0 ? 0 : m == 1 ? 1 : 2;
  E->valid = 1;
}

/* ---- LLRs (nr_dlsch_llr) and layer demapping (nr_layer_demapping), straight into the decoder buffer ---------- */
__global__ void k_llr(const nr_gpu_demod_job_t j)
{
  const int m = blockIdx.y;
  if (!(j.sym_mask >> m & 1))
    return;
  const uint32_t nre = j.nre[m];
  for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < nre; i += gridDim.x * blockDim.x)
    for (int l = 0; l < j.nl; l++) {
      const size_t row = ((size_t)m * NR_GPU_DEMOD_MAX_NL + l) * j.buf;
      const size_t mrow = ((size_t)m * NR_GPU_DEMOD_MAX_NL + (j.mag_per_layer ? l : 0)) * j.buf;
      nrd_llr_re_out(j.qm, i, nre, (const nrd_c16 *)j.comp + row, (const nrd_c16 *)j.mag + mrow,
                     (const nrd_c16 *)j.magb + mrow, (const nrd_c16 *)j.magr + mrow,
                     j.llr + j.llr_off[m] + ((size_t)i * j.nl + l) * j.qm);
    }
}
} // namespace nrdm

using namespace nrdm;

extern "C" int nr_pdsch_gpu_demod_init(int nant)
{
  if (g_ok)
    return 0;
  if (nant > NR_GPU_DEMOD_MAX_RX)
    return -1;
  int lo = 0, hi = 0;
  if (cudaDeviceGetStreamPriorityRange(&lo, &hi) != cudaSuccess)
    return -1;
  for (int c = 0; c < CTX; c++) {
    Ctx &x = g_ctx[c];
    if (cudaStreamCreateWithPriority(&x.s, cudaStreamNonBlocking, hi) != cudaSuccess
        || cudaMalloc((void **)&x.y, sizeof(nrd_c16) * NSYM * NR_GPU_DEMOD_MAX_RX * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.h, sizeof(nrd_c16) * NSYM * 16 * EXT) != cudaSuccess
        || cudaMallocHost((void **)&x.hy, sizeof(nrd_c16) * NSYM * NR_GPU_DEMOD_MAX_RX * EXT) != cudaSuccess
        || cudaMallocHost((void **)&x.hh, sizeof(nrd_c16) * NSYM * 16 * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.cs, sizeof(double2) * NSYM * 4 * MAXCH) != cudaSuccess
        || cudaMalloc((void **)&x.tm, sizeof(double2) * NSYM * 4 * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.sd, sizeof(double2) * NSYM * 2 * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.part, sizeof(double) * NSYM * EVM_NCH * EVM_P) != cudaSuccess
        || cudaMalloc((void **)&x.sum, sizeof(double) * NSYM * EVM_P) != cudaSuccess
        || cudaMalloc((void **)&x.xs, sizeof(double2) * NSYM * 2 * EXT) != cudaSuccess
        || cudaMalloc((void **)&x.dsh, sizeof(Shared)) != cudaSuccess
        || cudaMallocHost((void **)&x.sh, sizeof(Shared)) != cudaSuccess) {
      fprintf(stderr, "nr_pdsch_gpu_demod_init: %s\n", cudaGetErrorString(cudaGetLastError()));
      return -1;
    }
    memset(x.sh, 0, sizeof(Shared));
    if (cudaMemset(x.dsh, 0, sizeof(Shared)) != cudaSuccess)
      return -1;
    for (int k = 0; k < 13; k++)
      if (cudaEventCreate(&x.ev[k]) != cudaSuccess)
        return -1;
    if (cudaStreamCreateWithPriority(&x.s2, cudaStreamNonBlocking, hi) != cudaSuccess
        || cudaEventCreateWithFlags(&x.fork, cudaEventDisableTiming) != cudaSuccess
        || cudaEventCreateWithFlags(&x.join, cudaEventDisableTiming) != cudaSuccess)
      return -1;
  }
  g_prof = getenv("ISAC_PDSCH_GPU_EXACT_DEMOD_PROF") && atoi(getenv("ISAC_PDSCH_GPU_EXACT_DEMOD_PROF")) == 1;
  /* first launches before the RX fence (module load, kernel images) */
  nr_gpu_demod_job_t j = {};
  j.sym_mask = 0;
  j.ext = 16;
  k_gather<<<dim3(1, NSYM), 32, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h);
  k_mask_pw<<<NSYM, 256, 0, g_ctx[0].s>>>(j, g_ctx[0].h, g_ctx[0].dsh);
  k_align_terms<<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].dsh, g_ctx[0].tm);
  k_align_cs<<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(j, g_ctx[0].dsh, g_ctx[0].tm, g_ctx[0].cs);
  k_align_rot<<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(j, g_ctx[0].h, g_ctx[0].dsh, g_ctx[0].cs);
  k_comp<<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].dsh);
  k_kappa<<<NSYM, 256, 0, g_ctx[0].s>>>(j, g_ctx[0].dsh);
  k_mmse2<<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(j, g_ctx[0].h, g_ctx[0].dsh);
  k_level<<<1, 256, 0, g_ctx[0].s>>>(j, g_ctx[0].h, g_ctx[0].dsh);
  k_decide<<<1, 1, 0, g_ctx[0].s>>>(j, g_ctx[0].dsh);
  k_evm_zero<<<NSYM, 288, 0, g_ctx[0].s>>>(g_ctx[0].dsh);
  k_evm1<float><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].dsh, g_ctx[0].part, 0);
  k_evm_fold<<<NSYM, 64, 0, g_ctx[0].s>>>(j, g_ctx[0].part, 0, 3, g_ctx[0].sum);
  k_evm2<float><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm3<float><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm3<double><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm2<double><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].sd, g_ctx[0].dsh, g_ctx[0].part, g_ctx[0].sum);
  k_evm1<double><<<dim3(1, NSYM), 256, 0, g_ctx[0].s>>>(j, g_ctx[0].y, g_ctx[0].h, g_ctx[0].xs, g_ctx[0].dsh, g_ctx[0].part, 0);
  k_evm_out<<<NSYM, 32, 0, g_ctx[0].s>>>(j, g_ctx[0].dsh, g_ctx[0].sum);
  k_llr<<<dim3(1, NSYM), 128, 0, g_ctx[0].s>>>(j);
  if (cudaStreamSynchronize(g_ctx[0].s) != cudaSuccess)
    return -1;
  g_ok = 1;
  return 0;
}

extern "C" void nr_pdsch_gpu_demod_release(int c)
{
  if (c >= 0 && c < CTX)
    g_busy.fetch_and(~(1u << c));
}

extern "C" int nr_pdsch_gpu_demod_run(nr_gpu_demod_job_t *j)
{
  if (!g_ok || j->nbRx > NR_GPU_DEMOD_MAX_RX || j->nl > NR_GPU_DEMOD_MAX_NL || j->ext > (uint32_t)EXT
      || j->nl * j->nbRx > 16)
    return -1;
  NrGpuRtScope rt;
  if (!rt.admitted)
    return -1;
  const int c = acquire();
  Ctx &x = g_ctx[c];
  const nr_gpu_demod_job_t jj = *j;
  int maxre = 1;
  for (int m = 0; m < NSYM; m++)
    if (jj.sym_mask >> m & 1)
      maxre = jj.nre[m] > maxre ? jj.nre[m] : maxre;
  const int gx = (int)((jj.ext + 127) / 128);
  if (g_prof) cudaEventRecord(x.ev[0], x.s);
  k_gather<<<dim3(gx, NSYM), 128, 0, x.s>>>(jj, x.y, x.h);
  if (g_prof) cudaEventRecord(x.ev[1], x.s);
  k_level<<<jj.nl * jj.nbRx, 256, 0, x.s>>>(jj, x.h, x.dsh);
  k_decide<<<1, 1, 0, x.s>>>(jj, x.dsh);
  if (g_prof) cudaEventRecord(x.ev[2], x.s);
  k_mask_pw<<<NSYM, 256, 0, x.s>>>(jj, x.h, x.dsh);
  if (g_prof) cudaEventRecord(x.ev[3], x.s);
  if (jj.nl == 1 && jj.align)
  {
    k_align_terms<<<dim3(gx, NSYM), 128, 0, x.s>>>(jj, x.y, x.h, x.dsh, x.tm);
    k_align_cs<<<dim3((MAXCH * 4 + 127) / 128, NSYM), 128, 0, x.s>>>(jj, x.dsh, x.tm, x.cs);
    k_align_rot<<<dim3(gx, NSYM), 128, 0, x.s>>>(jj, x.h, x.dsh, x.cs);
  }
  if (g_prof) cudaEventRecord(x.ev[4], x.s);
  if (jj.want_evm) {
    cudaStream_t es = g_prof ? x.s : x.s2;
    if (!g_prof) {
      cudaEventRecord(x.fork, x.s);
      cudaStreamWaitEvent(x.s2, x.fork, 0);
    }
    k_evm_zero<<<NSYM, 288, 0, es>>>(x.dsh);
    if (jj.evm_fp64) {
      k_evm1<double><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(jj, x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      k_evm_fold<<<NSYM, 64, 0, es>>>(jj, x.part, 0, 3, x.sum);
      if (g_prof) cudaEventRecord(x.ev[10], es);
      k_evm2<double><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(jj, x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
      k_evm_fold<<<NSYM, 64, 0, es>>>(jj, x.part, 3, 51, x.sum);
      if (g_prof) cudaEventRecord(x.ev[11], es);
      k_evm3<double><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(jj, x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
    } else {
      k_evm1<float><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(jj, x.y, x.h, x.xs, x.dsh, x.part, jj.evm_erase);
      k_evm_fold<<<NSYM, 64, 0, es>>>(jj, x.part, 0, 3, x.sum);
      if (g_prof) cudaEventRecord(x.ev[10], es);
      k_evm2<float><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(jj, x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
      k_evm_fold<<<NSYM, 64, 0, es>>>(jj, x.part, 3, 51, x.sum);
      if (g_prof) cudaEventRecord(x.ev[11], es);
      k_evm3<float><<<dim3(EVM_NCH, NSYM), 256, 0, es>>>(jj, x.y, x.h, x.xs, x.sd, x.dsh, x.part, x.sum);
    }
    k_evm_fold<<<NSYM, 64, 0, es>>>(jj, x.part, 54, 20, x.sum);
    k_evm_out<<<NSYM, 32, 0, es>>>(jj, x.dsh, x.sum);
    if (!g_prof)
      cudaEventRecord(x.join, x.s2);
  }
  if (g_prof) cudaEventRecord(x.ev[5], x.s);
  k_comp<<<dim3(gx, NSYM), 128, 0, x.s>>>(jj, x.y, x.h, x.dsh);
  if (g_prof) cudaEventRecord(x.ev[6], x.s);
  if (jj.nl == 2)
    k_mmse2<<<dim3((maxre + 127) / 128, NSYM), 128, 0, x.s>>>(jj, x.h, x.dsh);
  else if (jj.nl == 3 && jj.qam_mode == 2)
    k_mmse2n<3><<<dim3((maxre + 127) / 128, NSYM), 128, 0, x.s>>>(jj, x.h, x.dsh);
  else if (jj.nl == 4 && jj.qam_mode == 2)
    k_mmse2n<4><<<dim3((maxre + 127) / 128, NSYM), 128, 0, x.s>>>(jj, x.h, x.dsh);
  else if (jj.nl == 3)
    k_mmse0n<3><<<dim3((maxre + 127) / 128, NSYM), 128, 0, x.s>>>(jj, x.h, x.dsh);
  else if (jj.nl == 4)
    k_mmse0n<4><<<dim3((maxre + 127) / 128, NSYM), 128, 0, x.s>>>(jj, x.h, x.dsh);
  if (g_prof) cudaEventRecord(x.ev[7], x.s);
  if (jj.want_kappa && jj.qm > 2)
    k_kappa<<<NSYM, 256, 0, x.s>>>(jj, x.dsh);
  if (g_prof) cudaEventRecord(x.ev[8], x.s);
  k_llr<<<dim3((maxre + 127) / 128, NSYM), 128, 0, x.s>>>(jj);
  if (g_prof) cudaEventRecord(x.ev[9], x.s);
  if (jj.want_kappa && !jj.want_evm) { /* CPU EVM fallback reads the extraction buffers */
    cudaMemcpyAsync(x.hy, x.y, sizeof(nrd_c16) * NSYM * NR_GPU_DEMOD_MAX_RX * EXT, cudaMemcpyDeviceToHost, x.s);
    cudaMemcpyAsync(x.hh, x.h, sizeof(nrd_c16) * NSYM * 16 * EXT, cudaMemcpyDeviceToHost, x.s);
  }
  if (jj.want_evm && !g_prof)
    cudaStreamWaitEvent(x.s, x.join, 0);
  cudaMemcpyAsync(x.sh, x.dsh, sizeof(Shared), cudaMemcpyDeviceToHost, x.s);
  cudaError_t e = cudaPeekAtLastError();
  while (e == cudaSuccess) {
    e = cudaStreamQuery(x.s);
    if (e == cudaErrorNotReady)
      e = cudaSuccess;
    else
      break;
  }
  if (e != cudaSuccess) {
    fprintf(stderr, "nr_pdsch_gpu_demod_run: %s\n", cudaGetErrorString(e));
    g_ok = 0;
    nr_pdsch_gpu_demod_release(c);
    return -1;
  }
  if (g_prof) {
    for (int k = 0; k < 9; k++) {
      float ms = 0.f;
      if (cudaEventElapsedTime(&ms, x.ev[k], x.ev[k + 1]) == cudaSuccess)
        g_prof_ns[k] += (unsigned long long)(ms * 1e6f);
    }
    if (jj.want_evm) {
      float a = 0.f, b = 0.f, c = 0.f;
      if (cudaEventElapsedTime(&a, x.ev[4], x.ev[10]) == cudaSuccess && cudaEventElapsedTime(&b, x.ev[10], x.ev[11]) == cudaSuccess
          && cudaEventElapsedTime(&c, x.ev[11], x.ev[5]) == cudaSuccess) {
        g_prof_ns[10] += (unsigned long long)(a * 1e6f);
        g_prof_ns[11] += (unsigned long long)(b * 1e6f);
        g_prof_ns[12] += (unsigned long long)(c * 1e6f);
      }
    }
    const unsigned long long n = ++g_prof_n;
    if (n % 2000 == 0)
      fprintf(stderr, "GPU_EXACT_DEMOD_PROF n=%llu us/grant gather=%.1f level=%.1f mask_pw=%.1f align=%.1f evm=%.1f comp=%.1f mmse2=%.1f kappa=%.1f llr=%.1f (evm1 %.1f evm2 %.1f evm3 %.1f)\n", n,
              g_prof_ns[0] / 1e3 / n, g_prof_ns[1] / 1e3 / n, g_prof_ns[2] / 1e3 / n, g_prof_ns[3] / 1e3 / n, g_prof_ns[4] / 1e3 / n,
              g_prof_ns[5] / 1e3 / n, g_prof_ns[6] / 1e3 / n, g_prof_ns[7] / 1e3 / n, g_prof_ns[8] / 1e3 / n, g_prof_ns[10] / 1e3 / n,
              g_prof_ns[11] / 1e3 / n, g_prof_ns[12] / 1e3 / n);
  }
  const nrd_decision d = x.sh->d;
  j->log2_maxh = d.log2_maxh;
  j->used_branch_set = d.used_branch_set;
  j->used_branch = d.used_branch;
  j->mrc_rx_index = d.mrc_rx_index;
  j->mrc_nb_rx = d.mrc_nb_rx;
  memcpy(j->kappa_sz, x.sh->kap_sz, sizeof(j->kappa_sz));
  memcpy(j->kappa_sm, x.sh->kap_sm, sizeof(j->kappa_sm));
  if (jj.want_evm)
    memcpy(j->evm, x.sh->evm, sizeof(j->evm));
  j->y_ext = (const int16_t *)x.hy;
  j->h_ext = (const int16_t *)x.hh;
  j->ctx = c;
  return 0;
}
