/* UCI multiplexed on PUSCH (TS 38.212 6.2.7 data/control mapping, 6.3.2.4 rate matching) as the OAI UE transmits it
 * (nr_ulsch_ue.c), exposed so the passive receiver inverts the SAME implementation. */
#ifndef NR_UCI_ON_PUSCH_H
#define NR_UCI_ON_PUSCH_H

#include <stdint.h>
#include "nr_transport_ue.h"

/* For a PUSCH codeword of G_total bits (every data RE of the allocation, no UCI) carrying the UCI described by
 * pdu->pusch_uci (harq_ack_bit_length, csi_payload.p1_bits / p2_bits, the three beta-offset indices, alpha_scaling) and an
 * UL-SCH of C code blocks of K bits: map[i] for each codeword bit i is 1 (an UL-SCH bit), 0 (an UL-SCH bit punctured by
 * HARQ-ACK of <= 2 bits; it keeps its place in the UL-SCH stream) or -1 (a UCI bit). Returns G_ulsch, the UL-SCH bit count
 * (the number of map entries >= 0), or 0 when the hypothesis is outside the implementation's scope (coded UCI part above
 * 1024 bits, no room for data). rm_out (optional) receives the rate-matching result (Q'_ACK, Q'_CSI1, Q'_CSI2 ...). */
uint32_t nr_uci_on_pusch_ulsch_map(const nfapi_nr_ue_pusch_pdu_t *pdu,
                                   uint16_t ptrs_symbols,
                                   int n_ptrs,
                                   uint32_t K,
                                   uint32_t C,
                                   uint32_t G_total,
                                   int8_t *map,
                                   rate_match_info_uci_t *rm_out);

#endif
