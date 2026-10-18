/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* ULSTALL: ISAC_UL_STALL_DIAG=1 (default off) splits every slow UL GPU call (FEP or LDPC) into its parts:
 *   launch  host time inside the launch call (driver lock / page-migration drain would show here),
 *   gpu     cudaEvent time between the stream reaching the call and its end (queue behind earlier work on the stream or
 *           partition + kernel run),
 *   wake    the rest: the host noticing completion late (poll sleep, preemption of the consumer),
 *   cpu / nivcsw / nvcsw  the calling thread's CPU time and context switches over the call (RUSAGE_THREAD): wall >> cpu with
 *           nivcsw > 0 is preemption, wall >> cpu with nvcsw only is the poll sleeps.
 * One "ULSTALL" line per call slower than ISAC_UL_STALL_US (default 2000), at most 400 per process. Header only, no GPU state. */
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

/* ISAC_UL_GPU_PRIO=1 (default off): the UL FEP / LDPC streams inside the discovery SM partition get the GREATEST stream priority
 * instead of the default one, so their blocks are placed before the pending blocks of CSI-RS discovery kernels sharing the
 * partition (priority orders pending blocks only; it never preempts a running block and never reaches the RT partition). */
static inline bool nr_pusch_gpu_ul_prio_hi(void)
{
  static int v = -1;
  if (v < 0) {
    /* DEFERRED/DO NOT USE: ISAC_UL_GPU_PRIO not in the frozen config t26zE (2026-10-18); slated for removal */
    const char *e = getenv("ISAC_UL_GPU_PRIO");
    v = e && atoi(e) == 1;
  }
  return v == 1;
}
/* ISAC_UL_GPU_PART=disc|rt (default disc = the old behaviour): where the UL FEP / LDPC streams live.
 *   disc  the discovery SM partition (CSI-RS discovery's long kernels sit in it: 14-455 ms UL stalls MEASURED),
 *   rt    the real-time partition at the LEAST stream priority (DL streams outrank them; a running UL block is never preempted).
 * (A UL partition carved out of the real-time SMs was tried: the driver refuses the second split, rc 915.)
 * Returns 1 disc, 2 rt, 0 = no green context (the caller makes a plain lowest-priority stream). hi / lo = the device's
 * greatest / least stream priority. */
static inline int nr_pusch_gpu_ul_stream(cudaStream_t *s, int hi, int lo, int idx)
{
  const char *e = getenv("ISAC_UL_GPU_PART");
  if ((!e || !strcmp(e, "rt")) && nr_gpu_green_stream(s, lo, true, idx)) return 2;
  return nr_gpu_green_stream(s, nr_pusch_gpu_ul_prio_hi() ? hi : 0, false) ? 1 : 0;
}
namespace uldiag {
struct Snap {
  uint64_t cpu_us = 0, ivcsw = 0, vcsw = 0;
};
static inline bool on()
{
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("ISAC_UL_STALL_DIAG");
    v = !e || atoi(e) == 1;
  }
  return v == 1;
}
static inline uint64_t thr_us()
{
  /* DEFERRED/DO NOT USE: ISAC_UL_STALL_US not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *e = getenv("ISAC_UL_STALL_US");
  static uint64_t us = e && atoi(e) > 0 ? (uint64_t)atoi(e) : 2000;
  return us;
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
