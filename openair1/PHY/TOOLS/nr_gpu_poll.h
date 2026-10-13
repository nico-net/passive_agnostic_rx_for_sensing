#pragma once
/* ISAC_GPU_POLL_SLEEP_US=N (P99, default 0 = pure spin = unchanged): the GPU completion polls of the decode threads
 * (cudaStreamQuery loops) sleep 20 us between polls once a wait has lasted N us.
 * Why: the PDSCH consumers (SCHED_FIFO 40, pinned) and the scan consumers (FIFO 50) spin on the GPU. A spinning RT thread
 * starves the bound per-CPU kernel workers (kworker/N) of its core; the GPU path they wait for then stalls until RT
 * throttling (sched_rt_runtime_us 950000 of 1000000) gives the core away: every consumer froze for ~950 ms
 * ([MEASURED, DGX air-like replay + perf sched]: kworker/3,4,7,8,9 max scheduling delay 940-950 ms, PDTIMSLOW 0.62-0.95 s
 * in all five consumers at once). A sleep lets the kworker run. Call nr_gpu_poll_relax(&t0) once per not-ready poll,
 * with t0 = 0 before the loop. */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static inline void nr_gpu_poll_relax(uint64_t *t0)
{
  static int us = -1;
  if (us < 0) {
    const char *e = getenv("ISAC_GPU_POLL_SLEEP_US");
    us = e && atoi(e) > 0 ? atoi(e) : 0;
  }
  if (!us)
    return;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const uint64_t ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
  if (!*t0) {
    *t0 = ns;
    return;
  }
  if (ns - *t0 >= (uint64_t)us * 1000ull) {
    const struct timespec ts = {0, 20000};
    nanosleep(&ts, NULL);
  }
}
