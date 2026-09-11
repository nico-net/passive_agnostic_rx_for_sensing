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

/* adaptive_RX_pipeline.md, Stage 1 / P05 (independent digital correction/recovery,
 * foundation-only). Per-branch digital correction state -- CFO accumulator, timing offset,
 * frame-wrap/slot-continuity bookkeeping and a lock/acq epoch snapshot for staleness detection.
 * Pure math on a caller-owned struct: no threads, no hardware, no globals, and -- load-bearing,
 * see nr_rx_branch_sync_apply_cfo() below -- no way to reach the radio at all: nothing in this
 * file takes a device handle, a UE context, or any pointer that could lead to one.
 *
 * Builds on P03's nr_rx_branch_t (identity, lock_epoch/acq_epoch, lifecycle) and P04's
 * nr_rx_span_pool_t (immutable spans stamped with the acq_epoch they were produced under). This
 * module is the third leg: what a branch does to ITS OWN copy of the digital stream once it has
 * samples, and how it recognizes a job it started under an epoch the branch has since moved past.
 *
 * Deliberately NOT wired into the nr-ue.c read loop -- see docs/passive_branch_wiring_plan.md
 * (this task) for exactly which nr-ue.c statements this struct's fields are meant to replace, and
 * why the wiring itself is not done here (nr-ue.c carries another session's uncommitted work).
 *
 * Field -> plan section mapping:
 *   - cfo_hz, cfo_accum_hz            -> plan sec 3.2 "digital correction" row (CFO); P05:
 *                                         "No branch-local correction may move the common
 *                                         hardware frequency" -- see nr_rx_branch_sync_apply_cfo().
 *   - timing_offset_samples,
 *     shift_for_next_frame            -> plan sec 3.2 "digital correction" row (timing); the
 *                                         per-branch analogue of nr-ue.c's UE->max_pos_acc-driven
 *                                         shiftForNextFrame (see the wiring plan doc).
 *   - last_absolute_slot, frame_wraps -> plan sec 4 P05: "RF discontinuity increments the
 *                                         acquisition epoch" and G1 test 4 ("inject frame wrap,
 *                                         sample-counter discontinuity ... assert correct epoch
 *                                         transitions and no old/new mixing").
 *   - synchronized                    -> plan sec 3.2 "branch lock epoch" row; the digital-state
 *                                         analogue of nr-ue.c's UE->is_synchronized, but
 *                                         per-branch and with no hardware side effect of its own.
 *   - snap_lock_epoch, snap_acq_epoch -> plan sec 3.2 schema; P05: "no old/new mixing" primitive
 *                                         -- see nr_rx_branch_sync_is_stale().
 */
#ifndef NR_RX_BRANCH_SYNC_H
#define NR_RX_BRANCH_SYNC_H

#include <stdint.h>
#include "nr_rx_branch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  double cfo_hz;                     /* most recent branch-local CFO correction applied by
                                         nr_rx_branch_sync_apply_cfo(); a DIGITAL derotation value
                                         (e.g. fed to a per-branch NCO/phase-correction step), never
                                         a retune request. */
  double cfo_accum_hz;                /* running total of every correction applied since the last
                                         nr_rx_branch_sync_reset() -- the accumulator itself. */
  int32_t timing_offset_samples;      /* branch-local fine timing offset, samples. Caller-owned:
                                         this module never writes it except to clear it on
                                         reset(); a future consumer (not this task) updates it. */
  int32_t shift_for_next_frame;       /* branch-local analogue of nr-ue.c's shiftForNextFrame
                                         (executables/nr-ue.c:1046 and callers) -- a per-branch
                                         digital frame-boundary correction. Caller-owned like
                                         timing_offset_samples above; cleared on reset(). */
  int64_t last_absolute_slot;         /* HIGH-WATER MARK (fix round 1, controller-ruled): the
                                         largest absolute_slot ever passed to
                                         nr_rx_branch_sync_on_slot(); it only advances, never
                                         regresses -- an accepted small-backward/frame-wrap step
                                         does NOT lower it. 0 before the first call. NOT cleared by
                                         reset() -- see that function's comment for why. */
  uint32_t frame_wraps;               /* lifetime count of frame-index decreases observed by
                                         nr_rx_branch_sync_on_slot() (the "inject frame wrap" half
                                         of G1 test 4). NOT cleared by reset(), for the same reason
                                         as last_absolute_slot: it is slot-continuity bookkeeping,
                                         not correction state, and time keeps moving forward across
                                         a loss-of-lock even though synchronization is lost. */
  uint8_t synchronized;               /* branch-local digital sync flag; cleared by reset(). Does
                                         NOT drive any hardware action by itself -- compare to
                                         UE->is_synchronized in nr-ue.c, whose clearing today sits
                                         alongside (but is logically separate from) hardware-owned
                                         reactions; see docs/passive_branch_wiring_plan.md. */
  uint32_t snap_lock_epoch;           /* branch's lock_epoch (nr_rx_branch_t.lock_epoch) at the
                                         moment this job/context was created for the branch --
                                         i.e. a direct field copy the caller takes explicitly
                                         (e.g. "s.snap_lock_epoch = branch->lock_epoch;") when
                                         dispatching work for this branch. This module provides no
                                         separate "snapshot" function on purpose: it is a one-line
                                         field copy, the same convention nr_rx_branch.c itself uses
                                         for its own fields (see e.g. nr_rx_branch_lock()), and
                                         adding an accessor for a two-field struct copy would be
                                         pure ceremony. NOT touched by reset() -- see that
                                         function's comment. */
  uint32_t snap_acq_epoch;            /* branch's acq_epoch (nr_rx_branch_t.acq_epoch) at the same
                                         moment as snap_lock_epoch above. Same convention, same
                                         reset() exemption. */
} nr_rx_branch_sync_t;

/* Clears correction state -- cfo_hz, cfo_accum_hz, timing_offset_samples,
 * shift_for_next_frame, synchronized -- to zero/false. Does NOT touch last_absolute_slot,
 * frame_wraps, snap_lock_epoch or snap_acq_epoch: the epoch snapshot is identity (P05: "epochs
 * must never go backwards", mirroring nr_rx_branch_reset()'s own contract), and
 * last_absolute_slot/frame_wraps are slot-continuity bookkeeping that stays valid across a lock
 * loss -- the sample clock keeps advancing even though synchronization was lost, so there is
 * nothing to rewind there. Call this on loss of lock, alongside nr_rx_branch_lose_lock() (this
 * module does not call that function itself -- it has no nr_rx_branch_t to call it on). Safe to
 * call with s == NULL (no-op). */
void nr_rx_branch_sync_reset(nr_rx_branch_sync_t *s);

/* Counts a frame wrap (s->frame_wraps++) whenever the frame index (absolute_slot /
 * slots_per_frame, integer division) is LOWER than the frame index s->last_absolute_slot was in
 * -- the "inject frame wrap" half of G1 test 4. Rejects (returns -1, LOG_E naming the delta,
 * state UNCHANGED) when absolute_slot moves backwards by MORE than one frame (i.e.
 * last_absolute_slot - absolute_slot > slots_per_frame): this is the out-of-order-job defect
 * class recorded in memory as "unsigned slot delta breaks concurrent CPI" -- a job that arrived
 * out of sequence from a concurrent producer, not a legitimate frame-boundary event. A small
 * backward step (at most one frame) is accepted (returns 0) and treated as an ordinary frame
 * wrap, not an error -- this module has no notion of the real SFN's own wrap modulus (e.g. 1024
 * frames), so it is the CALLER's job to pass an absolute_slot domain in which a backward step of
 * at most one frame is the only way a legitimate wrap can look (see
 * docs/passive_branch_wiring_plan.md for how the real nr-ue.c per-slot loop's absolute_slot maps
 * onto this).
 *
 * s->last_absolute_slot ITSELF (fix round 1, controller-ruled) is updated to absolute_slot ONLY
 * WHEN absolute_slot EXCEEDS its current value -- an accepted small-backward step never lowers
 * it. Before this rule, an accepted backward step lowered last_absolute_slot, so a later job
 * whose value fell between the old (higher) and new (lower) one would compute a positive delta
 * against the lowered reference and be read as ordinary forward progress with no backward check
 * at all -- re-admitting the exact defect class this function exists to reject (see
 * nr_rx_branch_sync_t.last_absolute_slot's field comment, and this task's Fix round 1 report for
 * why the coordinator's literal "N, then N-2, then reject N-1" test cannot be satisfied by any
 * monotonic distance-from-high-water-mark tolerance rule -- N-1 is numerically CLOSER to the mark
 * than the already-accepted N-2, so no such rule can reject the closer value while accepting the
 * farther one; RxBranchSyncOnSlot.HighWaterMarkDoesNotRegressRejectBoundaryAfterSmallBackward
 * Excursion demonstrates the actual property instead: the reject boundary itself does not
 * silently shift down after an accepted excursion).
 *
 * Rejects (returns -1, no state change) if s is NULL or slots_per_frame <= 0. Returns 0 on
 * success. */
int nr_rx_branch_sync_on_slot(nr_rx_branch_sync_t *s, int64_t absolute_slot, int32_t slots_per_frame);

/* True (1) iff s's epoch snapshot (snap_lock_epoch/snap_acq_epoch) no longer matches branch's
 * CURRENT lock_epoch/acq_epoch -- i.e. the branch lost lock and/or hit an RF discontinuity since
 * this job/context was snapshotted, so a caller must discard it rather than mix its results into
 * a fresh job. False (0) when both still match. Fails safe (returns 1, "stale") if s or branch is
 * NULL, LOG_E naming which pointer was NULL -- a caller that cannot even check should not treat
 * unchecked state as fresh. This is the "no old/new mixing" primitive P05 asks for. */
int nr_rx_branch_sync_is_stale(const nr_rx_branch_sync_t *s, const nr_rx_branch_t *branch);

/* Accumulates a branch-local digital frequency correction: s->cfo_hz = hz;
 * s->cfo_accum_hz += hz. DIGITAL-ONLY, BY CONSTRUCTION, NOT MERELY BY CONVENTION: this function's
 * signature takes nothing but a struct this module itself defines and a double -- no device
 * handle, no PHY_VARS_NR_UE, no openair0_device_t, nothing that could reach nrue_ru_set_freq(),
 * dev->trx_set_freq_func(), or any other retune/device API exists anywhere in this file's
 * translation unit's reachable call graph. That is the invariant P05 asks be "documented and
 * asserted": there is structurally nothing to assert at runtime because there is no path to
 * violate -- unlike the real hardware CFO retune (nr_ue_cfo_resync_hz / nrue_ru_set_freq() at
 * executables/nr-ue.c:2013-2014, driven by the process-wide nr_ue_cfo_resync_request the CFO trim
 * loop sets), which this function is explicitly NOT a replacement for and must never be wired to
 * trigger. Safe to call with s == NULL (no-op). */
void nr_rx_branch_sync_apply_cfo(nr_rx_branch_sync_t *s, double hz);

#ifdef __cplusplus
}
#endif
#endif
