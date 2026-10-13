/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* TIMRM diagnostic (ISAC_PDSCH_RM_GENIE=1, default off, no effect on decoding): bit-level genie for single-code-block PDSCH.
 * A CRC-passing TB is re-encoded (LDPC + rate matching + interleaver, the transmitter chain) and its codeword is compared with the
 * sign of the descrambled LLRs -> per-bucket bit-error map (RMMAP). A CRC-failing TB is compared against earlier CRC-passing TBs of
 * the same RNTI and TBS under every rv: a high sign agreement for some (earlier TB, rv) identifies a retransmission and its true rv
 * (RMGENIE). Receiver input only (LLRs, CRC verdicts); gNB logs are never used. */
#ifndef NR_PDSCH_RM_GENIE_H
#define NR_PDSCH_RM_GENIE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool nr_pdsch_rm_genie_on(void);
int nr_pdsch_rm_genie_level(void); /* ISAC_PDSCH_RM_GENIE value: 1 = RMMAP/RMGENIE, 2 = + TBS/rv sweep of failed grants (RMTBS) */
/* Any number of code blocks (HIGHMCS; was single-CB). tb = reassembled bytes incl. TB CRC (B/8 bytes), llr = descrambled, G entries, E = G. */
void nr_pdsch_rm_genie_note(uint16_t rnti, int frame, int slot, uint32_t A, const uint8_t *tb, uint8_t BG, uint8_t Qm,
                            uint8_t Nl, uint8_t rv, uint32_t tbslbrm, uint32_t G, const int16_t *llr, uint8_t mcs);
void nr_pdsch_rm_genie_fail(uint16_t rnti, int frame, int slot, uint32_t A, uint8_t BG, uint8_t Qm, uint8_t Nl, uint8_t rv,
                            uint32_t tbslbrm, uint32_t G, const int16_t *llr, uint8_t mcs);
bool nr_pdsch_rm_genie_dci_budget(void);
int nr_pdsch_rm_genie_same_as(uint16_t rnti, uint32_t A, const uint8_t *tb, int *frame, int *slot);
#ifdef __cplusplus
}
#endif
#endif
