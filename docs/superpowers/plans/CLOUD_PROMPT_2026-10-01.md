# Cloud session prompt — implement Track A (no X410) of the DGX next-steps plan

You are an autonomous cloud session working on the repository `nico-net/passive_agnostic_rx_for_sensing`
(OAI-based passive, agnostic 5G NR receiver). Nobody will answer questions during your run: decide, record why, continue.

## 0. Read first (in this order, completely)

1. `PROJECT_MEMORY.md` — the single source of truth (evidence labels §0.1, frozen sens6 profile §4.0, gates §12, DGX results §13.1/§14, known issues §24).
2. `CLAUDE.md` if it exists (it is created by Task A0).
3. `docs/superpowers/plans/2026-10-01-dgx-next-steps.md` — **the implementation plan you execute**.

## 1. Setup

```bash
git fetch origin --tags
git switch adaptive-rx-UL-DL && git pull --ff-only
git switch -c cloud/dgx-next-steps
git tag -l sens6-frozen-2026-09-30   # must exist; it is the frozen-sens6 reference
```

Work only on branch `cloud/dgx-next-steps`; push it to `origin` after every completed task
(`git push -u origin cloud/dgx-next-steps`). Never push to `adaptive-rx-UL-DL`, never force-push, never open a PR.

## 2. Execution method (operator decision): subagent-driven

Use the **superpowers:subagent-driven-development** skill: one fresh implementer subagent per task, then a fresh
reviewer subagent per task (superpowers:requesting-code-review), before starting the next task. Follow the plan's
"Agent / model / plugin policy" table exactly:

- **Haiku 4.5** (`claude-haiku-4-5-20251001`) — only steps marked 🔁 (repeated runs, score collection, mechanical edits).
- **Sonnet 5.5** (`claude-sonnet-5-5`) — normal implementation, tests, debugging, rfsim runs + analysis.
- **Opus 5.5** (`claude-opus-5-5`) — you (the orchestrator), the ★ task A7, the A3 header review gate, the final review.

Every implementer must use superpowers:test-driven-development (write the failing test first, see it fail, then
implement), superpowers:systematic-debugging on any failure, and superpowers:verification-before-completion before
reporting done. Use the **frontend-design** skill for A5. Use `/code-review` (code-review plugin) at the end of Track A.
If a plugin skill is not installed in this environment, follow the same process manually and say so in the final report.

Keep workflows small (at most ~5 concurrent agents). **Only one rfsim bed may run at a time** (fixed ports, CPU budget).

## 3. Scope in THIS cloud session (x86_64, no GPU, no ARM, no X410)

| Task | Do here? | Notes |
|---|---|---|
| A0 | YES | venv: `python3 -m venv`; if pip has no network, record it and skip pyzmq-dependent checks only |
| A1 | YES | **Re-baseline first**: the gate thresholds (CRC ≥ 98 %, drop_full ≤ 1 %) were measured on the DGX. Run the gate on unmodified HEAD; if this host misses them only because of CPU budget, record the measured baseline in the evidence and use "within run-to-run spread of the HEAD baseline on this host" as the gate for this session |
| A2 | YES | full (C module, gtest, rfsim) |
| A3 | YES | full; Opus header review gate first |
| A4 | YES | full |
| A5 | YES | full (frontend-design) |
| A6 | Code + dry-run tests only | Step 5 (measured A/B) is **DGX-only** — the core map is for the GB10 topology |
| A7 ★ | YES | TSAN concurrency test + rfsim A/B on this host (label the host) |
| A8 | **NO — DGX only** (needs the GB10 GPU) | leave unchecked, list as follow-up |
| A9 | **NO — DGX only** | leave unchecked |
| A10 | **NO — DGX only** | the measurement is about the DGX `UEthread_0` |
| A11 | Code change + "default unchanged" regression only | timing measurement is DGX-only |
| A12 | **NO — ARM-only bug (K22)** | cannot reproduce on x86 |
| A13 | x86 path only | verify the x86 path is byte-identical in behaviour; the aarch64 branch is verified on the DGX |
| A14 | YES (adapted) | see §5 |
| Track B (B1–B6) | **NO** | needs the X410 |

## 4. Build on this host

```bash
cd cmake_targets && mkdir -p ran_build/build && cd ran_build/build
cmake ../../.. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_SIMU=ON -DOAI_USRP=OFF -DENABLE_ISAC_SENSING=ON
ninja nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests && ctest -j"$(nproc)" --output-on-failure
```
If `./build_oai -I` is needed for dependencies, run it once. Record the host (CPU model, cores, RAM, OS, compiler)
in the evidence. Known x86 ctest baseline before your changes: 125/126, only `test_vrtsim_cirdb` (shm race) may fail;
on this host, record what you actually observe on unmodified HEAD and treat that as the baseline.
Also keep `-DENABLE_ISAC_SENSING=OFF` building (Global Constraints): build `nr-uesoftmodem` once in a second build
dir with sensing OFF at the end of A2 and A3.

## 5. Rules you must not break

- **sens6 is FROZEN**: before every commit run
  `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30`
  — it must succeed. New configs/launchers go in new files.
- gNB logs/configs are ground truth for validation only, never receiver input.
- Evidence labels on every claim. Results from this cloud host are `[SIM VERIFIED, cloud x86 <cpu>, <date>, <commit>]`
  and must **never** be merged with DGX numbers.
- `git add <explicit paths>` only; never `git add -A`, never `git stash`; commit messages = what + why + evidence path,
  ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` (or the executing model).
- `ISAC_RX_BRANCH_FO` stays unset. Stop processes with SIGINT, never SIGKILL first.
- Do not weaken a test to make it pass; if a plan step is wrong for this tree (a field or function name differs),
  fix the step in the plan file in the same commit and say why.
- Evidence of this session: `tests/passive_rx/cloud_run_2026-10-01/` (scores JSON, campaign summaries, ctest logs,
  TSAN output). No raw logs > 10 MB in git.

## 6. Finish (mandatory, in this order)

1. Track-A final review: `/code-review` on `adaptive-rx-UL-DL..cloud/dgx-next-steps` + an Opus reviewer subagent;
   fix findings; full ctest, blind-monitor shuffle seeds 1/3/5, rfsim regression gate, sens6 frozen check.
2. **Update `PROJECT_MEMORY.md`** (this is required): for every task you completed, the relevant section gets date,
   commit, evidence path and label — at least §3.3/§3.4 (new modules/tools), §10.2 (new env vars `ISAC_METRICS_PATH`,
   `ISAC_OBS_PATH`, `ISAC_SCAN_SCRATCH_MB`), §11 (the `ISAC_METRICS` line), §13 (new tests), §14 (cloud rfsim results,
   clearly separated from DGX §14.1), §16 status table, §21 (pointer to `nr_passive_obs.h` schema v1, field table),
   §24 (K26/K27 changes; new issues found), §25 (what is done, and the DGX follow-ups: A6 Step 5, A8, A9, A10, A11
   timing, A12, A13 aarch64 check, then Track B). Also tick the completed checkboxes in the plan file.
3. **Delete this prompt file**: `git rm docs/superpowers/plans/CLOUD_PROMPT_2026-10-01.md`.
4. Commit ("docs: cloud Track-A results, PROJECT_MEMORY update, remove cloud prompt") and push `cloud/dgx-next-steps`.
5. Final report (your last message): per task — status (done / partial / skipped-DGX), commits, test and rfsim
   results with numbers, deviations from the plan and why, open problems, and the exact list of DGX follow-ups.
