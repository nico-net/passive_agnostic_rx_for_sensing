# Cloud session prompt — offline validation of the passive agnostic receiver lanes

Paste everything below the line into the cloud session.

---

Repository: `github.com/nico-net/passive_agnostic_rx_for_sensing`, an OpenAirInterface fork. It is the OAI NR UE
repurposed as a **passive, fully blind ("agnostic") 5G receiver** that never attaches, never transmits, and must
discover every cell parameter from the air. The goal is to make it work on a **commercial gNB** (the target
commercial cell is **NSA**). Start from branch **`cloud/ue-localization`** (= `sdd/integration` @ 239eb144ac +
`docs/cloud/`; the name is historical). Read first:
- `docs/cloud/HANDOVER_AGNOSTICITY.md` §1–§2 (hard rules, gates G1–G6) and the §6 status log (newest at the
  bottom);
- `docs/cloud/full-running-agnosticity/`: the lane reports `gap-*-report.md`, `lanes-round3.md`, `context.md`.

## Scope: OFFLINE ONLY, finish only what can be finished offline

- Allowed: builds (cmake/ninja, `-DENABLE_TESTS=ON`), unit tests, gtests, offline link-level simulators (`nr_dlsim`
  / `nr_ulsim`) and the tree's own test harnesses.
- Not allowed:
  - **no live beds**: do not start `nr-softmodem`/`nr-uesoftmodem`, rfsim, OCUDU, srsUE or a core;
  - the live gate G4 stays in the lab: write "G4 pending (lab)", never claim a live result.
- Out of scope:
  - UE localisation (`docs/cloud/UE_LOCALIZATION_SPEC.md` is future work, do not implement);
  - the sensing pipeline (`openair1/PHY/NR_UE_ISAC/`);
  - multi-operator, prg/cbg lanes, X410/OTA, the §4.4 live matrix.

## Tasks, in this order

### 1. WIP snapshot triage

Run `git fetch github 'refs/heads/wip/*:refs/remotes/github/wip/*'`.
- Each `wip/2026-09-28/<name>` is one unvalidated snapshot commit; its parent is the tree's HEAD at the time,
  often an older integration commit.
- Some may not be pushed yet: list the missing ones and move on.
- Record the triage table (landed → SHA / retired → reason) in `docs/cloud/CLOUD_REPORT.md`.

| Branch | Content | Action |
|---|---|---|
| `gap-ssb` | SSB rate-match detector + production-path test (9 files) | Task 2 |
| `gap-cbg` | `cbg_contract_test.py` | List only (out of scope), keep the branch |
| `rfsim-integ` | 4 receiver files (`nr_pdcch_blind_monitor.c`, `nr_pdcch_ul_discovery.c`, `nr_pdcch_ul_interp_sweep.c`, `nr_pusch_passive_decode.c`) + `nr_pusch_passive_dmrs_pdu.h` + OCUDU ZMQ harness (`tests/passive_rx/run_ocudu_passive.sh`, `ocudu_owned_process.py`, `tests/passive_rx/ocudu/*`) | Diff the receiver files against `sdd/integration` (the header already exists there). For anything not yet landed: find its purpose (handover log, `gap-pusch-report.md`), port it test-first onto `sdd/gap-ul-wip`, run G1–G3 + G5. Put the harness on `sdd/ocudu-harness` (syntax + py tests only). |
| `rfsim-val`, `rfsim-local`, `rfsim-base` | Older harness copies, `ue.passive.agn.conf`, a 3-line `gnb.sa.rfsim.conf` change | Dedupe against `rfsim-integ` (newest per file); land with the harness if still needed, else retire |
| `ocudu-bed-matrix` | Plan-only matrix planner + 4 tests | Land on `sdd/gap-bed` if its tests pass |
| `ocudu-dl-clean`, `ocudu-dl-g4`, `ocudu-dl-r3` | Old iterations of the OCUDU-DL fix | SUPERSEDED by `sdd/gap-ocudu-dl` @ 02aa0cb5a3: never merge; report anything (tests especially) missing from the committed fix, port it only if it still applies, test-first |

### 2. ssb — `wip/2026-09-28/gap-ssb` → `sdd/gap-ssb`

SSB rate-matching detector plus a production-path test. The lab review found that the earlier tests only exercised
the helper.
1. G1 build.
2. G2 full ctest + `--gtest_shuffle` (known skips: blind_monitor 2, config_sweep 1).
3. G3 RED evidence: break the production integration and show the new test fails.
4. G5 with a fresh independent reviewer; fix Critical/Important findings.
5. Commit onto `sdd/gap-ssb`. G4 pending (lab): named line `PDSCH SSB-OBS`.

### 3. csirs — `sdd/gap-csirs` @ 621a91dabe

The lab found that every run exports one **false ZP resource** in symbol 13, on REs that are dark only because
CSI-RS ports 1–7 never reach the receiver. In 2/3 runs it was one of the new row-3 density-0.5 hypotheses.
1. Reproduce it as a failing unit test: a synthetic slot where only port 0 / CDM group 0 is received and the
   other ports' REs are empty.
2. Root-cause it and fix it: no ZP export from dark-only / unsupported evidence.
3. Run G1–G3 + G5 and commit. The wide search path stays opt-in (it fired 0 times).

This matters for commercial cells: a false ZP resource makes the PDSCH decoder rate-match around REs that
carry data.

### 4. nsa-mib — new lane `sdd/gap-nsa-mib`, directly relevant to the commercial NSA cell

Source finding (lane report `gap-nsa-report.md`):
- On a cell without SIB1 / CORESET#0 (NSA, kSSB ≥ 24), the MIB's `dmrs-TypeA-Position` is stored by
  `nr_ue_procedures.c` (`mac->dmrs_TypeA_Position`).
- But it reaches the blind monitor's `g_cfg.dmrs_typeA_position` only via
  `nr_pdcch_blind_monitor_autoconf_css0()` or manual BWP parsing.
- So on an NSA cell with pos3 the blind chain keeps pos2, and the DM-RS masks are wrong.

Steps:
1. RED test: real MIB delivery with FR1 kSSB 31 + pos3 → blind grant extraction must use symbol 3, and CSS0 must
   stay absent. Controls: pos2, a normal CSS0 cell, and repeated MIB delivery leaving learned geometry intact.
2. Minimal fix: hand this measured MIB fact to the blind monitor. It is measured from the air, not seeded.
3. G1–G3 + G5, commit.

Also verify, by test or source reading, that blind discovery is reachable with no CSS0 at all. The lane report
retracted the blanket "CSS0 bootstrap blocker" claim; confirm or refute it with a test and record the verdict.

### 5. ocudu-dl (optional, only if time remains) — `sdd/gap-ocudu-dl` @ 02aa0cb5a3

Committed; 2 of 3 live pairs pass in the lab. Offline: measure the probation-maintenance cost (lab: 1.52M checks,
drop_full 9 % → 13–16 %) with a micro-benchmark, and propose a fix. Change behaviour only test-first.

## Merge rules

- **Nothing goes into `sdd/integration`, `sdd/validation` or `adaptive-rx-UL-DL` from this session.** Merging
  there requires the live gate G4, which only the lab can run.
- A lane is **offline-complete** when G1, G2, G3, G5 and G6 all pass, it is committed on its lane branch
  (`sdd/gap-<lane>`) and pushed, and the report has its evidence.
- Offline-complete lanes are merged into one staging branch **`sdd/offline-validated`**, created from
  `sdd/integration` @ 239eb144ac:
  - `git merge --no-ff sdd/gap-<lane>`, one lane per merge commit, never squash or rebase a pushed branch;
  - after EVERY merge, re-run G1 + G2 on the merged tree; if it fails, revert that merge and report why;
  - resolve conflicts by reading both sides (never "take ours/theirs" wholesale), then re-run the lane's tests.
  The lab later runs G4 per lane and merges `sdd/offline-validated` (or individual lanes) into `sdd/integration`.
- Merge commit message: lane name, the gate evidence (test counts), "G4 pending (lab)", ending with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Branch deletion rules (temporary branches only)

A branch may be deleted from `github` (`git push github --delete <branch>`) only if ALL of these hold:
1. It is **temporary**: `wip/*`, or a helper branch this session created and fully merged.
2. Its content is **accounted for**: either `git merge-base --is-ancestor <branch> sdd/offline-validated` is true,
   or every file/hunk it contained is shown landed elsewhere, or it is recorded as RETIRED with a reason.
3. Its tip SHA is recorded in `CLOUD_REPORT.md` next to the reason, so it can be restored.

Never delete:
- `adaptive-rx-UL-DL`, `sdd/integration`, `sdd/validation`, `sdd/offline-validated`, `cloud/*`;
- any `sdd/gap-*` lane branch (the lab deletes those after G4 + merge);
- `wip/2026-09-28/gap-cbg` (out-of-scope work kept for later);
- any branch you did not triage.

No force-push. Delete remote branches only at the end, after the final report is written, as one listed batch.

## Agent dispatch rules

This session shares the operator's weekly usage limit, so spend it on the work, not on coordination.
- **You are the coordinator.** You own git (branches, commits, merges, pushes, deletions), the report and the
  order of work. Subagents never push, merge or delete, and never edit the report.
- **At most 3 subagents at once.** No workflows or larger fan-outs.
- **One implementer subagent per task**, with a self-contained brief: goal, files it may touch, tests to write
  first, gate commands, what to return (diff summary, RED/GREEN logs, test counts).
- **Parallel only where files are disjoint:** csirs (task 3) and nsa-mib (task 4) may run alongside ssb (task 2);
  the triage (task 1) comes first.
- **One heavy build at a time:** parallel ninja runs in a shared build dir corrupt each other. Subagents build
  only their own test targets.
- **G5 review is always a fresh subagent** that did not write the code: it gets only the diff against the lane
  base, the brief and the gate rules, and returns Approved or findings (Critical / Important / Minor). Fix
  Critical/Important, then re-review with a new reviewer.
- **Verify, don't trust:** before committing, the coordinator re-runs the task's tests and reads the diff.
- **Survive cutoffs:** after every task, commit, push the lane branch, and append to `CLOUD_REPORT.md`, so a session
  limit loses at most one task. On resume, read the report first.

## Rules

- Gates per behaviour change: G1 build, G2 ctest (+ shuffle), G3 test-first with RED→GREEN evidence, G5
  independent review, G6 hygiene. Never relax an assertion; root-cause fixes only.
- The receiver discovers everything blindly: never seed it from gNB configs/logs/SIB1-dedicated assumptions.
  Ground truth is for scoring only.
- `git add <explicit paths>` only (never `-A`), no stray files/logs. Commit messages state root cause + evidence,
  ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Follow the code's existing style: OAI brace style, `LOG_*` macros, `nr_` prefixes.

## Final reply

Per task: Status (DONE / DONE_WITH_CONCERNS / BLOCKED), branch + SHAs, tests + counts with RED evidence, G5
verdict. Then the Part-1 triage table, the list of merges into `sdd/offline-validated` (with the post-merge
G1+G2 result), the deleted branches (with tip SHAs), and the exact list of what the lab must still run (G4 per
lane).
