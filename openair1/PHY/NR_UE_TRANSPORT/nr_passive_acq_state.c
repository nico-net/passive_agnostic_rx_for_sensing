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
/* This module has NO PBCH/SSB signal of its own -- the receiver's blind-PDCCH RT path (the only
 * verified call site, nr_pdcch_blind_monitor_rt.c) does not expose one at the point this is
 * called. NR_ACQ_SEARCHING_PDCCH therefore covers everything from "no SSB yet" through "PDCCH not
 * yet locked" as ONE state: this module cannot distinguish those without a new hook into PBCH
 * decode, which is out of scope here (see the handover doc's "Not yet done"). Do not read
 * NR_ACQ_SEARCHING_PDCCH as proof of SSB/PBCH failure specifically. */
#include "nr_passive_acq_state.h"
#include "common/utils/LOG/log.h"
#include <string.h>
#include <pthread.h>

static const char *state_names[NR_ACQ_NUM_STATES] = {
  "SEARCHING_PDCCH", "PDCCH_LOCKED", "CORESET_VERIFIED", "CELL_CONFIGURED",
  "DL_CONVERGED", "UL_CONVERGED", "TRACKING", "LOST",
};
const char *nr_passive_acq_state_name(nr_passive_acq_state_t s)
{
  return (s >= 0 && s < NR_ACQ_NUM_STATES) ? state_names[s] : "INVALID";
}

static nr_passive_acq_snapshot_t g_snap = { .state = NR_ACQ_SEARCHING_PDCCH };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Ordinal progress order. LOST is intentionally excluded (sentinel, ordinal -1): re-acquisition
 * from LOST is immediate once evidence supports it, only the SEARCHING->...->TRACKING direction
 * of travel is ever hysteresis-gated, and only on the way down. */
static int ordinal(nr_passive_acq_state_t s)
{
  switch (s) {
    case NR_ACQ_SEARCHING_PDCCH:   return 0;
    case NR_ACQ_PDCCH_LOCKED:      return 1;
    case NR_ACQ_CORESET_VERIFIED:  return 2;
    case NR_ACQ_CELL_CONFIGURED:   return 3;
    case NR_ACQ_DL_CONVERGED:      return 4;
    case NR_ACQ_UL_CONVERGED:      return 4; // siblings: either one alone is equal progress
    case NR_ACQ_TRACKING:          return 5;
    default:                       return -1; // NR_ACQ_LOST
  }
}
/* The state the CURRENT evidence alone supports, with no memory of history. Hysteresis (in
 * nr_passive_acq_update) decides whether a WORSE target is believed yet. */
static nr_passive_acq_state_t target_of(const nr_passive_acq_inputs_t *in)
{
  const bool ul_converged = in->ul_width_winners > 0 || in->ul_interp_winners > 0;
  const bool dl_converged = in->dl_search_winners > 0;
  if (ul_converged && dl_converged) return NR_ACQ_TRACKING;
  if (ul_converged)                 return NR_ACQ_UL_CONVERGED;
  if (dl_converged)                 return NR_ACQ_DL_CONVERGED;
  if (in->ul_bwp_known)             return NR_ACQ_CELL_CONFIGURED;
  if (in->coreset_extent_verified)  return NR_ACQ_CORESET_VERIFIED;
  if (in->pdcch_length_found)       return NR_ACQ_PDCCH_LOCKED;
  return NR_ACQ_SEARCHING_PDCCH;
}
void nr_passive_acq_update(const nr_passive_acq_inputs_t *in)
{
  if (!in) return;
  pthread_mutex_lock(&g_lock);
  const nr_passive_acq_state_t prev = g_snap.state;
  const nr_passive_acq_state_t target = target_of(in);
  ++g_snap.updates;
  bool moved = false;
  if (ordinal(target) >= ordinal(prev)) {
    /* Forward (or lateral, e.g. DL_CONVERGED<->UL_CONVERGED) progress, and re-acquisition out of
     * LOST: applied immediately. A decoded winner is a fact, not noise to debounce. */
    g_snap.consecutive_regressions = 0;
    moved = target != prev;
  } else {
    /* Evidence regressed (e.g. a context's winner was invalidated by relearning). Require
     * NR_PASSIVE_ACQ_LOSS_HYSTERESIS consecutive regressed updates before declaring LOST, so one
     * transient dip during in-progress relearning cannot look like an acquisition failure. */
    if (++g_snap.consecutive_regressions >= NR_PASSIVE_ACQ_LOSS_HYSTERESIS && prev != NR_ACQ_LOST) {
      moved = true;
    }
  }
  if (moved) {
    const nr_passive_acq_state_t next = (ordinal(target) >= ordinal(prev)) ? target : NR_ACQ_LOST;
    LOG_A(PHY, "SENSING: ACQ_STATE %s -> %s (updates=%lu time_in_prev=%lu regressions=%lu) "
               "evidence[len_found=%d coreset_ok=%d ul_bwp=%d dl_win=%lu ul_width_win=%lu "
               "ul_interp_win=%lu]\n",
          nr_passive_acq_state_name(prev), nr_passive_acq_state_name(next),
          (unsigned long)g_snap.updates, (unsigned long)g_snap.time_in_state,
          (unsigned long)g_snap.consecutive_regressions,
          in->pdcch_length_found, in->coreset_extent_verified, in->ul_bwp_known,
          (unsigned long)in->dl_search_winners, (unsigned long)in->ul_width_winners,
          (unsigned long)in->ul_interp_winners);
    g_snap.state = next;
    g_snap.time_in_state = 0;
    ++g_snap.transitions;
    if (ordinal(target) >= ordinal(prev)) g_snap.consecutive_regressions = 0;
  } else {
    ++g_snap.time_in_state;
  }
  pthread_mutex_unlock(&g_lock);
}
nr_passive_acq_snapshot_t nr_passive_acq_snapshot(void)
{
  pthread_mutex_lock(&g_lock);
  nr_passive_acq_snapshot_t s = g_snap;
  pthread_mutex_unlock(&g_lock);
  return s;
}
void nr_passive_acq_reset(void)
{
  pthread_mutex_lock(&g_lock);
  memset(&g_snap, 0, sizeof(g_snap));
  g_snap.state = NR_ACQ_SEARCHING_PDCCH;
  pthread_mutex_unlock(&g_lock);
}
