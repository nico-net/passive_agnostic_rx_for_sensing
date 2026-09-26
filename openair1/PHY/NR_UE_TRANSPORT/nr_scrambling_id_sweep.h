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
#ifndef NR_SCRAMBLING_ID_SWEEP_H
#define NR_SCRAMBLING_ID_SWEEP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Blind discovery of dataScramblingIdentityPDSCH / PUSCH (0..1023, TS 38.211 7.3.1.1 / 6.3.1.1):
 * unlike the DM-RS identity (nr_dmrs_id_estimate.h), there is no coherence statistic that can score
 * a candidate without decoding -- the TB CRC is the only oracle. So this is an ORDERED walk of TB-
 * CRC hypotheses, not an accumulator: try one candidate per grant, keep the first one whose CRC
 * passes.
 *
 * Order matters for cost, not correctness: PCI first (the assumption every deployment has used so
 * far), then the DM-RS identity if one has been decided and is in range (a gNB that mis-set one
 * dedicated scrambling id often mis-set the other the same way), then the rest of 0..1023 in plain
 * order. A cell matching the common case latches on try 1; the worst case is still bounded at 1024
 * LDPC decodes, not unbounded. */
typedef struct {
  uint16_t order[1024];
  int      n;        // number of distinct candidates in order[] (<= 1024)
  int      pos;       // current walk position into order[]
  int      latched;   // -1 until a CRC pass confirms one; then the confirmed id (order[pos] at the time)
  uint32_t tries;      // feed() calls so far, informational
} nr_scrambling_id_sweep_t;

/* dmrs_id < 0 means "no DM-RS decision yet" -- skipped. A dmrs_id outside 0..1023 (the DM-RS
 * identity space is 0..65535, wider than the data identity's 0..1023) cannot be a valid data
 * identity either and is likewise skipped, falling through to the plain 0..1023 walk. */
void nr_scrambling_id_sweep_init(nr_scrambling_id_sweep_t *s, uint16_t pci, int dmrs_id);

/* The identity to decode THIS grant with. -1 if s is NULL/empty (should not happen after init). */
int nr_scrambling_id_sweep_current(const nr_scrambling_id_sweep_t *s);

/* Report the outcome of decoding with nr_scrambling_id_sweep_current()'s value. A pass latches (the
 * walk stops there for good); a fail advances to the next candidate. No-op once latched. */
void nr_scrambling_id_sweep_feed(nr_scrambling_id_sweep_t *s, int tb_crc_ok);

#ifdef __cplusplus
}
#endif
#endif
