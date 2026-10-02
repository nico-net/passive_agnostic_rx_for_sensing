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
 * called. NR_ACQ_SEARCHING therefore covers everything from "no SSB yet" through "PDCCH not
 * yet locked" as ONE state: this module cannot distinguish those without a new hook into PBCH
 * decode, which is out of scope here (see the handover doc's "Not yet done"). Do not read
 * NR_ACQ_SEARCHING as proof of SSB/PBCH failure specifically. */
#include "nr_passive_acq_state.h"
#include "common/utils/LOG/log.h"
#include <string.h>
#include <pthread.h>

static const char *state_names[NR_ACQ_NUM_STATES] = {
  "SEARCHING", "PBCH_LOCKED", "SIB1_DECODED", "PDCCH_LOCKED", "CORESET_VERIFIED",
  "CELL_CONFIGURED", "DL_CONVERGED", "UL_CONVERGED", "TRACKING", "LOST",
};
const char *nr_passive_acq_state_name(nr_passive_acq_state_t s)
{
  return (s >= 0 && s < NR_ACQ_NUM_STATES) ? state_names[s] : "INVALID";
}

static nr_passive_acq_snapshot_t g_snap = { .state = NR_ACQ_SEARCHING };
/* Event-latched evidence and the last polled inputs, so an event can re-evaluate immediately. */
static bool g_pbch_locked, g_sib1_decoded;
static nr_passive_acq_inputs_t g_last_in;
static nr_passive_acq_carrier_t g_carrier; static bool g_phy_geom_set;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Ordinal progress order. LOST is intentionally excluded (sentinel, ordinal -1): re-acquisition
 * from LOST is immediate once evidence supports it, only the SEARCHING->...->TRACKING direction
 * of travel is ever hysteresis-gated, and only on the way down. */
static int ordinal(nr_passive_acq_state_t s)
{
  switch (s) {
    case NR_ACQ_SEARCHING:         return 0;
    case NR_ACQ_PBCH_LOCKED:       return 1;
    case NR_ACQ_SIB1_DECODED:      return 2;
    case NR_ACQ_PDCCH_LOCKED:      return 3;
    case NR_ACQ_CORESET_VERIFIED:  return 4;
    case NR_ACQ_CELL_CONFIGURED:   return 5;
    case NR_ACQ_DL_CONVERGED:      return 6;
    case NR_ACQ_UL_CONVERGED:      return 6; // siblings: either one alone is equal progress
    case NR_ACQ_TRACKING:          return 7;
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
  if (g_sib1_decoded)               return NR_ACQ_SIB1_DECODED;
  if (g_pbch_locked)                return NR_ACQ_PBCH_LOCKED;
  return NR_ACQ_SEARCHING;
}
static void update_locked(const nr_passive_acq_inputs_t *in)
{
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
}
void nr_passive_acq_update(const nr_passive_acq_inputs_t *in)
{
  if (!in) return;
  pthread_mutex_lock(&g_lock);
  g_last_in = *in;
  update_locked(in);
  pthread_mutex_unlock(&g_lock);
}
void nr_passive_acq_note_pbch_locked(void)
{
  pthread_mutex_lock(&g_lock);
  ++g_snap.pbch_locks;
  g_pbch_locked = true;
  LOG_I(PHY, "SENSING: ACQ_EVENT pbch_locked (n=%lu, state=%s)\n", (unsigned long)g_snap.pbch_locks,
        nr_passive_acq_state_name(g_snap.state));
  update_locked(&g_last_in);
  pthread_mutex_unlock(&g_lock);
}
void nr_passive_acq_note_sib1(void)
{
  pthread_mutex_lock(&g_lock);
  ++g_snap.sib1_decodes;
  g_sib1_decoded = true;
  LOG_I(PHY, "SENSING: ACQ_EVENT sib1_decoded (n=%lu, state=%s)\n", (unsigned long)g_snap.sib1_decodes,
        nr_passive_acq_state_name(g_snap.state));
  update_locked(&g_last_in);
  pthread_mutex_unlock(&g_lock);
}
void nr_passive_acq_note_sync_loss(void)
{
  pthread_mutex_lock(&g_lock);
  const nr_passive_acq_state_t prev = g_snap.state;
  ++g_snap.sync_losses;
  g_pbch_locked = false; // the frame-to-sample mapping is what was lost; SIB1 facts still hold
  if (prev != NR_ACQ_LOST) {
    LOG_W(PHY, "SENSING: ACQ_STATE %s -> LOST (receive-stream discontinuity, no hysteresis; "
               "losses=%lu updates=%lu time_in_prev=%lu)\n",
          nr_passive_acq_state_name(prev), (unsigned long)g_snap.sync_losses,
          (unsigned long)g_snap.updates, (unsigned long)g_snap.time_in_state);
    g_snap.state = NR_ACQ_LOST;
    g_snap.time_in_state = 0;
    g_snap.consecutive_regressions = 0;
    ++g_snap.transitions;
  }
  pthread_mutex_unlock(&g_lock);
}
nr_passive_acq_carrier_verdict_t nr_passive_acq_verify_carrier(const nr_passive_acq_carrier_t *c)
{
  nr_passive_acq_carrier_verdict_t v = {0};
  v.bw_match = c->sib1_n_rb == c->phy_n_rb;
  v.mu_match = c->sib1_mu == c->phy_mu;
  /* FR1: offsetToPointA and k_SSB are in 15 kHz units; the grid is at mu. Convert to grid
   * subcarriers: one 15 kHz RB = 12 subcarriers of 15 kHz = 12 >> mu subcarriers at mu. Point A =
   * SSB subcarrier 0 - k_SSB(15k) - 12*offsetToPointA(15k), all expressed in the grid's own SCS. */
  const int sc15_per_sc = 1 << c->phy_mu;
  v.point_a_subcarrier = c->phy_ssb_start_subcarrier
                         - (c->sib1_k_ssb + 12 * c->sib1_offset_to_point_a) / sc15_per_sc;
  v.carrier_end_subcarrier = v.point_a_subcarrier + 12 * (c->sib1_offset_to_carrier + c->sib1_n_rb);
  /* The started grid covers exactly [0, 12*N_RB_DL). The carrier is consistent with it when its
   * first subcarrier (Point A + offsetToCarrier) is the grid's first and its last is the grid's
   * last -- the receiver was tuned to the cell's carrier, not merely somewhere that contains it. */
  v.grid_match = (v.point_a_subcarrier + 12 * c->sib1_offset_to_carrier == 0)
                 && (v.carrier_end_subcarrier == 12 * c->phy_n_rb);
  /* Absolute centre of the cell's carrier: the started grid's centre is subcarrier 6*N_RB_DL;
   * the SIB1 carrier's centre is Point A + 12*offsetToCarrier + 6*carrierBandwidth. */
  v.started_centre_hz = c->phy_dl_carrier_hz;
  if (c->phy_dl_carrier_hz > 0) {
    const double scs_hz = 15000.0 * (1 << c->phy_mu);
    const int sib1_centre_sc = v.point_a_subcarrier + 12 * c->sib1_offset_to_carrier + 6 * c->sib1_n_rb;
    v.derived_centre_hz = c->phy_dl_carrier_hz + (sib1_centre_sc - 6 * c->phy_n_rb) * scs_hz;
  }
  return v;
}
void nr_passive_acq_set_phy_geometry(int n_rb, int mu, int ssb_start_subcarrier, double dl_carrier_hz)
{
  pthread_mutex_lock(&g_lock);
  g_carrier.phy_n_rb = n_rb; g_carrier.phy_mu = mu; g_carrier.phy_ssb_start_subcarrier = ssb_start_subcarrier;
  g_carrier.phy_dl_carrier_hz = dl_carrier_hz;
  g_phy_geom_set = true;
  pthread_mutex_unlock(&g_lock);
}
static nr_tdd_config_t g_tdd;
void nr_passive_acq_note_sib1_tdd(const nr_tdd_pattern_t *p1, const nr_tdd_pattern_t *p2)
{
  nr_tdd_config_t t;
  const bool ok = nr_tdd_config_init(&t, p1, p2);
  pthread_mutex_lock(&g_lock);
  const bool first = !g_tdd.valid;
  if (ok)
    g_tdd = t;
  pthread_mutex_unlock(&g_lock);
  if (first || !ok)
    LOG_A(PHY, "SENSING: TDD from SIB1 %s: p1 period=%u slots dl=%u+%usym ul=%u+%usym | p2 period=%u dl=%u+%usym ul=%u+%usym\n",
          ok ? "DERIVED" : "REJECTED", p1->period_slots, p1->dl_slots, p1->dl_symbols, p1->ul_slots, p1->ul_symbols,
          p2 ? p2->period_slots : 0, p2 ? p2->dl_slots : 0, p2 ? p2->dl_symbols : 0, p2 ? p2->ul_slots : 0, p2 ? p2->ul_symbols : 0);
}
bool nr_passive_acq_tdd_known(void) { return g_tdd.valid; }
static int g_tdd_ref_mu = -1; /* BC9: SIB1 referenceSubcarrierSpacing; -1 = unknown */
void nr_passive_acq_note_sib1_tdd_ref_mu(int mu)
{
  pthread_mutex_lock(&g_lock);
  g_tdd_ref_mu = mu;
  pthread_mutex_unlock(&g_lock);
}
bool nr_passive_acq_tdd_pdsch_last_symbols(uint32_t dci_abs_slot, int mu, int n, int8_t *last)
{
  pthread_mutex_lock(&g_lock);
  const nr_tdd_config_t t = g_tdd;
  const int ref_mu = g_tdd_ref_mu;
  pthread_mutex_unlock(&g_lock);
  if (!t.valid || ref_mu < 0 || ref_mu != mu || last == NULL || n <= 0)
    return false;
  for (int k = 0; k < n; k++)
    last[k] = (int8_t)nr_tdd_pdsch_last_symbol(&t, dci_abs_slot + (uint32_t)k);
  return true;
}
bool nr_passive_acq_tdd_slot_has_downlink(uint32_t absolute_slot)
{
  pthread_mutex_lock(&g_lock); /* g_tdd is replaced by the SIB1 path: copy it consistently */
  const nr_tdd_config_t t = g_tdd;
  pthread_mutex_unlock(&g_lock);
  return t.valid ? nr_tdd_slot_has_downlink(&t, absolute_slot) : true;
}
void nr_passive_acq_note_sib1_carrier(int n_rb, int mu, int offset_to_point_a, int offset_to_carrier, int k_ssb)
{
  pthread_mutex_lock(&g_lock);
  g_carrier.sib1_n_rb = n_rb; g_carrier.sib1_mu = mu; g_carrier.sib1_offset_to_point_a = offset_to_point_a;
  g_carrier.sib1_offset_to_carrier = offset_to_carrier; g_carrier.sib1_k_ssb = k_ssb;
  if (!g_phy_geom_set) {
    LOG_W(PHY, "SENSING: ACQ carrier check skipped: PHY geometry never registered\n");
    pthread_mutex_unlock(&g_lock);
    return;
  }
  const nr_passive_acq_carrier_verdict_t v = nr_passive_acq_verify_carrier(&g_carrier);
  const bool ok = v.bw_match && v.mu_match && v.grid_match;
  if (g_snap.carrier_verified == 0 || (ok ? -1 : 1) == g_snap.carrier_verified) {
    if (ok)
      LOG_A(PHY, "SENSING: ACQ carrier CONFIRMED from SIB1: %d PRB mu=%d, Point A at grid subcarrier %d, "
                 "carrier ends at %d = grid end (started with %d PRB mu=%d, SSB found at subcarrier %d); "
                 "derived carrier centre %.6f MHz (started %.6f MHz)\n",
            n_rb, mu, v.point_a_subcarrier, v.carrier_end_subcarrier, g_carrier.phy_n_rb, g_carrier.phy_mu,
            g_carrier.phy_ssb_start_subcarrier, v.derived_centre_hz / 1e6, v.started_centre_hz / 1e6);
    else
      LOG_E(PHY, "SENSING: ACQ carrier MISMATCH: SIB1 says %d PRB mu=%d (Point A at grid subcarrier %d, "
                 "offsetToCarrier %d, carrier end %d) but PHY started with %d PRB mu=%d, grid end %d; "
                 "bw_match=%d mu_match=%d grid_match=%d -- the started sample grid is NOT this cell's carrier; "
                 "the cell's carrier centre is %.6f MHz, receiver started at %.6f MHz (retune target)\n",
            n_rb, mu, v.point_a_subcarrier, offset_to_carrier, v.carrier_end_subcarrier,
            g_carrier.phy_n_rb, g_carrier.phy_mu, 12 * g_carrier.phy_n_rb, v.bw_match, v.mu_match, v.grid_match,
            v.derived_centre_hz / 1e6, v.started_centre_hz / 1e6);
  }
  if (!ok) {
    /* BANDWIDTH / CENTRE ADAPTATION. The prose above tells a human the started grid is wrong; this
     * line tells the SUPERVISOR, which is where the correction belongs. Re-deriving N_RB_DL in
     * process would mean resizing the sample rate and every PHY buffer mid-stream; the receive chain
     * is sized once from the launch geometry. config_ue.c's own comment states the intended shape:
     * stop at broadcast facts and let the supervisor restart with the broadcast geometry. Emitted
     * once per verdict change, machine-readable, carrying everything a relaunch needs. The values
     * are SIB1's, i.e. measured off the air -- not the ones the receiver was started with. */
    static int s_retune_logged;
    if (!s_retune_logged) {
      s_retune_logged = 1;
      LOG_A(PHY, "SENSING: ISAC_ACQ_RETUNE {\"reason\":\"carrier_mismatch\",\"n_rb\":%d,\"mu\":%d,"
                 "\"centre_hz\":%.0f,\"started_n_rb\":%d,\"started_centre_hz\":%.0f,"
                 "\"bw_match\":%d,\"mu_match\":%d,\"grid_match\":%d}\n",
            n_rb, mu, v.derived_centre_hz, g_carrier.phy_n_rb, v.started_centre_hz,
            v.bw_match, v.mu_match, v.grid_match);
    }
  }
  g_snap.carrier_verified = ok ? 1 : -1;
  g_snap.carrier = v;
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
  memset(&g_last_in, 0, sizeof(g_last_in));
  g_pbch_locked = g_sib1_decoded = false;
  memset(&g_carrier, 0, sizeof(g_carrier)); g_phy_geom_set = false;
  g_snap.state = NR_ACQ_SEARCHING;
  pthread_mutex_unlock(&g_lock);
}
