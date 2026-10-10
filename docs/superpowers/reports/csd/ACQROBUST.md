# ACQROBUST: adaptive verification dwell, no fake footprint, deterministic GPU discovery input

Host: DGX aarch64 / GB10. Branch `csd/acq-robust` (base `csd/otatest` bfba6aebb3, incl. ACQ-FIX). Date 2026-10-10.
Offline only; every build, test and replay ran through `heavy.sh`. All three flags default off; flag off is unchanged behaviour.

## Flags

1. `ISAC_PDCCH_DWELL_ADAPTIVE=1` (`nr_pdcch_blind_monitor.c`, `nr_pdcch_discovered_step_ex()`). An occasion counts toward
   `NR_PDCCH_EXTENT_VERIFY_OCC` (4000) only if a candidate of that dwell's pass cleared the prescreen (energy gate, known-prescreen,
   DM-RS gate in `run_occasion_one()`); every occasion counts toward the hard cap `NR_PDCCH_DWELL_ADAPTIVE_CAP` (40000).
   The signal travels as an explicit parameter: `t_discovered_prescreen_passed` is set in the candidate loop, reset in
   `run_discovered_occasion()`, and passed to `nr_pdcch_discovered_step_ex(token, slot, prescreen_passed)` for the same occasion.
   `nr_pdcch_discovered_step()` stays as a wrapper (prescreen_passed=true).
2. `ISAC_PDCCH_NO_FAKE_FOOTPRINT=1` (`techA_commit_apply()`): the root period/offset/duration override 1/0/1 is skipped while
   `nr_pdcch_discovered_unverified_pending()` (queue enabled and an active dwell or queued entry). The footprint, `gate_publish()` and
   everything else are unchanged. The discovered dwell's own config in `nr_pdcch_discovered_snapshot()` is untouched.
3. `ISAC_PDCCH_NID_GPU_NODROP=1` (`nr_pdcch_nid_gpu.c`): (a) `nodrop_acquire()`: trylock polling plus `pthread_cond_timedwait` on a new
   `cv_free` (CLOCK_MONOTONIC via `pthread_condattr_setclock`, absolute CLOCK_MONOTONIC deadline), total wait <= 500 us; the worker
   signals `cv_free` whenever it frees a batch (`batch_freed()`); (b) whole-snapshot refusal keyed on the pushed slot, counters
   `dropped_snapshots` (first symbol refused) and `dropped_partial` (a later symbol refused, i.e. unavoidable partial), logged in the
   `PDCCH_NID_GPU pass` line and at stop, getter `nr_pdcch_nid_gpu_dropped_snapshots()`; (c) duty clock = 500 us per pushed-slot change
   (delta modulo 160), stamped on each batch when queued, so the worker's duty decision depends only on accepted input, not on worker timing.
   The flag is read in `nr_pdcch_nid_gpu_start()`.

## Tests [MEASURED, DGX]

ctest `test_nr_pdcch_{blind_monitor,nid_worker (+_duty,_incremental,_duration2/3,_nodrop),coreset_map,coreset_bank,scan_fast}`: 10/10 pass.
- `DiscoveredRobust.FlagOffRejectsAt4000RegardlessOfPrescreen`, `AdaptiveNoPrescreenSurvivesUntilCap` (alive at 39999, rejected at 40000),
  `AdaptivePrescreenWithoutCrcRejectsAt4000`, `AdaptiveCountsOnlyPrescreenedOccasions`,
  `NoFakeFootprintKeepsRootPeriodWhilePending` (end-to-end through autodiscover: off+pending -> 1/0/1; on+pending -> period kept; on+none pending -> 1/0/1).
- `test_nr_pdcch_nid_worker --nodrop` (fake GPU): bounded wait 0.3-20 ms, one snapshot refused as ONE count (not 14), second snapshot counted,
  recovery after the worker frees a batch, no data errors.

## Replays [MEASURED, DGX, 0.25x, t26h env minus radio flags + all five flags, no seed]

Command: `tests/passive_rx/acqfix_replay.sh <name> ISAC_PDCCH_DISC_COVER=1 ISAC_PDCCH_RBOFFSET_FIX=1 ISAC_PDCCH_DWELL_ADAPTIVE=1 ISAC_PDCCH_NO_FAKE_FOOTPRINT=1 ISAC_PDCCH_NID_GPU_NODROP=1`
(old capture: `CAP=.../raw_pci415_12s_20261008 CAP_TICK=210036277`). Logs `replays/replay_rob_*` (gitignored). Time = frame count from PBCH lock (10 ms/frame).

| capture | run | first VERIFIED bank add | notes |
|---|---|---:|---|
| b | 1-4 | 1.10, 1.07, 1.09, 1.12 s | nid 62922 (or 10163), no "recurrent oracle" |
| old 20261008 | 1-4 | 1.53, 1.50, 2.20, 1.51 s | main RNTIs 8f5a/b876/e01e/13ae all decoded in 4/4 runs |

Gate: b 4/4 <= 1.5 s (ACQ-FIX 10/10; only 4 runs requested here), old 4/4 main RNTIs: met, no regression. Zero "recurrent oracle committed" lines.
Capture b ran 4 runs, not 10.

NODROP determinism: `dropped_snapshots=0 dropped_partial=0` in all 8 runs (0.25x replay has no pressure), so the drop paths were not exercised in replay.
The first published geometry is NOT identical across runs of the same capture (old: 0/36 in runs 1,2,4; 169/2 L=2 in run 3; b: varying set).
Cause found: the replay start slot differs per run (first abs_slot 1340/1420/1460/1380 on the old capture), i.e. the pushed input set differs
because the raw-IQ start position differs, not because of drops. NODROP removes only the drop and wall-clock sources; it cannot make a live or
jittered-start input identical. Even run 3 (169/2 first) still ended with 4/4 main RNTIs via the cover.
Adaptive dwell in replay: the replay cfg has no energy gate, so every occasion passes the prescreen and behaves like 4000; the low-traffic case
is covered only by the unit tests (not simulated at replay level). Observed rejections (nid 9020) show the new log text at 4000/4000.

## OTA flags

Append to the t26j env: `ISAC_PDCCH_DWELL_ADAPTIVE=1 ISAC_PDCCH_NO_FAKE_FOOTPRINT=1 ISAC_PDCCH_NID_GPU_NODROP=1`
(with `ISAC_PDCCH_DISC_COVER=1 ISAC_PDCCH_RBOFFSET_FIX=1` already there). For DWELL_ADAPTIVE to matter the OTA run needs the noise/energy gate
configured (otherwise every occasion counts). Look for: `rejected (no fresh CRC evidence: N prescreen-passing occasions, M total, ISAC_PDCCH_DWELL_ADAPTIVE)`,
`dropped_snapshots=` in the pass lines, and SCANPASS/bank counters surviving after any Technique A commit.

## Self-check (before commit)

- New helpers all called: `nr_pdcch_dwell_adaptive_enabled` (step_ex), `nr_pdcch_discovered_step_ex` (run_discovered_occasion), `nr_pdcch_no_fake_footprint_enabled` and
  `nr_pdcch_discovered_unverified_pending` (techA_commit_apply), `duty_clock_us` (worker x2), `slot_clock_us` (push, feedback), `batch_freed` (worker x2, feedback),
  `nodrop_acquire` (push), `nr_pdcch_nid_gpu_dropped_snapshots` (stop, tests).
- Flags all read: DWELL_ADAPTIVE, NO_FAKE_FOOTPRINT (getenv per call), NID_GPU_NODROP (start). Constants used: NR_PDCCH_DWELL_ADAPTIVE_CAP, NODROP_WAIT_NS, NODROP_SLOT_US.
- Thread-local `t_discovered_prescreen_passed` declared (static _Thread_local, same file) before use; no use before declaration (build clean).
- Flag-off hunks: step_ex flag-off branch is the original code and log; techA override identical when flag off; push/worker flag-off branches are the original statements
  (`batch_freed`/`duty_clock_us` are no-ops/wall clock when off; the pass log line is the original). No blocking lock on the real-time path (trylock polling only, timedwait <= 500 us CLOCK_MONOTONIC on a MONOTONIC condvar).
- Known limit: `nr_pdcch_discovered_unverified_pending()` is a racy read of the queue counters (benign, decision at most one slot late).
