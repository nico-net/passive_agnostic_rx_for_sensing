/*
 * Passive UPLINK data-aided CFR: reconstruct the transmitted PUSCH symbols from a CRC-verified
 * transport block and measure H = Y/X at every data RE.
 *
 * The uplink counterpart of nr_pdsch_data_aided.{h,c}. The downlink has had a comb-1 data-aided
 * source since 2026-07-30; the uplink had only PUSCH DM-RS, which is comb-2 at best and only on the
 * DM-RS symbols, so the uplink contributed a small fraction of the rows per CPI that the downlink
 * did. This closes that asymmetry, and it only became possible once the passive PUSCH decode
 * actually reached CRC OK -- a TB you cannot decode is a TB you cannot re-encode.
 */
#ifndef NR_PUSCH_DATA_AIDED_H
#define NR_PUSCH_DATA_AIDED_H

#include "PHY/defs_nr_UE.h"
#include "PHY/defs_gNB.h"
#include "nfapi_nr_interface_scf.h"
#include "nr_pdcch_blind_monitor.h"

/// LDPC harq_unique_pid namespace for the uplink re-encode. Must stay disjoint from every other
/// user of the same nrLDPC_coding_interface -- the downlink decode, the downlink data-aided
/// re-encode and the passive uplink decode -- because a hardware accelerator keys its per-TB state
/// on this id and an alias silently mixes two transport blocks.
#define NR_PUSCH_PASSIVE_DA_TAG_BASE 4000u

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Submit H = Y/X over a PUSCH grant's data REs, in one of two modes.
 *
 * Re-encode (tb_bytes != NULL): X is the CRC-verified TB re-encoded. MUST only be used for a grant
 * carrying NO UCI (see the uci_ack_re gate at the call site): UCI OVERWRITES ULSCH resource
 * elements, so X there is not the ULSCH symbol this reconstructs. With llr given, the grant also
 * feeds the LLR-confidence calibration (nr_llr_confidence.h).
 *
 * Masked (tb_bytes == NULL): X is the hard decision of llr, re-scrambled and re-modulated, kept only
 * on REs whose min |LLR| clears the learned threshold. Valid with or without UCI, because a
 * descrambled hard decision re-scrambled with the same c(i) is the transmitted bit whatever it
 * encodes. Submits nothing until that modulation order has calibration.
 *
 * Silently no-ops unless NR_ISAC_SRC_PUSCH_DATA is an enabled source.
 *
 * @param ue            UE instance -- used only for its LDPC coding interface and thread pool
 * @param gnb           the passive gNB context holding rxdataF for this slot
 * @param pdu           the PUSCH PDU the decode ran against
 * @param g             the blind UL grant (allocation, DM-RS layout, RNTI)
 * @param tb_bytes      the CRC-verified transport block, or NULL for the masked mode
 * @param llr           descrambled LLRs over ALL of the grant's REs (data + UCI), or NULL
 * @param llr_G         length of llr; must equal the grant's full G or llr is ignored
 * @param harq_pid_tag  namespaced LDPC id, disjoint from every other user of this interface
 * @param ul_slot_idx   producer-timeline slow-time index, identical to the DM-RS path's
 * @param nof_ant       receive antennas to extract (per-antenna phases are preserved for AoA)
 * @param slot          the uplink slot, for the rxdataF ring offset
 */
void nr_isac_pusch_data_aided_submit(PHY_VARS_NR_UE *ue,
                                     PHY_VARS_gNB *gnb,
                                     const nfapi_nr_pusch_pdu_t *pdu,
                                     const nr_pdcch_blind_ul_result_t *g,
                                     const uint8_t *tb_bytes,
                                     const int16_t *llr,
                                     uint32_t llr_G,
                                     uint32_t harq_pid_tag,
                                     uint32_t ul_slot_idx,
                                     uint32_t nof_ant,
                                     int slot);

/// Per-run census: attempts, submissions, and every reason one did not happen.
void nr_isac_pusch_data_aided_stats_dump(void);

#ifdef __cplusplus
}
#endif
#endif
