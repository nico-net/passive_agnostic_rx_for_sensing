/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Test / benchmark fixture for nr_td_cb0_batch: a random TB encoded with the OAI chain (TB CRC, nr_segmentation,
 * LDPCencoder (ldpc_encoder.c), nr_rate_matching_ldpc, nr_interleaving_ldpc) into +-amp LLRs, and the receiver's
 * real decoder (nrLDPC_coding_decoder of nrLDPC_coding_segment_decoder.c, the libldpc.so path) driven with the
 * parameters passive_ldpc_decode_core gives it. Plain C so the OAI headers are not compiled as C++. */
#ifndef NR_TD_CB0_FIXTURE_H
#define NR_TD_CB0_FIXTURE_H
#include <stdint.h>
#include "nr_td_cb0_batch.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  /* in */
  uint32_t A;    /* TBS bits */
  uint16_t R;    /* target code rate x 10240 */
  uint8_t Qm, Nl, rv;
  uint32_t G;
  uint32_t tbslbrm;
  /* out */
  uint8_t BG;
  uint32_t C, K, Z, F;
  int16_t *llr; /* G LLRs, +amp for bit 0, -amp for bit 1 */
  uint8_t *payload; /* the A/8 random source bytes */
} cb0_fx_t;

/* crcTableInit (MUST precede any CRC: aarch64 tables are zero until then), logInit, thread pool, register LDPCdecoder with nr_td_cb0_set_ldpc_decoder. Idempotent. */
void cb0_fx_init(void);
/* Re-register LDPCdecoder (after a test cleared it). */
void cb0_fx_reinit_decoder(void);
/* Encode a random TB (seed) into fx->llr. Returns 0, or -1 if the OAI encoder chain refuses the parameters. */
int cb0_fx_encode(cb0_fx_t *fx, uint32_t seed, int amp);
void cb0_fx_free(cb0_fx_t *fx);
/* The item a hypothesis with exactly these parameters produces (llr shared). */
nr_td_cb0_item_t cb0_fx_item(const cb0_fx_t *fx);
/* The receiver's decoder on an item, as passive_ldpc_decode_core: probe = 1 decodes segment 0 only
 * (nb_segments_to_decode = 1 when C > 1) and applies the probe all-zero guard; probe = 0 decodes the full TB
 * (all segments, TB CRC when C > 1, all-zero guard). Returns 1 pass, 0 fail, -1 if the parameters are refused
 * before decoding (segmentation). seg0_ok (may be NULL) receives decodeSuccess[0]. */
int cb0_fx_rx_decode(const nr_td_cb0_item_t *it, int probe, int *seg0_ok);
/* As cb0_fx_rx_decode(probe = 0), and *bits_match = 1 iff the reassembled TB payload equals expect (A/8 bytes) and
 * the TB CRC recomputed on it equals the decoded CRC bits: the pass is checked against the source bits, not only
 * against the decoder's own CRC verdict. */
int cb0_fx_rx_decode_bits(const nr_td_cb0_item_t *it, const uint8_t *expect, int *bits_match);
/* crc24a / crc16 of a buffer (the TB CRC the encoder attaches): non-zero on random data iff the tables are set. */
uint32_t cb0_fx_tb_crc(const uint8_t *a, uint32_t A);
/* get_BG / nr_get_G / nr_compute_tbs pass-throughs for the .cc */
uint8_t cb0_fx_get_bg(uint32_t A, uint16_t R);
uint32_t cb0_fx_G(uint16_t nb_rb, uint16_t nsym, uint8_t nb_re_dmrs, uint16_t dmrs_len, uint8_t Qm, uint8_t Nl);
uint32_t cb0_fx_tbs(uint8_t Qm, uint16_t R, uint16_t nb_rb, uint16_t nsym, uint16_t nb_dmrs_prb, uint8_t Nl);

#ifdef __cplusplus
}
#endif
#endif
