# Cloud session prompt — passive agnostic receiver: UE localisation (no sensing) + remaining validation

Paste everything below the line into the cloud session.

---

You are working on `github.com/nico-net/passive_agnostic_rx_for_sensing` (an OpenAirInterface fork: the OAI NR UE
repurposed as a **passive, fully blind ("agnostic") 5G receiver** that never attaches and never transmits).
Start from branch **`cloud/ue-localization`** (= `sdd/integration` @ 239eb144ac + docs). Read first:

1. `docs/cloud/UE_LOCALIZATION_SPEC.md` — the design (single X410, 4-element λ/2 ULA, one gNB at a known position).
2. `docs/cloud/HANDOVER_AGNOSTICITY.md` — §1 hard rules, §2 gates G1–G6, §4 test procedures, §6 status log (newest
   at the bottom).
3. `docs/cloud/full-running-agnosticity/` — lane reports (`gap-*-report.md`), `lanes-round3.md`, `context.md`,
   `rfsim-results-val-phytest.md` (phy-test bed recipe), `ocudu-harness.md`.

Environment assumption: a cloud container — no SDR, no X410, and probably no OCUDU gNB, srsUE or open5gs core.
You CAN build OAI (`cmake_targets/build_oai` / cmake+ninja, `-DENABLE_TESTS=ON`) and run gtests. Try the OAI
rfsim **phy-test** bed (gNB `--phy-test` + passive receiver, no core); if it cannot run here, say so and stay
offline. Never claim a live result you did not run.

## Part A — UE localisation (implement + validate). NO SENSING.

Do **not** use or modify the sensing pipeline (`openair1/PHY/NR_UE_ISAC/`, range-Doppler/CFAR/tracker), and do not
implement spec §4 (UE-illuminated sensing). Implement only the UE-localisation path of spec §3/§6, as new code in
`openair1/PHY/NR_UE_TRANSPORT/` (e.g. `nr_ue_localization.{h,c}` + tests), opt-in (config key or `ISAC_UE_LOC=1`),
default behaviour bit-identical.

A1. **UL arrival timing per decoded PUSCH**, per RNTI: arrival offset of the UE's PUSCH vs the receiver's own DL
    frame timing, sub-sample (e.g. phase slope across the DM-RS channel estimate, or CIR peak + interpolation).
    Only from CRC-passed PUSCH. Traps already paid for: UL channel estimates are indexed RELATIVE to the
    allocation, not absolute subcarriers; a return code of 0 is not a CRC pass; the UL decode runs off the RT
    thread in `nr_pusch_passive_queue.c` — keep all new work off the RT thread too.
A2. **TA bookkeeping** per RNTI epoch: absolute TA from the RAR (12-bit, 16·64·Tc/2^µ units), relative updates from
    TA-command MAC CEs found in decoded DL transport blocks (6-bit, offset 31). On NSA cells the RAR at SCG
    addition is the only absolute anchor — handle "no absolute TA yet" explicitly. TA is a consistency check
    (spec §3), never the primary fix.
A3. **Bearing** from the per-antenna PUSCH DM-RS channel estimates: standalone narrowband estimator
    (interferometry for 2 elements, beamscan for ≥3) with the linear-array half-plane restriction, a CRB-based
    σ, and phase self-calibration from the DL direct path, whose bearing is known from the surveyed gNB position.
    Small and local to this module. Do NOT reuse `isac_aoa`.
A4. **Localizer**: bistatic geometry, foci = gNB and receiver. Timing gives d(UE→rx) − d(UE→gNB) (a hyperbola);
    bearing gives a ray; the UE height prior is 1.5 m. Fix = ray ∩ hyperbola in closed form, plus covariance.
    Add a TA-consistency outlier flag and a simple per-RNTI constant-velocity Kalman filter. Output: one
    machine-readable log line / JSON record per fix with a **pseudonymised** RNTI (salted hash) and no identities.
A5. **Validation**:
    - Offline gtests first (TDD, RED→GREEN recorded): closed-form geometry against brute force, including
      degenerate cases (no intersection, endfire, UE on the baseline); synthetic PUSCH DM-RS with an injected
      delay and per-antenna phase → recovered timing within 0.1 sample and bearing within the CRB; TA
      accumulation across RAR + MAC CEs; NSA "no RAR" path.
    - If the phy-test bed runs: inject a known UE→receiver delay and per-antenna phase (rfsimulator channel
      options, or a minimal test-only injector behind an env knob, off by default). Show the recovered
      range-difference matches the injection within 1 sample, n ≥ 3 runs, and that the default-off build is
      unchanged against baseline.
    - Pre-register the pass thresholds in `docs/cloud/CLOUD_REPORT.md` BEFORE the runs.

## Part B — finish validation of the open lanes (what is possible in the cloud)

State as of 2026-09-28 (details in the lane reports; `wip/2026-09-28/*` branches are unvalidated snapshots):

- **ssb** — `wip/2026-09-28/gap-ssb`: SSB rate-match detector + production-path test, G1 build was in progress.
  Do: G1, G2 (full ctest + `--gtest_shuffle`), G3 RED evidence for the production-path test, G5 with a fresh
  independent reviewer, then commit onto `sdd/gap-ssb`. Live G4 on phy-test if runnable, n ≥ 3, alternated with
  `sdd/integration`; named line `PDSCH SSB-OBS`.
- **csirs** — `sdd/gap-csirs` @ 621a91dabe (row-3 density 0.5, committed). Live 8-port on phy-test: CRC equal to
  baseline, but 8-port discovery 0/6 on BOTH baseline and lane (only port-0/CDM-group-0 is sequence-visible), and
  every run exported one ZP resource in symbol 13 on REs that are dark only because ports 1–7 never reach the
  receiver; 2/3 lane runs exported a new row-3 d0.5 hypothesis as that false ZP. Do: reproduce the dark-RE false
  ZP export as a failing unit test, root-cause it, fix it (no ZP export on unsupported or dark-only evidence),
  and re-run the gates. The wide path stays opt-in (it fired 0 times).
- **ocudu-dl** — `sdd/gap-ocudu-dl` @ 02aa0cb5a3, committed, live 2/3 pairs PASS on the OCUDU bed. The third
  pair needs OCUDU (local only) — do not attempt it. Optional offline: the probation-maintenance cost
  (1.52M checks → drop_full 9 % → 13–16 %). Measure it and propose a fix; do not change behaviour without a test.
- **bwp** — `sdd/gap-bwp` @ 8e9fdbf8b5 (thin telnet-trigger wrapper). A `--phy-test` gNB cannot execute a BWP
  switch (`du_get_f1_ue_data()` NULL deref, `f1ap_ids.c:142`; the phantom test UE has no F1 data). Live G4 needs
  the OAI SA bed with a core. Do it only if you can run open5gs here; otherwise leave it for local.
- **integration** — merge each lane that passes G1–G6 into `sdd/integration`, re-run G1+G2 on the merged tree.
  Do NOT merge into `adaptive-rx-UL-DL`: that needs the full §4.4 matrix, which needs the local OCUDU/SA beds.

## Part C — uncommitted work and WIP snapshots (triage, then land or retire)

On 2026-09-28 the uncommitted work of every tree was snapshotted as one commit per tree on `wip/2026-09-28/<name>`.
The parent of each snapshot is the tree's HEAD at the time (often an OLDER integration commit). None is validated.
Run `git fetch github 'refs/heads/wip/*:refs/remotes/github/wip/*'` and list them. The six marked (L) were made on
the local lab host and may not be on GitHub yet: if one is missing, record it as "not pushed" and move on.

| Branch `wip/2026-09-28/…` | Parent | Content | What to do |
|---|---|---|---|
| `gap-ssb` | 239eb144ac | SSB rate-match detector + production-path test, 9 files | Part B **ssb** above |
| `gap-cbg` | 239eb144ac | `cbg_contract_test.py` | Out of scope: leave, only list it |
| `rfsim-integ` (L) | 66176b7250 | 4 receiver files (`nr_pdcch_blind_monitor.c`, `nr_pdcch_ul_discovery.c`, `nr_pdcch_ul_interp_sweep.c`, `nr_pusch_passive_decode.c`) + new `nr_pusch_passive_dmrs_pdu.h`, AND the OCUDU ZMQ harness (`tests/passive_rx/run_ocudu_passive.sh`, `ocudu_owned_process.py`, `tests/passive_rx/ocudu/{gnb.ocudu.zmq.yaml,ocudu_zmq_broker.py,score_ocudu_run.py,ue.passive.ocudu.conf,ue.srsue.ocudu.conf}`) | (a) Receiver edits: diff against the same files on `sdd/integration`. If already landed, retire. If not, work out their purpose (see the handover status log and `gap-pusch-report.md`), write the failing test first, port onto a new lane `sdd/gap-ul-wip` from `sdd/integration`, run G1–G3 + G5, commit. (b) Harness: land it on a lane `sdd/ocudu-harness` (test infra only: syntax checks + py tests; it cannot run without OCUDU here). This is the canonical `run_ocudu_passive.sh` that `ocudu-harness.md` describes. |
| `rfsim-val` (L), `rfsim-local` (L), `rfsim-base` (L) | c295fa18f1 / 67bb0eaa11 / 222f98d072 | Older copies of the OCUDU harness, plus `ue.passive.agn.conf` and a 3-line `gnb.sa.rfsim.conf` change | Dedupe against `rfsim-integ` (keep the newest per file, note differences). Land `ue.passive.agn.conf` and the gNB conf change with the harness if they are still needed, else retire |
| `ocudu-bed-matrix` (L) | 239eb144ac | `tests/passive_rx/ocudu_bed_matrix.py` + `test_ocudu_bed_matrix.py` (plan-only, 18 arms × 3 pairs, 4/4 offline) | Land on `sdd/gap-bed` after its tests pass here. It stays plan-only; it must refuse to launch the arms marked unroutable |
| `ocudu-dl-clean` (L), `ocudu-dl-g4` (L), `ocudu-dl-r3` (L) | older | Earlier iterations of the OCUDU-DL CSI-RS ZP fix | SUPERSEDED by `sdd/gap-ocudu-dl` @ 02aa0cb5a3. Do not merge. Only diff them against 02aa0cb5a3 and report anything that exists there and not in the committed fix (tests especially). Port a missing test only if it still applies, test-first. |

Also untracked on the lab host and deliberately NOT snapshotted: gate-evidence logs (`cmake_targets/ocudu-r3-gates/`)
and build directories. Record each triage decision (landed → SHA / retired → reason) as a table in `CLOUD_REPORT.md`.

Out of scope: sensing (all of `NR_UE_ISAC`), nsa/prg/cbg lanes, X410/OTA, anything under `gnb_remote_logs/`/`cuLogs/`.

## Rules

- Gates G1–G6 for every behaviour change; TDD with RED→GREEN evidence; never relax an assertion; root-cause fixes.
- The receiver must discover everything blindly: never seed it from gNB configs/logs/SIB1-dedicated
  assumptions. Ground truth is for scoring only.
- `git add <explicit paths>` only (never `-A`), no stray files/logs. Commit messages state root cause + evidence
  and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Order: Part C triage first (it may change what Part A/B build on), then Part B (ssb, csirs), then Part A.
- Work on branches (`sdd/ue-loc` for Part A, the lane branches for Parts B/C); push them to `github`
  (no force-push, never push `adaptive-rx-UL-DL`).
- Append progress to `docs/cloud/CLOUD_REPORT.md` after every step, so a cutoff loses nothing.

## Final reply

Per part/lane: Status (DONE / DONE_WITH_CONCERNS / BLOCKED), branches + commit SHAs, tests + counts (with RED
evidence), live evidence (per-run table) or an explicit "not run here + why", what remains for the local beds, and
the Part C triage table.
