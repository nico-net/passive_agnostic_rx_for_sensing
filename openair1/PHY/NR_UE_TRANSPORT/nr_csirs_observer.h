/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_OBSERVER_H
#define NR_CSIRS_OBSERVER_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_passive_metrics.h"
#include "nr_periodic_track.h"

#define NR_CSIRS_OBSERVER_MAX 2048
#define NR_CSIRS_MISSING_PERIODS 4
typedef enum { NR_CSIRS_TIME_SEARCH, NR_CSIRS_TIME_IDSWEEP, NR_CSIRS_TIME_CONFIRM, NR_CSIRS_TIME_CFR } nr_csirs_time_t;
typedef struct {
  uint16_t row, ports, density, period, offset, offset2, freq_domain, start_rb, nr_of_rbs;
  uint8_t symb_l0;
  uint8_t n_offsets, zp;
} nr_csirs_resource_t;
typedef struct {
  nr_csirs_resource_t resource;
  uint64_t first_slot;
  uint32_t last_due;
  uint8_t misses, active, seen, disappeared;
} nr_csirs_observer_entry_t;
typedef struct {
  nr_csirs_observer_entry_t entry[NR_CSIRS_OBSERVER_MAX];
  /* periodicity learned from the scored occasions of entry[i] (advisory: never gates a detection); kept apart from entry[] so the
   * linear find() over entries stays as dense as before */
  nr_ptrack_t pt[NR_CSIRS_OBSERVER_MAX];
  uint64_t csirs_candidates, csirs_confirmed, csirs_revoked, zp_exported, zp_revoked;
  uint64_t csirs_search_us, csirs_idsweep_us, csirs_confirm_us, csirs_cfr_us;
  uint64_t csirs_time_to_confirm_slots_last, csirs_time_to_confirm_slots_sum;
  nr_ptrack_counters_t periodic; /* NR_PTRACK_CSIRS over all confirmed resources of this cell */
  uint32_t csirs_confirm_resource_count;
  uint64_t csirs_confirm_resource_key[NR_PASSIVE_CSIRS_RESOURCE_METRICS_MAX];
  uint64_t csirs_confirm_resource_slots[NR_PASSIVE_CSIRS_RESOURCE_METRICS_MAX];
} nr_csirs_observer_t;
void nr_csirs_observer_candidate(nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint64_t slot);
/* Return true only for a changed confirmed map, never for the first confirmation or a duplicate. */
bool nr_csirs_observer_confirm(nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint64_t slot);
/* Caller supplies a scored predicted occasion. Four consecutive misses signal disappearance. */
bool nr_csirs_observer_due(nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint32_t slot, bool hit);
/* Periodic tracker (advisory). Next learned occasion strictly after `slot` of a confirmed resource, 0 when its pattern is not (yet) learned.
 * Predicted-due occasions are scored first; a prediction never skips or suppresses the scoring of any other occasion. */
uint32_t nr_csirs_observer_predicted_next(const nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint32_t slot);
bool nr_csirs_observer_predicted_due(const nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint32_t slot);
/* Refining one discovery resource's lattice/extent preserves its confirmation count. */
void nr_csirs_observer_refine(nr_csirs_observer_t *o, const nr_csirs_resource_t *old, const nr_csirs_resource_t *updated);
void nr_csirs_observer_revoke_zp(nr_csirs_observer_t *o, const nr_csirs_resource_t *r);
void nr_csirs_observer_export_zp(nr_csirs_observer_t *o);
void nr_csirs_observer_add_time(nr_csirs_observer_t *o, nr_csirs_time_t kind, uint64_t us);
void nr_csirs_observer_metrics(const nr_csirs_observer_t *o, nr_passive_metrics_t *m);
#endif
