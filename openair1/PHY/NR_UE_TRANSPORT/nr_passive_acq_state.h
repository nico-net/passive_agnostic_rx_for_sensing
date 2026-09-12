/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */
#ifndef NR_PASSIVE_ACQ_STATE_H
#define NR_PASSIVE_ACQ_STATE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Explicit acquisition/discovery states for the passive receiver, named after what each one
 * actually observes (not aspirational SSB/PBCH names this module has no signal for -- see the
 * header comment in nr_passive_acq_state.c for exactly which existing counters/booleans drive
 * each transition). Strictly increasing "progress" order; NR_ACQ_LOST is reachable from any state
 * and is not part of that order. */
typedef enum {
  NR_ACQ_SEARCHING_PDCCH = 0, // no DCI length/CORESET locked yet
  NR_ACQ_PDCCH_LOCKED,        // DCI length found, CORESET extent not yet verified
  NR_ACQ_CORESET_VERIFIED,    // CORESET extent verified, UL BWP (SIB1-derived) not yet known
  NR_ACQ_CELL_CONFIGURED,     // UL BWP known; DL/UL field-interpretation searches not converged
  NR_ACQ_DL_CONVERGED,        // DL PDSCH-interpretation search has a winner; UL does not yet
  NR_ACQ_UL_CONVERGED,        // UL width/interp search has a winner; DL does not yet
  NR_ACQ_TRACKING,            // both DL and UL searches have a winner
  NR_ACQ_LOST,                // was tracking or configured; evidence collapsed (see hysteresis)
  NR_ACQ_NUM_STATES
} nr_passive_acq_state_t;

/* One call's worth of observed evidence. Every field is a snapshot read of an ALREADY-EXISTING
 * counter/boolean elsewhere in the receiver -- this struct carries no new measurement of its own,
 * only what the caller already knows. */
typedef struct {
  bool     pdcch_length_found;
  bool     coreset_extent_verified;
  bool     ul_bwp_known;       // SIB1-derived UL BWP resolved (0 = "SIB1 not decoded")
  uint64_t dl_search_winners;  // e.g. nr_pdsch_config_sweep winner count across contexts
  uint64_t ul_width_winners;   // nr_pdcch_ul_discovery_snapshot().width_winners
  uint64_t ul_interp_winners;  // nr_pdcch_ul_discovery_snapshot().interp_winners
} nr_passive_acq_inputs_t;

typedef struct {
  nr_passive_acq_state_t state;
  uint64_t updates, transitions;
  uint64_t time_in_state;      // consecutive updates spent in the current state
  uint64_t consecutive_regressions; // consecutive updates whose evidence regressed from `state`
} nr_passive_acq_snapshot_t;

/* HYSTERESIS: a single update with weaker evidence never demotes the state. Regression to
 * NR_ACQ_LOST requires NR_PASSIVE_ACQ_LOSS_HYSTERESIS consecutive updates whose evidence no longer
 * supports the current state -- one dropped grant or one CRC miss is expected noise, not loss. */
#define NR_PASSIVE_ACQ_LOSS_HYSTERESIS 8

const char *nr_passive_acq_state_name(nr_passive_acq_state_t s);
/* Advances the state machine by one observation. Never blocks, never allocates, safe to call from
 * a periodic (not per-slot) RT summary point -- see the call site's own period guard. Every actual
 * transition is logged at LOG_A with the evidence that caused it; every update is available via
 * nr_passive_acq_snapshot() regardless of logging. */
void nr_passive_acq_update(const nr_passive_acq_inputs_t *in);
nr_passive_acq_snapshot_t nr_passive_acq_snapshot(void);
void nr_passive_acq_reset(void);

#ifdef __cplusplus
}
#endif
#endif
