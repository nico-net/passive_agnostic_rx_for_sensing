# Task 6 report: Modulation-order oracle for the Technique D MCS table

## Status: DONE

Single commit `2b2b0c2fbd` on `sdd/agn-sweep` (worktree `sens6:/home/sens/NICOLA/agn-wt/sweep`).

## What I found on arrival

A prior implementer's uncommitted work was already present and, on inspection, matched the brief
**verbatim** for everything except Step 9:
- `CMakeLists.txt`: both `add_library(nr_pdcch_blind_monitor ...)` and
  `add_executable(test_nr_pdsch_config_sweep ...)` updated to include `nr_pdsch_qm_oracle.c`; new
  `test_nr_pdsch_qm_oracle` target added — matches brief exactly.
- `nr_pdsch_qm_oracle.h` / `.c` (new, untracked) — byte-for-byte the brief's Step 3 code.
- `tests/nr_pdsch_qm_oracle_test.cc` (new, untracked) — byte-for-byte the brief's Step 1 code.
- `nr_pdsch_config_sweep.h` / `.c` — `nr_pdsch_config_sweep_prune_qm` and `_observe_qm`,
  `qm_table_mask`, `prune_tables`, the `qm_tables`/`qm_obs` fields on `sweep_context_t`, the
  `#include "nr_pdsch_qm_oracle.h"` and `#include <stdlib.h>` — all matching the brief's Step 7
  exactly (confirmed against the controller-supplied line numbers: `ticket_context()` ~line 603,
  `sweep_context_t.reported` field, etc.).
- `tests/nr_pdsch_config_sweep_test.cc` — the four `PdschConfigSweepQm.*` tests from Step 5,
  verbatim.

**Not started**: Step 9 (wiring the oracle into `nr_pdsch_passive_decode.{h,c}` and
`nr_pdsch_passive_queue.c`) — no trace of `qm_measured` or `nr_pdsch_qm_oracle.h` anywhere in
those three files. I implemented this from scratch, matching the brief's Step 9 code exactly
against the controller-verified line anchors (`out->nvar = nvar;` at line 2357,
`pdtim_add(PDTIM_DEMOD, pdt_dem);` at 2601, `dec`/`dec2` at 683/710,
`nr_pdcch_dci11_layout_feedback(...)` at 911 — all confirmed present before editing).

## What I did

1. Verified no capture running (`pgrep -x nr-uesoftmodem` → none) before touching anything.
2. Read and diffed every uncommitted file against the brief; found it correct as summarized above.
3. Implemented Step 9:
   - `nr_pdsch_passive_decode.h`: added `uint8_t qm_measured;` to
     `nr_pdsch_passive_decode_result_t`, right after `nvar`.
   - `nr_pdsch_passive_decode.c`: added `#include "nr_pdsch_qm_oracle.h"`; `out->qm_measured = 0;`
     right after `out->nvar = nvar;`; and the Qm-oracle measurement block (select the symbol with
     the most valid data REs, same rule as the pre-existing EQDIAG code at ~line 2670, then call
     `nr_pdsch_qm_classify` on `rxdataF_comp[qm_m][0]`) right after `pdtim_add(PDTIM_DEMOD, ...)`.
   - `nr_pdsch_passive_queue.c`: added the `nr_pdsch_qm_oracle.h` include next to
     `nr_pdsch_config_sweep.h`; zero-initialised both `dec` (line 683) and `dec2` (line 710)
     declarations (`= {0}`); added the `nr_pdsch_config_sweep_observe_qm(...)` call, gated on
     `!job.sweep_ticket.settled && job.sweep_ticket.generation && dec.qm_measured`, immediately
     before the pre-existing `nr_pdcch_dci11_layout_feedback(...)` call.
   - All edits done by scp'ing the three files to local scratch, editing there, and scp'ing back
     (per lane-sweep.md's remote-edit workflow), then confirmed via `git diff --stat` on sens6 that
     only the intended lines changed.
4. Re-ran `cmake .` inside `$B` (required — CMakeLists.txt changed).
5. Built and ran both test binaries, then built `nr-uesoftmodem`, then re-ran ctest for both.

## Build/test results

- `test_nr_pdsch_qm_oracle`: built clean, **5/5 PASSED** (`QmOracle.McsTablesMatchTs38214`,
  `ClassifiesEveryOrderAtHighSnr`, `PureNoiseAbstains`, `LowSnr256QamAbstains`,
  `TooFewSymbolsAbstains`).
- `test_nr_pdsch_config_sweep`: built clean. Filtered run (`*Qm*:*RntiCache*`) — **5/5 PASSED**
  (the 4 new `PdschConfigSweepQm.*` tests plus `PdschConfigSweepRntiCache.EvictionProtects...`).
  Full suite: **33/34 PASSED, 1 SKIPPED** (`PdschReset.Timing`, a pre-existing timing-gated skip
  unrelated to this task).
- `nr-uesoftmodem`: built clean (`Built target nr-uesoftmodem`, no errors).
- Final `ctest -R 'test_nr_pdsch_config_sweep|test_nr_pdsch_qm_oracle'`: **100% tests passed, 0
  failed** (2/2).
- No test failed; nothing was retuned or relaxed.

## TDD evidence — honest accounting

I could **not** capture RED for the oracle tests (`test_nr_pdsch_qm_oracle`,
`PdschConfigSweepQm.*`) or the sweep-header/`.c` changes: their implementation (`nr_pdsch_qm_oracle.{h,c}`,
`nr_pdsch_config_sweep_prune_qm`/`_observe_qm`) was already present, alongside the tests, in the
uncommitted state I inherited — there was no window in which the tests existed without the
implementation on this machine. I confirmed this by reading `git status`/`git diff` before touching
anything (all shown in the transcript above); by the time I started, both halves of every
brief-specified TDD pair for Steps 1-8 were already on disk together.

Step 9 (the decode/queue wiring) has no dedicated unit test in the brief — it is exercised only by
building `nr-uesoftmodem` successfully and by the full-suite regression run — so there was no RED
run to capture there either; the closest evidence is that before my edit, `qm_measured` and
`nr_pdsch_config_sweep_observe_qm` were absent from `nr_pdsch_passive_decode.{h,c}` and
`nr_pdsch_passive_queue.c` (verified by `grep`, exit code 1, shown above), and after my edit the
full build succeeds with the wiring in place.

## Files touched

- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.h` (new, by prior implementer — verified)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.c` (new, by prior implementer — verified)
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_qm_oracle_test.cc` (new, by prior implementer — verified)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h` (by prior implementer — verified)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c` (by prior implementer — verified)
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc` (by prior implementer — verified)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h` (this session)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (this session)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (this session)
- `CMakeLists.txt` (by prior implementer — verified)

## Concerns

None outstanding. All brief-specified assertions passed unmodified; no test was retuned. Step 9's
live wiring has not been exercised on air in this session (that's outside scope — it needs an
actual capture, and the rule is never to build/capture concurrently); only the build + regression
suite confirm it compiles and links correctly.

## Fix round 1 (review finding)

**Finding (controller ruling)**: in `nr_pdsch_passive_queue.c`, `nr_pdsch_config_sweep_observe_qm()`
ran BEFORE `nr_pdsch_config_sweep_feedback()` for the same job. `prune_tables()` (called by
`observe_qm` when it prunes) compacts and re-indexes `st->hyp[]` without bumping the context
generation, so if the Qm-oracle prune fires on this job, `job.sweep_ticket.hypothesis` — resolved by
`nr_pdsch_config_sweep_feedback()` immediately after — can point at a hypothesis index that has
already moved, crediting this job's CRC outcome to the wrong surviving hypothesis. Ruling: keep the
duplicated prune tail in `prune_tables()` as-is; fix only the ordering.

**Change**: moved the Qm-oracle block (the `nr_pdsch_config_sweep_observe_qm()` call plus its
`LOG_A`) from before `nr_pdcch_dci11_layout_feedback()`/`nr_pdsch_config_sweep_feedback()` to
immediately after the `nr_pdsch_config_sweep_feedback()` call and its `CONVERGED` log line. Layout
feedback's own position relative to sweep-feedback is unchanged (still precedes it, as before); the
pre-existing DM-RS `observe_mask` call at ~line 682 (a separate, earlier tap) was not touched. Added
a comment at the new call site explaining why the ordering matters, so it isn't re-broken.

Verified no capture was running (`pgrep -x nr-uesoftmodem` → `none`) both before editing and before
building.

**Commands run**:
```
ssh sens6 "/home/sens/NICOLA/agn-wt/lane-make.sh sweep nr-uesoftmodem test_nr_pdsch_config_sweep"
ssh sens6 "cd .../build && ctest -R 'test_nr_pdsch_config_sweep|test_nr_pdsch_qm_oracle' --output-on-failure"
ssh sens6 "cd .../build && ./test_nr_pdsch_config_sweep --gtest_filter='*Qm*'"
ssh sens6 "cd .../build && ./test_nr_pdsch_qm_oracle"
```

**Output**:
- `nr-uesoftmodem`: `Built target nr-uesoftmodem` (clean, no errors).
- `test_nr_pdsch_config_sweep`: `Built target test_nr_pdsch_config_sweep` (clean).
- `ctest -R 'test_nr_pdsch_config_sweep|test_nr_pdsch_qm_oracle'`: **100% tests passed, 0 tests
  failed out of 2** (28.23 s / 0.01 s).
- `test_nr_pdsch_config_sweep --gtest_filter='*Qm*'`: **4/4 PASSED**
  (`PruneQmKeepsOnlyTablesPredictingTheMeasuredOrder`, `UninformativeMcsLeavesTheCatalogAlone`,
  `LiveObservationPrunesOnlyAfterTwoAgreeingSightings`, `ConflictingObservationsResetInsteadOfPruning`).
- `test_nr_pdsch_qm_oracle`: **5/5 PASSED**.

**Commit**: `d7603075b9` "Technique D: apply the Qm prune after this job's CRC feedback" on
`sdd/agn-sweep`, new commit (not amended), touches only `nr_pdsch_passive_queue.c` (1 file changed,
11 insertions, 5 deletions).
