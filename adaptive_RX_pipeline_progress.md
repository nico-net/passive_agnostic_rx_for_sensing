# Adaptive RX pipeline — progress and evidence ledger

Created: **2026-09-10**, Europe/Zurich.
Plan: [adaptive_RX_pipeline.md](adaptive_RX_pipeline.md).
Canonical location: `sens6:/home/sens/NICOLA/adaptive-rx-sensing/adaptive_RX_pipeline_progress.md`.

## Current status

**Documentation and read-only inventory complete; P01 (baseline manifest / acceptance profile) IN_PROGRESS as of 2026-09-10 (see session below); P02 (valid DL/UL replay fixtures) PASSED as of 2026-09-11 after two controller-ruled fix rounds to the passive-replay recorder (see P02 session + Fix round 1/2 below) -- the first implementation task under this plan to reach PASS, and the first receiver source changes made under this plan (two small, ruling-scoped gate fixes in `nr_passive_replay_capture.c`/`nr_pdcch_blind_monitor_rt.c`). G0 remains IN_PROGRESS overall, blocked only on P01's outstanding survey/acceptance-limit work. P03 (branch abstraction) IN_PROGRESS as of 2026-09-11: foundation delivered (`docs/passive_branch_globals_audit.md`, `nr_rx_branch.{h,c}`, `[sensing] rx_branches`/`rx_branch_phys_map`, 14/14 gtest) but deliberately not wired into the RT read loop (controller-scoped to P04/P05). P04 (immutable buffer delivery) IN_PROGRESS as of 2026-09-11: standalone `nr_rx_span_pool.{h,c}` delivered (refcounted per-branch spans, no sample copies, per-branch drop policy), 5/5 gtest, also deliberately not wired into the RT read loop (same controller ruling -- `nr-ue.c` is dirty with another session's edits) -- G1 remains NOT_STARTED.**

| Baseline fact | Value |
|---|---|
| Host / tree | `sens6:/home/sens/NICOLA/adaptive-rx-sensing` |
| Branch | `merge/adaptive-sensing` |
| Inspected commit | `50c8312fa46f17850aeee579abfb0a745f8465ff` (unchanged as of the P01 session) |
| Existing modified/untracked files (2026-09-10 plan baseline) | `M tests/passive_rx/run_adaptive_receive_test.sh`, `?? tests/passive_rx/aoa_track_dl.conf` |
| **Measured dirty state at P01 session time (2026-09-10, differs from the row above -- see P01 session notes)** | `git status --short` additionally shows `M executables/nr-ue-ru.c`, `M executables/nr-ue.c`, `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`, `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`, `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`, and untracked `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`, `tests/passive_rx/run_manual_x410.sh`, beyond the two rows above. None of it was reset/clean/stashed/touched by P01; recorded as a measured fact only (full list in the P01 manifest's `git.status_short`) |
| Build cache | Sensing ON, tests ON, CUDA OFF, RelWithDebInfo (re-measured 2026-09-10 P01 session: unchanged) |
| Newly written files | `adaptive_RX_pipeline.md`, `adaptive_RX_pipeline_progress.md`, plus P01's `tests/passive_rx/baseline_manifest.sh`, `tests/passive_rx/check_manifest.py`, `tests/passive_rx/acceptance_four_rx.json`, `tests/passive_rx/geometry_four_rx.json`, `tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json` (+ sibling `_tracked.patch`; superseded the review-round-1 regeneration of the original `manifest_20260910_50c8312-dirty.json` -- see "Fix round 1" addendum below) |
| Actual new pipeline code | None |
| Compilation / receiver tests / OTA | Not run |
| Separated-antenna survey | Not provided or verified (unchanged; `tests/passive_rx/geometry_four_rx.json` now formalizes this as `surveyed:false`) |
| Acceptance profile | v1 created 2026-09-10, NOT frozen except for the plan's exact invariants; every deployment-dependent limit UNSET by construction (`tests/passive_rx/acceptance_four_rx.json`) |
| Four-branch launcher | Proposed; not implemented |
| Optional UL localization | Specification only; default-off feature not implemented |

## Task checklist and accomplishment dates

Dates use `YYYY-MM-DD` in Europe/Zurich. Check an implementation item only when its corresponding evidence gate passes. Source presence is not an accomplishment under this new plan. Use `—` until complete. Keep the plan's initial checklist synchronized when statuses change.

| Check | Task | Required result | Status | Accomplished date | Evidence |
|---|---|---|---|---|---|
| [x] | D01 | Code/config/branch inventory | PASS | 2026-09-10 | Session 2026-09-10; plan section 2 |
| [x] | D02 | Detailed plan and progress ledger | PASS | 2026-09-10 | These two documents |
| [ ] | P01 | Baseline manifest / acceptance profile | IN_PROGRESS | — | Session 2026-09-10 (P01) below + "Fix round 1" addendum; current manifest `tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json` (commit `e4c8cadd5f`), `acceptance_four_rx.json`, `geometry_four_rx.json`. NOT checked/PASS: geometry unsurveyed and every deployment-dependent acceptance limit UNSET are BLOCKED sub-items |
| [x] | P02 | Valid DL/UL replay fixtures | PASS | 2026-09-11 | Session 2026-09-11 (P02) + Fix round 1 + Fix round 2 below. Audit found all 22 pre-existing `replay.bin` captures INADMISSIBLE (binary mismatch + full_auto=1). Two structural code defects (both coupling the passive-replay recorder's arm condition to full-auto-only paths) fixed under controller ruling: DL gate in `nr_passive_replay_capture.c`'s `nr_passive_replay_dl()` (round 1) and UL gate in `nr_pdcch_blind_monitor_rt.c`'s UL candidate loop (round 2, root-caused via live instrumentation). With both fixes, capture `sensing_manual_fixed.UtvBT7` (binary `c3810019...`) produced `REPLAY READY ... slots=320 UL=24 DL-controls=38` and replay-verified `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened` (exit 0). Registered in `tests/passive_rx/baselines/fixtures.json` (`status:"PASS"`, 1 admissible fixture); `check_manifest.py --fixtures` PASSES; negative selftest cases still correctly FAIL |
| [ ] | P03 | Branch ownership / lifecycle | IN_PROGRESS | — | Session 2026-09-11 (P03) below. Foundation delivered: `docs/passive_branch_globals_audit.md` (globals audit), `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch.{h,c}` (pure branch identity/epoch/lifecycle, 14/14 gtest), `[sensing] rx_branches`/`rx_branch_phys_map` config surface in `nr_isac.cc`/`.h`. NOT done: nothing wired into the `nr-ue.c` RT read loop (deliberately out of scope, see P03's controller ruling) and the `nb_antennas_rx` antenna-count cross-check is unwired (setter added, no live caller — see audit doc's "nb_antennas_rx reachability" section) — so G1 is not testable end-to-end from this task alone |
| [ ] | P04 | Immutable channel-buffer delivery | IN_PROGRESS | — | Session 2026-09-11 (P04) below. Standalone module delivered: `openair1/PHY/NR_UE_TRANSPORT/nr_rx_span_pool.{h,c}` (refcounted per-branch span pool, no sample copies, per-branch drop policy), 5/5 gtest (G1 test 1 and test 3 in pure form, plus refcount and epoch-carry cases). NOT done: nothing wired into the nr-ue.c RT read loop (deliberately out of scope, see P04's controller ruling -- that file is dirty with another session's uncommitted edits) -- so G1 stays NOT_STARTED end-to-end. |
| [ ] | P05 | Independent acquisition / recovery | NOT_STARTED | — | — |
| [ ] | P06 | Branch-local PDCCH discovery / grants | NOT_STARTED | — | — |
| [ ] | P07 | Independent DL decoding | NOT_STARTED | — | — |
| [ ] | P08 | Independent UL decoding / context pool | NOT_STARTED | — | — |
| [ ] | P09 | Namespace / TLS / concurrency audit | NOT_STARTED | — | — |
| [ ] | P10 | CFR ABI and producer migration | NOT_STARTED | — | — |
| [ ] | P11 | Support / allocation provenance | NOT_STARTED | — | — |
| [ ] | P12 | Physical time / reference contract | NOT_STARTED | — | — |
| [ ] | P13 | Four independent sensing engines | NOT_STARTED | — | — |
| [ ] | P14 | AoA removal / migration | NOT_STARTED | — | — |
| [ ] | P15 | UL scene-support preservation | NOT_STARTED | — | — |
| [ ] | P16 | Reports / consumers | NOT_STARTED | — | — |
| [ ] | P17 | DL geometry / Jacobians / rank | NOT_STARTED | — | — |
| [ ] | P18 | DL association / global estimator | NOT_STARTED | — | — |
| [ ] | P19 | DL admission / OTA comparison | NOT_STARTED | — | — |
| [ ] | P20 | Launcher / manifests | NOT_STARTED | — | — |
| [ ] | P21 | Sustained throughput / isolation | NOT_STARTED | — | — |
| [ ] | P22 | Required pipeline release evidence | NOT_STARTED | — | — |
| [ ] | U00 | Optional feature-off compatibility | NOT_STARTED | — | — |
| [ ] | U01 | Four-channel UL acquisition | NOT_STARTED | — | — |
| [ ] | U02 | Independent UL detections / metadata | NOT_STARTED | — | — |
| [ ] | U03 | DTD/DFS / pre-solver OTA logs | NOT_STARTED | — | — |
| [ ] | U04 | Same-transmission association | NOT_STARTED | — | — |
| [ ] | U05 | Model / actual-geometry observability | NOT_STARTED | — | — |
| [ ] | U06 | WNLS / sliding-window solver | NOT_STARTED | — | — |
| [ ] | U07 | RX-count / measurement-mode comparison | NOT_STARTED | — | — |
| [ ] | U08 | Motion / multipath robustness | NOT_STARTED | — | — |
| [ ] | U09 | Optional deliverables / limitations | NOT_STARTED | — | — |

## Gate summary

| Gate | Meaning | Status | Passed date | Artifact / reviewer |
|---|---|---|---|---|
| G0 | Reproducible baseline and acceptance profile | IN_PROGRESS | — | P02's own sub-condition now MET (2026-09-11, Fix round 2): a reproducible supported DL+UL replay fixture exists (`sensing_manual_fixed.UtvBT7`, `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened`), registered and checker-verified. G0 as a WHOLE still cannot PASS, because P01's own manifest remains IN_PROGRESS on survey/limits (every `deployment_dependent_limits` entry UNSET, `geometry.surveyed=false` -- unchanged by P02, not this task's scope). State plainly: G0 = IN_PROGRESS, blocked only on P01's outstanding survey/acceptance-limit work, not on P02 any longer |
| G1 | Acquisition routing, time and branch isolation | NOT_STARTED | — | — |
| G2 | Independent supported DL/UL decoding | NOT_STARTED | — | — |
| G3 | CFR identity, support and physical references | NOT_STARTED | — | — |
| G4 | Four detectors, AoA removed, UL support retained | NOT_STARTED | — | — |
| G5 | Observable AoA-free DL global fusion | NOT_STARTED | — | — |
| G6 | Sustained operation and required OTA release | NOT_STARTED | — | — |
| U00 | Optional UL disabled-mode compatibility | NOT_STARTED | — | — |
| UG1 | Four-channel UL acquisition | NOT_STARTED | — | — |
| UG2 | Independent UL target detections | NOT_STARTED | — | — |
| UG3 | OTA direct-path-referenced DTD/DFS | NOT_STARTED | — | — |
| UG4 | Same-UE/same-transmission association | NOT_STARTED | — | — |
| UG5 | Actual receiver geometry, rank and conditioning | NOT_STARTED | — | — |
| UG6 | Observable-mode localization and comparisons | NOT_STARTED | — | — |
| UG7 | Motion, multipath and dropout robustness | NOT_STARTED | — | — |

## Session — 2026-09-10: planning and inspection

**Scope:** read the current sens6 source and configuration, write the implementation specification and initialize the progress ledger. No receiver implementation was requested/executed in this documentation session.

**Files written:**

- `adaptive_RX_pipeline.md`: detailed stages, tests/gates, present/missing blocks, retractions/removals, verified-versus-proposed commands, debugging runbook and optional AoA-free UL phase.
- `adaptive_RX_pipeline_progress.md`: dated task/gate tables and evidence templates.

**Commands/actions performed:** read-only SSH `git branch`, `git rev-parse`, `git status`, `find`, `sed`, `grep` and source/config file listings. Read the publisher's abstract/introduction for DOI `10.1016/j.sigpro.2025.110265`. Created only the two Markdown documents and copied them to the canonical worktree. Documentation checks and copy hashes are recorded below when complete.

**Measured/observed facts:**

1. Branch is `merge/adaptive-sensing` at the recorded commit, with the two pre-existing file changes listed above.
2. Native sensing uses one process-wide engine. AoA-off requests one sensing channel, not four engines.
3. Passive decoding contains shared discovery/queue state and cross-branch diversity behavior.
4. The current global birth path requires AoA; no AoA-free four-RX global fusion is wired into it.
5. Current CFR ABI omits branch/emitter identity and hardware acquisition timestamp. Submission UTC is worker-time stamped.
6. The default adaptive config disables sensing, so the sensing-enabled build fails its launcher guard. The alternate AoA config's linear array does not pass the rank-two parser.
7. Optional UL direct/target path records and a four-RX DTD/DFS solver are absent from the inspected pipeline.

**Reasoned design conclusions, not OTA measurements:**

- Branch isolation and measurement contracts must precede fusion.
- Four unrestricted instantaneous 3-D DTD/DFS receiver pairs supply at most eight scalar measurements for twelve target-plus-moving-UE state unknowns; a full solution is underdetermined without additional information/time structure.
- A short constant-velocity window may improve observability, but rank/ambiguity must be established for the actual geometry and motion.

**Retractions:** see plan section 2.3. No previous hardware CRC numbers have been reused as new validation. The no-UCI reconstruction inference, pilot availability guarantee, AoA-off behavior and readiness of existing run configs were narrowed to what source actually establishes.

**Outcome:** D01/D02 complete. All P/U implementation tasks remain NOT_STARTED. No live validity/performance claims.

**Next action when implementation is authorized:** P01/P02—freeze the baseline manifest and acceptance profile and identify valid channel-specific DL/UL replay evidence; then implement branch ownership/routing. Repair the minimal test harness early if baseline acquisition needs it, without claiming four-branch support exists.

## Session — 2026-09-10: P01 baseline manifest and acceptance profile (IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-10 (sens6 host clock reads UTC 2026-09-10T23:xx -- see
  sens4/sens6 clock-skew note; the plan/ledger's own "2026-09-10" heading is consistent with this).
Task IDs / gate: P01 / G0 (Stage 0).
Intended falsifiable claim: a generated manifest's every recorded hash/identity matches the live
  tree/build; a manifest with one hash altered is detected as a mismatch; the shipped acceptance/
  geometry files are correctly rejected as incomplete (UNSET/unsurveyed); a manifest whose selected
  test conf has pdcch_blind_monitor_full_auto=1 is rejected (2026-09-11 TESTING MODE RULE amendment).
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing @
  50c8312fa46f17850aeee579abfb0a745f8465ff (unchanged from the plan's baseline). `git status --short`
  at generation time carried MORE dirty files than the plan's recorded two -- see "Current status"
  table above and the manifest's own `notes.tree_dirty_state_vs_plan_baseline` field. None were
  reset/clean/stashed/touched; the manifest generator only reads and records.
Files modified / added / removed: ADDED (this session): tests/passive_rx/baseline_manifest.sh,
  tests/passive_rx/check_manifest.py, tests/passive_rx/acceptance_four_rx.json,
  tests/passive_rx/geometry_four_rx.json, tests/passive_rx/baselines/manifest_20260910_50c8312-
  dirty.json, tests/passive_rx/baselines/manifest_20260910_50c8312-dirty_tracked.patch. MODIFIED:
  none. REMOVED: none. The two pre-existing dirty items (and every other dirty file found at session
  start) were left exactly as found.
Executable / driver / config / geometry / acceptance hashes:
  nr-uesoftmodem sha256 b064c9957bc1b68d717543eb2bac28c4b2616115bc675f5fed38685f008742fe
    (62803480 bytes, mtime 2026-09-10T23:09:00Z);
  liboai_usrpdevif.so sha256 ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583
    (3263992 bytes, mtime 2026-09-10T23:02:17Z); liboai_device.so -> liboai_usrpdevif.so;
  tracked-diff patch sha256 f078f2db56c2206386d891e3b9d3a21bca72576c91b718561297869d7ebc1368
    (787 lines, `git diff --binary` over ALL currently-dirty tracked files, not only the two the
    plan named);
  adaptive_manual_diag.conf sha256 86559555d95505b1d45c710c71bf5cbce97fde55e2e8cb2d423a0619332dba18;
  acceptance_four_rx.json sha256 57896964efde38903a8c4537dd09cc6d92e7c2f565a101bbf1778cbc42e6ad8b;
  geometry_four_rx.json sha256 f27963008c0d63279c25f3fdc3af846d43563b6fffa4af05181940b2532bcdb1;
  manifest itself sha256 f5ffa616687535cd5fe8e16f97bf8579b13dfa2618a0babf40d27ad67da170a2.
  Full per-file list (2004 scoped files, all 5 case confs, CMake cache, compiler/UHD versions):
  tests/passive_rx/baselines/manifest_20260910_50c8312-dirty.json.
Exact commands:
  bash tests/passive_rx/baseline_manifest.sh
  python3 tests/passive_rx/check_manifest.py tests/passive_rx/baselines/manifest_20260910_50c8312-dirty.json
  python3 tests/passive_rx/check_manifest.py --acceptance tests/passive_rx/acceptance_four_rx.json --geometry tests/passive_rx/geometry_four_rx.json
  python3 tests/passive_rx/check_manifest.py --selftest
  (No build, no radio, no launcher invocation. run_adaptive_receive_test.sh was read, never run.)
Artifact paths (include raw logs and VOID attempts): tests/passive_rx/baselines/manifest_20260910_
  50c8312-dirty.json (+ sibling _tracked.patch); full command transcripts in the P01 task report
  (/home/sens/NICOLA/.superpowers/sdd/adaptive_RX_pipeline/task-P01-report.md, local to the
  controlling session, not part of this repo).
Baseline and comparison definition: "live tree/build" = the sens6 checkout at the moment each
  command ran (git working tree + cmake_targets/ran_build/build), recomputed fresh by
  check_manifest.py -- never compared against a second copy of the manifest itself.
Predeclared assertions / thresholds: none deployment-dependent (Stage 0 has no scoring thresholds);
  the plan's EXACT invariants (section 3.3/4/5.4) and the 2026-09-11 full_auto=0 rule are the only
  frozen checks at this stage, both encoded in acceptance_four_rx.json.
Observed result, with denominators:
  - check_manifest.py against the fresh manifest: 0 mismatches ("MANIFEST OK"), 1 informational
    note (the manifest's own output file postdates its own scoped-file snapshot -- unavoidable
    self-reference, not scored as a mismatch).
  - check_manifest.py --acceptance/--geometry against the shipped v1 files: 15 reasons reported
    (14 UNSET deployment-dependent limits + geometry not surveyed) -- correctly INCOMPLETE.
  - check_manifest.py --selftest: 4/4 cases behaved as predeclared -- (a) fresh manifest PASS,
    (b) one binary hash altered -> FAIL, (c) shipped acceptance/geometry -> FAIL, (d) selected_test_
    conf swapped to adaptive_no_hints.conf (full_auto=1) -> FAIL. SELFTEST OVERALL: PASS.
  - Launcher-guard replication finding (measured, not assumed): against the CURRENT cache
    (ENABLE_ISAC_SENSING:BOOL=ON), every enable=0 (decoding-only-shaped) conf's own
    sensing/build guard would report BLOCKED -- the guard requires ENABLE_ISAC_SENSING:BOOL=OFF
    for an enable=0 conf. Only the enable=1 (sensing-enabled) confs currently pass that specific
    guard against this build. Recorded per-conf in the manifest; not exercised by running the
    launcher.
  - aoa_track_dl.conf's rx_array ("0,0;0.0434,0;0.0868,0;0.1302,0") independently verified to fail
    the rank-two AoA parser (openair1/PHY/NR_UE_ISAC/nr_isac.cc:102-125, condition at :118): the 3
    baseline vectors are exactly collinear (Gaussian-elimination rank 1), a sufficient condition for
    that parser's eigenvalue test to reject it regardless of numerical tolerance. Plan section 5.3's
    claim CONFIRMED, not merely repeated.
Status (PASS / FAIL / VOID / BLOCKED): IN_PROGRESS. Not PASS: the geometry survey and every
  deployment-dependent acceptance limit are BLOCKED (no survey performed; no application/deployment
  requirements supplied to derive limits from -- inventing them was explicitly disallowed).
Validity reasons and affected intervals: N/A -- no capture was taken this session (read-only
  identity/config work only).
Hypotheses supported / contradicted: supported -- aoa_track_dl.conf's rx_array fails the rank-two
  parser (plan section 5.3, now independently verified rather than inherited). Supported -- against
  the CURRENT build cache, every currently-existing enable=0 conf is launcher-BLOCKED for sensing/
  build mismatch, not just conceptually "decoding-only shaped" (a new, more precise finding than the
  plan recorded). Neither contradicts anything in the plan.
Retraction, if any: none. (Two STALE mentions elsewhere in this file -- "existing untracked file:
  aoa_track_dl.conf" and "existing modified file: run_adaptive_receive_test.sh" in the original
  2026-09-10 baseline-fact table -- are not retracted, they describe the plan's OWN inspection
  baseline; the "Current status" table above adds, rather than replaces, the row with what was
  actually measured at P01 session time.)
Remaining limitation: no conf in the repository currently matches "acquisition-only" (sensing
  disabled AND both DL and UL blind decode disabled) -- recorded as `"conf":null,"reason":"no
  config exists"` per the brief's instruction, not fabricated. Antenna-to-physical-channel mapping
  and receiver geometry remain UNSURVEYED; the acceptance profile is frozen for exact invariants
  only. P02 (reproducible DL/UL replay fixtures) has not started.
Next highest-value action: P02 -- select or capture a bounded receiver-only OTA fixture (manual
  mode, full_auto=0) sufficient to replay a known-valid supported single-antenna DL and UL case, per
  the plan's Stage 0 tests. A physical antenna survey is also a prerequisite for any future G5 work
  but is not on P02's critical path.
Reviewer / accomplishment date if gate passed: gate NOT passed; no reviewer assigned.
```

**Additional measured notes not captured by the template above:**

- **The tree carries more dirty state than the plan's own 2026-09-10 baseline row records.**
  `git status --short` at P01 session time additionally showed 5 more modified tracked files
  (`executables/nr-ue-ru.c`, `executables/nr-ue.c`, `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `radio/USRP/usrp_lib.cpp`,
  `tests/passive_rx/monitor/monitor.html`, `tests/passive_rx/monitor/monitor.py`) and 3 more
  untracked files (`tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/run_manual_x410.sh`) beyond the plan's recorded
  `M tests/passive_rx/run_adaptive_receive_test.sh` / `?? tests/passive_rx/aoa_track_dl.conf` pair.
  This is recorded as a measured fact (full list in the manifest's `git.status_short`), not resolved
  or explained further under P01 -- P01's job is to record identity, not to audit unrelated dirty
  work. None of it was touched, reset, or committed by this session.
- **`adaptive_manual_dlul.conf`** (untracked, part of the extra dirty state above) is a fifth
  `sensing = {...}` config discovered while classifying P01's three required cases. It is
  sensing-enabled (enable=1), full_auto=0 (admissible), and has BOTH DL and UL blind decode
  enabled with a pinned dedicated CORESET/search-space/BWP -- recorded as an additional
  `sensing-enabled` candidate in the manifest alongside `aoa_track_dl.conf` (the brief's named
  primary), not chosen as primary since the brief specifically names `aoa_track_dl.conf`.
- **Both sensing-enabled candidate confs set `pdcch_blind_monitor_pdsch` decode field to `1`**
  ("decode+count CRC pass rate only", per `nr_pdcch_blind_monitor.c`'s own config-key comment),
  not `2` ("also submit the reconstructed CFR"), while their own `sources=` lines list `pdsch_data`
  as a sensing source. Recorded as measured in the manifest; NOT investigated further here --
  P01 is identity/config recording, not decode-path validation (that is P02+ scope).

### Fix round 1 (2026-09-10, same day) — reviewer findings addressed

Coordinator review of the P01 session above returned "Approved with one Important finding + two
minors." All three addressed, commit `e4c8cadd5f22b40605f82f6adedc838ee8e056f5` (short
`e4c8cadd5f`), on top of the P01 commit `6fcb7a6c319d7ae7ad4b6cb03c075de0e2f7a77a` (short
`6fcb7a6c31`).

1. **IMPORTANT**: `antenna_mapping_and_geometry` and `timestamp_units` citations were hand-typed
   strings, unverified by `check_manifest.py`, and two had already drifted from the live source
   (`nr_isac.cc` `parse_array` recorded as "102-125", actually 102-128; `usrp_lib.cpp` RFCHAN
   recorded as "2485-2494", actually 2486-2496 for that specific `LOG_I` call). Fixed:
   `baseline_manifest.sh` gained `citation()`/`citation_json()` -- every citation is now a grepped
   `{file, line_start, line_end, token}` object, derived at generation time, never hand-typed
   (including a "loud, not silent" `CITATION_ERROR` to stderr if an end-token search comes up
   empty, after that exact failure mode silently produced a wrong 102-102 range on the first
   attempt at the `parse_array` citation -- caught before committing, not after).
   `check_manifest.py` gained `check_citations()` (recursive walk for `{file,line_start,line_end,
   token}` objects, re-verifies `token` still occurs in `[line_start,line_end]` of the LIVE file),
   wired into `check_identity()`. `--selftest` gained case (e): a copy with one citation's line
   range shifted by 500 must FAIL.
2. **MINOR**: `check_identity()`'s `os.readlink(liboai_device.so)` had no missing-file guard --
   wrapped in `try/except OSError`, now a listed mismatch rather than an unhandled crash.
3. **MINOR**: regenerated the manifest at the current HEAD (this branch's own prior P01 commit,
   `6fcb7a6c31`, which is the CORRECT thing for a fresh generation to record -- a manifest
   necessarily predates the commit that adds it, and re-running it after a commit to "catch up" is
   the pre-existing, already-documented self-reference limit, not a new one) and replaced the old
   `baselines/manifest_20260910_50c8312-dirty.json` (+ patch) pair via `git rm` rather than
   accumulating both, per the reviewer's instruction that `baselines/` holds one manifest per
   generation.

**Commands + output** (selftest run BEFORE the fix-round-1 commit, at the regenerated manifest's
own HEAD `6fcb7a6c31` -- this is the valid evidence; a run of the SAME command AFTER committing
would correctly show case (a) failing on `git.commit` drift, since the manifest was generated
before the commit that adds it, exactly as documented for the original P01 session above):

```text
$ python3 tests/passive_rx/check_manifest.py --selftest
(a) fresh manifest (manifest_20260910_6fcb7a6-dirty.json) expected PASS: PASS
(b) manifest with altered nr_uesoftmodem.sha256 expected FAIL: PASS
(c) shipped acceptance/geometry files expected FAIL (unset/unsurveyed): PASS
    <14 UNSET lines + "geometry: surveyed=False (must be true)">
(d) manifest with selected_test_conf full_auto=1 (adaptive_no_hints.conf) expected FAIL: PASS
(e) manifest with a shifted citation line range expected FAIL: PASS

SELFTEST OVERALL: PASS
```

**Files changed this round** (commit `e4c8cadd5f`): `tests/passive_rx/baseline_manifest.sh`
(modified), `tests/passive_rx/check_manifest.py` (modified),
`tests/passive_rx/baselines/manifest_20260910_50c8312-dirty.json` + `_tracked.patch` (removed),
`tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json` + `_tracked.patch` (added; git
recorded these as renames, same effect). No other file touched; the pre-existing dirty state
documented in the P01 session above is unchanged and was not re-inspected this round.

**`git status --short` after this round's commit:**

```text
 M executables/nr-ue-ru.c
 M executables/nr-ue.c
 M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c
 M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
 M radio/USRP/usrp_lib.cpp
 M tests/passive_rx/monitor/monitor.html
 M tests/passive_rx/monitor/monitor.py
 M tests/passive_rx/run_adaptive_receive_test.sh
?? adaptive_RX_pipeline.md
?? adaptive_RX_pipeline_progress.md
?? tests/passive_rx/MANUAL_DL_UL.md
?? tests/passive_rx/adaptive_manual_dlul.conf
?? tests/passive_rx/aoa_track_dl.conf
?? tests/passive_rx/run_manual_x410.sh
```

Identical dirty/untracked set to before this round (only the already-documented pre-existing
extras) -- confirms nothing beyond the intended 4 paths was touched.

## Session — 2026-09-11: P02 fixture audit and bounded capture (BLOCKED)

```text
Date/time (Europe/Zurich): 2026-09-11 (sens6 host clock reads UTC; capture window
  2026-09-11T00:14:34Z - 2026-09-11T00:17:39Z; see sens4/sens6 clock-skew note -- both timestamps
  are sens6's own clock, no cross-host comparison made).
Task IDs / gate: P02 / G0 (Stage 0).
Intended falsifiable claim: at least one existing or newly-captured replay.bin, produced by the
  CURRENT binary under manual mode (full_auto=0), replays with >=1 successful DL control and >=1 UL
  record and no radio opened; a fixtures.json entry with one hash altered is rejected by the
  checker.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing @
  e4c8cadd5f22b40605f82f6adedc838ee8e056f5 (unchanged this session -- P02 did not commit before
  writing this entry; see "Files modified" below for what is staged to commit).
  `git status --short` at session end: unchanged pre-existing dirty set (5 modified tracked files
  + `tests/passive_rx/run_adaptive_receive_test.sh` modified, `adaptive_RX_pipeline.md`,
  `adaptive_RX_pipeline_progress.md`, `tests/passive_rx/MANUAL_DL_UL.md`,
  `tests/passive_rx/adaptive_manual_dlul.conf`, `tests/passive_rx/aoa_track_dl.conf`,
  `tests/passive_rx/run_manual_x410.sh` untracked) PLUS this session's
  ` M tests/passive_rx/check_manifest.py` and new untracked
  `tests/passive_rx/baselines/fixture_audit_20260911.md`, `tests/passive_rx/baselines/fixtures.json`,
  `tests/passive_rx/replay_header.py`. Nothing reset/stashed/clobbered.
Files modified / added / removed: ADDED: tests/passive_rx/replay_header.py,
  tests/passive_rx/baselines/fixture_audit_20260911.md, tests/passive_rx/baselines/fixtures.json.
  MODIFIED (to be committed): tests/passive_rx/check_manifest.py (new check_fixtures() function,
  --fixtures CLI mode, selftest case (f)). MODIFIED but NOT committed (another session's untracked
  file; diff recorded in the fixture audit doc instead, per the brief): tests/passive_rx/
  run_manual_x410.sh (REPLAY=1 opt-in, byte-for-byte identical behaviour when REPLAY is unset --
  verified by rendering the effective env -i argument list both ways). REMOVED: none.
Executable / driver / config / geometry / acceptance hashes:
  nr-uesoftmodem sha256 b064c9957bc1b68d717543eb2bac28c4b2616115bc675f5fed38685f008742fe (same as
    the P01 manifest's recorded value -- binary unrebuilt since P01, confirmed by
    check_manifest.py NOT listing it as a mismatch even though git.commit/tracked_diff_patch_sha256
    now DO mismatch, see below);
  liboai_usrpdevif.so sha256 ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583
    (also unchanged/matching);
  attempt-2 receiver.conf sha256 a39e40f00e4ad0f456cb2b5c947f6e1e6bdbdadef069306231ef3075349ade92;
  attempt-2 run.log sha256 b9618723f3440b3832599c6b205a87d053937fdf2bac5b3c6ff6fdc48f830a6b.
Exact commands:
  python3 tests/passive_rx/check_manifest.py tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json
    (run BEFORE any P02 edit -- found 2 pre-existing mismatches, see "P01 manifest staleness" below)
  pgrep -a -x nr-uesoftmodem; pgrep -a -x nr-softmodem
  timeout 10s uhd_find_devices --args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174
  find openair1 openair2 executables radio -path '*/tests/*' -prune -o \
    ( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' ) \
    -newer cmake_targets/ran_build/build/nr-uesoftmodem -print -quit
  sudo -n python3 tests/passive_rx/replay_header.py <replay.bin>   (x22, all pre-existing fixtures)
  sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8090 bash tests/passive_rx/run_manual_x410.sh   (attempt 1)
  sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8091 bash tests/passive_rx/run_manual_x410.sh   (attempt 2)
  python3 tests/passive_rx/check_manifest.py --fixtures tests/passive_rx/baselines/fixtures.json
  python3 tests/passive_rx/check_manifest.py --selftest
  (No cmake/build command run at any point. No radio operation outside the two launcher invocations
  above, both through the manual launcher only.)
Artifact paths (include raw logs and VOID attempts):
  /home/sens/NICOLA/captures/sensing_manual_fixed.XmtvnI (attempt 1, VOID_RF_OR_ASSERT);
  /home/sens/NICOLA/captures/sensing_manual_fixed.JPdoGb (attempt 2, RF-valid, no replay.bin);
  full transcript: /home/sens/NICOLA/.superpowers/sdd/adaptive_RX_pipeline/task-P02-report.md
  (local to the controlling session, not part of this repo);
  tests/passive_rx/baselines/fixture_audit_20260911.md (full audit table + root-cause citations);
  tests/passive_rx/baselines/fixtures.json (registry, status=BLOCKED, admissible_fixtures=[]).
Baseline and comparison definition: "admissible" per the brief = same binary sha256 AND
  full_auto=0 AND >=1 successful DL record AND >=1 UL record, recomputed live by
  tests/passive_rx/replay_header.py + check_manifest.py --fixtures, never assumed from a filename
  or an old handover claim.
Predeclared assertions / thresholds: none deployment-dependent; the plan's exact invariants
  (full_auto=0, same-binary identity, no ground-truth-timing source) are the only frozen checks.
Observed result, with denominators:
  - 22/22 pre-existing replay.bin fixtures: INADMISSIBLE. All 22 fail on BOTH independent grounds
    (producing binary != current build's b064c995...; full_auto=1). Every one passed the reader's
    own internal self-consistency checks (magic match, file_bytes==header_bytes+iq_bytes) -- these
    are genuine, correctly-parseable captures, just not usable as a P02 baseline. Full table:
    tests/passive_rx/baselines/fixture_audit_20260911.md section 2.
  - Recorder bound check (brief requirement): iq_bytes=314,572,800 <= 512 MiB (536,870,912) for
    16 frames x 1,228,800 samples/frame (= 122.88 Msps x 10 ms, confirms 273 PRB geometry) x 4
    antennas x 4 B; matches every existing fixture's exact 316,268,528-byte size
    (header_bytes=1,695,728 + iq_bytes).
  - New capture attempt 1 (sensing_manual_fixed.XmtvnI): VOID_RF_OR_ASSERT. RFSTALL
    (ERROR_CODE_OVERFLOW) at the very first USRP_RX_START; 0 "SIB1 common facts"; nic_missed
    unchanged. Cold-start RF variance, not investigated further per the memory rule (kept as
    evidence, not retried indefinitely).
  - New capture attempt 2 (sensing_manual_fixed.JPdoGb): validity.txt=
    RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE, stop_reason=DURATION_COMPLETE, process_exit=0,
    cleanup_status=CLEAN, nic_missed before==after (27520822), SIB1 count=1, RXDISCONT/RFSTALL=0/0,
    receiver.conf pdcch_blind_monitor_full_auto=0 (confirmed), binary sha256 matches current build.
    DL: PDSCHQ crc_ok=38583/42014 (91.8%). UL: pusch_passive crc_ok=10562/12694 (83.2%). By every
    radio-level criterion this is a good, valid manual-mode capture. BUT: grep -aic replay run.log
    = 0 (not even a recorder failure-path log line fired), no replay.bin file exists in $OUT.
  - Root cause (proven from source, cited file:line, independently re-verified against the live
    tree with `sed -n '<range>p' <file> | grep -F -- '<token>'` for all 4 citations, all FOUND):
    nr_passive_replay_capture.c:206-209's ONLY arm-trigger requires
    job->sweep_ticket.generation != 0; sweep_ticket is populated ONLY inside
    nr_pdcch_blind_monitor_rt.c:2100's `if (g_pdsch_sweep_on && !is_dci10)` block (outside it,
    :2098 leaves it all-zero); g_pdsch_sweep_on is set in pdsch_sweep_maybe_enable() to
    `ready = cfg->dl_full_auto && (...)` (:164-166,184) -- REQUIRES full_auto=1. Under the mandatory
    TESTING MODE RULE (full_auto=0), this chain is provably always false, for every job, in every
    run, regardless of RF quality or duration. Not inferred from one run: the single call site of
    nr_pdsch_config_sweep_select() (the only place sweep_ticket.generation is ever assigned) is
    inside that same full_auto-gated block.
  - Attempt 3 NOT spent: the failure is deterministic/structural (proven above), not RF-variance;
    a third identical-code-path run cannot produce a different outcome, and running it anyway would
    be indistinguishable from retrying/tuning to force a pass, which the plan forbids.
  - check_manifest.py --fixtures tests/passive_rx/baselines/fixtures.json: "FIXTURES REJECTED (1):
    admissible_fixtures is empty" -- exit 1, correctly rejects (there is nothing to accept).
  - check_manifest.py --selftest: new cases (f1) synthetic single-fixture registry PASS,
    (f2) altered replay_bin_sha256 in a copy -> FAIL (correctly detected), (f3) real shipped
    fixtures.json -> FAIL (correctly detected, empty registry). Pre-existing (b)/(c)/(d)/(e) still
    PASS unchanged. (a) "fresh manifest passes" now FAILS -- see "P01 manifest staleness" below;
    this is NOT introduced by P02's logic, it is P01's manifest being older than the live tree.
    SELFTEST OVERALL: FAIL (driven entirely by pre-existing (a), not by any new case).
  - P01 manifest staleness (measured BEFORE any P02 edit, so not caused by this session):
    check_manifest.py against manifest_20260910_6fcb7a6-dirty.json reported 2 mismatches
    (git.commit: manifest=6fcb7a6c... live=e4c8cadd5f...; git.tracked_diff_patch_sha256 mismatch)
    -- the "P01 fix round 1" commit landed after the manifest was generated. build.nr_uesoftmodem.
    sha256 was NOT among the mismatches (binary unrebuilt). This session's own edits to
    tests/passive_rx/check_manifest.py and tests/passive_rx/run_manual_x410.sh add two more
    expected scoped_file diffs on top of that pre-existing staleness. Regenerating the P01 manifest
    is P01's scope, not done here.
Status (PASS / FAIL / VOID / BLOCKED): BLOCKED. Deliverable 1 (fixture audit) PASS. Deliverable 2
  (bounded capture) executed correctly (2 attempts, one RF-valid) but did not yield a usable
  fixture -- BLOCKED by a structural code gap outside P02's scope, not a capture-quality failure.
  Deliverable 3 (replay verification) NOT PERFORMED -- no fixture exists to verify (correctly not
  attempted; nothing to run ISAC_PASSIVE_REPLAY_INPUT against). Deliverable 4 (fixtures.json +
  checker extension + selftest) PASS on its own terms -- the checker code is built and correctly,
  demonstrably rejects both a synthetic altered-hash fixture and the real empty registry. G0: was
  IN_PROGRESS, now correctly downgraded to BLOCKED (see gate table) -- "at least one reproducible
  supported DL and UL case" is not met and cannot be met by more capture attempts alone.
Validity reasons and affected intervals: attempt 1 VOID_RF_OR_ASSERT (RFSTALL at stream start,
  whole run affected, 0 usable data). Attempt 2 RF-valid for its own SIB1/DL/UL/NIC/exit criteria
  (whole 180 s window usable for THOSE purposes) but yields no replay evidence at all (0/180s
  usable for the replay-fixture purpose specifically) -- two different validity questions about the
  same run, both recorded rather than collapsed into one label.
Hypotheses supported / contradicted: contradicted -- the working assumption (carried in the plan's
  own P02 description and MANUAL_DL_UL.md) that a REPLAY=1 opt-in on the manual launcher would be
  sufficient to produce a manual-mode replay fixture. It is necessary (the launcher now correctly
  passes the env vars, verified byte-for-byte) but not sufficient: the recorder's arm condition is
  wired exclusively to the full-auto sweep mechanism the TESTING MODE RULE forbids running.
Retraction, if any: none of P02's own prior claims (P02 had not started). Not a retraction of P01.
Remaining limitation: no admissible replay fixture exists; G0 cannot pass until either (a)
  nr_passive_replay_capture.c's arm condition is changed to not require sweep_ticket.generation
  when cfg->dl_full_auto=0 (a real design decision about what "settled" means for a job with no
  sweep, belonging to that file's owner, out of P02's scope), or (b) an alternative reproducible-
  fixture mechanism is agreed for manual mode. The 22 pre-existing fixtures remain available as
  DM-RS/PDSCH decode reference material (all internally well-formed, per the audit) but cannot
  serve as a P02 baseline under this plan's binary/full_auto invariants.
Next highest-value action: decide (with whoever owns nr_passive_replay_capture.c /
  nr_pdcch_blind_monitor_rt.c) whether to relax the recorder's arm condition for full_auto=0, or to
  define a different manual-mode-compatible arm signal (e.g. "N consecutive successful DL+UL jobs"
  instead of a sweep-settled ticket) -- then re-run P02's capture step once that lands. Until then,
  P02/G0 stay BLOCKED and no later stage should be scored against "a validated replay baseline
  exists".
Reviewer / accomplishment date if gate passed: gate NOT passed; no reviewer assigned.
```

### Fix round 1 + Fix round 2 (2026-09-11, same day) — controller-ruled: recorder arm condition is a plan/code defect, fixed in scope

The P02 session above reported the replay recorder BLOCKED by a structural defect and recommended
against self-directed patching. The controller reviewed, ruled it in scope as a small, targeted
fix, and directed two rounds of work (a session restart occurred between rounds; the tree state
-- uncommitted diagnostics, the 00:55 UTC binary, the staged manifest rename, and captures
3NB5Ri/JLDoZc/LH2I9w/Ma654f -- survived it and was continued rather than redone).

**Defect 1 (DL gate, fixed fix round 1).** `nr_passive_replay_capture.c`'s `nr_passive_replay_dl()`
(~line 201-209) required `job->sweep_ticket.generation != 0` to arm. `sweep_ticket` is populated
only inside `nr_pdcch_blind_monitor_rt.c:2100`'s `if (g_pdsch_sweep_on && !is_dci10)` block, and
`g_pdsch_sweep_on` requires `cfg->dl_full_auto` true (`:164-166,184`) -- structurally unreachable
under the plan's mandatory manual mode. **Ruling**: treat `generation==0` (manual mode, no sweep in
progress) as an already-settled ticket. Fixed both gates in `nr_passive_replay_dl()`, 2-line comment
citing the manual-mode rule, `LOG_I` "REPLAY geometry ..." kept in `nr_passive_replay_init()`.

Rebuilt (binary `b81b21e37dae9ad4666319274a6e32134055329eec150af3c75af4a89578c5d7`), regenerated the
manifest, captured twice (`3NB5Ri` RF-valid but still 0 replay activity; `JLDoZc` VOID_RF_OR_ASSERT
from an unrelated late-run RFSTALL). **Defect 1's fix alone was necessary but NOT sufficient** --
`3NB5Ri` proved this empirically (RF-valid, high DL/UL volume, still zero `replay`-related log
lines).

**Diagnosis, not more blind retries.** Rather than spend the 3rd allowed capture attempt on an
identical code path, added temporary `LOG_A` instrumentation (init geometry, armed-state, per-call
`dl#`/state/status/success/gen/settled/ul_seen, first-UL-seen one-shot, arm-trigger one-shot),
rebuilt twice more (`5745d305...`, DURATION=30; `74d2b818...`, DURATION=60 with the first-UL
diagnostic added), and ran two SHORT diagnostic captures (`LH2I9w`, `Ma654f`) -- explicitly NOT
counted against the 2-attempt cap, since their purpose was instrumentation, not fixture production.
`LH2I9w` showed `dl#1..20` with `success=1` repeatedly but `ul_seen=0` always. `Ma654f` (60s) never
printed "first UL seen" at all, despite `PDSCHQ decoded=7644 crc_ok=7050` and
`pusch_passive try=6114 crc_ok=5010` in the SAME run -- proving `nr_passive_replay_ul()` was never
called even once, while ordinary DL/UL decode volume was high.

**Defect 2 (UL gate, found by this diagnosis, fixed fix round 2, controller-verified).**
`nr_pdcch_blind_monitor_rt.c:1738-1742` called `nr_passive_replay_ul()` only inside
`if(cand_task[ti].ok && cand_task[ti].ul_auto)`. Manual mode pins `dci01 = "1:43"`, so `ul_auto` is
always false and the hook never fires -- `ul_seen` never becomes true, so the (now-fixed) DL arm
condition's `atomic_load(&ul_seen)` clause can never pass either. **Confirmed before fixing** (per
the ruling's own instruction to cite, not guess) that `boot_rnti`, `dci01_length` and
`ul_out.raw_payload` are all populated in the manual/pinned path independent of `ul_auto`:
`dci01_length` at `nr_pdcch_blind_monitor_rt.c:1071-1074` (unconditional whenever `scan_01`),
`boot_rnti` at `:1452-1460` (gated only on a confirmed-RNTI-set hit, not on `dl_full_auto`), and
`ul_out.raw_payload` at `:555-556` inside `nr_pdcch_blind_cand_worker_body()`'s
`nr_pdcch_blind_decode_01_mode()` call (runs whenever `t->ul_scan`, `ul_auto` is passed as a
parameter INTO the decode, not a gate on whether it runs). **Ruling**: move the
`nr_passive_replay_ul(...)` call so it fires once whenever `cand_task[ti].ok`, ahead of the existing
`if(cand_task[ti].ok && cand_task[ti].ul_auto)` block (which keeps gating DISCOVERY --
`nr_pdcch_ul_discovery_grant()` -- unchanged), and delete the copy inside that block. Applied
exactly as specified; 6-line comment added citing the three file:line facts above.

Trimmed the diagnostics to the two the ruling said earn their keep: the `REPLAY geometry ...` line
in `nr_passive_replay_init()` (kept, `LOG_I`) and ONE one-shot `LOG_I` "REPLAY ARMED at
absolute_slot=%ld" at the successful `RP_ARMED`->`RP_REQUESTED` transition inside
`nr_passive_replay_dl()`. Deleted: the per-call `dl#`/`diag_count` block and the first-UL one-shot.
Confirmed clean (`grep -c REPLAYDIAG` = 0 in both files).

**Pre-build check honored a live conflict.** Before rebuilding, `pgrep -x nr-uesoftmodem` showed
another session's autonomous multi-probe scan actively running (`adaptive-rx-UL-DL` tree,
`captures/autonomous_lan_8min_20260911_072035_3354627/`). Per instruction, NOT killed -- polled
`pgrep -x nr-uesoftmodem` every 60s (background poll, then a coordinator-directed foreground
`ssh ... for i in $(seq 60); do pgrep ... || exit 0; sleep 60; done; exit 1` capped by the tool at
600s and continued in the background) until it exited on its own (~free within the first poll
interval after the second instruction landed). Radio confirmed `claimed: False` before proceeding.

**Rebuild**: `cmake --build cmake_targets/ran_build/build --target nr-uesoftmodem oai_usrpdevif
--parallel 4`. Final binary sha256
`c3810019905f3fb661138d12fd5b9b6156d47e36344ae871f08047ba65dc7889` (only
`nr_pdcch_blind_monitor_rt.c.o`/`nr_passive_replay_capture.c.o` recompiled, relinked; one
pre-existing unrelated `-Wformat-zero-length` warning at `nr_pdcch_blind_monitor_rt.c:1291`, not
from this change).

**Manifest regenerated a final time** (one-pair rule: `git rm` the round-1-commit pair, regenerate
fresh so the new manifest's own scoped-file snapshot doesn't reference now-deleted siblings --
same self-reference trap P01's "Fix round 1" already documented) after ALL round-2 source/checker/
fixtures.json edits landed, so `check_manifest.py --selftest` case (a) genuinely reflects the final
state, not an intermediate one:
```text
$ python3 tests/passive_rx/check_manifest.py --selftest
(a) fresh manifest (manifest_20260911_b90276d-dirty.json) expected PASS: PASS
(b) manifest with altered nr_uesoftmodem.sha256 expected FAIL: PASS
(c) shipped acceptance/geometry files expected FAIL (unset/unsurveyed): PASS
(d) manifest with selected_test_conf full_auto=1 (adaptive_no_hints.conf) expected FAIL: PASS
(e) manifest with a shifted citation line range expected FAIL: PASS
(f1) synthetic single-fixture registry expected PASS: PASS
(f2) synthetic registry with altered replay.bin hash expected FAIL: PASS
(f3) shipped fixtures.json expected PASS (>=1 admissible fixture registered): PASS

SELFTEST OVERALL: PASS
```
Note case (f3)'s assertion direction flipped from round 1 (was "expected FAIL, empty registry") to
"expected PASS, >=1 admissible fixture registered" -- the shipped `fixtures.json` is no longer
empty by construction, so the OLD assertion would now be testing a stale assumption, not the
checker's correctness. The REQUIRED negative case per the brief (a copy with one hash altered is
rejected) is case (f2), on a synthetic registry, unaffected by this.

**Capture (final, fix round 2)**:
```text
$ sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8096 bash tests/passive_rx/run_manual_x410.sh
OUTPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7
verdict=RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE cleanup=CLEAN exit=0
```
validity=RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE, stop_reason=DURATION_COMPLETE, process_exit=0,
cleanup=CLEAN, SIB1=1, nic_missed before==after (27607564), RXDISCONT/RFSTALL=0/0, full_auto=0
confirmed, binary matches. `REPLAY ARMED at absolute_slot=8852`.
`REPLAY READY: .../replay.bin slots=320 UL=24 DL-controls=38 IQ=314572800 bytes`.
`replay_header.py`: version=2, n_ul=24, n_dl=38 (34 success / 4 failure), file_bytes match=True.
1 attempt sufficed (of 2 allowed).

**Replay verification (no radio, deliverable 3)**:
```text
$ cd cmake_targets/ran_build/build && sudo -n env \
    ISAC_PASSIVE_REPLAY_INPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7/replay.bin \
    LD_LIBRARY_PATH=$(pwd):/usr/local/lib ./nr-uesoftmodem \
    -O /home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7/receiver.conf \
    -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 --ue-rxgain 40 --ue-nb-ant-rx 4 \
    --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation --cont-fo-comp 1 --freq-sync-P 0.05 \
    --freq-sync-I 0.001 --initial-fo -16480 --thread-pool 0,1,6,7 --time-sync-I 0.01 \
    --ntn-initial-time-drift -4.25 -A 90
...
REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened

real	0m4.463s
$ echo $?
0
```
Confirmed `nr-uesoftmodem.c:428-434` (replay branch returns before any radio/UHD call) -- and the
tool's own "no radio opened" text in the PASS line is a second, independent confirmation.
No-ground-truth-timing check: `nr_passive_replay_read()` (`nr_passive_replay_capture.c:225-303`)
reads only the file's own header/IQ/job metadata (source-index continuity at `:266-269`) -- no gNB
log, GT file, or wall-clock alignment opened anywhere in it or its callee
`nr_pdsch_passive_decode()`.

**Fixture registration**: `tests/passive_rx/baselines/fixtures.json` `status` BLOCKED -> PASS,
`admissible_fixtures` `[]` -> 1 entry (`sensing_manual_fixed.UtvBT7`, all 3 file hashes +
producing-binary hash + full_auto=0 + header stats + replay verdict). `replay.bin` was `root:600`
by the C source's own `open(...,0600)` (out of this fix's scope to change); `chmod 644` applied to
THIS registered fixture only (sha256 re-verified unchanged after chmod) so the checker is runnable
without sudo, matching every other registered-artifact convention in this repo. `check_manifest.py
--fixtures tests/passive_rx/baselines/fixtures.json` -> `FIXTURES OK`, exit 0.

**Files changed this round** (beyond the P02 session's original 4): `openair1/PHY/NR_UE_TRANSPORT/
nr_passive_replay_capture.c` (modified, DL gate + one `LOG_I`), `openair1/PHY/NR_UE_TRANSPORT/
nr_pdcch_blind_monitor_rt.c` (modified, UL hook moved), `tests/passive_rx/check_manifest.py`
(modified, f3 assertion direction), `tests/passive_rx/baselines/fixtures.json` (modified, resolved),
`tests/passive_rx/baselines/manifest_20260911_b90276d-dirty.json` + `_tracked.patch` (regenerated,
same one-pair rule). `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` (no `_rt`) is another
session's dirty file -- confirmed NOT staged, NOT committed.

**Status**: P02 PASS, G0 IN_PROGRESS (P01's own outstanding survey/limits work is the only remaining
blocker, unchanged by this fix). See updated checklist/gate rows above.

## Documentation verification

Record structure/link/checklist checks and local/remote file-copy equality here. These are documentation checks only, not receiver or solver gate evidence.

- PASS on 2026-09-10: both documents contain the same 34 unique task IDs; only D01/D02 are checked and dated complete.
- PASS: balanced fenced blocks, valid relative document links, all G0–G6 and UG1–UG7 present, no diagram blocks.
- PASS: all 10 shell-example blocks pass `bash -n`; none of the documented build/test/receiver commands was executed.
- PASS on 2026-09-10: both documents published to the canonical sens6 worktree and local/remote SHA-256 equality verified. The two pre-existing user files retained their content hashes. Only the two requested Markdown files were added; receiver source/config/build files were not modified.
- Plan SHA-256 at publication: `5d3ece65dbc9c7b6c22837a1c0f43131120db1db4cbbde7542d478a940f27d10`. This ledger is updated after the initial copy check; its final copy is reverified during synchronization.

## Acceptance-profile record — fill before evaluated captures

As of 2026-09-10 (P01) this table has a versioned home: `tests/passive_rx/acceptance_four_rx.json`
(`profile_version:1`, `frozen_date:null` -- frozen for the plan's exact invariants only, per the
brief). Every row below is `status:"UNSET"`, `value:null` in that file by construction; no number
has been invented. `tests/passive_rx/check_manifest.py --acceptance ... --geometry ...` exits
non-zero while any row is UNSET or the geometry is unsurveyed (verified 2026-09-10, see the P01
session below).

| Required field | Value / evidence |
|---|---|
| Profile path / hash / freeze date | UNSET. Path/hash of the record itself: `tests/passive_rx/acceptance_four_rx.json` (sha256 `57896964efde38903a8c4537dd09cc6d92e7c2f565a101bbf1778cbc42e6ad8b` at 2026-09-10); `frozen_date:null` |
| Experiment purpose and active RX set | UNSET (`acceptance_four_rx.json:deployment_dependent_limits.experiment_purpose_and_active_rx_set`) |
| Surveyed geometry / coordinate frame / uncertainty | UNSET; formalized separately as `tests/passive_rx/geometry_four_rx.json` (`surveyed:false`, sha256 `f27963008c0d63279c25f3fdc3af846d43563b6fffa4af05181940b2532bcdb1` at 2026-09-10) |
| Supported traffic, layer/waveform scope and denominators | UNSET (`...deployment_dependent_limits.supported_traffic_layer_waveform_scope_denominators`) |
| Capture duration / minimum eligible event count | UNSET (`...capture_duration_minimum_eligible_event_count`) |
| Acquisition and sample continuity requirements | SET, as an EXACT INVARIANT (not deployment-dependent): "Exact identity; no unexplained baseline sample loss" -- now `acceptance_four_rx.json:exact_invariants.zero_unexplained_baseline_sample_loss` (`value:true`), alongside the plan's other exact invariants (`exact_source_config_binary_identity`, `baseline_launcher_exit_code:124`, `no_rf_tx_calls_in_passive_mode`, `single_hardware_owner`, `ul_fusion_enable_default:false`, and the 2026-09-11 amendment's `full_auto_must_be_zero:true`) |
| Queue / latency / memory limits and rationale | UNSET (`...queue_latency_memory_limits_and_rationale`) |
| Detector range **and Doppler** scoring thresholds | UNSET (`...detector_range_and_doppler_scoring_thresholds`) |
| DL position / velocity / availability criteria | UNSET (`...dl_position_velocity_availability_criteria`) |
| UL reference/association quality criteria | UNSET (`...ul_reference_association_quality_criteria`) |
| Rank tolerance, state scaling and conditioning policy | UNSET (`...rank_tolerance_state_scaling_conditioning_policy`) |
| UL position / velocity / availability criteria | UNSET (`...ul_position_velocity_availability_criteria`) |
| Catastrophic-error definition / robustness targets | UNSET (`...catastrophic_error_definition_robustness_targets`) |
| Covariance coverage / calibration procedure | UNSET (`...covariance_coverage_calibration_procedure`) |
| Calibration versus held-out evaluation allocation | UNSET (`...calibration_vs_held_out_evaluation_allocation`) |

Unset limits are not automatic passes. Fill them from application needs, geometry and independent calibration evidence; do not choose them after seeing evaluated metrics.

## Session — 2026-09-11: P03 branch abstraction (foundation) + globals audit (IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-11, ~10:30-11:15
Task IDs / gate: P03 (Stage 1 foundation); G1 not exercised (see below)
Intended falsifiable claim: a pure, unit-testable branch identity/epoch/lifecycle module exists
  and matches G1 tests 2 and 4 in isolated ("pure") form; a process-wide globals audit of the
  files P03's brief names exists with file:line for every symbol found; neither claim depends on
  anything running live or on the nr-ue.c RT read loop being touched (out of this task's scope
  per the controller's ruling in task-P03-brief.md).
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing,
  HEAD 8847b6a92200ffe8b044bda13bf83be56e0ee7ab (unchanged by this session). Pre-existing dirty
  state unchanged and untouched: `M executables/nr-ue-ru.c`, `M executables/nr-ue.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`,
  `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`,
  `M tests/passive_rx/run_adaptive_receive_test.sh`, and untracked
  `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/aoa_track_dl.conf`, `tests/passive_rx/run_manual_x410.sh`,
  `adaptive_RX_pipeline.md`, `adaptive_RX_pipeline_progress.md` (this file) -- confirmed via
  `git status --short` before and after this session's edits; none of these paths were staged.
Files modified / added / removed:
  Added: `docs/passive_branch_globals_audit.md`, `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch.h`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch.c`,
  `openair1/PHY/NR_UE_TRANSPORT/tests/nr_rx_branch_test.cc`.
  Modified: `CMakeLists.txt` (added `nr_rx_branch.c` to `PHY_NR_UE_SRC`; registered the
  `nr_rx_branch_test` gtest target, mirroring `test_nr_pdsch_config_sweep`'s pattern -- own .c
  file linked directly, `UTIL`+`GTest::gtest`, no PHY_NR_UE dependency), `nr_isac.h`/`.cc`
  (`[sensing] rx_branches`/`rx_branch_phys_map` parsing, `nr_isac_rx_branches()` accessor,
  `nr_isac_set_nb_antennas_rx()` setter), `nr_isac_stub.c` (matching no-op stubs, required so the
  `ENABLE_ISAC_SENSING=OFF` build still links).
Executable / driver / config / geometry / acceptance hashes: not applicable -- no receiver run,
  no capture; only the gtest binary `nr_rx_branch_test` was built and run.
Exact commands:
  ssh sens6 'pgrep -a -x nr-uesoftmodem' (empty both before configure and before build --
  polled once, no wait needed)
  cd /home/sens/NICOLA/adaptive-rx-sensing/cmake_targets/ran_build/build && cmake .
  cmake --build . --target nr_rx_branch_test --parallel 4
  ctest --test-dir cmake_targets/ran_build/build -R nr_rx_branch_test --output-on-failure
Artifact paths (include raw logs and VOID attempts): none beyond the ctest console output pasted
  in task-P03-report.md; one FAILING intermediate run (SegFault, `RxBranchParse.
  RejectsDuplicateBranch`, root-caused to LOG_E firing before `logInit()` -- fixed by adding
  `logInit();` to the test's `main()`, matching `nr_hyp_sweep_test.cc`'s existing pattern) is
  recorded in task-P03-report.md, not re-run against a stale binary.
Baseline and comparison definition: not applicable (new module, no prior behavior to compare
  against; explicitly NOT wired into any existing behavior in this task).
Predeclared assertions / thresholds: the brief's 5 parse-reject cases (duplicate branch,
  duplicate physical, out-of-range, more-branches-than-antennas, missing active-branch mapping)
  plus G1 test 2 and test 4 in pure form (see task-P03-brief.md's exact wording, reproduced in
  the test file's own comments above each case).
Observed result, with denominators: 14/14 gtest cases pass (`nr_rx_branch_test`, 4 suites:
  RxBranchParse x8, RxBranchCheckAntennas x1, RxBranchPermute x1, RxBranchLifecycle x4). No other
  target was built or run; `nr-uesoftmodem` was not rebuilt or executed.
Status (PASS / FAIL / VOID / BLOCKED): PASS for the two falsifiable claims stated above (module
  builds and its own tests pass; audit exists with file:line citations). P03 as a WHOLE task
  remains IN_PROGRESS -- see "Remaining limitation" below; this is a controller-scoped foundation
  slice of P03, not full P03 completion, and G1 itself is NOT exercised by this session.
Validity reasons and affected intervals: not applicable (no live data collected).
Hypotheses supported / contradicted: supports the audit's own finding that
  `PHY_VARS_NR_UE.rf_map` is the concrete field coupling a reused per-branch struct back to
  hardware ownership (`openair1/PHY/defs_nr_UE.h:285`, `nr-ue-ru.c`'s `openair0_dev[]`/
  `openair0_cfg_g[]` indexed by `rf_map.card`); supports that the three passive queues'
  single global `PHY_VARS_NR_UE *g_ue` (`nr_pdcch_passive_queue.c:73`,
  `nr_pdsch_passive_queue.c:88`, `nr_pusch_passive_queue.c:70`) is the clearest "one branch
  assumed" defect blocking P04/P07/P08. Contradicts nothing; no prior claim was tested.
Retraction, if any: none.
Remaining limitation: nothing in this session is wired into the nr-ue.c RT read loop (by
  controller ruling, deferred to P04/P05); `nr_isac_set_nb_antennas_rx()` has no live caller yet
  (nr-uesoftmodem.c is outside this task's committed-file scope -- see the audit doc's
  "nb_antennas_rx reachability" section for the exact citation of where the live value already
  exists at `nr_isac_init()`'s call time and why it doesn't reach the function today); G1's
  actual 5 tests (stall/drop, frame-wrap+discontinuity+relock against a REAL sample stream,
  acquisition-vs-standalone-replay comparison) all need P04's buffer delivery and P05's real
  digital correction to be answerable end-to-end, not just P03's pure identity/epoch struct.
Next highest-value action: P04 (immutable channel-buffer delivery) -- needs the `nr_rx_branch_t`
  built here as the addressing/epoch substrate for per-branch sample ownership.
Reviewer / accomplishment date if gate passed: not gated; G1 stays NOT_STARTED (unchanged by this
  session, per the controller's ruling that P03 alone cannot make G1 testable end-to-end).
```

## Session — 2026-09-11: P04 immutable buffer delivery (Stage 1, foundation) (IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-11, ~11:20-12:05
Task IDs / gate: P04 (Stage 1 immutable buffer delivery); G1 tests 1 and 3 exercised in pure form
  (see below); G1 as a whole stays NOT_STARTED (unchanged by this session, per the controller's
  ruling that P04 alone cannot make G1 testable end-to-end without the nr-ue.c read-loop wiring).
Intended falsifiable claim: a pure, unit-testable refcounted per-branch span pool exists that (a)
  routes byte-exact channel content to the correct branch under both an identity and a permuted
  P03 physical-channel map (G1 test 1, pure form), and (b) enforces the declared per-branch drop
  policy -- a stalled branch drops its own oldest entries and is counted, sibling branches and the
  producer are unaffected, no consumer ever observes an overwritten buffer (G1 test 3, pure form)
  -- plus refcount correctness (a span held by N branches returns to the free list only after all
  N release it; double release is rejected, not a double free) and epoch carry-through (a span's
  acq_epoch is fixed at acquire time and unaffected by later acquires/publishes under a different
  epoch). None of this depends on anything running live or on the nr-ue.c RT read loop being
  touched (out of this task's scope per the controller's ruling in task-P04-brief.md).
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing,
  HEAD 835481c8596c59eca799c5928d90e0203e7238f6 (unchanged by this session before commit). Pre-
  existing dirty state unchanged and untouched: `M executables/nr-ue-ru.c`,
  `M executables/nr-ue.c`, `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`,
  `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`,
  `M tests/passive_rx/run_adaptive_receive_test.sh`, and untracked
  `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/aoa_track_dl.conf`, `tests/passive_rx/run_manual_x410.sh`,
  `adaptive_RX_pipeline.md` -- confirmed via `git status --short` before and after this session's
  edits; none of these paths were staged.
Files modified / added / removed:
  Added: `openair1/PHY/NR_UE_TRANSPORT/nr_rx_span_pool.h`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_rx_span_pool.c`,
  `openair1/PHY/NR_UE_TRANSPORT/tests/nr_rx_span_pool_test.cc`.
  Modified: `CMakeLists.txt` (added `nr_rx_span_pool.c` to `PHY_NR_UE_SRC` next to
  `nr_rx_branch.c`; registered the `nr_rx_span_pool_test` gtest target mirroring
  `nr_rx_branch_test`'s pattern -- own .c + `nr_rx_branch.c` linked directly, `UTIL` +
  `GTest::gtest`, no PHY_NR_UE library dependency), `adaptive_RX_pipeline_progress.md` (this
  session entry + P04 row + status paragraph).
Executable / driver / config / geometry / acceptance hashes: not applicable -- no receiver run,
  no capture; only the gtest binary `nr_rx_span_pool_test` was built and run.
Exact commands:
  ssh sens6 'pgrep -x nr-uesoftmodem' (empty, polled before configure and before build)
  cd /home/sens/NICOLA/adaptive-rx-sensing/cmake_targets/ran_build/build && cmake .
  cmake --build . --target nr_rx_span_pool_test --parallel 4
  ctest --test-dir cmake_targets/ran_build/build -R nr_rx_span_pool_test --output-on-failure
Artifact paths (include raw logs and VOID attempts): none beyond the ctest/gtest console output
  pasted in task-P04-report.md; no failing intermediate run -- the module built and passed on the
  first build+run attempt.
Baseline and comparison definition: not applicable (new module, no prior behavior to compare
  against; explicitly NOT wired into any existing behavior in this task).
Predeclared assertions / thresholds: the brief's 4 test groups verbatim -- (1) routing: 4
  channels, distinct deterministic per-channel sequence, byte-exact `memcmp` under an identity
  map ("0:0,1:1,2:2,3:3") and a permuted map ("0:3,1:2,2:1,3:0") via P03's
  `nr_rx_branch_set_parse`, checked against the SAME published span (routing is an accessor-time
  decision in this design, not a publish-time one); (3) stall: hold_budget=3, 4 active branches,
  branch 2 never takes across 10 publishes -> branch 2's `dropped_spans==7`, its surviving ring
  holds spans {7,8,9} oldest-first, branches 0/1/3 (take+release every publish) see 0 drops and
  all 10 spans in order with correct `absolute_slot`/`first_sample_ts`, `producer_stalls==0`, and
  every buffer is filled with its own span index on acquire and checked byte-exact on take
  (overwrite detector); refcount: a span held by 2 branches stays off the free list until both
  release, a second release once refcount is 0 returns -1 (not a double free); epoch carry: a
  span's `acq_epoch` set at acquire is unchanged by a later acquire/publish under a different
  epoch, verified by taking both spans off one branch's ring and comparing each against a
  simulated branch epoch. Plus one additional init-validation suite (mirroring
  `nr_rx_branch_test.cc`'s rejection coverage): NULL pool, invalid n_ch/samples_per_buf/n_buf/
  hold_budget, active_branches with no bit set or an out-of-range bit, and the undersized-pool
  case `n_buf < 1 + n_active_branches*hold_budget` -- all rejected with -1 and an unchanged pool
  on the exact-fit boundary case.
Observed result, with denominators: 5/5 gtest cases pass (`nr_rx_span_pool_test`, 5 suites:
  SpanPoolRouting, SpanPoolStall, SpanPoolRefcount, SpanPoolEpoch, SpanPoolInit). Hand-verified
  the stall test's drop arithmetic against the implementation before trusting the assertion
  (publish i for i>=3 drops exactly one oldest entry per publish under hold_budget=3, giving 7
  drops over i=3..9 and a final ring of {7,8,9} -- matches). No other target was built or run;
  `nr-uesoftmodem` was not rebuilt or executed.
Status (PASS / FAIL / VOID / BLOCKED): PASS for the two falsifiable claims stated above (module
  builds and its own tests pass, including G1 tests 1 and 3 in pure form). P04 as a WHOLE task
  remains IN_PROGRESS -- see "Remaining limitation" below; this is a controller-scoped foundation
  slice of P04, not full P04 completion, and G1 itself is NOT exercised end-to-end by this
  session (no real producer thread, no nr-ue.c wiring, no live IQ).
Validity reasons and affected intervals: not applicable (no live data collected).
Hypotheses supported / contradicted: supports the brief's claimed design property that channel
  routing can be made an accessor-time decision (via `nr_rx_span_for_branch`/
  `nr_rx_span_channel` reading `physical_channel` off P03's `nr_rx_branch_t`) fully decoupled
  from publish-time behavior -- the SAME published span was routed correctly under two different
  branch-to-physical-channel maps without a second publish. Supports that a per-branch bounded
  ring with oldest-drop-on-full is sufficient to guarantee "a slow branch cannot hold others
  indefinitely" (plan sec 4) without any blocking, coordination or backpressure between branches
  -- verified structurally (drop_oldest only ever touches the stalled branch's own ring/refcount
  share) and empirically (branches 0/1/3's counters stayed exactly 0 while branch 2 accumulated
  drops). Contradicts nothing; no prior claim was tested.
Retraction, if any: none.
Remaining limitation: nothing in this session is wired into the nr-ue.c RT read loop (by
  controller ruling, deferred to P05+); there is no real producer thread and no live IQ --
  `nr_rx_span_pool_acquire`/`publish` were driven directly from the test's main thread, not from
  a hardware read loop, so the "~2 kHz single-producer" assumption behind the module's one-mutex
  design (documented as a `ponytail:` comment in the header) is architectural, not load-tested.
  `sample_rate_hz` was added to the pool as a plain pool-lifetime constant (plan sec 3.2 lists it
  under "Run / hardware") since it cost one field and no extra logic; `run_id`/`rx_id` were
  deliberately NOT duplicated into the span/pool -- they already live on P03's
  `nr_rx_branch_set_t`/`nr_rx_branch_t`, which every real caller of this pool already holds, so
  duplicating them here would be redundant state that could drift. `nr_rx_span_pool_release()`
  validates the (pool, span, branch_id) triple's *range* but does not verify that `branch_id`
  actually holds the specific ref-unit being released -- refcount is a single shared integer, not
  per-branch-tagged, so a caller could in principle route a release through the wrong branch_id
  without the module detecting it (it would still be a legitimate decrement of a real outstanding
  reference, just not from the branch that logically owned it). This matches the brief's stated
  scope (refcount as a plain count) and is flagged here as a caller-discipline requirement for
  P05's wiring, not a defect fixed in this session.
Next highest-value action: P05 (independent digital correction/recovery per branch), which is
  also the task that finally wires both `nr_rx_branch_t` (P03) and `nr_rx_span_pool_t` (P04) into
  the real nr-ue.c read loop -- current controller guidance is that this wiring waits on that
  file's owning session committing its edits first.
Reviewer / accomplishment date if gate passed: not gated; G1 stays NOT_STARTED (unchanged by this
  session, per the controller's ruling that P04 alone cannot make G1 testable end-to-end).
```

## Session template — copy for each future work session

```text
Date/time (Europe/Zurich):
Task IDs / gate:
Intended falsifiable claim:
Branch / full commit / dirty patch / untracked-file manifest:
Files modified / added / removed:
Executable / driver / config / geometry / acceptance hashes:
Exact commands:
Artifact paths (include raw logs and VOID attempts):
Baseline and comparison definition:
Predeclared assertions / thresholds:
Observed result, with denominators:
Status (PASS / FAIL / VOID / BLOCKED):
Validity reasons and affected intervals:
Hypotheses supported / contradicted:
Retraction, if any:
Remaining limitation:
Next highest-value action:
Reviewer / accomplishment date if gate passed:
```

## Experiment table template

| Run ID | Date | Task/gate | Source/config identity | Validity | Active RX / mode | Main measured result | Evidence path |
|---|---|---|---|---|---|---|---|
| sensing_manual_fixed.XmtvnI | 2026-09-11 | P02 / G0 | binary `b064c995...` (current), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | VOID_RF_OR_ASSERT | 4 RX, manual/passive-rx | RFSTALL (UHD ERROR_CODE_OVERFLOW) at first USRP_RX_START; 0 SIB1; no replay attempted | `/home/sens/NICOLA/captures/sensing_manual_fixed.XmtvnI` |
| sensing_manual_fixed.JPdoGb | 2026-09-11 | P02 / G0 | binary `b064c995...` (current), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE (radio-level valid) / BLOCKED (replay recorder never armed) | 4 RX, manual/passive-rx | SIB1=1, DL crc_ok 38583/42014 (91.8%), UL crc_ok 10562/12694 (83.2%), clean exit, 0 RXDISCONT/RFSTALL -- but 0 `replay`-related log lines and no `replay.bin` produced (structural, source-cited in P02 session below, not RF variance) | `/home/sens/NICOLA/captures/sensing_manual_fixed.JPdoGb` |
| sensing_manual_fixed.3NB5Ri | 2026-09-11 | P02 / G0 fix round 1 | binary `b81b21e3...` (DL sweep_ticket gate fixed), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE / still BLOCKED (UL gate not yet found) | 4 RX, manual/passive-rx | SIB1=1, DL crc_ok 91.9%, UL try=33704 crc_ok 80.9%, clean exit -- still 0 replay activity; DL gate fix alone insufficient (UL `ul_seen` never true) | `/home/sens/NICOLA/captures/sensing_manual_fixed.3NB5Ri` |
| sensing_manual_fixed.JLDoZc | 2026-09-11 | P02 / G0 fix round 1 | binary `b81b21e3...`, same conf, REPLAY=1 | VOID_RF_OR_ASSERT | 4 RX, manual/passive-rx | RFSTALL (UHD ERROR_CODE_OVERFLOW) at t=157.27s, ordinary late-run RF variance, unrelated to the recorder; used to live-verify via `/proc/<pid>/environ` that `ISAC_PASSIVE_REPLAY_CAPTURE`/`FAILURES` do reach the process | `/home/sens/NICOLA/captures/sensing_manual_fixed.JLDoZc` |
| sensing_manual_fixed.LH2I9w | 2026-09-11 | P02 / G0 diagnostic (not counted against the 2-attempt cap; temporary instrumented build, DURATION=30) | binary `5745d305...` | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE | 4 RX, manual/passive-rx | First live evidence of the second defect: `dl#1..20` show `success=1` repeatedly but `ul_seen=0` always | `/home/sens/NICOLA/captures/sensing_manual_fixed.LH2I9w` |
| sensing_manual_fixed.Ma654f | 2026-09-11 | P02 / G0 diagnostic (not counted against the 2-attempt cap; temporary instrumented build, DURATION=60) | binary `74d2b818...` | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE | 4 RX, manual/passive-rx | "first UL seen" NEVER printed despite PDSCHQ decoded=7644/crc_ok=7050 and pusch_passive try=6114/crc_ok=5010 -- proved `nr_passive_replay_ul()` was never called, root-causing the `ul_auto`-gated call site at `nr_pdcch_blind_monitor_rt.c:1738-1742` | `/home/sens/NICOLA/captures/sensing_manual_fixed.Ma654f` |
| sensing_manual_fixed.UtvBT7 | 2026-09-11 | P02 / G0 fix round 2 (final) | binary `c3810019...` (DL + UL gates both fixed, diagnostics trimmed to the two permanent lines), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE, replay PASS | 4 RX, manual/passive-rx | SIB1=1, clean exit, 0 RXDISCONT/RFSTALL. `REPLAY ARMED at absolute_slot=8852`; `REPLAY READY ... slots=320 UL=24 DL-controls=38 IQ=314572800 bytes`. Replay verification (no radio): `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened`, exit 0, wall 4.463s. **P02 PASS.** | `/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7` |

For optional UL comparisons, list every 2-RX pair, 3-RX triple and 4-RX set for DTD, DFS and joint modes. Include unobservable modes, failures and missing-data denominators. Do not place placeholder accuracy values in this ledger.

## Rules for corrections and completion

- Keep old session entries; append corrections with the contradicted claim and new evidence.
- A valid low-performance run is FAIL, not VOID. A broken measurement/time/source contract is VOID; do not interpret its accuracy metrics.
- Do not check an implementation task merely because it builds or a helper test passes.
- Distinguish existing source blocks from newly validated four-branch behavior.
- Keep optional UL completion separate from required DL pipeline completion.
- No full completion claim until all required gates have dated, reviewable evidence.
