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
/* k such that est_hz + k * period_hz lies nearest to prior_hz (0 when the estimate is already within period/2 of the prior). est_hz and prior_hz are in the SAME units: the
 * re-sync estimate is relative to the radio's current tuning, so the prior is the last VERIFIED total lock CFO minus that tuning (nr-ue.c). A real step of the tuning (a radio
 * left mistuned by an earlier wrong lock) therefore does not look like an alias: OTA tmA1B_1_160108 epoch 3 measured +14161 on a radio at -13977, prior +237 - (-13977) = 14214 -> k 0. */
int nr_cfo_alias_k(int est_hz, int prior_hz, int period_hz);
/* One alias hypothesis of a re-sync: PBCH CRC, and the best-SSB PBCH DM-RS correlation energy m (coherent over the 3 PBCH symbols, so a residual CFO costs it) with mu0 = mean
 * correlation energy of the wrong SSB indices decoded from the same samples (the noise-only distribution of m). */
typedef struct {
  int crc;
  double m, mu0;
} nr_resync_hyp_t;
/* Decision between the measured estimate (h0) and the alias nearest the verified prior (h1), both decoded at their full compensation. 1: take the alias, 0: keep the measurement.
 * *neither = 1 when neither passes CRC (measurement kept, the caller counts it). Exactly one CRC pass decides. Both pass: a noise-only correlation energy is exponential with mean mu0, so
 * the energy of a signal-plus-noise bin has variance 2 m mu0 + mu0^2; the winner needs a lead of 3 sigma of the difference, else the hypothesis nearer the prior (the alias, by its choice). */
int nr_resync_cfo_decide(const nr_resync_hyp_t *h0, const nr_resync_hyp_t *h1, int *neither);
/* Verified-lock CFO of one cell (PHY_VARS_NR_UE.cfo_verify), pure so that the gate can be unit-tested (nr-ue.c UE_synch only calls these).
 * last_*: the latest lock (total CFO Hz = estimate + radio tuning, PCI). prior_*: the last lock that was VERIFIED (a PDSCH TB CRC passed whose slot is after the lock's slot, first pass,
 * not a rescue: nr_cfo_crc_counts). prior_est_hz: the prior in the units of the next re-sync estimate. After NR_CFO_UNVERIFIED_MAX consecutive unverified locks of the prior PCI the
 * prior is dropped (behaviour = no prior): a true CFO that moved by more than half a wrap period (reference re-lock, device re-init) would otherwise be flipped for ever. */
#define NR_CFO_UNVERIFIED_MAX 3
typedef struct {
  int last_valid, prior_valid;
  int last_total, last_pci, prior_total, prior_pci, prior_est_hz;
  int unverified_streak;
  volatile int clear_req; /* set by the control thread (live tune), consumed by the cell's sync job: __atomic builtins only */
} nr_cfo_verify_t;
static inline int nr_cfo_crc_counts(long job_slot, long lock_slot, int rescued)
{
  return !rescued && job_slot > lock_slot; /* strictly after the lock: the slot at the lock is the last one produced before the loss; jobs enqueued before it and rescues (decodable at SOME FO) prove nothing. The first lock of a cell has lock_slot -1 (producer clock not started), every produced slot (>= 0) is after it */
}
void nr_cfo_verify_record_lock(nr_cfo_verify_t *v, int total_hz, int pci);
/* At a UE_synch entry: evaluates the previous lock once (lock_crc_ok = counted TBs since it), updates prior_est_hz for the radio tuning now in force. Returns 1 when the prior was dropped. */
int nr_cfo_verify_select_prior(nr_cfo_verify_t *v, unsigned long long lock_crc_ok, int cont_fo_comp, int radio_fo_hz);
void nr_cfo_verify_clear(nr_cfo_verify_t *v);
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
