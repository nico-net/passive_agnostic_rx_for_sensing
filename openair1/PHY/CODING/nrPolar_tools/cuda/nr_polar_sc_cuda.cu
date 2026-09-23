/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * See nr_polar_sc_cuda.h. Device side only: one thread block per item walks the op list the
 * host built from the CPU decoder's own tree recursion. One warp per item, dynamic shared sized
 * to the batch, pinned staging, synchronous copies, thread-0 repetition accumulate.
 * ponytail: alpha is laid out (n+1)*N when SC only needs 2N-1 (one active node per level) --
 * that is the remaining occupancy lever (14 blocks/SM today, shared-memory bound), but it
 * reindexes the op list, so do it only with the bit-exactness test in the loop.
 */
#include <cuda_runtime.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "nr_polar_sc_cuda_int.h"

static npc_dev_params_t *g_dp = NULL;
/* per-slot shared-memory footprint, so the launch can size smem to the batch instead of to
 * NPC_MAX_N. AL2 is N=256/n=8, i.e. 7 KB not 16 KB -- and blocks/SM is set by exactly this. */
static int g_ln[NPC_MAX_PARAMS], g_nn[NPC_MAX_PARAMS];

extern "C" int npc_gpu_upload_params(int slot, const npc_dev_params_t *p)
{
  if (!g_dp && cudaMalloc(&g_dp, sizeof(npc_dev_params_t) * NPC_MAX_PARAMS) != cudaSuccess) return -1;
  g_ln[slot] = (p->n + 1) * p->N; g_nn[slot] = p->N;
  return cudaMemcpy(&g_dp[slot], p, sizeof(*p), cudaMemcpyHostToDevice) == cudaSuccess ? 0 : -1;
}

/* ---- exact int16 semantics of the CPU path's simde intrinsics ------------------------------ */
/* 16-BIT OPS WITHOUT EVER FORMING +32768. The decoder depends on INT16_MIN wrapping the way
 * _mm_abs_epi16 / _mm_sign_epi16 wrap: |-32768| = -32768, which is the SMALLEST signed value, so
 * min(|a|,|b|) is -32768 whenever either operand is.
 *
 * Neither PTX abs.s16/min.s16 nor a plain `x == -32768 ? x : -x` survives nvcc here: written
 * either way, abs16(-32768) alone returns -32768, but INLINED into min16 the truncation is
 * dropped and the comparison happens on +32768 in a 32-bit register, so
 * min16(abs16(-32768), 6518) = 6518 and f_op(-32768, 6518) = -6518 instead of -32768. Measured on
 * device, both in the kernel and in a standalone probe (CUDA 12.4, sm_89, -O3). The fix is not to
 * fight the narrowing but to never need it: every intermediate below stays in [-32768, 32767],
 * so dropping a cast cannot change the value.
 *
 * sz matters because applyFtoleft() dispatches on sz & 15 -- >= 16 AVX2, 8 SSE, 4 MMX, all
 * sign_epi16 semantics -- while sz 1 and 2 fall through to its own scalar loop, which composes
 * the sign from the two operands' sign BITS and so has no sign_epi16 "second operand is zero ->
 * result is zero" case. The two disagree exactly at {a,b} = {0, -32768}. */
__device__ __forceinline__ int16_t f_op(int16_t a, int16_t b, int sz)
{
  int m;
  if (a == -32768 || b == -32768) {
    m = -32768;
  } else {
    const int aa = a < 0 ? -a : a, ab = b < 0 ? -b : b;
    m = aa < ab ? aa : ab;
  }
  int negate;
  if (sz & 3) { /* sz == 1 or 2: the CPU's scalar branch */
    negate = ((a < 0) != (b < 0));
  } else { /* simde_*_sign_epi16(minabs, simde_*_sign_epi16(a, b)) */
    if (a == 0 || b == 0) return 0;
    negate = (b > 0) ? (a < 0) : (a > 0 || a == -32768);
  }
  if (negate) m = (m == -32768) ? -32768 : -m;
  return (int16_t)m;
}
/* G when the left child's beta IS initialised: subs(alpha_v[sz+i], sign(alpha_v[i], betal[i])).
 * Fused for the same reason as above -- a separate sign16() would hand subs16() a +32768. */
__device__ __forceinline__ int16_t g_sub(int16_t x, int16_t v, int8_t bl)
{
  int t = (bl == 0) ? 0 : (int)v;
  if (bl < 0) t = (t == -32768) ? -32768 : -t;
  const int r = (int)x - t;
  return (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
}
/* G when the left child's beta is NOT initialised (see the kernel): the CPU takes
 * simde_mm256/mm_adds_epi16 when sz is a multiple of 8, and its own scalar loop otherwise --
 * and that loop clamps the low side at -SHRT_MAX, not -32768. The two disagree by one LSB at
 * the negative rail, which flips hard decisions, so the width has to be honoured. */
__device__ __forceinline__ int16_t adds16(int16_t a, int16_t b, int sz)
{
  const int r = (int)a + (int)b;
  const int lo = (sz & 7) ? -32767 : -32768;
  return r > 32767 ? 32767 : (r < lo ? (int16_t)lo : (int16_t)r);
}

#ifndef NPC_THREADS
#define NPC_THREADS 32
#endif
/* A single-warp block needs no block barrier: the shared accesses are volatile, so __syncwarp()
 * is enough to order them, and it is far cheaper than bar.sync on a ~1500-op serial chain. */
#if NPC_THREADS <= 32
#define NPC_SYNC() __syncwarp()
#else
#define NPC_SYNC() __syncthreads()
#endif
#define NPC_LEVELS 10 /* n <= 9 */

__device__ int *npc_dbg_params_out = NULL;
__device__ int16_t *npc_dbg_alpha_out = NULL;
__device__ int16_t *npc_dbg_trace = NULL; /* [nops][2]: value written at (L-1,fli) and (L-1,fli+sz) */
__global__ void __launch_bounds__(NPC_THREADS)
sc_decode_kernel(const npc_dev_params_t *P, const int *pid, const int16_t *llr, const int *vidx, int estride,
                 uint8_t *u_out, int nstride, int ln, int nmax)
{
  /* ln = max over the batch of (n+1)*N, nmax = max N. Every item indexes alpha/beta as
   * row*N + fli with its OWN N, so a common capacity is all the layout needs. */
  extern __shared__ char npc_smem[];
  int16_t *const alpha = (int16_t *)npc_smem;
  int8_t *const beta = (int8_t *)(npc_smem + 2 * ln);
  uint8_t *const u = (uint8_t *)(beta + ln);
  (void)nmax;
  const int item = blockIdx.x;
  const npc_dev_params_t *p = &P[pid[item]];
  const int N = p->N, n = p->n, E = p->E, tid = threadIdx.x;
  const int16_t *in = llr + (size_t)(vidx ? vidx[item] : item) * estride; /* vidx: items sharing one LLR vector */
  int16_t *root = alpha + n * N;

  /* nr_polar_rate_matching_int16(), i_bil = 0 for DCI */
  const int16_t fill = (p->rm_mode == 2) ? 32767 : 0;
  for (int i = tid; i < N; i += NPC_THREADS) root[i] = fill;
  for (int i = tid; i < (n + 1) * N; i += NPC_THREADS) beta[i] = -1; /* frozen = bit 0 = -1, forever */
  for (int i = tid; i < N; i += NPC_THREADS) u[i] = 0;
  NPC_SYNC();
  if (p->rm_mode == 0) {
    if (tid == 0) /* repetition: several i share one rmp[i]; int16 wrapping add, order-free */
      for (int i = 0; i < E; i++) root[p->rmp[i]] = (int16_t)(root[p->rmp[i]] + in[i]);
  } else {
    for (int i = tid; i < E; i += NPC_THREADS) root[p->rmp[i]] = in[i];
  }
  NPC_SYNC();

  for (int k = 0; k < p->nops; k++) {
    const npc_op_t op = p->ops[k];
    const int L = op.level, fli = op.fli, sz = 1 << (L - 1);
    /* volatile: nvcc otherwise vectorises these int16 loops into SIMD-in-register ops whose
     * behaviour at INT16_MIN differs from the scalar/simde semantics the CPU decoder relies on --
     * measured f_op(-32768, 6518) = -6518 inside the loop vs -32768 for the same primitive called
     * in isolation. Correctness first; revisit only with a measurement. */
    volatile const int16_t *av = alpha + L * N + fli;
    volatile int16_t *ac = alpha + (L - 1) * N + fli; /* children row: [0,sz) left, [sz,2sz) right */
    volatile int8_t *bc = beta + (L - 1) * N + fli;
    volatile int8_t *bv = beta + L * N + fli;
    if (op.code == NPC_OP_F) {
      if (!op.lfrozen) {
        for (int i = tid; i < sz; i += NPC_THREADS) ac[i] = f_op(av[i], av[i + sz], sz);
        NPC_SYNC();
        if (sz == 1 && tid == 0) { const int b = ac[0] <= 0; u[fli] = b; bc[0] = b ? 1 : -1; }
      }
    } else if (op.code == NPC_OP_G) {
      if (!op.rfrozen) {
        /* betaInit is a property of the TREE, not of the data: a node's left child gets its beta
         * only from applyFtoleft (sz == 1) or computeBeta, and BOTH skip an all-frozen child --
         * so for an all-frozen left, `!left->betaInit` is true on every decode, forever, and the
         * CPU takes its saturating-ADD branch. The sign+subs form is NOT equivalent there:
         * subs(x, -(-32768)) = x + 32768 but adds(x, -32768) = x - 32768, opposite signs at the
         * rail. That one case is the whole bit-exactness divergence (worst at AL8, where
         * repetition accumulation drives alpha into the rails even on real encoded vectors). */
        if (op.lfrozen) {
          for (int i = tid; i < sz; i += NPC_THREADS) ac[sz + i] = adds16(av[sz + i], av[i], sz);
        } else {
          for (int i = tid; i < sz; i += NPC_THREADS) ac[sz + i] = g_sub(av[sz + i], av[i], bc[i]);
        }
        NPC_SYNC();
        if (sz == 1 && tid == 0) { const int b = ac[1] <= 0; u[fli + 1] = b; bc[1] = b ? 1 : -1; }
      }
    } else {
      for (int i = tid; i < sz; i += NPC_THREADS) {
        const int8_t bl = bc[i], br = bc[sz + i];
        bv[i] = (bl == br) ? -1 : 1;
        bv[sz + i] = br;
      }
    }
    NPC_SYNC();
    if (npc_dbg_trace && item == 0 && tid == 0) {
      int16_t t0 = 0, t1 = 0; /* sample ONLY what this op writes; a skipped op records 0,0 */
      if (op.code == NPC_OP_F) { if (!op.lfrozen) { t0 = ac[0]; t1 = ac[sz - 1]; } }
      else if (op.code == NPC_OP_G) { if (!op.rfrozen) { t0 = ac[sz]; t1 = ac[2 * sz - 1]; } }
      else { t0 = bv[0]; t1 = bv[2 * sz - 1]; }
      npc_dbg_trace[4 * k] = t0; npc_dbg_trace[4 * k + 1] = t1;
      npc_dbg_trace[4 * k + 2] = av[0]; npc_dbg_trace[4 * k + 3] = av[sz];
    }
    NPC_SYNC();
  }
  for (int i = tid; i < N; i += NPC_THREADS) u_out[(size_t)item * nstride + i] = u[i];
  if (npc_dbg_alpha_out && item == 0)
    for (int i = tid; i < ln; i += NPC_THREADS) npc_dbg_alpha_out[i] = alpha[i];
  if (npc_dbg_params_out && item == 0 && tid == 0) {
    npc_dbg_params_out[0] = N; npc_dbg_params_out[1] = n; npc_dbg_params_out[2] = E;
    npc_dbg_params_out[3] = p->rm_mode; npc_dbg_params_out[4] = p->nops;
    npc_dbg_params_out[5] = p->rmp[0]; npc_dbg_params_out[6] = p->rmp[1];
    npc_dbg_params_out[7] = p->ops[0].code; npc_dbg_params_out[8] = p->ops[0].level; npc_dbg_params_out[9] = p->ops[0].fli;
  }
}

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

extern "C" int npc_gpu_dbg_device_params(int *out10)
{
  int *d = NULL;
  if (cudaMalloc(&d, sizeof(int) * 10) != cudaSuccess) return -1;
  cudaMemcpyToSymbol(npc_dbg_params_out, &d, sizeof(d));
  int16_t *a = NULL;
  if (cudaMalloc(&a, sizeof(int16_t) * NPC_LEVELS * NPC_MAX_N) != cudaSuccess) return -1;
  cudaMemcpyToSymbol(npc_dbg_alpha_out, &a, sizeof(a));
  int16_t *t = NULL;
  if (cudaMalloc(&t, sizeof(int16_t) * 4 * NPC_MAX_OPS) != cudaSuccess) return -1;
  cudaMemcpyToSymbol(npc_dbg_trace, &t, sizeof(t));
  return 0;
}
extern "C" int npc_gpu_verify_params(int slot, const void *host, int nbytes)
{
  void *tmp = malloc(nbytes);
  if (cudaMemcpy(tmp, &g_dp[slot], nbytes, cudaMemcpyDeviceToHost) != cudaSuccess) { free(tmp); return -1; }
  int first = -1;
  for (int i = 0; i < nbytes; i++)
    if (((const unsigned char *)tmp)[i] != ((const unsigned char *)host)[i]) { first = i; break; }
  free(tmp);
  return first;
}
extern "C" int npc_gpu_dbg_trace(int16_t *out)
{
  int16_t *t = NULL;
  cudaMemcpyFromSymbol(&t, npc_dbg_trace, sizeof(t));
  if (!t) return -1;
  return cudaMemcpy(out, t, sizeof(int16_t) * 4 * NPC_MAX_OPS, cudaMemcpyDeviceToHost) == cudaSuccess ? 0 : -1;
}
extern "C" int npc_gpu_dbg_alpha(int16_t *out)
{
  int16_t *a = NULL;
  cudaMemcpyFromSymbol(&a, npc_dbg_alpha_out, sizeof(a));
  if (!a) return -1;
  return cudaMemcpy(out, a, sizeof(int16_t) * NPC_LEVELS * NPC_MAX_N, cudaMemcpyDeviceToHost) == cudaSuccess ? 0 : -1;
}
extern "C" int npc_gpu_dbg_fetch(int *out10)
{
  int *d = NULL;
  cudaMemcpyFromSymbol(&d, npc_dbg_params_out, sizeof(d));
  if (!d) return -1;
  return cudaMemcpy(out10, d, sizeof(int) * 10, cudaMemcpyDeviceToHost) == cudaSuccess ? 0 : -1;
}
extern "C" void *npc_gpu_host_alloc(size_t bytes)
{ void *p = NULL; return cudaHostAlloc(&p, bytes, cudaHostAllocDefault) == cudaSuccess ? p : malloc(bytes); }
extern "C" void npc_gpu_host_free(void *p) { if (p && cudaFreeHost(p) != cudaSuccess) free(p); }

extern "C" int npc_gpu_decode(const int *pid, const int16_t *llr, const int *vidx, int n_vec, int n, int estride,
                              uint8_t *u_out, int nstride, double *t_h2d, double *t_kernel, double *t_d2h)
{
  /* vidx == NULL: item i reads LLR vector i (n_vec ignored). Otherwise only n_vec vectors are
   * uploaded and item i reads vector vidx[i]. */
  static int cap = 0;
  static int *d_pid = NULL, *d_vidx = NULL; static int16_t *d_llr = NULL; static uint8_t *d_u = NULL;
  if (n > cap) {
    cudaFree(d_pid); cudaFree(d_vidx); cudaFree(d_llr); cudaFree(d_u);
    cap = n;
    if (cudaMalloc(&d_pid, sizeof(int) * cap) || cudaMalloc(&d_vidx, sizeof(int) * cap)
        || cudaMalloc(&d_llr, sizeof(int16_t) * NPC_MAX_E * cap) || cudaMalloc(&d_u, (size_t)NPC_MAX_N * cap))
      return -1;
  }
  if (vidx == NULL) n_vec = n;
  if (n_vec > cap) return -1;
  const double t0 = now_us();
  cudaMemcpy(d_pid, pid, sizeof(int) * n, cudaMemcpyHostToDevice);
  if (vidx) cudaMemcpy(d_vidx, vidx, sizeof(int) * n, cudaMemcpyHostToDevice);
  cudaMemcpy(d_llr, llr, sizeof(int16_t) * (size_t)estride * n_vec, cudaMemcpyHostToDevice);
  int ln = 0, nmax = 0;
  for (int i = 0; i < n; i++) { if (g_ln[pid[i]] > ln) ln = g_ln[pid[i]]; if (g_nn[pid[i]] > nmax) nmax = g_nn[pid[i]]; }
  const size_t shbytes = (size_t)3 * ln + nmax;
  const double t1 = now_us();
  sc_decode_kernel<<<n, NPC_THREADS, shbytes>>>(g_dp, d_pid, d_llr, vidx ? d_vidx : NULL, estride, d_u, nstride, ln, nmax);
  if (cudaDeviceSynchronize() != cudaSuccess) return -2;
  const double t2 = now_us();
  cudaMemcpy(u_out, d_u, (size_t)nstride * n, cudaMemcpyDeviceToHost);
  const double t3 = now_us();
  *t_h2d = t1 - t0; *t_kernel = t2 - t1; *t_d2h = t3 - t2;
  return 0;
}
