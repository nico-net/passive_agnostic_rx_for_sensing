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
#ifndef NR_PDSCH_XOVERHEAD_H
#define NR_PDSCH_XOVERHEAD_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* xOverhead (PDSCH-ServingCellConfig, dedicated, never broadcast) enters the receiver only through
 * the TBS: N_RE per PRB = 12*L - N_DMRS - xOverhead. It is therefore observable through the TB CRC
 * -- but sweeping it as a 4th catalogue dimension would multiply Technique D's convergence time by 4
 * and push it past the 120 s captures we validate on. So this is REJECT-ONLY elimination, at zero
 * extra trials: a transport block that passed its CRC under TBS(x_used) cannot have been sized under
 * any x whose TBS for that same allocation differs. Every CRC-OK decode therefore REFUTES those
 * alternatives, and the assumed value is CONFIRMED once each of the other three has been refuted
 * at least once. An allocation for which two values happen to give the same TBS refutes nothing
 * (counted, not guessed) -- so a run with only such allocations stays UNRESOLVED rather than
 * inventing a confirmation. If the assumed value were wrong nothing would ever pass CRC, Technique D
 * would never settle, and the fallback (a real 4-way search) is the documented next step, not this. */
#define NR_XOH_CANDIDATES 4
static const uint16_t nr_xoh_values[NR_XOH_CANDIDATES] = {0, 6, 12, 18};

typedef struct {
  uint16_t assumed;                       // xOverhead the decoder is running with (REs per PRB)
  uint32_t crc_ok_seen;                   // CRC-OK decodes observed
  uint32_t refuted_by[NR_XOH_CANDIDATES]; // decodes whose TBS differed under this candidate
  uint32_t indistinct[NR_XOH_CANDIDATES]; // decodes whose TBS coincided (no information)
  bool     confirmed;                     // every alternative refuted at least once
} nr_pdsch_xoverhead_state_t;

/* Call for every PDSCH decode attempt with a valid TBS computation; only crc_ok decodes carry
 * evidence. Qm/R/nb_rb/nb_symb/nb_dmrs_re/tb_scaling/Nl are the exact nr_compute_tbs() inputs
 * the decode used. Returns true exactly when the assumed value becomes confirmed. */
bool nr_pdsch_xoverhead_observe(uint16_t Qm, uint16_t R, uint16_t nb_rb, uint16_t nb_symb,
                                uint16_t nb_dmrs_re, uint16_t used_oh, uint8_t tb_scaling,
                                uint8_t Nl, bool crc_ok);
nr_pdsch_xoverhead_state_t nr_pdsch_xoverhead_snapshot(void);
void nr_pdsch_xoverhead_reset(uint16_t assumed);

#ifdef __cplusplus
}
#endif
#endif
