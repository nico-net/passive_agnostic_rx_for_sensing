#pragma once
/* ISAC_E2E_TRACE=1 stage split (EVMGRANT stg=): per-thread accumulators of where a decode body waits. Each GPU completion poll loop adds
 * its wall time to the bucket of its stage (gpuwait); the pool-context / staging-lock acquisitions add to NRSS_LK; prep_segment (CPU
 * rate dematching before the LDPC launch) to NRSS_PREP; the GPU FEP worker handshake (queue thread, before the body) to NRSS_WQ.
 * nr_ss_on == 0 (default): nr_ss_now() returns 0 and nr_ss_add() is a no-op, no clock is read. Weak definitions: one copy per link, so
 * the standalone GPU tests link without the decoder. */
#include <stdint.h>
#include <time.h>
enum { NRSS_FEP, NRSS_CHEST, NRSS_DEMOD, NRSS_LDPC, NRSS_LK, NRSS_PREP, NRSS_WQ, NRSS_N };
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((weak)) __thread uint64_t nr_ss_ns[NRSS_N];
__attribute__((weak)) int nr_ss_on;
#ifdef __cplusplus
}
#endif
static inline uint64_t nr_ss_now(void)
{
  if (!nr_ss_on)
    return 0;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static inline void nr_ss_add(int k, uint64_t t0)
{
  if (t0)
    nr_ss_ns[k] += nr_ss_now() - t0;
}
