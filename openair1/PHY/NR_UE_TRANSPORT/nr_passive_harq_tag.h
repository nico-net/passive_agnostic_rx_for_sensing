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
 *   5000..    THIS header                   passive UL decode, + decode-context index
 *
 * CAVEAT, added P08a fix round 1 -- THE ACCELERATOR PRUNES THIS ID, so the disjointness this map
 * provides is disjointness in SOFTWARE, not necessarily at the device. nrLDPC_coding_aal.c:654-656:
 *
 *     segment_offset        = harq_unique_pid * NR_LDPC_MAX_NUM_CB + i      (NR_LDPC_MAX_NUM_CB 144)
 *     pruned_segment_offset = segment_offset % active_dev.num_harq_codeblock (default 512,
 *                             nrLDPC_coding_aal.c:1083, operator-settable)
 *
 * Two ids p != q therefore land on the SAME device slot for the same segment index iff
 * 144*(p-q) == 0 (mod 512); gcd(144,512) = 16, so that is (p-q) == 0 (mod 32). The aliasing period
 * is num_harq_codeblock / gcd(NR_LDPC_MAX_NUM_CB, num_harq_codeblock) = 32 at the defaults.
 * Consequences worth knowing before trusting a base on hardware:
 *   - the 1000-apart base spacing is a multiple of 32, so e.g. 5000 (passive UL decode, ctx 0) and
 *     1000 (attached DL re-encode, harq_process_nbr 0) alias after pruning. Moot INSIDE a
 *     --passive-rx process (the 1000 range is attached-only and the two never coexist), but it is
 *     not the device-level guarantee the map's shape suggests;
 *   - worse, the DL branch stride is itself 32, so branch b and branch b+1 at the same
 *     harq_process_nbr differ by exactly 32 and alias at the device -- the very collision that
 *     stride removes in software;
 *   - and fundamentally, 512/144 = 3.55, so a default-configured device holds barely three ids'
 *     worth of segments. No choice of bases makes more than that many CONCURRENT ids safe: the
 *     operator-side requirement is num_harq_codeblock >= 144 * (concurrent ids in flight).
 * Ideally the bases (and strides) would be chosen co-prime-safe against
 * num_harq_codeblock / NR_LDPC_MAX_NUM_CB rather than at a round 1000. That is a change to the
 * WHOLE map (P09's design, inherited, not introduced by P08a) and is deliberately NOT made here;
 * it is recorded so nobody reads this map as a hardware guarantee. This tree links
 * nrLDPC_coding_segment, which keeps no per-id state, so none of it is reachable today.
 *
 * P08a (adaptive_RX_pipeline.md Stage 2, finishing P09's audit finding 4.1): the passive UL decode
 * had NO namespace at all -- nr_ulsch_decoding.c set harq_unique_pid = ULSCH_id, which the passive
 * path pins at 0 in every one of its NR_PUSCH_PASSIVE_MAX_CTX concurrent decode contexts, so all of
 * them (and the attached DL decode's harq_pid=0/cw=0) shared id 0. The 5000 range below strides it
 * by decode context. The file's own PASSIVE_UL_HARQ_TAG_BASE 4000 was dead code AND collided with
 * the UL re-encode base; it is removed rather than revived.
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

/* ------------------------------------------------------------------------------------------- *
 * Passive UPLINK decode (5000 range).
 * ------------------------------------------------------------------------------------------- */

/// Base of the passive UL decode range. 5000 and not 4000: 4000 is already the passive UL
/// RE-ENCODE base (nr_pusch_data_aided.h:23, in use), which is exactly what the dead
/// PASSIVE_UL_HARQ_TAG_BASE 4000 in nr_pusch_passive_decode.c would have collided with had anyone
/// wired it up.
#define NR_PUSCH_PASSIVE_HARQ_TAG_BASE 5000u

/// Concurrent passive UL decode contexts. Duplicated from nr_pusch_passive_decode.h rather than
/// included, so this header stays free of PHY/defs_nr_UE.h and can be unit-tested on its own; the
/// two are static_assert'd equal in nr_pusch_passive_decode.c, where both are visible.
#define NR_PUSCH_PASSIVE_HARQ_MAX_CTX 6u

/// ULSCH array entries per decode context, i.e. the exact range of the ULSCH_id that
/// nr_ulsch_decoding.c adds to the base below (it indexes gNB->ulsch[ULSCH_id] and
/// gNB->pusch_vars[ULSCH_id], so it is an array index and canNOT itself be offset). Not a guess
/// and not "probably 1": passive_gnb_prepare() builds exactly one ULSCH and one pusch_vars per
/// context and sets gNB->max_nb_pusch from THIS constant, with a static_assert beside the
/// allocations. The stride follows the allocation; it does not have to be kept in step by hand.
#define NR_PUSCH_PASSIVE_ULSCH_PER_CTX 1u

/* Bound, as arithmetic:
 *   highest id = BASE + (MAX_CTX - 1) * ULSCH_PER_CTX + (ULSCH_PER_CTX - 1)
 *              = 5000 + 5 * 1 + 0 = 5005
 * and the next submitter type would start at BASE + SPAN = 6000, so 5005 < 6000 with 994 ids of
 * headroom. The assert fails the build rather than spilling if either constant is raised. */
static_assert((NR_PUSCH_PASSIVE_HARQ_MAX_CTX - 1) * NR_PUSCH_PASSIVE_ULSCH_PER_CTX
                      + (NR_PUSCH_PASSIVE_ULSCH_PER_CTX - 1)
                  < NR_PASSIVE_HARQ_NAMESPACE_SPAN,
              "passive UL harq_unique_pid range overflows into the next submitter type's namespace");

/**
 * @brief harq_unique_pid BASE for one passive UL decode context.
 * @param ctx decode-context index (0..NR_PUSCH_PASSIVE_MAX_CTX-1), the one thing that is live-
 *            distinct between two concurrent passive PUSCH decodes: every buffer the chain writes
 *            hangs off g_gnb[ctx].
 *
 * Returns the BASE, not the final id: nr_ulsch_decoding.c adds the ULSCH_id (the ulsch[] array
 * index) itself, which preserves upstream's "unique among the ULSCHs of one instance" property and
 * adds the context axis on top of it. ctx is folded into range rather than trusted, so a bad value
 * can at worst alias another passive UL decode instead of a different submitter type.
 */
static inline uint32_t nr_pusch_passive_harq_tag_base(int ctx)
{
  const uint32_t c = (uint32_t)(ctx < 0 ? 0 : ctx) % NR_PUSCH_PASSIVE_HARQ_MAX_CTX;
  return NR_PUSCH_PASSIVE_HARQ_TAG_BASE + c * NR_PUSCH_PASSIVE_ULSCH_PER_CTX;
}

#ifdef __cplusplus
}
#endif

#endif /* NR_PASSIVE_HARQ_TAG_H */
