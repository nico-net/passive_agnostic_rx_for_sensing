/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* ULSTALL: splits every slow UL GPU call (FEP or LDPC) into its parts:
 *   launch  host time inside the launch call (driver lock / page-migration drain would show here),
 *   gpu     cudaEvent time between the stream reaching the call and its end (queue behind earlier work on the stream or
 *           partition + kernel run),
 *   wake    the rest: the host noticing completion late (poll sleep, preemption of the consumer),
 *   cpu / nivcsw / nvcsw  the calling thread's CPU time and context switches over the call (RUSAGE_THREAD): wall >> cpu with
 *           nivcsw > 0 is preemption, wall >> cpu with nvcsw only is the poll sleeps.
 * One "ULSTALL" line per call slower than 2000 us, at most 400 per process. Header only, no GPU state. */
#ifndef NR_PUSCH_GPU_DIAG_H
#define NR_PUSCH_GPU_DIAG_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <atomic>
#include <string.h>
#include "nr_gpu_green.h"
#include <sys/resource.h>

/* Where the UL FEP / LDPC streams live: the real-time SM partition at the LEAST stream priority (DL streams outrank them; a
 * running UL block is never preempted), else the discovery partition at the default priority, where CSI-RS discovery's long
 * kernels caused 14-455 ms UL stalls (MEASURED). Returns 2 rt, 1 disc, 0 = no green context (the caller makes a plain stream). */
static inline int nr_pusch_gpu_ul_stream(cudaStream_t *s, int hi, int lo, int idx)
{
  (void)hi;
  if (nr_gpu_green_stream(s, lo, true, idx)) return 2;
  return nr_gpu_green_stream(s, 0, false) ? 1 : 0;
}
namespace uldiag {
struct Snap {
  uint64_t cpu_us = 0, ivcsw = 0, vcsw = 0;
};
static inline bool on() { return true; } /* shortcut: stall split always on; remove with its users in nr_pdsch_gpu_ldpc.cu */
static inline uint64_t thr_us()
{
  return 2000;
}
static inline Snap snap()
{
  struct rusage r;
  Snap s;
  if (getrusage(RUSAGE_THREAD, &r) == 0) {
    s.cpu_us = (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000 + (uint64_t)(r.ru_utime.tv_usec + r.ru_stime.tv_usec);
    s.ivcsw = (uint64_t)r.ru_nivcsw;
    s.vcsw = (uint64_t)r.ru_nvcsw;
  }
  return s;
}
static inline void report(const char *who, uint64_t dt_us, uint64_t launch_us, double gpu_us, int items, const Snap &a, const Snap &b)
{
  static std::atomic<int> n{0};
  if (dt_us < thr_us() || n.fetch_add(1) >= 400) return;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  const double wake = (double)dt_us - (double)launch_us - gpu_us;
  fprintf(stderr, "SENSING: ULSTALL %s t=%.3f dt_us=%lu launch_us=%lu gpu_us=%.0f wake_us=%.0f items=%d cpu_us=%lu nivcsw=%lu nvcsw=%lu\n", who,
          (double)t.tv_sec + 1e-9 * (double)t.tv_nsec, (unsigned long)dt_us, (unsigned long)launch_us, gpu_us, wake, items,
          (unsigned long)(b.cpu_us - a.cpu_us), (unsigned long)(b.ivcsw - a.ivcsw), (unsigned long)(b.vcsw - a.vcsw));
}
}  // namespace uldiag
#endif
