// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// MPCI G5: GPU backend of the in-process channelizer (see shm_iq_chan_gpu.h, iq_chan.h). Same algorithm as iq_ddc.c per (cell, channel): mixer with a
// per-4096-sample phasor computed on the host in double (iq_ddc's recurrence), Kaiser halfband stages on [history | block] float buffers, round to
// nearest even with clipping. Kernels are the T0 import (MPCIFIX ABI 2: fused mixer + stage 0, grid.y = channels); the host driver is new: it serves
// iq_chan_backend_t (synchronous run() on one top-priority stream of the receiver's own context), reads the input in place on a SHARED tier and stages
// it on a DISCRETE one. Expected difference to the CPU backend: float FMA contraction only, <= 1 LSB on isolated samples (compare()).
// Changes against the import: the history kernels (k_hist, k_hist_mix) read before they write inside one block, so any block length works (the import
// needed n >= the filter history); partial 4096-sample phasor blocks follow iq_ddc (phase advanced by the chunk length); per-channel pointers instead of
// one strided buffer (the ring's channels are separate arrays).
// iq_chan.h pulls in the C11 ring header (shm_iq_ring.h, _Atomic members); this file only uses its types by pointer and its constants, so the
// qualifier is dropped for the C++ view (same size and layout) and the C11 <stdatomic.h> is not included.
#define _STDATOMIC_H
#define _Atomic
extern "C" {
#include "iq_ddc.h"
#include "iq_chan.h"
}
#include "shm_iq_chan_gpu.h"
#undef _Atomic
#include "shm_iq_chan_kern.h"
#include "iq_ddc.h"
#include "nr_gpu_platform.h"
#include "nr_gpu_membudget.h"
#include "nr_gpu_memstat_cuda.cuh"
#include "nr_gpu_green.h"
#include <cuda_runtime.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TPB 256
#define OPT 8 /* outputs per thread (fused first stage and later stages): [Bench, DGX GB10 idle] 1 output/thread spent most of the time in CTA set-up and the barrier; 8 = 3-6x faster */
#define NCH_MAX ((int)SHM_IQ_MAX_CH)
#define NS_MAX IQ_DDC_MAX_STAGES
#define LOGE(...) fprintf(stderr, "IQ_CHAN_GPU " __VA_ARGS__)

struct InP { const int16_t *p[NCH_MAX]; };
struct OutP { int16_t *p[NCH_MAX]; };

// ------------------------------------------------------------------------------------------------ kernels
// one sc16 sample (re, im) in one 32-bit load (the input rows are 4-byte aligned: ring slots and malloc'd blocks)
static __device__ __forceinline__ void ld_mix(const int16_t *x, int i, const float *rot, float sr, float si, float *yr, float *yi)
{
  const short2 v = *(const short2 *)(x + 2 * i);
  ck_mix_ab((float)v.x, (float)v.y, rot, sr, si, yr, yi);
}
// decim 1, shifted: mixer straight to sc16
__global__ void k_mix_out(InP in, int n, const float *rot, const float *ph, OutP out)
{
  const int i = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (i >= n) return;
  const int b = i / IQ_DDC_ROT_BLOCK, nn = i % IQ_DDC_ROT_BLOCK;
  float yr, yi;
  ld_mix(in.p[a], i, rot + 2 * nn, ph[2 * b], ph[2 * b + 1], &yr, &yi);
  *(short2 *)(out.p[a] + 2 * i) = make_short2(ck_out(yr), ck_out(yi));
}

// Fused mixer + first halfband stage: a block of TPB outputs mixes the raw samples it needs into shared memory once and filters from there (import).
// History (the H0 mixed samples before the block) is read from xr0/xi0[0..H0).
__global__ void k_mix_stage0(InP in, int n, const float *rot, const float *ph, const float *xr0, const float *xi0, int stride0, int H0, const float *pair,
                             int np, int c, float *yr, float *yi, int out_stride, int Hout, OutP out, int final)
{
  extern __shared__ float sh[];
  const int a = blockIdx.y, m0 = blockIdx.x * (TPB * OPT), n_out = n / 2;
  const int L = 2 * TPB * OPT + 2 * c;
  float *sr = sh, *si = sh + L;
  const int16_t *x = in.p[a];
  for (int t = threadIdx.x; t < L; t += TPB) {
    const int p = 2 * m0 + t;
    float vr = 0.f, vi = 0.f;
    if (p < H0) { vr = xr0[(size_t)a * stride0 + p]; vi = xi0[(size_t)a * stride0 + p]; }
    else if (p - H0 < n) {
      const int i = p - H0, b = i / IQ_DDC_ROT_BLOCK, nn = i % IQ_DDC_ROT_BLOCK;
      ld_mix(x, i, rot + 2 * nn, ph[2 * b], ph[2 * b + 1], &vr, &vi);
    }
    sr[t] = vr; si[t] = vi;
  }
  __syncthreads();
  for (int q = 0; q < OPT; ++q) {
  const int m = m0 + threadIdx.x + q * TPB;
  if (m >= n_out) return;
  float r, i;
  ck_halfband(sr, si, threadIdx.x + q * TPB, c, np, pair, &r, &i);
  if (final) { *(short2 *)(out.p[a] + 2 * m) = make_short2(ck_out(r), ck_out(i)); } // last stage: straight to sc16 (no float round trip)
  else { yr[(size_t)a * out_stride + Hout + m] = r; yi[(size_t)a * out_stride + Hout + m] = i; }
  }
}

// stage-0 history for the next block: the mixed values of concat [history H0 | block n] at H0 + n - H0 + j = n + j. One CTA per channel, every thread
// reads before any writes (in place, any n).
__global__ void k_hist_mix(InP in, int n, const float *rot, const float *ph, float *xr0, float *xi0, int stride0, int H0)
{
  const int j = threadIdx.x, a = blockIdx.y;
  float vr = 0.f, vi = 0.f;
  if (j < H0) {
    const int p = n + j;
    if (p < H0) { vr = xr0[(size_t)a * stride0 + p]; vi = xi0[(size_t)a * stride0 + p]; }
    else {
      const int i = p - H0, b = i / IQ_DDC_ROT_BLOCK, nn = i % IQ_DDC_ROT_BLOCK;
      ld_mix(in.p[a], i, rot + 2 * nn, ph[2 * b], ph[2 * b + 1], &vr, &vi);
    }
  }
  __syncthreads();
  if (j < H0) { xr0[(size_t)a * stride0 + j] = vr; xi0[(size_t)a * stride0 + j] = vi; }
}

__global__ void k_stage(const float *xr, const float *xi, int in_stride, int n_out, const float *pair, int np, int c, float *yr, float *yi, int out_stride,
                        int Hout, OutP out, int final)
{
  const int a = blockIdx.y;
  for (int q = 0; q < OPT; ++q) {
  const int m = blockIdx.x * (TPB * OPT) + q * TPB + threadIdx.x;
  if (m >= n_out) return;
  float r, i;
  ck_halfband(xr + (size_t)a * in_stride, xi + (size_t)a * in_stride, m, c, np, pair, &r, &i);
  if (final) { *(short2 *)(out.p[a] + 2 * m) = make_short2(ck_out(r), ck_out(i)); }
  else { yr[(size_t)a * out_stride + Hout + m] = r; yi[(size_t)a * out_stride + Hout + m] = i; }
  }
}

// new history = the last H samples of [history | block] (in place, one CTA per channel, read before write)
__global__ void k_hist(float *xr, float *xi, int stride, int n, int H)
{
  const int i = threadIdx.x, a = blockIdx.y;
  float r = 0.f, q = 0.f;
  if (i < H) { r = xr[(size_t)a * stride + n + i]; q = xi[(size_t)a * stride + n + i]; }
  __syncthreads();
  if (i < H) { xr[(size_t)a * stride + i] = r; xi[(size_t)a * stride + i] = q; }
}

static dim3 grid(int n, int nch) { return dim3((unsigned)((n + TPB - 1) / TPB), (unsigned)nch); }
static unsigned hist_threads(int H) { return (unsigned)((H + 31) / 32 * 32); }

// ------------------------------------------------------------------------------------------------ host driver
static int g_place = 0; // 0 = default (see default_place)
static int default_place(void) { return IQ_CHAN_GPU_PLACE_RT; }

static int now_shared(void) { return nr_gpu_platform_shared_direct(); }

struct GCell {
  int decim, ns, nch;
  int H[NS_MAX + 1], c[NS_MAX], np[NS_MAX], stride[NS_MAX + 1];
  float *pair[NS_MAX];
  float *xr[NS_MAX + 1], *xi[NS_MAX + 1];
  float *rot_d, *rot_h;
  float *ph_h, *ph_dev; // ph_dev = mapped pointer of ph_h (SHARED) or a device copy (DISCRETE)
  int16_t *out_d, *out_h; // DISCRETE only
  size_t ob_bytes;        // bytes of out_d / out_h
  int ph_alloc;           // ph_dev is a device allocation (DISCRETE)
  int nblk_max;
  double shift, phase;
  int hist_dirty, rot_dirty;
  size_t bytes_dev, bytes_pin;
  iq_ddc_t design;
};

struct GCtx {
  iq_chan_geom_t g;
  int shared;
  cudaStream_t st;
  cudaEvent_t e0, e1;
  int16_t *d_in, *h_in; // DISCRETE staging
  size_t bytes_dev, bytes_pin;
  GCell *slot[IQ_CHAN_MAX_CELLS];
  int place;
  int demand_set; // the CPU-part demand (1 token) registered with the pool at the first run (the hooks are installed after create)
  iq_chan_gpu_stats_t stats;
};

static double mono_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}

// polled wait (no cudaEventSynchronize in the receive path): spin, then sleep 20 us between polls
static int wait_event(cudaEvent_t e)
{
  const double t0 = mono_ms();
  for (;;) {
    const cudaError_t r = cudaEventQuery(e);
    if (r == cudaSuccess) return 0;
    if (r != cudaErrorNotReady) { LOGE("wait: %s\n", cudaGetErrorString(r)); return -1; }
    if (mono_ms() - t0 > 0.1) { const struct timespec w = {0, 20000}; nanosleep(&w, NULL); }
  }
}

static int fence_ok(void) { return nr_gpu_allocations_allowed(); }

// Core pool (MPCI-CHANPOOL): the CPU side of a run (phasors, launches, DISCRETE staging copies) holds a CHAN lease of the receiver's pool; the GPU wait does
// not. The hooks live in iq_chan.c of the executable (weak: a host without them, e.g. a tool that only loads this module, runs unleased).
extern "C" int iq_chan_lease_acquire(int blocking) __attribute__((weak));
extern "C" void iq_chan_lease_release(int held) __attribute__((weak));
extern "C" void iq_chan_lease_set_demand(int tokens) __attribute__((weak));
static int lease_get(void) { return iq_chan_lease_acquire ? iq_chan_lease_acquire(1) : -1000; }
static void lease_put(int h) { if (iq_chan_lease_release && h != -1000) iq_chan_lease_release(h); }

// GPU budget: the channelizer is small (a few MB) and fixed once its cells are created: it commits its live bytes as module NR_GPU_MEM_CHAN (weight 0)
// and a new allocation is admitted iff everything committed so far plus it fits the budget B (nr_gpu_membudget.h). It does not wait for the shares of
// modules that have not committed yet (they size themselves from what is left, nr_gpu_membudget_headroom counts the committed CHAN bytes).
static pthread_mutex_t g_mb_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t g_mb_live;
static int mb_acquire(size_t bytes)
{
  pthread_mutex_lock(&g_mb_mu);
  const size_t B = nr_gpu_membudget_total(), used = nr_gpu_membudget_committed_total();
  const int ok = B == SIZE_MAX || used + bytes <= B;
  if (ok) { g_mb_live += bytes; nr_gpu_membudget_commit(NR_GPU_MEM_CHAN, g_mb_live, 1.0); }
  pthread_mutex_unlock(&g_mb_mu);
  return ok;
}
static void mb_release(size_t bytes)
{
  pthread_mutex_lock(&g_mb_mu);
  g_mb_live -= bytes < g_mb_live ? bytes : g_mb_live;
  nr_gpu_membudget_commit(NR_GPU_MEM_CHAN, g_mb_live, 1.0);
  pthread_mutex_unlock(&g_mb_mu);
}

static void cell_free(GCell *c)
{
  if (!c) return;
  if (fence_ok()) { // after the RX fence buffers stay resident until exit (platform rule)
    NrMsScope ms(NR_MS_CHAN, NR_MS_SHARED);
    for (int s = 0; s < NS_MAX; ++s) if (c->pair[s]) nr_ms_free(c->pair[s], (size_t)c->np[s] * 4);
    for (int s = 0; s <= NS_MAX; ++s) {
      const size_t b = (size_t)c->nch * c->stride[s] * 4;
      if (c->xr[s]) nr_ms_free(c->xr[s], b);
      if (c->xi[s]) nr_ms_free(c->xi[s], b);
    }
    if (c->rot_d) nr_ms_free(c->rot_d, 2 * IQ_DDC_ROT_BLOCK * 4);
    if (c->rot_h) { cudaFreeHost(c->rot_h); nr_ms_freed(NR_MS_PIN, 2 * IQ_DDC_ROT_BLOCK * 4); }
    if (c->ph_h) { cudaFreeHost(c->ph_h); nr_ms_freed(NR_MS_PIN, (size_t)c->nblk_max * 8); }
    if (c->ph_alloc) nr_ms_free(c->ph_dev, (size_t)c->nblk_max * 8);
    if (c->out_d) nr_ms_free(c->out_d, c->ob_bytes);
    if (c->out_h) { cudaFreeHost(c->out_h); nr_ms_freed(NR_MS_PIN, c->ob_bytes); }
    mb_release(c->bytes_dev + c->bytes_pin);
  }
  iq_ddc_free(&c->design);
  free(c);
}

// bytes of one cell: per stage s the [history | block] float rows (stage 0: history only, the fused kernel never stores the mixed block), the final
// output row, rot table and phasors; DISCRETE adds the device + pinned output.
static void cell_bytes(const iq_ddc_t *d, unsigned nch, int block, int shared, size_t *dev, size_t *pin)
{
  size_t b = 0;
  int n = block;
  for (int s = 0; s <= d->nstages; ++s, n /= 2) {
    const int H = s < d->nstages ? d->st[s].ntaps - 1 : 0;
    const int stride = d->nstages == 0 || s == d->nstages ? 0 : s == 0 ? H : H + n; // the last stage writes sc16 straight to the output
    b += 2 * (size_t)nch * stride * 4;
  }
  for (int s = 0; s < d->nstages; ++s) b += (size_t)d->st[s].npairs * 4;
  b += 2 * IQ_DDC_ROT_BLOCK * 4;
  const size_t nblk = (size_t)(block + IQ_DDC_ROT_BLOCK - 1) / IQ_DDC_ROT_BLOCK;
  size_t p = 2 * IQ_DDC_ROT_BLOCK * 4 + nblk * 8;
  if (!shared) {
    b += nblk * 8 + (size_t)nch * (block / d->decim) * 4;
    p += (size_t)nch * (block / d->decim) * 4;
  }
  *dev = b; *pin = p;
}

static void *g_create(const iq_chan_geom_t *geom);
static void g_destroy(void *ctx);

static void *g_prepare(void *ctx, const iq_chan_entry_t *e)
{
  GCtx *x = (GCtx *)ctx;
  if (!fence_ok()) return NULL;
  GCell *c = (GCell *)calloc(1, sizeof(GCell));
  if (!c) return NULL;
  c->nch = (int)x->g.nch;
  const double atten = e->atten_db > 0 ? e->atten_db : IQ_CHAN_DEFAULT_ATTEN_DB;
  if (iq_ddc_init(&c->design, x->g.in_rate, e->decim, e->passband_hz, atten)) { free(c); return NULL; }
  const iq_ddc_t *d = &c->design;
  c->decim = d->decim; c->ns = d->nstages;
  c->nblk_max = (x->g.block + IQ_DDC_ROT_BLOCK - 1) / IQ_DDC_ROT_BLOCK;
  cell_bytes(d, x->g.nch, x->g.block, x->shared, &c->bytes_dev, &c->bytes_pin);
  if (!mb_acquire(c->bytes_dev + c->bytes_pin)) {
    LOGE("prepare: %zu bytes do not fit the GPU budget\n", c->bytes_dev + c->bytes_pin);
    iq_ddc_free(&c->design); free(c); return NULL;
  }
  NrMsScope ms(NR_MS_CHAN, NR_MS_SHARED);
  const size_t nch = x->g.nch;
  int n = x->g.block;
  bool ok = true;
  for (int s = 0; s <= c->ns && ok; ++s, n /= 2) {
    c->H[s] = s < c->ns ? d->st[s].ntaps - 1 : 0;
    c->stride[s] = c->ns == 0 || s == c->ns ? 0 : s == 0 ? c->H[s] : c->H[s] + n;
    if (!c->stride[s]) continue;
    const size_t b = nch * c->stride[s] * 4;
    ok = nr_ms_malloc(&c->xr[s], b) == cudaSuccess && nr_ms_malloc(&c->xi[s], b) == cudaSuccess && cudaMemset(c->xr[s], 0, b) == cudaSuccess
         && cudaMemset(c->xi[s], 0, b) == cudaSuccess;
  }
  for (int s = 0; s < c->ns && ok; ++s) {
    c->c[s] = (d->st[s].ntaps - 1) / 2; c->np[s] = d->st[s].npairs;
    ok = nr_ms_malloc(&c->pair[s], (size_t)c->np[s] * 4) == cudaSuccess
         && cudaMemcpy(c->pair[s], d->st[s].pair, (size_t)c->np[s] * 4, cudaMemcpyHostToDevice) == cudaSuccess;
  }
  ok = ok && nr_ms_malloc(&c->rot_d, 2 * IQ_DDC_ROT_BLOCK * 4) == cudaSuccess && nr_ms_host_alloc(&c->rot_h, 2 * IQ_DDC_ROT_BLOCK * 4, cudaHostAllocDefault) == cudaSuccess;
  if (ok) {
    memcpy(c->rot_h, d->rot, 2 * IQ_DDC_ROT_BLOCK * 4);
    ok = cudaMemcpy(c->rot_d, c->rot_h, 2 * IQ_DDC_ROT_BLOCK * 4, cudaMemcpyHostToDevice) == cudaSuccess;
  }
  if (ok && x->shared) {
    ok = nr_ms_host_alloc(&c->ph_h, (size_t)c->nblk_max * 8, cudaHostAllocMapped) == cudaSuccess
         && cudaHostGetDevicePointer((void **)&c->ph_dev, c->ph_h, 0) == cudaSuccess;
  } else if (ok) {
    const size_t ob = c->ob_bytes = nch * (size_t)(x->g.block / c->decim) * 4;
    ok = nr_ms_host_alloc(&c->ph_h, (size_t)c->nblk_max * 8, cudaHostAllocDefault) == cudaSuccess && (c->ph_alloc = nr_ms_malloc(&c->ph_dev, (size_t)c->nblk_max * 8) == cudaSuccess)
         && nr_ms_malloc(&c->out_d, ob) == cudaSuccess && nr_ms_host_alloc(&c->out_h, ob, cudaHostAllocDefault) == cudaSuccess;
    if (ok) memset(c->out_h, 0, ob); // pre-fault
  }
  if (!ok) { LOGE("prepare: allocation failed (%s)\n", cudaGetErrorString(cudaGetLastError())); cell_free(c); return NULL; }
  c->shift = 0; c->phase = 0;
  return c;
}

static void g_release(void *ctx, void *cell) { (void)ctx; cell_free((GCell *)cell); }
static void g_install(void *ctx, int slot, void *cell) { ((GCtx *)ctx)->slot[slot] = (GCell *)cell; }
static void *g_retire(void *ctx, int slot)
{
  GCtx *x = (GCtx *)ctx;
  GCell *c = x->slot[slot];
  x->slot[slot] = NULL;
  return c;
}
static void g_set_shift(void *ctx, int slot, double hz)
{
  GCell *c = ((GCtx *)ctx)->slot[slot];
  if (!c) return;
  iq_ddc_set_shift(&c->design, hz); // host table, no allocation; phase kept (as iq_ddc_set_shift)
  c->shift = hz;
  c->rot_dirty = 1;
}
static void g_reset(void *ctx, int slot)
{
  GCtx *x = (GCtx *)ctx;
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s)
    if ((slot < 0 || slot == s) && x->slot[s]) { x->slot[s]->phase = 0; x->slot[s]->hist_dirty = 1; }
}

static int enqueue_cell(GCtx *x, GCell *c, const InP &in, int n, int16_t *const *outp)
{
  cudaStream_t st = x->st;
  const int nch = (int)x->g.nch;
  if (c->rot_dirty) {
    memcpy(c->rot_h, c->design.rot, 2 * IQ_DDC_ROT_BLOCK * 4);
    cudaMemcpyAsync(c->rot_d, c->rot_h, 2 * IQ_DDC_ROT_BLOCK * 4, cudaMemcpyHostToDevice, st);
    c->rot_dirty = 0;
  }
  if (c->hist_dirty) {
    for (int s = 0; s < c->ns; ++s)
      for (int a = 0; a < nch; ++a) {
        cudaMemsetAsync(c->xr[s] + (size_t)a * c->stride[s], 0, (size_t)c->H[s] * 4, st);
        cudaMemsetAsync(c->xi[s] + (size_t)a * c->stride[s], 0, (size_t)c->H[s] * 4, st);
      }
    c->hist_dirty = 0;
  }
  // block-start phasors on the host in double: exactly iq_ddc's mix_split recurrence (chunks of IQ_DDC_ROT_BLOCK from the start of the call)
  const double step = -2 * M_PI * c->shift / x->g.in_rate;
  int nblk = 0;
  for (int n0 = 0; n0 < n; n0 += IQ_DDC_ROT_BLOCK, ++nblk) {
    const int len = n - n0 < IQ_DDC_ROT_BLOCK ? n - n0 : IQ_DDC_ROT_BLOCK;
    c->ph_h[2 * nblk] = (float)cos(c->phase); c->ph_h[2 * nblk + 1] = (float)sin(c->phase);
    c->phase = remainder(c->phase + step * len, 2 * M_PI);
  }
  if (!x->shared) cudaMemcpyAsync(c->ph_dev, c->ph_h, (size_t)nblk * 8, cudaMemcpyHostToDevice, st);
  OutP out;
  const int nout = n / c->decim;
  for (int a = 0; a < nch; ++a) out.p[a] = x->shared ? outp[a] : c->out_d + (size_t)a * 2 * (x->g.block / c->decim);
  if (c->ns == 0) k_mix_out<<<grid(n, nch), TPB, 0, st>>>(in, n, c->rot_d, c->ph_dev, out);
  else {
    const size_t shm = (size_t)2 * (2 * TPB * OPT + 2 * c->c[0]) * sizeof(float);
    k_mix_stage0<<<dim3((unsigned)((n / 2 + TPB * OPT - 1) / (TPB * OPT)), (unsigned)nch), TPB, shm, st>>>(in, n, c->rot_d, c->ph_dev, c->xr[0], c->xi[0], c->stride[0], c->H[0], c->pair[0], c->np[0], c->c[0],
                                                      c->xr[1], c->xi[1], c->stride[1], c->H[1], out, c->ns == 1);
    k_hist_mix<<<dim3(1, nch), hist_threads(c->H[0]), 0, st>>>(in, n, c->rot_d, c->ph_dev, c->xr[0], c->xi[0], c->stride[0], c->H[0]);
    int ns = n / 2;
    for (int s = 1; s < c->ns; ++s, ns /= 2) {
      k_stage<<<dim3((unsigned)((ns / 2 + TPB * OPT - 1) / (TPB * OPT)), (unsigned)nch), TPB, 0, st>>>(c->xr[s], c->xi[s], c->stride[s], ns / 2, c->pair[s], c->np[s], c->c[s], c->xr[s + 1], c->xi[s + 1],
                                                  c->stride[s + 1], c->H[s + 1], out, s + 1 == c->ns);
      k_hist<<<dim3(1, nch), hist_threads(c->H[s]), 0, st>>>(c->xr[s], c->xi[s], c->stride[s], ns, c->H[s]);
    }
  }
  if (!x->shared) {
    const size_t row = (size_t)(x->g.block / c->decim) * 4;
    cudaMemcpy2DAsync(c->out_h, row, c->out_d, row, (size_t)nout * 4, (size_t)nch, cudaMemcpyDeviceToHost, st);
  }
  return 0;
}

static int g_run(void *ctx, const int16_t *const *in, int n, int16_t *const *out)
{
  GCtx *x = (GCtx *)ctx;
  const int nch = (int)x->g.nch;
  if (n <= 0 || n > x->g.block) return -1;
  const double w0 = mono_ms();
  int any = 0;
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) {
    GCell *c = x->slot[s];
    if (!c || !out[s * nch]) continue;
    if (n % c->decim) return -1;
    if (c->decim == 1 && c->shift == 0) { // zero shift, no decimation: the input itself (iq_ddc does the same memcpy)
      for (int a = 0; a < nch; ++a) memcpy(out[s * nch + a], in[a], (size_t)n * 4);
      continue;
    }
    any = 1;
  }
  if (!any) return 0;
  if (!x->demand_set && iq_chan_lease_set_demand) { iq_chan_lease_set_demand(1); x->demand_set = 1; } // iq_chan_stop sends 0
  int lease = lease_get();
  InP ip;
  if (x->shared) for (int a = 0; a < nch; ++a) ip.p[a] = in[a];
  else {
    for (int a = 0; a < nch; ++a) { memcpy(x->h_in + (size_t)a * 2 * x->g.block, in[a], (size_t)n * 4); ip.p[a] = x->d_in + (size_t)a * 2 * x->g.block; }
  }
  cudaEventRecord(x->e0, x->st);
  if (!x->shared) cudaMemcpy2DAsync(x->d_in, (size_t)x->g.block * 4, x->h_in, (size_t)x->g.block * 4, (size_t)n * 4, (size_t)nch, cudaMemcpyHostToDevice, x->st);
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) {
    GCell *c = x->slot[s];
    if (!c || !out[s * nch] || (c->decim == 1 && c->shift == 0)) continue;
    enqueue_cell(x, c, ip, n, out + s * nch);
  }
  cudaEventRecord(x->e1, x->st);
  lease_put(lease); // never across the GPU wait
  if (cudaGetLastError() != cudaSuccess || wait_event(x->e1)) return -1;
  if (!x->shared) {
    lease = lease_get();
    for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) {
      GCell *c = x->slot[s];
      if (!c || !out[s * nch] || (c->decim == 1 && c->shift == 0)) continue;
      const size_t row = (size_t)(x->g.block / c->decim);
      for (int a = 0; a < nch; ++a) memcpy(out[s * nch + a], c->out_h + (size_t)a * 2 * row, (size_t)(n / c->decim) * 4);
    }
    lease_put(lease);
  }
  float ms = 0;
  cudaEventElapsedTime(&ms, x->e0, x->e1);
  x->stats.runs++;
  x->stats.gpu_ms += ms;
  x->stats.wall_ms += mono_ms() - w0;
  return 0;
}

static bool make_stream(GCtx *x)
{
  int lo = 0, hi = 0;
  cudaDeviceGetStreamPriorityRange(&lo, &hi);
  const int idx = x->place == IQ_CHAN_GPU_PLACE_GROUP ? NR_GREEN_PROBE_IDX : NR_GREEN_UL_IDX; // UL index = the context over all real-time SMs
  if (nr_gpu_green_stream(&x->st, hi, true, idx)) return true;
  return cudaStreamCreateWithPriority(&x->st, cudaStreamNonBlocking, hi) == cudaSuccess;
}

static void *g_create(const iq_chan_geom_t *geom)
{
  if (!fence_ok() || !geom || geom->nch < 1 || geom->nch > SHM_IQ_MAX_CH || geom->block <= 0) return NULL;
  const nr_gpu_caps_t *caps = nr_gpu_platform_probe();
  if (!caps || caps->model == NR_GPU_NONE) return NULL;
  GCtx *x = (GCtx *)calloc(1, sizeof(GCtx));
  if (!x) return NULL;
  x->g = *geom;
  x->shared = now_shared();
  x->place = g_place ? g_place : default_place();
  if (!x->shared) {
    x->bytes_dev = x->bytes_pin = (size_t)geom->nch * geom->block * 4;
    if (!mb_acquire(x->bytes_dev + x->bytes_pin)) { free(x); return NULL; }
  }
  bool ok = make_stream(x) && cudaEventCreate(&x->e0) == cudaSuccess && cudaEventCreate(&x->e1) == cudaSuccess;
  if (ok && !x->shared) {
    NrMsScope ms(NR_MS_CHAN, NR_MS_SHARED);
    ok = nr_ms_malloc(&x->d_in, x->bytes_dev) == cudaSuccess && nr_ms_host_alloc(&x->h_in, x->bytes_pin, cudaHostAllocDefault) == cudaSuccess;
    if (ok) memset(x->h_in, 0, x->bytes_pin);
  }
  if (!ok) { LOGE("create failed (%s)\n", cudaGetErrorString(cudaGetLastError())); g_destroy(x); return NULL; }
  return x;
}

static void g_destroy(void *ctx)
{
  GCtx *x = (GCtx *)ctx;
  if (!x) return;
  for (int s = 0; s < IQ_CHAN_MAX_CELLS; ++s) cell_free(x->slot[s]);
  if (fence_ok()) {
    NrMsScope ms(NR_MS_CHAN, NR_MS_SHARED);
    if (x->d_in) nr_ms_free(x->d_in, x->bytes_dev);
    if (x->h_in) { cudaFreeHost(x->h_in); nr_ms_freed(NR_MS_PIN, x->bytes_pin); }
    if (x->e0) cudaEventDestroy(x->e0);
    if (x->e1) cudaEventDestroy(x->e1);
    if (x->st) cudaStreamDestroy(x->st);
    if (!x->shared) mb_release(x->bytes_dev + x->bytes_pin);
  }
  free(x);
}

// ------------------------------------------------------------------------------------------------ bench / compare / sizing
static void load_entries(const iq_chan_load_t *l, iq_chan_entry_t *e)
{
  for (unsigned k = 0; k < l->n_cells; ++k) {
    memset(&e[k], 0, sizeof(e[k]));
    e[k].decim = l->cell[k].decim > 0 ? l->cell[k].decim : 1;
    e[k].passband_hz = l->cell[k].passband_hz > 0 ? l->cell[k].passband_hz : 0.3 * l->in_rate / e[k].decim;
    e[k].shift_hz = l->cell[k].shifted ? 0.1237 * l->in_rate * (k & 1 ? 1 : -1) : 0; // the CPU bench's shifts (iq_chan_cpu_bench_k)
  }
}

static int g_bench(const iq_chan_load_t *l, int block, double seconds, iq_chan_gpu_bench_t *out)
{
  memset(out, 0, sizeof(*out));
  if (!l || !l->n_cells || l->nch < 1 || l->nch > SHM_IQ_MAX_CH) return -1;
  if (block <= 0) block = IQ_CHAN_DEFAULT_BLOCK;
  const iq_chan_geom_t g = {.in_rate = l->in_rate, .nch = l->nch, .block = block};
  GCtx *x = (GCtx *)g_create(&g);
  if (!x) return -1;
  iq_chan_entry_t e[IQ_CHAN_MAX_CELLS];
  load_entries(l, e);
  int16_t *in[SHM_IQ_MAX_CH] = {}, *ob[IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH] = {};
  int rc = -1;
  for (unsigned k = 0; k < l->n_cells; ++k) {
    void *c = g_prepare(x, &e[k]);
    if (!c) goto done;
    g_install(x, (int)k, c);
    g_set_shift(x, (int)k, e[k].shift_hz);
    for (unsigned a = 0; a < l->nch; ++a) if (!(ob[k * l->nch + a] = (int16_t *)malloc((size_t)block / e[k].decim * 4 + 64))) goto done;
  }
  for (unsigned a = 0; a < l->nch; ++a) {
    if (!(in[a] = (int16_t *)malloc((size_t)block * 4))) goto done;
    for (int i = 0; i < 2 * block; ++i) in[a][i] = (int16_t)((rand() & 0x3ff) - 512);
  }
  { const double t0 = mono_ms(); do { if (g_run(x, in, block, ob)) goto done; } while (mono_ms() - t0 < 300); }
  {
    memset(&x->stats, 0, sizeof(x->stats));
    const double t0 = mono_ms();
    long nb = 0;
    while (mono_ms() - t0 < seconds * 1e3 || nb < 4) {
      if (g_run(x, in, block, ob)) goto done;
      ++nb;
    }
    const double wall = mono_ms() - t0;
    out->blocks = (double)nb; out->seconds = wall * 1e-3;
    out->wall_ms_per_block = x->stats.wall_ms / nb; out->gpu_ms_per_block = x->stats.gpu_ms / nb;
    out->samples_per_s = (double)nb * block / (wall * 1e-3);
    out->rt_factor = out->samples_per_s / l->in_rate;
    unsigned m[4] = {};
    if (nr_green_smmask(x->st, m)) out->sms = __builtin_popcount(m[0]) + __builtin_popcount(m[1]) + __builtin_popcount(m[2]) + __builtin_popcount(m[3]);
    cudaGetLastError();
    rc = 0;
  }
done:
  for (unsigned a = 0; a < SHM_IQ_MAX_CH; ++a) free(in[a]);
  for (unsigned k = 0; k < IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH; ++k) free(ob[k]);
  g_destroy(x);
  return rc;
}

static size_t g_capacity(const iq_chan_load_t *load)
{
  iq_chan_gpu_bench_t b;
  return g_bench(load, 0, 0.25, &b) ? 0 : (size_t)b.samples_per_s;
}

static int g_compare(unsigned nch, double rate, int block, int nblocks, const iq_chan_gpu_cell_t *cells, int ncells, const int16_t *const *in,
                     iq_chan_gpu_check_t *res)
{
  memset(res, 0, sizeof(*res));
  if (ncells < 1 || ncells > IQ_CHAN_MAX_CELLS || nch < 1 || nch > SHM_IQ_MAX_CH) return -1;
  const iq_chan_geom_t g = {.in_rate = rate, .nch = nch, .block = block};
  GCtx *x = (GCtx *)g_create(&g);
  if (!x) return -1;
  int rc = -1;
  iq_ddc_t ref[IQ_CHAN_MAX_CELLS][SHM_IQ_MAX_CH];
  memset(ref, 0, sizeof(ref));
  int16_t *go[IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH] = {}, *co = (int16_t *)malloc((size_t)block * 4 + 64);
  for (int k = 0; k < ncells; ++k) {
    iq_chan_entry_t e = {};
    e.decim = cells[k].decim; e.passband_hz = cells[k].passband_hz; e.shift_hz = cells[k].shift_hz;
    void *c = g_prepare(x, &e);
    if (!c) goto done;
    g_install(x, k, c);
    g_set_shift(x, k, cells[k].shift_hz);
    for (unsigned a = 0; a < nch; ++a) {
      if (iq_ddc_init(&ref[k][a], rate, cells[k].decim, cells[k].passband_hz, IQ_CHAN_DEFAULT_ATTEN_DB)) goto done;
      iq_ddc_set_shift(&ref[k][a], cells[k].shift_hz);
      if (!(go[k * nch + a] = (int16_t *)malloc((size_t)block / cells[k].decim * 4 + 64))) goto done;
    }
  }
  if (!co) goto done;
  for (int b = 0; b < nblocks; ++b) {
    const int16_t *blk[SHM_IQ_MAX_CH];
    for (unsigned a = 0; a < nch; ++a) blk[a] = in[a] + 2 * (size_t)b * block;
    if (g_run(x, blk, block, go)) goto done;
    for (int k = 0; k < ncells; ++k)
      for (unsigned a = 0; a < nch; ++a) {
        const int no = iq_ddc_process(&ref[k][a], blk[a], block, co);
        for (int i = 0; i < 2 * no; ++i) {
          const long d = labs((long)go[k * nch + a][i] - co[i]);
          if (d > res->max_lsb) res->max_lsb = d;
          res->differing += d != 0;
          res->total++;
        }
      }
  }
  res->ok = res->max_lsb <= 1 && res->differing * 1000 <= res->total;
  rc = 0;
done:
  for (int k = 0; k < IQ_CHAN_MAX_CELLS; ++k) for (unsigned a = 0; a < SHM_IQ_MAX_CH; ++a) iq_ddc_free(&ref[k][a]);
  for (unsigned k = 0; k < IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH; ++k) free(go[k]);
  free(co);
  g_destroy(x);
  return rc;
}

static size_t g_bytes(const iq_chan_load_t *l, int block)
{
  if (!l) return 0;
  if (block <= 0) block = IQ_CHAN_DEFAULT_BLOCK;
  const int shared = now_shared();
  size_t t = shared ? 0 : 2 * (size_t)l->nch * block * 4;
  iq_chan_entry_t e[IQ_CHAN_MAX_CELLS];
  load_entries(l, e);
  for (unsigned k = 0; k < l->n_cells; ++k) {
    iq_ddc_t d;
    if (iq_ddc_init(&d, l->in_rate, e[k].decim, e[k].passband_hz, IQ_CHAN_DEFAULT_ATTEN_DB)) continue;
    size_t dv, pn;
    cell_bytes(&d, l->nch, block, shared, &dv, &pn);
    t += dv + pn;
    iq_ddc_free(&d);
  }
  return t;
}

static int g_available(void)
{
  int n = 0;
  const nr_gpu_caps_t *caps = nr_gpu_platform_probe();
  return caps && caps->model != NR_GPU_NONE && cudaGetDeviceCount(&n) == cudaSuccess && n > 0;
}
static int g_tier_shared(void) { return now_shared(); }
static void g_set_placement(int p) { g_place = p == IQ_CHAN_GPU_PLACE_RT || p == IQ_CHAN_GPU_PLACE_GROUP ? p : 0; }
static int g_placement(void) { return g_place ? g_place : default_place(); }
static void g_stats(void *ctx, iq_chan_gpu_stats_t *st) { *st = ctx ? ((GCtx *)ctx)->stats : iq_chan_gpu_stats_t{}; }

// about 1 ms of signal per run (smallest power of two >= rate x 1 ms, at least the CPU default): every run pays a few launches and one polled wait;
// [Bench, DGX GB10 idle]: 4 cells at 245.76 MS/s x 4 went from 1.3x (65536) to about 2x real time (262144); the cost is about 1 ms more latency to the cells.
static int g_block_hint(double rate)
{
  int b = IQ_CHAN_DEFAULT_BLOCK;
  while (b < (1 << 24) && (double)b < rate * 1e-3) b *= 2;
  return b;
}

static int g_direct_in(void *ctx) { return ctx && ((GCtx *)ctx)->shared; } // SHARED: the kernels read the wideband ring in place

static const iq_chan_backend_t g_backend = {
  "gpu", g_create, g_destroy, g_prepare, g_release, g_install, g_retire, g_set_shift, g_reset, g_run, g_capacity, NULL, g_block_hint, g_direct_in,
};
static const iq_chan_gpu_api_t g_api = {IQ_CHAN_GPU_ABI, &g_backend, g_available, g_tier_shared, g_compare, g_bytes, g_bench, g_set_placement, g_placement, g_stats};

extern "C" __attribute__((visibility("default"))) const iq_chan_gpu_api_t *iq_chan_gpu_api(void) { return &g_api; }
