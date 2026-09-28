# Cloud session prompt — UE localisation (offline) + offline validation of open lanes

Paste everything below the line into the cloud session.

---

Repository: `github.com/nico-net/passive_agnostic_rx_for_sensing`, an OpenAirInterface fork. It is the OAI NR UE
repurposed as a **passive, fully blind ("agnostic") 5G receiver** that never attaches and never transmits. Start
from branch **`cloud/ue-localization`** (= `sdd/integration` @ 239eb144ac + `docs/cloud/`). Read first:
`docs/cloud/UE_LOCALIZATION_SPEC.md` (design), `docs/cloud/HANDOVER_AGNOSTICITY.md` §1–§2 (rules, gates),
`docs/cloud/full-running-agnosticity/` (lane reports).

## Scope: OFFLINE ONLY

- Build (cmake/ninja, `-DENABLE_TESTS=ON`), unit tests, gtests, offline link-level simulation (`nr_ulsim` /
  `nr_dlsim` and the tree's own test harnesses). **No live beds**: do not start `nr-softmodem`/`nr-uesoftmodem`,
  rfsim, OCUDU, srsUE or a core. Live gate G4 is left for the lab. Write "G4 pending (lab)" and never claim live
  results.
- **No sensing:** do not use or modify `openair1/PHY/NR_UE_ISAC/` (range-Doppler, CFAR, tracker, `isac_aoa`), and do
  not implement spec §4 (UE-illuminated sensing).
- Out of scope: nsa/prg/cbg lanes, X410/OTA, §4.4 matrix, merging into `adaptive-rx-UL-DL` or `sdd/integration`
  (merge requires G4).

Priority: **Part A (UE localisation) is the main deliverable** (~75 % of the effort). Parts B and C are bounded.

## Part A — UE localisation, implement + validate offline

Geometry: one receiver (one X410, 4-element λ/2 ULA along a known axis) at a surveyed position; one gNB at a
known position; UEs at unknown positions, height prior 1.5 m. The UE's PUSCH gives a bistatic timing constraint
(a hyperbola with foci gNB and receiver) and a bearing (a ray); the fix is their intersection. The overheard TA
serves as a consistency check.

**Reuse what exists; verify before assuming:**
- `nr_passive_mac_ta.{h,c}` (+ `tests/nr_passive_mac_ta_test.cc`): pure parsers for the RAR 12-bit T_A (absolute)
  and the TA-Command MAC CE (LCID 0x3D, 6-bit relative), plus `nr_passive_mac_ta_metres()`.
  `nr_passive_mac_report_ta()` in `nr_pdsch_passive_queue.c` is where they fire on CRC-verified TBs.
- `nr_pusch_passive_decode.c`: per-grant `pvp->delay.est_delay` (integer CIR-peak delay) and the window re-placement
  (`ul_sample_offset - d`). The comment there records a ~3.3 ppm sawtooth drift of the raw residual delay. The
  per-antenna DM-RS estimates are in `ul_ch_estimates[nl*num_sp + ant]`. The comment says ABSOLUTE subcarrier
  index, and an older note says relative: verify with a test before relying on either. The UL decode runs off the
  RT thread (`nr_pusch_passive_queue.c`); keep all new work there or later.

**TOGGLE CONTRACT (hard requirement).** The operator will first test the passive agnostic receiver ALONE, so UE
localisation must be a single runtime flag, **OFF by default everywhere**:
- One config key (`ue_loc_enable = 0|1` in the passive receiver's config section) with an env override
  `ISAC_UE_LOC=0|1`. No conf file or runner script in the repo turns it on.
- When OFF: no allocation, no thread, no queue, no log line, no extra computation. The existing decode path
  (including the `est_delay` window re-placement) is byte-for-byte unchanged. Prove it with (a) a test that runs
  the hook with the flag off and asserts every localisation counter stays 0 and no measurement is copied, and
  (b) the full existing ctest suite green with the flag off.
- When ON: purely additive. It reads measurements the receiver already produced, never changes a decode
  decision, a window position, an RNTI table or a CRC outcome. It is only a consumer, and a failure inside it
  (bad geometry, NaN, full queue) is logged once and dropped, never propagated.
- Startup prints one line stating the flag's state, so a capture's log shows which mode it ran in.

**Known problems the implementation must handle, test, or explicitly flag (do not discover them in the lab):**
1. **N_TA_offset is unknown blind** (FR1: 0 / 25600 / 39936 T_c). 25600 T_c ≈ 13 µs ≈ 3.9 km of path, so a
   wrong hypothesis is a km-scale error, not a small bias. Hypotheses must stay explicit until TA-consistency
   resolves them; otherwise output "ambiguous", not a fix.
2. **UE timing is not exact.** The UE's own transmit-timing error and the gNB's TA control loop let UL timing
   wander by samples to tens of samples between TA commands. The error on Δd is set by that loop, not by our
   estimator. Model it as a noise term, and report expected accuracy with it, not only the estimator's CRB.
3. **TA granularity** at 30 kHz: 16·64/2 T_c ≈ 0.26 µs ≈ 39 m one-way per step. TA is a coarse check only.
4. **Receiver DL reference**: the DL frame timing the receiver tracks is its sync point, which may be the strongest
   path, not the first. d(gNB,rx) must come from surveyed positions, and any sync offset to the first path must
   be measured (DL CIR), not assumed. The receiver's clock drift (~3.3 ppm sawtooth) cancels only if UL and DL
   references come from the same slot.
5. **Multipath / NLOS**: a reflected UL path biases both Δd (late) and θ. One receiver has no redundancy to detect
   it; use the first-path estimate, not the peak, and flag low K-factor / spread CIRs.
6. **Bearing limits**: the 4-element λ/2 ULA has ~25° beamwidth; mirror ambiguity about the array axis;
   azimuth/elevation coupling (height prior); per-channel phase calibration drifts (re-calibrate continuously
   from the DL direct path). On the X410 two branches are measured 8–15 dB weaker (physical), so per-channel SNR
   must weight the estimator. A per-branch frequency offset rotates inter-antenna phase between calibration and
   use: estimate or bound it.
7. **UE identity / linkage**: RNTIs change on re-attach; in NSA CFRA the C-RNTI is never seen in plaintext and
   the RAR link is heuristic (T4). Many NSA UEs send UL data over LTE, so NR PUSCH may be rare (PUCCH is not
   decoded): expect sparse fixes, and handle long gaps in the tracker.
8. **Geometry dilution**: near the extension of the gNB–receiver baseline the hyperbola degenerates, and far
   away the cross-range error grows linearly with distance (1° ≈ 1.7 m at 100 m). The covariance must show it;
   no fix is emitted when the covariance exceeds a bound.
9. **UL SNR**: UEs power-control toward the gNB, not the receiver, so far UEs may be too weak for bearing even
   when timing works. Degrade gracefully (timing + prior, larger covariance).
10. **Resources**: the extra work runs per PUSCH. Keep it off the RT thread, bounded (drop when the queue is full,
    count drops), and measure its CPU cost in the report.
11. **Privacy**: localising third-party UEs is personal data. Pseudonymised RNTIs only, no raw identities in logs,
    and the lab validates only with its own UEs.
12. **Validation honesty**: link-level simulation is an upper bound; items 2, 4, 5, 7 only show up live/OTA. List
    them in `UE_LOC_IMPLEMENTATION.md` as what the lab must measure.

**Tasks (TDD for each: failing test first, record RED→GREEN in the report).** New pure-C modules in
`openair1/PHY/NR_UE_TRANSPORT/` with no PHY state, so they are unit-testable. Hook only in T6, opt-in, default
bit-identical.

- **T1 `nr_ue_loc_geom`** — 3-D bistatic geometry with a fixed target height: Δd = |UE−rx| − |UE−gNB|; closed-form
  ray ∩ hyperbola (ray from the bearing at the prior height). Return 0/1/2 roots, choose physically (in front of
  the array, Δd consistent), plus a 2×2 covariance by Jacobian from σ_Δd and σ_θ. Tests: brute-force grid agreement
  < 1 cm; degenerate cases (no intersection, UE on the gNB–rx baseline, endfire bearing, ray parallel to an
  asymptote).
- **T2 `nr_ue_loc_timing`** — UL arrival → Δd. Model: the UE transmits at its DL reception − (N_TA + N_TA_offset)·T_c,
  so UL_arrival − DL_frame_ref(rx) = [d(UE,rx) − d(UE,gNB) − d(gNB,rx)]/c − N_TA_offset·T_c + TA error.
  - (a) Integer part from the window offset + `est_delay`; fractional part from the DM-RS phase slope across
    subcarriers (or CIR-peak interpolation), per antenna then combined.
  - (b) The reference is the receiver's OWN DL frame timing in the SAME slot, so receiver clock drift cancels.
    Prove it with a test that injects the recorded sawtooth drift on both.
  - (c) N_TA_offset is unknown blind (FR1: 0 / 25600 / 39936 T_c): carry it as hypotheses, resolved by
    TA-consistency (T4) or left explicitly ambiguous. Never read it from a gNB config.
  - Tests: synthetic DM-RS channels with fractional delay + AWGN → |error| < 0.1 sample at a stated SNR, and a
    variance within 2× of the CRB.
- **T3 `nr_ue_loc_bearing`** — per-antenna phase from the DM-RS estimates (coherent average over REs, with the
  SFO/delay phase ramp removed first). Interferometry for 2 elements, beamscan + parabolic refinement for ≥3. Scan
  restricted to the half-plane the linear array faces. CRB σ (it grows at endfire). **Self-calibration**: per-channel
  phase offsets from the DL direct path, whose bearing is known from the surveyed gNB position (use the DL
  SSB/PBCH DM-RS channel estimate). Tests: synthetic steering vectors + per-channel phase errors {0, 0.9, −1.7,
  2.4} rad → bearing error > 5° uncalibrated, within 2σ_CRB calibrated; mirror/endfire behaviour; SNR sweep vs CRB.
- **T4 `nr_ue_loc_ta`** — per-RNTI TA state built on the existing parsers: absolute T_A from a RAR, plus
  accumulated MAC-CE deltas (applied at the standard activation delay; TAG id respected). Per-RNTI epochs: reset
  when a new RAR/RNTI appears, expire after silence.
  - **RAR→RNTI linkage** is the hard part. In CBRA the RAR gives the TC-RNTI. In CFRA (NSA SCG addition) the UE
    keeps a C-RNTI we never see in plaintext: link it to the RNTI whose PUSCH arrives on the RAR's UL-grant slot.
    Test both.
  - "No absolute TA yet" is a first-class state. The TA-consistency check compares the fix's predicted
    d(UE,gNB) against the TA range (± TA granularity + margin) and flags outliers; it also resolves N_TA_offset
    in T2.
- **T5 `nr_ue_loc`** — per-RNTI localizer: measurement fusion (T1–T4) + a constant-velocity Kalman filter.
  - Emits one JSON line per fix: pseudonymised RNTI (salted hash, salt per run), time, x/y, covariance, Δd, θ,
    TA-consistency flag, N_TA_offset hypothesis. No raw RNTI, no identities.
  - Tests: synthetic trajectories (walking, driving, static) with noise → RMSE within 1.5× of the CRB-predicted
    value; TA outlier flagged; bearing-absent grants degrade to prior + timing without crashing.
- **T6 integration hook (opt-in)** — config key or env `ISAC_UE_LOC=1`; default off, bit-identical. In the PUSCH
  passive path after `nr_rx_pusch*` (DM-RS estimate + delay are available; timing does not need CRC, but only
  CRC-passed grants may create or update an RNTI track) copy a small measurement struct and hand it to a consumer.
  Same for the DL direct-path calibration sample. Tests: existing ctest suites unchanged with the knob off; a
  linked test that drives the hook with a synthetic slot.
- **T7 end-to-end offline validation** — extend/wrap `nr_ulsim` (or a dedicated test) to transmit a real PUSCH
  through a channel with a known fractional delay and per-antenna steering phases (4 RX), decode it with the
  production receive functions, and run T2–T5 → recovered Δd and θ vs truth.
  - Pre-register thresholds in `docs/cloud/CLOUD_REPORT.md` BEFORE running, e.g. |Δd error| < 1 m at 100 MHz and
    high SNR, and θ within 2σ_CRB.
  - Then an SNR sweep, reported as a table.
  - State clearly that link-level simulation is an upper bound, not an OTA result.

Deliver on branch `sdd/ue-loc` (from `cloud/ue-localization`), pushed to `github`, with a short design note
`docs/cloud/UE_LOC_IMPLEMENTATION.md` (what was built, where it hooks, measured offline accuracy, what the lab must
run for G4: phy-test/rfsim with an injected delay, then OTA with GNSS-surveyed own phones).

## Part B — open lanes, offline only (bounded)

- **ssb** — `wip/2026-09-28/gap-ssb` (SSB rate-match detector + production-path test). G1 build, G2 full ctest +
  `--gtest_shuffle` (known skips: blind_monitor 2, config_sweep 1), G3 RED evidence for the production-path test
  (break the integration → the test fails), G5 with a fresh independent reviewer subagent, fix
  Critical/Important. Commit onto `sdd/gap-ssb`, push. G4 pending (lab).
- **csirs** — `sdd/gap-csirs` @ 621a91dabe. The lab found that every run exports one false ZP resource in
  symbol 13, on REs that are dark only because CSI-RS ports 1–7 never reach the receiver. In 2/3 runs it was a
  new row-3 density-0.5 hypothesis. Reproduce this as a failing unit test (a synthetic slot where only
  port 0 / CDM group 0 is received), root-cause it, fix it (no ZP export from dark-only / unsupported evidence), and
  run G1–G3 + G5. Commit, push. The wide path stays opt-in.
- **ocudu-dl** (optional, only if time remains) — `sdd/gap-ocudu-dl` @ 02aa0cb5a3. Offline, measure the
  probation-maintenance cost (lab: 1.52M checks, drop_full 9 % → 13–16 %) with a micro-benchmark and propose a
  fix. Change behaviour only test-first.

## Part C — WIP snapshot triage (bounded)

`git fetch github 'refs/heads/wip/*:refs/remotes/github/wip/*'`. Each `wip/2026-09-28/<name>` is one unvalidated
snapshot commit whose parent is the tree's HEAD at the time (often an older integration commit). Some may not
be pushed yet: list the missing ones and move on. Record a triage table (landed → SHA / retired → reason) in
`CLOUD_REPORT.md`.

| Branch | Content | Action |
|---|---|---|
| `gap-ssb` | SSB detector + tests | Part B |
| `gap-cbg` | `cbg_contract_test.py` | List only (out of scope) |
| `rfsim-integ` | 4 receiver files (`nr_pdcch_blind_monitor.c`, `nr_pdcch_ul_discovery.c`, `nr_pdcch_ul_interp_sweep.c`, `nr_pusch_passive_decode.c`) + `nr_pusch_passive_dmrs_pdu.h` + OCUDU ZMQ harness (`tests/passive_rx/run_ocudu_passive.sh`, `ocudu_owned_process.py`, `tests/passive_rx/ocudu/*`) | Diff the receiver files vs `sdd/integration` (the header already exists there). Anything not landed: find its purpose, port test-first onto `sdd/gap-ul-wip`, G1–G3 + G5. Put the harness on `sdd/ocudu-harness` (syntax + py tests only). |
| `rfsim-val`, `rfsim-local`, `rfsim-base` | Older harness copies, `ue.passive.agn.conf`, a 3-line `gnb.sa.rfsim.conf` change | Dedupe vs `rfsim-integ` (newest per file); land with the harness if still needed, else retire |
| `ocudu-bed-matrix` | Plan-only matrix planner + 4 tests | Land on `sdd/gap-bed` if its tests pass |
| `ocudu-dl-clean`, `ocudu-dl-g4`, `ocudu-dl-r3` | Old iterations of the OCUDU-DL fix | SUPERSEDED by 02aa0cb5a3: do not merge; report anything (tests especially) missing from the committed fix |

## Agent dispatch rules

This session shares the operator's weekly usage limit with everything else, so spend it on implementation, not
on coordination.
- **You are the coordinator.** You own git (branches, commits, pushes), the report and the order of work. Subagents
  never push, never merge, and never edit the report.
- **At most 3 subagents running at once.** No workflows or fan-outs beyond that.
- **One implementer subagent per task** (T1…T7, each Part B lane, the Part C triage), with a self-contained brief:
  goal, files it may touch, the tests it must write first, the gate commands, what to return (diff summary,
  RED/GREEN logs, test counts).
- **Parallel only where files are disjoint:** T1, T3 and T4 are independent pure modules and may run together. T2
  depends on T1's types; T5 needs T1–T4; T6 and T7 run after T5. Part B csirs and ssb touch different files and
  may run in parallel with Part A tasks.
- **One heavy build at a time.** Serialize full builds (a shared build dir; parallel ninja runs corrupt each other
  and starve the VM). Subagents build only their own test targets, or ask the coordinator to build.
- **G5 review is a fresh subagent** that did not write the code: give it only the diff vs the lane base, the
  task brief and the gate rules. It returns Approved / findings (Critical / Important / Minor). Fix Critical and
  Important, then re-review with a fresh reviewer.
- **Verify, don't trust:** before committing, the coordinator re-runs the task's tests itself and reads the diff.
  A subagent's "all green" is not evidence.
- **Survive cutoffs:** after every task, commit (on the branch) and append to `CLOUD_REPORT.md`, so a session
  limit mid-run loses at most one task. If you resume, read the report first and continue from the last
  completed task.

## Rules

- Order: Part C triage first (quick, it may change the base), then Part A T1→T7, then Part B.
- Gates per behaviour change: G1 build, G2 ctest (+ shuffle), G3 test-first, G5 independent review
  (fresh subagent), G6 hygiene. Never relax an assertion; root-cause fixes only.
- The receiver discovers everything blindly: never seed from gNB configs/logs/SIB1-dedicated assumptions.
  Ground truth is for scoring only.
- `git add <explicit paths>` only (never `-A`), no stray files/logs. Commit messages state root cause + evidence
  and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Push branches to `github`: no force-push, never push `adaptive-rx-UL-DL`, no merges into `sdd/integration`.
- Append progress to `docs/cloud/CLOUD_REPORT.md` after every task, so a cutoff loses nothing. Follow the code's
  existing style (OAI brace style, `LOG_*` macros, `nr_` prefixes).

## Final reply

Per part/task: Status (DONE / DONE_WITH_CONCERNS / BLOCKED), branch + SHAs, tests + counts with RED evidence, the
T7 accuracy table against the pre-registered thresholds, the Part C triage table, and the exact list of what the
lab must still run (G4 per lane, UE-localisation live steps).
