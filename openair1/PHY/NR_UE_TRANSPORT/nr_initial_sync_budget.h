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
/* RX_RECOVER re-sync: the PSS+SSS CFO estimate (PSS half-symbol phase, then the SSS phase over the PSS-SSS spacing, scaled by 4.3 in sss_nr.c) is the SSS residual after a PSS
 * that can be several kHz off at a weak peak; the SSS phase wraps at +-scs/4.3, so a PSS error beyond that returns the true offset minus one period
 * (OTA 2026-11-05 tmE1B_1_160625 cell 923: -13802 / tmA1B_1_160108: -14282 for a true ~+200 Hz; +14161 healed the latter). The period of the wrap in Hz. */
static inline int nr_sss_cfo_period_hz(int scs_hz)
{
  return (int)(2.0 * scs_hz / 4.3 + 0.5);
}
/* k such that est_hz + k * period_hz lies nearest to prior_hz (0 when the estimate is already within period/2 of the prior). */
int nr_cfo_alias_k(int est_hz, int prior_hz, int period_hz);
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
