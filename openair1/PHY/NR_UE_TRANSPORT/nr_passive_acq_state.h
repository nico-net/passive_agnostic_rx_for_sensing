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
 * actually observes. The state is the HIGHEST piece of evidence currently held, not a strict
 * chain: on this receiver SIB1 lands before any PDCCH-length discovery, so a run legitimately
 * goes SEARCHING -> PBCH_LOCKED -> SIB1_DECODED -> CELL_CONFIGURED without ever reporting
 * PDCCH_LOCKED as a state (it is still visible as a boolean in the heartbeat line).
 *
 * Two kinds of input feed it. POLLED discovery evidence (nr_passive_acq_update, from the blind
 * PDCCH monitor's occasion loop) and EVENT edges (nr_passive_acq_note_*), which exist because the
 * poll site only runs once the PDCCH monitor is configured -- measured on 4 s raw captures where
 * MIB and SIB1 both decoded while the monitor ran ZERO occasions, so a poll-only tracker reported
 * nothing at all. NR_ACQ_LOST is reachable from any state and is not part of the order. */
typedef enum {
  NR_ACQ_SEARCHING = 0,       // nothing yet: no PBCH lock (renamed from SEARCHING_PDCCH, see below)
  NR_ACQ_PBCH_LOCKED,         // MIB decoded and applied (event from nr-ue.c's sync path)
  NR_ACQ_SIB1_DECODED,        // SIB1 common config published (event from config_ue.c)
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
  int phy_n_rb, phy_mu, phy_ssb_start_subcarrier;
  double phy_dl_carrier_hz;
  int sib1_n_rb, sib1_mu, sib1_offset_to_point_a, sib1_offset_to_carrier, sib1_k_ssb;
} nr_passive_acq_carrier_t;
typedef struct {
  bool bw_match, mu_match, grid_match;
  int  point_a_subcarrier;   // derived Point A position in the started grid (0 = grid start)
  int  carrier_end_subcarrier;
  double derived_centre_hz;   // absolute carrier centre implied by SIB1, 0 if PHY frequency unknown
  double started_centre_hz;
} nr_passive_acq_carrier_verdict_t;

typedef struct {
  nr_passive_acq_state_t state;
  uint64_t updates, transitions, sync_losses, pbch_locks, sib1_decodes;
  int  carrier_verified;     // 0 = not yet checked, 1 = matches, -1 = MISMATCH
  nr_passive_acq_carrier_verdict_t carrier;
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
/* Event edges from the acquisition path itself, so early progress is reported even before the
 * blind PDCCH monitor has run its first occasion. Each re-evaluates the state immediately using
 * the most recently polled discovery inputs (all-false until the first poll). */
void nr_passive_acq_note_pbch_locked(void); // nr-ue.c: MIB applied, frame number known
void nr_passive_acq_note_sib1(void);        // config_ue.c: common config published (passive mode)

/* SIB1 carrier verification. The receiver is STARTED with a bandwidth, numerology and RF centre
 * taken from the recorder / command line (there is no PHY re-init after sync), so those were listed
 * as "assumed" parameters. SIB1 carries the cell's own carrierBandwidth and subcarrierSpacing, and
 * -- via offsetToPointA and the MIB's k_SSB, measured against where the SSB was actually FOUND in
 * the started FFT grid -- where the carrier sits inside that grid. This turns the assumption into a
 * verified fact or a loud mismatch. Pure integer arithmetic, unit-tested.
 *   phy_*      : what the PHY was started with (fp->N_RB_DL, fp->numerology_index,
 *                fp->ssb_start_subcarrier = SSB subcarrier 0 relative to the grid's first carrier)
 *   sib1_*     : carrierBandwidth (PRB), subcarrierSpacing (mu), offsetToPointA (15 kHz RBs, FR1),
 *                offsetToCarrier (PRB at mu), k_ssb (MIB ssb-SubcarrierOffset, at 15 kHz for FR1)
 * The carrier is consistent when Point A + offsetToCarrier lands at grid subcarrier 0 and the
 * carrier ends at N_RB_DL*12, i.e. the started grid IS the cell's carrier. */
nr_passive_acq_carrier_verdict_t nr_passive_acq_verify_carrier(const nr_passive_acq_carrier_t *c);
/* PHY registers its started geometry once (nr-uesoftmodem.c); MAC reports SIB1's carrier facts
 * (config_ue.c). The verdict is logged once and kept in the snapshot. */
/* dl_carrier_hz = the RF centre the PHY was started with; needed to express the SIB1-derived
 * carrier centre as an absolute frequency (the receiver could retune to it on a mismatch). */
void nr_passive_acq_set_phy_geometry(int n_rb, int mu, int ssb_start_subcarrier, double dl_carrier_hz);
void nr_passive_acq_note_sib1_carrier(int n_rb, int mu, int offset_to_point_a, int offset_to_carrier, int k_ssb);
/* TDD pattern from SIB1 (tdd-UL-DL-ConfigurationCommon). Slots are in the reference-SCS numbering
 * of the pattern; the query takes the receiver's absolute slot at the SAME numerology. Unknown -> DL. */
#include "nr_tdd_pattern.h"
void nr_passive_acq_note_sib1_tdd(const nr_tdd_pattern_t *p1, const nr_tdd_pattern_t *p2);
bool nr_passive_acq_tdd_slot_has_downlink(uint32_t absolute_slot);
bool nr_passive_acq_tdd_known(void);
/* BC9: reference SCS (referenceSubcarrierSpacing, mu) of the SIB1 pattern; must precede/accompany note_sib1_tdd. */
void nr_passive_acq_note_sib1_tdd_ref_mu(int mu);
/* BC9: last[k] = nr_tdd_pdsch_last_symbol(dci_abs_slot + k) for k < n, for the TDD exclusion of the Technique D sweep.
 * False (nothing filled) unless a valid SIB1 pattern AND its reference numerology are known and equal `mu` (the
 * receiver's numerology of dci_abs_slot): an unknown pattern (NSA, a cell without SIB1) never excludes. */
bool nr_passive_acq_tdd_pdsch_last_symbols(uint32_t dci_abs_slot, int mu, int n, int8_t *last);
/* Hard invalidation: the receive stream itself was lost (RXDISCONT), so the frame-to-sample
 * mapping -- and therefore every hypothesis being scored against it -- is invalid NOW. Drops
 * straight to NR_ACQ_LOST with NO hysteresis, unlike nr_passive_acq_update()'s evidence path:
 * a discontinuity is not noisy evidence that might recover, it is proof. The discovery state this
 * module reads is all LATCHED (winners stay settled, bwp_size stays set), so without this call a
 * sync loss is structurally invisible to the state machine -- measured on a deliberate 3 s gap
 * injected into a saved raw capture, where the receiver reacquired but the state never left
 * TRACKING. Safe to call from the RX thread: one lock, no allocation, event-driven (a handful of
 * calls per run, not per slot). */
void nr_passive_acq_note_sync_loss(void);
nr_passive_acq_snapshot_t nr_passive_acq_snapshot(void);
void nr_passive_acq_reset(void);

#ifdef __cplusplus
}
#endif
#endif
