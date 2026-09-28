# Task 2 Report: Fix CSI-RS row/port mismatch and stale partial-band test

## Summary

Commit 358318fade (by Nicola Gallucci, 2026-09-25 19:55:41 UTC) implemented all required changes from the brief. All tests pass with PARTIAL-BAND whole-band z=2.06 (< 2.5 requirement). Both test binaries (test_nr_csirs_blind_search, test_nr_csirs_blind_synth) compile and run successfully. Ready to ship.

## Verification Against Brief

### Step 1: Add failing tests ✓

The commit adds:
- New `TEST(CsirsBlindEnum, RowPortsMatchTheSpecTable)` with correct expectations per TS 38.211 Table 7.4.1.5.3-1:
  - rows 1,2 = 1 port
  - row 3 = 2 ports
  - rows 4,5 = 4 ports
  - row 6 = 0 (not enumerated)
- Modifies `EnumeratesRealConfigurationsOnly` to use `EXPECT_GT(nr_csirs_blind_row_ports(c[i].row), 0)` instead of hardcoded row list
- Updates symbol range expectations (0-13 instead of 2-12) to accommodate all enumerated rows
- Updates `WholeBandMeanDilutesAPartialBandResource` to fill with noise instead of zeros (models on-air unrelated REs correctly)

### Step 2: Implementation ✓

The commit adds:
- `nr_csirs_blind_row_ports()` declaration in `nr_csirs_blind_search.h` with proper docstring
- `nr_csirs_blind_row_ports()` implementation in `nr_csirs_blind_search.c`:
  - Iterates through `kRows` table
  - Returns port count from `kPorts` table for enumerated rows
  - Returns 0 for unknown rows
- Replaces switch statement in `nr_csirs_blind_rt.c` with call to `nr_csirs_blind_row_ports()`
- Adds guard check: `if (n_ports <= 0 || n_ports > NR_CSIRS_BLIND_RT_MAX_PORTS) return;`

All changes match the brief exactly.

## Build and Test Results

### Build

```bash
ssh sens6 "/home/sens/NICOLA/agn-wt/lane-make.sh csi test_nr_csirs_blind_search test_nr_csirs_blind_synth nr-uesoftmodem"
```

**Result:** All targets built successfully. Build completed with 0 errors, 0 warnings relevant to the modified files.

### Test Execution

#### test_nr_csirs_blind_search

```bash
ssh sens6 "cd /home/sens/NICOLA/agn-wt/csi/cmake_targets/ran_build/build && ctest -R 'nr_csirs_blind' --output-on-failure"
```

**Output:**
```
Test project /home/sens/NICOLA/agn-wt/csi/cmake_targets/ran_build/build
    Start 25: test_nr_csirs_blind_search
1/2 Test #25: test_nr_csirs_blind_search .......   Passed    0.00 sec
    Start 26: test_nr_csirs_blind_synth
2/2 Test #26: test_nr_csirs_blind_synth ........   Passed    0.02 sec

100% tests passed, 0 tests failed out of 2
```

**PARTIAL-BAND Result:**
```
PARTIAL-BAND: whole-band z=2.06, best-run z=6.35 over blocks [0..1]
[  PASSED  ] 24 tests.
```

**Status:** ✓ PASS - whole-band z=2.06 < 2.5 requirement met

#### test_nr_csirs_blind_synth

**Output:**
```
SCAN: z_true=5.94 (n_used=819) best_other=1.59 epr_true=1805.96
nr_csirs_blind_synth_check: PASS
```

**Status:** ✓ PASS

## RED Evidence (Pre-Commit State)

As stated in the brief, the two tests failed before these changes. RED evidence was not captured in this session (the commit pre-exists on the branch). Per the brief's instruction to "record honestly in the report that RED was not captured by you, and cite the brief's statement that the two tests failed before (pre-existing failures)," the following is noted:

- The brief explicitly states: "The enumeration test still asserts rows {1,2,4}, and the unused `kPorts` table is the compiler warning that points at the gap."
- The brief states: "`WholeBandMeanDilutesAPartialBandResource` zero-fills 80 % of the band, but the block correlator skips zero-energy blocks by design (`if (e_rx > 0.0 && e_ref > 0.0)`), so zeros no longer model 'unrelated REs'."

Both failures are confirmed by the brief itself as pre-existing. The commit correctly addresses both issues.

## Commit Details

- **Commit:** 358318fade9585adcb7d69fee6cd46d3a3d1de4f
- **Branch:** sdd/agn-csi (lane: csi)
- **Author:** Nicola Gallucci
- **Date:** Fri Sep 25 19:55:41 2026 +0000
- **Files Modified:**
  - openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c (14 ++ 14 --)
  - openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c (8 ++)
  - openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h (4 ++)
  - openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_blind_search_test.cc (24 ++ 5 --)

## Conclusion

All task requirements have been met:

1. ✓ Commit implements all changes from the brief
2. ✓ Both test binaries compile and run without errors
3. ✓ test_nr_csirs_blind_search: PASSED (24 tests), PARTIAL-BAND whole-band z=2.06 < 2.5
4. ✓ test_nr_csirs_blind_synth: PASS
5. ✓ Symbol range assertion widened from {2..12} to {0..13} to accommodate all enumerated rows; no thresholds relaxed
6. ✓ Commit message includes required attribution

**Status: READY TO SHIP** (after fix round 1 below)

---

## Fix Round 1: Loop Bound Defensive Form

**Finding:** `nr_csirs_blind_row_ports()` used `sizeof(kRows)` as loop bound, which only worked because kRows is uint8_t. The sibling function `nr_csirs_blind_row_needs_bits()` correctly uses `sizeof(kRows) / sizeof(kRows[0])`.

### Change

In `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c` line 232:

```c
// Before:
for (unsigned i = 0; i < sizeof(kRows); i++)

// After:
for (unsigned i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
```

### Commit

```
Commit: 3aa2a8e53a
Message: Blind CSI-RS: element-count loop bound in nr_csirs_blind_row_ports
```

### Rebuild and Test

```bash
ssh sens6 "/home/sens/NICOLA/agn-wt/lane-make.sh csi test_nr_csirs_blind_search"
```

**Build Result:** ✓ Built target test_nr_csirs_blind_search

**Test Result:**
```bash
./test_nr_csirs_blind_search 2>&1 | grep -E 'PASSED|PARTIAL-BAND'
```

**Output:**
```
PARTIAL-BAND: whole-band z=2.06, best-run z=6.35 over blocks [0..1]
[  PASSED  ] 24 tests.
```

**Status:** ✓ PASS - All tests pass with identical metrics. Fix verified.

---

**Final Status: READY TO SHIP**
