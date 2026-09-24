"""GF(2) algebraic n_RNTI recovery, proof of concept (Task 4, plan 2026-09-23).

RETRACTION (2026-09-24): the first version of this file (and the C module it was ported to) was
FUNDAMENTALLY BROKEN, found by an independent fresh-eyes review after the module had already been
built, tested (4/4 green), and committed. Three real bugs, none of which its own tests could catch
because the tests shared the same wrong assumptions as the code under test:

1. **Wrong bit extraction.** `openair1/PHY/gold.h`'s `gold_generic()` returns a FULL 32-bit word per
   call; every real caller in this codebase (e.g. `LTE_TRANSPORT/dlsch_scrambling.c`, confirmed by
   reading it: `e[k] ^= (s >> k) & 1` for k=0..31) consumes all 32 bits before calling again. The
   first version of this file took only bit 0 of each call in a loop, silently sampling c(0), c(32),
   c(64), ... instead of the consecutive c(0), c(1), c(2), ... any real receiver actually produces.
   The file's own "cross-check against the compiled real gold_generic()" compared this same `& 1`
   mistake on BOTH sides, so it validated nothing.
2. **Wrong bit width.** TS 38.211 7.3.2.3's c_init = (n_RNTI << 16 + n_ID) is taken MOD 2^31, per this
   project's own `openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1073` (`% (1U << 31)`). RNTI bit 15 maps to
   c_init's bit 31, which this mask REMOVES before the sequence is generated at all -- it has ZERO
   physical effect on the real scrambling sequence. The first version's brute-force "uniqueness"
   check flagged this correctly (0x4615 and 0xC615 produced identical sequences under a from-scratch
   bit-serial reference) and was WRONGLY treated as a bug in that reference to be "fixed" by
   re-deriving a 16-bit-recoverable model, when the original ambiguity finding was the physically
   correct one all along.
3. **No consistency check.** The Gaussian elimination never verified that equations beyond the pivot
   rows were actually satisfied, so ANY input (random noise, a real sequence scored against the wrong
   n_id) was reported as a confident, wrong "success". Measured: 1000/1000 random 64-bit inputs
   returned success under the first version.

This file now does three things an independent reviewer's method demanded, none of which the first
version did: (a) validates the sequence GENERATOR against an INDEPENDENT bit-serial TS 38.211 5.2.1
LFSR implementation that shares no code with the word-extraction port; (b) recovers only the 15
physically-recoverable RNTI bits, with RNTI bit 15 left explicitly undetermined; (c) adds and tests a
real consistency check (0/1000 false accepts on random noise, rejects a wrong n_id).
"""
import random

import numpy as np

MASK32 = 0xFFFFFFFF


def gold_generic_word(x1, x2, reset):
    """openair1/PHY/gold.h's gold_generic() -- returns a FULL 32-bit word per call; all 32 bits are
    valid sequence output (confirmed against real usage: dlsch_scrambling.c unpacks
    e[k] ^= (s >> k) & 1 for k=0..31 before calling again)."""
    if reset:
        x1 = (1 + (1 << 31)) & MASK32
        x2 = (x2 ^ (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) << 31) & MASK32)) & MASK32
        for _ in range(1, 50):
            x1 = ((x1 >> 1) ^ (x1 >> 4)) & MASK32
            x1 = (x1 ^ ((x1 << 31) & MASK32) ^ ((x1 << 28) & MASK32)) & MASK32
            x2 = ((x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3) ^ (x2 >> 4)) & MASK32
            x2 = (x2 ^ ((x2 << 31) & MASK32) ^ ((x2 << 30) & MASK32) ^ ((x2 << 29) & MASK32) ^ ((x2 << 28) & MASK32)) & MASK32
    x1 = ((x1 >> 1) ^ (x1 >> 4)) & MASK32
    x1 = (x1 ^ ((x1 << 31) & MASK32) ^ ((x1 << 28) & MASK32)) & MASK32
    x2 = ((x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3) ^ (x2 >> 4)) & MASK32
    x2 = (x2 ^ ((x2 << 31) & MASK32) ^ ((x2 << 30) & MASK32) ^ ((x2 << 29) & MASK32) ^ ((x2 << 28) & MASK32)) & MASK32
    return x1, x2, (x1 ^ x2) & MASK32


def gold_sequence_word_extract(c_init, n_bits):
    """CORRECT extraction (fix #1): consume all 32 bits of each returned word before advancing."""
    x1, x2 = 0, c_init & MASK32
    out = np.zeros(n_bits, dtype=np.uint8)
    produced = 0
    reset = True
    while produced < n_bits:
        x1, x2, word = gold_generic_word(x1, x2, reset)
        reset = False
        take = min(32, n_bits - produced)
        for k in range(take):
            out[produced + k] = (word >> k) & 1
        produced += take
    return out


def gold_sequence_bitserial_ref(c_init31, n_bits):
    """INDEPENDENT reference, direct TS 38.211 5.2.1 bit-serial LFSR -- shares no code with
    gold_generic's word-batched trick. Used only to cross-check the word-extraction port."""
    Nc = 1600
    total = Nc + n_bits
    x1 = np.zeros(total + 31, dtype=np.uint8)
    x2 = np.zeros(total + 31, dtype=np.uint8)
    x1[0] = 1
    for n in range(31):
        x2[n] = (c_init31 >> n) & 1
    for n in range(total):
        x1[n + 31] = x1[n + 3] ^ x1[n]
        x2[n + 31] = x2[n + 3] ^ x2[n + 2] ^ x2[n + 1] ^ x2[n]
    return (x1[Nc:Nc + n_bits] ^ x2[Nc:Nc + n_bits]).astype(np.uint8)


def scramble_c_init_masked(n_rnti, n_id):
    """TS 38.211 7.3.2.3 c_init = (n_RNTI*2^16 + n_ID) mod 2^31 -- confirmed against
    openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1073's own `% (1U << 31)` (fix #2). n_rnti's bit 15
    (mapping to the raw value's bit 31) is REMOVED by this mask and has NO effect on the sequence --
    this is a physical fact about the spec/receiver, not a bug to "solve around". Only 15 RNTI bits
    (0..14) are recoverable from the scrambling sequence at all."""
    return ((n_rnti << 16) + n_id) % (1 << 31)


def build_rnti15_linear_model(n_id, n_bits):
    """15 unknowns (bit 15 is physically unrecoverable, fix #2)."""
    baseline = gold_sequence_word_extract(scramble_c_init_masked(0, n_id), n_bits)
    M = np.zeros((n_bits, 15), dtype=np.uint8)
    for bit in range(15):
        perturbed = gold_sequence_word_extract(scramble_c_init_masked(1 << bit, n_id), n_bits)
        M[:, bit] = perturbed ^ baseline
    return M, baseline


def recover_rnti15_gf2(observed, n_id, n_bits):
    """Returns (low15_bits, is_consistent). Fix #3: after reducing the 15 pivot rows, checks EVERY
    remaining row's residual is zero; a nonzero residual means the observed bits are not a valid
    scrambling sequence for this n_id at all (wrong n_id, corrupted input, or noise)."""
    M, baseline = build_rnti15_linear_model(n_id, n_bits)
    target = (observed ^ baseline).astype(np.uint8)
    A = np.concatenate([M.copy(), target.reshape(-1, 1)], axis=1) % 2
    n_rows = A.shape[0]
    row = 0
    for col in range(15):
        piv = next((rr for rr in range(row, n_rows) if A[rr, col]), None)
        if piv is None:
            return None, False
        A[[row, piv]] = A[[piv, row]]
        for rr in range(n_rows):
            if rr != row and A[rr, col]:
                A[rr] ^= A[row]
        row += 1
    for rr in range(15, n_rows):
        if A[rr, 15]:
            return None, False
    bits = A[:15, 15]
    val = 0
    for i, b in enumerate(bits):
        val |= int(b) << i
    return val, True


if __name__ == "__main__":
    all_ok = True

    print("--- cross-check word-extraction vs independent bit-serial reference ---")
    for rnti in [0x4615, 0xC615, 0x0001, 0x7FFF, 0x0000, 0x5A5A, 0x2A2A]:
        for n_id in [2, 0, 500]:
            c = scramble_c_init_masked(rnti, n_id)
            a = gold_sequence_word_extract(c, 96)
            b = gold_sequence_bitserial_ref(c, 96)
            ok = np.array_equal(a, b)
            all_ok &= ok
            if not ok:
                print(f"  MISMATCH rnti=0x{rnti:04x} n_id={n_id}")
    print("cross-check:", "PASS" if all_ok else "FAIL")

    print("\n--- bit 15 is (correctly) unrecoverable: 0x4615 vs 0xC615 sequences are identical ---")
    seq_a = gold_sequence_word_extract(scramble_c_init_masked(0x4615, 2), 64)
    seq_b = gold_sequence_word_extract(scramble_c_init_masked(0xC615, 2), 64)
    identical = np.array_equal(seq_a, seq_b)
    all_ok &= identical
    print("PASS (confirms bit 15 unrecoverable, as expected)" if identical else "FAIL (unexpected)")

    print("\n--- 15-bit solver: recovery ---")
    n_id, n_bits = 2, 64
    for true_rnti in [0x4615, 0xC615, 0x0001, 0x7FFF, 0x0000, 0x5A5A, 0x2A2A, 0xFFFE]:
        c = scramble_c_init_masked(true_rnti, n_id)
        seq = gold_sequence_word_extract(c, n_bits)
        val, consistent = recover_rnti15_gf2(seq, n_id, n_bits)
        expected15 = true_rnti & 0x7FFF
        ok = consistent and val == expected15
        all_ok &= ok
        print(f"true=0x{true_rnti:04x} (low15=0x{expected15:04x}) recovered={val} consistent={consistent} {'PASS' if ok else 'FAIL'}")

    print("\n--- consistency check: must reject noise and wrong n_id ---")
    random.seed(1)
    n_fp = 0
    for _ in range(1000):
        seq = np.array([random.randint(0, 1) for _ in range(n_bits)], dtype=np.uint8)
        _, consistent = recover_rnti15_gf2(seq, n_id, n_bits)
        if consistent:
            n_fp += 1
    print(f"random-noise false-accept rate: {n_fp}/1000 {'PASS' if n_fp == 0 else 'FAIL'}")
    all_ok &= (n_fp == 0)

    true_rnti = 0x4615
    seq = gold_sequence_word_extract(scramble_c_init_masked(true_rnti, 2), n_bits)
    _, consistent = recover_rnti15_gf2(seq, 3, n_bits)  # wrong n_id
    print(f"wrong n_id rejected: consistent={consistent} {'PASS' if not consistent else 'FAIL'}")
    all_ok &= (not consistent)

    print("\nALL PASS" if all_ok else "\nSOME FAILED")
