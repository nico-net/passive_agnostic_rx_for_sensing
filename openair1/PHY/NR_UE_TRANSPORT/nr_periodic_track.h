/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PERIODIC_TRACK_H
#define NR_PERIODIC_TRACK_H
/* Periodicity tracker for resources that appear without a DCI (SRS, CSI-RS/TRS, PUCCH, SPS/configured-grant PUSCH).
 * Pure state machine, no I/O and no globals: one nr_ptrack_t per detected resource, owned by the per-cell detector.
 * The period/offset are learned from the detection slots only (TS 38.331 period sets), tolerate missed detections and
 * a few stray detections, and are withdrawn on evidence (misses of predicted occasions). */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NR_PTRACK_SRS, NR_PTRACK_CSIRS, NR_PTRACK_PUCCH, NR_PTRACK_CG, NR_PTRACK_TYPES } nr_ptrack_type_t;
#define NR_PTRACK_HITS 16        /* detection ring; the estimate uses the last NR_PTRACK_HITS detections */
#define NR_PTRACK_MIN_HITS 4     /* detections that must fit one (period, offset) before it is trusted */
#define NR_PTRACK_EXPIRY 10240   /* slots without any detection after which the history is stale */
typedef struct {
  uint64_t hits[NR_PTRACK_HITS];
  unsigned n;                 /* detections kept (ascending slots) */
  unsigned period, offset;    /* valid when confirmed: occasions are slots with slot%period==offset */
  bool confirmed;
  unsigned miss_run;          /* consecutive missed predicted occasions */
  uint32_t outcomes;          /* last predicted occasions, bit 0 = newest, 1 = hit */
  unsigned n_outcomes;
} nr_ptrack_t;
/* Per resource type; additive, kept by the owner of the trackers. */
typedef struct {
  uint64_t predicted, hit, miss, demoted, confirmed, audit_hit;
} nr_ptrack_counters_t;
/* The standard period set of a type, ascending; returns the count. */
unsigned nr_ptrack_period_set(nr_ptrack_type_t type, const unsigned **periods);
/* Pure estimator. hits ascending. Picks the largest period of the type's set under which at least NR_PTRACK_MIN_HITS and 80 % of
 * the detections share one offset and at least 75 % of the occasions between the first and last matching detection were detected. */
bool nr_ptrack_estimate(nr_ptrack_type_t type, const uint64_t *hits, unsigned n, unsigned *period, unsigned *offset);
void nr_ptrack_init(nr_ptrack_t *t);
/* A detection at `slot`. It counts as a predicted hit when the resource was due, otherwise as an audit/blind hit. */
void nr_ptrack_hit(nr_ptrack_t *t, nr_ptrack_type_t type, uint64_t slot, nr_ptrack_counters_t *c);
/* The resource was due at `slot` and was not detected (correlation/CRC evidence below its threshold). May demote. */
void nr_ptrack_miss(nr_ptrack_t *t, nr_ptrack_type_t type, uint64_t slot, nr_ptrack_counters_t *c);
bool nr_ptrack_due(const nr_ptrack_t *t, uint64_t slot);
/* Next occasion strictly after `slot`; 0 when not confirmed. */
uint64_t nr_ptrack_next(const nr_ptrack_t *t, uint64_t slot);
#ifdef __cplusplus
}
#endif
#endif
