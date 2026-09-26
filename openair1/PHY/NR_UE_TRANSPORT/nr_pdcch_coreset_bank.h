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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.h
 * \brief Multi-CORESET bank: pure data + bookkeeping (which verified dedicated CORESET geometries
 * are operational, and who owns each). Moved out of nr_pdcch_blind_monitor_rt.c (2026-09-25, see
 * fix-link-report.md's "Fix round 1" / ruling R14): the block has no RT-symbol dependency, so it
 * belongs in the offline-gtest-linked nr_pdcch_blind_monitor library, not the RT-only translation
 * unit -- that is also what let nr_pdcch_blind_monitor.c's own bank lookup
 * (nr_pdcch_blind_monitor_bank_has_geometry) link without a test stub standing in for RT code.
 * nr_pdcch_blind_monitor_rt.c now reads/writes the bank exclusively through these accessors.
 */

#ifndef __NR_PDCCH_CORESET_BANK_H__
#define __NR_PDCCH_CORESET_BANK_H__

#include <stdint.h>
#include <stdbool.h>
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h" // nr_pdcch_blind_monitor_cfg_t (defined there; pulls in nr_pdcch_blind_monitor.h too, same pattern nr_pdcch_blind_monitor.c already uses for this same struct)
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.h" // nr_pdcch_al1_map_t, NR_PDCCH_AL1_MAX_FAM/_UNION_MAX

/* One bank entry. cfg/owners are immutable after the release-store publishes the entry.
 * The AL1 UNION fields (Task 15) change after publication; nr_pdcch_blind_monitor_rt.c guards them
 * with its g_al1_mu. Exposed (not hidden behind per-field accessors) because the RT union decoding
 * reads and narrows them in place under that mutex. */
typedef struct {
  nr_pdcch_blind_monitor_cfg_t cfg;
  uint16_t owners[NR_PDCCH_BLIND_MAX_UE];
  uint8_t nowners;
  /* AL1 UNION. Unlike cfg these change after publication, so they are guarded by g_al1_mu.
   * al1_fam: one mapping per AL1 family still consistent with the evidence (0 = never ambiguous);
   * al1_union: the deduplicated AL1 REG sets of {banked mapping} + al1_fam, the banked family first
   * in CCE order -- so entries >= the CORESET's CCE count are exactly the sets the banked mapping
   * alone would never demap. */
  nr_pdcch_al1_map_t al1_fam[NR_PDCCH_AL1_MAX_FAM];
  uint8_t n_al1_fam;
  uint8_t al1_bank_out;   /* 1 = AL1 evidence excluded the banked mapping's own family */
  uint16_t n_al1_union;
  uint16_t al1_union[NR_PDCCH_AL1_UNION_MAX][6];
} nr_pdcch_discovered_coreset_t;

/* Number of verified geometries currently banked (0..NR_PDCCH_DISCOVERED_CORESETS). */
int nr_pdcch_coreset_bank_count(void);

/* Config of bank entry `index` (0 <= index < nr_pdcch_coreset_bank_count()). Never NULL for a
 * valid index; entries are immutable once published, safe to read without a lock. */
const nr_pdcch_blind_monitor_cfg_t *nr_pdcch_coreset_bank_cfg(int index);

/* Whole bank entry `index` (0 <= index < nr_pdcch_coreset_bank_count()), for the AL1 UNION fields. */
nr_pdcch_discovered_coreset_t *nr_pdcch_coreset_bank_entry(int index);

/* Does an already-banked geometry cover this RB interval/symbol/mapping? */
bool nr_pdcch_coreset_bank_covers(int rb_offset, int span_rb, int duration, int symbol,
                                  int bundle, int interleaver, int shift, int dmrs_id);

/* Is `rnti` already an owner of some banked geometry? */
bool nr_pdcch_coreset_bank_has_owner(uint16_t rnti);

/* Modal dci_length_override across the bank (0 if none set). */
int nr_pdcch_coreset_bank_length_hint(void);

/* Add/merge a verified geometry, recording `owner` against it. Returns the entry's index (an existing
 * entry's index when the geometry is already banked), or -1 when nothing was archived. A newly
 * taken slot is zeroed (AL1 UNION fields included) before publication. */
int nr_pdcch_coreset_bank_add(const nr_pdcch_blind_monitor_cfg_t *cfg, uint16_t owner);

#endif
