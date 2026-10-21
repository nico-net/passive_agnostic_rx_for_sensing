#pragma once
/* The GPU completion polls of the decode threads
 * (cudaStreamQuery loops) sleep 20 us between polls once a wait has lasted 200 us.
 * Why: the PDSCH consumers (SCHED_FIFO 40, pinned) and the scan consumers (FIFO 50) spin on the GPU. A spinning RT thread
 * starves the bound per-CPU kernel workers (kworker/N) of its core; the GPU path they wait for then stalls until RT
 * throttling (sched_rt_runtime_us 950000 of 1000000) gives the core away: every consumer froze for ~950 ms
 * ([MEASURED, DGX air-like replay + perf sched]: kworker/3,4,7,8,9 max scheduling delay 940-950 ms, PDTIMSLOW 0.62-0.95 s
 * in all five consumers at once). A sleep lets the kworker run. Call nr_gpu_poll_relax(&t0) once per not-ready poll,
 * with t0 = 0 before the loop. */
#include <stdint.h>
#include <stdlib.h>
#include "nr_stage_split.h" /* ISAC_E2E_TRACE stage split: callers of the poll loops add their wait time */
#include <time.h>

/* core pool: a decode thread that has waited this long on the GPU gives its token away for the sleep when somebody waits (weak: absent in unit tests) */
#ifdef __cplusplus
extern "C" void nr_pool_site_gpu_wait(void) __attribute__((weak));
#else
extern void nr_pool_site_gpu_wait(void) __attribute__((weak));
#endif

static inline void nr_gpu_poll_relax_us(uint64_t *t0, int us)
{
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
    if (nr_pool_site_gpu_wait) {
      nr_pool_site_gpu_wait(); /* sleeps 20 us (giving the token away meanwhile when somebody waits) */
      return;
    }
    const struct timespec ts = {0, 20000};
    nanosleep(&ts, NULL);
  }
}

static inline void nr_gpu_poll_relax(uint64_t *t0) { nr_gpu_poll_relax_us(t0, 200); }
