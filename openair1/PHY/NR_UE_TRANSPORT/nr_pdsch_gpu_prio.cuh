/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDSCH_GPU_PRIO_CUH
#define NR_PDSCH_GPU_PRIO_CUH
/* GPUSCHED ISAC_PDSCH_GPU_PRIO=1 (default off; off = streams and launches exactly as before).
 * Every context of the PDSCH FEP/chest, demodulation and LDPC pools gets a low-priority twin of its real-time stream,
 * one step below the greatest priority (still above the discovery streams). A decode body marked low (solver probe or
 * recovery retry, nr_pdsch_gpu_set_lowprio() per body from the host) launches on the twin, so the block scheduler serves
 * the pending blocks of real grants first. Stream priority orders pending blocks only; it never preempts running ones.
 * Results are unchanged by construction: same kernels, same buffers, only the stream differs, and a context is used by one
 * decode at a time and is idle (its work complete) when it is released. */
#include <cuda_runtime.h>
#include <cstdlib>
#include "nr_gpu_green.h"

extern __thread int nr_pdsch_gpu_low; /* the calling decode body is low priority (nr_pdsch_gpu_fep.cu) */

static inline bool nr_pdsch_gpu_prio_on(void)
{
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("ISAC_PDSCH_GPU_PRIO");
    on = e && atoi(e) == 1;
  }
  return on == 1;
}

/* nr_pdsch_gpu_low_stream: creates the low-priority twin of a real-time stream (same SM partition). lo/hi = the device's
 * least/greatest priority. Returns false on failure (the caller then keeps one stream). */
static inline bool nr_pdsch_gpu_low_stream(cudaStream_t *s, int lo, int hi)
{
  const int p = hi < lo ? hi + 1 : hi;
  return nr_gpu_green_stream(s, p, true) || cudaStreamCreateWithPriority(s, cudaStreamNonBlocking, p) == cudaSuccess;
}
#endif
