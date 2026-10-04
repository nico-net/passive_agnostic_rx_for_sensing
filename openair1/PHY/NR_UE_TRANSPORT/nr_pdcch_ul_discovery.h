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

#ifndef NR_PDCCH_UL_DISCOVERY_H
#define NR_PDCCH_UL_DISCOVERY_H
#include "nr_dci_bits.h"
#include "nr_pdcch_blind_monitor.h"
/* Thread-safe controller. Keeps independent contexts for up to 16 bootstrapped UEs; identity/context changes
 * discard evidence and advance the generation echoed through the grant book. */
typedef struct {
  uint64_t generation, width_trials, interp_trials;
  int width_classes, interp_classes, raw_samples;
  int width_winners, interp_winners;
  uint64_t rejected_feedback;
  int interp_refusals; ///< bounded joint catalog refused, baseline remains available
} nr_pdcch_ul_discovery_snapshot_t;
nr_pdcch_ul_discovery_snapshot_t nr_pdcch_ul_discovery_snapshot(void);
void nr_pdcch_ul_discovery_reset(void);
bool nr_pdcch_ul_discovery_grant(const nr_pdcch_blind_ul_opts_t *fixed,
                                 uint16_t length, uint16_t confirmed_rnti,
                                 nr_dci_bits_t payload, nr_pdcch_blind_ul_result_t *out);
/* Only call for an actual decoded transport block; never for dropped, stale, CFR-only,
 * unsupported, or setup-error jobs. crc_ok must mean non-zero TB with verified CRC.
 * Exactly one producer-time class owns the result, including settled grants;
 * missing/dual owners, identity mismatches and stale generations cannot score. */
void nr_pdcch_ul_discovery_feedback(const nr_pdcch_blind_ul_result_t *, bool crc_ok);
#endif
