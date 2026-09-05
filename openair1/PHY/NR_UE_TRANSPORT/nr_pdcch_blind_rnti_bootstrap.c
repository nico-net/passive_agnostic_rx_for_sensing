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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c
 * \brief Phase 3 Technique B: standalone C-RNTI bootstrap state machine.
 *
 * Pure plumbing with no runtime dependencies -- this state tracks persistence-confirmed RNTIs
 * for the passive receiver, accumulating what the blind PDCCH monitor's accept path already
 * classifies (rnti_persistence_check). The test is unit-testable and has no hardware dependency.
 */

#include "nr_pdcch_blind_monitor.h"

// ---- Phase 3 Technique B: C-RNTI bootstrap --------------------------------------------------
// Two consecutive sightings of the SAME rnti confirm it, mirroring rnti_persistence_check()'s own
// "a real UE's RNTI recurs; a noise accept is (almost always) a one-off" rationale -- deliberately
// NOT re-implemented against a persistence window here, because THIS state only needs "have we
// seen this RNTI at least twice, ever" rather than a bounded time window; the age check below is
// what prevents an old confirmation from anchoring the scan forever.
#define RNTI_BOOTSTRAP_STALE_SLOTS 20000u  // ~10 s at this deployment's ~2000 slots/s

static uint16_t g_boot_rnti          = 0;
static uint8_t  g_boot_class         = 0xFF;
static uint32_t g_boot_last_slot     = 0;
static int      g_boot_confirmed     = 0;  // 0 = not confirmed, 1+ = confirmed (sighting count)
static uint16_t g_boot_pending_rnti  = 0;
static uint8_t  g_boot_pending_class = 0xFF;

void nr_pdcch_blind_rnti_bootstrap_record(uint16_t rnti, uint8_t rnti_class, uint32_t abs_slot)
{
  // Only C-RNTI and TC-RNTI describe the DEDICATED search space this technique targets; SI-RNTI
  // (Phase 1's own domain) and P-RNTI carry no information about it.
  if (rnti_class != NR_BLIND_RNTI_CLASS_C && rnti_class != NR_BLIND_RNTI_CLASS_TC) {
    return;
  }
  
  // Check if this is a second sighting of the pending RNTI
  if (rnti == g_boot_pending_rnti && rnti_class == g_boot_pending_class && g_boot_pending_rnti != 0) {
    // Second sighting of the same pending RNTI -> confirm it
    g_boot_rnti      = rnti;
    g_boot_class     = rnti_class;
    g_boot_last_slot = abs_slot;
    g_boot_confirmed = 1;  // Mark as confirmed
    return;
  }
  
  // Check if this is a repeat sighting of an already-confirmed RNTI
  if (rnti == g_boot_rnti && rnti_class == g_boot_class && g_boot_confirmed > 0) {
    // Already confirmed; a further sighting just refreshes staleness.
    g_boot_last_slot = abs_slot;
    g_boot_confirmed++;  // Track further sightings
    return;
  }
  
  // First sighting of a NEW candidate -- park it as pending, do not confirm on one sighting.
  g_boot_pending_rnti  = rnti;
  g_boot_pending_class = rnti_class;
}

bool nr_pdcch_blind_monitor_confirmed_rnti(uint32_t now_abs_slot, uint16_t* rnti_out, uint8_t* class_out,
                                           uint32_t* age_slots_out)
{
  if (g_boot_confirmed < 1) {
    return false;
  }
  // now_abs_slot >= g_boot_last_slot always holds in real RT use (slots only advance), but do not
  // assume it in a unit-testable function -- a caller passing an out-of-order "now" gets a benign
  // "not stale" answer via the unsigned-wrap guard below rather than an undefined huge age.
  const uint32_t age = (now_abs_slot >= g_boot_last_slot) ? (now_abs_slot - g_boot_last_slot) : 0;
  if (age > RNTI_BOOTSTRAP_STALE_SLOTS) {
    return false;
  }
  *rnti_out = g_boot_rnti;
  *class_out = g_boot_class;
  *age_slots_out = age;
  return true;
}

void nr_pdcch_blind_rnti_bootstrap_reset_for_test(void)
{
  g_boot_rnti = 0;
  g_boot_class = 0xFF;
  g_boot_last_slot = 0;
  g_boot_confirmed = 0;
  g_boot_pending_rnti = 0;
  g_boot_pending_class = 0xFF;
}
