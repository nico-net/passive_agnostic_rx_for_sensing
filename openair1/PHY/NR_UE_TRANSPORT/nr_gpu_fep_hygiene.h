#ifndef NR_GPU_FEP_HYGIENE_H
#define NR_GPU_FEP_HYGIENE_H

#include "nr_passive_sample_lifetime.h"
#include <math.h>
#include <stdint.h>

/* The producer publishes before writing, so check the ring both before and immediately after
 * the synchronous FEP copy. A stale request never reaches LLR production. */
static inline int64_t nr_gpu_fep_run_checked(long source, long slots_per_frame,
                                             long (*producer)(void *), void (*stale)(void *, int),
                                             int (*fep)(void *), int64_t (*llr)(void *), void *ctx)
{
  if (!nr_passive_samples_valid(producer(ctx), source, slots_per_frame)) {
    stale(ctx, 0);
    return -1;
  }
  if (fep(ctx) != 0)
    return -1;
  if (!nr_passive_samples_valid(producer(ctx), source, slots_per_frame)) {
    stale(ctx, 1);
    return -1;
  }
  return llr(ctx);
}

static inline bool nr_gpu_fep_health_pass(double corr, double scale, double sign_agree, bool crc_agree)
{
  return isfinite(corr) && corr >= 0.9999 && isfinite(scale) && scale >= 0.99 && scale <= 1.01
      && isfinite(sign_agree) && sign_agree >= 0.999 && crc_agree;
}

static inline bool nr_gpu_fep_failure_disables(uint64_t prior_failures)
{
  return prior_failures + 1 >= 2;
}

#endif
