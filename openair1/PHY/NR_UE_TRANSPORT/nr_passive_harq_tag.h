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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_passive_harq_tag.h
 * \brief nrLDPC_coding_interface harq_unique_pid namespace for the passive DL decode.
 *
 * A hardware LDPC accelerator keys its per-transport-block state on harq_unique_pid
 * (nrLDPC_coding_aal.c:654/742 indexes its segment buffers by
 * `harq_unique_pid * NR_LDPC_MAX_NUM_CB`), so two transport blocks that are in flight at the same
 * moment under the same id silently overwrite each other's segments. The id must therefore be
 * unique across every concurrent user of the ONE dlopen'd coding interface.
 *
 * The namespace map, 1000 apart per submitter TYPE (see docs/passive_branch_namespace_audit.md for
 * the full audit, including the uplink findings this header deliberately does NOT change):
 *      0..31  nr_dlsch_decoding.c:76        attached UE DL decode, 2*harq_pid + cw_idx
 *   1000..    phy_procedures_nr_ue.c:2137   attached UE DL re-encode, 1000 + harq_process_nbr
 *   2000..    THIS header                   passive DL decode
 *   3000..    nr_pdcch_blind_monitor_rt.c:96 passive DL re-encode, per-UE stride of 16
 *   4000..    nr_pusch_data_aided.h:23      passive UL re-encode, + decode-context index
 *
 * P09 (adaptive_RX_pipeline.md Stage 2): the 2000 range was namespaced by submitter type only.
 * P06a's fan-out enqueues the SAME grant to N branches concurrently, and each branch's job carries
 * its own independently-numbered `harq_process_nbr`, so `BASE + harq_process_nbr` aliases between
 * branch 0's and branch 1's job on the same occasion -- by construction, not as a corner case.
 * The id is therefore strided by branch as well. branch_id 0 (legacy / single branch) reproduces
 * the pre-P09 value exactly.
 */

#ifndef NR_PASSIVE_HARQ_TAG_H
#define NR_PASSIVE_HARQ_TAG_H

#include <assert.h> /* static_assert in C11 as well as C++ */
#include <stdint.h>

#include "nr_rx_branch.h" /* NR_RX_BRANCH_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/// Spacing between adjacent submitter-type bases in the map above. The per-type range must not
/// grow past it, or one type's ids start aliasing the next type's.
#define NR_PASSIVE_HARQ_NAMESPACE_SPAN 1000u

/// Base of the passive DL decode range (moved here from nr_pdsch_passive_decode.c so the bound
/// below can be checked where the stride is defined).
#define NR_PDSCH_PASSIVE_HARQ_TAG_BASE 2000u

/// Ids reserved per branch inside that range. 32 = the full span of the DCI HARQ-process-number
/// field at its WIDEST configurable width: the field is 4 bits by default but 5 with
/// harq-ProcessNumberSizeDCI-1-1, and this receiver's own DCI parser is told which
/// (`nr_pdcch_blind_monitor.h:263` `harq_pid_bits`, "default 4; 5 with
/// harq-ProcessNumberSizeDCI-1-1", the 7th operator-settable field of
/// `pdcch_blind_monitor_dci_bits`, read at that width by `nr_pdcch_blind_monitor.c:2475`).
/// Striding at 16 would therefore alias harq process 0 with harq process 16 ON THE SAME BRANCH on
/// any deployment that configures the 5-bit field -- the same collision this header exists to
/// remove, one axis over -- and would also break the branch-0 legacy-tag identity above hpn 15.
#define NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE 32u

/* Bound, done as arithmetic rather than by picking a comfortable-looking constant:
 *   highest id = BASE + (NR_RX_BRANCH_MAX - 1) * STRIDE + (STRIDE - 1)
 *              = 2000 + 3 * 32 + 31 = 2127
 * and the next submitter type starts at BASE + SPAN = 3000, so 2127 < 3000 holds with 873 ids of
 * headroom. The assert is what keeps that true if NR_RX_BRANCH_MAX or the stride is ever raised
 * (NR_RX_BRANCH_MAX would have to reach 31 before it fails). */
static_assert((NR_RX_BRANCH_MAX - 1) * NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE
                      + (NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE - 1)
                  < NR_PASSIVE_HARQ_NAMESPACE_SPAN,
              "passive DL harq_unique_pid range overflows into the next submitter type's namespace");

/**
 * @brief harq_unique_pid for one passive DL decode.
 * @param branch_id         P07 branch identity of the job being decoded (0 in legacy mode).
 * @param harq_process_nbr  the grant's DCI HARQ process number.
 *
 * Both arguments are reduced into their declared ranges rather than trusted: harq_process_nbr comes
 * from a BLINDLY decoded DCI and branch_id from a job the producer filled, so a malformed value must
 * stay inside this type's namespace (where it can at worst alias another passive DL decode) instead
 * of running into the 3000 re-encode range (where it would alias a different submitter entirely).
 * Same defensive pattern as nr_pdcch_blind_monitor_rt.c's blind_harq_tag().
 */
static inline uint32_t nr_pdsch_passive_harq_tag(uint8_t branch_id, uint8_t harq_process_nbr)
{
  return NR_PDSCH_PASSIVE_HARQ_TAG_BASE
         + (uint32_t)(branch_id % NR_RX_BRANCH_MAX) * NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE
         + (uint32_t)(harq_process_nbr % NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE);
}

#ifdef __cplusplus
}
#endif

#endif /* NR_PASSIVE_HARQ_TAG_H */
