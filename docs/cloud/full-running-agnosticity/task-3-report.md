# Task 3: Pin the CSI-RS search to the IDSWEEP candidate — Implementation Report

**Status:** COMPLETE  
**Date:** 2026-09-25  
**Git commit:** `1c226ed0f5` (sdd/agn-csi branch)

## What Was Implemented

Added `nr_csirs_blind_pin()` function and pinning state to the blind CSI-RS search module, enabling the receiver to serve a single candidate on every call while the scramblingID sweep is in progress, and while it awaits confirmation.

### Problem Solved

Round-robin over ~683 candidates only scores a candidate when its slot-assignment turn lands on a CSI-RS resource slot—roughly once per `n·period` calls (≈14 seconds at n=683, period=40). Once the scramblingID sweep identifies the correct sequence, the candidate then needs 3 periodic hits to confirm (via `nr_csirs_blind_infer_period`). Without pinning, those 3 hits arrive minutes apart. **With pinning, they arrive within 3 periods (~120 slots = 4 seconds at period=40).**

### Implementation Details

#### 1. State Fields Added to `nr_csirs_blind_state_t`
```c
int                  pinned;      ///< candidate served on every next() while pin_left > 0, or -1
uint32_t             pin_left;    ///< remaining pinned next() calls
```
Initialized to -1 and 0 respectively in `nr_csirs_blind_init()`.

#### 2. New Function: `nr_csirs_blind_pin()`
```c
void nr_csirs_blind_pin(nr_csirs_blind_state_t *st, int idx, uint32_t budget)
{
  if (st == NULL || idx < 0 || idx >= st->n)
    return;
  st->pinned = idx;
  st->pin_left = budget;
}
```
Marks a candidate for repeated service across up to `budget` calls to `nr_csirs_blind_next()`.

#### 3. Modified Function: `nr_csirs_blind_next()`
Now checks for an active pin **before** advancing round-robin:
```c
if (st->pin_left > 0 && st->pinned >= 0 && st->pinned < st->n) {
  st->pin_left--;
  return st->pinned;
}
// ... else round-robin resumes
```

#### 4. RT Integration in `nr_csirs_blind_rt.c`
Two call sites:

1. **IDSWEEP phase** (line ~391): After identifying a candidate worth sweeping scramblingIDs, pin it to visit on every call:
   ```c
   nr_csirs_blind_pin(&g_st, g_id_pin, NR_CSIRS_BLIND_PIN_SWEEP_CALLS);
   ```
   Budget = `32 * 640` (1024 IDs ÷ 32 per aligned visit × longest period).

2. **IDSWEEP solve phase** (line ~476): Once a scramblingID is confirmed, re-pin the same candidate to gather hits rapidly:
   ```c
   nr_csirs_blind_pin(&g_st, g_id_pin, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
   ```
   Budget = `4 * 640` (> CSIRS_MIN_HITS × longest period, ~2560 calls / 85 seconds at 30 Hz).

### Test Coverage

Two new unit tests added to `nr_csirs_blind_search_test.cc`:

1. **`CsirsBlindPin::PinnedCandidateIsServedUntilBudgetThenRoundRobinResumes`**
   - Pins candidate 7 for 3 calls
   - Verifies it is returned 3 times
   - Confirms round-robin resumes from its cursor (returning 0, 1)
   - ✅ PASS

2. **`CsirsBlindPin::PinnedPeriodicResourceConfirmsWithinThreePeriods`**
   - Pins candidate 7 for `NR_CSIRS_BLIND_PIN_CONFIRM_CALLS`
   - Simulates a periodic resource at period=40, offset=2
   - Verifies confirmation arrives within 2 periods + offset (≈82 calls)
   - ✅ PASS

### Build Status

All builds completed successfully:
- `test_nr_csirs_blind_search` ✅
- `test_nr_csirs_blind_synth` ✅
- `nr-uesoftmodem` ✅

All existing tests remain passing (0 failures).

## Scope and Design Notes

- **No breaking changes:** Pinning defaults off (`pinned = -1`); unmodified callers see no change.
- **Confirmation is automatic:** Once `nr_csirs_blind_feed()` confirms, the pin expires on the next call to `nr_csirs_blind_next()` (the confirmed candidate is then served forever via the `confirmed >= 0` branch).
- **Budget guards against drift:** If a pin budget expires before confirmation, round-robin naturally resumes, preventing infinite lockup on a bad candidate.
- **Two-phase sweep:** The IDSWEEP phase keeps the candidate visible while 1024 scramblingIDs are tried per slot; the IDSWEEP-SOLVED phase then keeps it pinned while hits accumulate for a periodicity test (which requires ≥3 occurrences, typically spanning 2-3 periods).

## Files Modified

| File | Changes |
|------|---------|
| `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h` | State fields; function decl + two `#define` macros |
| `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c` | Init code; `nr_csirs_blind_pin()` impl; `nr_csirs_blind_next()` modified |
| `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c` | Two pin call sites in the IDSWEEP logic |
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_blind_search_test.cc` | Two new test cases |

## Metrics

- **Lines added:** 63
- **Lines removed:** 1  
- **Net delta:** +62 lines
- **Test suite:** 2 new tests; 100% pass rate

## Next Steps (Out of Scope)

Once the pinning is live-validated to accelerate confirmation as expected:
- Measure wall-clock time to first confirmed CSI-RS on a real cell vs the pre-pinning baseline
- Validate that the two budgets (`SWEEP_CALLS` and `CONFIRM_CALLS`) are sufficient across cells with varying periods (4–640 slots)
- Consider per-period adaptive budgets if a fixed budget proves too tight or too loose on live traffic
