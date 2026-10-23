/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CUDA toolkit 12.3 .. 13.x compatibility for the CUDA translation units of the GPU pass (include from .cu files only).
 *
 * The runtime API renamed or re-versioned four things the graph pass uses between 12.3 and 13.0; each wrapper below picks the
 * spelling of the toolkit that compiles it. On 13.x (the DGX) every wrapper is the plain call the code made before this header existed.
 *   nr_drv_sym            driver entry point: cudaGetDriverEntryPointByVersion (12.5+) / cudaGetDriverEntryPoint (older: the ABI of the compile-time toolkit)
 *   nr_capture_info       cudaStreamGetCaptureInfo with the edge-data out-parameter: _v3 in 12.3 .. 12.x, the plain name in 13.x
 *   nr_capture_set_deps   cudaStreamUpdateCaptureDependencies with edge data: _v2 in 12.x, the plain name in 13.x
 *   nr_stream_ctx         cuStreamGetCtx: the green-context out-parameter form is 12.5+ (cuStreamGetCtx_v2); below it the 2-argument form, no green context
 * Conditional graph nodes (the in-graph no-chance gate) need toolkit 12.3; the driver is probed at run time by the caller. */
#pragma once
#include <cuda_runtime.h>
#include <cuda.h>

#if CUDART_VERSION < 12030
#error "the GPU pass needs CUDA toolkit 12.3 or newer (graph conditional nodes)"
#endif

/* driver ABI versions for nr_drv_sym lookups (ignored below 12.5, where the entry point is the compile-time toolkit's) */
#define NR_DRV_VER_STREAM_CTX 12050  /* cuStreamGetCtx_v2, cuCtxFromGreenCtx, the green-context entry points */
#define NR_DRV_VER_COND_HANDLE 12030 /* cuGraphConditionalHandleCreate */
#if CUDART_VERSION >= 13000
#define NR_DRV_VER_GRAPH_ADD_NODE 13000 /* cuGraphAddNode with CUgraphEdgeData (13.x signature) */
#else
#define NR_DRV_VER_GRAPH_ADD_NODE 12030 /* cuGraphAddNode_v2 with CUgraphEdgeData (12.3 .. 12.x signature) */
#endif

/* the driver entry point `name` at ABI `ver`; nullptr when the runtime or the driver does not have it */
static inline void *nr_drv_sym(const char *name, int ver)
{
  void *f = nullptr;
  cudaDriverEntryPointQueryResult q;
#if CUDART_VERSION >= 12050
  if (cudaGetDriverEntryPointByVersion(name, &f, ver, cudaEnableDefault, &q) != cudaSuccess || q != cudaDriverEntryPointSuccess) return nullptr;
#else
  (void)ver;
  if (cudaGetDriverEntryPoint(name, &f, cudaEnableDefault, &q) != cudaSuccess || q != cudaDriverEntryPointSuccess) return nullptr;
#endif
  return f;
}

static inline cudaError_t nr_capture_info(cudaStream_t s, cudaStreamCaptureStatus *st, unsigned long long *id, cudaGraph_t *g, const cudaGraphNode_t **deps,
                                          const cudaGraphEdgeData **edges, size_t *n)
{
#if CUDART_VERSION >= 13000
  return cudaStreamGetCaptureInfo(s, st, id, g, deps, edges, n);
#else
  return cudaStreamGetCaptureInfo_v3(s, st, id, g, deps, edges, n);
#endif
}

/* replaces the capture dependencies of s by the n nodes (edges: per-edge data or nullptr) */
static inline cudaError_t nr_capture_set_deps(cudaStream_t s, cudaGraphNode_t *nodes, const cudaGraphEdgeData *edges, size_t n)
{
#if CUDART_VERSION >= 13000
  return cudaStreamUpdateCaptureDependencies(s, nodes, edges, n, cudaStreamSetCaptureDependencies);
#else
  return cudaStreamUpdateCaptureDependencies_v2(s, nodes, edges, n, cudaStreamSetCaptureDependencies);
#endif
}

/* fn: the cuStreamGetCtx entry point looked up with nr_drv_sym("cuStreamGetCtx", NR_DRV_VER_STREAM_CTX); *green is nullptr when the toolkit has no green-context form */
static inline CUresult nr_stream_ctx(void *fn, CUstream s, CUcontext *ctx, CUgreenCtx *green)
{
#if CUDART_VERSION >= 12050
  return ((CUresult(*)(CUstream, CUcontext *, CUgreenCtx *))fn)(s, ctx, green);
#else
  *green = nullptr;
  return ((CUresult(*)(CUstream, CUcontext *))fn)(s, ctx);
#endif
}
