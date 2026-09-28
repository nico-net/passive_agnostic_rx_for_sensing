# Task 4 Report: AL1 cover module (pure)

**Date:** 2026-09-25  
**Status:** COMPLETED  
**Commit:** `9f25513b4c` Blind PDCCH: AL1 cover of the CCE-to-REG mapping catalogue (pure module)

## Summary

Implemented a pure C module for AL1 PDCCH candidate enumeration and covering, enabling blind discovery to reduce the CCE-to-REG mapping search space from 81–1081 mappings to 2–11 minimal covers.

## TDD Execution

### Implementation
- Created `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.h` (header with 5 public functions)
- Created `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.c` (286 lines, three core algorithms)
- Created `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_al1_map_test.cc` (7 gtest cases)
- Modified `CMakeLists.txt` to register test and add source to `nr_pdcch_blind_monitor` library

### Test Results (GREEN)

All **7 tests PASSED** (123 ms total):

| Test | Status | Time | Notes |
|------|--------|------|-------|
| EnumerationMatchesTheSpecCount | PASS | 0 ms | All 5 shapes enumerated correctly (81, 181, 1081, 946, 865 mappings) |
| RegsetIsSortedAndInRange | PASS | 0 ms | REG sets properly sorted and bounds-checked |
| CoverUnionEqualsFullUnion | PASS | 45 ms | Cover union equals full enumeration for all shapes; distinct counts match expected |
| CoverFitsTheLaneBoundForEveryLegalShape | PASS | 72 ms | Every legal shape (6–270 RB, D=1–3) produces cover within `NR_PDCCH_AL1_MAX_COVER=32` |
| CoverIsFastEnoughForTheLanePath | PASS | 1 ms | **AL1 cover 270x2: 1.94 ms** (threshold 20 ms) ✓ |
| NarrowKeepsTheTruthAndCollapsesToItsFamily | PASS | 2 ms | Narrowing converges to single family; truth preserved |
| EveryBundle6MappingSharesTheNonInterleavedFamily | PASS | 0 ms | All L=6 mappings share non-interleaved family |

### Enumeration Count Verification

Expected vs. measured (all exact):
- 48 RB, D=1: 81 maps, 88 distinct, 11 cover ✓
- 270 RB, D=1: 181 maps, 90 distinct, 2 cover ✓
- 270 RB, D=2: 1081 maps, 990 distinct, 11 cover ✓
- 270 RB, D=3: 946 maps, 1080 distinct, 10 cover ✓
- 216 RB, D=2: 865 maps, 792 distinct, 11 cover ✓

No mismatches. Counts validated against independent Python enumeration (`scratchpad/al1_cover.py`, TS 38.211 7.3.2.2 rules).

### Library Integration

- `nr_pdcch_blind_monitor` library successfully compiled and linked
- Implementation added to source list for shared reuse
- Test executable links with GTest only (no other dependencies)

## Algorithms

### `nr_pdcch_al1_enumerate()`
Generates all legal mappings per TS 38.211 7.3.2.2:
- Non-interleaved (L=0)
- L in {2,6} (D=1,2) or {3,6} (D=3), R in {2,3,6}
- Every valid shift 0 to N_REG/L−1
- Output capped by `NR_PDCCH_AL1_MAX_MAPS=1200`

### `nr_pdcch_al1_regset()`
Maps one AL1 CCE to its 6 REGs under a given mapping:
- Non-interleaved: direct 6×cce+[0..5]
- Interleaved: per-bundle permutation with canonical reordering
- Validation: bundle/interleaver/shift legality, CCE in range

### `nr_pdcch_al1_cover()`
**Greedy set cover algorithm** to find minimal mapping subset whose AL1 families union equals the full catalogue union:
1. Enumerate all mappings, compute each family's REG set
2. Hash each REG set into dense index (open-addressing hash table, 8192 slots)
3. Greedy: iterate, select mapping with most uncovered REGs, mark covered
4. Non-interleaved always first (deterministic, spec-friendly)
5. Bounded by worst-case `O(nm)` where n=1081, m=135 CCEs (negligible)

Time complexity: O(nm) enumeration + O(n² · log m) greedy (dominated by qsort for family fingerprints). Measured 1.94 ms at worst case (270 RB, D=2).

### `nr_pdcch_al1_narrow()`
In-place filter: keep mappings whose AL1 family contains all observed REG sets.
- Computes family keys (one 64-bit key per REG set)
- For each candidate, checks inclusion of all observations
- Returns survivor count; capped at 16 observations

### `nr_pdcch_al1_family_count()`
Computes distinct AL1 families among candidates via FNV-1a fingerprinting.
- Sorts each family's keys, computes hash fingerprint
- Deduplicates by sorting fingerprints
- Enables "exact mapping" detection when count=1

## Notes

**Warnings (non-blocking):**
- Unused function `kset_find()` (line 94, nr_pdcch_al1_map.c): kept for symmetry, may be useful for future extensions
- Suggested braces in test (line 40): cosmetic, test logic is correct

**Design rationale:**
- Standalone module: no PHY header dependencies, pure TS 38.211 implementation
- Hash-backed union for correct coverage (vs. approximations)
- Deterministic greedy: first maximum wins, non-interleaved first
- 64-bit key encoding: supports N_REG ≤ 810 (practical for 270 RB × 3 symbols = 810)

## Files Modified
- `CMakeLists.txt`: +7 lines (test target) +1 source in library
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.h`: +42 lines
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.c`: +286 lines
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_al1_map_test.cc`: +149 lines

**Total:** 4 files, 485 new lines, 1 commit.

## Verification Checklist
- [x] All 7 tests pass
- [x] AL1 cover speed < 20 ms (measured 1.94 ms)
- [x] No count mismatches vs. expected enumeration
- [x] Library builds cleanly
- [x] Committed on `sdd/agn-al1` branch
- [x] Staged only affected files (no `git add -A`)
- [x] Attribution included in commit message
