#ifndef NR_GPU_FEP_HYGIENE_H
#define NR_GPU_FEP_HYGIENE_H

#include "nr_passive_sample_lifetime.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

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

typedef enum { NR_GPU_FEP_SKIPPED, NR_GPU_FEP_PASS, NR_GPU_FEP_FAIL } nr_gpu_fep_check_t;

/* CPU LLRs near zero have unstable signs across fixed-point and float demodulators. */
static inline double nr_gpu_fep_sign_agreement(const int16_t *cpu, const int16_t *gpu, uint32_t n)
{
  uint32_t comparable = 0, same = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (abs((int)cpu[i]) <= 8) continue;
    comparable++;
    same += (cpu[i] < 0) == (gpu[i] < 0);
  }
  return comparable ? (double)same / comparable : NAN;
}

static inline nr_gpu_fep_check_t nr_gpu_fep_check_result(bool comparable, double corr, double scale,
                                                        double sign_agree, bool crc_agree)
{
  if (!comparable || corr < 0 || !isfinite(sign_agree)) return NR_GPU_FEP_SKIPPED;
  return nr_gpu_fep_health_pass(corr, scale, sign_agree, crc_agree) ? NR_GPU_FEP_PASS : NR_GPU_FEP_FAIL;
}

static inline nr_gpu_fep_check_t nr_gpu_fep_reference_result(long producer, long source, long slots_per_frame,
                                                            bool comparable, double corr, double scale,
                                                            double sign_agree, bool crc_agree)
{
  return nr_gpu_fep_check_result(comparable && nr_passive_samples_valid(producer, source, slots_per_frame),
                                 corr, scale, sign_agree, crc_agree);
}

static inline bool nr_gpu_fep_should_disable(bool enforce, uint64_t prior_failures)
{
  return enforce && nr_gpu_fep_failure_disables(prior_failures);
}

#endif
