/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDSCH_GPU_PRIO_CUH
#define NR_PDSCH_GPU_PRIO_CUH
/* The PDSCH low-priority twin streams are gone (the PDSCH pools run on the real-time streams only). Only the
 * graph-pass probe stream in nr_gpu_pass_graph.cu still asks; it is always off. */
#include <cuda_runtime.h>
#include "nr_gpu_green.h"

static inline bool nr_pdsch_gpu_prio_on(void) { return false; }

/* nr_pdsch_gpu_low_stream: creates the low-priority twin of a real-time stream (same SM partition). lo/hi = the device's
 * least/greatest priority. Returns false on failure (the caller then keeps one stream). */
static inline bool nr_pdsch_gpu_low_stream(cudaStream_t *s, int lo, int hi, int idx = 0) /* idx: the RT green context of the stream it twins (graphs launch only in their own) */
{
  const int p = hi < lo ? hi + 1 : hi;
  return nr_gpu_green_stream(s, p, true, idx) || cudaStreamCreateWithPriority(s, cudaStreamNonBlocking, p) == cudaSuccess;
}
#endif
