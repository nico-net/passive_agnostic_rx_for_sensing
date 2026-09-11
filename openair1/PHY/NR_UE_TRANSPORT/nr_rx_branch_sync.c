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

#include "nr_rx_branch_sync.h"
#include "common/utils/LOG/log.h"

void nr_rx_branch_sync_reset(nr_rx_branch_sync_t *s)
{
  if (!s)
    return;
  s->cfo_hz = 0.0;
  s->cfo_accum_hz = 0.0;
  s->timing_offset_samples = 0;
  s->shift_for_next_frame = 0;
  s->synchronized = 0;
  /* last_absolute_slot, frame_wraps, snap_lock_epoch, snap_acq_epoch: deliberately untouched --
   * see the header comment on this function and on the struct fields themselves. */
}

int nr_rx_branch_sync_on_slot(nr_rx_branch_sync_t *s, int64_t absolute_slot, int32_t slots_per_frame)
{
  if (!s || slots_per_frame <= 0) {
    LOG_E(PHY, "rx_branch_sync: on_slot called with s=%p slots_per_frame=%d\n", (void *)s,
          slots_per_frame);
    return -1;
  }

  const int64_t delta = absolute_slot - s->last_absolute_slot;
  if (delta < 0 && -delta > slots_per_frame) {
    LOG_E(PHY,
          "rx_branch_sync: out-of-order slot rejected (last=%ld new=%ld delta=%ld > "
          "slots_per_frame=%d)\n",
          (long)s->last_absolute_slot, (long)absolute_slot, (long)delta, slots_per_frame);
    return -1;
  }

  const int64_t prev_frame = s->last_absolute_slot / slots_per_frame;
  const int64_t new_frame = absolute_slot / slots_per_frame;
  if (new_frame < prev_frame)
    s->frame_wraps++;

  /* Fix round 1 (controller-ruled): last_absolute_slot is a HIGH-WATER MARK -- it only ever
   * advances, never regresses, even on an accepted small-backward (frame-wrap) step above. Before
   * this fix, an accepted backward step lowered last_absolute_slot, so a LATER job whose value
   * fell between the old (higher) and new (lower) value would compute a POSITIVE delta against
   * the lowered reference and sail through the forward-progress path above with no backward/
   * out-of-order check at all -- silently re-admitting the exact "unsigned slot delta breaks
   * concurrent CPI" defect class this function exists to reject. Ratcheting means every future
   * call is always evaluated against the TRUE historical maximum, so a value below it always
   * takes the backward/tolerance-checked branch, never the unchecked forward one. See
   * nr_rx_branch_sync_test.cc's RxBranchSyncOnSlot.HighWaterMark* cases. */
  if (absolute_slot > s->last_absolute_slot)
    s->last_absolute_slot = absolute_slot;
  return 0;
}

int nr_rx_branch_sync_is_stale(const nr_rx_branch_sync_t *s, const nr_rx_branch_t *branch)
{
  if (!s || !branch) {
    LOG_E(PHY, "rx_branch_sync: is_stale called with s=%p branch=%p\n", (const void *)s,
          (const void *)branch);
    return 1; /* fail safe: unverifiable state is treated as stale */
  }
  return (s->snap_lock_epoch != branch->lock_epoch) || (s->snap_acq_epoch != branch->acq_epoch);
}

void nr_rx_branch_sync_apply_cfo(nr_rx_branch_sync_t *s, double hz)
{
  if (!s)
    return;
  s->cfo_hz = hz;
  s->cfo_accum_hz += hz;
}
