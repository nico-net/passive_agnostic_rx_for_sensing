#ifndef NR_PDCCH_JOINT_SOLVE_H
#define NR_PDCCH_JOINT_SOLVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* JOINT RNTI + DCI-PAYLOAD RECOVERY FROM SCRAMBLED PDCCH LLRs (5GDescrambler-style, arXiv:2609.07367 idea;
 * this is OUR implementation, not a port, and it makes no claim to reproduce that paper's numbers).
 *
 * THE PROBLEM. On a cell whose pdcch-DMRS-ScramblingID is configured, the PDCCH data scrambling of a
 * UE-specific DCI is c_init = (C-RNTI << 16 + n_ID) mod 2^31 (TS 38.211 7.3.2.3) and the DCI CRC is masked
 * with the same C-RNTI (38.212 7.3.2). A passive receiver that does not know the C-RNTI therefore cannot
 * even descramble, so the usual "try a candidate RNTI, polar-decode, check CRC" needs a 65536-way search
 * per PDCCH candidate.
 *
 * THE OBSERVATION. Every stage between the unknown bits and the scrambled coded bits is GF(2)-affine: CRC24C
 * with the 24 leading ones, the RNTI XOR into the last 16 CRC bits, the polar transform, sub-block
 * interleaving + rate matching (selection), and the Gold-sequence scrambling (linear in c_init's bits). So
 *      scrambled_bits = M * x  XOR  b          x = [ DCI payload (A bits) ; C-RNTI (16 bits) ]
 * for ONE fixed (A, aggregation level, n_ID) matrix M and baseline b. Recovering (payload, RNTI) from a
 * noisy observation is then decoding a fixed linear code -- no RNTI list, no 65536-way loop.
 *
 * HOW M IS OBTAINED. NOT by re-deriving the polar/CRC/rate-matching chain (this project has already been
 * bitten twice by self-consistent re-implementations that were wrong). The caller supplies an ENCODE
 * CALLBACK that wraps the real encoder (polar_encoder_fast in the tests); M's columns are PROBED from it with
 * unit vectors, and the model is REFUSED (NULL) unless a random-superposition check confirms the callback
 * really is affine. Only the Gold scrambling is modelled here (via openair1/PHY/gold.h, cross-checked in
 * the tests against an independent bit-serial 38.211 5.2.1 LFSR).
 *
 * DECODING. Ordered-statistics decoding (OSD) on the code's generator: take the K most reliable
 * linearly-independent observations, solve exactly, then (order 1/2) re-test flips of the least-certain of
 * those K. This uses the soft information; a hard-decision exact solve would fail on a single bit error.
 *
 * ACCEPTANCE (there is no other oracle: an empty PDCCH candidate also "decodes" to SOMETHING). The K
 * most-reliable positions match the answer BY CONSTRUCTION, so only the remaining E-K "redundancy"
 * positions carry evidence. For pure noise their agreement with any FIXED candidate is a zero-mean sum with
 * a known spread sigma_O = sqrt(sum w^2)/sum w (w = |LLR| over those positions); the best of N examined
 * candidates is bounded by sigma_O*(sqrt(2 ln N) + 2) -- derived from N and the observation itself, the same
 * convention the stage-1 nID sweep uses, not a tuned constant. The result reports both numbers.
 *
 * LIMITS, stated so they are not rediscovered: (1) n_ID (pdcch-DMRS-ScramblingID) and the DCI length A and
 * the aggregation level must already be known (stage 1 and the DCI-length sweep supply them). (2) It needs
 * enough parity: at AL1 there are only E-K = 108-61 = 47 redundancy bits, so the acceptance bar is high and
 * only clean observations will clear it -- measured, see the tests. (3) NOT wired into any live path and NOT
 * validated on air; the LLRs it wants are the PRE-descrambling QPSK demodulator outputs of a candidate.
 */

#define NR_PDCCH_JOINT_MAX_E 864 /* AL8: 8 CCE * 108 bits */
#define NR_PDCCH_JOINT_MAX_A 64  /* polar_encoder_fast's own payload limit */

/* Encode callback: write the E coded bits (0/1 per byte, index = coded-bit order) that the REAL encoder
 * produces for a payload in the low A bits of `payload` and a 16-bit CRC mask `crc_mask`, BEFORE scrambling.
 * Return 0 on success. */
typedef int (*nr_pdcch_joint_encode_fn)(void *ctx, uint64_t payload, uint16_t crc_mask, uint8_t *coded, int E);

typedef struct nr_pdcch_joint_model nr_pdcch_joint_model_t;

/* scramble_with_rnti = 1: c_init uses the C-RNTI (n_RNTI = C-RNTI cell); 0: c_init uses n_RNTI = 0 (then only
 * the CRC mask carries the RNTI, and this recovers it from the CRC alone). Returns NULL if the callback is
 * not affine, or if the unknowns are not all observable (rank < A+16). */
nr_pdcch_joint_model_t *nr_pdcch_joint_model_new(nr_pdcch_joint_encode_fn enc, void *ctx, int A, int E,
                                                 uint16_t n_id, int scramble_with_rnti);
void nr_pdcch_joint_model_free(nr_pdcch_joint_model_t *m);
int nr_pdcch_joint_model_unknowns(const nr_pdcch_joint_model_t *m); /* A + 16 */

typedef struct {
  uint64_t payload;   /* recovered DCI payload, low A bits (polar_encoder_fast's packing) */
  uint16_t rnti;      /* recovered C-RNTI, all 16 bits (bit 15 comes from the CRC mask) */
  double corr;        /* agreement of the winner with the observation on the redundancy positions, [-1,1] */
  double threshold;   /* acceptance bar for corr derived from the observation and the candidate count */
  int n_candidates;   /* candidates examined */
  int accepted;       /* corr >= threshold */
} nr_pdcch_joint_result_t;

/* llr[i] is the LLR of scrambled coded bit i; llr > 0 means bit 0 (the polar decoder's convention).
 * order: 0 (exact solve on the K most reliable), 1 or 2 (also test 1 / 2 flips). Returns 1 if accepted. */
int nr_pdcch_joint_solve(const nr_pdcch_joint_model_t *m, const int16_t *llr, int order,
                         nr_pdcch_joint_result_t *out);

#ifdef __cplusplus
}
#endif
#endif
