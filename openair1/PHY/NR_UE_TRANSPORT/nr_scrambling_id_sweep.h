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
#include <stdbool.h>
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

/* ---- WHICH GRANTS CARRY THE RRC-DEDICATED IDENTITIES (final review I2) -------------------------
 * TS 38.211 7.3.1.1 / 6.3.1.1: dataScramblingIdentityPDSCH/PUSCH applies only when the RNTI is a
 * C-RNTI (or MCS-C-/CS-RNTI) AND the transmission is not scheduled by DCI 1_0 / 0_0 in a common search
 * space -- exactly OAI's own UE condition (nr_ue_procedures.c: TYPE_C_RNTI_ && ss_type != common;
 * nr_ue_scheduler.c: TYPE_C_RNTI_ && !(0_0 && common)). SI-, RA-, TC- and P-RNTI always use N_ID^cell.
 * The DM-RS scramblingID0/1 (7.4.1.1.1 / 6.4.1.1.1.1) are likewise C-RNTI-class only; this receiver
 * applies the SAME predicate to them, i.e. N_ID^cell also for a C-RNTI fallback DCI in a CSS (the spec
 * would allow scramblingID0 there): the blind monitor cannot reliably tell a TC-RNTI from a C-RNTI in
 * a CSS, and a wrong identity on a TC/Msg4 grant is fatal while N_ID^cell on a rare CSS C-RNTI grant
 * only costs that grant. The decided/latched identities are therefore applied to, and learnt from,
 * exactly the grants this returns true for. */
static inline bool nr_scrambling_dedicated(bool c_rnti_class, bool fallback_dci, bool common_ss)
{
  return c_rnti_class && !(fallback_dci && common_ss);
}

/* ---- LINK HEALTH (final review I1 / C1) ----------------------------------------------------------
 * "The link is healthy while this class fails": a CRC pass on a grant that does NOT use the dedicated
 * identities (SIB1, RAR, paging, CSS fallback -- all N_ID^cell), or a dedicated pass by ANOTHER RNTI,
 * within the last NR_SCR_LINK_WINDOW noted outcomes. 256 outcomes is several SIB1 periods on any cell
 * that carries traffic, and far more than the NR_SCR_WALK_MIN_FAILS streak that must fall inside it.
 * Multi-writer (every decode consumer): fields are accessed with __atomic builtins; a torn
 * (rnti, n) pair costs at most one wrong answer and never corrupts the counters. */
#define NR_SCR_LINK_WINDOW 256
typedef struct {
  uint64_t n;             ///< outcomes noted
  uint64_t common_pass_n; ///< n at the last pass of a non-dedicated (N_ID^cell) grant, 0 = never
  uint64_t ded_pass_n;    ///< n at the last pass of a dedicated grant, 0 = never
  uint32_t ded_pass_rnti; ///< ... and its RNTI
} nr_scr_link_t;
void nr_scr_link_note(nr_scr_link_t *l, uint16_t rnti, bool dedicated, bool crc_ok);
/* True when something OTHER than `rnti`'s dedicated grants passed recently (see above). */
bool nr_scr_link_healthy(const nr_scr_link_t *l, uint16_t rnti);

/* ---- WALK ELIGIBILITY (final review I1) ----------------------------------------------------------
 * The data-identity walk may move only when (a) the DM-RS identity of the grant's nSCID is DECIDED by
 * its margin gate (the coherence estimator needs no CRC, so this is not circular, unlike the former
 * "Technique D converged" gate: convergence needs CRC passes, which a wrong data identity never gives),
 * (b) the link is healthy (nr_scr_link_healthy), and (c) this class has failed CRC on at least
 * NR_SCR_WALK_MIN_FAILS consecutive dedicated TBs (window reset by any pass). */
#define NR_SCR_WALK_MIN_FAILS 20
static inline bool nr_scrambling_walk_eligible(int dmrs_decided_id, bool link_healthy, uint32_t dedicated_fails_since_ok)
{
  return dmrs_decided_id >= 0 && link_healthy && dedicated_fails_since_ok >= NR_SCR_WALK_MIN_FAILS;
}

#ifdef __cplusplus
}
#endif
#endif
