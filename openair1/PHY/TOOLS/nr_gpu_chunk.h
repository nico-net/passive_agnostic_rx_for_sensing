/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_CHUNK_H
#define NR_GPU_CHUNK_H
/* Bounded discovery kernels. MEASURED (tests/passive_rx/gpuspan/launchblock_bench.cu, GB10): a graph with a conditional (IF) node, as every real-time
 * pass graph has, has its device-side body launch held until a long-running kernel of ANY other stream or green context ends (stall = the kernel's
 * remaining run time; a 1-thread or a 64-block kernel alike; back-to-back queued chunks stall for the whole queue, chunks with a host wait in between
 * stall for at most one chunk). So every discovery kernel is cut into chunks of about NR_GPU_CHUNK_TARGET_US of run time and the stream is drained
 * (host wait) between chunks: the GPU is idle for a moment, the pending body launch goes through. The chunk size is adapted per call site from the
 * measured GPU run time of the previous chunk (events on the stream: nr_gpu_wait.h NrGpuSpan), never its host wall time: on air every chunk carried a fixed 1-1.5 ms of
 * launch / wake-up / scheduling latency under real-time load (ab_p*, ab_g* 2026-10-21), more than the target, so a wall-time rule kept chunks at 2 units and a
 * 1-s call became 10-19 s. A call is also capped at NR_GPU_CHUNK_MAX_CALLS chunks. Header-only, no CUDA dependency. */
#include <stdint.h>
#include <chrono>
#include <cstdio>
#include <thread>

#define NR_GPU_CHUNK_TARGET_US 1000
#define NR_GPU_ADMIT_MAX_US 100
#define NR_GPU_CHUNK_MAX_CALLS 64 /* fail-safe: chunks per call; beyond it the rest runs in the remaining budget of chunks, whatever their length */
extern "C" int nr_gpu_budget_rt_active(void) __attribute__((weak)); /* the executable's nr_gpu_budget.c; weak: the discovery modules are dlopen'd */

struct NrGpuChunker {
  long per = 1;           /* units (blocks, slots, ...) per chunk, adapted */
  uint64_t chunks = 0;    /* chunks run (tests) */
  const char *name = "?"; /* call site, for the slow-call line */
  uint64_t gpu_us = 0;    /* GPU run time of the last chunk (or its wall time when the GPU time is unavailable), for the slow-call line */
};
static inline uint64_t nr_gpu_chunk_now_us()
{
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/* admission: a chunk does not start while a real DL grant is in flight, for at most NR_GPU_ADMIT_MAX_US per chunk (discovery is never starved) */
static inline void nr_gpu_chunk_admit()
{
  if (!nr_gpu_budget_rt_active) return;
  const uint64_t t0 = nr_gpu_chunk_now_us();
  while (nr_gpu_budget_rt_active() > 0 && nr_gpu_chunk_now_us() - t0 < NR_GPU_ADMIT_MAX_US) std::this_thread::yield();
}
/* adaptation after a chunk of n units ran gpu_us microseconds on the GPU (pure, unit-tested): the chunk size that would have taken the target, at most 16x
 * bigger per step (a chunk that took 10 us is not a licence for the whole call), at least one unit; no change inside [0.75, 1.5] x target. */
static inline void nr_gpu_chunk_adapt(NrGpuChunker &c, long n, uint64_t gpu_us, uint64_t target = NR_GPU_CHUNK_TARGET_US)
{
  c.gpu_us = gpu_us;
  if (gpu_us < 1) gpu_us = 1;
  if (gpu_us > target + target / 2) { c.per = (long)((uint64_t)n * target / gpu_us); if (c.per < 1) c.per = 1; }
  else if (gpu_us < target * 3 / 4 && n >= c.per) {
    long up = (long)((uint64_t)n * target / gpu_us);
    if (up > c.per * 16) up = c.per * 16;
    c.per = up > c.per * 2 ? up : c.per * 2;
  }
}
/* launch(first, n) enqueues the units [first, first+n) and returns 0 or an error; wait() drains the stream and returns 0 or an error; now() is the clock (us);
 * gpu() returns the GPU run time (us) of the chunk just waited for, or 0 when unknown (then the host wall time of the chunk is used).
 * Every unit runs exactly once, in order. Returns the first non-zero launch/wait result. */
template<class Launch, class Wait, class Admit, class Now, class Gpu>
static inline int nr_gpu_chunked(NrGpuChunker &c, long total, Launch launch, Wait wait, Admit admit, Now now, Gpu gpu)
{
  const uint64_t call_t0 = now();
  const uint64_t chunks0 = c.chunks;
  uint64_t gpu_sum = 0;
  struct SlowCall { NrGpuChunker &c; uint64_t t0, k0; long total; Now now; uint64_t &gpu_sum; ~SlowCall() {
    const uint64_t us = now() - t0;
    /* always on: one stderr line per call longer than 200 ms: chunk count, wall time, summed GPU time, last chunk size and GPU time of the slow call sites */
    if (us > 200000) fprintf(stderr, "[GPU] CHUNK_SLOW site=%s total_units=%ld chunks=%lu wall_us=%lu gpu_us=%lu per_end=%ld last_chunk_gpu_us=%lu\n", c.name, total,
                             (unsigned long)(c.chunks - k0), (unsigned long)us, (unsigned long)gpu_sum, c.per, (unsigned long)c.gpu_us);
  } } slow_call{c, call_t0, chunks0, total, now, gpu_sum};
  for (long first = 0; first < total;) {
    long n = c.per < total - first ? c.per : total - first;
    const uint64_t done = c.chunks - chunks0;
    if (done + 1 >= NR_GPU_CHUNK_MAX_CALLS / 2) { /* fail-safe: spread what is left over the chunks that remain in the budget */
      const uint64_t left = NR_GPU_CHUNK_MAX_CALLS > done ? NR_GPU_CHUNK_MAX_CALLS - done : 1;
      const long need = (long)(((uint64_t)(total - first) + left - 1) / left);
      if (need > n) n = need < total - first ? need : total - first;
    }
    admit();
    const uint64_t t0 = now();
    int rc = launch(first, n);
    if (rc) return rc;
    if ((rc = wait())) return rc;
    const uint64_t wall = now() - t0;
    uint64_t g = gpu();
    if (!g) g = wall;
    gpu_sum += g;
    nr_gpu_chunk_adapt(c, n, g);
    ++c.chunks;
    first += n;
  }
  return 0;
}
/* tests without GPU timing: the host time of the chunk is its run time */
template<class Launch, class Wait, class Admit, class Now>
static inline int nr_gpu_chunked(NrGpuChunker &c, long total, Launch launch, Wait wait, Admit admit, Now now)
{
  return nr_gpu_chunked(c, total, launch, wait, admit, now, [] { return (uint64_t)0; });
}
template<class Launch, class Wait, class Gpu>
static inline int nr_gpu_chunked(NrGpuChunker &c, long total, Launch launch, Wait wait, Gpu gpu)
{
  return nr_gpu_chunked(c, total, launch, wait, nr_gpu_chunk_admit, nr_gpu_chunk_now_us, gpu);
}
#endif
