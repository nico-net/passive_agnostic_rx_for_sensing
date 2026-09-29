# Merge-conflict resolution report: adaptive-rx-UL-DL -> sdd/agn-fix

**Date:** 2026-09-26
**Lane:** fix, `sens6:/home/sens/NICOLA/agn-wt/fix`, branch `sdd/agn-fix`
**Merge commit:** `8b96b6d266` "Merge adaptive-rx-UL-DL into sdd/agn-fix: bank module carries Task 15 AL1-family state"

## Starting state

A previous agent's session had ended mid-merge (`git merge --no-ff adaptive-rx-UL-DL`, `MERGE_HEAD`
set). Inspection showed the previous agent had already done nearly all of the real resolution work
before running out of session: both `CMakeLists.txt` and `nr_pdcch_blind_monitor_rt.c` had their
conflict markers manually removed (still showing `UU` in `git status` only because `git add` was
never run), and it had additionally hand-edited `nr_pdcch_coreset_bank.{h,c}` (not conflicted by
git, since only the `fix` side introduced that file) to carry Task 15's new per-entry AL1 fields,
plus `nr_pdcch_blind_monitor.c`/`nr_pdcch_blind_monitor_rt.h` (also not git-conflicted, staged
cleanly by the auto-merge) to resolve a second, independent overlap. My job was to verify that
work was correct and complete, then finish: stage, build, test, commit.

## The two conflicted hunks

### 1. `CMakeLists.txt` (one hunk, `nr_pdcch_blind_monitor` library's `add_library()` line)
- **fix side** added `nr_pdcch_coreset_bank.c` and `nr_tdd_pattern.c` to the source list (R14's bank
  move + R14 follow-up's `nr_tdd_pattern.c` move).
- **adaptive-rx-UL-DL side** added `nr_pdsch_qm_oracle.c` (Task 16, unrelated).
- **Resolution (already done):** union of both -- the line carries all three additions. Verified: no
  stray hunks elsewhere in the file (`git diff --cc` showed exactly one `@@@` block).

### 2. `nr_pdcch_blind_monitor_rt.c` (7 hunks)
- **fix side**: deleted the entire multi-CORESET bank block (`g_coreset_bank[8]`,
  `_Atomic g_coreset_bank_n`, `nr_pdcch_blind_monitor_bank_has_geometry`, `coreset_same_geometry`,
  `coreset_bank_covers/_has_owner/_length_hint/_add`) from this file -- moved verbatim into the new
  `nr_pdcch_coreset_bank.{h,c}` in the offline-linked library, called back into via accessors.
- **adaptive-rx-UL-DL side (Task 15)**: added AL1-union decoding state and logic directly onto the
  old `g_coreset_bank[]` struct/globals: `al1_fam[]`/`n_al1_fam`/`al1_bank_out`/`n_al1_union`/
  `al1_union[][6]` fields, `g_al1_mu` mutex, `al1_bank_map`/`al1_union_refresh`/`al1_union_store`/
  `al1_from_ladder`/`al1_union_accept`/`al1_verify_report`, plus AL16 lane support
  (`LANE_BATCH_AL_MAX` 8->16, `LANE_RE_PER_LANE` resized, `nr_pdcch_blind_parse_lane_als`/
  `nr_pdcch_blind_lane_re_budget`), and changed `coreset_bank_add()` to return the entry's index
  (needed by `al1_union_store(bi, ...)`).
- **Resolution rule (per brief)**: bank struct + pure ops stay in `nr_pdcch_coreset_bank.{h,c}`;
  Task 15's new per-entry fields move there too; RT-only logic (union decode, virtual-CCE reorder,
  mutex, `al1_from_ladder`) stays in `rt.c`, reading/writing the entry through
  `nr_pdcch_coreset_bank_entry(bi)` instead of `&g_coreset_bank[bi]`.
- **What was already done and verified correct:**
  - `nr_pdcch_coreset_bank.h`'s `nr_pdcch_discovered_coreset_t` carries the AL1 fields, with a
    comment explaining they're guarded by `rt.c`'s `g_al1_mu` (not a bank-owned lock) and are
    exposed rather than accessor-hidden because RT code mutates them in place under that mutex.
  - `nr_pdcch_coreset_bank.c` unchanged in its own logic; `nr_pdcch_coreset_bank_add()` returns
    `at` (the index) as Task 15 needs, and its slot-reuse `memset(&g_coreset_bank[at], 0, sizeof(...))`
    zeros the whole struct, AL1 fields included, for free.
  - New accessors used throughout `rt.c`: `nr_pdcch_coreset_bank_count()`, `_cfg(i)`, `_entry(i)`,
    `_covers(...)`, `_has_owner(rnti)`, `_length_hint()`, `_add(cfg, owner)`. Grepped the whole file:
    zero remaining references to the old `g_coreset_bank`/`g_coreset_bank_n`/`coreset_bank_*` names.
  - `g_al1_mu` lock/unlock pairs balanced (checked both call sites: the AL1-position-building block
    around line 3184-3219, and the three functions in the 187-330 range).
  - A third, independent overlap this same file had (not called out in the brief but real): both
    sides touched the lane-AL-list machinery. Resolved by moving `nr_pdcch_blind_parse_lane_als()`/
    `nr_pdcch_blind_lane_re_budget()` and the `LANE_BATCH_AL_MAX`/`LANE_RE_PER_LANE`/
    `NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS` macros into `nr_pdcch_blind_monitor.c`/
    `nr_pdcch_blind_monitor_rt.h` (the already offline-linked library + its header) instead of
    leaving them in `rt.c` -- this is exactly the pattern Task 15's own report flagged as the
    needed follow-up ("if the other lane fixes the link by stubbing rt.c symbols rather than
    linking rt.c, this test will need the same treatment"). Verified single definition point (macros
    in the header only, functions in the .c only) and that `rt.c` still compiles against them via
    the shared header.

## My contribution

The previous agent had done all the substantive resolution. I verified it by reading every hunk,
confirmed no leftover conflict markers anywhere in the tree (`grep -rn '<<<<<<<\|=======\|>>>>>>>'`,
one unrelated hit in `channel_sim.c` log text), confirmed the CMakeLists.txt test-target definitions
(`test_nr_pdcch_al1_map`, `test_nr_tdd_pattern`, `test_nr_passive_acq_state`, etc.) still reference
their intended sources with no duplication conflicts, staged the two conflicted files plus the four
files the previous agent had edited outside the conflict markers
(`nr_pdcch_blind_monitor.c`, `nr_pdcch_blind_monitor_rt.h`, `nr_pdcch_coreset_bank.{h,c}`), and
committed.

One real blocker hit along the way: at merge-inspection time a live capture (`nr-uesoftmodem
--passive-rx` on the X410, a different worktree, `multirx-clean-adaptive`) was running on sens6.
Per the lane rule ("never build while a capture is running"), I committed the merge (a git-only
operation, no build) and then used the Monitor tool to wait for the capture to exit rather than
polling/sleeping or building through it. Build and test ran only after the capture process was
confirmed gone (`pgrep -x nr-uesoftmodem` -> none).

## Build

`ssh sens6 "/home/sens/NICOLA/agn-wt/lane-make.sh fix nr-uesoftmodem test_nr_pdcch_blind_monitor test_nr_pdcch_al1_map test_nr_passive_acq_state test_nr_tdd_pattern test_nr_pdsch_config_sweep test_nr_csirs_blind_search"`

All six targets + `nr-uesoftmodem` built with 0 errors. Only pre-existing warnings surfaced (both
named in earlier fix-round reports): unused `n_verified` at `rt.c:2780` and a zero-length printf
format at `rt.c:3691`.

## Tests

`ctest` from `$B`, filtered to this task's targets:

```
Test #10: test_nr_dl_adaptive .............. Passed  0.15 sec
Test #13: test_nr_pdcch_blind_monitor ...... Passed  0.95 sec
Test #21: test_nr_passive_acq_state ........ Passed  0.00 sec
Test #24: test_nr_pdsch_config_sweep ....... Passed 37.27 sec
Test #26: test_nr_csirs_blind_search ....... Passed  0.00 sec
Test #34: test_nr_pdcch_al1_map ............ Passed  0.44 sec
Test #38: test_nr_tdd_pattern .............. Passed  0.00 sec
100% tests passed, 0 tests failed out of 7
```

Full unfiltered `./test_nr_pdcch_blind_monitor`:

```
143 tests from 21 test suites ran.
PASSED: 141
SKIPPED: 2 (PdcchReplay.OtaCss0DecoderContract, PdcchReplay.BudgetTimingEveryWidth -- both
            pre-existing, unrelated to this merge, documented in fix-link-report.md)
FAILED: 0
```

Also spot-checked `LookaheadLanes.Al16IsAcceptedAndFitsTheLaneBudget` (Task 15's AL16 test,
previously unable to link at all per that task's own report) in isolation: **PASS**.

## Concerns

None. The merge is behaviourally the union of both branches' intent as described in the brief:
CORESET-bank bookkeeping lives in the library (`nr_pdcch_coreset_bank.{h,c}`), Task 15's AL1-family
state rides along in the same struct guarded by the same `g_al1_mu` semantics as
adaptive-rx-UL-DL had it, and RT-only logic (union decoding, virtual-CCE reorder, mutex,
`al1_from_ladder`) stayed in `rt.c`. No test assertions were relaxed. No production decision logic
was changed -- this was a link-topology-and-field-location resolution only.
