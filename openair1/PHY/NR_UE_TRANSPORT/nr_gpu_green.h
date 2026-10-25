/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_GREEN_H
#define NR_GPU_GREEN_H
/* GPUSHARE SM partition with CUDA green contexts: background discovery (CSI-RS discovery, PDCCH NID) streams live in a green
 * context on the first max(1, SMs/6) SMs of the device (8 of 48 on the GB10), rounded up by the driver to its co-scheduling granularity; real-time streams (PDSCH FEP/chest/
 * demod/LDPC, PDCCH FEP) live in green contexts on the remaining SMs, so discovery blocks never share an SM with them.
 * Header-only, CUDA sources only; driver entry points come from cudaGetDriverEntryPointByVersion (no -lcuda). Each
 * translation unit makes its own green contexts over the same split. Green-context streams share the primary context's
 * memory, events and modules. On any failure the caller creates its stream the usual way. */
#include <cuda_runtime.h>
#include <cuda.h>
#include "../TOOLS/nr_gpu_compat.h"
#include <cstdio>
#include <cstdlib>

/* SM masks (bit per %smid, up to 128 SMs) of the SMs a full-occupancy grid on stream s lands on. false on any CUDA error. */
__global__ static void nr_green_where_kernel(unsigned *m)
{
  unsigned id;
  asm("mov.u32 %0, %%smid;" : "=r"(id));
  if (threadIdx.x == 0 && id < 128) atomicOr(&m[id / 32], 1u << (id % 32));
}
static inline bool nr_green_smmask(cudaStream_t s, unsigned *m)
{
  int sms = 0;
  unsigned *d = nullptr;
  if (cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, 0) != cudaSuccess || cudaMalloc(&d, 4 * sizeof(unsigned)) != cudaSuccess) return false;
  cudaMemsetAsync(d, 0, 4 * sizeof(unsigned), s);
  nr_green_where_kernel<<<64 * sms, 128, 0, s>>>(d);
  const bool ok = cudaMemcpyAsync(m, d, 4 * sizeof(unsigned), cudaMemcpyDeviceToHost, s) == cudaSuccess && cudaStreamSynchronize(s) == cudaSuccess;
  cudaFree(d);
  return ok;
}
/* nr_gpu_green_stream: creates *s (non-blocking, given priority) in this translation unit's discovery (rt = false) or
 * real-time (rt = true) green context; returns true on success, false when
 * unavailable. The contexts are created once, on first use, and never destroyed. */
/* stream idx of the UL / probe real-time context (idx below it: the DL real-time context) */
#define NR_GREEN_UL_IDX 100
/* stream idx of the probe pool (layout probes, the lowest-value work): their own small context */
#define NR_GREEN_PROBE_IDX 200
static inline bool nr_gpu_green_stream(cudaStream_t *s, int priority, bool rt, int idx = 0)
{
#if CUDART_VERSION >= 12050
  static int state = -1; /* -1 not read, 0 off, 1 on */
  static int sms = 8; /* discovery SMs requested: max(1, device SMs / 6), set at the first call; the driver rounds up and the result is read back */
  static CUgreenCtx green[2], rgreen[8];
  static bool made[2], rmade[8];
  static CUdevResource part[2], all;
  static unsigned ngrp, ul_first, ul_n, dl_first, dl_n, pr_n;
  static CUdevResource rgrp[16];
  static CUdevice dev;
  static CUresult (*desc_gen)(CUdevResourceDesc *, CUdevResource *, unsigned);
  static CUresult (*ctx_create)(CUgreenCtx *, CUdevResourceDesc, CUdevice, unsigned);
  static CUresult (*stream_create)(CUstream *, CUgreenCtx, unsigned, int);
  if (state < 0) {
    state = 0;
    void *f[6] = {};
    const char *name[6] = {"cuDeviceGet", "cuDeviceGetDevResource", "cuDevSmResourceSplitByCount",
                           "cuDevResourceGenerateDesc", "cuGreenCtxCreate", "cuGreenCtxStreamCreate"};
    for (int i = 0; i < 6; ++i) f[i] = nr_drv_sym(name[i], NR_DRV_VER_STREAM_CTX);
    unsigned groups = 1;
    int device = 0, nsm = 0;
    cudaGetDevice(&device);
    if (cudaDeviceGetAttribute(&nsm, cudaDevAttrMultiProcessorCount, device) == cudaSuccess && nsm > 0) sms = nsm / 6 > 1 ? nsm / 6 : 1;
    bool ok = f[0] && f[1] && f[2] && f[3] && f[4] && f[5]
        && ((CUresult(*)(CUdevice *, int))f[0])(&dev, device) == CUDA_SUCCESS
        && ((CUresult(*)(CUdevice, CUdevResource *, CUdevResourceType))f[1])(dev, &all, CU_DEV_RESOURCE_TYPE_SM) == CUDA_SUCCESS
        && ((CUresult(*)(CUdevResource *, unsigned *, const CUdevResource *, CUdevResource *, unsigned, unsigned))f[2])(
               &part[0], &groups, &all, &part[1], 0, (unsigned)sms) == CUDA_SUCCESS && groups == 1 && part[1].sm.smCount > 0;
    cudaGetLastError();
    if (!ok) {
      fprintf(stderr, "[GPU] green contexts unavailable (discovery SMs %d), all streams use all SMs\n", sms);
      return false;
    }
    desc_gen = (CUresult(*)(CUdevResourceDesc *, CUdevResource *, unsigned))f[3];
    ctx_create = (CUresult(*)(CUgreenCtx *, CUdevResourceDesc, CUdevice, unsigned))f[4];
    stream_create = (CUresult(*)(CUstream *, CUgreenCtx, unsigned, int))f[5];
    fprintf(stderr, "[GPU] discovery on %u SMs, real-time on the other SMs (%u of %u SMs)\n", part[0].sm.smCount, part[1].sm.smCount, all.sm.smCount);
    if (part[1].sm.smCount >= 32) {
      /* Fix A+D, P99-UL Task 3: the real-time SMs are split in three green contexts: the DL real-grant context (stream idx < NR_GREEN_UL_IDX) on its own SMs, a UL context (idx >= NR_GREEN_UL_IDX) on
       * ALL real-time SMs, i.e. overlapping the DL ones, and a small probe context (idx >= NR_GREEN_PROBE_IDX) on one group. UL streams are the lowest priority (graph pool): UL borrows the DL SMs while no
       * DL block is waiting for one, and a DL launch finds its SMs free again after at most one UL block (bench, nr_gpu_pass_test.cc GpuPassBench.DlSmShare: UL span 376 -> 288 us at 106 PRB, 571 -> 353 at
       * 217 PRB; DL span unchanged when alone, no worse in tail under load). A probe never occupies an SM a DL grant needs (its group is the small one). All SMs are split once into groups of the driver's
       * granularity (8 SMs on the GB10: 4 groups + a remainder; a split part cannot be split again, rc 915): the first group(s) are the discovery SMs, the next group is the probe group and the first UL
       * group, the rest (groups and remainder) is the DL context. A GPU with fewer than 32 real-time SMs keeps one real-time context (UL, DL and probes then yield by stream priority only). */
      CUdevResource rem = {};
      unsigned g = 16;
      const CUresult rc = ((CUresult(*)(CUdevResource *, unsigned *, const CUdevResource *, CUdevResource *, unsigned, unsigned))f[2])(rgrp, &g, &all, &rem, 0, 4);
      const unsigned sz = g ? rgrp[0].sm.smCount : 0, disc_n = sz ? (part[0].sm.smCount + sz - 1) / sz : 0;
      if (rc == CUDA_SUCCESS && g < 16 && sz && g >= disc_n + 1) {
        unsigned tot = g;
        if (rem.sm.smCount > 0) rgrp[tot++] = rem;
        ul_first = disc_n;
        pr_n = 1;
        dl_first = disc_n + 1;
        dl_n = tot > dl_first ? tot - dl_first : 0;
        ul_n = tot - ul_first; /* the UL context spans the probe group and the DL groups */
        if (dl_n) {
          unsigned dsm = 0;
          for (unsigned i = 0; i < dl_n; i++) dsm += rgrp[dl_first + i].sm.smCount;
          ngrp = 2;
          { unsigned usm = 0; for (unsigned i = 0; i < ul_n; i++) usm += rgrp[ul_first + i].sm.smCount; fprintf(stderr, "[GPU] real-time SMs split: DL %u SMs, UL up to %u SMs (overlapping DL), probes %u SMs\n", dsm, usm, rgrp[ul_first].sm.smCount); }
        }
      } else fprintf(stderr, "[GPU] real-time split of %u SMs failed (rc=%d groups=%u): one real-time context\n", all.sm.smCount, (int)rc, g);
      cudaGetLastError();
    }
    state = 1;
  }
  if (!rt && idx == -1) return false; /* no low-SM discovery stream: the callers keep the normal one */
  if (state != 1) return false;
  if (rt && ngrp) {
    const int q = idx >= NR_GREEN_PROBE_IDX ? 2 : idx >= NR_GREEN_UL_IDX ? 1 : 0;
    if (!rmade[q]) {
      CUdevResourceDesc desc;
      if (desc_gen(&desc, &rgrp[q ? ul_first : dl_first], q == 2 ? pr_n : q ? ul_n : dl_n) != CUDA_SUCCESS || ctx_create(&rgreen[q], desc, dev, CU_GREEN_CTX_DEFAULT_STREAM) != CUDA_SUCCESS)
        return false;
      rmade[q] = true;
    }
    return stream_create((CUstream *)s, rgreen[q], CU_STREAM_NON_BLOCKING, priority) == CUDA_SUCCESS;
  }
  const int k = rt ? 1 : 0;
  if (!made[k]) {
    CUdevResourceDesc desc;
    if (desc_gen(&desc, &part[k], 1) != CUDA_SUCCESS || ctx_create(&green[k], desc, dev, CU_GREEN_CTX_DEFAULT_STREAM) != CUDA_SUCCESS)
      return false;
    made[k] = true;
  }
  return stream_create((CUstream *)s, green[k], CU_STREAM_NON_BLOCKING, priority) == CUDA_SUCCESS;
#else
  (void)s; (void)priority; (void)rt; (void)idx;
  return false;
#endif
}
/* A discovery consumer asks for a low-SM stream with nr_gpu_green_stream(&lo, prio, false, -1); there is none (lo stays 0), so the pick is always the normal stream. */
static inline cudaStream_t nr_gpu_green_disc_pick(cudaStream_t hi, cudaStream_t lo, int large_active)
{
  return lo && large_active > 0 ? lo : hi;
}
/* PDSCH FEP/demod/LDPC pool contexts handed out: all of them (no limit). */
static inline int nr_gpu_ctx_limit(int pool) { return pool; }
#endif
