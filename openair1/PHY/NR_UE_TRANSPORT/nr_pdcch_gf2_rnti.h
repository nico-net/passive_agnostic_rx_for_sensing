#ifndef NR_PDCCH_GF2_RNTI_H
#define NR_PDCCH_GF2_RNTI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* GF(2) algebraic RNTI recovery (5GDescrambler-style, arxiv.org/abs/2609.07367): TS 38.211 7.3.2.3's
 * PDCCH data-scrambling c_init = (n_RNTI << 16) + n_ID, taken MOD 2^31 (confirmed against this
 * project's own openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1073's `% (1U << 31)`), is a fixed n_ID offset
 * plus 15 of n_RNTI's 16 bits shifted into the initial LFSR state -- so each output scrambling bit
 * is a fixed GF(2)-linear combination of those 15 bits, XOR a constant baseline (the n_RNTI=0
 * sequence). Given >= 15 known scrambling bits, this recovers those 15 bits as one linear solve
 * instead of a brute force.
 *
 * IMPORTANT, CORRECTED 2026-09-24 after a fresh-eyes review found this module's first version
 * fundamentally broken (it returned a wrong-but-"successful" RNTI for every real input; see
 * docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py's module docstring for the full
 * retraction). Two physical facts this version gets right that the first version did not:
 *   1. gold_generic() returns a FULL 32-bit word per call, and every real caller in this codebase
 *      (e.g. LTE_TRANSPORT/dlsch_scrambling.c) consumes all 32 bits before calling again. The first
 *      version took only bit 0 of each call, silently sampling every 32nd sequence bit instead of
 *      consecutive ones.
 *   2. RNTI bit 15 maps to c_init's bit 31, which the `% (1U << 31)` mask REMOVES before the
 *      sequence is generated at all -- it has ZERO effect on the real scrambling sequence. It is
 *      not recoverable by this or any method operating on the scrambling sequence alone. This
 *      module recovers the other 15 bits and returns them in `*rnti_out` (bit 15 always 0); the
 *      caller must determine the true bit 15 some other way (e.g. try both candidates and let a
 *      transport-block CRC decide, or use knowledge of the RNTI's expected range).
 * This version also adds a consistency check the first lacked: with n_bits > 15, the extra
 * equations are checked for a zero residual after solving, so noise or a wrong n_id is REJECTED
 * (returns 0) instead of reported as a confident, wrong success -- measured 0/1000 false accepts on
 * random input in the Python PoC, where the first version accepted 1000/1000.
 *
 * All of the above (word extraction, the 31-bit mask, the consistency check) is independently
 * cross-checked in docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py against a from-scratch
 * bit-serial TS 38.211 5.2.1 LFSR reference that shares no code with either the word-extraction port
 * or the original (buggy) model -- the first version's "verification" compared its own bug against
 * itself and is why it went undetected until an independent review ran real hand-derived arithmetic
 * against it. This module also has NO precondition that n_RNTI equals the C-RNTI in general: per
 * 38.211, that only holds on a UE-specific search space with a configured pdcch-DMRS-ScramblingID
 * (otherwise n_RNTI=0 and n_ID is the PCI, and there is nothing meaningful to recover). NOT wired
 * into any live decode path -- standalone, unit-tested module only. */

/* seq: n_bits >= 15 CONSECUTIVE scrambling-sequence bits c(0..n_bits-1) (0/1 per byte), as produced
 * by the true n_RNTI (in production, recovered from known/CRC-checkable bits XORed against the
 * observed ciphertext -- this module only does the linear algebra). n_id: the cell/CORESET's
 * pdcch-DMRS-ScramblingID (assumed already known independently). On success, fills *rnti_out with
 * the recovered value in bits 0-14 (bit 15 always 0 -- see the header comment: it is not
 * recoverable from the sequence) and returns 1. Returns 0 if n_bits < 15, n_bits exceeds the
 * module's internal buffer, or `seq` is not a valid scrambling sequence for ANY RNTI at this n_id
 * (checked via a full residual test on every bit beyond the first 15, not merely assumed -- note
 * this check is only possible when n_bits > 15; at exactly 15 bits there is no spare evidence to
 * cross-validate against, so callers wanting the noise/wrong-n_id rejection should supply more). */
int nr_pdcch_gf2_rnti_recover(const uint8_t *seq, int n_bits, uint16_t n_id, uint16_t *rnti_out);

#ifdef __cplusplus
}
#endif
#endif
