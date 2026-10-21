/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_CHUNK_H
#define NR_GPU_CHUNK_H
/* Bounded discovery kernels. MEASURED (tests/passive_rx/gpuspan/launchblock_bench.cu, GB10): a graph with a conditional (IF) node, as every real-time
 * pass graph has, has its device-side body launch held until a long-running kernel of ANY other stream or green context ends (stall = the kernel's
 * remaining run time; a 1-thread or a 64-block kernel alike; back-to-back queued chunks stall for the whole queue, chunks with a host wait in between
 * stall for at most one chunk). So every discovery kernel is cut into chunks of about NR_GPU_CHUNK_TARGET_US of run time and the stream is drained
 * (host wait) between chunks: the GPU is idle for a moment, the pending body launch goes through. The chunk size is adapted per call site from the
 * measured host time of the previous chunk, so it follows the GPU (sm_89 / sm_121) with no constant to tune. Header-only, no CUDA dependency. */
#include <stdint.h>
#include <chrono>
#include <thread>

#define NR_GPU_CHUNK_TARGET_US 1000
#define NR_GPU_ADMIT_MAX_US 100
extern "C" int nr_gpu_budget_rt_active(void) __attribute__((weak)); /* the executable's nr_gpu_budget.c; weak: the discovery modules are dlopen'd */

struct NrGpuChunker {
  long per = 1;           /* units (blocks, slots, ...) per chunk, adapted */
  uint64_t chunks = 0;    /* chunks run (tests) */
  uint64_t base = ~0ull;  /* per-chunk wall-time floor (launch + wake-up + queueing behind real-time graphs), not kernel run time */
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
/* adaptation after a chunk of n units took us microseconds (pure, unit-tested). Only the time above the floor c.base counts as work: with per=1 starting
 * and a plain "us < target/2" growth rule, a floor above 500 us (poll wake-up + admission + GPU contention) pinned per at 1 for ever (3000 chunks instead of 20,
 * sim in DISC-CONFIRM-20261021.md). shortcut: the floor rises only through one-unit chunks, so a load that rises while per is large first shrinks the chunks (safe direction). */
static inline void nr_gpu_chunk_adapt(NrGpuChunker &c, long n, uint64_t us, uint64_t target = NR_GPU_CHUNK_TARGET_US)
{
  if (us < c.base) c.base = us; else if (n == 1 && us < 2 * c.base) c.base += (us - c.base) / 4; /* only one-unit chunks measure the floor */
  const uint64_t work = us > c.base ? us - c.base : 0;
  if (work > target + target / 2) { c.per = (long)((uint64_t)n * target / work); if (c.per < 1) c.per = 1; }
  else if (work < target / 2 && n >= c.per) c.per = c.per * 2;
}
/* launch(first, n) enqueues the units [first, first+n) and returns 0 or an error; wait() drains the stream and returns 0 or an error; now() is the clock (us).
 * Every unit runs exactly once, in order. Returns the first non-zero launch/wait result. */
template<class Launch, class Wait, class Admit, class Now>
static inline int nr_gpu_chunked(NrGpuChunker &c, long total, Launch launch, Wait wait, Admit admit, Now now)
{
  for (long first = 0; first < total;) {
    const long n = c.per < total - first ? c.per : total - first;
    admit();
    const uint64_t t0 = now();
    int rc = launch(first, n);
    if (rc) return rc;
    if ((rc = wait())) return rc;
    nr_gpu_chunk_adapt(c, n, now() - t0);
    ++c.chunks;
    first += n;
  }
  return 0;
}
template<class Launch, class Wait>
static inline int nr_gpu_chunked(NrGpuChunker &c, long total, Launch launch, Wait wait)
{
  return nr_gpu_chunked(c, total, launch, wait, nr_gpu_chunk_admit, nr_gpu_chunk_now_us);
}
#endif
