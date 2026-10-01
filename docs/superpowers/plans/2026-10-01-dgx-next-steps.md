# DGX Next Steps (metrics, observation API, campaign logs, dashboard, cores/GPU, Milan OTA) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the passive receiver machine-readable performance metrics, a per-grant observation API for sensing consumers, fully documented per-campaign logs, a receiver-health dashboard, a DGX-specific core map with measured parallelization/GPU gains, and then validate all of it over the air on the new Milan cell.

**Architecture:** Everything that can be built and proven offline or in the rfsim beds is in **Track A (no X410 needed)**; everything that needs the radio is in **Track B (X410 needed)**, and Track B consumes Track A's artifacts (metrics JSONL, observations JSONL, campaign runner, DGX launcher, dashboard). New C code lives in small new files (`nr_passive_metrics.{c,h}`, `nr_passive_obs.{c,h}`) that the existing receiver calls at two already-identified sites; new Python lives under `tests/passive_rx/{metrics,campaign,dgx}/`. No existing sens6 file is modified.

**Tech Stack:** C11 (OAI style, `LOG_*` macros, `_Atomic`, pthreads), gtest via CMake/ctest (Ninja build in `cmake_targets/ran_build/build`), CUDA 13.0 for `sm_121` (GB10), Python 3.12 stdlib + `unittest` (+ `pyzmq` in a venv for the dashboard), OAI rfsim phy-test beds.

**Spec:** `PROJECT_MEMORY.md` (read completely first) — in particular §0.1 evidence labels, §4.0 frozen sens6 profile, §4.2 DGX host, §11 logging reference, §12 gates, §14.1–§14.4 DGX results + core-allocation design, §15.0/§15.4 Milan site, §21 observation-record target, §24 known issues (K3, K10, K11, K17, K21–K27), §25 next steps. Plus the operator's request of 2026-10-01: dashboard update, sensing API, receiver performance metrics, core allocation and GPU improvements, detailed logs for every campaign, split by "needs X410" / "does not need X410".

## Global Constraints

- **sens6 is FROZEN** (§4.0): never modify `tests/passive_rx/captures/*`, `tests/passive_rx/*.conf`, `tests/passive_rx/sens6_host_snapshot_2026-09-30/*`. Gate before every commit: `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` must succeed. DGX variants go in **new** files.
- **Agnosticity rule** (§1.3): gNB configs/logs are ground truth for validation only; nothing in this plan may feed them to the receiver.
- **Evidence labels** (§0.1) in every PROJECT_MEMORY.md update: `[OFFLINE VERIFIED]`, `[SIM VERIFIED]`, `[OTA VERIFIED <date>, <commit>]`, `[HYPOTHESIS]`. Never merge results from different hosts/cells/bandwidths.
- **Real-time rule:** nothing new may block the PHY receive thread (`UEthread_0`) or a decode consumer on I/O. File/ZMQ writes happen on a dedicated writer thread fed by a bounded lock-free-enough ring; when full, **drop and count**, never wait.
- **Build must keep working with `-DENABLE_ISAC_SENSING=OFF`** (the receiver-only build). New modules must not depend on `NR_UE_ISAC`.
- **OAI code style:** match the surrounding file; `LOG_I/LOG_W/LOG_E/LOG_A(PHY, ...)`, no `printf` in library code, `nr_` prefixes, SPDX header `/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */` like neighbouring files.
- **Regression gate after every C change (rfsim, DGX):** `tests/passive_rx/dgx/rfsim_regress.sh` (Task A1) must report on the 106-PRB baseline arm: Technique D CONVERGED, TB CRC ≥ 98.0 %, scanq drop_full ≤ 1 %; ctest must show no failures beyond the known ARM set {`dft_test`, `test_nr_modulation`, `test_nr_pusch_ra0_qam256`, `test_nr_pusch_ra0_qam64` (intermittent)}.
- **Never build while a radio capture runs on the same host** (§9). Check `pgrep -x nr-uesoftmodem` first.
- **Python:** stdlib + `unittest` only for tests (pytest is not installed); runtime deps in `tests/passive_rx/requirements-dgx.txt`, installed into `tests/passive_rx/.venv` (Ubuntu 24.04 blocks system pip).
- **Commits:** one per task minimum, message = what + why + evidence path, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` (or the executing model). Push to `github` (`origin`) branch `adaptive-rx-UL-DL` only after the task's review passes. Explicit `git add <paths>`, never `-A`; never `git stash`.
- **BRANCHFO stays OFF** and must never be enabled at rank > 1 (§14.4).

## Agent / model / plugin policy (applies to every task)

| Tier | Model | Use for | Required plugin skills |
|---|---|---|---|
| Orchestrator | **Opus 5.5** (`claude-opus-5-5`) | Running this plan (superpowers:subagent-driven-development), task hand-off, reviews of **critical** tasks (marked ★), the final branch review, PROJECT_MEMORY.md integration, OTA campaign decisions | superpowers:subagent-driven-development, superpowers:dispatching-parallel-agents, superpowers:requesting-code-review, superpowers:finishing-a-development-branch, code-review plugin (`/code-review` at the end of each track) |
| Critical implementer | **Opus 5.5** | ★ tasks only: A7 (multi-consumer PDCCH thread safety), A10 (UE_thread split decision + design), B3 (Milan survey live decisions) | superpowers:test-driven-development, superpowers:systematic-debugging, superpowers:verification-before-completion |
| Implementer / tester / debugger | **Sonnet 5.5** (`claude-sonnet-5-5`) | Normal coding, gtests, rfsim runs + analysis, debugging | superpowers:test-driven-development, superpowers:systematic-debugging, superpowers:verification-before-completion; **frontend-design** for A5 (dashboard) |
| Mechanical worker | **Haiku 4.5** (`claude-haiku-4-5-20251001`) | Repetitive/simple steps explicitly marked 🔁: running N identical rfsim/OTA repeats and collecting score lines, copying evidence, regex additions with given tests, reformatting tables | superpowers:verification-before-completion |
| Docs | Sonnet 5.5 | PROJECT_MEMORY.md section updates at the end of each task; one-time CLAUDE.md creation | claude-md-management (`claude-md-improver`) for A0 only |

Parallelism: tasks with no shared files may run in parallel under superpowers:dispatching-parallel-agents, **each in its own worktree** (superpowers:using-git-worktrees, worktrees under `/home/nicola/NICOLA/wt/<task>`), **but only one rfsim/OTA run on the host at a time** (the rfsim beds use fixed ports and the host CPU budget). Parallel-safe groups are listed in "Execution order" at the end.

## Review Focus

1. **Observation/metrics writer under overload** — at 273 PRB the DL consumer produces ~180 grants/s; if the disk is slow or the writer thread stalls, the receiver must keep decoding with identical CRC and report `obs_dropped>0`, never block. Owned by Task A3 (ring overflow test + rfsim A/B with a throttled writer).
2. **Receiver started without the new options** — no `ISAC_METRICS_PATH`/`ISAC_OBS_PATH` must mean byte-identical behaviour except one `ISAC_METRICS` log line per 20 s. Owned by A2/A3 (ctest + rfsim regression with options unset).
3. **Campaign runner killed mid-run (Ctrl-C / SIGTERM / power loss)** — the campaign dir must still contain `manifest.json`, the partial `rx.log` and a `run.json` with `"status": "interrupted"`; children receive SIGINT, never SIGKILL first (X410 claim hygiene §8 X5/X6). Owned by A4.
4. **Dashboard started before the receiver / log rotated / metrics file truncated** — the monitor must show "no data yet" and recover when lines arrive; a truncated JSON line is skipped and counted. Owned by A5.
5. **Core map on a host with different topology** (e.g. sens6 x86, or DGX with SMT/isolcpus changed) — the DGX launcher must refuse to pin to a CPU that is offline or not in `/sys/devices/system/cpu/online`, and print the map it applied. Owned by A6.

---

## TRACK A — does NOT need the X410 (offline + rfsim; runnable in a cloud session except A8 and A9, which need the DGX GPU)

### Task A0: Agent onboarding file and Python environment (🔁 Haiku)

**Files:**
- Create: `CLAUDE.md`
- Create: `tests/passive_rx/requirements-dgx.txt`
- Create: `tests/passive_rx/dgx/README.txt`

**Interfaces:**
- Produces: `tests/passive_rx/.venv/bin/python` with `pyzmq` (used by A5), and the rule set every later agent reads.

- [x] **Step 1: Write `CLAUDE.md`** (use the claude-md-management `claude-md-improver` skill to check it):

```markdown
# CLAUDE.md — passive agnostic 5G NR receiver (OAI UE based)

Read `PROJECT_MEMORY.md` completely before doing anything; it is the single source of truth.

Hard rules:
- sens6 is FROZEN: never edit tests/passive_rx/captures/*, tests/passive_rx/*.conf, tests/passive_rx/sens6_host_snapshot_2026-09-30/*.
  Check: git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30
- gNB logs/configs are validation ground truth only, never receiver input.
- Evidence labels (PROJECT_MEMORY §0.1) on every claim; never merge results across hosts/cells/bandwidths.
- Never build while nr-uesoftmodem is running (pgrep -x nr-uesoftmodem).
- Stop a receiver with SIGINT, never SIGKILL (X410 claim hygiene).
- ISAC_RX_BRANCH_FO must stay unset (never enable at rank > 1).
- git add <explicit paths>; never -A, never stash.

Build (DGX, aarch64): cmake_targets/ran_build/build, Ninja:
  ninja nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests && ctest -j4
Known ARM ctest failures: dft_test, test_nr_modulation, test_nr_pusch_ra0_qam256, test_nr_pusch_ra0_qam64 (intermittent).
Regression: tests/passive_rx/dgx/rfsim_regress.sh (106 PRB baseline: CONVERGED, CRC >= 98 %, drop_full <= 1 %).
Plan in progress: docs/superpowers/plans/2026-10-01-dgx-next-steps.md
```

- [x] **Step 2: Write `tests/passive_rx/requirements-dgx.txt`**

```text
pyzmq>=25
```

- [x] **Step 3: Create the venv and verify**

Run:
```bash
cd /home/nicola/NICOLA/passive_agnostic_rx_for_sensing/tests/passive_rx
python3 -m venv .venv && .venv/bin/pip install -r requirements-dgx.txt
.venv/bin/python -c "import zmq; print(zmq.zmq_version())"
grep -qx '.venv/' .gitignore 2>/dev/null || echo '.venv/' >> .gitignore
```
Expected: prints a libzmq version (e.g. `4.3.5`).

- [x] **Step 4: Write `tests/passive_rx/dgx/README.txt`**

```text
DGX Spark (spark-74c3, aarch64) tools. NEW files only; the sens6 launchers under ../captures are frozen.
rfsim_regress.sh  - rfsim regression gate (Task A1)
run_rx_dgx.sh     - DGX receiver launcher with the X925 core map (Task A6)
coremap_dgx.env   - core map definition (Task A6)
Evidence of 2026-09-30/10-01 lives in ../dgx_host_snapshot_2026-09-30/.
```

- [x] **Step 5: Commit**

```bash
git add CLAUDE.md tests/passive_rx/requirements-dgx.txt tests/passive_rx/dgx/README.txt tests/passive_rx/.gitignore
git commit -m "chore: CLAUDE.md onboarding, DGX python requirements and tools README"
```

---

### Task A1: In-tree rfsim regression gate with JSON scores (Sonnet)

Promote the scratch tools (`tests/passive_rx/dgx_host_snapshot_2026-09-30/tools/{run2.sh,score.sh}`) into a maintained, tested gate that every later C task uses.

**Files:**
- Create: `tests/passive_rx/dgx/rfsim_arm.sh` (one arm: gNB + receiver, timestamped `rx.log`)
- Create: `tests/passive_rx/dgx/score_rx.py` (score one arm dir → JSON)
- Create: `tests/passive_rx/dgx/rfsim_regress.sh` (N baseline arms + pass/fail)
- Test: `tests/passive_rx/dgx/test_score_rx.py`
- Test fixture: `tests/passive_rx/dgx/fixtures/base_r1_rx.log` (first + last 400 lines of a fresh baseline run, created in Step 7)

**Interfaces:**
- Produces: `rfsim_arm.sh <outdir> <secs>` with env `GNBCONF RXCONF CELL RXEXTRA GNBARGS`; writes `<outdir>/{gnb/gnb.log,rx/rx.log,rx/time.txt}`.
- Produces: `score_rx.py <armdir>... [--json]` → one JSON object per arm: `{"arm", "sync_s", "first_crnti_s", "conv_s", "ttc_s", "n_converged", "bank_len", "ldpc_ok", "ldpc_seg_fail", "pdsch_decoded", "pdsch_crc_ok", "crc_pct", "scanq_queued", "scanq_drop_full", "drop_full_pct", "cpu_pct", "max_rss_kb"}` (missing values = `null`).
- Produces: `rfsim_regress.sh [n_runs=2]` exit 0 iff every run passes the gate in Global Constraints; prints the JSON lines.

- [x] **Step 1: Write `rfsim_arm.sh`**

```bash
#!/bin/bash
# One rfsim phy-test arm on the DGX: gNB (plain HEAD nr-softmodem --phy-test) + passive receiver.
# usage: GNBCONF=.. RXCONF=.. CELL="-C .. -r .. --ssb .." [RXEXTRA=..] [GNBARGS="-m 9 -n 0 -M 106 -l 1"] rfsim_arm.sh <outdir> <secs>
set -u
R=$(cd "$(dirname "$0")/../../.." && pwd); B=${BUILD:-$R/cmake_targets/ran_build/build}
OUT=$(realpath -m "$1"); DUR=$2
: "${GNBCONF:=$R/tests/passive_rx/gnb.sa.rfsim.conf}" "${RXCONF:=$R/tests/passive_rx/ue.passive.bwp.agn.conf}"
: "${CELL:=-C 3319680000 -r 106 --ssb 516}" "${GNBARGS:=-m 9 -n 0 -M 106 -l 1}" "${RXEXTRA:=}"
if pgrep -x nr-uesoftmodem >/dev/null || pgrep -x nr-softmodem >/dev/null; then echo "another softmodem is running" >&2; exit 2; fi
rm -rf "$OUT"; mkdir -p "$OUT/gnb" "$OUT/rx"; rm -rf /tmp/passive_rx; mkdir -p /tmp/passive_rx
( cd "$OUT/gnb" && touch nrL1_stats.log nrMAC_stats.log nr_stats.log &&
  exec timeout -s INT $((DUR+25)) "$B/nr-softmodem" --phy-test --noS1 -D 0xff -O "$GNBCONF" --rfsim $GNBARGS > gnb.log 2>&1 ) &
G=$!
sleep 10
cd "$OUT/rx" && touch nrL1_stats.log nr_stats.log
/usr/bin/time -v -o time.txt timeout -s INT "$DUR" "$B/nr-uesoftmodem" --passive-rx --rfsim -O "$RXCONF" $CELL \
  --numerology 1 --band 78 $RXEXTRA 2>&1 |
  gawk -e '@load "time"; BEGIN{t0=gettimeofday()} {printf "%.3f %s\n", gettimeofday()-t0, $0; fflush()}' > rx.log
echo "rx_rc=${PIPESTATUS[0]}" >> time.txt
kill -INT $G 2>/dev/null; wait $G
```

- [x] **Step 2: Write the failing test `test_score_rx.py`**

```python
import json, os, shutil, subprocess, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
SCORE = os.path.join(HERE, "score_rx.py")
FIXTURE = os.path.join(HERE, "fixtures", "base_r1_rx.log")

LOG = """\
5.149 [PHY]    Initial sync successful, PCI: 0
14.147 [PHY]    blind PDCCH rnti_seen x rnti=0x1234 sfn=1
14.300 [PHY]    SENSING: multi-CORESET bank add index=0 offset=0 span=102 symbol=0 mapping=0/0/0 len=46
14.892 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=0 S=1 L=13 mask=0x804 table=0
19.000 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=2 S=1 L=5 mask=0x4 table=0
140.0 [PHY]    SENSING: LDPCDIAG ok=57897 seg_fail=468 tb_fail=0 zero_tb=0 iface_err=0 segs_decoded=0/1 (0.0%)
140.1 [PHY]    SENSING: PDSCHQ queued=58414 decoded=58414 crc_ok=57897 (99.1%) dropped[full=0 stale=0] max_lag_slots=12/20
140.2 [PHY]    SENSING: blind PDCCH monitor summary: occasions=1 scanq[queued=404224 done=404061 drop_full=161 drop_stale=0 maxlag=8] last_reject=x
"""
TIME = "\tPercent of CPU this job got: 299%\n\tMaximum resident set size (kbytes): 687992\nrx_rc=124\n"

class ScoreRx(unittest.TestCase):
    def make_arm(self, log=LOG, time=TIME):
        d = tempfile.mkdtemp(); os.makedirs(os.path.join(d, "rx"))
        open(os.path.join(d, "rx", "rx.log"), "w").write(log)
        open(os.path.join(d, "rx", "time.txt"), "w").write(time)
        return d

    def score(self, d):
        out = subprocess.run([sys.executable, SCORE, "--json", d], capture_output=True, text=True, check=True).stdout
        return json.loads(out.strip().splitlines()[-1])

    def test_full_run(self):
        s = self.score(self.make_arm())
        self.assertAlmostEqual(s["ttc_s"], 0.745, places=3)
        self.assertEqual(s["n_converged"], 2)
        self.assertEqual(s["bank_len"], 46)
        self.assertAlmostEqual(s["crc_pct"], 99.11, places=2)
        self.assertAlmostEqual(s["drop_full_pct"], 0.0398, places=4)
        self.assertEqual(s["cpu_pct"], 299)

    def test_run_that_never_synced_scores_nulls_not_crash(self):
        s = self.score(self.make_arm(log="4.2 [PHY] Initial sync: pbch not decoded on any branch\n", time="rx_rc=124\n"))
        self.assertIsNone(s["sync_s"]); self.assertIsNone(s["crc_pct"]); self.assertEqual(s["n_converged"], 0)

    def test_ansi_colours_are_stripped(self):
        s = self.score(self.make_arm(log="\x1b[32m5.0 [PHY]    Initial sync successful, PCI: 0\x1b[0m\n"))
        self.assertEqual(s["sync_s"], 5.0)

    @unittest.skipUnless(os.path.exists(FIXTURE), "fixture base_r1_rx.log not yet created (plan Task A1 Step 7)")
    def test_real_baseline_fixture(self):
        d = tempfile.mkdtemp(); os.makedirs(os.path.join(d, "rx"))
        shutil.copy(FIXTURE, os.path.join(d, "rx", "rx.log"))
        s = self.score(d)
        self.assertIsNotNone(s["sync_s"])
        self.assertGreaterEqual(s["n_converged"], 1)

if __name__ == "__main__":
    unittest.main()
```

- [x] **Step 3: Run it to verify it fails**

Run: `python3 tests/passive_rx/dgx/test_score_rx.py -v`
Expected: FAIL (`score_rx.py` does not exist → `CalledProcessError`/`FileNotFoundError`).

- [x] **Step 4: Write `score_rx.py`**

```python
#!/usr/bin/env python3
"""Score DGX rfsim/OTA arm dirs (rx/rx.log with a seconds prefix per line, rx/time.txt) -> JSON."""
import json, os, re, sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")

def _t(line):
    try:
        return float(line.split(" ", 1)[0])
    except ValueError:
        return None

def score(arm):
    rxd = os.path.join(arm, "rx") if os.path.isdir(os.path.join(arm, "rx")) else arm
    s = dict(arm=os.path.basename(os.path.normpath(arm)), sync_s=None, first_crnti_s=None, conv_s=None, ttc_s=None,
             n_converged=0, bank_len=None, ldpc_ok=None, ldpc_seg_fail=None, pdsch_decoded=None, pdsch_crc_ok=None,
             crc_pct=None, scanq_queued=None, scanq_drop_full=None, drop_full_pct=None, cpu_pct=None, max_rss_kb=None)
    try:
        lines = [ANSI.sub("", l) for l in open(os.path.join(rxd, "rx.log"), errors="replace")]
    except FileNotFoundError:
        lines = []
    for l in lines:
        if s["sync_s"] is None and "Initial sync successful" in l:
            s["sync_s"] = _t(l)
        if s["first_crnti_s"] is None and "rnti=0x1234" in l:
            s["first_crnti_s"] = _t(l)
        if "Technique D CONVERGED" in l:
            s["n_converged"] += 1
            if s["conv_s"] is None:
                s["conv_s"] = _t(l)
        m = re.search(r"bank add .*len=(\d+)", l)
        if m and s["bank_len"] is None:
            s["bank_len"] = int(m.group(1))
        m = re.search(r"LDPCDIAG ok=(\d+) seg_fail=(\d+)", l)
        if m:
            s["ldpc_ok"], s["ldpc_seg_fail"] = int(m.group(1)), int(m.group(2))
        m = re.search(r"PDSCHQ queued=\d+ decoded=(\d+) crc_ok=(\d+)", l)
        if m:
            s["pdsch_decoded"], s["pdsch_crc_ok"] = int(m.group(1)), int(m.group(2))
        m = re.search(r"scanq\[queued=(\d+) done=\d+ drop_full=(\d+)", l)
        if m:
            s["scanq_queued"], s["scanq_drop_full"] = int(m.group(1)), int(m.group(2))
    if s["first_crnti_s"] is not None and s["conv_s"] is not None:
        s["ttc_s"] = round(s["conv_s"] - s["first_crnti_s"], 3)
    if s["pdsch_decoded"]:
        s["crc_pct"] = round(100.0 * s["pdsch_crc_ok"] / s["pdsch_decoded"], 2)
    if s["scanq_queued"]:
        s["drop_full_pct"] = round(100.0 * s["scanq_drop_full"] / s["scanq_queued"], 4)
    try:
        for l in open(os.path.join(rxd, "time.txt")):
            if "Percent of CPU" in l:
                s["cpu_pct"] = int(l.rsplit(":", 1)[1].strip().rstrip("%") or 0)
            if "Maximum resident" in l:
                s["max_rss_kb"] = int(l.rsplit(":", 1)[1])
    except FileNotFoundError:
        pass
    return s

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--json"]
    for a in args:
        print(json.dumps(score(a), sort_keys=True))
```

- [x] **Step 5: Run the test to verify it passes**

Run: `python3 tests/passive_rx/dgx/test_score_rx.py -v`
Expected: 3 tests OK + 1 skipped (`test_real_baseline_fixture` skips until the Step 7 fixture exists; with the fixture present: 4 OK). Plan fix: 57897/58414 = 99.11 %, not 99.12 (arithmetic error in the original expectation).

- [x] **Step 6: Write `rfsim_regress.sh`** (gate thresholds overridable via env `GATE_CRC_MIN`/`GATE_DROP_MAX`; defaults are the DGX values 98.0 / 1.0)

```bash
#!/bin/bash
# Regression gate: N x 150 s 106-PRB fully agnostic baseline. Exit 0 iff all pass.
# Thresholds are overridable per host: GATE_CRC_MIN (default 98.0, DGX), GATE_DROP_MAX (default 1.0, DGX),
# e.g. a CPU-limited cloud host may use a re-baselined threshold.
set -u
export GATE_CRC_MIN=${GATE_CRC_MIN:-98.0} GATE_DROP_MAX=${GATE_DROP_MAX:-1.0}
H=$(cd "$(dirname "$0")" && pwd); N=${1:-2}; OUT=${OUT:-/tmp/rfsim_regress_$(date +%Y%m%d_%H%M%S)}
rc=0
for i in $(seq 1 "$N"); do
  "$H/rfsim_arm.sh" "$OUT/base_r$i" 150 >/dev/null 2>&1
  j=$(python3 "$H/score_rx.py" --json "$OUT/base_r$i"); echo "$j"
  python3 - "$j" <<'EOF' || rc=1
import json, os, sys
s = json.loads(sys.argv[1])
ok = (s["n_converged"] >= 1 and (s["crc_pct"] or 0) >= float(os.environ["GATE_CRC_MIN"])
      and s["drop_full_pct"] is not None and s["drop_full_pct"] <= float(os.environ["GATE_DROP_MAX"]))
print(("PASS " if ok else "FAIL ") + s["arm"]); sys.exit(0 if ok else 1)
EOF
done
echo "evidence: $OUT"; exit $rc
```

- [x] **Step 7: Run the gate on current HEAD (establishes the baseline)**

Run: `chmod +x tests/passive_rx/dgx/*.sh && tests/passive_rx/dgx/rfsim_regress.sh 2`
Expected: two `PASS base_rN` lines (HEAD measured 98.6–99.1 % on 2026-09-30/10-01), exit 0. Cloud-host notes (2026-10-01): the arm auto-applies `V4SHIM` (kernel without IPv6; `v4only_shim.c` LD_PRELOAD for the gNB) and `SCANTHREAD` (nproc<=5: unpinned `--sensing.pdcch_blind_monitor_scan_thread 1:8:-1`; frozen conf pins core 5); HEAD measured crc 94.9-96.9 % / drop_full 0.8-1.5 % there, so use `GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5` on that host (evidence: `tests/passive_rx/cloud_run_2026-10-01/a1_baseline/`). The fixture additionally keeps the first rnti line plus all `bank add`/`CONVERGED` lines between head and tail (convergence falls in the middle of a 150 s log), ANSI stripped. Then create the fixture: `mkdir -p tests/passive_rx/dgx/fixtures && F=tests/passive_rx/dgx/fixtures/base_r1_rx.log && head -400 $OUT/base_r1/rx/rx.log > $F && tail -400 $OUT/base_r1/rx/rx.log >> $F`.

- [ ] **Step 8: Commit**

```bash
git add tests/passive_rx/dgx/rfsim_arm.sh tests/passive_rx/dgx/score_rx.py tests/passive_rx/dgx/rfsim_regress.sh tests/passive_rx/dgx/test_score_rx.py tests/passive_rx/dgx/fixtures/base_r1_rx.log
git commit -m "test(dgx): in-tree rfsim regression gate with JSON arm scores"
```

---

### Task A2: Machine-readable receiver metrics (`ISAC_METRICS` JSON) (Sonnet)

Today every counter is free text parsed by regex (§11, research 2026-10-01). Add one snapshot struct, one JSON serializer (unit-tested), getters for the file-static counters, and emit at the existing 20 s `sum_due` site.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (add a counters getter near the counters at ~169/~1364; call the emitter inside `if (sum_due) {` at ~6696)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (getter for `g_ldpc_*` at ~651-662)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` (getter for `g_try`, `g_crc_ok`)
- Modify: `CMakeLists.txt:1464` (add `nr_passive_metrics.c` to `nr_pdcch_blind_monitor`), and add a test block next to `test_nr_scrambling_id_sweep` (~2538)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_metrics_test.cc`

**Interfaces:**
- Produces (header):

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PASSIVE_METRICS_H
#define NR_PASSIVE_METRICS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_PASSIVE_METRICS_SCHEMA 1
typedef struct {
  uint64_t t_mono_ns;       // CLOCK_MONOTONIC at snapshot
  int64_t abs_slot;         // producer absolute slot (nr_ue_diag_producer_absolute_slot), -1 unknown
  int pci;                  // -1 unknown
  const char *acq_state;    // nr_passive_acq_state_name(), never NULL ("UNKNOWN")
  uint64_t acq_transitions, acq_sync_losses, acq_pbch_locks, acq_sib1_decodes;
  uint64_t pdcch_occasions, pdcch_candidates, pdcch_accepts, pdcch_accepts_c;
  uint64_t scanq_queued, scanq_processed, scanq_drop_full, scanq_drop_stale, scanq_max_lag;
  uint64_t pdschq_queued, pdschq_decoded, pdschq_crc_ok, pdschq_drop_full, pdschq_drop_stale, pdschq_max_lag;
  uint64_t ldpc_ok, ldpc_seg_fail, ldpc_tb_fail, ldpc_zero_tb;
  uint64_t pusch_try, pusch_crc_ok;
  uint64_t obs_pushed, obs_written, obs_dropped; // filled by Task A3, 0 until then
} nr_passive_metrics_t;
/* Serialize to one JSON object (no newline). Returns bytes written (excl. NUL) or -1 if buf too small. */
int nr_passive_metrics_to_json(const nr_passive_metrics_t *m, char *buf, size_t n);
/* Fill a snapshot from all live getters (receiver side). */
void nr_passive_metrics_collect(nr_passive_metrics_t *m);
/* LOG_A "SENSING: ISAC_METRICS {json}" and, if env ISAC_METRICS_PATH is set, append the JSON line to that file
 * (opened once, line-buffered; this runs every 20 s on the summary path, not per slot). */
void nr_passive_metrics_emit(void);
#ifdef __cplusplus
}
#endif
#endif
```

- Produces (getters, declare in the corresponding existing headers):
  - `void nr_pdcch_blind_monitor_counters(uint64_t *occasions, uint64_t *candidates, uint64_t *accepts, uint64_t *accepts_c);` in `nr_pdcch_blind_monitor_rt.h`
  - `void nr_pdsch_passive_ldpc_counters(uint64_t *ok, uint64_t *seg_fail, uint64_t *tb_fail, uint64_t *zero_tb);` in `nr_pdsch_passive_decode.h`
  - `void nr_pusch_passive_counters(uint64_t *try_, uint64_t *crc_ok);` in `nr_pusch_passive_decode.h`
- Consumes: `nr_pdcch_passive_queue_get_stats()`, `nr_pdsch_passive_queue_get_stats()`, `nr_passive_acq_snapshot()`, `nr_passive_acq_state_name()` (all exist).

- [x] **Step 1: Write the failing gtest `nr_passive_metrics_test.cc`**

```cpp
#include <gtest/gtest.h>
#include <string>
extern "C" {
#include "nr_passive_metrics.h"
}

TEST(PassiveMetrics, SerializesAllFieldsAsOneJsonObject) {
  nr_passive_metrics_t m = {};
  m.t_mono_ns = 123; m.abs_slot = 456; m.pci = 64; m.acq_state = "TRACKING";
  m.pdschq_decoded = 58414; m.pdschq_crc_ok = 57897; m.scanq_queued = 404224; m.scanq_drop_full = 161;
  char buf[2048];
  const int n = nr_passive_metrics_to_json(&m, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  EXPECT_EQ(s.front(), '{'); EXPECT_EQ(s.back(), '}');
  EXPECT_EQ(s.find('\n'), std::string::npos);
  EXPECT_NE(s.find("\"schema\":1"), std::string::npos);
  EXPECT_NE(s.find("\"acq_state\":\"TRACKING\""), std::string::npos);
  EXPECT_NE(s.find("\"pdschq_crc_ok\":57897"), std::string::npos);
  EXPECT_NE(s.find("\"scanq_drop_full\":161"), std::string::npos);
  EXPECT_NE(s.find("\"pci\":64"), std::string::npos);
}

TEST(PassiveMetrics, ReturnsMinusOneWhenBufferTooSmall) {
  nr_passive_metrics_t m = {};
  m.acq_state = "SEARCHING";
  char buf[16];
  EXPECT_EQ(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), -1);
}

TEST(PassiveMetrics, NullStateNameIsReportedAsUnknown) {
  nr_passive_metrics_t m = {};
  char buf[2048];
  ASSERT_GT(nr_passive_metrics_to_json(&m, buf, sizeof(buf)), 0);
  EXPECT_NE(std::string(buf).find("\"acq_state\":\"UNKNOWN\""), std::string::npos);
}
```

- [x] **Step 2: Add the CMake test block** (after the `test_nr_scrambling_id_sweep` block, ~2544). The serializer is split into its own translation unit section so the test does not need the receiver:

```cmake
  add_executable(test_nr_passive_metrics ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_passive_metrics_test.cc
                                         ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_passive_metrics_json.c)
  target_include_directories(test_nr_passive_metrics PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_passive_metrics PRIVATE GTest::gtest GTest::gtest_main)
  add_dependencies(tests test_nr_passive_metrics)
  add_test(NAME test_nr_passive_metrics COMMAND ./test_nr_passive_metrics)
```

(So: `nr_passive_metrics_json.c` holds only `nr_passive_metrics_to_json`; `nr_passive_metrics.c` holds `collect`/`emit`. Add both to the `nr_pdcch_blind_monitor` library at CMakeLists.txt:1464.)

- [x] **Step 3: Run to verify it fails**

Run: `cd cmake_targets/ran_build/build && cmake . >/dev/null && ninja test_nr_passive_metrics`
Expected: FAIL — `nr_passive_metrics_json.c` not found / undefined reference.

- [x] **Step 4: Write `nr_passive_metrics_json.c`**

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_metrics.h"
#include <inttypes.h>
#include <stdio.h>

int nr_passive_metrics_to_json(const nr_passive_metrics_t *m, char *buf, size_t n)
{
  const char *st = m->acq_state ? m->acq_state : "UNKNOWN";
  const int w = snprintf(buf, n,
      "{\"schema\":%d,\"t_mono_ns\":%" PRIu64 ",\"abs_slot\":%" PRId64 ",\"pci\":%d,\"acq_state\":\"%s\","
      "\"acq_transitions\":%" PRIu64 ",\"acq_sync_losses\":%" PRIu64 ",\"acq_pbch_locks\":%" PRIu64 ",\"acq_sib1_decodes\":%" PRIu64 ","
      "\"pdcch_occasions\":%" PRIu64 ",\"pdcch_candidates\":%" PRIu64 ",\"pdcch_accepts\":%" PRIu64 ",\"pdcch_accepts_c\":%" PRIu64 ","
      "\"scanq_queued\":%" PRIu64 ",\"scanq_processed\":%" PRIu64 ",\"scanq_drop_full\":%" PRIu64 ",\"scanq_drop_stale\":%" PRIu64 ",\"scanq_max_lag\":%" PRIu64 ","
      "\"pdschq_queued\":%" PRIu64 ",\"pdschq_decoded\":%" PRIu64 ",\"pdschq_crc_ok\":%" PRIu64 ",\"pdschq_drop_full\":%" PRIu64 ",\"pdschq_drop_stale\":%" PRIu64 ",\"pdschq_max_lag\":%" PRIu64 ","
      "\"ldpc_ok\":%" PRIu64 ",\"ldpc_seg_fail\":%" PRIu64 ",\"ldpc_tb_fail\":%" PRIu64 ",\"ldpc_zero_tb\":%" PRIu64 ","
      "\"pusch_try\":%" PRIu64 ",\"pusch_crc_ok\":%" PRIu64 ","
      "\"obs_pushed\":%" PRIu64 ",\"obs_written\":%" PRIu64 ",\"obs_dropped\":%" PRIu64 "}",
      NR_PASSIVE_METRICS_SCHEMA, m->t_mono_ns, m->abs_slot, m->pci, st,
      m->acq_transitions, m->acq_sync_losses, m->acq_pbch_locks, m->acq_sib1_decodes,
      m->pdcch_occasions, m->pdcch_candidates, m->pdcch_accepts, m->pdcch_accepts_c,
      m->scanq_queued, m->scanq_processed, m->scanq_drop_full, m->scanq_drop_stale, m->scanq_max_lag,
      m->pdschq_queued, m->pdschq_decoded, m->pdschq_crc_ok, m->pdschq_drop_full, m->pdschq_drop_stale, m->pdschq_max_lag,
      m->ldpc_ok, m->ldpc_seg_fail, m->ldpc_tb_fail, m->ldpc_zero_tb,
      m->pusch_try, m->pusch_crc_ok,
      m->obs_pushed, m->obs_written, m->obs_dropped);
  return (w < 0 || (size_t)w >= n) ? -1 : w;
}
```

- [x] **Step 5: Run the test to verify it passes**

Run: `ninja test_nr_passive_metrics && ./test_nr_passive_metrics`
Expected: 3 tests PASS.

- [x] **Step 6: Add the three getters** (exact code):

In `nr_pdcch_blind_monitor_rt.c` (below the counter definitions, ~1375):
```c
void nr_pdcch_blind_monitor_counters(uint64_t *occasions, uint64_t *candidates, uint64_t *accepts, uint64_t *accepts_c)
{
  /* Plain uint64 written by the single scan consumer; an aligned 64-bit load is not torn on aarch64/x86-64.
   * Values are monotonic counters for a 20 s metrics line, so a one-increment race is harmless. */
  *occasions = g_occasions_run;
  *candidates = g_candidates_run;
  *accepts = g_accepts;
  *accepts_c = g_accepts_class[NR_BLIND_RNTI_CLASS_C];
}
```
In `nr_pdsch_passive_decode.c` (after ~662):
```c
void nr_pdsch_passive_ldpc_counters(uint64_t *ok, uint64_t *seg_fail, uint64_t *tb_fail, uint64_t *zero_tb)
{
  *ok = atomic_load(&g_ldpc_ok);
  *seg_fail = atomic_load(&g_ldpc_seg_fail);
  *tb_fail = atomic_load(&g_ldpc_tb_fail);
  *zero_tb = atomic_load(&g_ldpc_zero_tb);
}
```
In `nr_pusch_passive_decode.c` (next to the `g_try`/`g_crc_ok` atomics):
```c
void nr_pusch_passive_counters(uint64_t *try_, uint64_t *crc_ok)
{
  *try_ = atomic_load(&g_try);
  *crc_ok = atomic_load(&g_crc_ok);
}
```
Add the prototypes to the three headers listed in Interfaces.

- [x] **Step 7: Write `nr_passive_metrics.c`**

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "common/utils/LOG/log.h"
#include "nr_passive_acq_state.h"
#include "nr_pdcch_passive_queue.h"
#include "nr_pdsch_passive_queue.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "nr_pdsch_passive_decode.h"
#include "nr_pusch_passive_decode.h"

extern _Atomic long nr_ue_diag_producer_absolute_slot; // executables/nr-ue.c
int nr_passive_metrics_pci = -1;                       // set by nr-ue.c once PBCH locks (-1 before)
/* Filled by Task A3; weak so this file links before A3 lands. */
__attribute__((weak)) void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped)
{
  *pushed = *written = *dropped = 0;
}

void nr_passive_metrics_collect(nr_passive_metrics_t *m)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  *m = (nr_passive_metrics_t){0};
  m->t_mono_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  m->abs_slot = (int64_t)nr_ue_diag_producer_absolute_slot;
  m->pci = nr_passive_metrics_pci;
  const nr_passive_acq_snapshot_t a = nr_passive_acq_snapshot();
  m->acq_state = nr_passive_acq_state_name(a.state);
  m->acq_transitions = a.transitions; m->acq_sync_losses = a.sync_losses;
  m->acq_pbch_locks = a.pbch_locks; m->acq_sib1_decodes = a.sib1_decodes;
  nr_pdcch_blind_monitor_counters(&m->pdcch_occasions, &m->pdcch_candidates, &m->pdcch_accepts, &m->pdcch_accepts_c);
  nr_pdcch_passive_queue_stats_t sq; nr_pdcch_passive_queue_get_stats(&sq);
  m->scanq_queued = sq.queued; m->scanq_processed = sq.processed; m->scanq_drop_full = sq.dropped_full;
  m->scanq_drop_stale = sq.dropped_stale; m->scanq_max_lag = sq.max_lag_slots;
  nr_pdsch_passive_queue_stats_t pq; nr_pdsch_passive_queue_get_stats(&pq);
  m->pdschq_queued = pq.queued; m->pdschq_decoded = pq.decoded; m->pdschq_crc_ok = pq.crc_ok;
  m->pdschq_drop_full = pq.dropped_full; m->pdschq_drop_stale = pq.dropped_stale; m->pdschq_max_lag = pq.max_lag_slots;
  nr_pdsch_passive_ldpc_counters(&m->ldpc_ok, &m->ldpc_seg_fail, &m->ldpc_tb_fail, &m->ldpc_zero_tb);
  nr_pusch_passive_counters(&m->pusch_try, &m->pusch_crc_ok);
  nr_passive_obs_stats(&m->obs_pushed, &m->obs_written, &m->obs_dropped);
}

void nr_passive_metrics_emit(void)
{
  static FILE *f = NULL;
  static int tried = 0;
  nr_passive_metrics_t m;
  char buf[2048];
  nr_passive_metrics_collect(&m);
  if (nr_passive_metrics_to_json(&m, buf, sizeof(buf)) < 0) {
    LOG_W(PHY, "SENSING: ISAC_METRICS buffer too small\n");
    return;
  }
  LOG_A(PHY, "SENSING: ISAC_METRICS %s\n", buf);
  if (!tried) {
    tried = 1;
    const char *p = getenv("ISAC_METRICS_PATH");
    if (p && *p) {
      f = fopen(p, "a");
      if (!f)
        LOG_W(PHY, "SENSING: ISAC_METRICS_PATH=%s cannot be opened, file output off\n", p);
      else
        setvbuf(f, NULL, _IOLBF, 0);
    }
  }
  if (f)
    fprintf(f, "%s\n", buf);
}
```

Also: in `executables/nr-ue.c`, right after the `nr_passive_acq_note_pbch_locked();` call (the ACQ_EVENT pbch_locked is logged inside that function, which has no frame_parms in scope), add `nr_passive_metrics_pci = UE->frame_parms.Nid_cell;` with `extern int nr_passive_metrics_pci;` next to the `nr_ue_diag_producer_absolute_slot` definition. `nr_passive_metrics.c` also needs `#include <stdatomic.h>` and uses an explicit relaxed atomic load of the slot counter. In CMake the two sources go on the `nr_pdcch_blind_monitor` library line (nr_dci11_pin.c is the last entry).

- [x] **Step 8: Call the emitter** — in `nr_pdcch_blind_monitor_rt.c` inside `if (sum_due) {` (~6696), as the last statement of that block:
```c
    nr_passive_metrics_emit(); /* machine-readable twin of the text summaries above (Task A2) */
```
and `#include "nr_passive_metrics.h"` at the top.

- [x] **Step 9: Build, ctest, rfsim gate, and check the new line**

Run:
```bash
cd cmake_targets/ran_build/build && ninja nr-uesoftmodem tests && ctest -j4 2>&1 | tail -8
cd ../../.. && ISAC_METRICS_PATH=/tmp/m.jsonl OUT=/tmp/a2 tests/passive_rx/dgx/rfsim_regress.sh 1
grep -c "ISAC_METRICS {" /tmp/a2/base_r1/rx/rx.log; tail -1 /tmp/m.jsonl | python3 -m json.tool | head -5
```
Expected: ctest only the known ARM failures; gate PASS; ≥ 6 `ISAC_METRICS` lines in 150 s; last JSON has `"acq_state"` and `pdschq_crc_ok` equal to the last text `PDSCHQ crc_ok=`. NOTE: `rfsim_arm.sh` must pass `ISAC_METRICS_PATH` through (it inherits the environment — verify it appears in `/proc/<pid>/environ` if the file stays empty).

- [x] **Step 10: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.h openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c \
  openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics_json.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_metrics_test.cc \
  openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h \
  openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h \
  openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.h \
  executables/nr-ue.c CMakeLists.txt
git commit -m "feat(rx): ISAC_METRICS JSON snapshot every summary period (+ ISAC_METRICS_PATH file)"
```

---

### Task A3: Per-grant observation API (`nr_passive_obs`) — the sensing API (Sonnet implements, Opus reviews the header before Step 3)

Implements the §21 record for DL and UL grants, independent of `ENABLE_ISAC_SENSING`, written as JSONL by a writer thread, never blocking decode.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c` (record→JSON + ring + writer thread)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (DL hook after the Technique D feedback block, ~1140, where `job`, `dec`, `st`, `ue` are in scope)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` (UL hook at the end of `nr_pusch_passive_decode()` (~1512), where `out` is final, so CRC-fail grants are recorded too)
- Modify: `executables/nr-uesoftmodem.c` (open at start from env `ISAC_OBS_PATH`; close at exit)
- Modify: `CMakeLists.txt` (source into `nr_pdcch_blind_monitor` lib; test block)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc`
- Schema documentation lives in the header comment of `nr_passive_obs.h` (single source); PROJECT_MEMORY §21 gets a pointer (A14).

**Interfaces:**
- Produces (header, the API contract downstream sensing consumes):

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * nr_passive_obs.h -- per-grant observation records (PROJECT_MEMORY §21), JSON Lines, schema 1.
 * THIS COMMENT IS THE SINGLE SOURCE OF THE SCHEMA. PROJECT_MEMORY §21 only points here.
 *
 * One JSON object per line, one line per PDSCH/PUSCH grant whose transport block was decoded
 * (TB CRC pass or fail). Written by nr-uesoftmodem when ISAC_OBS_PATH is set (file opened in append
 * mode). Keys are emitted in the order below. Every v1 record is a scheduled-DATA grant: dir=DL is
 * PDSCH data, dir=UL is PUSCH data.
 *
 * Unknown values: EVERY unknown is JSON null. In C a signed integer field holds -1 and a float field
 * holds NAN (non-finite floats are also emitted as null). No integer field has a valid negative value.
 * Unsigned fields (dir, rnti, dmrs_symb_pos, t_mono_ns) are always known.
 *
 * Versioning: adding a key keeps schema 1 (consumers MUST ignore unknown keys). Changing a key's
 * meaning/unit, removing a key, or emitting records that are not scheduled-data grants (SSB, CSI-RS,
 * PDCCH ...) requires NR_PASSIVE_OBS_SCHEMA 2.
 *
 * Coverage (v1): DL = deferred PDSCH queue consumer only (nr_pdsch_passive_queue.c), layout probes
 * excluded; the in-line DL decode used when the queue is not running is NOT recorded. UL = every
 * nr_pusch_passive_decode() call (both callers) with status OK / CRC_FAIL / ZERO_TB; UNSUPPORTED,
 * ERROR and cfr_only calls are not recorded.
 *
 * key                 C type    unit / meaning                                         unknown  DL source                          UL source
 * schema              (const)   NR_PASSIVE_OBS_SCHEMA                                  never    -                                  -
 * abs_slot            int64     receiver monotonic slot of the grant's samples (time   null     job.absolute_slot                  abs_slot arg (0 -> null)
 *                               axis for sensing)
 * t_mono_ns           uint64    CLOCK_MONOTONIC ns when the record was built = decode  never    clock_gettime                      clock_gettime
 *                               COMPLETION (includes queue latency; not air time)
 * frame               int16     SFN 0..1023 of the grant's slot                        null     job.frame_rx                       frame arg (PUSCH slot)
 * slot                int16     slot in frame of the grant (UL: DCI slot + k2)         null     job.nr_slot_rx                     slot arg
 * pci                 int16     physical cell id 0..1007                               null     frame_parms.Nid_cell               frame_parms.Nid_cell
 * dir                 uint8     "DL" | "UL" (JSON string)                              never    NR_OBS_DIR_DL                      NR_OBS_DIR_UL
 * rnti                uint16    CRC-recovered RNTI (decimal)                           never    job.rnti                           g->rnti
 * rnti_class          int8      nr_blind_rnti_class_t: 0 C,1 TC,2 SI,3 RA,4 P          null     job.rnti_class                     -1 (no class on UL)
 * start_rb            int16     lowest allocated PRB, CRB-indexed (BWP start + offset) null     BWPStart+freq_alloc.first_rb       g->bwp_start+g->start_rb
 * nb_rb               int16     allocated PRB COUNT (allocation may be non-contiguous) null     freq_alloc.num_rbs                 g->num_rb
 * start_sym           int8      first OFDM symbol S                                    null     dlsch_pdu.start_symbol             g->start_symbol
 * nb_sym              int8      symbol count L                                         null     dlsch_pdu.number_symbols           g->num_symbols
 * mcs                 int8      MCS index                                              null     job.grant.mcs                      g->mcs
 * mcs_table           int8      0 qam64, 1 qam256, 2 qam64LowSE                        null     job.grant.mcs_table                g->mcs_table
 * qm                  int8      modulation order (bits/symbol)                         null     dec.cw.qamModOrder                 out->qam_mod_order
 * nl                  int8      layers (rank)                                          null     dec.cw.Nl (DM-RS port count)       g->nrOfLayers
 * dmrs_symb_pos       uint16    DM-RS symbol bitmap, bit l = symbol l (decimal)        never    dlsch_pdu.dlDmrsSymbPos            g->ul_dmrs_symb_pos
 * dmrs_scrambling_id  int32     DM-RS scrambling identity 0..65535                     null     dlsch_pdu.dlDmrsScramblingId       g->ul_dmrs_scrambling_id
 * tbs                 int32     transport block size, BITS                             null     dec.cw.TBS                         out->tbs_bytes*8
 * harq_pid            int8      HARQ process                                           null(*)  job.grant.harq_pid                 g->harq_pid
 * rv                  int8      redundancy version as signalled                        null     job.grant.rv                       g->rv
 * ndi                 int8      new-data indicator as signalled                        null(*)  job.grant.ndi                      g->ndi
 * crc                 int8      1 TB CRC pass, 0 fail (nr_obs_crc_t)                   null(**) decode status                      out->status
 * nvar                float     DL noise variance, linear, receiver-internal int16^2   null     dec.nvar                           NAN
 *                               scale: compare only within one run / gain / config
 * snr_db              float     UL post-estimation SNR, dB, receiver-internal          null     NAN                                out->snr_db
 * fo_comp_hz          float     FO the receiver digitally removed from these samples   null     job.fo_hz                          fo_hz arg
 *                               before the FFT, Hz; + = received carrier above LO.
 *                               NOT a per-grant measurement; 0 = no digital comp.
 * delay_samples       float     UL DM-RS CIR peak offset vs the FFT window, samples at null     NAN                                out->est_delay
 *                               fs_hz; + = later. 0 also means "no clear peak"
 * carrier_hz          int64     carrier centre frequency of this direction, Hz         null     frame_parms.dl_CarrierFreq         frame_parms.ul_CarrierFreq
 * scs_khz             int16     subcarrier spacing, kHz (grant BW = nb_rb*12*scs_khz)  null     frame_parms.subcarrier_spacing/1e3 same
 * fs_hz               int64     receiver sample rate = N_fft * SCS, Hz                 null     frame_parms.samples_per_subframe*1e3 same
 *
 * (*)  null for DL grants with rnti_class SI/RA/P (DCI 1_0: field reserved/absent, TS 38.212 7.3.1.2.1).
 * (**) null for UL ZERO_TB: all-zero TB, the CRC passes by construction, so it is not a verified decode.
 * [KNOWN ISSUE] UL snr_db holds the CFR mean power in dB (not an SNR) when the noise estimate is 0
 *              (nr_pusch_passive_decode.c:1120 vs :1381).
 * Evidence: schema [IMPLEMENTED, NOT VALIDATED] until A3 Step 9 (rfsim) passes ([SIM VERIFIED]); no OTA.
 */
#ifndef NR_PASSIVE_OBS_H
#define NR_PASSIVE_OBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_PASSIVE_OBS_SCHEMA 1
typedef enum { NR_OBS_DIR_DL = 0, NR_OBS_DIR_UL = 1 } nr_obs_dir_t;
typedef enum { NR_OBS_CRC_NA = -1, NR_OBS_CRC_FAIL = 0, NR_OBS_CRC_OK = 1 } nr_obs_crc_t;
/* One observed grant; JSON key = field name except `dir` (string). See the table above. */
typedef struct {
  int64_t abs_slot;
  uint64_t t_mono_ns;
  int16_t frame, slot;
  int16_t pci;
  uint8_t dir;                   // nr_obs_dir_t
  uint16_t rnti;
  int8_t rnti_class;             // nr_blind_rnti_class_t, -1 unknown
  int16_t start_rb, nb_rb;       // CRB-indexed lowest PRB; PRB count
  int8_t start_sym, nb_sym;
  int8_t mcs, mcs_table, qm, nl; // nl = layers (rank)
  uint16_t dmrs_symb_pos;        // bitmap
  int32_t dmrs_scrambling_id;    // -1 unknown
  int32_t tbs;                   // BITS, -1 unknown
  int8_t harq_pid, rv, ndi;      // -1 unknown/absent
  int8_t crc;                    // nr_obs_crc_t
  float nvar;                    // DL only, NAN on UL
  float snr_db;                  // UL only, NAN on DL
  float fo_comp_hz;              // applied FO compensation, NAN unknown
  float delay_samples;           // UL only, NAN on DL
  int64_t carrier_hz;            // -1 unknown
  int16_t scs_khz;               // -1 unknown
  int64_t fs_hz;                 // -1 unknown
} nr_passive_obs_t;
/* Serialises one record as a single JSON object, no newline, NUL-terminated.
 * Returns the byte count (excluding NUL), or -1 if it does not fit in n. 1024 bytes always suffice. */
int nr_passive_obs_to_json(const nr_passive_obs_t *o, char *buf, size_t n);
/* Lifecycle. open/close are called from one controlling thread (any thread), never concurrently with
 * each other; push and stats may be called from any thread at any time, including before open and
 * after close.
 * open: appends to `path`, starts the writer thread, resets the counters. capacity = ring slots
 *       (whole records, any value >= 1). Returns false (and changes nothing) if already open,
 *       capacity == 0, or the file/ring cannot be created. Re-open after close is allowed. */
bool nr_passive_obs_open(const char *path, uint32_t capacity);
/* close: stops accepting pushes, writes every record already accepted, flushes, joins the writer,
 *        closes the file. A no-op when not open. */
void nr_passive_obs_close(void);
/* push: never blocks on I/O (a short mutex only). Copies *o into the ring. Returns true if accepted.
 *       When not open: returns false and counts nothing. When the ring is full: returns false and
 *       increments `dropped`. MT-safe, also against a concurrent close. */
bool nr_passive_obs_push(const nr_passive_obs_t *o);
/* stats: pushed = accepted, written = lines fully written to the file, dropped = ring-full rejects,
 *        since the last successful open (still readable after close). After close,
 *        pushed - written = records lost to I/O errors. Any pointer may be NULL. MT-safe. */
void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped);
#ifdef __cplusplus
}
#endif
#endif
```

- Consumes: nothing from other tasks. A2's `nr_passive_obs_stats` weak default is overridden automatically when this file links.

- [x] **Step 1: Opus review gate** — dispatch an Opus reviewer (superpowers:requesting-code-review) on the header above **before** implementing: check field coverage vs PROJECT_MEMORY §21, units, unknown-value conventions, MT-safety contract. Apply its fixes to the header in this plan section and in the file.

- [x] **Step 2: Write the failing test `nr_passive_obs_test.cc`**

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_passive_obs.h"
}

static nr_passive_obs_t sample() {
  nr_passive_obs_t o = {};
  o.abs_slot = 1000; o.frame = 12; o.slot = 3; o.pci = 64; o.dir = NR_OBS_DIR_DL; o.rnti = 0x4768; o.rnti_class = 0;
  o.start_rb = 0; o.nb_rb = 106; o.start_sym = 1; o.nb_sym = 13; o.mcs = 9; o.mcs_table = 0; o.qm = 2; o.nl = 1;
  o.dmrs_symb_pos = 0x804; o.dmrs_scrambling_id = 64; o.tbs = 25104; o.harq_pid = 3; o.rv = 0; o.ndi = 1;
  o.crc = NR_OBS_CRC_OK; o.nvar = 12.5f; o.snr_db = NAN; o.fo_comp_hz = -13.4f; o.delay_samples = NAN;
  o.carrier_hz = 3619200000LL; o.scs_khz = 30; o.fs_hz = 61440000;
  return o;
}

TEST(PassiveObs, JsonHasSchemaAndNullsForNan) {
  char buf[1024];
  const nr_passive_obs_t o = sample();
  const int n = nr_passive_obs_to_json(&o, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  EXPECT_NE(s.find("\"schema\":1"), std::string::npos);
  EXPECT_NE(s.find("\"dir\":\"DL\""), std::string::npos);
  EXPECT_NE(s.find("\"rnti\":18280"), std::string::npos);
  EXPECT_NE(s.find("\"snr_db\":null"), std::string::npos);
  EXPECT_NE(s.find("\"crc\":1"), std::string::npos);
  EXPECT_EQ(s.find('\n'), std::string::npos);
}

TEST(PassiveObs, UnknownsAreNullAndFloatsKeepPrecision) {
  char buf[1024];
  nr_passive_obs_t o = sample();
  o.harq_pid = -1; o.crc = NR_OBS_CRC_NA; o.carrier_hz = -1; o.abs_slot = -1; o.nvar = INFINITY;
  const int n = nr_passive_obs_to_json(&o, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  const std::string s(buf, n);
  for (const char *k : {"\"harq_pid\":null", "\"crc\":null", "\"carrier_hz\":null", "\"abs_slot\":null",
                        "\"nvar\":null", "\"fo_comp_hz\":-13.4", "\"fs_hz\":61440000", "\"scs_khz\":30"})
    EXPECT_NE(s.find(k), std::string::npos) << k;
  EXPECT_EQ(s.find("-1,"), std::string::npos);   // no integer sentinel leaks into JSON
  EXPECT_EQ(nr_passive_obs_to_json(&o, buf, 16), -1);
}

TEST(PassiveObs, PushWithoutOpenIsNoop) {
  const nr_passive_obs_t o = sample();
  EXPECT_FALSE(nr_passive_obs_push(&o));
}

TEST(PassiveObs, WritesEveryRecordFromManyThreads) {
  char path[] = "/tmp/obs_test_XXXXXX"; const int fd = mkstemp(path); close(fd);
  ASSERT_TRUE(nr_passive_obs_open(path, 1 << 16));
  EXPECT_FALSE(nr_passive_obs_open(path, 16)); // open twice is rejected
  std::vector<std::thread> th;
  for (int t = 0; t < 4; t++) th.emplace_back([] { nr_passive_obs_t o = sample(); for (int i = 0; i < 5000; i++) nr_passive_obs_push(&o); });
  for (auto &x : th) x.join();
  nr_passive_obs_close();
  { nr_passive_obs_t o = sample(); EXPECT_FALSE(nr_passive_obs_push(&o)); } // push after close is a no-op
  uint64_t p, w, d; nr_passive_obs_stats(&p, &w, &d);
  EXPECT_EQ(p, 20000u); EXPECT_EQ(d, 0u); EXPECT_EQ(w, 20000u);
  std::ifstream f(path); int lines = 0; std::string l; while (std::getline(f, l)) lines++;
  EXPECT_EQ(lines, 20000); unlink(path);
}

TEST(PassiveObs, FullRingDropsAndCountsInsteadOfBlocking) {
  char path[] = "/tmp/obs_test_XXXXXX"; const int fd = mkstemp(path); close(fd);
  setenv("ISAC_OBS_TEST_WRITER_PAUSE_MS", "300", 1); // writer sleeps first: forces the ring to fill
  ASSERT_TRUE(nr_passive_obs_open(path, 64));
  const nr_passive_obs_t o = sample(); int ok = 0;
  for (int i = 0; i < 1000; i++) ok += nr_passive_obs_push(&o);
  uint64_t p, w, d; nr_passive_obs_stats(&p, &w, &d);
  EXPECT_LE(ok, 64); EXPECT_EQ(p + d, 1000u); EXPECT_GE(d, 936u);
  nr_passive_obs_close(); unsetenv("ISAC_OBS_TEST_WRITER_PAUSE_MS"); unlink(path);
}
```

CMake block (next to A2's):
```cmake
  add_executable(test_nr_passive_obs ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc
                                     ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_passive_obs.c)
  target_include_directories(test_nr_passive_obs PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_passive_obs PRIVATE GTest::gtest GTest::gtest_main pthread)
  add_dependencies(tests test_nr_passive_obs)
  add_test(NAME test_nr_passive_obs COMMAND ./test_nr_passive_obs)
```
`nr_passive_obs.c` must therefore not include OAI logging (use `fprintf(stderr, ...)` only in the open-failure path) so the test links standalone.

- [x] **Step 3: Run to verify it fails**

Run: `cmake . >/dev/null && ninja test_nr_passive_obs`
Expected: FAIL (missing source).

- [x] **Step 4: Write `nr_passive_obs.c`** (mutex-protected ring — simplest correct MT-safe design; pushes are ~200/s so a mutex is not a bottleneck; the writer does I/O outside the lock):

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_obs.h"
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static nr_passive_obs_t *g_ring = NULL;
static uint32_t g_cap = 0, g_head = 0, g_count = 0;
static bool g_open = false, g_stop = false;
static FILE *g_f = NULL;
static pthread_t g_thr;
static _Atomic uint64_t g_pushed, g_written, g_dropped;

typedef struct {
  char *p;
  size_t n, w;
  bool ovf;
} jb_t;

static void jb_put(jb_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_put(jb_t *j, const char *fmt, ...)
{
  if (j->ovf)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int k = vsnprintf(j->p + j->w, j->n - j->w, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)k >= j->n - j->w) {
    j->ovf = true;
    return;
  }
  j->w += (size_t)k;
}

static void jb_int(jb_t *j, const char *key, int64_t v)
{
  if (v < 0)
    jb_put(j, ",\"%s\":null", key);
  else
    jb_put(j, ",\"%s\":%" PRId64, key, v);
}

static void jb_flt(jb_t *j, const char *key, float v)
{
  if (!isfinite(v))
    jb_put(j, ",\"%s\":null", key);
  else
    jb_put(j, ",\"%s\":%.7g", key, (double)v);
}

int nr_passive_obs_to_json(const nr_passive_obs_t *o, char *buf, size_t n)
{
  if (!o || !buf || n == 0)
    return -1;
  jb_t j = {buf, n, 0, false};
  jb_put(&j, "{\"schema\":%d", NR_PASSIVE_OBS_SCHEMA);
  jb_int(&j, "abs_slot", o->abs_slot);
  jb_put(&j, ",\"t_mono_ns\":%" PRIu64, o->t_mono_ns);
  jb_int(&j, "frame", o->frame);
  jb_int(&j, "slot", o->slot);
  jb_int(&j, "pci", o->pci);
  jb_put(&j, ",\"dir\":\"%s\",\"rnti\":%u", o->dir == NR_OBS_DIR_UL ? "UL" : "DL", (unsigned)o->rnti);
  jb_int(&j, "rnti_class", o->rnti_class);
  jb_int(&j, "start_rb", o->start_rb);
  jb_int(&j, "nb_rb", o->nb_rb);
  jb_int(&j, "start_sym", o->start_sym);
  jb_int(&j, "nb_sym", o->nb_sym);
  jb_int(&j, "mcs", o->mcs);
  jb_int(&j, "mcs_table", o->mcs_table);
  jb_int(&j, "qm", o->qm);
  jb_int(&j, "nl", o->nl);
  jb_put(&j, ",\"dmrs_symb_pos\":%u", (unsigned)o->dmrs_symb_pos);
  jb_int(&j, "dmrs_scrambling_id", o->dmrs_scrambling_id);
  jb_int(&j, "tbs", o->tbs);
  jb_int(&j, "harq_pid", o->harq_pid);
  jb_int(&j, "rv", o->rv);
  jb_int(&j, "ndi", o->ndi);
  jb_int(&j, "crc", o->crc);
  jb_flt(&j, "nvar", o->nvar);
  jb_flt(&j, "snr_db", o->snr_db);
  jb_flt(&j, "fo_comp_hz", o->fo_comp_hz);
  jb_flt(&j, "delay_samples", o->delay_samples);
  jb_int(&j, "carrier_hz", o->carrier_hz);
  jb_int(&j, "scs_khz", o->scs_khz);
  jb_int(&j, "fs_hz", o->fs_hz);
  jb_put(&j, "}");
  return j.ovf ? -1 : (int)j.w;
}

static void *writer(void *arg)
{
  (void)arg;
  /* Test hook only: a no-op unless the variable is set. Makes the ring fill so overflow can be tested. */
  const char *pause = getenv("ISAC_OBS_TEST_WRITER_PAUSE_MS");
  if (pause && atoi(pause) > 0)
    usleep((useconds_t)atoi(pause) * 1000);
  char line[1024];
  for (;;) {
    pthread_mutex_lock(&g_mu);
    if (g_count == 0 && !g_stop) {
      /* ring drained: flush so a live tail sees the lines (I/O outside the lock) */
      pthread_mutex_unlock(&g_mu);
      fflush(g_f);
      pthread_mutex_lock(&g_mu);
    }
    while (g_count == 0 && !g_stop)
      pthread_cond_wait(&g_cv, &g_mu);
    if (g_count == 0 && g_stop) {
      pthread_mutex_unlock(&g_mu);
      break;
    }
    const nr_passive_obs_t o = g_ring[g_head];
    g_head = (g_head + 1) % g_cap;
    g_count--;
    pthread_mutex_unlock(&g_mu);
    const int k = nr_passive_obs_to_json(&o, line, sizeof line);
    if (k > 0 && fwrite(line, 1, (size_t)k, g_f) == (size_t)k && fputc('\n', g_f) != EOF)
      atomic_fetch_add(&g_written, 1);
  }
  fflush(g_f);
  return NULL;
}

bool nr_passive_obs_open(const char *path, uint32_t capacity)
{
  pthread_mutex_lock(&g_mu);
  const bool already = g_open;
  pthread_mutex_unlock(&g_mu);
  if (already || capacity == 0 || !path)
    return false;
  g_f = fopen(path, "a");
  if (!g_f) {
    fprintf(stderr, "nr_passive_obs: cannot open %s\n", path);
    return false;
  }
  g_ring = calloc(capacity, sizeof(*g_ring));
  if (!g_ring) {
    fclose(g_f);
    g_f = NULL;
    return false;
  }
  g_cap = capacity;
  g_head = g_count = 0;
  g_stop = false;
  atomic_store(&g_pushed, 0);
  atomic_store(&g_written, 0);
  atomic_store(&g_dropped, 0);
  pthread_mutex_lock(&g_mu);
  g_open = true;
  pthread_mutex_unlock(&g_mu);
  if (pthread_create(&g_thr, NULL, writer, NULL) != 0) {
    pthread_mutex_lock(&g_mu);
    g_open = false;
    pthread_mutex_unlock(&g_mu);
    fclose(g_f);
    free(g_ring);
    g_f = NULL;
    g_ring = NULL;
    return false;
  }
  return true;
}

bool nr_passive_obs_push(const nr_passive_obs_t *o)
{
  pthread_mutex_lock(&g_mu);
  if (!g_open) {
    pthread_mutex_unlock(&g_mu);
    return false;
  }
  if (g_count == g_cap) {
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_dropped, 1);
    return false;
  }
  g_ring[(g_head + g_count) % g_cap] = *o;
  g_count++;
  atomic_fetch_add(&g_pushed, 1); /* inside the lock: written <= pushed at every instant */
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mu);
  return true;
}

void nr_passive_obs_close(void)
{
  pthread_mutex_lock(&g_mu);
  if (!g_open) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  g_open = false;
  g_stop = true; /* no push is accepted after this; the writer drains every accepted record */
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mu);
  pthread_join(g_thr, NULL);
  fclose(g_f);
  free(g_ring);
  g_ring = NULL;
  g_f = NULL;
}

void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped)
{
  if (pushed)
    *pushed = atomic_load(&g_pushed);
  if (written)
    *written = atomic_load(&g_written);
  if (dropped)
    *dropped = atomic_load(&g_dropped);
}
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `ninja test_nr_passive_obs && ./test_nr_passive_obs`
Expected: 5 tests PASS. If `FullRingDropsAndCountsInsteadOfBlocking` is flaky, raise the pause, never weaken the assertion.

- [x] **Step 6: DL hook** — in `nr_pdsch_passive_queue.c`, directly after the `Technique D Qm oracle` block (before `if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && !job.layout_probe)`), add:

```c
      if (!job.layout_probe && (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL)) {
        /* Per-grant observation record (Task A3; schema in nr_passive_obs.h). Non-blocking. */
        struct timespec ts_;
        clock_gettime(CLOCK_MONOTONIC, &ts_);
        const NR_DL_FRAME_PARMS *ofp_ = &ue->frame_parms;
        /* DCI 1_0 with SI/RA/P-RNTI: HARQ process / NDI are reserved or absent (TS 38.212 7.3.1.2.1) */
        const bool no_harq_ = job.rnti_class >= NR_BLIND_RNTI_CLASS_SI;
        const nr_passive_obs_t o_ = {
            .abs_slot = job.absolute_slot,
            .t_mono_ns = (uint64_t)ts_.tv_sec * 1000000000ull + (uint64_t)ts_.tv_nsec,
            .frame = (int16_t)job.frame_rx, .slot = (int16_t)job.nr_slot_rx, .pci = (int16_t)ofp_->Nid_cell,
            .dir = NR_OBS_DIR_DL, .rnti = job.rnti, .rnti_class = (int8_t)job.rnti_class,
            .start_rb = (int16_t)(job.dlsch_pdu.BWPStart + job.freq_alloc.first_rb),
            .nb_rb = (int16_t)job.freq_alloc.num_rbs,
            .start_sym = (int8_t)job.dlsch_pdu.start_symbol, .nb_sym = (int8_t)job.dlsch_pdu.number_symbols,
            .mcs = (int8_t)job.grant.mcs, .mcs_table = (int8_t)job.grant.mcs_table,
            .qm = (int8_t)dec.cw.qamModOrder, .nl = (int8_t)dec.cw.Nl,
            .dmrs_symb_pos = job.dlsch_pdu.dlDmrsSymbPos, .dmrs_scrambling_id = job.dlsch_pdu.dlDmrsScramblingId,
            .tbs = (int32_t)dec.cw.TBS,                                  /* bits */
            .harq_pid = no_harq_ ? -1 : (int8_t)job.grant.harq_pid, .rv = (int8_t)job.grant.rv,
            .ndi = no_harq_ ? -1 : (int8_t)job.grant.ndi,               /* NOT cw.new_data_indicator (forced 1) */
            .crc = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK ? NR_OBS_CRC_OK : NR_OBS_CRC_FAIL,
            .nvar = (float)dec.nvar, .snr_db = NAN, .fo_comp_hz = (float)job.fo_hz, .delay_samples = NAN,
            .carrier_hz = ofp_->dl_CarrierFreq ? (int64_t)ofp_->dl_CarrierFreq : -1,
            .scs_khz = (int16_t)(ofp_->subcarrier_spacing / 1000),
            .fs_hz = (int64_t)ofp_->samples_per_subframe * 1000};
        nr_passive_obs_push(&o_);
      }
```
(+ `#include "nr_passive_obs.h"`, `<math.h>`, `<time.h>`). If any field name differs in this tree, fix it from `nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h:464-527` — do not guess.

- [x] **Step 7: UL hook** — at the end of `nr_pusch_passive_decode()` (the function at ~1512; add before its final `return`), build the record from the grant `g` (`nr_pdcch_blind_ul_result_t`: `rnti, start_rb, num_rb, start_symbol, num_symbols, mcs, mcs_table, nrOfLayers, ul_dmrs_symb_pos, ul_dmrs_scrambling_id, harq_pid, rv, ndi`) and `out` (`status, qam_mod_order, tbs_bytes, snr_db, est_delay`): `.dir = NR_OBS_DIR_UL`, `.crc` = OK for `status==OK`, FAIL for `CRC_FAIL`, skip the record for `UNSUPPORTED/ERROR`, `.tbs = out->tbs_bytes*8`, `.nvar = NAN`, `.snr_db = out->snr_db`, `.delay_samples = out->est_delay`, `.fo_comp_hz = fo_hz`. Read the function once and use the exact local variable names; the implementer must quote the final code in the commit message body.

Final code (inserted after the PUSCHDIAG block, before `return ok;`; `g` is the FDRA-resolved grant, may be NULL):
```c
  if (g != NULL && (out->status == NR_PUSCH_PASSIVE_OK || out->status == NR_PUSCH_PASSIVE_CRC_FAIL ||
                    out->status == NR_PUSCH_PASSIVE_ZERO_TB)) {
    /* Per-grant observation record (Task A3; schema in nr_passive_obs.h). cfr_only calls end UNSUPPORTED. */
    struct timespec ts_;
    clock_gettime(CLOCK_MONOTONIC, &ts_);
    const NR_DL_FRAME_PARMS *ofp_ = &ue->frame_parms;
    const nr_passive_obs_t o_ = {
        .abs_slot = abs_slot ? (int64_t)abs_slot : -1,                  /* 0 = "derive" -> unknown */
        .t_mono_ns = (uint64_t)ts_.tv_sec * 1000000000ull + (uint64_t)ts_.tv_nsec,
        .frame = (int16_t)frame, .slot = (int16_t)slot, .pci = (int16_t)ofp_->Nid_cell,
        .dir = NR_OBS_DIR_UL, .rnti = g->rnti, .rnti_class = -1,
        .start_rb = (int16_t)(g->bwp_start + g->start_rb), .nb_rb = (int16_t)g->num_rb,
        .start_sym = (int8_t)g->start_symbol, .nb_sym = (int8_t)g->num_symbols,
        .mcs = (int8_t)g->mcs, .mcs_table = (int8_t)g->mcs_table, .qm = (int8_t)out->qam_mod_order,
        .nl = (int8_t)g->nrOfLayers, .dmrs_symb_pos = g->ul_dmrs_symb_pos,
        .dmrs_scrambling_id = g->ul_dmrs_scrambling_id, .tbs = (int32_t)out->tbs_bytes * 8,
        .harq_pid = (int8_t)g->harq_pid, .rv = (int8_t)g->rv, .ndi = (int8_t)g->ndi,
        .crc = out->status == NR_PUSCH_PASSIVE_OK ? NR_OBS_CRC_OK
             : out->status == NR_PUSCH_PASSIVE_CRC_FAIL ? NR_OBS_CRC_FAIL : NR_OBS_CRC_NA,
        .nvar = NAN, .snr_db = out->snr_db, .fo_comp_hz = (float)fo_hz, .delay_samples = (float)out->est_delay,
        .carrier_hz = ofp_->ul_CarrierFreq ? (int64_t)ofp_->ul_CarrierFreq : -1,
        .scs_khz = (int16_t)(ofp_->subcarrier_spacing / 1000),
        .fs_hz = (int64_t)ofp_->samples_per_subframe * 1000};
    nr_passive_obs_push(&o_);
  }
```

- [x] **Step 8: Open/close** — in `executables/nr-uesoftmodem.c`, after `nr_pdcch_blind_monitor_init();` (~253):
```c
  {
    const char *obs_path = getenv("ISAC_OBS_PATH");
    if (obs_path && *obs_path && nr_passive_obs_open(obs_path, 1u << 14))
      LOG_I(PHY, "SENSING: per-grant observations -> %s (ring 16384, drop-on-full)\n", obs_path);
  }
```
and `nr_passive_obs_close();` in the exit path (next to the existing blind-monitor/queue stop calls; grep `nr_pdsch_passive_queue_stop` for the place).

- [x] **Step 9: rfsim validation** (Review Focus 1 and 2)

Run:
```bash
ISAC_OBS_PATH=/tmp/obs.jsonl ISAC_METRICS_PATH=/tmp/m.jsonl OUT=/tmp/a3 tests/passive_rx/dgx/rfsim_regress.sh 1
python3 - <<'EOF'
import json
obs=[json.loads(l) for l in open('/tmp/obs.jsonl')]; m=[json.loads(l) for l in open('/tmp/m.jsonl')][-1]
dl=[o for o in obs if o['dir']=='DL']
print(len(dl), m['pdschq_decoded'], m['obs_dropped'], sum(o['crc']==1 for o in dl), m['pdschq_crc_ok'])
EOF
OUT=/tmp/a3_off tests/passive_rx/dgx/rfsim_regress.sh 1
```
Expected: gate PASS both with and without the env vars; `obs_dropped == 0`; `n_dl <= pdschq_decoded` (layout probes are excluded); `|n_dl_crc_ok - pdschq_crc_ok| <= 1 %` of `pdschq_crc_ok`; layout-probe share reported; CRC % with and without obs within run-to-run spread (ruling replaces "within 1 % of pdschq_decoded").

- [x] **Step 10: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c \
  openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_obs_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c \
  openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c executables/nr-uesoftmodem.c CMakeLists.txt
git commit -m "feat(rx): per-grant observation API (ISAC_OBS_PATH JSONL, non-blocking ring, DL+UL)"
```

---

### Task A4: Campaign runner with detailed per-campaign logs (Sonnet)

A campaign = a directory holding a manifest (who/what/where/when/which build/which config), and one sub-dir per run with the raw log, metrics, observations, thread profile, NIC counters and a machine-readable verdict. Works for rfsim now and OTA later (B-track) by passing a different command. Does NOT wrap or edit `run_arm.sh`.

**Files:**
- Create: `tests/passive_rx/campaign/campaign.py`
- Create: `tests/passive_rx/campaign/verdict.py`
- Test: `tests/passive_rx/campaign/test_campaign.py`

**Interfaces:**
- Consumes: `tests/passive_rx/dgx/score_rx.py:score(armdir) -> dict` (A1), metrics JSONL schema 1 (A2), obs JSONL schema 1 (A3).
- Produces CLI:
  `campaign.py new --name <slug> --site <text> --cell <text> --notes <text> [--root /home/nicola/NICOLA/campaigns]` → prints campaign dir `<root>/<YYYYmmdd>_<slug>/`
  `campaign.py run <campaign_dir> --arm <name> --secs N [--nic <ifname>] -- <command ...>` → creates `<campaign_dir>/runs/<NNN>_<arm>/` and runs the command with `cwd` = that dir, env `ISAC_METRICS_PATH=<run>/metrics.jsonl ISAC_OBS_PATH=<run>/obs.jsonl`.
  `campaign.py summarize <campaign_dir>` → writes `<campaign_dir>/summary.json` and `summary.md`.
- Produces files: `manifest.json`, `runs/<NNN>_<arm>/{cmd.txt,env.txt,run.json,rx.log,metrics.jsonl,obs.jsonl,nic.csv,thrprof.txt,verdict.json}`, `index.jsonl` (one line per run).
- `verdict.py: verdict(run_dir) -> dict` with keys `{"verdict": "VALID"|"VOID_NO_SYNC"|"VOID_RFSTALL"|"VOID_NIC_LOSS"|"VOID_NO_SIB1"|"INTERRUPTED", "reasons": [..], "score": <score_rx dict>, "last_metrics": <dict or null>}`.

- [x] **Step 1: Write the failing test `test_campaign.py`**

```python
import json, os, signal, subprocess, sys, tempfile, time, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); CAMP = os.path.join(HERE, "campaign.py")

def run(*a, **kw):
    return subprocess.run([sys.executable, CAMP, *a], capture_output=True, text=True, **kw)

class Campaign(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()

    def new(self):
        r = run("new", "--name", "t", "--site", "DEIB", "--cell", "unknown", "--notes", "unit", "--root", self.root, check=True)
        return r.stdout.strip()

    def test_manifest_records_build_and_host(self):
        d = self.new()
        m = json.load(open(os.path.join(d, "manifest.json")))
        for k in ("git_commit", "git_dirty", "hostname", "arch", "kernel", "created_utc", "site", "cell", "uhd_version", "sens6_frozen_ok"):
            self.assertIn(k, m)

    def test_run_captures_log_and_verdict(self):
        d = self.new()
        script = "for i in 1 2 3; do echo \"$i.0 [PHY]    Initial sync successful, PCI: 7\"; done; " \
                 "echo '{\"schema\":1,\"acq_state\":\"PBCH_LOCKED\"}' > \"$ISAC_METRICS_PATH\""
        run("run", d, "--arm", "fake", "--secs", "5", "--", "bash", "-c", script, check=True)
        rd = os.path.join(d, "runs", "001_fake")
        self.assertTrue(os.path.exists(os.path.join(rd, "rx.log")))
        v = json.load(open(os.path.join(rd, "verdict.json")))
        self.assertEqual(v["last_metrics"]["acq_state"], "PBCH_LOCKED")
        self.assertEqual(len(open(os.path.join(d, "index.jsonl")).read().splitlines()), 1)

    def test_rfstall_is_void(self):
        d = self.new()
        run("run", d, "--arm", "st", "--secs", "5", "--", "bash", "-c",
            "echo '1.0 [PHY] Initial sync successful, PCI: 1'; echo '2.0 SENSING: RFSTALL USRP_RX_READ reason=x'", check=True)
        v = json.load(open(os.path.join(d, "runs", "001_st", "verdict.json")))
        self.assertEqual(v["verdict"], "VOID_RFSTALL")

    def test_sigterm_mid_run_leaves_interrupted_record(self):
        d = self.new()
        p = subprocess.Popen([sys.executable, CAMP, "run", d, "--arm", "long", "--secs", "60", "--", "sleep", "60"])
        time.sleep(1.5); p.send_signal(signal.SIGTERM); p.wait(timeout=20)
        rj = json.load(open(os.path.join(d, "runs", "001_long", "run.json")))
        self.assertEqual(rj["status"], "interrupted")

if __name__ == "__main__":
    unittest.main()
```

- [x] **Step 2: Run to verify it fails**

Run: `python3 tests/passive_rx/campaign/test_campaign.py -v`
Expected: FAIL (campaign.py missing).

- [x] **Step 3: Write `verdict.py`**

```python
#!/usr/bin/env python3
"""Machine-readable verdict of one campaign run dir (receiver-oriented; sensing CPIs are not required)."""
import json, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dgx"))
from score_rx import score  # noqa: E402

FAULT = re.compile(r"RFSTALL|RFTSDISC|RXDISCONT|rx xport timed out|No CHDR|device_init failed|Assertion")

def _last_json(path):
    last = None
    try:
        for l in open(path, errors="replace"):
            l = l.strip()
            if l.startswith("{"):
                try:
                    last = json.loads(l)
                except json.JSONDecodeError:
                    continue
    except FileNotFoundError:
        pass
    return last

def _nic_loss(run_dir):
    try:
        rows = [l.strip().split(",") for l in open(os.path.join(run_dir, "nic.csv")) if l[0].isdigit()]
        return int(rows[-1][1]) - int(rows[0][1]) if len(rows) >= 2 else 0
    except (FileNotFoundError, IndexError, ValueError):
        return 0

def verdict(run_dir):
    s = score(run_dir)
    reasons, v = [], "VALID"
    try:
        rj = json.load(open(os.path.join(run_dir, "run.json")))
    except (FileNotFoundError, json.JSONDecodeError):
        rj = {}
    log = open(os.path.join(run_dir, "rx.log"), errors="replace").read() if os.path.exists(os.path.join(run_dir, "rx.log")) else ""
    if rj.get("status") == "interrupted":
        v = "INTERRUPTED"; reasons.append("runner interrupted")
    elif FAULT.search(log):
        v = "VOID_RFSTALL"; reasons.append(FAULT.search(log).group(0))
    elif _nic_loss(run_dir) > 0:
        v = "VOID_NIC_LOSS"; reasons.append("nic rx_missed_errors delta=%d" % _nic_loss(run_dir))
    elif s["sync_s"] is None:
        v = "VOID_NO_SYNC"; reasons.append("no 'Initial sync successful'")
    elif rj.get("expect_sib1") and "SIB1 decoded" not in log:
        v = "VOID_NO_SIB1"; reasons.append("expect_sib1 set but no SIB1")
    return {"verdict": v, "reasons": reasons, "score": s, "last_metrics": _last_json(os.path.join(run_dir, "metrics.jsonl"))}

if __name__ == "__main__":
    for d in sys.argv[1:]:
        print(json.dumps(verdict(d), sort_keys=True))
```

- [x] **Step 4: Write `campaign.py`**

```python
#!/usr/bin/env python3
"""Campaign runner: manifest + per-run capture + verdict + summary. Stops children with SIGINT (never SIGKILL first)."""
import argparse, datetime, json, os, platform, signal, socket, subprocess, sys, threading, time
HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)
from verdict import verdict  # noqa: E402

def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30).stdout.strip()
    except Exception as e:  # noqa: BLE001 - manifest must never fail because a probe failed
        return "error: %s" % e

def cmd_new(a):
    d = os.path.join(a.root, datetime.datetime.utcnow().strftime("%Y%m%d") + "_" + a.name)
    os.makedirs(os.path.join(d, "runs"), exist_ok=False)
    m = dict(created_utc=datetime.datetime.utcnow().isoformat() + "Z", site=a.site, cell=a.cell, notes=a.notes,
             hostname=socket.gethostname(), arch=platform.machine(), kernel=platform.release(),
             git_commit=sh("git -C %s rev-parse HEAD" % REPO), git_branch=sh("git -C %s rev-parse --abbrev-ref HEAD" % REPO),
             git_dirty=bool(sh("git -C %s status --porcelain --untracked-files=no" % REPO)),
             sens6_frozen_ok=subprocess.run(["git", "-C", REPO, "diff", "--quiet", "sens6-frozen-2026-09-30", "--",
                                             "tests/passive_rx/captures", "tests/passive_rx/sens6_host_snapshot_2026-09-30"]).returncode == 0,
             uhd_version=sh("uhd_config_info --version 2>/dev/null | head -1"), lscpu=sh("lscpu -e"),
             nvidia=sh("nvidia-smi --query-gpu=name,driver_version,compute_cap --format=csv,noheader 2>/dev/null"),
             cmdline=sh("cat /proc/cmdline"), ulimit_r=sh("bash -c 'ulimit -r'"))
    json.dump(m, open(os.path.join(d, "manifest.json"), "w"), indent=2)
    print(d)

def _nic_sampler(nic, path, stop):
    with open(path, "w") as f:
        f.write("epoch,rx_missed_errors,rx_packets\n")
        while not stop.is_set():
            try:
                miss = open("/sys/class/net/%s/statistics/rx_missed_errors" % nic).read().strip()
                pk = open("/sys/class/net/%s/statistics/rx_packets" % nic).read().strip()
                f.write("%d,%s,%s\n" % (time.time(), miss, pk)); f.flush()
            except OSError:
                pass
            stop.wait(1.0)

def cmd_run(a):
    runs = os.path.join(a.campaign, "runs"); n = len(os.listdir(runs)) + 1
    rd = os.path.join(runs, "%03d_%s" % (n, a.arm)); os.makedirs(rd)
    env = dict(os.environ, ISAC_METRICS_PATH=os.path.join(rd, "metrics.jsonl"), ISAC_OBS_PATH=os.path.join(rd, "obs.jsonl"))
    open(os.path.join(rd, "cmd.txt"), "w").write(" ".join(a.command) + "\n")
    open(os.path.join(rd, "env.txt"), "w").write("\n".join("%s=%s" % kv for kv in sorted(env.items()) if kv[0].startswith(("ISAC_", "NR_", "LDPC"))) + "\n")
    rj = dict(arm=a.arm, secs=a.secs, start_utc=datetime.datetime.utcnow().isoformat() + "Z", status="running", expect_sib1=a.expect_sib1)
    json.dump(rj, open(os.path.join(rd, "run.json"), "w"))
    stop = threading.Event()
    if a.nic:
        threading.Thread(target=_nic_sampler, args=(a.nic, os.path.join(rd, "nic.csv"), stop), daemon=True).start()
    t0 = time.time()
    with open(os.path.join(rd, "rx.log"), "w") as log:
        p = subprocess.Popen(a.command, cwd=rd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             start_new_session=True)
        interrupted = {"v": False}
        def on_sig(signum, frame):  # noqa: ARG001
            interrupted["v"] = True
            os.killpg(p.pid, signal.SIGINT)
        signal.signal(signal.SIGTERM, on_sig); signal.signal(signal.SIGINT, on_sig)
        deadline = t0 + a.secs + 30
        for line in p.stdout:
            log.write("%.3f %s" % (time.time() - t0, line) if not line[:1].isdigit() else line); log.flush()
            if time.time() > deadline:
                os.killpg(p.pid, signal.SIGINT); break
        try:
            p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGTERM); p.wait(timeout=30)
    stop.set()
    rj.update(end_utc=datetime.datetime.utcnow().isoformat() + "Z", rc=p.returncode,
              status="interrupted" if interrupted["v"] else "done", wall_s=round(time.time() - t0, 1))
    json.dump(rj, open(os.path.join(rd, "run.json"), "w"))
    v = verdict(rd); json.dump(v, open(os.path.join(rd, "verdict.json"), "w"), indent=2)
    with open(os.path.join(a.campaign, "index.jsonl"), "a") as f:
        f.write(json.dumps(dict(run=os.path.basename(rd), arm=a.arm, verdict=v["verdict"], score=v["score"])) + "\n")
    print(rd, v["verdict"])

def cmd_summarize(a):
    rows = [json.loads(l) for l in open(os.path.join(a.campaign, "index.jsonl"))]
    by = {}
    for r in rows:
        by.setdefault(r["arm"], []).append(r)
    summ = {arm: dict(n=len(rs), valid=sum(r["verdict"] == "VALID" for r in rs),
                      crc_pct=[r["score"]["crc_pct"] for r in rs], ttc_s=[r["score"]["ttc_s"] for r in rs],
                      drop_full_pct=[r["score"]["drop_full_pct"] for r in rs], verdicts=[r["verdict"] for r in rs])
            for arm, rs in by.items()}
    json.dump(summ, open(os.path.join(a.campaign, "summary.json"), "w"), indent=2)
    with open(os.path.join(a.campaign, "summary.md"), "w") as f:
        f.write("| arm | runs | VALID | CRC %% | ttc s | drop_full %% | verdicts |\n|---|---|---|---|---|---|---|\n")
        for arm, s in summ.items():
            f.write("| %s | %d | %d | %s | %s | %s | %s |\n" % (arm, s["n"], s["valid"], s["crc_pct"], s["ttc_s"], s["drop_full_pct"], s["verdicts"]))
    print(os.path.join(a.campaign, "summary.md"))

def main():
    ap = argparse.ArgumentParser(); sub = ap.add_subparsers(dest="c", required=True)
    n = sub.add_parser("new"); n.add_argument("--name", required=True); n.add_argument("--site", required=True)
    n.add_argument("--cell", required=True); n.add_argument("--notes", default="")
    n.add_argument("--root", default="/home/nicola/NICOLA/campaigns"); n.set_defaults(f=cmd_new)
    r = sub.add_parser("run"); r.add_argument("campaign"); r.add_argument("--arm", required=True)
    r.add_argument("--secs", type=int, required=True); r.add_argument("--nic"); r.add_argument("--expect-sib1", action="store_true")
    r.add_argument("command", nargs=argparse.REMAINDER); r.set_defaults(f=cmd_run)
    s = sub.add_parser("summarize"); s.add_argument("campaign"); s.set_defaults(f=cmd_summarize)
    a = ap.parse_args()
    if getattr(a, "command", None) and a.command[:1] == ["--"]:
        a.command = a.command[1:]
    a.f(a)

if __name__ == "__main__":
    main()
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `python3 tests/passive_rx/campaign/test_campaign.py -v`
Expected: 4 tests OK.

- [x] **Step 6: Real rfsim campaign smoke** (🔁 Haiku may run this step)

Run:
```bash
C=$(python3 tests/passive_rx/campaign/campaign.py new --name rfsim_smoke --site DGX-rfsim --cell "phy-test 106PRB PCI0" --root /tmp/campaigns)
python3 tests/passive_rx/campaign/campaign.py run $C --arm base --secs 150 -- tests/passive_rx/dgx/rfsim_arm.sh ./arm 150
python3 tests/passive_rx/campaign/campaign.py summarize $C && cat $C/summary.md
```
Expected: one `VALID` run. NOTE: `rfsim_arm.sh` writes its own `arm/rx/rx.log`; `verdict()` scores `run_dir` — so make `rfsim_arm.sh` accept `OUT=.` or teach `score()` to look into `arm/rx` (pick one; the implementer adds a unit test for the chosen layout).

> Layout decision (A4, cloud): `rfsim_arm.sh` and `score_rx.py` are unchanged. `verdict.arm_dir()` prefers an arm sub-dir (`<run>/*/rx/rx.log` or `<run>/*/rx.log`) over the runner-captured wrapper stdout `<run>/rx.log`, so the command above works as written. Unit-tested in `VerdictLayout`. Runner notes: `--` splits the child command before argparse (REMAINDER swallowed options); the deadline is polled (works for silent children); escalation SIGINT -> SIGTERM -> (last resort) SIGKILL, grace via `CAMPAIGN_GRACE_S` / `CAMPAIGN_TERM_GRACE_S` (default 30 s). Step 6 run is still pending (orchestrator).

- [x] **Step 7: Commit**

```bash
git add tests/passive_rx/campaign/campaign.py tests/passive_rx/campaign/verdict.py tests/passive_rx/campaign/test_campaign.py
git commit -m "feat(campaign): per-campaign manifest, per-run logs/metrics/obs/NIC, machine verdicts, summary"
```

---

### Task A5: Dashboard update — receiver health from metrics + observations (Sonnet, frontend-design skill)

The monitor today is sensing-oriented plus regex log tailing; RFCENSUS, scanq and the blind-monitor summary are not shown (research 2026-10-01). Add a structured receiver-health source (A2 metrics JSONL) and a per-grant source (A3 obs JSONL), a "Receiver health" tab, and robustness to missing/truncated files.

**Files:**
- Modify: `tests/passive_rx/monitor/monitor.py` (new `JsonlTail` class, `--metrics` and `--obs` options, `/health` endpoint)
- Modify: `tests/passive_rx/monitor/monitor.html` (new tab `Receiver health` added to `TABS` at ~102)
- Create: `tests/passive_rx/monitor/test_health.py` (unittest)

**Interfaces:**
- Consumes: metrics JSONL schema 1 (A2), obs JSONL schema 1 (A3), campaign run-dir layout (A4: `metrics.jsonl`, `obs.jsonl`).
- Produces: `GET /health` → `{"metrics": <last metrics object or null>, "metrics_age_s": float|null, "bad_lines": int, "rates": {"crc_pct_window": float|null, "grants_per_s": float|null, "drop_full_pct": float|null}, "obs": {"dl_per_s": float, "ul_per_s": float, "top_rnti": [[rnti, n], ...], "prb_hist": [n0..n9]}}`.
- Launch: `tests/passive_rx/.venv/bin/python tests/passive_rx/monitor/monitor.py --metrics <run>/metrics.jsonl --obs <run>/obs.jsonl --log <run>/rx.log --port 8080`.

- [x] **Step 1: Write the failing test `test_health.py`**

```python
import json, os, sys, tempfile, time, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from monitor import JsonlTail, health_snapshot  # noqa: E402

class Health(unittest.TestCase):
    def test_missing_file_is_no_data_not_error(self):
        t = JsonlTail("/nonexistent/metrics.jsonl"); t.poll()
        self.assertIsNone(t.last); self.assertEqual(t.bad_lines, 0)

    def test_truncated_line_is_skipped_and_counted(self):
        p = tempfile.mktemp(); open(p, "w").write('{"schema":1,"pdschq_decoded":10}\n{"schema":1,"pdsch')
        t = JsonlTail(p); t.poll()
        self.assertEqual(t.last["pdschq_decoded"], 10); self.assertEqual(t.bad_lines, 0)  # partial tail is pending, not bad
        open(p, "a").write('q_decoded":20}\n'); t.poll()
        self.assertEqual(t.last["pdschq_decoded"], 20)

    def test_garbage_line_counts_bad(self):
        p = tempfile.mktemp(); open(p, "w").write('not json\n{"schema":1}\n')
        t = JsonlTail(p); t.poll()
        self.assertEqual(t.bad_lines, 1)

    def test_file_truncated_restarts_from_zero(self):
        p = tempfile.mktemp(); open(p, "w").write('{"a":1}\n{"a":2}\n'); t = JsonlTail(p); t.poll()
        open(p, "w").write('{"a":3}\n'); t.poll()
        self.assertEqual(t.last["a"], 3)

    def test_window_crc_rate_from_two_snapshots(self):
        m = JsonlTail(None); o = JsonlTail(None)
        m.history = [{"t_mono_ns": 0, "pdschq_decoded": 100, "pdschq_crc_ok": 90, "scanq_queued": 1000, "scanq_drop_full": 0},
                     {"t_mono_ns": 20_000_000_000, "pdschq_decoded": 300, "pdschq_crc_ok": 285, "scanq_queued": 3000, "scanq_drop_full": 10}]
        m.last = m.history[-1]
        h = health_snapshot(m, o)
        self.assertAlmostEqual(h["rates"]["crc_pct_window"], 97.5)
        self.assertAlmostEqual(h["rates"]["grants_per_s"], 10.0)
        self.assertAlmostEqual(h["rates"]["drop_full_pct"], 0.5)

if __name__ == "__main__":
    unittest.main()
```

- [x] **Step 2: Run to verify it fails**

Run: `tests/passive_rx/.venv/bin/python tests/passive_rx/monitor/test_health.py -v`
Expected: FAIL (`ImportError: cannot import name 'JsonlTail'`).

- [x] **Step 3: Add to `monitor.py`** (module level, above the HTTP handler):

```python
class JsonlTail:
    """Incremental JSONL reader: tolerates a missing file, a partial last line, truncation/rotation."""
    def __init__(self, path, keep=512):
        self.path, self.keep = path, keep
        self.pos, self.pending, self.last, self.history, self.bad_lines = 0, "", None, [], 0

    def poll(self):
        if not self.path:
            return []
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return []
        if size < self.pos:  # truncated or rotated
            self.pos, self.pending = 0, ""
        new = []
        with open(self.path, errors="replace") as f:
            f.seek(self.pos)
            data = self.pending + f.read()
            self.pos = f.tell()
        lines = data.split("\n")
        self.pending = lines.pop()  # "" if data ended with a newline
        for l in lines:
            if not l.strip():
                continue
            try:
                obj = json.loads(l)
            except json.JSONDecodeError:
                self.bad_lines += 1
                continue
            new.append(obj)
        if new:
            self.history = (self.history + new)[-self.keep:]
            self.last = self.history[-1]
        return new


def health_snapshot(metrics, obs):
    h = {"metrics": metrics.last, "metrics_age_s": None, "bad_lines": metrics.bad_lines + obs.bad_lines,
         "rates": {"crc_pct_window": None, "grants_per_s": None, "drop_full_pct": None},
         "obs": {"dl_per_s": 0.0, "ul_per_s": 0.0, "top_rnti": [], "prb_hist": [0] * 10}}
    if len(metrics.history) >= 2:
        a, b = metrics.history[-2], metrics.history[-1]
        dt = (b["t_mono_ns"] - a["t_mono_ns"]) / 1e9
        dd = b["pdschq_decoded"] - a["pdschq_decoded"]
        if dd > 0:
            h["rates"]["crc_pct_window"] = 100.0 * (b["pdschq_crc_ok"] - a["pdschq_crc_ok"]) / dd
        if dt > 0:
            h["rates"]["grants_per_s"] = dd / dt
        dq = b["scanq_queued"] - a["scanq_queued"]
        if dq > 0:
            h["rates"]["drop_full_pct"] = 100.0 * (b["scanq_drop_full"] - a["scanq_drop_full"]) / dq
    recent = obs.history[-2000:]
    if len(recent) >= 2:
        span = max((recent[-1]["t_mono_ns"] - recent[0]["t_mono_ns"]) / 1e9, 1e-9)
        h["obs"]["dl_per_s"] = sum(o["dir"] == "DL" for o in recent) / span
        h["obs"]["ul_per_s"] = sum(o["dir"] == "UL" for o in recent) / span
        cnt = {}
        for o in recent:
            cnt[o["rnti"]] = cnt.get(o["rnti"], 0) + 1
            b = min(9, max(0, o["nb_rb"] * 10 // 275))
            h["obs"]["prb_hist"][b] += 1
        h["obs"]["top_rnti"] = sorted(cnt.items(), key=lambda kv: -kv[1])[:5]
    return h
```

Wire it: parse `--metrics` and `--obs` in the existing argparse block (~644-658); create `METRICS = JsonlTail(args.metrics)`, `OBS = JsonlTail(args.obs, keep=5000)`; poll both in the existing background loop that tails `--log` (or a new 1 s thread); add to the request handler (~601-635): `if self.path == "/health": return self._json(health_snapshot(METRICS, OBS))` (use the handler's existing JSON-reply helper name; read it first).

- [x] **Step 4: Run the tests to verify they pass**

Run: `tests/passive_rx/.venv/bin/python tests/passive_rx/monitor/test_health.py -v && tests/passive_rx/.venv/bin/python tests/passive_rx/monitor/test_monitor.py`
Expected: 5 new tests OK; the existing `test_monitor.py` still passes.

- [x] **Step 5: Add the "Receiver health" tab** to `monitor.html` (invoke the **frontend-design** skill first; follow the page's existing vanilla-JS style, no new libraries). Required content, polling `/health` every 2 s:
  - Tiles: acquisition state (colour by the REAL nr_passive_acq_state_name() names: DL_CONVERGED/UL_CONVERGED/TRACKING green, LOST/INVALID red, all other states amber), PCI, CRC % (window), grants/s, scanq drop_full % (red > 1 %), PDSCH queue drops, obs dropped (red > 0), metrics age (red > 45 s = receiver silent).
  - Sparkline of `crc_pct_window` and `grants_per_s` over the last 30 snapshots (canvas, same drawing helpers as the existing `dlMap`).
  - Table: top-5 RNTIs from observations; PRB-width histogram (10 bins).
  - An explicit "no data yet" state when `metrics` is null.

- [x] **Step 6: Manual check against a real run** (🔁 Haiku)

Run: start an rfsim arm with metrics+obs (A4 Step 6 campaign dir), then
`tests/passive_rx/.venv/bin/python tests/passive_rx/monitor/monitor.py --metrics <run>/metrics.jsonl --obs <run>/obs.jsonl --log <run>/rx.log --port 8080 &` and `curl -s localhost:8080/health | python3 -m json.tool | head -30`.
Expected: `acq_state` present, `grants_per_s` ≈ 380 at 106 PRB phy-test, `bad_lines` 0. Stop the monitor with `kill -INT`.

- [x] **Step 7: Commit**

```bash
git add tests/passive_rx/monitor/monitor.py tests/passive_rx/monitor/monitor.html tests/passive_rx/monitor/test_health.py
git commit -m "feat(monitor): receiver-health tab from ISAC_METRICS and per-grant observations (/health)"
```

---

> A5 implementation notes (2026-10-01): `JsonlTail` reads bytes, caps each poll at 4 MiB (seeks to the tail and drops the first partial line), resets on truncation, inode change or file disappearance; `metrics_age_s` is wall-clock now minus the metrics file mtime (the receiver's monotonic clock is not comparable); null `nb_rb`/`rnti` in obs are skipped; handler gets `metrics`/`obs` args; the tab skips the log panels so it sits at the top. Tests: 10 in `test_health.py`.

### Task A6: DGX core map and launcher (Sonnet; 🔁 Haiku for the A/B repeats)

Implements §14.3 as a new launcher (sens6's `run_arm.sh` untouched) and measures it.

**Files:**
- Create: `tests/passive_rx/dgx/coremap_dgx.env`
- Create: `tests/passive_rx/dgx/run_rx_dgx.sh`
- Create: `tests/passive_rx/dgx/test_run_rx_dgx.py` (dry-run tests)
- Modify: `tests/passive_rx/dgx/rfsim_arm.sh` (optional `COREMAP=1` → prefix the receiver with the pinning env/args from `coremap_dgx.env`)

**Interfaces:**
- Produces: `run_rx_dgx.sh [--dry-run] -- <nr-uesoftmodem args...>`; env `INSTANCE=A|B` (cluster 0 / cluster 1). Prints `COREMAP applied: ...` and execs the receiver with: `ISAC_UE_RT_CORE`, `ISAC_PDCCH_USS_CORE`, `--thread-pool`, `--sync-actor-core`, `--dl-actor-core-start`, `--ul-actor-core-start`, and a generated `-O` conf **overlay is NOT used** — the conf-embedded cores (`scan_thread`, `pdsch`, `ul_thread`) must be set by the caller's conf; the launcher only checks and warns if the conf pins to an A725 core for the scan thread.
- `coremap_dgx.env` content (instance A; B = +10):

```bash
# DGX Spark GB10: cluster0 = cpus 0-9 (A725: 0-4, X925: 5-9); cluster1 = 10-19 (A725: 10-14, X925: 15-19). PROJECT_MEMORY 14.3
A_RT_CORE=5        # UEthread_0 (RF read + per-slot RT path)
A_SCAN_CORE=6      # passivePdcch0 -> conf: pdcch_blind_monitor_scan_thread = "1:8:6"
A_USS_CORE=7       # pdcchUssHash
A_TPOOL="8,9,0,1"  # --thread-pool
A_PDSCH_FIRST=2    # conf: pdcch_blind_monitor_pdsch = "...:3:<depth>:2"  -> cores 2,3,4
A_ACTORS=0         # sync/dl/ul actors start core (A725)
```

- [x] **Step 1: Write the failing dry-run tests** (`test_run_rx_dgx.py`):

```python
import os, subprocess, unittest
H = os.path.dirname(os.path.abspath(__file__)); L = os.path.join(H, "run_rx_dgx.sh")

def dry(env=None, args=("--passive-rx",)):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([L, "--dry-run", "--", *args], capture_output=True, text=True, env=e)

class Launcher(unittest.TestCase):
    def test_instance_a_pins_rt_to_x925_core5(self):
        r = dry({"INSTANCE": "A"}); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ISAC_UE_RT_CORE=5", r.stdout); self.assertIn("--thread-pool 8,9,0,1", r.stdout)

    def test_instance_b_is_cluster1(self):
        r = dry({"INSTANCE": "B"}); self.assertIn("ISAC_UE_RT_CORE=15", r.stdout); self.assertIn("--thread-pool 18,19,10,11", r.stdout)

    def test_refuses_offline_cpu(self):
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-3"}); self.assertNotEqual(r.returncode, 0)
        self.assertIn("not online", r.stderr)

if __name__ == "__main__":
    unittest.main()
```

- [x] **Step 2: Run to verify it fails**

Run: `python3 tests/passive_rx/dgx/test_run_rx_dgx.py -v` → FAIL (launcher missing).

- [x] **Step 3: Write `run_rx_dgx.sh`**

```bash
#!/bin/bash
# DGX receiver launcher with the X925 core map (PROJECT_MEMORY 14.3). sens6 uses captures/run_arm.sh (frozen).
set -u
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../../.." && pwd); B=${BUILD:-$R/cmake_targets/ran_build/build}
DRY=0; [ "${1:-}" = "--dry-run" ] && { DRY=1; shift; }; [ "${1:-}" = "--" ] && shift
. "$H/coremap_dgx.env"
off=0; [ "${INSTANCE:-A}" = "B" ] && off=10
RT=$((A_RT_CORE+off)); USS=$((A_USS_CORE+off)); ACT=$((A_ACTORS+off))
TP=$(echo "$A_TPOOL" | tr ',' '\n' | while read c; do echo $((c+off)); done | paste -sd,)
online=${ONLINE_CPUS_OVERRIDE:-$(cat /sys/devices/system/cpu/online)}
expand() { echo "$1" | tr ',' '\n' | while IFS=- read a b; do seq "$a" "${b:-$a}"; done; }
for c in $RT $USS $ACT $(echo "$TP" | tr ',' ' '); do
  expand "$online" | grep -qx "$c" || { echo "core $c is not online (online=$online)" >&2; exit 3; }
done
ENVS="ISAC_UE_RT_CORE=$RT ISAC_PDCCH_USS_CORE=$USS"
ARGS="--thread-pool $TP --sync-actor-core $ACT --dl-actor-core-start $ACT --ul-actor-core-start $ACT"
echo "COREMAP applied: instance=${INSTANCE:-A} $ENVS $ARGS"
[ "$DRY" = 1 ] && { echo "env $ENVS $B/nr-uesoftmodem $* $ARGS"; exit 0; }
exec env $ENVS "$B/nr-uesoftmodem" "$@" $ARGS
```
Note: `--dl-actor-core-start` pins actor i to core start+i (4 DL actors → cores 0..3 on instance A: all A725, intended). If the conf's `scan_thread` core is on an A725 core, print a warning (grep the `-O` file for `scan_thread`).

- [x] **Step 4: Run tests to verify they pass** → `python3 tests/passive_rx/dgx/test_run_rx_dgx.py -v` → OK (tests extended to 12: instance validation, scan_thread warning, "not online" refusal on this host, rfsim_arm COREMAP=1 refusal).

- [ ] **Step 5: Measured A/B in rfsim** (🔁 Haiku runs, Sonnet analyses) — 273 PRB 1 RX (`gnb.sa.rfsim.100mhz.conf` + a copy of `ue.passive.auto.100mhz.conf` saved as **new** file `tests/passive_rx/dgx/ue.passive.auto.100mhz.dgx.cfg` (extension `.cfg`, not `.conf`: the sens6-frozen gate pathspec `tests/passive_rx/*.conf` matches across `/`, so any new `.conf` under tests/passive_rx/ breaks it; libconfig ignores the extension; the original pdsch already ends `:3:20:2`, only scan_thread changes `1:8:5`->`1:8:6`) with `scan_thread "1:8:6"` and pdsch cores `...:3:20:2`), 3 runs unpinned vs 3 runs `COREMAP=1`, alternating, each via `campaign.py run` + `thrprof.sh` from the evidence tools. Record per arm: crc_pct, ttc_s, drop_full_pct, per-thread CPU, `pdschq_max_lag`.
  - DGX-only; not run in the cloud session 2026-10-01
Expected (pass): pinned is not worse on any metric by more than run-to-run spread; report the numbers either way.

- [x] **Step 6: Commit** (+ PROJECT_MEMORY §14.3 "measured" sub-table with the A/B numbers and `[SIM VERIFIED]`) -- cloud session: code + dry-run tests only (Steps 1-4, rfsim_arm COREMAP=1, dgx.cfg); the PROJECT_MEMORY "measured" sub-table awaits the DGX Step 5 run.

```bash
git add tests/passive_rx/dgx/coremap_dgx.env tests/passive_rx/dgx/run_rx_dgx.sh tests/passive_rx/dgx/test_run_rx_dgx.py \
  tests/passive_rx/dgx/rfsim_arm.sh tests/passive_rx/dgx/ue.passive.auto.100mhz.dgx.cfg PROJECT_MEMORY.md
git commit -m "feat(dgx): X925 core-map launcher + rfsim A/B measurement"
```

---

### Task A7 ★: Thread-safe blind-PDCCH scan → N consumers (Opus implements; Sonnet writes the rfsim A/B)

`passivePdcch0` is one of three serial hot threads (79 % at 273 PRB, K27). More consumers are blocked by unsynchronised state (research 2026-10-01): `g_energy_floor`/`g_energy_nseen` (rt.c:1406-1409), `ue->dci_thres` EMA (rt.c:5566), `g_recent[]`/`g_recent_head`/`g_recent_count` persistence table (rt.c:1715-1726), and plain counters (`g_occasions_run`, `g_candidates_run`, `g_accepts*`, `g_held_*`, `g_dec_*`, `g_acc_slot`, `g_occ_slot`, `g_btim_*`). Phase 2 of the occasion (accepts, EMA, persistence, CFR/PDSCH submission) is sequential by design (rt.c:~5320).

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.c` (drop the >1 warning once safe; keep `MAX_CONSUMERS 4`)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc` (add a concurrency test) + a TSAN build
- *(cloud 2026-10-01, implemented)* Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_phase2.{c,h}` (lean
  `nr_pdcch_blind_monitor` library): `g_phase2_mu`, the RNTI-persistence ring and the energy floor moved out of rt.c,
  because rt.c is compiled into PHY_NR_UE and is NOT in the gtest's link closure -- the `#ifdef NR_PDCCH_BLIND_TESTING`
  static-export hook of Step 1 cannot link. Also modified (races found by TSAN on a 2-consumer rfsim run, not in the
  research list): `dci_nr.c` (DM-RS probe accumulators / hot-CCE, mode flags, capture writer), `nr_passive_metrics.c`
  (emit serialised), `nr_pdcch_passive_queue.c` (max_lag CAS), `nr_pdcch_blind_rnti_bootstrap.c` (table had no lock),
  `nr_pdsch_passive_queue.c` (producer-side pending batch), `nr_pdcch_blind_monitor.c` (discovery flags atomic);
  `tests/passive_rx/dgx/thrprof.sh` (per-thread CPU for the A/B). Evidence: `tests/passive_rx/cloud_run_2026-10-01/a7_concurrency/`.

**Interfaces:**
- Consumes: A1 regression gate, A2 metrics (scanq fields).
- Produces: `pdcch_blind_monitor_scan_thread = "2:16:6"` works (2 consumers, cores 6,7) with identical decode results.

Design (decided here, implement exactly):
1. Counters → `_Atomic uint64_t` with `memory_order_relaxed` increments (and the A2 getter uses `atomic_load`).
2. Energy floor + `dci_thres` EMA + persistence table → one `static pthread_mutex_t g_phase2_mu`, taken for the **whole Phase 2 block** of one occasion (the part after candidate decode that the rt.c:~5320 comment calls sequential). Phase 1 (FEP, LLR, demap, candidate decode — the ~80 % of the cost) runs unlocked in parallel. This preserves the sequential semantics of Phase 2 exactly while parallelising the expensive part.
3. Ordering: Phase 2 of occasion k may now run before Phase 2 of occasion k-1 if consumers race. Persistence (`rnti_persistence_check`) needs ≥ 2 sightings within a staleness window measured in slots, so out-of-order by < depth slots is harmless; the energy-floor EMA is order-insensitive at the 1e-2 level. Document this in a comment at the mutex.

Rulings at implementation (cloud 2026-10-01, Opus): (a) the energy floor is fed per candidate in the PRE-PASS (Phase 1),
not in Phase 2, so it has its own leaf lock in `nr_pdcch_blind_phase2.c` instead of `g_phase2_mu` (same per-sample
sequence, no serialisation of Phase 1 behind another occasion's Phase 2). (b) "Phase 2" = from the decode join to the
END of the occasion (BTIM post, summary, ACQ update included). (c) An occasion of the AUTODISCOVER pass (root cfg,
`autodiscover=1`) holds `g_phase2_mu` for the whole occasion: its Phase 1 drives the discovery state machine in
`nr_pdcch_blind_monitor.c`, which rewrites the very cfg the occasion reads; bank / CORESET#0-USS / CSS0 passes carry
`autodiscover=0` and run Phase 1 unlocked. (d) `g_pdsch_configuration`/`g_pdsch_sweep_on` became `__thread`: they are set
per pass in Phase 1 and read in the same occasion's Phase 2, so a shared value let another consumer re-key the grants.
(e) The persistence ring stores one entry per SIGHTING by design, so "contains the RNTI exactly once" is tested as
"exactly one of 40 000 accepts is held (the first sighting) and all 64 ring entries are that RNTI".

- [x] **Step 1: Write the failing concurrency test** — in `nr_pdcch_blind_monitor_test.cc`, a test that calls the Phase-2 entry (the function containing `rnti_persistence_check`; extract it as `static` → `nr_pdcch_blind_phase2_for_test()` exported only under `#ifdef NR_PDCCH_BLIND_TESTING`, which the gtest target defines) from 4 threads × 10 000 synthetic accepts of the same RNTI and asserts: accept counter == 40 000, the persistence table contains the RNTI exactly once, no crash. Run it under TSAN:

```bash
cmake -B /tmp/tsan -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DCMAKE_C_FLAGS=-fsanitize=thread -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
ninja -C /tmp/tsan test_nr_pdcch_blind_monitor && /tmp/tsan/test_nr_pdcch_blind_monitor --gtest_filter='*Concurrent*'
```
Expected before the fix: TSAN reports data races on `g_recent*` / counters (FAIL).

- [x] **Step 2: Implement design points 1–3.**

- [x] **Step 3: Verify** — TSAN run clean; full `test_nr_pdcch_blind_monitor` + shuffle seeds 1/3/5 pass (195 + new, 2 skips).

- [x] **Step 4: rfsim A/B** (Sonnet): 273 PRB 1 RX, `scan_thread "1:8:6"` vs `"2:16:6"`, 3 runs each alternating, via A4 campaign runner. Pass: accepts per occasion and CRC % within run-to-run spread; `passivePdcch0+1` total CPU ≈ single consumer's; scanq `max_lag` not worse. Then 106-PRB regression gate.
  *(cloud 2026-10-01, orchestrator ruling: 4-core host, so 106 PRB and UNPINNED `"1:8:-1"` vs `"2:16:-1"`; run by Opus
  with `tests/passive_rx/dgx/thrprof.sh` for per-thread CPU. `max_lag` follows the queue depth (8 vs 16), not the consumer
  count -- controls `"2:8:-1"` -> 8 and `"1:16:-1"` -> 16. The 273-PRB `"1:8:6"`/`"2:16:6"` A/B stays a DGX step.)*

- [x] **Step 5: Commit** with the A/B table in the body; update PROJECT_MEMORY K27 (scan consumer no longer single-threaded) with `[SIM VERIFIED]`. *(cloud: the K27 text is handed to the orchestrator, which integrates PROJECT_MEMORY.md.)*

---

### Task A8: USS hash tracker on the GPU (multi-cell ready), CPU serial path kept as reference (Sonnet; DGX only — needs a GPU)

`pdcchUssHash` does ~88 M inner steps per job, independent per RNTI (loop `rnti` 1..65535 × 3 hash ids × ≤64 samples × 7 M), reduced to a top-4 (research 2026-10-01, `nr_pdcch_uss_tracker.c` `score_snapshot()` ~:120). It is pure integer/float work on stored samples (no IQ), the same shape as idsweep (11× on GB10, 2026-09-30). Decision (operator, 2026-10-01): go **directly to the GPU** and design the kernel for **batches of (cell, geometry, job)** so the multi-cell work (§17–§20) reuses it; the existing serial CPU `score_snapshot()` stays as the bit-exact reference and as the fallback when the GPU module is not built (`ENABLE_LDPC_CUDA=OFF`, default) or fails to load. No multi-threaded CPU variant.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_gpu.cu` (kernel + host wrapper)
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_gpu.h` (C ABI, dlopen'd like `libpdcch_gpu.so`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_tracker.c` (export the snapshot layout; try GPU, fall back to CPU)
- Modify: `openair1/PHY/CODING/CMakeLists.txt` (new `uss_gpu` MODULE target next to `pdcch_gpu`, same `LDPC_CUDA_ARCH` property, built only with `ENABLE_LDPC_CUDA`)
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_uss_gpu_test.cc` (+ CMake block inside the `ENABLE_LDPC_CUDA` section)

**Interfaces:**
- Produces (C ABI, `nr_pdcch_uss_gpu.h`):

```c
/* One scoring job = one (cell, CORESET geometry) snapshot. A batch scores many jobs in one launch. */
typedef struct {
  const void *snapshot;   /* the tracker's own snapshot struct (layout exported from nr_pdcch_uss_tracker.c) */
  uint32_t job_tag;       /* caller's id (cell_track_id << 16 | geometry index), echoed back */
} nr_uss_gpu_job_t;
typedef struct {
  uint32_t job_tag;
  uint16_t top_rnti[4];
  float top_score[4];
  uint8_t top_hash[4], top_m[4];
} nr_uss_gpu_result_t;
int nr_uss_gpu_init(void);                          /* 0 = ok; <0 = no device / load failure (caller falls back) */
int nr_uss_gpu_score(const nr_uss_gpu_job_t *jobs, int n_jobs, nr_uss_gpu_result_t *out); /* 0 = ok */
void nr_uss_gpu_shutdown(void);
```
- Produces (tracker side): `int nr_pdcch_uss_score_cpu_for_test(const void *snapshot, nr_uss_gpu_result_t *out);` under `NR_PDCCH_BLIND_TESTING`; env `ISAC_PDCCH_USS_GPU=0` forces the CPU path.
- Kernel layout: one thread per (job, RNTI); per-thread loop over the 3 hash ids × samples × 7 M exactly as the CPU code; block-level top-4 reduction, then a per-job merge kernel; **ties broken by lower RNTI** (the CPU `insert_top` order) so results are bit-identical.

- [ ] **Step 1: Failing equivalence test** — synthetic snapshots (fixed seeds): (a) one planted RNTI 0x4768 whose hashed candidates match the observed CCEs, (b) pure noise, (c) two planted RNTIs with equal scores (tie rule). Batch all 3 as one `nr_uss_gpu_score()` call with distinct `job_tag`s; assert each GPU result equals `nr_pdcch_uss_score_cpu_for_test()` field-for-field (scores compared exactly — use the same float accumulation order per thread as the CPU loop).
- [ ] **Step 2: Run → FAIL** (module missing). Build in the GPU build dir from Task A9 Step 1 (`-DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121`; run A9 Step 1 first if that dir does not exist).
- [ ] **Step 3: Implement** kernel + host wrapper (unified memory on GB10: `cudaMallocManaged` for the snapshot batch is acceptable; keep one persistent stream; no per-job allocations).
- [ ] **Step 4: Wire the tracker** — at tracker start call `nr_uss_gpu_init()` via dlopen (pattern: `nr_gpu_pdcch_fep_load()` in `nr_pdcch_gpu_fep.h:118`); on success score queued jobs in batches (drain up to 16 queued jobs per launch), on any GPU error log once and fall back to the CPU path permanently for the run. Log `USS_TRACK ... path=gpu|cpu score_us=`.
- [ ] **Step 5: Verify** — the test passes; on the 273-PRB rfsim arm with the GPU build: `USS_TRACK top=` lines identical to a CPU run on the same bed (same seeds/config), `pdcchUssHash` thread CPU (thrprof) drops below 10 %, regression gate PASS. Also run the CPU build (`ENABLE_LDPC_CUDA=OFF`) gate to prove the fallback.
- [ ] **Step 6: Commit**; PROJECT_MEMORY §14.3 item 3 and K27 updated with the measured GPU speed-up (`[SIM VERIFIED]`).

### Task A9: GPU build and re-measurement on GB10 (Sonnet)

K17 ("GPU LDPC 20× slower") was measured on a discrete RTX 4060 Ti over PCIe; the GB10 has unified memory. Re-measure before deciding any GPU work.

**Files:**
- Create: `tests/passive_rx/dgx/gpu_bench.sh`
- Modify: `PROJECT_MEMORY.md` (§4.2 build options, K17)

- [ ] **Step 1: Configure a separate GPU build dir** (never overwrite the CPU build):

```bash
cmake -S . -B cmake_targets/ran_build/build_gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON \
  -DOAI_USRP=ON -DOAI_SIMU=ON -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121 \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc -DUHD_LIBRARIES=/usr/local/lib/libuhd.so -DUHD_INCLUDE_DIRS=/usr/local/include
ninja -C cmake_targets/ran_build/build_gpu ldpc_cuda pdsch_gpu pdcch_gpu polar_gpu ldpctest nr_pdsch_gpu_fep_test nr_pdcch_gpu_fep_test nr_polar_sc_cuda_test
```
If `121` is rejected, try `121a`, then `120`; record which worked.

- [ ] **Step 2: Correctness** — run `nr_pdsch_gpu_fep_test`, `nr_pdcch_gpu_fep_test`, `nr_polar_sc_cuda_test`. Expected: pass. Any failure → superpowers:systematic-debugging, record as a new K-entry, stop GPU work on that module.

- [ ] **Step 3: `gpu_bench.sh`** — LDPC CPU vs CUDA: `LDPC_BENCH=1 ./ldpctest -l 8448 -r ... ` at BG1 Z=384 for 1, 8, 32, 106 segments (use the ldpctest options present in this tree: `./ldpctest -h`), both `--loader.ldpc.shlibversion ""` and `_cuda`; print wall µs/TB. PDSCH GPU FEP: rfsim 273-PRB arm with `NR_GPU_FEP=1 ISAC_GPU_SELFCHECK=1` vs CPU, compare `PDSCHQ max_lag`, per-thread CPU (thrprof), CRC.

- [ ] **Step 4: Decide per module** (write it in PROJECT_MEMORY K17 with numbers): enable by default on the DGX only if CRC is identical and latency or CPU is better by > 20 %.

- [ ] **Step 5: Commit** `gpu_bench.sh` + PROJECT_MEMORY update.

---

### Task A10 ★: `UEthread_0` split — measure, then design (Opus)

`UEthread_0` is 100 % at 273 PRB. Inline work per slot (research 2026-10-01): `readFrame()` (nr-ue.c:729), `UE_dl_preprocessing()` (:599 → PBCH tracking `pbch_processing()`, `pdcch_processing()`, blind-monitor RT tap incl. single-antenna `nr_slot_fep_ant()` rt.c:2823/:2854, replay, PUSCH monitor). DL actors do almost nothing in passive mode. In rfsim part of the 100 % may be socket I/O.

- [ ] **Step 1: Measure where the time goes** — 273-PRB rfsim arm with `ISAC_PDCCH_TIMING=1` (BTIM_RT, over_slot) and `perf record -g -t <UEthread tid> -- sleep 20` if `perf` is available without sudo (`perf_event_paranoid` ≤ 1); otherwise add a temporary `clock_gettime` breakdown (read/preprocess/pbch/pdcch-tap/other) printed every 2000 slots, **on a throwaway branch**. Output: a table of µs/slot per stage at 106 and 273 PRB, 1 RX and 4 RX (pin49r4 bed).

- [ ] **Step 2: Decision rule** — if RF read ≥ 30 % → split a thin reader thread (ring of slots) from processing; if PBCH tracking or the RT tap ≥ 30 % → move that stage to a DL actor with the slot's IQ pointer and an epoch tag; else → no split, document.

- [ ] **Step 3: Write a separate plan** `docs/superpowers/plans/<date>-uethread-split.md` with the chosen design (superpowers:writing-plans), reviewed by the operator before implementation. **This task ends at the reviewed plan.**

---

### Task A11: Faster initial sync at 273 PRB × 4 RX (Sonnet)

Initial sync took ~90 s on the rank-4 bed (§14.2). The scan batch is `min(512 MB / bytes_per_gscn, Tpool.len_thr)` (nr_initial_sync.c:32, :634-640); the DGX has 121 GiB.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` (env `ISAC_SCAN_SCRATCH_MB`, default 512 = today)
- Test: rfsim timing only (no unit test: the change is a budget constant) — plus an assertion test that the default path is unchanged: run `rfsim_regress.sh 1` with the env unset.

- [x] **Step 1:** Add `ISAC_SCAN_SCRATCH_MB` (clamped 64..16384) replacing the 512 MB constant; log the batch size chosen.
- [ ] **Step 2:** Measure `sync_s` on the pin49r4 rank-4 bed with `--ue-scan-carrier` (4 RX): default vs `ISAC_SCAN_SCRATCH_MB=8192` with `--thread-pool` of 8 X925/A725 cores; 2 runs each. Also pinned `--ssb` for reference.
  *Timing measurement DGX-only; not run in cloud session 2026-10-01.*
- [ ] **Step 3:** Pass: scan-mode `sync_s` improves ≥ 2× with no PCI/SSB-offset change; default unchanged. Commit + PROJECT_MEMORY §14.2.
  *Timing measurement DGX-only; not run in cloud session 2026-10-01.* Cloud: the pure `nr_initial_sync_scan_batch()` lives in `nr_initial_sync_budget.{c,h}` (gtest `test_nr_initial_sync_budget`); default-unchanged evidence in `tests/passive_rx/cloud_run_2026-10-01/a11_scan_scratch/`.

---

### Task A12: K22 — UL 64/256QAM bit errors on aarch64 (Sonnet, superpowers:systematic-debugging)

- [ ] **Step 1: Reproduce** — `./nr_pusch_ra0sim -n 10 -s 60 -S 60.1 -r 16 -w 46 -R 106 -m 25 -q 1 -v 1` (expect sign_errors > 0).
- [ ] **Step 2: Localize TX vs RX** — build the fixture twice with `-DSIMDE_NO_NATIVE` / scalar fallbacks for (a) the TX modulation path only (`openair1/PHY/MODULATION/nr_modulation.c`) and (b) the gNB-side RX path only (`nr_ulsch_demodulation.c`, `nr_ulsch_llr_computation.c`, `nr_phy_common.c` LLR functions); whichever makes sign_errors 0 holds the bug. Also run `test_nr_modulation` (known SIMD imag ±2 LSB on ARM).
- [ ] **Step 3: Minimal fix + unit test** that fails on aarch64 before the fix (compare SIMD vs scalar reference on random inputs, exact equality).
- [ ] **Step 4:** `test_nr_pusch_ra0_qam64/256` pass 20/20 consecutive runs; full ctest; rfsim gate. Commit; K22 → resolved.

---

### Task A13: Arch-aware offline sync contract script (K26) (🔁 Haiku)

- **Files:** Modify: `tests/passive_rx/offline_sync_contract/build_and_run.sh` (**not a sens6-frozen path** — check with the §4.0 git diff anyway).
- [x] **Step 1:** At the top: `ARCH=$(uname -m)`; if `aarch64`, apply exactly the transformations of `tests/passive_rx/dgx_host_snapshot_2026-09-30/tools/offline_sync_arm.sh` (strip `-DAVX2 -DGFNI -DSIMDE_X86_* -mno-avx512f -mgfni`, `-march=native`→`-mcpu=native`, gtest from `cmake_targets/ran_build/build/lib/libgtest.a` + CPM include dir found with `find ~/.cache/cpm -path '*googletest/include' | head -1`); x86 path byte-identical to today.
  - Evidence: `tests/passive_rx/cloud_run_2026-10-01/a13_sync_contract/README.txt` [OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 03fb79aae3]
  - X86 path: byte-identical cc commands confirmed via bash -x trace comparison
  - X86 fallback path: gtest integration tested, all OfflineSync.* 5/5 PASS
- [ ] **Step 2:** Run on the DGX → `OfflineSync.*` 5/5 PASS.
  - Note: aarch64 run pending on the DGX (cloud session 2026-10-01 verified the x86 path only)
- [ ] **Step 3:** Commit; K26 → resolved.

---

### Task A14: PROJECT_MEMORY integration and Track-A review (Opus orchestrator)

- [ ] **Step 1:** For every A-task: the §-section it touched carries date, commit, evidence path, label.
- [ ] **Step 2:** §21 gets a pointer to `nr_passive_obs.h` as the implemented schema v1 and a table "field → available / source".
- [ ] **Step 3:** Run `/code-review` (code-review plugin) on the Track-A diff range and superpowers:requesting-code-review with an Opus reviewer; fix findings.
- [ ] **Step 4:** Final gates: full ctest, shuffle seeds, `rfsim_regress.sh 3`, sens6 frozen diff empty. Push.

---

## TRACK B — NEEDS the X410 (DGX + X410 + antennas at DEIB). Human operator required for cabling, `sudo`, and X410 power.

Prerequisites: Track A tasks A1–A6 merged (campaign runner, metrics, observations, dashboard, launcher). Every B run is executed through `campaign.py run` so every campaign is fully logged.

### Task B1: X410 bring-up and characterization on the DGX (Sonnet guides; operator executes `sudo` steps)

**Files:**
- Create: `tests/passive_rx/dgx/host_network/x410-data.nmconnection.example` (NetworkManager profile template, no secrets)
- Create: `tests/passive_rx/dgx_host_snapshot_2026-09-30/x410_characterization_<date>.txt` → **new dated dir** `tests/passive_rx/dgx_x410_<date>/` (do not add files to the 2026-09-30 snapshot)
- Modify: `PROJECT_MEMORY.md` (§4.2 X410 row, §6.3 new channel map, §8 re-verify column)

- [ ] **Step 1:** Cable QSFP28 → a CX-7 port; `lspci | grep -i mellanox`, `ip -br link` — confirm the port stays up (K24). If the device vanishes again: `journalctl -k | grep mlx5` and ask the operator (do not touch firmware/power settings without approval).
- [ ] **Step 2:** Follow PROJECT_MEMORY §7 steps 2–7 exactly (mgmt address, `uhd_find_devices`, `uhd_usrp_probe` once, MTU 9000 + `ping -M do -s 8972`, rings, UHD 4.11 ↔ MPM compat; **ask before any image flash**).
- [ ] **Step 3:** `benchmark_rate` 300 s × 4 ch × 122.88 MS/s, NIC IRQs pinned per `coremap_dgx.env` guidance (A725 cores 3–4), NIC temperature logged every 10 s. Pass: 0 drops/overruns/seq errors, NIC missed delta 0.
- [ ] **Step 4:** Per-channel RMS on `RX1` vs `TERMINATION` (§7 step 7) at the Milan site; record imbalance.
- [ ] **Step 5:** Commit evidence + PROJECT_MEMORY updates (`[OTA VERIFIED <date>, <commit>]` for G0 benchmark only).

### Task B2: G0 receiver capture on the DGX (Sonnet; 🔁 Haiku repeats)

- [ ] **Step 1:** `C=$(campaign.py new --name g0_dgx --site "DEIB Polimi" --cell "unknown" --notes "600 s stream stability")`.
- [ ] **Step 2:** 3 × 600 s: `campaign.py run $C --arm g0_1rx --secs 600 --nic <cx7-port> -- sudo -E tests/passive_rx/dgx/run_rx_dgx.sh -- --passive-rx --usrp-args type=x4xx,addr=<data>,mgmt_addr=<mgmt> -r 106 --numerology 1 --band 78 -C <centre> --ue-scan-carrier --ue-rxgain <g> --ue-nb-ant-rx 1 --ue-nb-ant-tx 1 -O <agnostic conf copy for DGX>` (wait ≥ 60 s between runs and check `claimed: False`; never SIGKILL).
- [ ] **Step 3:** Pass (G0): no RFSTALL, NIC missed delta 0, `metrics_age` never > 45 s in the dashboard. Then 4 RX.
- [ ] **Step 4:** Commit campaign summary into `tests/passive_rx/dgx_x410_<date>/`; PROJECT_MEMORY §15 new sub-section "Milan campaign N".

### Task B3 ★: Milan cell survey G1–G5A + scan-confirm CFO fix validation (Opus orchestrates live decisions; Haiku repeats)

- [ ] **Step 1:** Wide scan: n78 windows in 100 MHz steps (`--ue-scan-carrier`, 273 PRB windows, 1 RX), one campaign run per window; collect `ISAC_ACQ_SSB {json}`, PCI, GSCN, SSB offset, CFO from logs/metrics.
- [ ] **Step 2:** For each PCI found: pinned `--ssb`, ≥ 5 runs; G3 `RFCENSUS pbch_ok=50 pbch_fail=0`; G4 MIB `k_SSB` (≥ 24 ⇒ NSA path, G5B — record, do not force).
- [ ] **Step 3:** **Scan-confirm validation** (ported fix `177d24513f`/`fbfb06ec49`): in scan mode the confirm pass must measure ≈ the scan-pass CFO (no `LOG_W` "confirm-pass offset < 0.1x"), and SIB1 must decode. Record both CFO values per run.
- [ ] **Step 4:** G5A: `rm -rf /tmp/passive_rx` (K11) before each acceptance run; SIB1 PLMN (MCC 222 expected in Italy — report the decoded value, map operator only via external registry), `ACQ carrier CONFIRMED` (or one ADAPT relaunch on `ISAC_ACQ_RETUNE`), TDD pattern.
- [ ] **Step 5:** PROJECT_MEMORY §15 Milan section with per-run table and gate verdicts; commit.

### Task B4: Dedicated path on the Milan cell (G6–G8) with observations (Sonnet; Opus reviews conclusions)

- [ ] **Step 1:** Only after B3 G5A PASS. 5 runs × 300 s at 1 RX, then 4 RX, via campaign runner with metrics + observations + dashboard.
- [ ] **Step 2:** Score on self-consistency only (no gNB truth, §15.0): bank add, `PDCCH_SCRAMBLING_ID CONFIRMED`, C-RNTI persistence, Technique D CONVERGED, TB CRC %, obs records vs `pdschq_decoded` (Review Focus 1 under real load), `obs_dropped == 0`.
- [ ] **Step 3:** PROJECT_MEMORY update; commit.

### Task B5: OTA core-map and GPU A/B (Sonnet; 🔁 Haiku repeats)

- [ ] **Step 1:** Alternating A/B/A/B, n ≥ 5 per arm: unpinned vs `run_rx_dgx.sh` map; if A9 enabled any GPU module, GPU vs CPU. Metrics: RFSTALL count, NIC missed, scanq drop_full, pdschq max_lag, CRC.
- [ ] **Step 2:** Adopt the winner as the DGX default in `coremap_dgx.env`; PROJECT_MEMORY §14.3 "OTA measured"; commit.

### Task B6: Track-B review and campaign archive (Opus)

- [ ] **Step 1:** `/code-review` on the Track-B diff; superpowers:finishing-a-development-branch.
- [ ] **Step 2:** Verify every campaign dir has `manifest.json`, `summary.md`, and per-run `verdict.json`; copy summaries (not raw IQ/logs > 10 MB) into `tests/passive_rx/dgx_x410_<date>/campaigns/`; push.

---

## Execution order and parallelism

- **Wave 1 (parallel, separate worktrees, no rfsim contention):** A0, A12 (starts with fixture only), A13.
- **Wave 2:** A1 (needs the host's rfsim slot).
- **Wave 3 (parallel code, serialized rfsim steps):** A2 → A3 (A3 after A2 because both touch CMake test blocks and the metrics getter) ‖ A4 ‖ A9 Steps 1–2.
- **Wave 4:** A5 (needs A2+A3+A4), A6, A8 (after A9 Step 1 — GPU build dir; DGX only, not in a GPU-less cloud session).
- **Wave 5 (critical, one at a time):** A7 ★, A10 ★ (measurement), A11.
- **Wave 6:** A14 review + push. Track B starts when the X410 is on site (B1 can start as soon as A1–A4 are merged; B3 needs A5–A6).

## Self-review record (2026-10-01)

- Spec coverage: dashboard → A5; sensing API → A3 (+§21 in A14); receiver performance metrics → A2 (+A1 scores, A4 verdicts); core allocation → A6, A7, A10, A11, B5; GPU → A8 (GPU-first USS tracker, multi-cell batch ready), A9, B5; detailed per-campaign logs → A4 (+B-track usage); X410 split → Track A / Track B; model tiers + plugins → policy table and per-task tags.
- Out of scope on purpose (separate plans later, per PROJECT_MEMORY §25): CellContext/multi-cell (G12–G14), NSA fixture (G5B), CSI-RS G4, state machine §19.
- Known judgement calls to confirm with the operator: A10 ends at a plan (no code); A8 is GPU-first (operator decision 2026-10-01); A5 keeps the existing single-page vanilla-JS style.
