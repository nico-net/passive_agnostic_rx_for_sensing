# Task 18 report: DM-RS oracle reads the wrong PRBs when the BWP does not start at CRB 0

## Status: DONE

## Defect confirmed

`dmrs_oracle_measure()` in `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (~line 453)
indexed `coh[p]` (from `nr_dmrs_prb_coherence()`) and the physical subcarrier `k` directly from
the grant's BWP-relative `rb0`. `nr_dmrs_prb_coherence()` (`nr_dmrs_id_estimate.c:117-118`) fills
`out[]` by **absolute carrier CRB, CRB0-referenced** ("Per-PRB DM-RS pair coherence over the WHOLE
carrier ... refPoint 0" — confirmed from the code, not just the comment: it builds pilots for the
whole `N_RB_DL` carrier and never sees the grant's `BWPStart`). The sibling rank/CDM/DM-RS-ID
probes in the same file (lines ~999, ~1001, ~1051-1052) already do this correctly:
`start_sc = fp->first_carrier_offset + (pdu->BWPStart + pr_rb0) * 12`. `dmrs_oracle_measure()` and
its two callers (the oracle gate and the Task 14 k0 probe) were missing that `+ BWPStart`.

## Call-site audit (every site checked, per the brief's steps)

- `dmrs_oracle_measure()` definition (~line 453-497): confirmed the bug — `rb0` used raw for both
  `coh[p]` indexing and `k = fp->first_carrier_offset + p*12 + r`.
- Oracle-gate call (~line 752, was reported at ~queue.c:689-703 by the Task 9 reviewer before Task
  14's additions shifted line numbers — same site, now identified precisely): feeds `rb0` from
  `probe_span()`, which returns the grant's BWP-relative index (`nr_pdsch_prb_set.h`'s own doc:
  "All PRB indices are BWP-relative"). Buggy.
- k0-probe call (~line 805, Task 14's code): reuses the **same** `rb0`/`nrb` local variables as the
  oracle-gate call above it (both live inside the same `if` block). One fix point covers both.
- The other `coh[2][275]` block (~lines 712-723, the BWP-discovery probe) was checked and is
  **not** affected: it scans `p = 0 .. nrb-1` over the whole carrier for BWP discovery itself and
  never mixes in a grant's `rb0` — correctly out of scope.
- No other `dmrs_oracle_measure()` callers exist (`grep -n dmrs_oracle_measure` returns exactly the
  definition + these two call sites).

## Fix

1. **New pure helper** `nr_dmrs_oracle_crb(int bwp_start, int rb0)` in
   `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.{h,c}` (the file already documents itself as
   "Frequency-domain resource arithmetic ... All PRB indices are BWP-relative" — the natural home
   for the one CRB-absolute exception). Body: `return bwp_start + rb0;` — trivial by design; bounds
   safety is handled where it already lived, by the existing `p < fp->N_RB_DL` check inside
   `dmrs_oracle_measure()`'s loops, now checking an absolute CRB against the absolute carrier size
   (previously checking a BWP-relative value against the carrier size, which happened to be correct
   only at `BWPStart = 0`).
2. **Applied once**, where the shared `rb0` is computed for both call sites
   (`nr_pdsch_passive_queue.c` ~line 747):
   ```c
   const int rb0 = nr_dmrs_oracle_crb(job.dlsch_pdu.BWPStart, oracle_rb0), nrb = oracle_nrb;
   ```
   This fixes the oracle-gate call and the k0-probe call in one edit, since both read this same
   `rb0`.
3. Tightened `dmrs_oracle_measure()`'s doc comment to state the ABSOLUTE-CRB contract explicitly, so
   a future caller can't reintroduce this by construction.
4. `BWPStart = 0` is bit-identical: `nr_dmrs_oracle_crb(0, x) == x` (asserted by test).

## TDD evidence

**RED** — test added to `nr_pdsch_prb_set_test.cc` referencing the not-yet-declared/implemented
helper; build of `test_nr_pdsch_prb_set` fails to compile:
```
error: 'nr_dmrs_oracle_crb' was not declared in this scope
```
(4 errors, one per `EXPECT_EQ` call — full output captured during the session.)

**GREEN** — after adding the declaration to `nr_pdsch_prb_set.h` and the one-line implementation to
`nr_pdsch_prb_set.c`:
```
$ ssh sens6 "/home/sens/NICOLA/agn-wt/lane-make.sh t18 test_nr_pdsch_prb_set"
Building CXX object CMakeFiles/test_nr_pdsch_prb_set.dir/.../nr_pdsch_prb_set_test.cc.o
Building C object CMakeFiles/test_nr_pdsch_prb_set.dir/.../nr_pdsch_prb_set.c.o
Linking CXX executable test_nr_pdsch_prb_set
Built target test_nr_pdsch_prb_set

$ ssh sens6 "cd .../build && ctest -R test_nr_pdsch_prb_set --output-on-failure"
1/1 Test #33: test_nr_pdsch_prb_set ............   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

Test cases (in `PrbSet.DmrsOracleCrbAddsBwpStart`):
- `nr_dmrs_oracle_crb(0, 0) == 0`, `nr_dmrs_oracle_crb(0, 5) == 5` — BWPStart = 0 unchanged.
- `nr_dmrs_oracle_crb(20, 5) == 25` — the brief's own example.
- `nr_dmrs_oracle_crb(260, 12) == 272` — carrier-edge case (last valid CRB index of a 273-PRB
  carrier).

Then applied the real fix to `nr_pdsch_passive_queue.c` and rebuilt `nr-uesoftmodem` (first full
build for this fresh lane build dir — succeeded, exit code 0) plus both required regression
targets:
```
$ ssh sens6 "cd .../build && ctest -R 'test_nr_pdsch_config_sweep|test_nr_pdsch_prb_set' --output-on-failure"
1/2 Test #24: test_nr_pdsch_config_sweep .......   Passed   36.25 sec
2/2 Test #33: test_nr_pdsch_prb_set ............   Passed    0.01 sec
100% tests passed, 0 tests failed out of 2
```

## Commit

```
a5e735ec47 Passive PDSCH: DM-RS oracle indexes carrier CRBs (add BWPStart)
 4 files changed, 30 insertions(+), 2 deletions(-)
```
Files: `nr_pdsch_passive_queue.c`, `nr_pdsch_prb_set.c`, `nr_pdsch_prb_set.h`,
`tests/nr_pdsch_prb_set_test.cc` — exactly the task's files, `git diff --stat` verified before
staging (explicit paths only, no `git add -A`).

## Process note (not a code concern)

Before the first build, `pgrep -x nr-uesoftmodem` on sens6 found a live capture running (PIDs
608058/608059, a different worktree `multirx-clean-adaptive`, X410 hardware, `-r 273`, under a
`timeout 150` already at 110s elapsed). Per the lane rule, I did not build; polled
(`until ! pgrep ...`) until it exited naturally (~30s later) rather than reporting BLOCKED, then
re-verified clear before every subsequent build/commit step. No build ran concurrently with a
capture.

## Concerns

None. `BWPStart = 0` (the lab cell) is provably unchanged (identity addition, asserted by test);
the fix is a single-point, minimal-diff change reusing the exact convention the sibling probes
already use; both required regression tests and a full `nr-uesoftmodem` build pass.
