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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_harq_init_tx.h
 * \brief Per-(RNTI, HARQ pid) record of the last TRANSPORT-BLOCK PARAMETERS a passive receiver
 * could resolve on its own, kept so a later RESERVED-MCS grant on the same HARQ process can still
 * be decoded. DL (TS 38.214 5.1.3.1): 29-31 for Table 5.1.3.1-1/-3 (qam64/qam64LowSE), 28-31 for
 * Table 5.1.3.1-2 (qam256). UL (TS 38.214 6.1.4.1) shares the SAME reserved boundary per table --
 * nr_get_Qm_ul()/nr_get_code_rate_ul() literally reuse Table_51311/51312/51313 for UL tables 0/1/2
 * -- plus two UL-only transform-precoded tables (61411/61412) at table index 3/4, ALSO 28-31. So:
 * 29-31 for tables 0/2, 28-31 for tables 1/3/4. (An earlier version of this file said UL was
 * "28-31 / 27-31 for qam256" -- corrected, G5 review, gap-harq: there is no "27-31" boundary.)
 *
 * A reserved MCS index carries no target code rate of its own: the spec says the UE "shall assume
 * the same modulation order and transport block size as the initial PDSCH/PUSCH transmission of the
 * same HARQ process". An ATTACHED UE tracks that trivially in its own HARQ process array. A passive
 * receiver has no such array -- this is it, deliberately tiny (the modulation order itself is
 * NOT what is missing: nr_get_Qm_dl()/nr_get_Qm_ul() already return the right value for a reserved
 * row straight from the spec table, e.g. Table_51311[29..31] = {2,0},{4,0},{6,0} -- only the code
 * rate is zero there, which is what makes TBS/base-graph unrecoverable without history).
 *
 * NDI is the correctness gate, not a convenience: TS 38.321 5.3.2.2 says "same NDI, same HARQ
 * process" IS the definition of a retransmission. A record whose NDI does not match the current
 * grant means either the true initial transmission was never observed, or one was and this receiver
 * missed it -- either way there is nothing safe to reuse, and the caller must refuse the grant
 * exactly as it did before this table existed, rather than guess.
 *
 * Header-only (static inline, no PHY/UE dependencies) so it can be unit-tested standalone -- see
 * tests/nr_harq_init_tx_test.cc -- and so the passive DL and UL decoders, which do not share a
 * per-RNTI table, can each keep their own instance without duplicating the logic.
 */

#ifndef NR_HARQ_INIT_TX_H
#define NR_HARQ_INIT_TX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// One HARQ process's last-known-resolvable transport-block parameters.
typedef struct {
  bool     used;      ///< false = empty slot
  uint16_t rnti;
  uint8_t  pid;       ///< HARQ process number
  uint8_t  ndi;       ///< NDI the record was taken under
  uint8_t  qm;        ///< modulation order (bits/symbol), for reference/consistency checks only --
                      ///< the reserved row's OWN Qm (from the DCI's own MCS field) is already
                      ///< correct and is what a caller should actually use
  uint8_t  nl;        ///< number of layers of the recorded transmission, same caveat as qm: a
                      ///< retransmission's own antenna-ports field is its own layer count, this is
                      ///< kept for logging a mismatch, not for overriding it
  uint8_t  bg;        ///< LDPC base graph (1 or 2), reused as-is on a reserved-MCS retransmission
  uint32_t tbs;       ///< transport block size, bits -- THE field a reserved MCS cannot supply
  uint32_t code_rate; ///< target code rate (x1024 scale, OAI convention), for diagnostics only
  uint64_t touched;
} nr_harq_init_tx_t;

/// Fixed-size, RNTI+pid-keyed, least-recently-used table. One instance per (module, direction): the
/// passive DL and UL decoders each keep their own, so a UL HARQ pid can never collide with a DL one
/// that happens to share the same number. 64 (G5 review, gap-harq; was 16): a single DL HARQ-pid
/// field is up to 5 bits (`harq-ProcessNumberSizeDCI-1-1`), i.e. up to 32 live processes for ONE
/// RNTI alone -- 16 total was undersized the moment more than one RNTI was live at all, let alone
/// one RNTI using every process. 64 covers 2 UEs at the 5-bit maximum with headroom, same rationale
/// nr_pdsch_passive_decode.c's own per-RNTI tables (RNTI_DEC_MAX) already use.
#define NR_HARQ_INIT_TX_N 64

typedef struct {
  nr_harq_init_tx_t e[NR_HARQ_INIT_TX_N];
  uint64_t clock;
  uint64_t hits;   ///< nr_harq_init_tx_lookup() successes -- surfaced on the live stats line (G5 review)
  uint64_t evicts; ///< nr_harq_init_tx_record() calls that evicted a DIFFERENT (rnti, pid)'s entry
} nr_harq_init_tx_table_t;

/// NULL when (rnti, pid) has no slot yet.
static inline nr_harq_init_tx_t *nr_harq_init_tx_find(nr_harq_init_tx_table_t *t, uint16_t rnti, uint8_t pid)
{
  for (int i = 0; i < NR_HARQ_INIT_TX_N; i++)
    if (t->e[i].used && t->e[i].rnti == rnti && t->e[i].pid == pid)
      return &t->e[i];
  return NULL;
}

/// Record (or refresh) this (rnti, pid)'s transport-block parameters, from a grant whose OWN mcs
/// was resolvable (never called for a reserved-MCS grant, which only reads via the lookup below).
/// Evicts the least-recently-touched slot when the table is full and (rnti, pid) is new -- bounded
/// state, wraps around rather than growing, across an arbitrarily long capture.
static inline void nr_harq_init_tx_record(nr_harq_init_tx_table_t *t, uint16_t rnti, uint8_t pid, uint8_t ndi,
                                          uint8_t qm, uint8_t nl, uint8_t bg, uint32_t tbs, uint32_t code_rate)
{
  nr_harq_init_tx_t *e = nr_harq_init_tx_find(t, rnti, pid);
  if (e == NULL) {
    e = &t->e[0];
    for (int i = 1; i < NR_HARQ_INIT_TX_N; i++)
      if (!t->e[i].used || t->e[i].touched < e->touched)
        e = &t->e[i];
    if (e->used)
      t->evicts++; // a genuinely different (rnti, pid) occupied this slot, not just an empty one
  }
  e->used = true;
  e->rnti = rnti;
  e->pid = pid;
  e->ndi = ndi;
  e->qm = qm;
  e->nl = nl;
  e->bg = bg;
  e->tbs = tbs;
  e->code_rate = code_rate;
  e->touched = ++t->clock;
}

/// True iff (rnti, pid) has a recorded transmission whose NDI matches `ndi` -- i.e. no new data has
/// started on this HARQ process since the record was taken, so its TBS/base-graph are still what a
/// reserved-MCS grant on this process means. False (including "never recorded") means the true
/// initial transmission was missed: the caller must refuse rather than guess.
static inline bool nr_harq_init_tx_lookup(nr_harq_init_tx_table_t *t, uint16_t rnti, uint8_t pid, uint8_t ndi,
                                          nr_harq_init_tx_t *out)
{
  const nr_harq_init_tx_t *e = nr_harq_init_tx_find(t, rnti, pid);
  if (e == NULL || e->ndi != ndi)
    return false;
  *out = *e;
  t->hits++;
  return true;
}

#ifdef __cplusplus
}
#endif

#endif // NR_HARQ_INIT_TX_H
