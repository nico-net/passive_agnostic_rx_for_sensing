/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Fast-SSC polar decoder for DCI (TS 38.212 7.3.2/5.3.1), replaces polar_decoder_int16() on the PDCCH path.
 * Decisions are those of the min-sum SC decoder: only rate-0 and rate-1 nodes are shortcut, and rate-1 only when no soft
 * value of the node is exactly 0 (a tie is the one case where "hard decision of alpha" and min-sum SC differ). Arithmetic is
 * symmetric-saturating int16, so large |LLR| (>= 100) decodes correctly, unlike the -32768 hazard of the reference.
 * Return contract = polar_decoder_int16(ones_flag=1, DCI): CRC residual (== RNTI for a genuine DCI), payload right-aligned in out[]. */
#ifndef NR_POLAR_FSSC_H
#define NR_POLAR_FSSC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_POLAR_FSSC_LANES 8
/* One candidate. llr: E = 108*al soft bits (same vector polar_decoder_int16 gets). out: 3 words (zeroed then filled). */
uint32_t nr_polar_dci_decode_fssc(const int16_t *llr, uint64_t *out, uint16_t len, uint8_t al);
/* Up to 8 candidates sharing (len, al) in one call. res[j] / out[j*3..j*3+2] as above. n in 1..8. */
void nr_polar_dci_decode_fssc_x8(const int16_t *const llr[], int n, uint32_t res[], uint64_t out[], uint16_t len, uint8_t al);
#ifdef __cplusplus
}
#endif
#endif
