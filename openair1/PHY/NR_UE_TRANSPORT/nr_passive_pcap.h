/*
 * Write CRC-verified passive transport blocks to a Wireshark-readable pcap.
 *
 * The passive receiver already recovers other UEs' transport blocks and checks their CRC, but it
 * only ever COUNTED them -- the bytes in nr_pdsch_passive_decode_result_t::tb were dropped on the
 * floor because this branch was built for sensing (channel measurements), not packet capture.
 * This turns those verified bytes into a capture file.
 *
 * Enabled by ISAC_PASSIVE_PCAP=<path>; a no-op when unset, so the default path is unchanged.
 * Output is DLT_WIRESHARK_UPPER_PDU carrying the "mac-nr-framed" dissector, i.e. exactly what
 * common/utils/T/tracer/macpdu2wireshark.c writes, so Wireshark dissects it with no manual
 * "Decode As" step.
 */
#ifndef NR_PASSIVE_PCAP_H
#define NR_PASSIVE_PCAP_H

#include <stdbool.h>
#include <stdint.h>

/// \param uplink   true for a PUSCH transport block, false for PDSCH
/// \param rnti     the RNTI the PDCCH CRC descrambled to
/// \param si_rnti  true to tag the PDU as SI-RNTI rather than C-RNTI
/// \param tb       decoded payload, MSB-first, as handed back by the passive decode
/// \param tb_bits  transport block size in BITS, including the TB CRC
void nr_passive_pcap_write(bool uplink,
                           uint16_t rnti,
                           bool si_rnti,
                           int frame,
                           int slot,
                           int harq_pid,
                           const uint8_t *tb,
                           int tb_bits);

#endif
