# Adaptive four-branch passive RX pipeline — implementation and validation plan

Written: **2026-09-10**, Europe/Zurich. Status: **PLAN; receiver implementation has not started under this plan**.

Canonical location: `sens6:/home/sens/NICOLA/adaptive-rx-sensing/adaptive_RX_pipeline.md`.
Progress/evidence ledger: [adaptive_RX_pipeline_progress.md](adaptive_RX_pipeline_progress.md).
A local workspace copy of both documents is kept in `/home/sens/NICOLA/` for review.

## 1. Scope and non-negotiable contracts

Build four independently decoding passive receiver branches from four physically separated antennas on one X410. Each branch processes its own DL and UL samples, derives its own channel measurements, and runs its own DL and UL range–Doppler detection and local tracking. Add a central, AoA-free DL fusion stage after the branch contracts pass validation.

The optional final phase adds UL differential measurements and localization with unknown UE position. It remains separate from the required DL fusion deliverable. **No PMBM is included. The optional UL phase does not add track management or feed the global tracker yet.**

Rules:

- One process/component owns hardware acquisition. Four independent branch contexts consume channel-specific samples. Four modem processes must not compete for the X410.
- The receiver remains passive. No gNB changes, gNB logs as runtime inputs, scheduler hints, custom coordination, UE cooperation or external synchronization. The X410's own acquisition timestamps are allowed and must be retained.
- Each branch independently acquires the cell and recovers grants. Shared scheduling infrastructure may dispatch work, but learned decoder state, grants, payload success and channel estimates must not leak between branches.
- UL and DL remain distinct illuminator views. Do not average their complex measurements or substitute one link's range/rate for the other.
- Spatial separation replaces the compact-array model. Remove AoA, angular association and array steering. Retain surveyed receiver positions and physical timing/delay calibration.
- Keep detection score, threshold statistic, current-CPI variance, localization covariance, timing identity and any `p_real`/track confidence conceptually separate. A score is not a calibrated probability.
- Evaluate detection with range **and** Doppler. Ground truth is available only to the evaluator, never to decoding, association, clock alignment or estimation.
- **TESTING MODE RULE (added 2026-09-11, user instruction): every test, replay and OTA capture under this plan MUST use MANUAL mode only** — `pdcch_blind_monitor_full_auto = 0`. **The admissible manual profile is `tests/passive_rx/adaptive_manual_dlul.conf` launched by `tests/passive_rx/run_manual_x410.sh`** (both untracked in the tree as of 2026-09-11 01:45 Zurich, ported by a separate session; sensing enabled, `autoconf = 0`, `autodiscover = 0`, explicit CORESET/SS/BWP/TDA/DMRS/DCI layouts; see `tests/passive_rx/MANUAL_DL_UL.md`). `adaptive_manual_diag.conf` is the sensing-off variant of the same family. Never full-auto (`pdcch_blind_monitor_full_auto = 1`, `adaptive_no_hints.conf`). A run executed in full-auto mode is VOID for every gate in this plan regardless of its result. The launcher/manifest must record the effective `pdcch_blind_monitor_full_auto` value and reject `1`. The bullet below about manual grant layouts not being portable defaults is subordinate to this rule: manual mode is the required test mode; its layouts are still recorded as configuration identity, not assumed portable.
- Prefer measurement-driven acquisition and correction. Historical RF settings, CFO seeds, timing drift and manual grant layouts are not portable defaults.
- Do not broaden supported waveform/layer/UCI cases by removing rejection guards. Unsupported cases need explicit counters; they are not successful decodes.
- This document authorizes no claim of live performance. Every implementation item remains pending until its evidence gate passes.

## 2. Inspected baseline, branch and present implementation

Read-only source inspection on 2026-09-10:

| Item | Observed value |
|---|---|
| Host / repository | `sens6:/home/sens/NICOLA/adaptive-rx-sensing` |
| Current branch; planned implementation branch | `merge/adaptive-sensing` |
| Baseline commit | `50c8312fa46f17850aeee579abfb0a745f8465ff` |
| Pre-existing modified file | `tests/passive_rx/run_adaptive_receive_test.sh` |
| Pre-existing untracked file | `tests/passive_rx/aoa_track_dl.conf` |
| Build directory | `cmake_targets/ran_build/build` |
| Cached build type / generator | `RelWithDebInfo` / `Unix Makefiles` |
| Cached sensing / tests / CUDA | `ENABLE_ISAC_SENSING=ON`, `ENABLE_TESTS=ON`, `ENABLE_CHANNEL_SIM_CUDA=OFF` |
| Checks performed for this plan | Source/configuration reads only; no compilation, replay, OTA run or hardware operation |

Preserve the two pre-existing edits. Do not reset, clean, stash, change branch or overwrite them to establish a baseline. Record their content hashes/patch with the first implementation evidence. Do not infer which binary is running from the checked-out branch or build cache.

### 2.1 Present and implemented blocks

“Present” below means implemented in inspected source, not newly validated on hardware.

| Block | Existing implementation / evidence | Reuse boundary |
|---|---|---|
| Multi-channel RF input | `executables/nr-ue.c` reads channel-indexed buffers with RF timestamps and continuity checks; USRP driver has RFCHAN/RFGAIN/RFPOW diagnostics | Retain hardware owner and diagnostics; refactor delivery to branch contexts |
| Passive acquisition / control recovery | MIB/SIB1 bootstrap, blind PDCCH, common/dedicated discovery, DL assignments and UL grants | Reuse algorithms; isolate all mutable state per branch |
| Passive PDSCH decode | `nr_pdsch_passive_decode.c`, deferred queue, per-thread scratch, single-branch selection and diversity retries | Reuse single-RX decode routines; remove cross-branch retries from the new path |
| Passive PUSCH decode | `nr_pusch_passive_decode.c`, worker contexts, per-grant timing refinement, UCI search and deferred queue | Reuse supported single-layer path; scope context identities by branch |
| CFR sources | CSI-RS, blind PDSCH DM-RS, PDSCH data, PUSCH DM-RS/data, SSB hooks | Route branch-local measurements explicitly; audit support and reconstruction provenance |
| Single-antenna sensing engine | `SensingEngine(config, maximum_prb, 1)` is supported; existing parity tests instantiate it | Instantiate four independent engines rather than one four-antenna detector |
| DL/UL separation | `sensing_engine.cc` builds distinct CFR windows and runs separate detectors, clocks and local trackers | Retain separation in each branch |
| Detection DSP | Adaptive CPI, synchronization correction, allocation-family alignment/static subtraction, irregular-time likelihood, CLEAN, local thresholding and multipath collapse | Reuse algorithms and existing CPU/CUDA parity contracts |
| Local tracking | `motion_tracker.cc`: range/rate state, association, clutter/track feedback and coasting | Retain branch-local DL and UL histories |
| Reporting | JSONL/ZeroMQ, source counts, detection scores, DL covariance, drop counters | Extend schema, IDs, timing and UL covariance; remove angular fields |
| Global tracking | `hierarchical_tracker.cc` / `enu_tracker.cc` create position tracks using AoA | Replace entry/measurement model for multi-receiver DL fusion; salvage generic math only |
| Offline merging | `tests/passive_rx/merge_receivers_walltime_n.py` and `tests/sensing_sim/merge_receivers_n.py` | Diagnostic references only; not a live timestamp/association solution |

Important anchors in the inspected snapshot:

- `nr_isac.cc`: process-wide `engine` and `pipeline`; constructor requests four sensing antennas only through the AoA switch.
- `sensing_engine.cc:297`: submission UTC is generated with `system_clock::now()`, after producer/decoder delays.
- `sensing_engine.cc:584`: CPI plan uses the preceding track state; `close_ready_windows()` requires viable DL before optional UL processing.
- `sensing_engine.cc:724`: detector pipeline and local/global tracker wiring.
- `hierarchical_tracker.cc:145`: global birth requires a current or cached valid AoA.
- `nr_pdsch_passive_decode.c:920`: number of DL layers/ports cannot exceed receive-antenna count.
- `nr_pusch_passive_decode.c:628`: supported-scope guards precede the pilot tap; `:1276` gates data reconstruction on `out->uci_ack_re == 0` after acceptance.

### 2.2 Missing blocks

1. Four fully independent acquisition/control/decoder contexts. Current global discovery, queues and branch-selection controls do not provide this.
2. A hardware-channel-to-branch mapping and bounded buffer lifetime independent of branch progress.
3. A CFR ABI carrying branch, emitter/session, allocation provenance, acquisition timestamp and correction/reference metadata.
4. Branch-specific configuration, counters, resets, writer paths and worker/LDPC namespaces.
5. Four sensing engines with no cross-channel sample, support-mask, noise or history coupling.
6. AoA-free DL multi-receiver association, geometric initialization and global tracking.
7. Observability checks using the actual surveyed antenna placement, including any declared height constraint.
8. Validated throughput for four branch decoders plus eight detector views. Current CUDA implementation exists, but CUDA is off in the inspected cache.
9. End-to-end four-RX replay fixtures and OTA results. Existing helper/policy tests are not sufficient.
10. Optional UL direct/target path provenance, DTD/DFS records, same-transmission association, observability analysis and localization solver.

### 2.3 Retractions and corrections to earlier summaries

| Earlier simplification | Correct statement and implementation consequence |
|---|---|
| “Turning AoA off gives the desired independent detectors.” | It currently creates one sensing engine requesting one channel and truncates multi-channel submissions. Explicit four-engine routing is required. |
| “Four-channel decoding is already parallel branch decoding.” | Existing worker parallelism processes jobs; diversity retry stops at the first successful branch. Discovery state and queues remain shared. |
| “The decoding part can simply be retained at one antenna.” | Algorithms can be reused within supported scope. Independent one-RX DL contexts reject multilayer grants; do not promise unchanged payload coverage or reconfigure the gNB to force rank one. |
| “Pilot CFR is always available even if payload decoding fails.” | Pilot CFR does not need successful payload CRC once its path is reached, but upstream grant/waveform/layer/RV/configuration guards can prevent that path entirely. |
| “UL data reconstruction proves no UCI was transmitted.” | The current code checks accepted decode plus `uci_ack_re == 0`. This is the implemented gate, not independent proof that the transmission contains no unmodeled UCI. Preserve and audit RE reconstruction scope. |
| “Removing hierarchical tracking automatically preserves UL support.” | Scene support/coasting is wired through the hierarchical path. The standalone motion-tracker call does not currently pass that support. Extract it explicitly. Birth support presently affects global AoA births, not an independent new Stage-1 confirmation algorithm. |
| “Report UTC is acquisition UTC.” | Submission UTC includes worker latency. Air-slot fields exist, but raw hardware sample time and continuity epochs do not traverse the CFR ABI. |
| “Existing receiver merging is ready for OTA fusion.” | Scripts pair approximate wall time; some simulation modes infer/remap time using ground truth. Those paths must not enter runtime fusion or validation timing. |
| “The supplied AoA run config is a working baseline.” | Its four collinear coordinates fail the rank-two array parser. Its full-auto flag is also off, despite the no-hints comment. Do not use it as proof of sensing operation. |
| “CPI slot count in the legacy config controls this native planner.” | The inspected native parser uses `duration_bank_ms` and dwell/row limits. Do not assume legacy `cpi_slots` drives it. |
| “The README ordering is the exact implementation.” | Current-CPI variance is estimated before family alignment in `process_window`; local statistics are computed on CLEAN residual maps before component collapse and final acceptance. Preserve code behavior in parity tests. |
| “Four receivers guarantee unknown-UE UL localization without AoA.” | They do not. Count unknowns and test rank, conditioning and ambiguity before outputting a position; see the optional phase. |

### 2.4 Removals and replacements

| Item | Planned disposition | Required preservation |
|---|---|---|
| `aoa.cc/.h`, AoA attachment and steering | Remove production code, APIs and build dependencies | Single-channel phase/delay correction remains |
| AoA configuration and `AOA_ENABLE` / `AOA_UL_ENABLE` | Remove; reject obsolete keys clearly during migration | Explicit four-branch activation independent of array geometry |
| `nr_isac_aoa_antennas()`-controlled extraction | Replace all producers with branch-aware routing | Every enabled physical channel remains addressable |
| Rank-two compact-array geometry, broadside and angular calibration | Remove array-manifold requirements | Replace with individual surveyed RX positions and per-chain delay/gain metadata |
| DL/UL bearing confirmation and bearing-only EKF updates | Remove | Preserve only existing scene-level UL support without claiming target-specific association |
| AoA-dependent hierarchical/ENU tracker | Replace position initialization and measurement updates | Retain reusable numerical/bistatic helpers and local range/rate trackers |
| Passive cross-channel MRC, best-branch selection and rescue retries | Remove from the four-branch execution path | Do not break generic OAI/gNB/attached-UE combining outside this passive feature |
| Shared decode success used for all antennas' data CFR | Remove from independent mode | Each branch's payload reconstruction must cite its own accepted decode |
| AoA report fields, monitor views, configs, tests and benchmarks | Remove or rewrite affected consumers; version the schema | Preserve detector and timing tests; do not delete a failing non-AoA contract test |
| Array calibration structure | Refactor, not blanket-delete | Physical channel mapping, cable/filter delays and relevant gain calibration |

## 3. Architecture and shared contracts

### 3.1 Ownership

`AcquisitionOwner` owns UHD reads, the common sample counter and hardware continuity. It publishes immutable channel-specific sample spans. `ReceiverBranch[0..3]` owns synchronization, cell context, PDCCH discovery, decoded UE sessions, UL scheduling, DL/UL queues and decoder resources. Each branch owns a `SensingEngine` configured for one antenna. A separate fusion consumer reads published DL detections; it never supplies grants or payloads back to a branch.

Worker pools and immutable code/tables may be shared. Mutable scratch must be context-owned or lifetime-scoped. Thread-local storage alone is not sufficient when a worker later processes another branch: snapshot/restore or clear branch-dependent state at job boundaries.

Each branch owns two detection views, not necessarily two independent CPI planners. Preserve the present shared per-branch plan and DL-viability requirement initially. If DL grant recovery disappears, that branch can no longer schedule new UL receptions reliably. Full UL-only autonomous operation is out of scope.

### 3.2 Measurement schema

At minimum, carry the following from acquisition to CFR, detection and reports:

| Field group | Required contents |
|---|---|
| Run / hardware | `run_id`, physical channel, `rx_id`, sample rate, RF continuity epoch, branch lock epoch |
| Acquisition time | 64-bit first sample timestamp, absolute slot, SFN/frame, slot, OFDM symbol and fractional sample/symbol mapping; acquisition-derived CPI start/end/midpoint |
| Cell / emitter | Carrier frequency, numerology, PCI/cell key; UL RNTI plus session generation and any BWP/scrambling identity needed to distinguish an emitter |
| Resource identity | Direction, allocation key, transmission absolute slot, RB allocation, symbol span, DM-RS configuration, HARQ/RV/configuration generation where applicable |
| Measurement provenance | Pilot versus data, actual occupied/support REs, allocation membership set for a CPI, branch-local CRC result and reconstruction mode |
| Reference | Applied FFT-window shift, phase/frequency/timing corrections, selected direct/reference path, calibration version, reference validity and epoch |
| Uncertainty | Noise estimate, range/rate covariance and validity, support/dwell information, score/statistic/threshold as distinct fields |
| Health | Samples/rows/grants admitted, unsupported/rejected counts, queue drops, stale jobs, discarded intervals, invalid CPIs and resets |

RNTI alone is not globally unique. Include cell and a reassignment/session epoch. A local integer allocation counter is not comparable across receivers: construct a canonical key from the decoded transmission facts and acquisition time. Never reuse the same frame/slot number across a wrap without an epoch or unwrapped sample timeline.

Audit the UL pilot producer carefully: it currently exports allocation-wide channel-estimator output at the first DM-RS symbol. Identify which entries are direct pilot observations versus interpolated estimates. Do not label interpolated values as independent observed REs or inflate information/occupancy. Resolve this upstream contract before interpreting detector or fusion performance.

### 3.3 Validity and completion

Use distinct states: `NOT_STARTED`, `IN_PROGRESS`, `PASS`, `FAIL`, `BLOCKED`, `VOID`. A missing implementation is not `PASS`; a skipped test is not a completed gate.

- **VOID:** corrupted/misidentified samples, invalid clock mapping, stale binary, unknown source/config identity, accidental mixed transmissions, missing required calibration, abnormal receiver exit or uncontrolled loss invalidates the intended experiment. Record reasons and health diagnostics; do not interpret performance metrics.
- **FAIL on valid data:** poor decoding/detection, high localization error or solver nonconvergence with intact contracts remains a failure and must be reported. Do not relabel poor performance VOID.
- **Expected robustness faults:** deliberately injected branch dropout/outliers do not invalidate the surviving correctly identified data. Mark affected intervals unusable, retain them in availability accounting, and evaluate rejection/recovery. Full four-RX coverage cannot pass on a reduced subset.
- A gate passes only with source/config/binary identity, exact command, retained artifacts, explicit assertions and reviewable results.
- Gate thresholds must be frozen before the evaluated capture. Required exact invariants are stated below. Deployment-dependent error/latency/coverage limits belong in a versioned acceptance profile, with their physical rationale. An unset required limit means the gate is incomplete; do not invent favorable limits after viewing results.

## 4. Required implementation stages, tests and exit gates

The stages define evidence dependencies, not a requirement to delay test infrastructure. Start P20's minimal manifest/replay/launcher plumbing alongside P01 when needed; complete its four-branch behavior at G6. G5 must use a validated harness even though final sustained-operation acceptance is G6. Do not make baseline acquisition depend on a launcher that can only be built after all gates have passed.

### Stage 0 — establish a reproducible baseline (P01–P02)

**P01: Baseline manifest and acceptance profile.** Record branch, full commit, tracked patch, relevant untracked files, executable and driver hashes, CMake cache, configuration, antenna mapping, receiver geometry, compiler/backend, timestamp units and experiment purpose. Preserve pre-existing edits. Record acquisition-only, decoding-only and sensing-enabled cases separately.

**P02: Retain baseline fixtures.** Select existing valid captures or collect a bounded receiver-only OTA capture after the launcher guard is correct. Capture enough raw channel samples and metadata to replay successful and failed DL/UL grants. A failure-only replay file cannot prove normal-path equivalence or continuous CPI timing.

Tests: verify all artifact hashes; replay a known valid supported single-antenna case; deliberately mismatch binary/config manifests and require rejection; check no source of runtime ground-truth timing is used.

**G0 exit:** complete manifest and predeclared acceptance profile; at least one reproducible supported DL and UL case; invalid fixtures rejected. Historical handover CRC percentages are context, not baseline evidence.

### Stage 1 — acquisition fan-out and independent branch lifecycle (P03–P05)

**P03: Branch abstraction.** Add branch ID, physical channel mapping, per-branch lifecycle, configuration and reset state. Reuse `PHY_VARS_NR_UE` where appropriate, but separate hardware ownership from fields that currently couple receiver state to `nrue_ru_read`, retune or device restart. Audit process-wide MAC/RRC, discovery and synchronization globals reached by passive operation.

**P04: Immutable buffer delivery.** Deliver channel-specific IQ spans with sample timestamp, length and epoch. Use bounded ownership/refcounts or explicit copies. A slow branch must not read overwritten samples or hold all other branches indefinitely. Define and count drop policy per branch.

**P05: Independent digital correction/recovery.** Each branch acquires and corrects its own digital stream. Branch loss of lock increments its lock epoch, clears its stale grants/CPI history and reacquires locally. RF discontinuity increments the acquisition epoch. No branch-local correction may move the common hardware frequency or consume/discard another branch's samples.

Tests:

1. Feed four distinct deterministic sample sequences; assert byte-exact routing and channel IDs.
2. Permute physical-channel mapping and verify only the corresponding branch identity changes.
3. Stall one consumer until its budget expires; assert declared drops, no stale reads and unchanged surviving streams.
4. Inject frame wrap, sample-counter discontinuity and one-branch re-lock; assert correct epoch transitions and no old/new mixing.
5. Compare branch acquisition and digital correction against standalone replay of the same channel.

**G1 exit:** all routing/time/ownership invariants pass; no unauthorized hardware operation from branch workers; deliberate faults are isolated and visible.

### Stage 2 — independent DL/UL decoding (P06–P09)

**P06: PDCCH/configuration isolation.** Refactor mutable state in `nr_pdcch_blind_monitor_rt.c`, its configuration/discovery helpers and passive PDCCH queue into branch-owned contexts. Include learned RNTIs, candidate persistence, energy/noise gates, discovered layouts, timing and invalidation state. A branch must not acquire UL grants through another branch.

**P07: DL workers.** Refactor `nr_pdsch_passive_decode.c` and `nr_pdsch_passive_queue.c` to consume only one branch's samples and noise estimates. Disable cross-branch selection/MRC/rescue in independent mode. Data reconstruction uses the same branch's accepted transport block. Keep unsupported multilayer/PTRS/rate-matching cases explicit.

**P08: UL workers.** Refactor `nr_pusch_passive_decode.c` and `nr_pusch_passive_queue.c` so gNB receive-context scratch, timing refinement, UCI exploration, CFO snapshot and statistics belong to a branch/job. The current global context cap is six; four branches times existing consumer counts cannot simply reuse indices. Replace the allocation policy with an explicit bounded context pool. Preserve supported-scope and CRC/reconstruction guards.

**P09: Shared-resource audit.** Make decoder/re-encoder identifiers unique for concurrently active jobs across branch, direction, decoder versus reconstruction and UE/session. Audit TLS reuse, global counters/configuration and worker snapshots. Snapshot mutable configuration with queued samples. Restore reused-worker state even on errors.

Tests:

1. Replay each channel alone and then all four in parallel; for deterministic CPU execution require matching grant interpretation, CRC verdicts, rejection reasons and reconstructed symbols.
2. Delay/reorder workers; require the same results and correct time/config snapshots.
3. Make only RX2 decode a payload successfully; assert RX0/RX1/RX3 do not obtain data-aided CFR from that payload.
4. Present incompatible layouts/RNTIs to different branches; assert discovery state cannot leak.
5. Exercise failed TB CRC, segment failure, rejected/all-zero TB, UCI-rescued and unsupported waveform/layer/RV cases; verify exact admitted pilot/data paths and counters.
6. Exercise branch-local noise estimation and timing using distinct per-channel impairments.
7. Run targeted race/memory checks on replay where the OAI/backend build permits; instrument remaining shared resources if a sanitizer is unavailable. Absence of a crash is not proof of isolation.

**G2 exit:** supported DL and UL replay verdicts match standalone references; zero cross-branch grant/payload/state leakage; no resource namespace collision. Record unsupported traffic fractions separately from CRC failure.

### Stage 3 — branch-aware CFR and physical references (P10–P12)

**P10: CFR ABI and producer migration.** Replace singleton submission routing in `nr_isac.cc/.h`. Update CSI-RS, SSB, blind PDSCH DM-RS, PDSCH data and PUSCH pilot/data producers, including stubs. A one-antenna branch submits one antenna without needing an AoA switch. Carry acquisition time and identity all the way through deferred queues.

**P11: Support and reconstruction audit.** Preserve the CRB/Point-A subcarrier convention, actual OFDM symbol time, branch-specific noise and measured support. Audit channel-estimator interpolation and any per-slot/subslot grouping. Keep allocation membership for every CPI; one detection may integrate many transmissions. Do not claim symbol-level Doppler evidence from a single OFDM symbol.

**P12: Range/frequency reference contract.** Define whether reported range is excess path relative to the admitted direct path and which corrections produced it. Retain pre-normalization delay/phase diagnostics where needed; export correction values and validity. A reflected dominant path is not automatically LOS. Branches need consistent physical definitions, not identical numerical offsets.

Tests:

1. Same CFR delivered with different worker delays yields identical physical measurement times and detections; only diagnostic processing timestamps may differ.
2. Exercise SFN wrap, reorder, duplicate allocation, RNTI reassignment and epoch reset.
3. Sweep known allocation offsets/sizes and pilot patterns in deterministic fixtures; verify frequency axis, support masks and symbol timestamps.
4. Apply known common timing/CFO/SFO perturbations to direct and target paths; assert expected corrected excess range/rate and covariance behavior.
5. Fail direct-path admission deliberately; require invalid reference status rather than a confident absolute/excess range claim.

**G3 exit:** no measurement ambiguity in branch/emitter/resource/time/reference; reconstruction scope is explicit; all observed-support and correction tests pass. Fusion cannot begin before this gate.

### Stage 4 — four detectors and AoA removal (P13–P16)

**P13: Four engine instances.** Give each branch its own input pool, accumulator, CPI planner, DL/UL clocks, detector calls, clutter maps, local trackers, sequence counters and output stream. Reuse the established single-antenna algorithm. Preserve actual irregular timestamps and missing RE masks.

**P14: Removal sweep.** Execute the removal/replacement table in section 2.4. Audit `pipeline_types.h`, CPU/CUDA synchronization paths, CMake targets, public C stubs, configs, reports, monitor/plot consumers and tests for residual AoA dependencies. Preserve per-channel hardware calibration outside the deleted array-manifold representation.

**P15: UL scene support.** Extract existing scene-level support/coasting into an AoA-free branch helper. Define valid UL evidence, aging and absence explicitly. Preserve the current distinction between no observation and valid observation with no detection. Do not turn scene activity into candidate-specific confirmation. New DL global-birth support will be explicit in Stage 5 rather than silently changing Stage-1 birth rules.

**P16: Reports.** Version the schema. Publish unique branch IDs, acquisition-derived times, range/rate covariance for DL and UL, source membership, validity and health. Configure collision-free JSONL paths and one multiplexed publisher or distinct endpoints. Remove angular values rather than emitting fake zero angles.

Tests:

1. Existing single-antenna detector parity tests pass after adapting only schema/AoA-dependent assertions.
2. Four-engine replay equals four standalone runs within existing declared CPU/CUDA numerical tolerances; detection decisions must agree for non-boundary fixtures.
3. Permuting branch IDs and delaying one branch cannot alter others' maps, histories or thresholds.
4. Invalid/missing UL does not suppress valid DL; insufficient DL retains the current explicit CPI viability behavior.
5. Replay with and without valid UL scene activity verifies the intended coasting/support transitions, without fabricated target association.
6. Parser rejects removed AoA-only keys clearly; supported no-AoA configs build and run. Generic OAI paths outside the passive feature still build.

**G4 exit:** four independent branch pipelines produce eight separately identified DL/UL views when data is available; no runtime AoA dependency; no changed detector decisions from cross-branch state.

### Stage 5 — AoA-free global DL fusion (P17–P19)

**P17: Measurement model and geometry.** For surveyed gNB position `g`, receiver `r_i`, target `x`, target velocity `v`, use excess range `d_i = ||x-g|| + ||x-r_i|| - ||g-r_i||` and its time derivative for fixed transmitter/receivers. Map Doppler using the measured implementation convention. Use actual acquisition-derived measurement times, not nearest report completion times.

Survey all four receiver positions and their uncertainty. Explicitly declare 2-D/known-height or unrestricted 3-D operation. Do not assume height or a side of a planar geometry without a documented physical constraint. Compute scaled/whitened Jacobians and check ambiguity and conditioning before position birth.

**P18: Association and estimator.** Associate DL detections using shared illuminator identity, time support and joint range/rate consistency under the geometric model. Different receivers' bistatic ranges/rates need not be equal. Propagate the state to each measurement time. Use an AoA-free global tracker with explicit association/birth/update/coast behavior; no PMBM requirement. Do not fuse both a local track and its underlying detections as independent evidence.

**P19: Admission and outputs.** Reject unobservable/ambiguous births or report them as unresolved. Include contributing receiver IDs, residuals in range and Doppler, covariance validity and geometry diagnostics. Keep correlated/common errors explicit or conservative. UL may retain scene support but supplies no geometric target constraint in this required phase.

Tests:

1. Independent finite-difference checks of range/rate Jacobians and units/sign.
2. Valid measured-data replay with asynchronous CPIs, missing RX and duplicate records; no duplicate update or forced timestamp equality.
3. Multi-target crossing and incorrect pairing cases; joint range/Doppler association must reject known wrong combinations.
4. Collinear/planar/near-coincident geometry and mirror solutions; assert no confident unsupported state.
5. Compare 2/3/4-RX subsets on the same OTA evidence, reporting failures/coverage as well as errors.

**G5 exit:** predeclared position/velocity and availability criteria pass on valid OTA data; ambiguity and rank failures are detected; no ground truth or UL transmitter location enters runtime computation.

### Stage 6 — throughput, launchers and operational validity (P20–P22)

**P20: Launcher and manifest.** Implement the future launcher/interface in section 5. It must preserve branch/commit/build/driver/config/geometry identity, use bounded execution, select unique output paths and report per-branch acquisition/decode/sensing states. Global “SIB1 seen once” is insufficient for four branches.

**P21: Sustained load.** Measure worker latency, queue depth, row backlog, drop counts, processing throughput and memory for all branches concurrently. Benchmark full decoding plus sensing; isolated CUDA kernel timings do not prove real-time throughput. Keep per-branch fairness so one high-load or stalled branch cannot monopolize execution.

**P22: Receiver-only OTA release evidence.** Run a short smoke capture, then the predeclared sustained interval, then controlled branch dropout/reacquisition. Archive all attempts, including VOID captures. Do not modify RF gain/CFO seeds/decoder search limits in response to scoring alone.

Tests: real-time-rate replay stress; full four-branch OTA load; deliberate stalled worker and malformed job; bounded termination/restart; source/binary mismatch rejection; output schema/parser checks; single owner of the hardware; no RF TX calls in passive mode.

**G6 exit:** all four required branches acquire and contribute supported DL/UL data; queues remain within declared budgets without unexplained loss in baseline captures; no RF continuity errors; clean bounded exit; all mandatory stages have dated evidence. Performance is not considered done with unfilled acceptance limits.

## 5. Branch, build, test and run commands

Commands below are for **sens6**. They were inspected/documented, not executed as part of writing this plan. Existing commands and proposed interfaces are deliberately distinguished.

### 5.1 Inspect the correct checkout — available now

```bash
ssh sens6
cd /home/sens/NICOLA/adaptive-rx-sensing
git branch --show-current
git rev-parse HEAD
git status --short
git diff -- tests/passive_rx/run_adaptive_receive_test.sh
```

Expected branch at plan creation: `merge/adaptive-sensing`. Continue there when implementation begins. Recheck instructions and working changes before editing; do not switch an active worktree to manufacture the expected branch name.

### 5.2 Build existing targets — available now, not yet run for this plan

The current cache already has sensing and tests enabled. Rebuild the executable **and** the dynamically loaded driver after relevant changes:

```bash
cd /home/sens/NICOLA/adaptive-rx-sensing
cmake --build cmake_targets/ran_build/build --target nr-uesoftmodem oai_usrpdevif --parallel 4
cmake --build cmake_targets/ran_build/build --target test_nr_isac_python_parity --parallel 4
ctest --test-dir cmake_targets/ran_build/build --output-on-failure -R '^test_nr_isac_python_parity$'
```

Four build jobs are a bounded starting point, not a measured optimal setting. Keep the existing configured toolchain/options. If a cache is absent, use the repository's OAI build procedure; do not pretend a bare fresh CMake invocation reproduces it.

Existing optional offline synchronization contracts (requires a configured `OAI_SIMU` build and the prerequisites described in its README):

```bash
cmake --build cmake_targets/ran_build/build --target test_nr_pdcch_blind_monitor dfts --parallel 4
bash tests/passive_rx/offline_sync_contract/build_and_run.sh
```

These analytical tests exercise production signal-processing functions, not OTA performance. Existing CUDA parity/benchmark targets are conditional on `ENABLE_CHANNEL_SIM_CUDA`; they are unavailable in the inspected CUDA-off configuration unless the backend is explicitly configured and built. Record backend changes as a new manifest.

### 5.3 Current launcher and known blockers

Existing launcher: `tests/passive_rx/run_adaptive_receive_test.sh`. It accepts `CONF`, `DURATION`, `ATTEMPTS`, `ACQ_TIMEOUT_S`, `MRC`, `UL_BRANCH` and `RXGAIN`. Its branch guard accepts `adaptive-rx-UL-DL` and `merge/adaptive-sensing`.

**Manual mode only (see section 1 rule):** the admissible launcher is `tests/passive_rx/run_manual_x410.sh` with `adaptive_manual_dlul.conf` (`sudo -n env DURATION=120 bash tests/passive_rx/run_manual_x410.sh`; it does not take `CONF`). `run_adaptive_receive_test.sh` with `adaptive_no_hints.conf` (full-auto) is NOT admissible under this plan; `run_manual_x410.sh` currently does not arm `ISAC_PASSIVE_REPLAY_CAPTURE`, so replay fixtures (P02) need an opt-in added to it. Its `-C 3450000000 --ssb 150 --initial-fo -16480` etc. are the deployment-specific manual profile, recorded as configuration identity.

**There is currently no verified command that launches the requested four independent pipelines.**

- Default `adaptive_no_hints.conf` enables adaptive DL/UL discovery but has `sensing.enable=0`. The inspected sensing-enabled build is rejected by the launcher's config/build guard.
- `aoa_track_dl.conf` enables sensing, disables UL decoding, sets full-auto off and specifies a collinear array rejected by the AoA parser. Do not present it as a runnable sensing baseline.
- The script's fixed frequency, gain default, initial CFO and time drift describe an old receiver configuration. They are not measurements of the new separated-antenna layout.
- Existing replay capture is configured for failures; a new complete replay fixture is needed for parity and timing gates.

Do not bypass these guards, toggle features merely to obtain a `VALID` label, or claim the legacy launcher implements branch isolation.

### 5.4 Required new run interface — NOT IMPLEMENTED

Stage 6 must add `tests/passive_rx/run_four_branch_receive_test.sh`, `adaptive_four_rx.conf`, a surveyed `geometry_four_rx.json` and a versioned `acceptance_four_rx.json`. Proposed config concepts: explicit active channel list; one-antenna decoder/engine per branch; independent DL/UL source controls; per-branch resource budgets; DL fusion enable; `ul_fusion_enable=0` by default. These names are an implementation contract, not existing OAI options.

Required commands once those files and their validation exist:

```bash
cd /home/sens/NICOLA/adaptive-rx-sensing
CONF=adaptive_four_rx.conf \
GEOMETRY=tests/passive_rx/geometry_four_rx.json \
ACCEPTANCE=tests/passive_rx/acceptance_four_rx.json \
ACTIVE_RX=0,1,2,3 DL_FUSION=0 UL_FUSION=0 DURATION=120 \
bash tests/passive_rx/run_four_branch_receive_test.sh
```

After G0–G4 pass, enable the required DL fusion stage:

```bash
CONF=adaptive_four_rx.conf \
GEOMETRY=tests/passive_rx/geometry_four_rx.json \
ACCEPTANCE=tests/passive_rx/acceptance_four_rx.json \
ACTIVE_RX=0,1,2,3 DL_FUSION=1 UL_FUSION=0 DURATION=1800 \
bash tests/passive_rx/run_four_branch_receive_test.sh
```

120/1800 seconds are proposed smoke/sustained test durations, not RF tuning parameters or sufficient evidence by themselves. P01 must freeze the duration and coverage requirements appropriate to observed traffic before scoring.

Proposed aggregate contract-test target, to be created and registered in CMake:

```bash
cmake --build cmake_targets/ran_build/build --target test_nr_isac_four_branch_contracts --parallel 4
ctest --test-dir cmake_targets/ran_build/build --output-on-failure -R '^test_nr_isac_four_branch_contracts$'
```

The runner must report effective configuration, exact active channels, acquisition/lock epochs, all branch outputs, fusion mode and artifact directory. It must refuse unsupported or ignored keys. Optional modes must actually enter the intended code path; a no-op option is a failed test.

## 6. Troubleshooting bugs and stalls

Use one discriminating measurement at a time. Historical causes below are documented in code/handover files; they are not diagnoses of a new run.

### 6.1 First inspection commands — available now

Set the path printed by the launcher; do not silently select another run:

```bash
RUN_DIR=/home/sens/NICOLA/captures/REPLACE_WITH_EXACT_RUN
cat "$RUN_DIR/validity.txt" "$RUN_DIR/process_exit.txt"
grep -aE 'SIB1 common facts|RXDISCONT|RFSTALL|RFCHAN|RFGAIN|RFPOW|PUSCHQ|scanq|CRC|dropped|discarded|ULCFRIDX' "$RUN_DIR/run.log" | tail -160
pgrep -a -x nr-uesoftmodem
pgrep -a -x nr-softmodem
```

`rg` is absent on the inspected sens6 host, so these commands use `grep`. To inspect a particular running process, set its PID only after matching its executable and exact configuration path:

```bash
RX_PID=REPLACE_WITH_VERIFIED_PID
ps -L -p "$RX_PID" -o pid,tid,psr,stat,pcpu,wchan:32,comm
```

If progress is stalled, retain queue ages, latest acquisition timestamp and worker state before termination. A debugger backtrace may pause the receiver; label that capture diagnostic and do not score it as an uninterrupted performance run.

### 6.2 Symptoms, root-cause checks and bounded recovery

| Symptom | Highest-value evidence / next action | Recovery and validity rule |
|---|---|---|
| Binary older than source, unexpected behavior after a fix | Compare executable **and driver** hashes/build times with manifest and changed files | Rebuild affected targets plus modem/driver as needed; old-binary experiment is VOID for the change |
| Config/build sensing mismatch | Compare `ENABLE_ISAC_SENSING` with effective config; inspect launcher rejection | Build the intended mode or supply the intended valid config; do not remove the guard |
| AoA config rejected / sensing silently absent | Inspect rank-two parser message, enabled sources and startup confirmation | Replace obsolete config during AoA removal; current linear-array AoA config cannot establish a baseline |
| PBCH works but SIB1/control/data never acquires | Per-branch CFO/acquisition logs and SIB1 outcome; do not infer success from PBCH alone | Bound acquisition time, preserve VOID attempt, reacquire branch locally when implemented; avoid live shared-device retune |
| One branch weak/undecodable | Verify RFCHAN physical mapping, RFGAIN readback, RFPOW dBFS/clipping, branch noise and selected samples | Resolve mapping/gain delivery first; no arbitrary gain trims or normalization to hide imbalance |
| DL loses payload coverage at one antenna | Count decoded layer/DM-RS-port layouts and unsupported reasons | Report single-RX scope loss; do not force gNB rank or use another branch's payload |
| UL grant seen but CFR zero/wrong range | Inspect `ULCFRIDX`, allocation-relative channel-estimate indexing versus absolute CRB/FFT coordinates | Correct producer indexing/support before changing detection or fusion thresholds |
| UL timing wanders / CRC collapses | Verify branch-specific CFO snapshot, actual FFT-window shift and measured delay | Preserve the established UL FEP sign convention and measured `ta_new = ta - est_delay` behavior; validate with replay, not a blind TA sweep |
| UL decode changes when DL load changes | Inspect UCI hypothesis/mapping/rescue counters and reconstruction gate | Check actual UCI de-interleaving/RE mapping on retained samples; never assume CRC success makes every reconstructed RE valid |
| Queue full/stale, growing sensing backlog | Compare producer progress, consumer age, occupancy, per-stage time and job costs | Fix scheduling/ownership bottleneck and replay at real arrival rate; merely enlarging queues can increase stale reads |
| Shared worker hangs | Capture thread state and queue ownership; inspect nested pool waits, locks, TLS and context IDs | Fix lock/resource dependency; blind extra worker counts are not a diagnosis |
| RFSTALL / RXDISCONT / NIC misses | Check acquisition sample continuity and NIC counter deltas | Invalidate affected run/epochs; preserve evidence; never paper over gaps by renumbering slots |
| Run dies early but SIB1 was seen | Check exit status, assertion/core log and final sample time | Baseline launcher expects timeout exit 124; another exit is VOID unless an explicitly designed test expects it |
| No CPI / only branch 0 output | Check source admission, engine routing, minimum rows, DL viability and AoA-dependent antenna count | Fix routing/admission; do not lower limits solely to produce detections |
| Duplicate or inconsistent global tracks | Inspect acquisition time, reference epoch, allocation membership and per-RX range/Doppler residuals | Fix identity/reference/association before process-noise or threshold changes |
| X410 claimed or another modem running | Read exact process ownership; launcher lock/device-discovery output | Leave unrelated sessions alone. Do not run probes or another application against an active streamer |

For the exact process belonging to a failed diagnostic run, a bounded termination procedure is:

```bash
# Only after verifying RX_PID belongs to this run and documenting its evidence:
kill -TERM "$RX_PID"
```

Use `sudo` only if that verified process requires it. Recheck process exit and device ownership before restarting. Prefer the launcher's own timeout/cleanup. No broad `pkill`, firmware reset, NIC changes or killing another user's modem. If a device remains claimed after its owner exits, record a hardware recovery blocker; do not cycle it repeatedly while treating captures as comparable.

## 7. Progress reporting and completion checklist

Update [adaptive_RX_pipeline_progress.md](adaptive_RX_pipeline_progress.md) after each implementation/test session. Every entry needs date, task IDs, files changed, commit plus dirty-state identity, commands, artifact paths, measured outcome, PASS/FAIL/VOID status, retractions and the next highest-value action. A completion date is the date its gate passed, not the date coding started.

The checklist below is the initial snapshot. Synchronize it with the ledger when a gate changes. `—` means not accomplished. Documentation tasks are complete; implementation/OTA tasks are not.

| Check | Task | Deliverable / gate | Accomplished date |
|---|---|---|---|
| [x] | D01 | Read-only code/config/branch inventory | 2026-09-10 |
| [x] | D02 | Detailed plan and progress ledger created | 2026-09-10 |
| [ ] | P01 | Baseline manifest and frozen acceptance profile / G0 | — |
| [ ] | P02 | Valid DL/UL replay fixtures / G0 | — |
| [ ] | P03 | Branch ownership and lifecycle / G1 | — |
| [ ] | P04 | Immutable four-channel buffer delivery / G1 | — |
| [ ] | P05 | Independent acquisition/correction/recovery / G1 | — |
| [ ] | P06 | Branch-local PDCCH discovery/config/grants / G2 | — |
| [ ] | P07 | Independent DL decoding/reconstruction / G2 | — |
| [ ] | P08 | Independent UL decoding/context pool / G2 | — |
| [ ] | P09 | Decoder namespace/TLS/concurrency audit / G2 | — |
| [ ] | P10 | Branch-aware CFR ABI and all producers / G3 | — |
| [ ] | P11 | Measured support/allocation provenance audit / G3 | — |
| [ ] | P12 | Acquisition time and physical reference contract / G3 | — |
| [ ] | P13 | Four engines and independent histories / G4 | — |
| [ ] | P14 | Complete AoA removal/migration / G4 | — |
| [ ] | P15 | Preserved AoA-free UL scene support / G4 | — |
| [ ] | P16 | Versioned reports and consumers / G4 | — |
| [ ] | P17 | Surveyed DL model/Jacobians/observability / G5 | — |
| [ ] | P18 | Asynchronous DL association/global estimator / G5 | — |
| [ ] | P19 | DL admission, uncertainty and OTA comparison / G5 | — |
| [ ] | P20 | Four-branch launcher and manifests / G6 | — |
| [ ] | P21 | Sustained concurrent throughput and isolation / G6 | — |
| [ ] | P22 | Required pipeline OTA release evidence / G6 | — |
| [ ] | U00 | Optional UL-off compatibility contract | — |
| [ ] | U01 | Four-channel UL acquisition / UG1 | — |
| [ ] | U02 | Independent UL detections/reference metadata / UG2 | — |
| [ ] | U03 | DTD/DFS extraction and pre-solver OTA logs / UG3 | — |
| [ ] | U04 | Same-transmission association / UG4 | — |
| [ ] | U05 | Model, Jacobians and actual geometry audit / UG5 | — |
| [ ] | U06 | Observable WNLS/sliding-window solver | — |
| [ ] | U07 | 2/3/4-RX and DTD/DFS/joint OTA comparison / UG6 | — |
| [ ] | U08 | Motion/multipath robustness / UG7 | — |
| [ ] | U09 | Optional-module deliverables and limitations report | — |

Required pipeline completion means G0–G6 pass. Optional UL completion additionally requires U00 and UG1–UG7, with unsupported/unobservable modes correctly rejected and reported. A successful rejection test is not evidence that localization is possible in that mode.

## 8. Optional final phase — OTA multi-receiver UL fusion without AoA

### 8.1 Reference, objective and limits

Liehu Wu et al., “Moving target localization in passive distributed MIMO radar systems with unknown transmitter positions,” *Signal Processing*, Vol. 239, 110265, 2026. [DOI / publisher reference](https://doi.org/10.1016/j.sigpro.2025.110265).

The publisher's abstract/introduction describe combining AoA, differential direct-versus-scattered path delay and frequency measurements to estimate moving targets and unknown transmitters. We use the DTD/DFS concept only. The paper's AoA-assisted estimator and its identifiability/performance results do not establish those properties for this AoA-free four-RX design. [Publisher abstract and introduction](https://www.sciencedirect.com/science/article/pii/S0165168425003792)

The equations, state counting and proposed checks below are our AoA-free design derived from path-length geometry, not a reproduction of the paper's estimator. The paper's full estimator equations were not needed or adopted for this plan.

OTA is the primary validation path. Reuse the existing OAI passive receiver and UL detector. Do not start a new Sionna pipeline. Analytical fixtures or Sionna-generated measurements are permitted only to isolate an identified problem such as sign, Jacobian, rank or convergence. No PMBM, no new UL track manager, no global-tracker integration in this optional phase, and no detector tuning to improve localization metrics.

### 8.2 U00 — feature-off compatibility

The compatibility baseline is the completed four-branch, AoA-free required pipeline with optional UL fusion absent. This does not require preserving the old AoA implementation removed in Stage 4.

Add a runtime flag, proposed name `ul_fusion_enable`, default false. A disabled module must not allocate fusion workspaces, subscribe to branch streams, change source admission/decoder decisions, modify CPIs, alter detector thresholds or change local/DL global tracks. Optional reference logging has a separate explicit mode; it is off by default too.

Use read-only copies of admitted measurements. Bound optional work and report its own drops. It must not backpressure hardware acquisition or branch decoding. Include versioned configuration for mode (`DTD`, `DFS`, `DTD_DFS`), geometry, state dimension, time window and uncertainty/admission policy.

**Test/gate:** replay identical input before and after linking the module, with it disabled; numerical baseline detections/tracks and validity counters agree (excluding nondeterministic processing-duration telemetry). No optional queue/workers are active. Enabling logging only changes optional records and measured overhead; base decisions remain identical. OTA stress confirms base real-time budgets remain met.

### 8.3 U01–U02 — independent UL detections and retained paths

First pass G1–G4. Verify simultaneous independent reception on RX0–RX3. For each detected component retain:

```text
run_id, rx_id, physical_channel, acquisition_epoch, branch_lock_epoch
oai_sample_timestamp, sample_rate, absolute_slot, frame, slot, symbol
cell_key, carrier_hz, rnti, ue_session_id
ul_allocation_id, cpi_id, cpi_start/end/midpoint, allocation_membership
tau_direct_s, tau_target_s, fd_direct_hz, fd_target_hz
direct_reference_valid, reference_epoch, correction/calibration_metadata
score, decision_statistic, threshold, measurement_covariance, validity_reason
```

The delay/frequency values need a **common local coordinate system**; absolute over-the-air transmit time is not presumed known. Retain raw/local values plus applied corrections so target and direct paths are compared consistently. The direct path may be removed by static subtraction or phase normalization in today's detector. Capture its reference estimate before those operations; do not try to recover it from a final cleaned map or hard-code it to zero.

Audit whether the existing selected synchronization peak is actually the direct UL path. Multipath can make another peak stronger. Admit a direct reference using measured delay continuity, support, quality and competing-path ambiguity; report invalid when evidence is insufficient. No oracle UE position is used to choose it.

A Doppler detection integrates multiple symbols/transmissions. Carry the entire supporting allocation set and effective measurement epoch. A scalar allocation ID must not pretend an entire CPI came from one PUSCH. Per-branch CPI IDs are local and cannot establish cross-RX simultaneity.

**UG1 — four-channel acquisition:** all four channels continuously yield correctly identified/timestamped UL sample intervals during the declared capture. Audit mapping, gaps, common acquisition epoch and local lock state. Baseline acquisition loss is zero; intentional dropout belongs to UG7.

**UG2 — independent detections:** on valid OTA data, report independent target detections from multiple receivers with no shared payload/grant outcome. Log availability per RX and the intersection supporting four-RX comparisons. Meet the predeclared matched-event coverage; do not demand every receiver detect every target in every CPI or hide misses. Ground truth labels detections only in the evaluator.

### 8.4 U03 — differential UL measurements and mandatory pre-solver OTA logging

For receiver `i`, define range-valued DTD and frequency-valued DFS:

\[
d_i = \mathrm{DTD}_i = c(\tau_{\mathrm{target},i}-\tau_{\mathrm{direct},i}),
\qquad
\mathrm{DFS}_i = f_{\mathrm{target},i}-f_{\mathrm{direct},i}.
\]

The time difference itself is in seconds; `DTD_m` is its multiplication by `c`. Never mix those units. Use the actual UL carrier frequency for Doppler-to-rate conversion.

Under the common received-frequency convention, increasing path length gives negative frequency shift, so `DFS = -(f_c/c) * d_dot`. **Treat this sign as an experimentally tested contract**, since FFT/rotation/report conventions can invert it. Read the current detector axis/sign and verify known motion using OTA geometry whose ground truth stays in the evaluator. Where a reported UL range/rate is already direct-referenced, map it once; do not subtract the direct contribution twice.

Construct a joint covariance from the direct and target estimates. For `q = [tau_t, tau_d, f_t, f_d]`, use

\[
A=\begin{bmatrix}c&-c&0&0\\0&0&1&-1\end{bmatrix},
\quad z=Aq,\quad R_z=A R_q A^T.
\]

Include within-RX direct/target correlation, shared calibration error and cross-RX common errors where known. Do not treat the direct reference as noise-free. Unknown covariance must yield a stated conservative policy or invalid uncertainty, not a zero covariance matrix.

Before any localization solver, collect OTA JSONL containing all four receivers' raw/local direct and target delays/frequencies, DTD, DFS, score, covariance, timestamp, RNTI/session, allocation support and reference validity. Produce per-RX time series and a missing/outlier census. Check direct-path stability, DTD/DFS temporal consistency, receiver differences, phase wraps/aliasing, drift and ambiguous paths. DTD/DFS are generally different across receivers; consistency does not mean numerical equality.

**UG3 — direct-path referencing / first mandatory measurement-quality gate:** declared stable and moving target captures pass timing/sign/unit tests; common clock offsets cancel as modeled; jumps correspond to recorded reference changes or are rejected; missing/ambiguous direct paths are excluded with reasons. Freeze the measured covariance/quality policy on calibration data, then validate it on separate data. Retain raw and differential logs. **No solver work proceeds on unvalidated OTA differential measurements**, apart from minimal analytical checks needed to diagnose them.

### 8.5 U04 — same-transmission cross-receiver association

Eligibility requires the same cell, RNTI/session, canonical UL allocation/transmission key, compatible sample/frame/slot/symbol timing, common acquisition epoch, and identical or overlapping CPI support with an explicitly recorded intersection. Never merge different emissions merely because their RNTI, range or report UTC looks similar.

Two independent CPIs may contain different allocation sets. Fuse only the supported common transmission set, or construct a documented common-support measurement in the optional module and validate its estimator without changing the base detector. Otherwise reject the pairing. Account for measurement-time differences and temporal correlation; no fabricated exact timestamps and no duplicate reuse as independent evidence.

After hard identity gates, associate target components using joint DTD/DFS feasibility under geometry and uncertainty. Values are not expected to match across RX. Retain alternatives when target association is ambiguous; do not force one correspondence to obtain a solution. A bounded per-window hypothesis set is allowed; a persistent UL track manager is not.

**UG4 — association:** adversarial replay of simultaneous UEs, RNTI reuse, same-RB different slots, wrap/relock, partial CPI overlap, duplicate packets and multiple target components yields zero identity-incompatible associations. OTA logs expose every participating allocation/support intersection and rejection reason. Target-association accuracy and ambiguity/coverage are scored separately on evaluator labels; thresholds are frozen before scoring.

### 8.6 U05 — AoA-free geometry, derivatives and observability

Let receiver positions `r_i` be surveyed and stationary, UE/transmitter position/velocity be `t, v_t`, and target position/velocity be `x, v_x`:

\[
d_i=\|x-t\|+\|x-r_i\|-\|t-r_i\|.
\]

With unit vectors `u_xt=(x-t)/||x-t||`, `u_xi=(x-r_i)/||x-r_i||`, `u_ti=(t-r_i)/||t-r_i||`:

\[
\dot d_i=(u_{xt}+u_{xi})^T v_x+(-u_{xt}-u_{ti})^T v_t.
\]

No UE location or stationary-UE assumption is inserted silently. If the receiver itself moves, extend the model with its known/measured velocity; that is outside the fixed-X410 antenna setup.

For state `xi=[x,t,v_x,v_t]`, define `a_i=u_xt+u_xi`, `b_i=-u_xt-u_ti` and `P(y)=(I-u_y u_y^T)/||y||`. Jacobian rows are:

\[
\nabla_\xi d_i=[a_i^T,\ b_i^T,\ 0,\ 0],
\]

\[
\nabla_x\dot d_i=P(x-t)(v_x-v_t)+P(x-r_i)v_x,
\]

\[
\nabla_t\dot d_i=-P(x-t)(v_x-v_t)-P(t-r_i)v_t,
\quad \nabla_{v_x}\dot d_i=a_i,\quad \nabla_{v_t}\dot d_i=b_i.
\]

Multiply the range-rate Jacobian by the verified Doppler scale/sign for DFS. Guard coincident points where unit vectors are undefined. Check analytical derivatives against independent centered finite differences over multiple physical scales and nondegenerate states; declare absolute/relative tolerances and step-size stability before acceptance.

**Necessary instantaneous state counts, before any constraints:**

| Mode | Four-RX observations | Unknowns | Consequence |
|---|---|---|---|
| 3-D DTD, unknown target and UE positions | 4 | 6 positions | Underdetermined; velocity is not estimated |
| 3-D DFS, both positions and velocities unknown | 4 | 12 | Underdetermined |
| 3-D DTD+DFS, moving UE and target | 8 | 12 | Underdetermined even with perfect measurements |
| 3-D DTD+DFS, explicitly stationary but unknown UE | 8 | 9 (`x,t,v_x`) | Still underdetermined |
| 2-D DTD+DFS with justified height constraints for both objects | 8 | 8 | Count permits a solution; rank, mirror ambiguity and conditioning still decide |

Counts do not prove uniqueness. Finite residuals and solver convergence do not establish observability. Surveyed receiver positions and their uncertainty are required inputs; they were **not supplied/verified for the separated arrangement when this plan was written**.

For actual OTA placement, save raw and state-scaled/measurement-whitened Jacobians, singular values, rank tolerance, numerical rank, nullspace directions and condition number. Report infinite full-state condition number if rank deficient. Evaluate plausible states over the surveyed operating region and at accepted solutions; do not use hidden UE ground truth to select a favorable rank result. Analyze planar reflection ambiguities and weak geometric diversity. Uncertain geometry contributes to covariance or explicit nuisance parameters.

**UG5 — geometry:** reproducible actual-placement artifact plus equation/Jacobian tests, necessary state counts, singular spectra and ambiguity analysis. Clearly classify each 2/3/4-RX and DTD/DFS/joint mode as observable, underdetermined, ill-conditioned or conditionally observable under stated constraints. A deficient mode passes this gate only as a correctly diagnosed limitation, not as a localization success.

### 8.7 U06 — weighted nonlinear solver and short window

After UG1–UG5, implement weighted nonlinear least squares for eligible measurements:

\[
\hat\xi=\arg\min_\xi (z-h(\xi))^T R^{-1}(z-h(\xi)).
\]

This equals a sum of per-RX quadratic residuals when the block-independence assumption is justified. Otherwise use the joint covariance or an explicitly conservative approximation. Support DTD-only, DFS-only and DTD+DFS; unsupported identifiability must produce a status, not a fabricated state.

Use stable linear solves/whitening rather than explicitly inverting poorly conditioned matrices. Use multiple initializations derived from the allowed surveyed region and observations, not truth. Report competing acceptable minima. Robust weighting or outlier rejection must use fixed measurement diagnostics/residual rules; retain both raw and robust residuals and recheck rank after rejecting an RX.

For instantaneous underdetermination, demonstrate the nullspace/ambiguity and then evaluate a short sliding window:

\[
x(s)=x_0+v_x(s-s_0),\qquad t(s)=t_0+v_t(s-s_0).
\]

The 3-D moving-UE state has 12 unknowns across the window. More epochs can add constraints, but repeated stationary/degenerate observations may not add rank. Recompute the window Jacobian; never assume a constant-velocity constraint guarantees identifiability. Keep actual per-measurement times and account for overlapping-CPI correlation. Reject windows with acceleration/model mismatch rather than shrinking covariance to fit them. Do not silently pin UE motion to zero.

Return at least:

```text
status, mode, state_dimension, assumptions, time/window
target_state (nullable), transmitter_nuisance_state (nullable)
solver_residual, per_rx_DTD_DFS_residuals, robust_weights
rx_ids, contributing_rx_count, allocation_support
rank, singular_values, scaled_condition_number, ambiguity_status
covariance (nullable), covariance_valid, uncertainty_method
iterations, convergence_status, rejection_reason
```

Statuses include `VALID`, `INSUFFICIENT_RX`, `INVALID_REFERENCE`, `ASSOCIATION_AMBIGUOUS`, `UNOBSERVABLE`, `ILL_CONDITIONED`, `MULTIPLE_SOLUTIONS` and `NO_CONVERGENCE`. A pseudoinverse's finite entries are not a full-state covariance in a rank-deficient problem. “Confidence” must identify its calibration; do not invent a probability from residual magnitude.

Tests: finite-difference Jacobians; dimensional/unit/sign consistency; exact nondegenerate fixtures; known nullspaces/mirror cases; multi-start ambiguity; missing/direct-path-corrupted inputs; covariance whitening; window-time perturbation; overlapping-window correlation; robust rejection losing sufficient rank. These are targeted analytical checks, not a new simulation campaign.

### 8.8 U07–U08 — OTA localization and robustness gates

**UG6 — localization:** use independently known target positions and velocities where measured, aligned to the same acquisition time without feeding ground truth into the estimator. Unknown UE state remains unknown to the solver. Use a disjoint calibration/development capture and evaluation capture; freeze detector, association, solver and acceptance settings before evaluation.

Evaluate every relevant receiver subset: six 2-RX pairs, four 3-RX triples, and the 4-RX set, each with DTD, DFS and joint modes. If a mode is unobservable, report that outcome and coverage instead of manufacturing position-error numbers. Use the same raw four-RX capture and common eligible-event set for paired comparisons, plus a separate operational availability analysis including all misses. Never cherry-pick the best subset without showing all subset results.

Report target position error (median, RMSE, p95), velocity error if estimated, range/DTD and Doppler/DFS residual distributions, convergence/valid-solution rates with denominators, ambiguity/rejection rates, contributing RX count, latency and covariance coverage where estimable. Bootstrap or other uncertainty estimates must respect temporal correlation. Missing/nonconvergent events remain in availability denominators. Meet the acceptance profile frozen at UG3/UG5; otherwise record FAIL or a demonstrated observability limitation.

**UG7 — robustness:** repeat with moving targets, realistic multipath, temporary path blockage, unequal branch SNR, branch dropout and reacquisition. Label corrupted measurements using independent diagnostic/evaluation evidence. Compare fixed ordinary weighting and the declared robust method on identical observations. Show whether rejection prevents large errors, how much coverage is lost and whether remaining geometry stays observable. Report catastrophic errors against a predeclared application bound; do not choose that bound from the worst result.

Changing the base detector during these comparisons creates a new experiment and invalidates paired attribution. Do not tune it purely to make fusion look better. A valid poor result remains a reported failure.

### 8.9 Optional module files, commands and final deliverables (U09)

Proposed new files, names to be finalized during implementation and then reflected in this document:

| Area | Proposed files / changes |
|---|---|
| Reference export | Extend branch CFR/detection metadata and UL report writer; no base-detector threshold changes |
| Extraction | `openair1/PHY/NR_UE_ISAC/ul_differential_measurement.{h,cc}` |
| Association | `openair1/PHY/NR_UE_ISAC/ul_cross_rx_association.{h,cc}` |
| Model / observability | `openair1/PHY/NR_UE_ISAC/ul_fusion_model.{h,cc}` |
| Solver | `openair1/PHY/NR_UE_ISAC/ul_fusion_solver.{h,cc}` |
| Optional orchestration | `openair1/PHY/NR_UE_ISAC/ul_fusion.{h,cc}` |
| Tests | Dedicated extraction, association, model/rank and solver tests plus disabled-mode replay parity |
| OTA reporting | `tests/passive_rx/ul_fusion/` manifest/schema validators and evaluation tools |
| Documentation | This plan, progress ledger and linked raw/summary artifact manifests |

Proposed run modes below are **NOT IMPLEMENTED**. Stage 6 / U00 must implement and validate them before use:

```bash
# Differential logging only: no localization, mandatory before solver work.
CONF=adaptive_four_rx.conf GEOMETRY=tests/passive_rx/geometry_four_rx.json \
ACCEPTANCE=tests/passive_rx/acceptance_four_rx.json \
ACTIVE_RX=0,1,2,3 DL_FUSION=0 UL_FUSION=0 UL_REFERENCE_LOG=1 DURATION=1800 \
bash tests/passive_rx/run_four_branch_receive_test.sh

# Only after UG1–UG5: optional joint localization, no UL track management.
CONF=adaptive_four_rx.conf GEOMETRY=tests/passive_rx/geometry_four_rx.json \
ACCEPTANCE=tests/passive_rx/acceptance_four_rx.json \
ACTIVE_RX=0,1,2,3 DL_FUSION=0 UL_FUSION=1 UL_FUSION_MODE=DTD_DFS \
UL_REFERENCE_LOG=1 DURATION=1800 \
bash tests/passive_rx/run_four_branch_receive_test.sh
```

For mode comparisons, change only the fusion mode on retained identical OTA measurements wherever possible; separate live runs have different propagation/traffic and are not paired evidence. `UL_FUSION_MODE=DTD` and `DFS` must execute real reduced-measurement models or report unobservability.

Final optional-module deliverables:

1. Exact modified/new/removed file list, branch, commit and dirty patch.
2. Four-RX independent UL detection evidence, mapping, timestamps and supported scope.
3. DTD/DFS extraction with raw direct/target values, sign/units and covariance tests.
4. Cross-RX association and allocation-support audit, including rejects/ambiguities.
5. AoA-free measurement model and checked Jacobians.
6. Actual-placement observability report with state counts, singular values, rank, condition and ambiguities.
7. Optional WNLS and, when justified, short-window solver; feature-off parity evidence.
8. OTA 2/3/4-RX results for all declared subsets.
9. DTD versus DFS versus joint comparisons, including unobservable modes and coverage.
10. Remaining limits before global-tracker integration: unknown UE motion, direct-path ambiguity, multipath, geometry, timing/reference errors, association errors, covariance calibration, load and supported UL traffic. No PMBM or new track-management claim.

Update the dated checklist and ledger with actual evidence for every item. Until then, the optional module is a specification, not an implemented feature.
