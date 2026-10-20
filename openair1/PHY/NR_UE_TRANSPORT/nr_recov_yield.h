/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* TAIL-FIX: the recovery lane (retry arms, old-path re-decode) runs only while no first-pass decode waits in the DL ring or the UL real queue. Header-only so the
 * decoder (which does not link nr_pdsch_recovery.c), the recovery worker and the unit test use one rule. busy() true = a first-pass decode waits.
 * Returns true when the lane may run now; false after max_ns of polling: the caller gives its remaining work up (its IQ snapshot holder is scarce). */
#ifndef NR_RECOV_YIELD_H
#define NR_RECOV_YIELD_H
#include <stdbool.h>
#include <stdint.h>

static inline bool nr_recov_wait_idle(bool (*busy)(void), uint64_t (*now_ns)(void), void (*nap)(void), uint64_t max_ns)
{
  if (!busy) return true;
  const uint64_t t0 = now_ns();
  while (busy()) {
    if (now_ns() - t0 > max_ns) return false;
    nap();
  }
  return true;
}
#endif
