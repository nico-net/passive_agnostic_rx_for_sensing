# Task 8: PRB-set helpers — completion report

## Summary
Implemented complete PRB-set arithmetic for non-contiguous PDSCH allocations: RA type 0, dynamicSwitch FDRA, interleaved VRB→PRB mapping, and PRG segment splitting.

## Work done (TDD flow)

### RED: Test creation
Created `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_prb_set_test.cc` with 9 test cases covering:
- RBG size lookup (TS 38.214 Table 5.1.2.2.1-1)
- Type-0 bitmap allocation with aligned/misaligned BWPs
- DynamicSwitch field parsing (MSB-based type selection)
- Interleaved VRB→PRB mapping (bundle sizes L∈{2,4})
- PRG segment extraction with PRB bundling boundaries

### GREEN: Implementation
Implemented two new source files:

**`nr_pdsch_prb_set.h`**: Public interface, 39 lines
- `nr_rbg_size()`: RBG size P from BWP span and config2
- `nr_rbg_count()`: RBG count accounting for misaligned start
- `nr_ra_type0_prbs()`: Extract PRB list from RBG bitmap
- `nr_fdra_dynamic_split()`: Parse dynamicSwitch field (TS 38.212 7.3.1.2.2)
- `nr_vrb_to_prb_interleaved()`: Interleaved bundle-domain permutation (TS 38.211 7.3.1.6)
- `nr_prb_segments()`: Segment PRB list by contiguity and PRG boundaries

**`nr_pdsch_prb_set.c`**: Implementation, 86 lines
- All functions match spec text exactly (hand-derived test expectations verified by executing all 9 tests)
- No dependencies outside stdint.h/stdbool.h
- Safe input validation: bounds checks, null pointer checks, return -1 on overflow

### Test verification
All 9 tests pass:
```
[==========] Running 9 tests from 1 test suite.
[ RUN      ] PrbSet.RbgSizeTable                                  [       OK ]
[ RUN      ] PrbSet.Type0AlignedBwp                               [       OK ]
[ RUN      ] PrbSet.Type0MisalignedBwpHasShortFirstAndLastRbg     [       OK ]
[ RUN      ] PrbSet.DynamicSwitchMsbSelectsType                   [       OK ]
[ RUN      ] PrbSet.InterleavedVrbEvenBundleCount                 [       OK ]
[ RUN      ] PrbSet.InterleavedVrbSevenBundles                    [       OK ]
[ RUN      ] PrbSet.InterleavedVrbMisalignedIsIdentityWithThreeBundles [       OK ]
[ RUN      ] PrbSet.InterleavedVrbIsAPermutation                  [       OK ]
[ RUN      ] PrbSet.SegmentsFollowDataOrderAndPrgBoundaries       [       OK ]
[==========] 9 tests, 1 test suite. [  PASSED  ]
```

### Integration
- Modified `CMakeLists.txt`:
  - Added `${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.c` to `add_library(nr_pdcch_blind_monitor ...)`
  - Registered `test_nr_pdsch_prb_set` executable and test after `test_nr_pdcch_joint_live` block (line ~2562)
- `nr_pdcch_blind_monitor` library rebuilds successfully with new source
- No regressions: all existing tests still pass

## Commit
- **SHA**: 2895cc06cd (short form: `2895cc06cd`)
- **Message**: "Passive PDSCH: PRB-set arithmetic for RA type 0, dynamicSwitch, interleaved VRB and PRG segments"
- **Files modified**: 4 (1 modified, 3 new)
  - CMakeLists.txt: +9/-1
  - nr_pdsch_prb_set.c: +86 new
  - nr_pdsch_prb_set.h: +39 new
  - nr_pdsch_prb_set_test.cc: +101 new

## No concerns
- All tests pass with expected values (no values changed to satisfy tests)
- Library still builds and integrates cleanly
- Code follows OAI style and uses standard C (C99)
- No external dependencies beyond stdlib

## Next steps
Tasks 9–12 wire these helpers into PDSCH channel estimation, CFR extraction, and RE enumeration for non-contiguous allocations.
