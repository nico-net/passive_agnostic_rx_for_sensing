# R6b — independent R3–R6 review fixes

[IMPLEMENTED, NOT VALIDATED on live traffic] Host: DGX aarch64 (`spark-74c3`), branch `rr/reconfig-robustness`, baseline `6bc2dda583`. Scope: the review of `de03ab9977`, `37afdfac4c`, `4cdcdea644`, `084bd85f90`, and `6bc2dda583`. The orchestration brief and authoritative `SPEC_CURRENT.md` govern this task; no spec/plan or other worktree was changed.

## Findings and decisions

1. **Fixed — mixed bank clocks.** [IMPLEMENTED, NOT VALIDATED on live traffic] `nr_pdcch_blind_monitor_rt.c` now saves `source_absolute_slot` in the dispatcher's thread-local `t_bank_slot`. Tick, unique accepts, and both DL/UL DCI proof calls use that clock. Previously tick could seed `last_accept_slot` above every wrapped SFN-based acceptance slot. [OFFLINE VERIFIED] `AcceptEveryOccasionAcrossSfnWrapStaysVerified` covers sustained accepts across the 20,480-slot (10.24 s at mu=1) boundary and another 10 s, with elsewhere traffic asserted. This bank-level fixture already passed before the wiring fix: the defect is in RT call sites, which the focused binaries do not link. The call sites were inspected and the full receiver rebuilt; no end-to-end wrap validation is claimed.
2. **Fixed — idle-resume SUSPECT.** [OFFLINE VERIFIED] `note_occasion()` returns before incrementing misses when the RNTI has no activity elsewhere. A locked-length accept still clears misses. `IdleThenUlResumeStaysLocked` failed before the fix and now passes; `InactiveRntiNeverSuspect` now also requires zero idle misses, and `LockedToSuspectAfterNMisses` remains green.
3. **Fixed — false RELOCK; redundant pin reset removed.** [OFFLINE VERIFIED] Same-length `context_lock()` returns zero and preserves its pin/cursor; both RT callers additionally require `old_len > 0 && old_len != found`. A changed length still reports its predecessor. `RelockSameLengthReturnsLocked` and `RelockPreservesSamePinAndClearsReplacedPin` cover the transition signal and pin behavior. [IMPLEMENTED, NOT VALIDATED on live traffic] The same-length path therefore skips the RELOCK log and cell-wide Technique D reset. **Pin subfinding: not a bug in the active pin's invalidation**: R4's `context_lock()` already clears the replaced primary context pin, and `context_add()` clears the replaced slot's pin under the length lock. The callback's extra reset targeted the unused legacy global pin; it was removed rather than adding another reset.
4. **Fixed — RECONF=0 bootstrap mutation.** [IMPLEMENTED, NOT VALIDATED on live traffic] The additional confirmed CORESET#0-USS C-RNTI branch now requires the shared `reconf_lengths_enabled()` helper. Source comparison with `83afec4df8` establishes parity for this branch: unset/0 makes the new disjunct false, leaving exactly `ss_bucket == 0` for `record_trusted`, followed by the unchanged dedicated-geometry `record` branch. Short-circuiting also avoids the added confirmation lookup. No claim of whole-receiver bit-identical execution is made.
5. **Fixed — fallback exclusion and duplicate length.** [OFFLINE VERIFIED] The sweep has a third exclusion, set to `dci10_length` during a DL second-length search. It is honored by preferred ordering, trial selection, winner selection, and the measured null. The original single-exclusion trial behavior remains unchanged. `SecondLengthExcludesFallback` verifies a strong 1_0 candidate cannot lock, while another dedicated length can. Relocking to the existing secondary moves that length's pin/cursor/recency to the primary and clears the secondary, preventing duplicate decode tasks (`RelockToSecondLengthDeduplicatesAndKeepsItsPin`). **Other-UE winner subfinding: not a bug, skipped.** [IMPLEMENTED, NOT VALIDATED on live traffic] The caller sets `rnti_min == rnti_max == bootstrap_rnti`; both GPU cache admission paths check that exact range, and the CPU scorer calls `nr_pdcch_blind_decode_raw_11()` with it, including alternate unscrambling. That helper's raw decoder rejects CRC masks outside the range. Thus another UE cannot supply this search's evidence. Adding `winner_rnti == bootstrap_rnti` would also unnecessarily replace the existing two-bootstrap-hit gate with the winner accessor's five-distinct-occasion requirement. The initial combined test was narrowed to the confirmed exclusion defect after this inspection.
6. **Fixed — single occupancy hit demotion.** [OFFLINE VERIFIED] A receive-thread-owned eight-sample history requires hits in at least three discovery samples, including the current sample. Multiple windows in one sample count once. Single-hit and repeated-hit/expiry bank fixtures failed with the original immediate-hit behavior and now pass. [IMPLEMENTED, NOT VALIDATED on live traffic] The existing atomic handoff publishes only qualified occupancy; known CORESET#0 windows remain excluded. Recurrent non-CORESET#0 common traffic is still not distinguishable from dedicated traffic using occupancy alone.
7. **Fixed — slot-phase starvation.** [OFFLINE VERIFIED] DL scouts/second-length searches and UL second-length searches now use per-geometry occasion counters under their existing length locks. `ScoutDutyUsesOccasionsNotSlotPhase` checks five searches over 100 occasions whose slots all have remainder one modulo 20. [IMPLEMENTED, NOT VALIDATED on live traffic] All three RT `abs_slot % 20` gates were replaced by the counter decision.
8. **Not a bug — pre-bank discovery duty, unchanged.** [OFFLINE VERIFIED] The existing duty helper returns true immediately when `bank_count == 0`, without advancing the counter. `ReconfBeforeFirstBankKeepsFullDiscoveryDuty` explicitly checks 100 unpaused, pre-bank calls. Throttling an unpaused walk *after* a bank exists agrees with spec §4.3's “after the first entry is banked” rule. Existing paused/unpaused and flag-off scheduler tests remain green.
9. **Fixed — lock-order documentation.** [IMPLEMENTED, NOT VALIDATED by a new concurrency measurement] The length-lock declarations now document `dl -> bank`, `dl -> bootstrap` (also `ul -> bootstrap`), and the sole reverse bank-to-length path: the remove hook after all dispatcher leases have drained.

[IMPLEMENTED, NOT VALIDATED on live traffic] Production files changed: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor{,_rt}.c`, `nr_pdcch_dci_length_sweep.{c,h}`, and `nr_pdcch_coreset_bank.{c,h}`. Regression fixtures are in that directory's `tests/nr_pdcch_{blind_monitor,dci_length_sweep,coreset_bank}_test.cc`.

## Validation — DGX aarch64

[OFFLINE VERIFIED] No `nr-uesoftmodem` process was present before builds/test groups. Builds used `CCACHE_DIR=/tmp/rr-r6b-ccache`, nice 19, at most eight jobs; ctest used four. `ISAC_RX_BRANCH_FO` was not enabled. No receiver, rfsim bed, simulator campaign, OTA, or GPU workload was launched.

[OFFLINE VERIFIED] Test-first evidence:

```bash
CCACHE_DIR=/tmp/rr-r6b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_pdcch_dci_length_sweep test_nr_pdcch_coreset_bank
nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_dci_length_sweep
nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_coreset_bank
```

[OFFLINE VERIFIED] Initial compile failed on the missing third exclusion, scout counter API, and occupancy filter API (`/tmp/rr-r6b-red-build.log`). With declarations and an immediate-hit adapter retaining the old occupancy behavior, runtime red was 37/43 length tests passing (six failed) and 10/12 bank tests passing (two failed): `/tmp/rr-r6b-red-runtime-build.log`, `/tmp/rr-r6b-red-length.log`, `/tmp/rr-r6b-red-bank.log`. The adapter was replaced by the actual filter. Clock wiring and flag-off parity were source-reviewed, not represented as failing unit tests.

[OFFLINE VERIFIED] Focused build and final length/bank results: 43/43 and 12/12 pass (`/tmp/rr-r6b-target-build-final.log`, `/tmp/rr-r6b-length-final.log`, `/tmp/rr-r6b-bank.log`). The focused build also included `test_nr_pdcch_blind_monitor`.

```bash
CCACHE_DIR=/tmp/rr-r6b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
for seed in 1 3 5; do
  for target in test_nr_pdcch_dci_length_sweep test_nr_pdcch_coreset_bank test_nr_pdcch_blind_monitor; do
    nice -n 19 cmake_targets/ran_build/build/$target --gtest_shuffle --gtest_random_seed=$seed
  done
done
```

[OFFLINE VERIFIED] Required target-list build passed all eight incremental steps (`/tmp/rr-r6b-full-build.log`). Exactly one full ctest run passed 122/131 in 27.51 s (`/tmp/rr-r6b-ctest.log`). All nine failures are on the brief's known list: ARM `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`; sandbox `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. No new failure appeared.

[OFFLINE VERIFIED] Shuffle seeds 1/3/5 each passed: length 43/43; bank 12/12; blind monitor 208/210 with the two existing capture-dependent skips (`PdcchReplay.OtaCss0DecoderContract`, `PdcchReplay.BudgetTimingEveryWidth`). All nine runs exited zero. Logs: `/tmp/rr-r6b-test_nr_pdcch_{dci_length_sweep,coreset_bank,blind_monitor}-shuffle-{1,3,5}.log`.

[OFFLINE VERIFIED] `git diff --check` and the frozen gate passed:

```bash
git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30
```

## DEFERRED MEASUREMENTS

[PLANNED] Live validation is deferred by `CODEX_BRIEF.md`'s shared-machine restriction. The orchestrator can run these exact stable-cell flag-off/on comparisons, including a SIB1-less arm, in a quiet DGX slot:

```bash
cd /home/nicola/NICOLA/wt/rr-robust
if pgrep -x nr-uesoftmodem; then exit 2; fi
for reconf in 0 1; do
  for ignore_sib1 in 0 1; do
    env -u ISAC_RX_BRANCH_FO -u NR_GPU_FEP ISAC_RECONF=$reconf ISAC_TD_IGNORE_SIB1=$ignore_sib1 NR_GPU_POLAR=0 SCANTHREAD=2:16:6 BUILD="$PWD/cmake_targets/ran_build/build" GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r6b-reconf-$reconf-ignore-sib1-$ignore_sib1 bash tests/passive_rx/dgx/rfsim_regress.sh 2
  done
done
```

[PARTIAL] These commands test stable operation and queue-drop/CRC regression, not injected reconfiguration recovery. The SIB1-ignore flag does not itself remove CORESET#0 or make the entire cell SIB1-absent. A genuine SIB1-absent replay, injected length/CORESET changes, 60-minute soak, and live wrap/idle-resume assertions remain orchestrator/replay-fixture work. No runnable injection fixture was invented here.

[PARTIAL] A genuine changed-length relock/removal still uses the existing cell-wide Technique D reset API. Narrowing that API is outside R6b; this task removes the false same-length trigger.
