# gap-misc lane report

Branch `sdd/gap-misc` on sens6 (`/home/sens/NICOLA/agn-wt/gap-misc`), forked from `sdd/validation`
@ `c295fa18f1`. Build dir configured fresh this session (`cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DENABLE_TESTS=ON -DAVX2=ON -DENABLE_ISAC_SENSING=ON -DT_TRACER=ON`, default Makefile generator —
matches `context.md`'s documented `make -j12`/ctest workflow; my first attempt used `-GNinja`,
which produced a cache with no `make`-visible targets and was reconfigured).

STATUS: **DONE** (6 of the original 7 fixes; fix 7 dropped per coordinator instruction — see below).

## Commits (sdd/gap-misc, oldest first)

1. `2d79cc4b96` — Fix stats-file cleanup targeting `$SCRIPT_DIR` instead of the run's real CWD.
2. `fb7adc9b2e` — Fix false "UPLINK DETECTED" verdict matching the receiver's own SENSING logs.
3. `62d495516e` — Add a hard-deadline watchdog so a run can't outlive duration + a bounded grace.
4. `4f97db7bd9` — Add `nr_dci11_pin_is_valid()` acquire accessor, use it in the RT scan thread.
5. `fa24a25ce9` — Fix stale `nvar` comment made incorrect by fix round 1 (`15658710b9`).
6. `8852f768cf` — Move `nr_pdsch_passive_alloc_normalise()` to the pure prb_set library, add tests.

Each commit's diff was verified in isolation (`git diff HEAD --stat`/`git diff HEAD --`) before
committing, by reconstructing intermediate file versions from the original (`git show HEAD:path`)
so multiple concerns landed in the same file (`run_passive_rx.sh`, `nr_pdsch_passive_decode.c`)
could still get one commit each.

## Item 1 — stats-file chmod path (commit 1)

`tests/passive_rx/run_passive_rx.sh` chmod'd/rm'd `nr*_stats*.log` under `$SCRIPT_DIR`
(`tests/passive_rx/`), but `nr-uesoftmodem` creates those files in its own CWD — wherever the
script itself was invoked from (it never `cd`s). Those only coincide under the harness's own
documented invocation (`cd $R && ./tests/passive_rx/run_passive_rx.sh`); any other invocation made
the cleanup a silent no-op, leaving the active UE's root-owned stats file blocking the passive
receiver's `fopen()` with an `AssertFatal`. Fixed by capturing `RUN_CWD="$PWD"` up front and using
it for both the `rm -f` and `chmod 666` targets.

## Item 2 — false "UPLINK DETECTED" (commit 2)

Root cause, confirmed with a synthetic log before touching the fix: the keyword grep
(`prach|RAPROC|preamble|Msg3|PUCCH|PUSCH`, case-insensitive) also matches the receiver's own
`SENSING: ... occ[...pusch=0...]` CFR-occupancy telemetry line and the blind-PDCCH module's own
PUSCH/PRACH decode diagnostic lines — none of which indicate the passive receiver itself
transmitting. Verified: a log containing only such `SENSING:`-tagged lines scored `ul=1` before the
fix, `ul=0` after. Fixed by excluding lines containing `SENSING:` from the match count — every one
of this receiver's own decode/telemetry lines carries that tag; a genuine self-transmission never
does.

## Item 3 — runs overrunning duration (commit 3)

Nothing between the `sleep "$DURATION"` and the `EXIT` trap firing was individually
timeout-guarded: `isac-track`/`merge_receivers_walltime*.py`/`rnti_gate.py` all run synchronously
with no per-command timeout, and the sequential per-UE/per-receiver `wait_for` attach budget alone
can reach tens of minutes (30 + `NUM_UE`*300 + `NUM_RX`*150 seconds in the worst case). This matches
what was observed live 2026-09-27 (a 600s run still alive 30+ minutes later). Added a background
watchdog, armed right after `trap cleanup EXIT INT TERM`, that SIGTERMs and (10s later) SIGKILLs
this run's own process group (looked up via `ps -o pgid=`, not assumed to equal `$$`) once
`30 + NUM_UE*300 + NUM_RX*150 + DURATION + WATCHDOG_GRACE_S` (default grace 600s) elapses.
`cleanup()` cancels the watchdog PID on a normal exit. This does not diagnose which specific step
hangs; it bounds the whole run regardless of which one does, which is what the task asked for.
**Not live-tested** (would require deliberately hanging a real capture, which risks colliding with
other lanes' live runs on sens6 — validated by `bash -n`, manual arithmetic trace, and code reading
only, per the task's own instruction to validate via shell-level tests / a short dry run rather than
a live harness run).

## Item 4 — `nr_dci11_pin.valid` acquire discipline (commit 4)

The brief cited 4 plain `pin->valid` reads at `nr_pdcch_blind_monitor_rt.c` lines
~5647/5648/5682/5691. Re-grepped fresh rather than trusting the inherited line numbers (per the
project's own "verify before asserting" rule): **only 3 occurrences exist on this tree**
(lines 5666/5687/5696 at the time of the fix) — the brief's numbers are stale by about 19 lines,
consistent with this file changing daily across lanes. Added `nr_dci11_pin_is_valid()` (acquire-
ordered, same cast idiom as the existing internal `pin_valid()` store-side helper) to
`nr_dci11_pin.{h,c}` and routed all 3 sites through it. New gtest
(`Dci11Pin.IsValidAccessorTracksFieldThroughSeedAndSelectTransitions`) drives the accessor through
never-seeded / seeded / rotated-out / reseeded states and asserts it agrees with the raw field at
every step.

## Item 5 — three deferred minors (commits 5 and 6)

- **Stale `nvar` comment** (commit 5): the "SEGMENTED ESTIMATE" comment in
  `nr_pdsch_passive_decode.c` claimed `nr_dl_chest_nvar_ant[]` "ends up holding the LAST segment's
  per-branch value." Traced via `git show 15658710b9:...` to confirm fix round 1 had already made
  this false (it's the width-weighted MEAN across segments, per the SAME commit's code a few lines
  below) — the comment simply never got updated. Fixed the wording; no code change.
- **`nr_pdsch_passive_alloc_normalise()` unit test** (commit 6): it had none because it lived in
  `nr_pdsch_passive_decode.c`, which pulls in `PHY_VARS_NR_UE`/NFAPI and can't link into the
  lightweight `test_nr_pdsch_prb_set` target. It only ever depended on `freq_alloc_bitmap_t`
  (`common/utils/bits.h`) and the already-pure, already-tested `nr_prb_list_normalise()` — so it was
  moved into `nr_pdsch_prb_set.{h,c}` (its declaration's own doc comment already called it "the
  pure core of nr_pdsch_passive_alloc_normalise()", i.e. this was always where it belonged).
  `nr_pdsch_passive_decode.h` now just includes `nr_pdsch_prb_set.h`, so all 3 existing call sites
  (`nr_pdsch_passive_decode.c`, `nr_pdsch_passive_queue.c`, `nr_pdcch_blind_monitor_rt.c`) needed no
  changes. Added 3 tests (no-op on a legacy grant, correct derivation on a valid list, untouched on
  an invalid one) — confirmed by building the full `PHY_NR_UE` static library from a clean working
  tree (see below) that moving it did not break the heavy target either.
- **Generation-bump fix for in-flight tickets after a DM-RS-mask prune: DROPPED, not committed.**
  See "Item 5's third minor" section below for why, and for the coordinator instruction that
  formally dropped it (superseded by lane `perf`'s `fe96d0c4b6` on `sdd/gap-perf`).

### Item 5's third minor: investigated, a naive fix was built, measured unsafe, and reverted

I implemented the generation bump exactly as specified ("in-flight tickets going stale after a
DM-RS-mask prune") in `nr_pdsch_config_sweep_observe()`: bump `c->generation` whenever
`prune_to_observed()` actually narrows the catalog (`prune_commit()`'s own doc comment: "indices
have moved, and keeping it would score one hypothesis with another's"). Building and running the
full `test_nr_pdsch_config_sweep` suite (`--gtest_shuffle`) immediately found a real regression:
`PdschConfigSweepTypeB.ObservingATypeBOnlyMaskWidensToExactlyItsMatchingEntries` FAILED
(`nr_pdsch_config_sweep_snapshot(&t, &st)` returned false where the test expects true). Root-caused,
not just observed: that test deliberately re-uses the SAME ticket to snapshot a context AFTER a
real type-A→type-B catalog replacement — a design the module already relies on (mirrors the
k0-layer mechanism per its own comment) and one my bump broke by construction, since
`ticket_context()`'s generation check is shared by every operation (`snapshot`/`observe`/
`feedback`/`add_k0`), not just the feedback-safety one the fix targeted.

Worse: reading `nr_pdsch_passive_queue.c` (the real production caller) showed this call site is
load-bearing in a way that makes a blanket bump actively harmful, not just test-breaking.
`nr_pdsch_config_sweep_observe(&job.sweep_ticket, mask, ...)` (line ~771, the DM-RS-mask oracle) and
`nr_pdsch_config_sweep_feedback(&job.sweep_ticket, ...)` (line ~1098, the TB-CRC scoring that drives
"Technique D CONVERGED") run on the SAME `job.sweep_ticket` for the SAME grant. Bumping generation
in `observe()` would make that job's OWN subsequent `feedback()` call fail every time its own mask
observation happened to narrow the catalog — which is common on a context's very first few grants,
exactly when the mask is first learned. That is not a rare edge case; it is a real hit to Technique
D's convergence rate on the primary DM-RS-mask path. (The file already has a documented, DELIBERATE
workaround for the structurally identical qm-oracle case: `nr_pdsch_passive_queue.c`'s own comment
at ~1103-1107 explains why `feedback()` is called BEFORE `observe_qm()` for the same job — and
explicitly flags the DM-RS-mask site as "a separate, earlier tap ... out of scope" for that
fix, i.e. this exact hazard was already known and deliberately deferred, not undiscovered.)

Reverted the `observe()` bump; added a comment there recording why (so it isn't reintroduced blind)
and a passing regression test (`PdschConfigSweepPerRnti.MaskObservationDoesNotBumpTheContextGeneration`)
asserting a fresh `select()` on the same key sees the SAME (unbumped) generation after a real
narrowing observation. I then verified the qm-oracle path (`observe_qm()`) is actually SAFE for the
identical bump — queue.c's own ordering means feedback() for a given job always runs BEFORE that
job's own `observe_qm()`, so there is no same-ticket same-job hazard to break there, only the
cross-ticket one the fix is meant to catch — built it, ran the full suite (all `PdschConfigSweepQm.*`
tests plus a new one green), and was about to commit it as fix 7 when the coordinator reported that
lane `perf` had already landed a superset fix for the identical bug class in the same file
(`fe96d0c4b6` on `sdd/gap-perf`, retiring tickets on prunes and probation restores with red-first
tests) and instructed dropping mine to avoid a merge conflict. **Reverted `nr_pdsch_config_sweep.c`
and `nr_pdsch_config_sweep_test.cc` to HEAD (`git checkout --`) and rebuilt/re-ran the full suite to
confirm the working tree is clean and green** (see "Final verification" below) — nothing from this
investigation is committed on `sdd/gap-misc`.

## Tests run + counts

- `test_nr_pdsch_prb_set` (`--gtest_shuffle`): **17/17 pass** (14 pre-existing + 3 new
  `PrbSet.PassiveAllocNormalise*`).
- `test_nr_pdcch_blind_monitor --gtest_filter='Dci11Pin.*'` (`--gtest_shuffle`): **10/10 pass**
  (9 pre-existing + 1 new `IsValidAccessorTracksFieldThroughSeedAndSelectTransitions`).
- `test_nr_pdcch_blind_monitor` full suite (`--gtest_shuffle`): **155/157 pass, 2 skipped**
  (`PdcchReplay.BudgetTimingEveryWidth`, `PdcchReplay.OtaCss0DecoderContract` — both pre-existing,
  env-gated OTA/replay tests unrelated to this lane).
- `PHY_NR_UE` (the full heavy static library `nr_pdsch_passive_decode.c` links into): built clean
  from scratch, confirming the item-5 header/library move doesn't break the real executable's
  dependency graph. Only pre-existing warnings (`dmrs_first` maybe-uninitialized at a line untouched
  by this lane's edits, an unused variable, a zero-length format string) — no new warnings, no
  errors.
- `test_nr_pdsch_config_sweep` (dropped-fix investigation only, not part of the committed work):
  42 tests, 1 real failure found and root-caused (see above), fix reverted, working tree confirmed
  clean via `git status --short` + a rebuild.
- No live/OTA runs this lane — none of the 6 committed fixes required one; item 3's watchdog was
  deliberately validated offline only (see its own section above).

## Final verification (post-revert-of-item-7, on the actual committed tree)

```
$ git status --short        # empty
$ git log --oneline -7      # the 6 commits above on top of c295fa18f1
```
Rebuilt `test_nr_pdcch_blind_monitor` and `test_nr_pdsch_prb_set` from this exact tree
(`lane-make.sh gap-misc test_nr_pdcch_blind_monitor test_nr_pdsch_prb_set`) — clean build, then
re-ran both suites: identical 17/17 and 155/157(+2 skipped) results as above.

## Open concerns

- Item 3's watchdog is unvalidated live (see its own note above) — the arithmetic and process-group
  kill logic were reviewed carefully (PGID looked up dynamically, not assumed `== $$`) but the
  mechanism itself has not been exercised end to end.
- Item 5's DM-RS-mask-prune ticket-staleness bug is real and NOT fixed by anything in this lane's
  commits (superseded by `fe96d0c4b6` on `sdd/gap-perf`, per the coordinator) — do not treat
  `sdd/gap-misc` as having addressed it.
- The brief's line-number citations for item 4 were stale (off by ~19 lines) and its assumed count
  (4 occurrences) was off by one (3 actual) — noted in case whoever wrote the brief wants to know
  their source snapshot predates several other lanes' edits to that file.

## Current test-only follow-up (2026-09-27)

The current `sdd/gap-misc` working tree is at HEAD `8852f768cffd9579e0d5da73f8963441368b504e`
with exactly two modified test files and no production-file changes. The measured unstaged diff is
10 insertions / 2 deletions, passes `git diff --check`, and has SHA-256
`6676edb5d2941bf1f83a8b410b57e2c301f2ce2e28bcffda1fa0a0294ad0f58c`.

1. `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc` changes the CSS0 test PCI
   from 2 to valid, test-unique 997. This forces the intended configuration-change path when the
   test is shuffled; it does not alter the asserted behavior or production code. File SHA-256:
   `a75bfa52c70f7527d4263697e111f95d01cde894111cdf83301f0e176d284962`.
2. `openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_ra_procedures.cpp` adds three ABI-compatible link
   stubs for the new test binary dependencies. Independent review verified that they do not mask
   `init_RA` behavior. File SHA-256:
   `9e18ab334e456fcd0666d6560bdb8fb91cfa9e03dd160e7e83746875589d49b8`.

Evidence: the RA test link failure was reproduced before the stub change, then the target built and
its ctest and shuffled runs passed (1/1 and 2/2; progress ledger 11:20). The CSS0 shuffle failure
was reproduced at seed 928 (3 pass / 2 fail) because the test reused an already-applied derivation;
after the unique-PCI fixture change, blind-monitor shuffle seeds 1, 3, 5, 927, and 928 each passed
155 tests with the same two known skips. Seed 928's config suite passed 40 tests with one known
skip; PRB-set, scrambling, and RA counts were 17, 11, and 2. This is test-fixture RED→GREEN
evidence, not a production behavior change. The independent G5 review at 12:08 approved both
changes, verified PCI 997 is valid and assertions are unchanged, and verified the stubs do not
mask `init_RA` behavior. These records are in `progress.md` (11:20, 11:57, 12:00, 12:08).

G4 is **N/A for these two test-only hunks only**: the live UE binary does not include or execute
test fixture code or test-link stubs, so a radio arm cannot exercise these changes. This ruling
does not waive live validation for the earlier misc runtime and harness commits (stats-file path,
false uplink verdict, watchdog, acquire accessor, and allocation-normalisation move); assess those
separately against the handover's G4 wording before any commit/merge. No commit is authorized by
this scoped G4 ruling.
