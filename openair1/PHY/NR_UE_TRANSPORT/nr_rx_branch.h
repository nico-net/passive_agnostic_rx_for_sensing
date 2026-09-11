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

/* adaptive_RX_pipeline.md, Stage 1 / P03 (branch abstraction, foundation-only). Pure identity,
 * epoch and lifecycle bookkeeping for the four independent passive-RX branches -- no threads, no
 * hardware, no globals. Deliberately off the real-time path: nothing here is wired into the
 * nr-ue.c read loop yet (that is P04/P05).
 *
 * Field -> plan section mapping:
 *   - branch_id, physical_channel, rx_id      -> plan sec 3.2 schema "Run / hardware" row
 *                                                 (physical channel, rx_id) and sec 3.1
 *                                                 ("ReceiverBranch[0..3]" identity/physical
 *                                                 channel mapping).
 *   - acq_epoch                                -> plan sec 3.2 "RF continuity epoch", owned by
 *                                                 AcquisitionOwner (sec 3.1); common-mode across
 *                                                 every active branch (P05: "RF discontinuity
 *                                                 increments the acquisition epoch").
 *   - lock_epoch, state, lock_absolute_slot    -> plan sec 3.2 "branch lock epoch"; sec 3.1
 *                                                 "ReceiverBranch[0..3] owns synchronization ...";
 *                                                 P05: "Branch loss of lock increments its lock
 *                                                 epoch, clears its stale grants/CPI history and
 *                                                 reacquires locally."
 *   - counters                                 -> plan sec 3.2 "Health" row (samples/rows admitted,
 *                                                 queue drops, resets) and G1 test 3 (declared
 *                                                 drops on a stalled consumer).
 *   - nr_rx_branch_set_t.run_id                -> plan sec 3.2 "run_id".
 */
#ifndef NR_RX_BRANCH_H
#define NR_RX_BRANCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_RX_BRANCH_MAX 4
#define NR_RX_BRANCH_ID_LEN 16

typedef enum {
  NR_RXB_DISABLED = 0,  /* slot not named in the active branch list */
  NR_RXB_ACQUIRING,     /* active, not yet locked (or reset after a prior lock) */
  NR_RXB_LOCKED,        /* synchronized; lock_absolute_slot valid */
  NR_RXB_LOST           /* was locked/acquiring, lost lock or hit an RF discontinuity */
} nr_rx_branch_state_t;

typedef struct {
  uint8_t branch_id;              /* identity; never reassigned after parse */
  int8_t physical_channel;        /* -1 = unassigned (DISABLED branch); else 0..NR_RX_BRANCH_MAX-1 */
  char rx_id[NR_RX_BRANCH_ID_LEN];
  uint32_t acq_epoch;             /* AcquisitionOwner-owned; bumped on every active branch together */
  uint32_t lock_epoch;            /* branch-owned; bumped only by nr_rx_branch_lose_lock() */
  nr_rx_branch_state_t state;
  uint64_t lock_absolute_slot;    /* set by nr_rx_branch_lock(); meaningless outside LOCKED */
  struct {
    uint64_t samples_in;
    uint64_t samples_dropped;
    uint64_t relocks;
    uint64_t discontinuities;
  } counters;
} nr_rx_branch_t;

typedef struct {
  nr_rx_branch_t b[NR_RX_BRANCH_MAX];
  uint8_t n_active;
  uint64_t run_id;   /* caller-assigned; not touched by this module */
} nr_rx_branch_set_t;

/* P13a fix round 2: the index of branch_id in a per-branch array, or -1 when it names NO branch --
 * which is what NR_ISAC_BRANCH_NONE (0xFF) is, and what a branch view carries whenever its physical
 * channel maps to no active branch. Masking instead (`branch_id & (NR_RX_BRANCH_MAX-1)`, the idiom
 * this replaced) silently attributes the sentinel to a REAL branch: 0xFF & 3 == 3. Deliberately
 * takes the raw uint8_t and not an enum, because every producer of it is a uint8_t on the wire. */
static inline int nr_rx_branch_counter_index(uint8_t branch_id)
{
  return branch_id < NR_RX_BRANCH_MAX ? (int)branch_id : -1;
}

/* Parses "0,1,2,3"-style active_list and "0:0,1:1,2:2,3:3"-style branch:physical phys_map
 * (comma-separated, order-independent) into *set. rx_id_prefix defaults to "rx" when NULL/empty;
 * each active branch gets rx_id = "<prefix><branch_id>" (branch_id is always one digit, 0..3).
 *
 * Rejects (returns -1, logs LOG_E(PHY, ...) naming the offending key and value; *set is still
 * fully zero-initialized/safe to read on failure):
 *   - empty/oversized active_list or phys_map
 *   - a malformed or out-of-range (not 0..NR_RX_BRANCH_MAX-1) branch id or physical channel
 *   - a branch id repeated in active_list, or repeated as a phys_map key
 *   - a physical channel value used by two phys_map entries
 *   - an active branch (named in active_list) with no phys_map entry
 *   - the converse: a phys_map entry for a branch NOT named in active_list (P13a). Without this
 *     rejection a DISABLED slot could carry physical_channel >= 0, and the two "active" predicates
 *     used across the receiver -- `physical_channel >= 0` and `state != NR_RXB_DISABLED`/n_active --
 *     would disagree, which is a silent output-collision in the per-branch sensing engines. On
 *     success the two are now EQUIVALENT, and callers may rely on that.
 *   - rx_id_prefix longer than NR_RX_BRANCH_ID_LEN-2 (14) chars -- past that, "%s%d" into the
 *     rx_id buffer would truncate the trailing branch digit and every branch's rx_id would
 *     silently collide on the same truncated prefix
 * Returns 0 on success. On success every active branch is NR_RXB_ACQUIRING with epochs at 0;
 * every inactive slot is NR_RXB_DISABLED with physical_channel -1. set->run_id is left at 0 --
 * callers set it themselves. */
int nr_rx_branch_set_parse(nr_rx_branch_set_t *set, const char *active_list, const char *phys_map,
                            const char *rx_id_prefix);

/* Pure sanity check, independent of parse: fails (-1, LOG_E naming "rx_branches") when
 * set->n_active exceeds nb_antennas_rx. 0 on success. Exists as its own function so it can be
 * unit-tested without a live antenna count, and so a caller with a live nb_antennas_rx (see
 * nr_isac.cc) can invoke it right after nr_rx_branch_set_parse(). */
int nr_rx_branch_set_check_antennas(const nr_rx_branch_set_t *set, int nb_antennas_rx);

/* Transitions to LOCKED at absolute_slot. If the branch was NR_RXB_LOST, counts a relock
 * (counters.relocks++) -- this is the "relocks on the next lock" half of lose_lock()'s contract.
 * Does not touch acq_epoch or lock_epoch. */
void nr_rx_branch_lock(nr_rx_branch_t *b, uint64_t absolute_slot);

/* Transitions to LOST and increments lock_epoch. Does not touch acq_epoch. */
void nr_rx_branch_lose_lock(nr_rx_branch_t *b);

/* RF discontinuity is common-mode (AcquisitionOwner-level, not per-branch): increments acq_epoch
 * and counters.discontinuities, and sets state=NR_RXB_LOST, on every branch with
 * physical_channel >= 0 (i.e. every active branch, DISABLED slots untouched). Deliberately does
 * NOT touch lock_epoch -- lock_epoch is reserved for nr_rx_branch_lose_lock()'s deliberate,
 * branch-local loss-of-sync path; a caller that also wants a lock-epoch bump on discontinuity
 * calls nr_rx_branch_lose_lock() itself. */
void nr_rx_branch_set_rf_discontinuity(nr_rx_branch_set_t *set);

/* Clears counters and lock_absolute_slot and returns state to NR_RXB_ACQUIRING (active branch,
 * physical_channel >= 0) or NR_RXB_DISABLED (inactive). Identity (branch_id, physical_channel,
 * rx_id) and both epochs are left untouched -- epochs must never go backwards. */
void nr_rx_branch_reset(nr_rx_branch_t *b);

/* adaptive_RX_pipeline.md P06a: one dispatch descriptor per branch a piece of work must be
 * fanned out to. Carries the branch identity AND the epoch snapshot taken at fan-out time, which
 * is what nr_rx_branch_dispatch_is_stale() later tests -- the same "no old/new mixing" rule
 * nr_rx_branch_sync_is_stale() states for nr_rx_branch_sync_t, expressed for a work item that
 * travels through a queue instead of living next to the branch. */
typedef struct {
  uint8_t branch_id;
  int8_t physical_channel;
  uint32_t lock_epoch;
  uint32_t acq_epoch;
} nr_rx_branch_dispatch_t;

/* Fills out[] with one descriptor per ACTIVE branch (physical_channel >= 0), in branch-id order,
 * each stamped with that branch's CURRENT lock_epoch/acq_epoch. Returns the number written, or -1
 * on a NULL argument / max < 1 (nothing written). Writes at most max entries and returns -1 if the
 * set has more active branches than max, rather than silently fanning out to a subset: a caller
 * that drops branches without knowing it produces per-branch coverage numbers that are wrong in a
 * way no downstream counter can reveal.
 *
 * A NULL set is NOT an error for the caller's purposes but cannot be answered here, so it returns
 * -1 and the caller decides what "no branch set" means (for the DL producer: the single legacy
 * job, branch 0 / physical channel 0 -- see nr_pdsch_passive_queue_enqueue_fanout()). */
int nr_rx_branch_set_dispatch(const nr_rx_branch_set_t *set, nr_rx_branch_dispatch_t *out, int max);

/* True (1) iff d's epoch snapshot no longer matches the branch it names in *set -- i.e. that
 * branch lost lock and/or hit an RF discontinuity since the work item was created, so its result
 * must be discarded rather than mixed into the current epoch. Fails safe (returns 1, "stale") on a
 * NULL argument or when d names no active branch. */
int nr_rx_branch_dispatch_is_stale(const nr_rx_branch_set_t *set, const nr_rx_branch_dispatch_t *d);

#ifdef __cplusplus
}
#endif
#endif
