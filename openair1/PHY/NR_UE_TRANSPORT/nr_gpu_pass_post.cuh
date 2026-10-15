/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 3: device kernels for the host steps between the GPU decode stages (chest -> [chest post] -> demod -> [rate dematching] ->
 * LDPC -> [CRC]). Included at the end of nr_pdsch_gpu_fep.cu (libpdsch_gpu.so); the interface is nr_gpu_pass_post.h. The nr_gpu_pass_launch_*
 * wrappers only enqueue on the given stream: no host wait, no allocation, no host-to-device copy, so they are graph-capturable. Results are
 * bit-exact with the CPU (the integer ones in every build, the FP64 chest-post build too) except the float chest-post build, which is the
 * product one and stays within 1e-6 relative of the FP64 values on the test inputs. */
#include "nr_gpu_pass_post.h"
#include "nr_pdsch_gpu_ldpc.h"
static_assert(NR_GPU_LDPC_DEV_IN_STRIDE == 68 * 384, "k_rdm writes the LDPC input at this stride");

namespace gpupass {

/* ------------------------------------------------------------------ CRC (crc_byte.c) ------------------------------------------------------------------ */
/* Same construction as crcTableInit(): table[c] = crcbit(&c, 1, poly), registers left-justified in 32 bits (CRC16: the 16-bit table value << 16). */
struct CrcTab {
  uint32_t t[3][256]; /* 0 = 24A, 1 = 24B, 2 = 16 */
  constexpr CrcTab() : t()
  {
    const uint32_t poly[3] = {0x864cfb00u, 0x80006300u, 0x10210000u};
    for (int k = 0; k < 3; k++)
      for (uint32_t c0 = 0; c0 < 256; c0++) {
        uint32_t crc = 0, c = c0 << 24;
        for (int i = 8; i != 0; i--) {
          crc = ((1u << 31) & (c ^ crc)) ? (crc << 1) ^ poly[k] : crc << 1;
          c <<= 1;
        }
        t[k][c0] = crc;
      }
  }
};
__constant__ CrcTab c_crc = CrcTab();
__constant__ uint32_t c_crc_poly[3] = {0x864cfb00u, 0x80006300u, 0x10210000u};

__device__ __forceinline__ uint32_t mulx(uint32_t r, uint32_t poly) { return (r << 1) ^ ((r >> 31) ? poly : 0u); }
/* a(x) * b(x) mod P in the left-justified register representation (w = 24 or 16 live bits) */
__device__ uint32_t gmul(uint32_t a, uint32_t b, uint32_t poly, int w)
{
  uint32_t acc = 0;
  for (int j = 31; j >= 32 - w; j--) {
    acc = mulx(acc, poly);
    if ((a >> j) & 1u)
      acc ^= b;
  }
  return acc;
}
/* x^(8 * nbytes) mod P */
__device__ uint32_t xpow8(uint32_t nbytes, uint32_t poly, int w)
{
  uint32_t base = 1u << (32 - w);
  for (int i = 0; i < 8; i++)
    base = mulx(base, poly);
  uint32_t res = 1u << (32 - w);
  for (; nbytes; nbytes >>= 1) {
    if (nbytes & 1u)
      res = gmul(res, base, poly, w);
    base = gmul(base, base, poly, w);
  }
  return res;
}

struct CrcMsg { /* the bytes of one message: one code block, or the TB reassembled from the code blocks */
  const uint8_t *bits;
  const nr_gpu_crc_seg_t *segs;
  bool tb;
  __device__ uint8_t at(uint32_t i) const
  {
    if (!tb)
      return bits[segs[blockIdx.x].off + i];
    const uint32_t pay = segs[0].pay_bytes;
    return bits[segs[i / pay].off + i % pay];
  }
};

/* check_crc(bytes, n, type): one block per message, the byte range split over the threads, the partial CRCs shifted into place by multiplying
 * with x^(8 * bytes behind) and XOR-ed (CRC is linear and starts at 0), then the same residual-bit step and compare as crc24a/crc24b/crc16. */
__global__ void k_crc(const uint8_t *bits, const nr_gpu_crc_seg_t *segs, int nseg, uint32_t tbs, uint8_t *seg_ok, uint8_t *tb_ok)
{
  const bool tb = (int)blockIdx.x == nseg;
  if (tb && nseg <= 1)
    return;
  const CrcMsg m = {bits, segs, tb};
  uint32_t n, type;
  if (tb) {
    n = tbs + (tbs > 3824u ? 24u : 16u);
    type = tbs > 3824u ? 0u : 2u;
    if (segs[0].pay_bytes == 0 || (uint64_t)nseg * segs[0].pay_bytes < (n + 7) / 8) { /* no reassembly possible: no division, no read past the segments */
      if (threadIdx.x == 0) *tb_ok = 0;
      return;
    }
  } else {
    n = segs[blockIdx.x].n_bits;
    type = segs[blockIdx.x].type;
  }
  const int w = type == 2 ? 16 : 24, cl = w / 8;
  const uint32_t poly = c_crc_poly[type];
  __shared__ uint32_t red[8];
  if (n < (uint32_t)w) { /* no message, no CRC: the CPU would read before the buffer */
    if (threadIdx.x == 0) {
      if (tb) *tb_ok = 0; else seg_ok[blockIdx.x] = 0;
    }
    return;
  }
  const uint32_t bitlen = n - w, nb = bitlen / 8;
  const uint32_t per = (nb + blockDim.x - 1) / blockDim.x;
  const uint32_t b0 = min(nb, threadIdx.x * per), b1 = min(nb, b0 + per);
  uint32_t crc = 0;
  for (uint32_t i = b0; i < b1; i++)
    crc = (crc << 8) ^ c_crc.t[type][m.at(i) ^ (crc >> 24)];
  if (b1 > b0)
    crc = gmul(crc, xpow8(nb - b1, poly, w), poly, w);
  else
    crc = 0;
  for (int o = 16; o; o >>= 1)
    crc ^= __shfl_xor_sync(0xffffffffu, crc, o);
  if ((threadIdx.x & 31) == 0)
    red[threadIdx.x >> 5] = crc;
  __syncthreads();
  if (threadIdx.x == 0) {
    crc = 0;
    for (unsigned i = 0; i < blockDim.x / 32; i++)
      crc ^= red[i];
    const int resbit = bitlen % 8;
    if (resbit > 0)
      crc = (crc << resbit) ^ c_crc.t[type][(m.at(nb) >> (8 - resbit)) ^ (crc >> (32 - resbit))];
    crc >>= 32 - w;
    uint32_t old = 0;
    for (int i = 0; i < cl; i++)
      old |= (uint32_t)m.at((n >> 3) - cl + i) << ((cl - 1 - i) << 3);
    old &= (1u << w) - 1u;
    const uint8_t ok = crc == old;
    if (tb) *tb_ok = ok; else seg_ok[blockIdx.x] = ok;
  }
}
/* nseg == 1: the TB is the code block */
__global__ void k_zero(uint8_t *p) { *p = 0; }
__global__ void k_crc_tb1(const uint8_t *seg_ok, uint8_t *tb_ok) { *tb_ok = *seg_ok; }

/* ----------------------------------------------------- rate dematching (nr_rate_matching.c + prep_segment) ----------------------------------------------------- */
__constant__ uint8_t c_index_k0[2][4] = {{0, 17, 33, 56}, {0, 13, 25, 43}};

/* One block per segment; every thread owns input positions j of the Kc * Z int8 vector. d(i) = the int16 sum of the soft values nr_rate_matching_ldpc_rx
 * adds at circular-buffer position i (clear = 1): the selectable positions form a circle of Nvalid = Ncb - filler entries, the stream k = 0..E-1 starts at
 * the rank s of k0 and runs round it, so position of rank r receives k = (r - s) mod Nvalid + n * Nvalid. soft(k) is the deinterleaved LLR. */
__global__ void k_rdm(const int16_t *llr, const nr_gpu_rdm_seg_t *segs, int nseg, int8_t *ldpc_in)
{
  const nr_gpu_rdm_seg_t sg = segs[blockIdx.x];
  int8_t *out = ldpc_in + (size_t)blockIdx.x * (68 * 384);
  const uint32_t Z = sg.Z, E = sg.E, Qm = sg.Qm, BG = sg.BG;
  const uint32_t Kc = BG == 2 ? 52 : 68, N = (BG == 1) ? 66 * Z : 50 * Z;
  const uint32_t Foffset = sg.K - sg.F - 2 * Z; /* uint32 as the CPU call: a wrapped value is refused below */
  uint32_t Ncb = N;
  if (sg.Tbslbrm != 0) {
    const uint32_t Nref = 3 * sg.Tbslbrm / (2 * sg.C);
    Ncb = min(N, Nref);
  }
  const bool bad = sg.C == 0 || (BG != 1 && BG != 2) || sg.rv > 3 || Foffset > Ncb || (Qm != 2 && Qm != 4 && Qm != 6 && Qm != 8) || E % Qm != 0 ||
                   sg.Kprime < 2 * Z || sg.Kprime > sg.K;
  const uint32_t k0 = bad ? 0 : (c_index_k0[BG - 1][sg.rv] * Ncb / N) * Z;
  const uint32_t Fx = Foffset < Ncb ? min(sg.F, Ncb - Foffset) : 0u; /* filler positions inside the circular buffer */
  const uint32_t Nvalid = Ncb - Fx;
  uint32_t s = 0; /* rank of the first position */
  if (k0 < Foffset)
    s = k0;
  else {
    const uint32_t i1 = k0 < Foffset + sg.F ? Foffset + sg.F : k0;
    s = i1 >= Ncb ? 0 : i1 - Fx;
  }
  const uint32_t EQ = Qm ? E / Qm : 1;
  for (uint32_t j = threadIdx.x; j < Kc * Z; j += blockDim.x) {
    int v = 0;
    if (bad || Nvalid == 0 || j < 2 * Z)
      v = 0;
    else if (j >= sg.Kprime && j < sg.K)
      v = j < sg.Kprime + sg.F ? 127 : 0;
    else {
      const uint32_t i = j - 2 * Z;
      if (i < Ncb && !(i >= Foffset && i < Foffset + Fx)) {
        const uint32_t r = i < Foffset ? i : i - Fx;
        uint32_t k = r >= s ? r - s : r + Nvalid - s;
        int acc = 0;
        for (; k < E; k += Nvalid)
          acc += llr[sg.llr_off + k / EQ + (k % EQ) * Qm];
        v = (int16_t)acc; /* int16 wraparound of d[ind] += soft */
      }
    }
    out[j] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
  }
}

/* --------------------------------------------------------------- chest post (nr_pdsch_passive_decode.c) --------------------------------------------------------------- */
template <class R> struct PostOut;
template <> struct PostOut<float> { typedef nr_gpu_chest_post_out_t T; };
template <> struct PostOut<double> { typedef nr_gpu_chest_post_out64_t T; };

__device__ __forceinline__ float  r_sqrt(float x) { return sqrtf(x); }
__device__ __forceinline__ double r_sqrt(double x) { return sqrt(x); }
__device__ __forceinline__ float  r_atan2(float y, float x) { return atan2f(y, x); }
__device__ __forceinline__ double r_atan2(double y, double x) { return atan2(y, x); }
/* x^2 + y^2 the way the CPU build of the test host contracts it (aarch64 GCC: one product, one fused multiply-add); the order of the two
 * differs between the two uses in the CPU source (found by comparing against the compiled reference). */
/* NOTE: an x86 build without FMA does not fuse at all; the FP64 build is bit-equal to the aarch64 CPU build only. Explicit intrinsics, so nvcc's own contraction cannot change the FP64 bits. */
__device__ __forceinline__ float  r_ssq_a(float x, float y) { return x * x + y * y; }
__device__ __forceinline__ float  r_ssq_b(float x, float y) { return x * x + y * y; }
__device__ __forceinline__ double r_ssq_a(double x, double y) { return __fma_rn(x, x, __dmul_rn(y, y)); } /* per half: fma(re, re, im * im) */
__device__ __forceinline__ double r_ssq_b(double x, double y) { return __fma_rn(y, y, __dmul_rn(x, x)); } /* coh_num: fma(im, im, re * re) */
template <class R> __device__ __forceinline__ R r_dt(const nr_gpu_chest_post_par_t &p);
template <> __device__ __forceinline__ float r_dt<float>(const nr_gpu_chest_post_par_t &p) { return p.dt_f; }
template <> __device__ __forceinline__ double r_dt<double>(const nr_gpu_chest_post_par_t &p) { return p.dt_d; }
template <class R> __device__ __forceinline__ R r_df(const nr_gpu_chest_post_par_t &p);
template <> __device__ __forceinline__ float r_df<float>(const nr_gpu_chest_post_par_t &p) { return p.df_f; }
template <> __device__ __forceinline__ double r_df<double>(const nr_gpu_chest_post_par_t &p) { return p.df_d; }

/* Block 1: the nvar normalisation and the per-branch substitution (integer math, same order and same truncations as the CPU). */
template <class O> __device__ void nvar_part(const nr_gpu_chest_post_par_t &p, O *out)
{
  if (threadIdx.x)
    return;
  if (p.n_dmrs_sym * p.nl <= 0) {
    out->nvar_valid = 0; /* nothing estimated: nvar untouched */
    return;
  }
  out->nvar_valid = 1;
  uint32_t nvar = 0, nvar_ant[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  const int nk = p.n_dmrs_sym * p.nl;
  for (int k = 0; k < nk; k++) {
    const uint32_t *q = p.nvar_rows + (size_t)k * p.nrx;
    uint64_t acc = 0;
    int cnt = 0;
    for (int a = 0; a < p.nrx; a++)
      if (q[a] > 0) {
        acc += q[a];
        cnt++;
      }
    nvar += cnt > 0 ? (uint32_t)(acc / (uint64_t)cnt) : 0;
    for (int a = 0; a < 8; a++)
      nvar_ant[a] = a < p.nrx ? q[a] : 0;
  }
  const uint32_t den = p.nvfix ? (uint32_t)(p.n_dmrs_sym * p.nl) : (uint32_t)(p.number_symbols * p.nl * p.nrx);
  const uint32_t nvar_den = den > 0 ? den : 1u;
  nvar /= nvar_den;
  if (p.branch >= 0 && p.branch < 8 && nvar_ant[p.branch] > 0) {
    const uint32_t nvar_branch = (uint32_t)((uint64_t)nvar_ant[p.branch] * (uint64_t)(p.n_dmrs_sym * p.nl) / nvar_den);
    if (nvar_branch > 0)
      nvar = nvar_branch;
  }
  out->nvar = nvar;
}

__device__ __forceinline__ long long red_ll(long long v, long long *sm)
{
  for (int o = 16; o; o >>= 1)
    v += __shfl_xor_sync(0xffffffffu, v, o);
  __syncthreads();
  if ((threadIdx.x & 31) == 0)
    sm[threadIdx.x >> 5] = v;
  __syncthreads();
  long long t = 0;
  for (unsigned i = 0; i < blockDim.x / 32; i++)
    t += sm[i];
  return t;
}
__device__ __forceinline__ float red_f(float v, float *sm)
{
  for (int o = 16; o; o >>= 1)
    v += __shfl_xor_sync(0xffffffffu, v, o);
  __syncthreads();
  if ((threadIdx.x & 31) == 0)
    sm[threadIdx.x >> 5] = v;
  __syncthreads();
  float t = 0;
  for (unsigned i = 0; i < blockDim.x / 32; i++)
    t += sm[i];
  return t;
}

/* Block 0: the DM-RS phase-slope CFO/SFO measurement. re/im/k sums of the CPU are exact integers in double (|h| < 2^15, N <= 4096), so they are
 * accumulated in int64 in any order and are bit-equal. The magnitude sum is order dependent: the FP64 build adds it in k order on two threads
 * (what the CPU does), the float build reduces it in parallel (it only feeds the coherence gate). */
template <class R> __device__ void fo_part(const nr_gpu_chest_post_par_t &p, typename PostOut<R>::T *out)
{
  constexpr bool exact = sizeof(R) == 8;
  const int N = p.N;
  const short2 *h0 = (const short2 *)p.h0, *h1 = (const short2 *)p.h1;
  __shared__ int kmn, kmx;
  __shared__ long long sm[8];
  __shared__ float smf[8];
  __shared__ long long tot[2][4]; /* re, im, ksum, kn */
  __shared__ R magsum[2];
  if (threadIdx.x == 0) {
    kmn = N;
    kmx = 0;
  }
  __syncthreads();
  int lmn = N, lmx = 0;
  for (int k = threadIdx.x; k < N; k += blockDim.x) {
    const short2 a = h0[k], b = h1[k];
    if (a.x != 0 || a.y != 0 || b.x != 0 || b.y != 0) {
      lmn = min(lmn, k);
      lmx = max(lmx, k);
    }
  }
  atomicMin(&kmn, lmn);
  atomicMax(&kmx, lmx);
  __syncthreads();
  const int kmin = kmn, kmax = kmx;
  const int kmid = (kmin <= kmax) ? (kmin + kmax) / 2 : 0;
  long long acc[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
  float mag[2] = {0.f, 0.f};
  for (int k = threadIdx.x; k < N; k += blockDim.x) {
    const short2 a = h0[k], b = h1[k];
    const long long rr = (long long)b.x * a.x + (long long)b.y * a.y;
    const long long ii = (long long)b.y * a.x - (long long)b.x * a.y;
    if (rr == 0 && ii == 0)
      continue;
    const int h = (k <= kmid) ? 0 : 1;
    acc[h][0] += rr;
    acc[h][1] += ii;
    acc[h][2] += k;
    acc[h][3] += 1;
    if constexpr (!exact)
      mag[h] += sqrtf((float)((long long)b.x * b.x + (long long)b.y * b.y) * (float)((long long)a.x * a.x + (long long)a.y * a.y));
  }
  for (int h = 0; h < 2; h++) {
    for (int c = 0; c < 4; c++) {
      const long long t = red_ll(acc[h][c], sm);
      if (threadIdx.x == 0)
        tot[h][c] = t;
    }
    if constexpr (!exact) {
      const float t = red_f(mag[h], smf);
      if (threadIdx.x == 0)
        magsum[h] = (R)t;
    }
  }
  __syncthreads();
  if constexpr (exact)
  if (threadIdx.x < 2) { /* thread h adds its half's sqrt terms in k order, as the CPU loop does */
    const int h = threadIdx.x;
    double s = 0.0;
    for (int k = h ? kmid + 1 : 0; k < (h ? N : kmid + 1); k++) {
      const short2 a = h0[k], b = h1[k];
      const long long rr = (long long)b.x * a.x + (long long)b.y * a.y;
      const long long ii = (long long)b.y * a.x - (long long)b.x * a.y;
      if (rr == 0 && ii == 0)
        continue;
      s += sqrt(((double)b.x * b.x + (double)b.y * b.y) * ((double)a.x * a.x + (double)a.y * a.y));
    }
    magsum[h] = (R)s;
  }
  __syncthreads();
  if (threadIdx.x)
    return;
  const R re[2] = {(R)tot[0][0], (R)tot[1][0]}, im[2] = {(R)tot[0][1], (R)tot[1][1]};
  const R ksum[2] = {(R)tot[0][2], (R)tot[1][2]};
  const int kn[2] = {(int)tot[0][3], (int)tot[1][3]};
  const R PI = (R)3.14159265358979323846;
  const R dt = r_dt<R>(p), df = r_df<R>(p);
  const R coh_num = r_sqrt(r_ssq_b(re[0] + re[1], im[0] + im[1]));
  const R coh_den = magsum[0] + magsum[1];
  const R coherence = (coh_den > (R)0) ? (coh_num / coh_den) : (R)0;
  const R coh_h[2] = {(magsum[0] > (R)0) ? r_sqrt(r_ssq_a(re[0], im[0])) / magsum[0] : (R)0,
                      (magsum[1] > (R)0) ? r_sqrt(r_ssq_a(re[1], im[1])) / magsum[1] : (R)0};
  const R coh_min = (coh_h[0] < coh_h[1]) ? coh_h[0] : coh_h[1];
  const bool halves_usable = (kn[0] >= 8 && kn[1] >= 8);
  const R coh_gate = halves_usable ? coh_min : coherence;
  uint32_t flags = 0;
  R cfo = 0, sfo_ppm = 0, sfo_dk = 0;
  if (kn[0] + kn[1] > 0 && coh_gate < (R)0.30)
    flags |= NR_GPU_POST_FO_REJECTED;
  if (kn[0] + kn[1] > 0 && coh_gate >= (R)0.30) {
    flags |= NR_GPU_POST_FO_VALID;
    if constexpr (sizeof(R) == 8)
      cfo = r_atan2(im[0] + im[1], re[0] + re[1]) / ((R)2.0 * PI * dt);
    else /* the sums added exactly, one rounding into float */
      cfo = atan2f((float)(tot[0][1] + tot[1][1]), (float)(tot[0][0] + tot[1][0])) / ((R)2.0 * PI * dt);
    if (kn[0] > 16 && kn[1] > 16 && coh_h[0] >= (R)0.30 && coh_h[1] >= (R)0.30) {
      R dphi, dk;
      if constexpr (sizeof(R) == 8) { /* the CPU's lines */
        const R p_lo = r_atan2(im[0], re[0]), p_hi = r_atan2(im[1], re[1]);
        const R k_lo = ksum[0] / (R)kn[0], k_hi = ksum[1] / (R)kn[1];
        dphi = p_hi - p_lo;
        while (dphi > PI) dphi -= (R)2.0 * PI;
        while (dphi < -PI) dphi += (R)2.0 * PI;
        dk = k_hi - k_lo;
      } else {
        /* float: arg(b) - arg(a) as one atan2 of the exact cross and dot product of the two half-band sums (int128), and the exact k separation:
         * the difference of two float angles would lose the digits that the small SFO phase lives in */
        const __int128 cr = (__int128)tot[1][1] * tot[0][0] - (__int128)tot[1][0] * tot[0][1];
        const __int128 dt_ = (__int128)tot[1][0] * tot[0][0] + (__int128)tot[1][1] * tot[0][1];
        dphi = atan2f((float)cr, (float)dt_);
        dk = (float)((tot[1][2] * tot[0][3] - tot[0][2] * tot[1][3])) / ((float)tot[0][3] * (float)tot[1][3]);
      }
      if (dk != (R)0) {
        sfo_ppm = -(dphi / dk) / ((R)2.0 * PI * df * dt) * (R)1.0e6;
        sfo_dk = (dk < (R)0) ? -dk : dk;
        flags |= NR_GPU_POST_SFO_MEASURED;
      }
    }
  }
  out->flags = flags;
  out->cfo_hz = cfo;
  out->sfo_ppm = sfo_ppm;
  out->sfo_dk = sfo_dk;
  out->coh_gate = coh_gate;
  out->coherence = coherence;
  out->kn0 = kn[0];
  out->kn1 = kn[1];
  for (int h = 0; h < 2; h++) {
    out->re[h] = tot[h][0];
    out->im[h] = tot[h][1];
    out->ksum[h] = tot[h][2];
    out->magsum[h] = magsum[h];
  }
}

template <class R> __global__ void k_chest_post(nr_gpu_chest_post_par_t p, typename PostOut<R>::T *out)
{
  if (blockIdx.x == 1) {
    nvar_part(p, out);
    return;
  }
  if (threadIdx.x == 0) { /* defaults of the block-0 outputs when there is no measurement */
    out->flags = 0;
    out->cfo_hz = out->sfo_ppm = out->sfo_dk = out->coh_gate = out->coherence = 0;
    out->kn0 = out->kn1 = 0;
    for (int h = 0; h < 2; h++) {
      out->re[h] = out->im[h] = out->ksum[h] = 0;
      out->magsum[h] = 0;
    }
  }
  __syncthreads();
  if (p.h0 && p.h1)
    fo_part<R>(p, out);
}

} // namespace gpupass

extern "C" int nr_gpu_pass_launch_chest_post(const nr_gpu_chest_post_par_t *p, nr_gpu_chest_post_out_t *out_dev, void *stream)
{
  gpupass::k_chest_post<float><<<2, 256, 0, (cudaStream_t)stream>>>(*p, out_dev);
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}
extern "C" int nr_gpu_pass_launch_chest_post_f64(const nr_gpu_chest_post_par_t *p, nr_gpu_chest_post_out64_t *out_dev, void *stream)
{
  gpupass::k_chest_post<double><<<2, 256, 0, (cudaStream_t)stream>>>(*p, out_dev);
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}
extern "C" int nr_gpu_pass_launch_rdm(const int16_t *llr, const nr_gpu_rdm_seg_t *segs, int nseg, int8_t *ldpc_in, void *stream)
{
  if (nseg < 1)
    return 0;
  gpupass::k_rdm<<<nseg, 256, 0, (cudaStream_t)stream>>>(llr, segs, nseg, ldpc_in);
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}
extern "C" int nr_gpu_pass_launch_crc(const uint8_t *bits, const nr_gpu_crc_seg_t *segs, int nseg, uint32_t tbs, uint8_t *seg_ok, uint8_t *tb_ok, void *stream)
{
  if (nseg < 1) {
    gpupass::k_zero<<<1, 1, 0, (cudaStream_t)stream>>>(tb_ok);
    return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
  }
  gpupass::k_crc<<<nseg + (nseg > 1), 256, 0, (cudaStream_t)stream>>>(bits, segs, nseg, tbs, seg_ok, tb_ok);
  if (nseg == 1)
    gpupass::k_crc_tb1<<<1, 1, 0, (cudaStream_t)stream>>>(seg_ok, tb_ok);
  return cudaPeekAtLastError() == cudaSuccess ? 0 : -1;
}
