/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PASSIVE_JOB_EPOCH_H
#define NR_PASSIVE_JOB_EPOCH_H
#include <stdbool.h>
#include <stdint.h>

/* A supplied clock makes queue admission/rejection deterministic in offline tests. */
static inline uint32_t nr_passive_job_epoch_stamp(bool enabled, uint32_t (*current)(void))
{
  return enabled ? current() : 0;
}

static inline bool nr_passive_job_epoch_old(bool enabled, uint32_t stamped, uint32_t (*current)(void))
{
  return enabled && stamped != current();
}
#endif
