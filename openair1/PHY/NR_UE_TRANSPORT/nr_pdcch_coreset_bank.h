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

/* Number of verified geometries currently banked (0..NR_PDCCH_DISCOVERED_CORESETS). */
int nr_pdcch_coreset_bank_count(void);

/* Config of bank entry `index` (0 <= index < nr_pdcch_coreset_bank_count()). Never NULL for a
 * valid index; entries are immutable once published, safe to read without a lock. */
const nr_pdcch_blind_monitor_cfg_t *nr_pdcch_coreset_bank_cfg(int index);

/* Does an already-banked geometry cover this RB interval/symbol/mapping? */
bool nr_pdcch_coreset_bank_covers(int rb_offset, int span_rb, int duration, int symbol,
                                  int bundle, int interleaver, int shift, int dmrs_id);

/* Is `rnti` already an owner of some banked geometry? */
bool nr_pdcch_coreset_bank_has_owner(uint16_t rnti);

/* Modal dci_length_override across the bank (0 if none set). */
int nr_pdcch_coreset_bank_length_hint(void);

/* Add/merge a verified geometry, recording `owner` against it. */
void nr_pdcch_coreset_bank_add(const nr_pdcch_blind_monitor_cfg_t *cfg, uint16_t owner);

#endif
