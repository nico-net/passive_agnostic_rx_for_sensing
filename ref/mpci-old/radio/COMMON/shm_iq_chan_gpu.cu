// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// GPU backend of the shared channelizer (see shm_iq_chan.c, shm_iq_chan_gpu.h). Same algorithm as iq_ddc.c per (cell, channel):
// mixer with a per-4096-block phasor from double, Kaiser halfband stages on [history | block] float buffers, round-to-nearest-even
// with clipping. One stream, kernels batched over channels (grid.y) per cell and stage. Expected difference to the CPU backend:
// float FMA contraction only, <= 1 LSB of the int16 output on isolated samples (the selftest checks this).
#include "shm_iq_chan_gpu.h"
#include "shm_iq_chan_kern.h"
#include <cuda_runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "SHMIQ_CHAN_GPU %s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); goto fail; } } while (0)
#define TPB 256

struct Cell {
  int decim, ns, H[IQ_DDC_MAX_STAGES + 1], c[IQ_DDC_MAX_STAGES], np[IQ_DDC_MAX_STAGES];
  float *pair[IQ_DDC_MAX_STAGES];
  float *xr[IQ_DDC_MAX_STAGES + 1], *xi[IQ_DDC_MAX_STAGES + 1]; // stage s input [nch][H_s + n_s]; [ns] = final output [nch][n_ns]
  int stride[IQ_DDC_MAX_STAGES + 1];
  float *rot, *ph[CHAN_GPU_SLOTS]; // device: rot table, block phasors (sr, si) per slot
  int16_t *out[CHAN_GPU_SLOTS];    // device [nch][2 nmax/decim] (row stride of the full block)
  size_t hoff;             // byte offset of this cell in a slot's pinned output
  double shift, phase;
};

struct Chan {
  double rate;
  int nch, ncell, nmax;
  std::vector<Cell> cell;
  int16_t *d_in[CHAN_GPU_SLOTS], *h_in[CHAN_GPU_SLOTS], *h_out[CHAN_GPU_SLOTS];
  float *h_ph[CHAN_GPU_SLOTS];
  cudaStream_t st;
  cudaEvent_t e0[CHAN_GPU_SLOTS], e1[CHAN_GPU_SLOTS];
  float ms[CHAN_GPU_SLOTS];
  double last_ms;
  int fuse; // fused mixer + stage 0 (default on; ISAC_SHM_IQ_CHAN_GPU_FUSE=0 = the original kernels)
};

__global__ void k_mix(const int16_t *in, int in_stride, int n, const float *rot, const float *ph, float *xr, float *xi, int stride, int H0)
{
  const int i = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (i >= n) return;
  const int b = i / IQ_DDC_ROT_BLOCK, nn = i % IQ_DDC_ROT_BLOCK;
  float yr, yi;
  ck_mix(in + (size_t)a * in_stride + 2 * i, rot + 2 * nn, ph[2 * b], ph[2 * b + 1], &yr, &yi);
  xr[(size_t)a * stride + H0 + i] = yr; xi[(size_t)a * stride + H0 + i] = yi;
}

__global__ void k_stage(const float *xr, const float *xi, int in_stride, int n_out, const float *pair, int np, int c, float *yr, float *yi,
                        int out_stride, int Hout)
{
  const int m = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (m >= n_out) return;
  float r, i;
  ck_halfband(xr + (size_t)a * in_stride, xi + (size_t)a * in_stride, m, c, np, pair, &r, &i);
  yr[(size_t)a * out_stride + Hout + m] = r; yi[(size_t)a * out_stride + Hout + m] = i;
}

// new history = last H samples of [history | block]; n >= H is guaranteed by the block size (>= 16384 > 1022)
__global__ void k_hist(float *xr, float *xi, int stride, int n, int H)
{
  const int i = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (i >= H) return;
  xr[(size_t)a * stride + i] = xr[(size_t)a * stride + n + i];
  xi[(size_t)a * stride + i] = xi[(size_t)a * stride + n + i];
}

__global__ void k_out(const float *xr, const float *xi, int stride, int n, int16_t *out, int out_stride)
{
  const int m = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (m >= n) return;
  out[(size_t)a * 2 * out_stride + 2 * m] = ck_out(xr[(size_t)a * stride + m]);
  out[(size_t)a * 2 * out_stride + 2 * m + 1] = ck_out(xi[(size_t)a * stride + m]);
}


// Fused mixer + first halfband stage (decim >= 2): a block of TPB outputs mixes the raw samples it needs into shared memory once and
// filters from there, so the mixed float samples never make a round trip through device memory (the unfused path writes 8 bytes and
// reads them back about 3 times per input sample; the GB10 LPDDR is the limit). Same expressions as k_mix + k_stage, in the same order,
// so the result is bit-identical to them. History (the H0 mixed samples before the block) is read from xr0/xi0[0..H0).
__global__ void k_mix_stage0(const int16_t *in, int in_stride, int n, const float *rot, const float *ph, const float *xr0, const float *xi0, int stride0,
                             int H0, const float *pair, int np, int c, float *yr, float *yi, int out_stride, int Hout)
{
  extern __shared__ float sh[];
  const int a = blockIdx.y, m0 = blockIdx.x * TPB, n_out = n / 2;
  const int L = 2 * TPB + 2 * c; // concat samples 2*m0 .. 2*m0 + 2*TPB - 1 + 2c
  float *sr = sh, *si = sh + L;
  for (int t = threadIdx.x; t < L; t += TPB) {
    const int p = 2 * m0 + t; // concat index; < H0 = history, else raw sample p - H0
    float vr = 0.f, vi = 0.f;
    if (p < H0) { vr = xr0[(size_t)a * stride0 + p]; vi = xi0[(size_t)a * stride0 + p]; }
    else if (p - H0 < n) {
      const int i = p - H0, b = i / IQ_DDC_ROT_BLOCK, nn = i % IQ_DDC_ROT_BLOCK;
      ck_mix(in + (size_t)a * in_stride + 2 * i, rot + 2 * nn, ph[2 * b], ph[2 * b + 1], &vr, &vi);
    }
    sr[t] = vr; si[t] = vi;
  }
  __syncthreads();
  const int m = m0 + threadIdx.x;
  if (m >= n_out) return;
  float r, i;
  ck_halfband(sr, si, threadIdx.x, c, np, pair, &r, &i);
  yr[(size_t)a * out_stride + Hout + m] = r; yi[(size_t)a * out_stride + Hout + m] = i;
}

// stage-0 history for the next block: the mixed values of the last H0 raw samples of this block (n >= H0)
__global__ void k_hist_mix(const int16_t *in, int in_stride, int n, const float *rot, const float *ph, float *xr0, float *xi0, int stride0, int H0)
{
  const int j = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (j >= H0) return;
  const int i = n - H0 + j, b = i / IQ_DDC_ROT_BLOCK, nn = i % IQ_DDC_ROT_BLOCK;
  float yr, yi;
  ck_mix(in + (size_t)a * in_stride + 2 * i, rot + 2 * nn, ph[2 * b], ph[2 * b + 1], &yr, &yi);
  xr0[(size_t)a * stride0 + j] = yr; xi0[(size_t)a * stride0 + j] = yi;
}

// decim 1: mixer straight to sc16
__global__ void k_mix_out(const int16_t *in, int in_stride, int n, const float *rot, const float *ph, int16_t *out, int out_stride)
{
  const int i = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (i >= n) return;
  const int b = i / IQ_DDC_ROT_BLOCK, nn = i % IQ_DDC_ROT_BLOCK;
  float yr, yi;
  ck_mix(in + (size_t)a * in_stride + 2 * i, rot + 2 * nn, ph[2 * b], ph[2 * b + 1], &yr, &yi);
  out[(size_t)a * 2 * out_stride + 2 * i] = ck_out(yr);
  out[(size_t)a * 2 * out_stride + 2 * i + 1] = ck_out(yi);
}

static dim3 grid(int n, int nch) { return dim3((unsigned)((n + TPB - 1) / TPB), (unsigned)nch); }

extern "C" int chan_gpu_abi(void) { return CHAN_GPU_ABI; }

extern "C" void chan_gpu_destroy(void *g_)
{
  Chan *g = (Chan *)g_;
  if (!g) return;
  for (Cell &c : g->cell) {
    for (int s = 0; s < IQ_DDC_MAX_STAGES; ++s) cudaFree(c.pair[s]);
    for (int s = 0; s <= IQ_DDC_MAX_STAGES; ++s) { cudaFree(c.xr[s]); cudaFree(c.xi[s]); }
    cudaFree(c.rot);
    for (int k = 0; k < CHAN_GPU_SLOTS; ++k) { cudaFree(c.ph[k]); cudaFree(c.out[k]); }
  }
  for (int k = 0; k < CHAN_GPU_SLOTS; ++k) {
    cudaFree(g->d_in[k]); cudaFreeHost(g->h_in[k]); cudaFreeHost(g->h_out[k]); cudaFreeHost(g->h_ph[k]);
    if (g->e0[k]) cudaEventDestroy(g->e0[k]);
    if (g->e1[k]) cudaEventDestroy(g->e1[k]);
  }
  if (g->st) cudaStreamDestroy(g->st);
  delete g;
}

extern "C" void *chan_gpu_create(double rate_in, int nch, int ncell, const iq_ddc_t *const *designs, int nmax)
{
  Chan *g = new Chan();
  { const char *e = getenv("ISAC_SHM_IQ_CHAN_GPU_FUSE"); g->fuse = !(e && atoi(e) == 0); }
  g->rate = rate_in; g->nch = nch; g->ncell = ncell; g->nmax = nmax;
  g->cell.assign((size_t)ncell, Cell());
  size_t out_total = 0;
  if (nmax % IQ_DDC_ROT_BLOCK) goto fail;
  { // the channelizer is real-time work: greatest stream priority (orders it ahead of this process's own pending blocks only; other
    // processes' contexts are time-sliced by the driver, see the report)
    int lo = 0, hi = 0;
    cudaDeviceGetStreamPriorityRange(&lo, &hi);
    CK(cudaStreamCreateWithPriority(&g->st, cudaStreamNonBlocking, hi));
  }
  for (int k = 0; k < CHAN_GPU_SLOTS; ++k) {
    CK(cudaEventCreate(&g->e0[k])); CK(cudaEventCreate(&g->e1[k]));
    CK(cudaMalloc(&g->d_in[k], (size_t)nch * nmax * 4));
    CK(cudaMallocHost(&g->h_in[k], (size_t)nch * nmax * 4));
    memset(g->h_in[k], 0, (size_t)nch * nmax * 4); // pre-fault
  }
  for (int k = 0; k < ncell; ++k) {
    Cell &c = g->cell[(size_t)k];
    const iq_ddc_t *d = designs[k];
    c.decim = d->decim; c.ns = d->nstages;
    int n = nmax;
    for (int s = 0; s <= c.ns; ++s, n /= 2) {
      c.H[s] = s < c.ns ? d->st[s].ntaps - 1 : 0;
      c.stride[s] = c.H[s] + n;
      CK(cudaMalloc(&c.xr[s], (size_t)nch * c.stride[s] * 4)); CK(cudaMalloc(&c.xi[s], (size_t)nch * c.stride[s] * 4));
      CK(cudaMemset(c.xr[s], 0, (size_t)nch * c.stride[s] * 4)); CK(cudaMemset(c.xi[s], 0, (size_t)nch * c.stride[s] * 4));
    }
    for (int s = 0; s < c.ns; ++s) {
      c.c[s] = (d->st[s].ntaps - 1) / 2; c.np[s] = d->st[s].npairs;
      CK(cudaMalloc(&c.pair[s], (size_t)c.np[s] * 4));
      CK(cudaMemcpy(c.pair[s], d->st[s].pair, (size_t)c.np[s] * 4, cudaMemcpyHostToDevice));
    }
    CK(cudaMalloc(&c.rot, 2 * IQ_DDC_ROT_BLOCK * 4));
    for (int q = 0; q < CHAN_GPU_SLOTS; ++q) {
      CK(cudaMalloc(&c.ph[q], (size_t)(nmax / IQ_DDC_ROT_BLOCK) * 2 * 4));
      CK(cudaMalloc(&c.out[q], (size_t)nch * (nmax / c.decim) * 4));
    }
    c.hoff = out_total;
    out_total += (size_t)nch * (nmax / c.decim) * 4;
    c.shift = 0; c.phase = 0;
    CK(cudaMemcpy(c.rot, d->rot, 2 * IQ_DDC_ROT_BLOCK * 4, cudaMemcpyHostToDevice));
  }
  for (int q = 0; q < CHAN_GPU_SLOTS; ++q) {
    CK(cudaMallocHost(&g->h_out[q], out_total));
    memset(g->h_out[q], 0, out_total);
    CK(cudaMallocHost(&g->h_ph[q], (size_t)ncell * (nmax / IQ_DDC_ROT_BLOCK) * 2 * 4));
  }
  for (int k = 0; k < ncell; ++k) g->cell[(size_t)k].shift = designs[k]->shift_hz;
  return g;
fail:
  chan_gpu_destroy(g);
  return NULL;
}

extern "C" int chan_gpu_set_shift(void *g_, int cell, const iq_ddc_t *d)
{
  Chan *g = (Chan *)g_;
  g->cell[(size_t)cell].shift = d->shift_hz;
  return cudaMemcpy(g->cell[(size_t)cell].rot, d->rot, 2 * IQ_DDC_ROT_BLOCK * 4, cudaMemcpyHostToDevice) == cudaSuccess ? 0 : -1;
}

extern "C" void chan_gpu_reset(void *g_)
{
  Chan *g = (Chan *)g_;
  cudaStreamSynchronize(g->st);
  for (Cell &c : g->cell) {
    c.phase = 0;
    for (int s = 0; s < c.ns; ++s)
      for (int a = 0; a < g->nch; ++a) {
        cudaMemset(c.xr[s] + (size_t)a * c.stride[s], 0, (size_t)c.H[s] * 4);
        cudaMemset(c.xi[s] + (size_t)a * c.stride[s], 0, (size_t)c.H[s] * 4);
      }
  }
}

extern "C" int16_t *chan_gpu_in(void *g_, int slot, int ch) { Chan *g = (Chan *)g_; return g->h_in[slot] + (size_t)ch * 2 * g->nmax; }

extern "C" int16_t *chan_gpu_out(void *g_, int slot, int cell, int ch)
{
  Chan *g = (Chan *)g_;
  const Cell &c = g->cell[(size_t)cell];
  return (int16_t *)((char *)g->h_out[slot] + c.hoff) + (size_t)ch * 2 * (g->nmax / c.decim);
}

// Enqueue block `slot` (n samples already in chan_gpu_in(slot, ch)); returns without waiting.
extern "C" int chan_gpu_submit(void *g_, int slot, int n)
{
  Chan *g = (Chan *)g_;
  const int nch = g->nch, nblk = n / IQ_DDC_ROT_BLOCK;
  if (slot < 0 || slot >= CHAN_GPU_SLOTS || n <= 0 || n > g->nmax || n % IQ_DDC_ROT_BLOCK) return -1;
  // block-start phasors on the host in double, exactly iq_ddc_process's recurrence
  for (int k = 0; k < g->ncell; ++k) {
    Cell &c = g->cell[(size_t)k];
    const double step = -2 * M_PI * c.shift / g->rate;
    float *p = g->h_ph[slot] + (size_t)k * (g->nmax / IQ_DDC_ROT_BLOCK) * 2;
    for (int b = 0; b < nblk; ++b) {
      p[2 * b] = (float)cos(c.phase); p[2 * b + 1] = (float)sin(c.phase);
      c.phase = remainder(c.phase + step * IQ_DDC_ROT_BLOCK, 2 * M_PI);
    }
  }
  cudaEventRecord(g->e0[slot], g->st);
  // one copy per call, not per channel/cell: at a 0.27 ms block every API call (~2-5 us) counts
  cudaMemcpy2DAsync(g->d_in[slot], (size_t)g->nmax * 4, g->h_in[slot], (size_t)g->nmax * 4, (size_t)n * 4, (size_t)nch, cudaMemcpyHostToDevice, g->st);
  for (int k = 0; k < g->ncell; ++k) {
    Cell &c = g->cell[(size_t)k];
    cudaMemcpyAsync(c.ph[slot], g->h_ph[slot] + (size_t)k * (g->nmax / IQ_DDC_ROT_BLOCK) * 2, (size_t)nblk * 8, cudaMemcpyHostToDevice, g->st);
    const int ostr = g->nmax / c.decim;
    if (c.ns == 0) k_mix_out<<<grid(n, nch), TPB, 0, g->st>>>(g->d_in[slot], 2 * g->nmax, n, c.rot, c.ph[slot], c.out[slot], ostr);
    else {
      int ns = n;
      if (g->fuse) { // fused mixer + stage 0, then the stage-0 history from the raw tail
        const size_t shm = (size_t)2 * (2 * TPB + 2 * c.c[0]) * sizeof(float);
        k_mix_stage0<<<grid(n / 2, nch), TPB, shm, g->st>>>(g->d_in[slot], 2 * g->nmax, n, c.rot, c.ph[slot], c.xr[0], c.xi[0], c.stride[0], c.H[0],
                                                         c.pair[0], c.np[0], c.c[0], c.xr[1], c.xi[1], c.stride[1], c.H[1]);
        k_hist_mix<<<grid(c.H[0], nch), TPB, 0, g->st>>>(g->d_in[slot], 2 * g->nmax, n, c.rot, c.ph[slot], c.xr[0], c.xi[0], c.stride[0], c.H[0]);
        ns = n / 2;
        for (int s = 1; s < c.ns; ++s, ns /= 2) {
          k_stage<<<grid(ns / 2, nch), TPB, 0, g->st>>>(c.xr[s], c.xi[s], c.stride[s], ns / 2, c.pair[s], c.np[s], c.c[s], c.xr[s + 1], c.xi[s + 1],
                                                       c.stride[s + 1], c.H[s + 1]);
          k_hist<<<grid(c.H[s], nch), TPB, 0, g->st>>>(c.xr[s], c.xi[s], c.stride[s], ns, c.H[s]);
        }
      } else {
        k_mix<<<grid(n, nch), TPB, 0, g->st>>>(g->d_in[slot], 2 * g->nmax, n, c.rot, c.ph[slot], c.xr[0], c.xi[0], c.stride[0], c.H[0]);
        for (int s = 0; s < c.ns; ++s, ns /= 2) {
          k_stage<<<grid(ns / 2, nch), TPB, 0, g->st>>>(c.xr[s], c.xi[s], c.stride[s], ns / 2, c.pair[s], c.np[s], c.c[s], c.xr[s + 1], c.xi[s + 1],
                                                       c.stride[s + 1], c.H[s + 1]);
          k_hist<<<grid(c.H[s], nch), TPB, 0, g->st>>>(c.xr[s], c.xi[s], c.stride[s], ns, c.H[s]);
        }
      }
      k_out<<<grid(ns, nch), TPB, 0, g->st>>>(c.xr[c.ns], c.xi[c.ns], c.stride[c.ns], ns, c.out[slot], ostr);
    }
    cudaMemcpy2DAsync((char *)g->h_out[slot] + c.hoff, (size_t)ostr * 4, c.out[slot], (size_t)ostr * 4, (size_t)(n / c.decim) * 4, (size_t)nch, cudaMemcpyDeviceToHost, g->st);
  }
  cudaEventRecord(g->e1[slot], g->st);
  return cudaGetLastError() == cudaSuccess ? 0 : -1;
}

extern "C" int chan_gpu_wait(void *g_, int slot)
{
  Chan *g = (Chan *)g_;
  if (cudaEventSynchronize(g->e1[slot]) != cudaSuccess || cudaGetLastError() != cudaSuccess) return -1;
  cudaEventElapsedTime(&g->ms[slot], g->e0[slot], g->e1[slot]);
  g->last_ms = g->ms[slot];
  return 0;
}

// Blocking v1 call (selftest, bench): copy in, submit + wait on slot 0, copy out.
extern "C" int chan_gpu_run(void *g_, const int16_t *const *in, int n, int16_t *const *out)
{
  Chan *g = (Chan *)g_;
  for (int a = 0; a < g->nch; ++a) memcpy(chan_gpu_in(g, 0, a), in[a], (size_t)n * 4);
  if (chan_gpu_submit(g, 0, n) || chan_gpu_wait(g, 0)) return -1;
  for (int k = 0; k < g->ncell; ++k)
    for (int a = 0; a < g->nch; ++a) memcpy(out[k * g->nch + a], chan_gpu_out(g, 0, k, a), (size_t)(n / g->cell[(size_t)k].decim) * 4);
  return 0;
}

extern "C" float chan_gpu_last_ms(void *g_) { return (float)((Chan *)g_)->last_ms; }
