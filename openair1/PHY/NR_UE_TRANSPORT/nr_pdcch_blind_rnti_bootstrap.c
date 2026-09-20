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
#include <string.h>
#include <stdio.h>   // snprintf, for the BOOTTABLE diagnostic dump

// ---- Phase 3 Technique B: C-RNTI bootstrap, MULTI-UE ----------------------------------------
// A cell carries many UEs and a passive receiver hears all of them, so tracking ONE C-RNTI threw
// away every grant addressed to anyone else. This keeps a table.
//
// Confirmation is still "seen at least twice", because one sighting is indistinguishable from a
// false CRC pass. What changed on 2026-09-07 is that a confirmed entry is no longer DISPLACED by a
// newcomer: measured that day, the real C-RNTI 0x463d (194,460 genuine decodes) lost its slot to
// 0x8e6d -- a value the gNB never transmitted, 146 false passes -- because two of them happened to
// land consecutively, and the run then collected roughly half the grants it should have. With a
// table, a newcomer takes a FREE slot instead of the incumbent's, and eviction (when full) removes
// the weakest-evidenced entry rather than the oldest arrival.
#define RNTI_BOOTSTRAP_STALE_SLOTS 20000u  // ~10 s at this deployment's ~2000 slots/s

typedef struct {
  uint16_t rnti;
  uint8_t  cls;
  uint32_t last_slot;
  uint32_t sightings;  ///< 0 = free slot; 1 = pending; >=2 = confirmed
} nr_boot_entry_t;

static nr_boot_entry_t g_boot[NR_PDCCH_BLIND_MAX_UE];

static int boot_find(uint16_t rnti, uint8_t cls)
{
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE; i++) {
    if (g_boot[i].sightings > 0 && g_boot[i].rnti == rnti && g_boot[i].cls == cls) {
      return i;
    }
  }
  return -1;
}

/// Free slot, else the weakest-evidenced entry. Never evicts on age alone: a UE that is quiet for a
/// moment is still a real UE, whereas a noise RNTI never accumulates sightings.
static int boot_slot_for_new(void)
{
  int worst = 0;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE; i++) {
    if (g_boot[i].sightings == 0) {
      return i;
    }
    if (g_boot[i].sightings < g_boot[worst].sightings) {
      worst = i;
    }
  }
  return worst;
}

void nr_pdcch_blind_rnti_bootstrap_record(uint16_t rnti, uint8_t rnti_class, uint32_t abs_slot)
{
  // Only C-RNTI and TC-RNTI describe the DEDICATED search space this technique targets; SI-RNTI
  // (Phase 1's own domain) and P-RNTI carry no information about it.
  if (rnti_class != NR_BLIND_RNTI_CLASS_C && rnti_class != NR_BLIND_RNTI_CLASS_TC) {
    return;
  }
  const int i = boot_find(rnti, rnti_class);
  if (i >= 0) {
    if (g_boot[i].sightings < UINT32_MAX) {
      g_boot[i].sightings++;
    }
    g_boot[i].last_slot = abs_slot;
    return;
  }
  const int n = boot_slot_for_new();
  g_boot[n].rnti      = rnti;
  g_boot[n].cls       = rnti_class;
  g_boot[n].last_slot = abs_slot;
  g_boot[n].sightings = 1;  // pending; a single sighting is not evidence
}

static bool boot_entry_live(const nr_boot_entry_t *e, uint32_t now)
{
  if (e->sightings < 2) {
    return false;  // pending, not confirmed
  }
  const uint32_t age = (now >= e->last_slot) ? (now - e->last_slot) : 0;
  return age <= RNTI_BOOTSTRAP_STALE_SLOTS;
}

int nr_pdcch_blind_monitor_confirmed_rnti_set(uint32_t now_abs_slot, uint16_t *out, int max_out)
{
  if (out == NULL || max_out <= 0) {
    return 0;
  }
  int n = 0;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE && n < max_out; i++) {
    if (boot_entry_live(&g_boot[i], now_abs_slot)) {
      out[n++] = g_boot[i].rnti;
    }
  }
  return n;
}

bool nr_pdcch_blind_monitor_rnti_confirmed(uint32_t now_abs_slot, uint16_t rnti)
{
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE; i++) {
    if (g_boot[i].rnti == rnti && boot_entry_live(&g_boot[i], now_abs_slot)) {
      return true;
    }
  }
  return false;
}

/// Best-evidenced live entry. Kept for the single-RNTI consumers (Technique C's sweep and the
/// footprint log line), which want one representative UE rather than the whole set.
bool nr_pdcch_blind_monitor_confirmed_rnti(uint32_t now_abs_slot, uint16_t* rnti_out, uint8_t* class_out,
                                           uint32_t* age_slots_out)
{
  int best = -1;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE; i++) {
    if (!boot_entry_live(&g_boot[i], now_abs_slot)) {
      continue;
    }
    if (best < 0 || g_boot[i].sightings > g_boot[best].sightings) {
      best = i;
    }
  }
  if (best < 0) {
    return false;
  }
  *rnti_out = g_boot[best].rnti;
  *class_out = g_boot[best].cls;
  *age_slots_out = (now_abs_slot >= g_boot[best].last_slot) ? (now_abs_slot - g_boot[best].last_slot) : 0;
  return true;
}

/* DIAGNOSTIC. Everything downstream -- dedicated CORESET verification, the DL length sweep and the
 * UL PUSCH scan -- is gated on an entry reaching sightings >= 2 (boot_entry_live). When that never
 * happens the whole chain reads as "bootstrap_rnti=0x0" with no way to tell WHY from the outside:
 * a table of one-sighting entries (accepts are false, RNTIs never repeat) looks identical to an
 * empty table. Prints the raw table so the two are distinguishable. */
int nr_pdcch_blind_rnti_bootstrap_dump(uint32_t now_abs_slot, char *buf, int buflen)
{
  if (buf == NULL || buflen <= 0) {
    return 0;
  }
  int off = 0, live = 0, used = 0;
  for (int i = 0; i < NR_PDCCH_BLIND_MAX_UE && off < buflen - 32; i++) {
    if (g_boot[i].sightings == 0) {
      continue;
    }
    used++;
    const uint32_t age = (now_abs_slot >= g_boot[i].last_slot) ? (now_abs_slot - g_boot[i].last_slot) : 0;
    if (boot_entry_live(&g_boot[i], now_abs_slot)) {
      live++;
    }
    off += snprintf(buf + off, (size_t)(buflen - off), "%s0x%04x:n=%u,age=%u",
                    (off > 0) ? " " : "", g_boot[i].rnti, g_boot[i].sightings, age);
  }
  if (off < buflen - 24) {
    off += snprintf(buf + off, (size_t)(buflen - off), " | used=%d live=%d", used, live);
  }
  return off;
}

void nr_pdcch_blind_rnti_bootstrap_reset_for_test(void)
{
  memset(g_boot, 0, sizeof(g_boot));
}
