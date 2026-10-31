/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_INITIAL_SYNC_BUDGET_H
#define NR_INITIAL_SYNC_BUDGET_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NR_INITIAL_SYNC_SCRATCH_MB_DEFAULT 512L
#define NR_INITIAL_SYNC_SCRATCH_MB_MIN 64L
#define NR_INITIAL_SYNC_SCRATCH_MB_MAX 16384L

/* Automatic cell acquisition (coarse CFO search, timing/drift confirm passes): on iff passive and the operator pinned
 * neither the SSB (--ssb) nor the CFO (--initial-fo). Requires --ue-fo-compensation. */
#define NR_ACQ_CFO_MAX_HZ 60000.0
static inline int nr_auto_acquire_trigger(int passive, int ssb_given, int initial_fo_given)
{
  return passive && !ssb_given && !initial_fo_given;
}
/* A1: a pinned start (--ssb and/or --initial-fo given) treats --initial-fo as the CENTRE of a CFO search, not as the only hypothesis:
 * after NR_ACQ_PINNED_ESCALATE_AFTER consecutive failed full scans of the same cell every following scan runs the ladder below around the seed
 * (the SSB pin and the PCI pin stay). PBCH CRC alone accepts a hypothesis. The PSS/SSS CFO estimate aliases at +-NR_ACQ_ALIAS_HZ (half the inverse
 * of the 2-symbol PSS-SSS spacing at 30 kHz SCS: 403's descriptor CFO 2178 was that alias of the true offset), so each rung also tries both aliases. */
#define NR_ACQ_PINNED_ESCALATE_AFTER 3
#define NR_ACQ_ALIAS_HZ 7040
#define NR_ACQ_LADDER_MAX 48
/* Hypotheses (Hz, absolute) around centre_hz within +-limit_hz, nearest-to-centre first: centre, centre +-alias, then per SCS step k = 1, 2, ...
 * centre +-k*scs and their +-alias neighbours. Returns the count (<= max). */
int nr_acq_cfo_ladder(int centre_hz, int scs_hz, int limit_hz, int *out, int max);
/* A2: true when gate_slot (frame * slots_per_frame + slot) lies in the search-space window [offset, offset + dur) of a period (slots). A period <= 0 is never. */
static inline int nr_acq_slot_in_window(unsigned gate_slot, int period, int offset, int dur)
{
  if (period <= 0)
    return 0;
  const unsigned rem = gate_slot % (unsigned)period, d = dur > 0 ? (unsigned)dur : 1u;
  return rem >= (unsigned)offset && rem < (unsigned)offset + d;
}
/* set once by get_options() (nr-uesoftmodem.c) */
extern int nr_auto_acquire_on;

/* Number of GSCNs scanned concurrently: min(budget / bytes_per_gscn, len_thr), each floored at 1.
 * scratch_mb is clamped to [MIN, MAX]. The caller still caps the result at numGscn. */
int nr_initial_sync_scan_batch(size_t bytes_per_gscn, size_t len_thr, long scratch_mb);

#ifdef __cplusplus
}
#endif
#endif
