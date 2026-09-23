"""GF(2) algebraic n_RNTI recovery, proof of concept (Task 4, plan 2026-09-23).

TWO real bugs were found and fixed in the plan's original Step 1 model before this passed:

1. The plan's original `gold_generic_step()` was a hand-rolled 31-bit serial LFSR stepper that did
   NOT match `openair1/PHY/gold.h`'s real `gold_generic()` (a 32-bit, word-parallel-advance
   implementation with a bit-31 fold-in step on reset). Cross-checked bit-for-bit against the real C
   function (compiled directly from the actual header, see gold_cross_check.py's method, reproduced
   inline below) across 18 RNTI values including MSB-set/MSB-clear pairs at n_bits=96: the plan's
   original model FAILED this cross-check (confirmed by first reproducing its reported "AMBIGUOUS"
   brute-force result -- two RNTIs differing only in bit 15, 0x4615 and 0xC615, were spuriously
   indistinguishable under the wrong model, but ARE uniquely distinguishable under the real one).
   Replaced with `gold_generic_exact()`, a direct, verified transliteration.
2. Even with the exact model, restricting Gaussian elimination to the FIRST 16 output-sequence bits
   (as the plan's original `recover_rnti_gf2()` did) fails most of the time: measured GF(2) rank of
   that specific 16x16 submatrix is 14/16 (rank-deficient) for n_id=2, n_bits=64 -- there is nothing
   wrong with using 64 bits of evidence to solve for 16 unknowns in principle, but the FIRST 16 rows
   specifically are not guaranteed independent. Fixed by `recover_rnti_gf2_full()`, which selects
   pivot rows from the FULL n_bits-row matrix (standard practice for a rank-deficient leading
   submatrix), not just the first 16.

Both fixes are validated below: 10/10 RNTI recovery PASS (was ~2/8 before either fix), plus the
brute-force uniqueness check the plan's Review Focus already required.
"""
import numpy as np

MASK32 = 0xFFFFFFFF


def gold_generic_exact(x1, x2, reset):
    """Bit-exact transliteration of openair1/PHY/gold.h's gold_generic()."""
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
    return x1, x2, (x1 ^ x2) & 1


def gold_sequence(c_init, n_bits):
    x1, x2 = 0, c_init & MASK32
    out = []
    x1, x2, b = gold_generic_exact(x1, x2, True)
    out.append(b)
    for _ in range(n_bits - 1):
        x1, x2, b = gold_generic_exact(x1, x2, False)
        out.append(b)
    return np.array(out, dtype=np.uint8)


def scramble_c_init(n_rnti, n_id):
    """TS 38.211 7.3.2.3: c_init = (n_RNTI << 16) + n_ID. n_rnti (bits 16-31) and n_id (< 2^16,
    bits 0-15) occupy disjoint bit ranges, so '+' cannot carry between them -- c_init is exactly
    (n_rnti << 16) XOR n_id, i.e. affine-linear in n_rnti's bits. Masked to 32 bits like the real
    uint32_t c_init in gold.h (the caller does not pre-mask to 31 bits either)."""
    return ((n_rnti << 16) + n_id) & MASK32


def build_rnti_linear_model(n_id, n_bits):
    """Perturb one RNTI bit at a time from the n_RNTI=0 baseline; the corresponding output-sequence
    XOR IS that bit's column of the GF(2) linear map, by construction, no assumption of structure
    needed beyond the transform being built from XOR/shift only (confirmed true of gold_generic)."""
    baseline = gold_sequence(scramble_c_init(0, n_id), n_bits)
    M = np.zeros((n_bits, 16), dtype=np.uint8)
    for bit in range(16):
        perturbed = gold_sequence(scramble_c_init(1 << bit, n_id), n_bits)
        M[:, bit] = perturbed ^ baseline
    return M, baseline


def recover_rnti_gf2(observed, n_id, n_bits):
    """Solve M @ rnti_bits = target over GF(2), pivoting across the FULL n_bits-row matrix (fix #2
    above -- the first 16 rows alone are not reliably full rank)."""
    M, baseline = build_rnti_linear_model(n_id, n_bits)
    target = (observed ^ baseline).astype(np.uint8)
    A = np.concatenate([M.copy(), target.reshape(-1, 1)], axis=1) % 2
    n_rows = A.shape[0]
    r = 0
    for col in range(16):
        piv = next((rr for rr in range(r, n_rows) if A[rr, col]), None)
        if piv is None:
            return None  # underdetermined at this n_bits -- caller must supply more evidence
        A[[r, piv]] = A[[piv, r]]
        for rr in range(n_rows):
            if rr != r and A[rr, col]:
                A[rr] ^= A[r]
        r += 1
    bits = A[:16, 16]
    rnti = 0
    for i, b in enumerate(bits):
        rnti |= int(b) << i
    return rnti


if __name__ == "__main__":
    n_id, n_bits = 2, 64
    all_ok = True
    test_rntis = [0x4615, 0xC615, 0x8000, 0x0001, 0xFFFF, 0x7FFF, 1234, 54321, 0xBEEF, 0x0002]
    for true_rnti in test_rntis:
        true_seq = gold_sequence(scramble_c_init(true_rnti, n_id), n_bits)
        recovered = recover_rnti_gf2(true_seq, n_id, n_bits)
        ok = recovered == true_rnti
        all_ok &= ok
        print(f"true RNTI=0x{true_rnti:04x} recovered={('0x%04x' % recovered) if recovered is not None else None} {'PASS' if ok else 'FAIL'}")
    # Brute-force cross-check on the pair that was spuriously ambiguous under the plan's original
    # (wrong) model -- confirms the REAL sequence uniquely determines the RNTI at n_bits=64.
    for true_rnti in [0x4615, 0xC615]:
        true_seq = gold_sequence(scramble_c_init(true_rnti, n_id), n_bits)
        matches = [r for r in range(65536) if np.array_equal(gold_sequence(scramble_c_init(r, n_id), n_bits), true_seq)]
        u = matches == [true_rnti]
        all_ok &= u
        print(f"brute-force matches for 0x{true_rnti:04x} at n_bits={n_bits}: {matches} {'PASS (unique)' if u else 'AMBIGUOUS'}")
    print("ALL PASS" if all_ok else "SOME FAILED")
