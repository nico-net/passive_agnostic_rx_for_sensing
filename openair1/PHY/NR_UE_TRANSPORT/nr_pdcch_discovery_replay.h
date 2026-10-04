/* Opt-in diagnostic snapshot, local little-endian same-build ABI v3 (three-word payload; incompatible with v1/v2).
 * v2 offset includes BWPStart; v1 omitted it.
 * Each header is followed by grid_count c16_t, fft_size*duration c16_t,
 * and expected_re c16_t. No pointers or inferred RNTI labels are persisted.
 * kind 1 is an accepted CSS0 SI grant; kind 2 is an UNVERIFIED hypothesis.
 */
#ifndef NR_PDCCH_DISCOVERY_REPLAY_H
#define NR_PDCCH_DISCOVERY_REPLAY_H
#include <stdint.h>
#include "nr_dci_bits.h"
#define NR_PDCCH_REPLAY_MAGIC UINT32_C(0x31524450)
typedef struct {
  uint32_t magic, version, header_bytes, kind;
  uint64_t source_slot;
  nr_dci_bits_t expected_payload;
  uint32_t frame, slot, pci, span, offset, duration, first_symbol;
  uint32_t bundle, interleaver, shift, dmrs_id, scrambling_rnti;
  uint32_t n_candidates, grid_count, fft_size, carrier_rb, first_carrier_offset;
  uint32_t expected_index, expected_rnti, expected_length, expected_re;
  uint16_t cce[64];
  uint8_t al[64];
} nr_pdcch_discovery_replay_t;
#endif
