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
  float *rot, *ph;         // device: rot table, block phasors (sr, si)
  int16_t *out;            // device [nch][2 n/decim]
  double shift, phase;
};

struct Chan {
  double rate;
  int nch, ncell, nmax;
  std::vector<Cell> cell;
  int16_t *d_in, *h_in, *h_out;
  float *h_ph;
  cudaStream_t st;
  cudaEvent_t e0, e1;
  float ms;
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

__global__ void k_out(const float *xr, const float *xi, int stride, int n, int16_t *out)
{
  const int m = blockIdx.x * blockDim.x + threadIdx.x, a = blockIdx.y;
  if (m >= n) return;
  out[(size_t)a * 2 * n + 2 * m] = ck_out(xr[(size_t)a * stride + m]);
  out[(size_t)a * 2 * n + 2 * m + 1] = ck_out(xi[(size_t)a * stride + m]);
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
    cudaFree(c.rot); cudaFree(c.ph); cudaFree(c.out);
  }
  cudaFree(g->d_in); cudaFreeHost(g->h_in); cudaFreeHost(g->h_out); cudaFreeHost(g->h_ph);
  if (g->st) cudaStreamDestroy(g->st);
  if (g->e0) cudaEventDestroy(g->e0);
  if (g->e1) cudaEventDestroy(g->e1);
  delete g;
}

extern "C" void *chan_gpu_create(double rate_in, int nch, int ncell, const iq_ddc_t *const *designs, int nmax)
{
  Chan *g = new Chan();
  g->rate = rate_in; g->nch = nch; g->ncell = ncell; g->nmax = nmax;
  g->cell.assign((size_t)ncell, Cell());
  size_t out_total = 0;
  if (nmax % IQ_DDC_ROT_BLOCK) goto fail;
  CK(cudaStreamCreate(&g->st)); CK(cudaEventCreate(&g->e0)); CK(cudaEventCreate(&g->e1));
  CK(cudaMalloc(&g->d_in, (size_t)nch * nmax * 4));
  CK(cudaMallocHost(&g->h_in, (size_t)nch * nmax * 4));
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
    CK(cudaMalloc(&c.ph, (size_t)(nmax / IQ_DDC_ROT_BLOCK) * 2 * 4));
    CK(cudaMalloc(&c.out, (size_t)nch * (nmax / c.decim) * 4));
    out_total += (size_t)nch * (nmax / c.decim) * 4;
    c.shift = 0; c.phase = 0;
    CK(cudaMemcpy(c.rot, d->rot, 2 * IQ_DDC_ROT_BLOCK * 4, cudaMemcpyHostToDevice));
  }
  CK(cudaMallocHost(&g->h_out, out_total));
  CK(cudaMallocHost(&g->h_ph, (size_t)ncell * (nmax / IQ_DDC_ROT_BLOCK) * 2 * 4));
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

extern "C" int chan_gpu_run(void *g_, const int16_t *const *in, int n, int16_t *const *out)
{
  Chan *g = (Chan *)g_;
  const int nch = g->nch, nblk = n / IQ_DDC_ROT_BLOCK;
  if (n <= 0 || n > g->nmax || n % IQ_DDC_ROT_BLOCK) return -1;
  for (int a = 0; a < nch; ++a) memcpy(g->h_in + (size_t)a * 2 * g->nmax, in[a], (size_t)n * 4);
  // block-start phasors on the host in double, exactly iq_ddc_process's recurrence
  for (int k = 0; k < g->ncell; ++k) {
    Cell &c = g->cell[(size_t)k];
    const double step = -2 * M_PI * c.shift / g->rate;
    float *p = g->h_ph + (size_t)k * (g->nmax / IQ_DDC_ROT_BLOCK) * 2;
    for (int b = 0; b < nblk; ++b) {
      p[2 * b] = (float)cos(c.phase); p[2 * b + 1] = (float)sin(c.phase);
      c.phase = remainder(c.phase + step * IQ_DDC_ROT_BLOCK, 2 * M_PI);
    }
  }
  cudaEventRecord(g->e0, g->st);
  for (int a = 0; a < nch; ++a)
    cudaMemcpyAsync(g->d_in + (size_t)a * 2 * g->nmax, g->h_in + (size_t)a * 2 * g->nmax, (size_t)n * 4, cudaMemcpyHostToDevice, g->st);
  size_t hoff = 0;
  for (int k = 0; k < g->ncell; ++k) {
    Cell &c = g->cell[(size_t)k];
    cudaMemcpyAsync(c.ph, g->h_ph + (size_t)k * (g->nmax / IQ_DDC_ROT_BLOCK) * 2, (size_t)nblk * 8, cudaMemcpyHostToDevice, g->st);
    k_mix<<<grid(n, nch), TPB, 0, g->st>>>(g->d_in, 2 * g->nmax, n, c.rot, c.ph, c.xr[0], c.xi[0], c.stride[0], c.H[0]);
    int ns = n;
    for (int s = 0; s < c.ns; ++s, ns /= 2) {
      k_stage<<<grid(ns / 2, nch), TPB, 0, g->st>>>(c.xr[s], c.xi[s], c.stride[s], ns / 2, c.pair[s], c.np[s], c.c[s], c.xr[s + 1], c.xi[s + 1],
                                                   c.stride[s + 1], c.H[s + 1]);
      k_hist<<<grid(c.H[s], nch), TPB, 0, g->st>>>(c.xr[s], c.xi[s], c.stride[s], ns, c.H[s]);
    }
    k_out<<<grid(ns, nch), TPB, 0, g->st>>>(c.xr[c.ns], c.xi[c.ns], c.stride[c.ns], ns, c.out);
    for (int a = 0; a < nch; ++a) {
      cudaMemcpyAsync((char *)g->h_out + hoff, c.out + (size_t)a * 2 * ns, (size_t)ns * 4, cudaMemcpyDeviceToHost, g->st);
      hoff += (size_t)ns * 4;
    }
  }
  cudaEventRecord(g->e1, g->st);
  if (cudaStreamSynchronize(g->st) != cudaSuccess || cudaGetLastError() != cudaSuccess) return -1;
  cudaEventElapsedTime(&g->ms, g->e0, g->e1);
  hoff = 0;
  for (int k = 0; k < g->ncell; ++k)
    for (int a = 0; a < nch; ++a) {
      const size_t bytes = (size_t)(n / g->cell[(size_t)k].decim) * 4;
      memcpy(out[k * nch + a], (char *)g->h_out + hoff, bytes);
      hoff += bytes;
    }
  return 0;
}

extern "C" float chan_gpu_last_ms(void *g_) { return ((Chan *)g_)->ms; }
