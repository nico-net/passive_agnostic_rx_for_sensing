#ifndef NR_PDCCH_GF2_RNTI_H
#define NR_PDCCH_GF2_RNTI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* GF(2) algebraic RNTI recovery (5GDescrambler-style, arxiv.org/abs/2609.07367): TS 38.211 7.3.2.3's
 * PDCCH data-scrambling c_init = (n_RNTI << 16) + n_ID is a fixed n_ID offset plus n_RNTI shifted
 * into the initial LFSR state, and the Gold-sequence recurrence (openair1/PHY/gold.h's
 * gold_generic(), XOR/shift only) is linear in that initial state -- so each output scrambling bit
 * is a fixed linear combination of the 16 RNTI bits, XOR a constant baseline (the n_RNTI=0
 * sequence). Given >= 16 known scrambling bits (the caller's problem to supply -- e.g. from a
 * UE-specific search space's descrambled-at-RNTI-0 hypothesis XORed against a CRC-recoverable
 * payload), this recovers the RNTI as one GF(2) linear solve instead of a 65536-way brute force.
 *
 * The linear-model construction and the solver's pivoting strategy were both independently verified
 * against a compiled copy of the real gold_generic() and a full 65536-way brute force before this
 * port was written (docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py) -- two real bugs
 * were found and fixed in that process (a non-bit-exact sequence model, and Gaussian elimination
 * restricted to a rank-deficient leading submatrix); see that file's module docstring. This C port
 * calls the real gold_generic() directly rather than re-deriving the LFSR a third time. NOT wired
 * into any live decode path -- standalone, unit-tested module only. */

/* seq: n_bits >= 16 scrambling-sequence bits (0/1 per byte) produced with the TRUE n_RNTI. n_id:
 * the cell/CORESET's pdcch-DMRS-ScramblingID (assumed already known independently). Returns 1 and
 * fills *rnti_out on success, 0 if n_bits < 16 (underdetermined) or no consistent RNTI exists for
 * this (seq, n_id) pair. */
int nr_pdcch_gf2_rnti_recover(const uint8_t *seq, int n_bits, uint16_t n_id, uint16_t *rnti_out);

#ifdef __cplusplus
}
#endif
#endif
