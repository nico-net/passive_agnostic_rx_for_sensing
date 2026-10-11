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
 * memory, events and modules. On any failure the caller creates its stream the usual way. */
#include <cuda_runtime.h>
#include <cuda.h>
#include <cstdio>
#include <cstdlib>

/* nr_gpu_green_stream: creates *s (non-blocking, given priority) in this translation unit's discovery (rt = false) or
 * real-time (rt = true) green context when the flags above ask for it; returns true on success, false when off or
 * unavailable. The contexts are created once, on first use, and never destroyed. */
static inline bool nr_gpu_green_stream(cudaStream_t *s, int priority, bool rt)
{
#if CUDART_VERSION >= 12050
  static int state = -1; /* -1 not read, 0 off, 1 on */
  static int sms, isolate;
  static CUgreenCtx green[2];
  static bool made[2];
  static CUdevResource part[2];
  static CUdevice dev;
  static CUresult (*desc_gen)(CUdevResourceDesc *, CUdevResource *, unsigned);
  static CUresult (*ctx_create)(CUgreenCtx *, CUdevResourceDesc, CUdevice, unsigned);
  static CUresult (*stream_create)(CUstream *, CUgreenCtx, unsigned, int);
  if (state < 0) {
    const char *e = getenv("ISAC_GPU_DISC_SMS"), *r = getenv("ISAC_GPU_RT_ISOLATE");
    sms = e ? atoi(e) : 0;
    isolate = r && atoi(r) == 1;
    state = 0;
    if (sms <= 0) return false;
    void *f[6] = {};
    const char *name[6] = {"cuDeviceGet", "cuDeviceGetDevResource", "cuDevSmResourceSplitByCount",
                           "cuDevResourceGenerateDesc", "cuGreenCtxCreate", "cuGreenCtxStreamCreate"};
    cudaDriverEntryPointQueryResult q;
    for (int i = 0; i < 6; ++i)
      if (cudaGetDriverEntryPointByVersion(name[i], &f[i], 12050, cudaEnableDefault, &q) != cudaSuccess
          || q != cudaDriverEntryPointSuccess) f[i] = nullptr;
    CUdevResource all;
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
    state = 1;
  }
  const int k = rt ? 1 : 0;
  if (state != 1 || (rt && !isolate)) return false;
  if (!made[k]) {
    CUdevResourceDesc desc;
    if (desc_gen(&desc, &part[k], 1) != CUDA_SUCCESS || ctx_create(&green[k], desc, dev, CU_GREEN_CTX_DEFAULT_STREAM) != CUDA_SUCCESS)
      return false;
    made[k] = true;
  }
  return stream_create((CUstream *)s, green[k], CU_STREAM_NON_BLOCKING, priority) == CUDA_SUCCESS;
#else
  (void)s; (void)priority; (void)rt;
  return false;
#endif
}
#endif
