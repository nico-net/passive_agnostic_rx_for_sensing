/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_GREEN_H
#define NR_GPU_GREEN_H
/* GPUSHARE SM partition with CUDA green contexts (default off; off = streams created exactly as before).
 *  ISAC_GPU_DISC_SMS=N   background discovery (CSI-RS discovery, PDCCH NID) streams live in a green context on the
 *                        first N SMs (rounded up by the driver to its co-scheduling granularity).
 *  ISAC_GPU_RT_ISOLATE=1 (needs N > 0) real-time streams (PDSCH FEP/chest/demod/LDPC, PDCCH FEP) live in a green
 *                        context on the remaining SMs, so discovery blocks never share an SM with them.
 * Header-only, CUDA sources only; driver entry points come from cudaGetDriverEntryPointByVersion (no -lcuda). Each
 * translation unit makes its own green contexts over the same split. Green-context streams share the primary context's
 * memory, events and modules. On any failure the caller creates its stream the usual way.
 *  ISAC_GPU_RT_PARTS=P   (GPUALLOC, needs RT_ISOLATE; default off) the real-time SMs are split into up to P disjoint
 *                        green contexts of at least floor(rt_sms / P) SMs (the driver rounds to its granularity) and stream idx
 *                        goes to group idx % groups, so concurrent PDSCH consumers (one pool context each) do not
 *                        share SMs. Off, or if the driver yields < 2 groups: one RT context exactly as before. */
#include <cuda_runtime.h>
#include <cuda.h>
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
/* LOW must be a subset of the DISC SMs and disjoint from the RT SMs (the driver does not promise either for a split taken from all SMs) */
static inline bool nr_green_low_ok(const unsigned *lo, const unsigned *disc, const unsigned *rt, bool have_rt)
{
  bool sub = true, dis = true, any = false;
  for (int i = 0; i < 4; i++) {
    sub &= (lo[i] & ~disc[i]) == 0;
    if (have_rt) dis &= (lo[i] & rt[i]) == 0;
    any |= lo[i] != 0;
  }
  return any && sub && dis;
}

/* nr_gpu_green_stream: creates *s (non-blocking, given priority) in this translation unit's discovery (rt = false) or
 * real-time (rt = true) green context when the flags above ask for it; returns true on success, false when off or
 * unavailable. The contexts are created once, on first use, and never destroyed. */
static inline bool nr_gpu_green_stream_impl(cudaStream_t *s, int priority, bool rt, int idx)
{
#if CUDART_VERSION >= 12050
  static int state = -1; /* -1 not read, 0 off, 1 on */
  static int sms, isolate;
  static CUgreenCtx green[2], rgreen[8], lgreen;
  static bool made[2], rmade[8], lmade, lfail;
  static CUdevResource part[2], rpart[8], lpart, all;
  static int lsms;
  static unsigned ngrp;
  static CUdevice dev;
  static CUresult (*desc_gen)(CUdevResourceDesc *, CUdevResource *, unsigned);
  static CUresult (*ctx_create)(CUgreenCtx *, CUdevResourceDesc, CUdevice, unsigned);
  static CUresult (*stream_create)(CUstream *, CUgreenCtx, unsigned, int);
  if (state < 0) {
    const char *e = getenv("ISAC_GPU_DISC_SMS"), *r = getenv("ISAC_GPU_RT_ISOLATE");
    sms = e ? atoi(e) : 8;
    isolate = !r || atoi(r) == 1;
    state = 0;
    if (sms <= 0) return false;
    void *f[6] = {};
    const char *name[6] = {"cuDeviceGet", "cuDeviceGetDevResource", "cuDevSmResourceSplitByCount",
                           "cuDevResourceGenerateDesc", "cuGreenCtxCreate", "cuGreenCtxStreamCreate"};
    cudaDriverEntryPointQueryResult q;
    for (int i = 0; i < 6; ++i)
      if (cudaGetDriverEntryPointByVersion(name[i], &f[i], 12050, cudaEnableDefault, &q) != cudaSuccess
          || q != cudaDriverEntryPointSuccess) f[i] = nullptr;
    unsigned groups = 1;
    int device = 0;
    cudaGetDevice(&device);
    bool ok = f[0] && f[1] && f[2] && f[3] && f[4] && f[5]
        && ((CUresult(*)(CUdevice *, int))f[0])(&dev, device) == CUDA_SUCCESS
        && ((CUresult(*)(CUdevice, CUdevResource *, CUdevResourceType))f[1])(dev, &all, CU_DEV_RESOURCE_TYPE_SM) == CUDA_SUCCESS
        && ((CUresult(*)(CUdevResource *, unsigned *, const CUdevResource *, CUdevResource *, unsigned, unsigned))f[2])(
               &part[0], &groups, &all, &part[1], 0, (unsigned)sms) == CUDA_SUCCESS && groups == 1 && part[1].sm.smCount > 0;
    cudaGetLastError();
    if (!ok) {
      fprintf(stderr, "[GPU] ISAC_GPU_DISC_SMS=%d: green contexts unavailable, all streams use all SMs\n", sms);
      return false;
    }
    desc_gen = (CUresult(*)(CUdevResourceDesc *, CUdevResource *, unsigned))f[3];
    ctx_create = (CUresult(*)(CUgreenCtx *, CUdevResourceDesc, CUdevice, unsigned))f[4];
    stream_create = (CUresult(*)(CUstream *, CUgreenCtx, unsigned, int))f[5];
    fprintf(stderr, "[GPU] ISAC_GPU_DISC_SMS=%d: discovery on %u SMs, real-time on %s (%u of %u SMs)\n", sms,
            part[0].sm.smCount, isolate ? "the other SMs" : "all SMs", isolate ? part[1].sm.smCount : all.sm.smCount, all.sm.smCount);
    if (isolate) {
      /* DEFERRED/DO NOT USE: ISAC_GPU_RT_PARTS not in the frozen config t26zE (2026-10-18); slated for removal */
      const char *pe = getenv("ISAC_GPU_RT_PARTS");
      const int want = pe ? atoi(pe) : 0;
      const unsigned per = want > 1 ? (unsigned)(part[1].sm.smCount / (unsigned)want) : 0;
      if (want > 1 && want <= 8 && per >= 8) {
        CUdevResource rem;
        unsigned g = (unsigned)want;
        const CUresult rc = ((CUresult(*)(CUdevResource *, unsigned *, const CUdevResource *, CUdevResource *, unsigned, unsigned))f[2])(
            rpart, &g, &part[1], &rem, 0, per);
        if (rc == CUDA_SUCCESS && g >= 2) ngrp = g;
        else fprintf(stderr, "[GPU] ISAC_GPU_RT_PARTS: split of %u SMs by %u failed (rc=%d groups=%u)\n", part[1].sm.smCount, per, (int)rc, g);
        cudaGetLastError();
      }
      if (want > 1)
        fprintf(stderr, "[GPU] ISAC_GPU_RT_PARTS=%d: %u real-time SM groups of %u SMs\n", want, ngrp ? ngrp : 1u,
                ngrp ? rpart[0].sm.smCount : part[1].sm.smCount);
    }
    /* Dynamic partition: ISAC_GPU_DISC_SMS_LOW (> 0) is a smaller discovery context (the leading SMs, so a subset of the discovery ones); idx == -1 asks for it. */
    /* DEFERRED/DO NOT USE: ISAC_GPU_DISC_SMS_LOW not in the frozen config t26zE (2026-10-18); slated for removal */
    const char *le = getenv("ISAC_GPU_DISC_SMS_LOW");
    lsms = le ? atoi(le) : 0;
    if (lsms > 0 && lsms < (int)part[0].sm.smCount) {
      CUdevResource lrem;
      unsigned g = 1;
      const CUresult lrc = ((CUresult(*)(CUdevResource *, unsigned *, const CUdevResource *, CUdevResource *, unsigned, unsigned))f[2])(
          &lpart, &g, &all, &lrem, 0, (unsigned)lsms); /* from all SMs: a split part cannot be split again (rc 915); the driver hands out the same leading SMs */
      if (lrc != CUDA_SUCCESS || g != 1) {
        fprintf(stderr, "[GPU] ISAC_GPU_DISC_SMS_LOW=%d: split of %u SMs failed (rc=%d groups=%u), single discovery partition\n", lsms, part[0].sm.smCount, (int)lrc, g);
        lsms = 0;
      }
      cudaGetLastError();
    } else lsms = 0;
    if (lsms) fprintf(stderr, "[GPU] ISAC_GPU_DISC_SMS_LOW=%d: %u SMs while a large real grant is in flight\n", lsms, lpart.sm.smCount);
    state = 1;
  }
  if (!rt && idx == -1) { /* the LOW discovery stream; false when not configured (the caller keeps using the normal one) */
    if (state != 1 || !lsms) return false;
    if (lfail) return false;
    if (!lmade) {
      CUdevResourceDesc desc;
      if (desc_gen(&desc, &lpart, 1) != CUDA_SUCCESS || ctx_create(&lgreen, desc, dev, CU_GREEN_CTX_DEFAULT_STREAM) != CUDA_SUCCESS)
        return false;
      /* startup check of the SM sets: otherwise LOW is disabled (the callers keep the DISC stream) */
      cudaStream_t sl = nullptr, sd = nullptr, sr = nullptr;
      unsigned ml[4] = {}, md[4] = {}, mr[4] = {};
      bool ok = true;
      for (int k = 0; k < (isolate ? 2 : 1) && ok; k++) {
        if (!made[k]) {
          CUdevResourceDesc d2;
          ok = desc_gen(&d2, &part[k], 1) == CUDA_SUCCESS && ctx_create(&green[k], d2, dev, CU_GREEN_CTX_DEFAULT_STREAM) == CUDA_SUCCESS;
          made[k] = ok;
        }
      }
      ok = ok && stream_create((CUstream *)&sl, lgreen, CU_STREAM_NON_BLOCKING, 0) == CUDA_SUCCESS
           && stream_create((CUstream *)&sd, green[0], CU_STREAM_NON_BLOCKING, 0) == CUDA_SUCCESS
           && (!isolate || stream_create((CUstream *)&sr, green[1], CU_STREAM_NON_BLOCKING, 0) == CUDA_SUCCESS)
           && nr_green_smmask(sl, ml) && nr_green_smmask(sd, md) && (!isolate || nr_green_smmask(sr, mr))
           && nr_green_low_ok(ml, md, mr, isolate);
      if (sl) cudaStreamDestroy(sl);
      if (sd) cudaStreamDestroy(sd);
      if (sr) cudaStreamDestroy(sr);
      cudaGetLastError();
      if (!ok) {
        lfail = true;
        fprintf(stderr, "[GPU] ISAC_GPU_DISC_SMS_LOW=%d: the low SM set is not inside the discovery SMs / overlaps the real-time SMs: disabled, discovery keeps its normal partition\n", lsms);
        return false;
      }
      lmade = true;
    }
    return stream_create((CUstream *)s, lgreen, CU_STREAM_NON_BLOCKING, priority) == CUDA_SUCCESS;
  }
  if (state != 1 || (rt && !isolate)) return false;
  if (rt && ngrp) {
    const int q = (idx < 0 ? 0 : idx) % (int)ngrp;
    if (!rmade[q]) {
      CUdevResourceDesc desc;
      if (desc_gen(&desc, &rpart[q], 1) != CUDA_SUCCESS || ctx_create(&rgreen[q], desc, dev, CU_GREEN_CTX_DEFAULT_STREAM) != CUDA_SUCCESS)
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
/* ISAC_GPU_GREEN_LOG=1 (default off): one stderr line per call that fails (the caller then creates an ordinary stream on all SMs) and, for the first
 * green stream of each (translation unit, rt, idx < 8), the SM mask it really lands on, so a run shows whether a consumer got its partition and whether
 * the partitions of different translation units are disjoint (tu = address of a per-TU static: each TU has its own green contexts). */
static inline bool nr_gpu_green_stream(cudaStream_t *s, int priority, bool rt, int idx = 0)
{
  static int logon = -1;
  static bool seen[2][8];
  /* DEFERRED/DO NOT USE: ISAC_GPU_GREEN_LOG not in the frozen config t26zE (2026-10-18); slated for removal */
  if (logon < 0) { const char *e = getenv("ISAC_GPU_GREEN_LOG"); logon = e && atoi(e) == 1; }
  const bool ok = nr_gpu_green_stream_impl(s, priority, rt, idx);
  if (!logon) return ok;
  if (!ok) {
    fprintf(stderr, "[GPU green] tu=%p rt=%d idx=%d prio=%d: NOT green (ordinary stream on all SMs)\n", (void *)&seen, (int)rt, idx, priority);
  } else if (idx >= -1 && idx < 8 && !seen[rt][idx < 0 ? 0 : idx]) {
    seen[rt][idx < 0 ? 0 : idx] = true;
    unsigned m[4] = {};
    const bool g = nr_green_smmask(*s, m);
    fprintf(stderr, "[GPU green] tu=%p rt=%d idx=%d prio=%d: green stream, SMs=%d mask=%08x%08x%08x%08x\n", (void *)&seen, (int)rt, idx, priority,
            g ? __builtin_popcount(m[0]) + __builtin_popcount(m[1]) + __builtin_popcount(m[2]) + __builtin_popcount(m[3]) : -1, m[3], m[2], m[1], m[0]);
  }
  return ok;
}
/* Only the CSI-RS GPU sweep (nr_csirs_disc_gpu_sweep.cu) uses the LOW stream; the other discovery consumers keep their DISC stream. */
/* Dynamic discovery partition (ISAC_GPU_DISC_SMS_LOW, default off). A discovery consumer creates its normal stream and, through
 * nr_gpu_green_stream(&lo, prio, false, -1), a second one on fewer SMs (lo stays 0 when not configured). At the START of each job, with
 * its previous job synchronised, it calls nr_gpu_green_disc_pick(hi, lo, nr_gpu_budget_rt_large_active()) and uses the result for the whole job:
 * a stream is never switched under a kernel that is already queued, so in-flight work finishes untouched. */
static inline cudaStream_t nr_gpu_green_disc_pick(cudaStream_t hi, cudaStream_t lo, int large_active)
{
  return lo && large_active > 0 ? lo : hi;
}
/* GPUALLOC ISAC_PDSCH_GPU_CTX_LIMIT=N (default off): PDSCH FEP/demod/LDPC pool contexts handed out are limited to the
 * first N (>= 1), so at most N consumers have GPU work in flight and the others wait on the CPU side. */
static inline int nr_gpu_ctx_limit(int pool)
{
  static int n = -1;
  /* DEFERRED/DO NOT USE: ISAC_PDSCH_GPU_CTX_LIMIT not in the frozen config t26zE (2026-10-18); slated for removal */
  if (n < 0) { const char *e = getenv("ISAC_PDSCH_GPU_CTX_LIMIT"); n = e ? atoi(e) : 0; }
  return n >= 1 && n < pool ? n : pool;
}
#endif
