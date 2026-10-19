/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DL HARQ soft combining x retry arms (2026-10-19). Header-only so the decoder and the unit test share one rule.
 *  - Lane: with combining on, the retry arms run only in the async recovery lane (recov_mode != 0: 1 defers them, 2 is the worker), never inline on the first pass.
 *  - Sum: only the first pass of a grant writes the soft sum; every arm body is quiet (it decodes into the private buffer, d_to_be_cleared) so an arm can neither
 *    add its LLRs (a different geometry) nor count a transmission twice.
 *  - Rescue: an arm that passes CRC retires the entry of that TB (same ndi, first transmitted no later than the job's slot).
 *  - Ordering: the entry's busy flag is the lock. Consumers take it non-blocking (busy = decode without combining); the worker waits for it (bounded). */
#ifndef NR_HQ_RETRY_H
#define NR_HQ_RETRY_H
#include <stdbool.h>
#include <stdint.h>

static inline bool nr_hq_retry_lane(bool harq_on, int recov_mode)
{
  return !harq_on || recov_mode != 0;
}
/* the soft sum is touched only by a non-quiet (first pass) decode */
static inline bool nr_hq_may_write_sum(bool quiet)
{
  return !quiet;
}
/* the stored sum belongs to the TB that the rescued job carried (the 1-bit ndi alone repeats every second TB, so the first-transmission slot orders them) */
static inline bool nr_hq_rescue_match(bool soft_valid, uint8_t e_ndi, int64_t e_first_slot, uint8_t job_ndi, int64_t job_slot)
{
  return soft_valid && e_ndi == job_ndi && job_slot >= e_first_slot;
}
/* worker side: take the entry lock, waiting up to `spins` naps; false = still held by a consumer (the sum stays; harmless, the TB is decoded) */
static inline bool nr_hq_lock_wait(void *busy_p, void (*nap)(void), int spins)
{
  int *busy = (int *)busy_p; /* an _Atomic int in C, std::atomic<int> in the test: both are a plain int object */
  for (int i = 0;; i++) {
    int expect = 0;
    if (__atomic_compare_exchange_n(busy, &expect, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return true;
    if (i >= spins)
      return false;
    nap();
  }
}
#endif
