# Phase 0: goal metrics, CPU/GPU inventory, GPU timeline, DL CRC root cause, target pipeline — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce the measuring stick for goals G1-G2 (one script, one scorecard), the complete map of what runs on CPU and GPU today, the root causes of the GPU stalls and of the low DL CRC, and the target GPU pipeline with a per-stage 500 us budget, so that the Phase 1 plan can be written from facts.

**Architecture:** Analysis-first. Task 1 is the only new code shipped to users (a log parser with unit tests). Tasks 2-5 are measurements and code reading that end in committed reports. Task 6 turns them into the target design. OTA and on-air traces are run by the orchestrator; agents prepare scripts and analyse.

**Tech Stack:** Python 3 (stdlib only, `unittest`), C/CUDA code reading, nsys (`/usr/local/cuda/bin/nsys`) + existing `tests/passive_rx/sysan_nsys.py`, existing OTA runner `ota_window_gen.sh` under `heavy.sh`.

**Spec:** `docs/superpowers/plans/2026-10-16-rx-goals-roadmap.md` (goals, pass criteria, rules). Evidence so far: `docs/superpowers/reports/csd/ROOTCAUSE-20261016.md`, `ALLON-OTA-20261016.md`, `PDCCH-PARBANKS-OTA-20261016.md`, `ULCAUSE-20261016.md`, `MPCI-OTA-20261016.md`, `LDPC-PERCB-20261016.md`.

## Global Constraints
- Worktree `/home/nicola/NICOLA/wt/rr-csd-int16`, branch `csd/int16`. Builds: `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j12 <targets>` in `cmake_targets/ran_build/build_t26`; agents cannot run cmake.
- Never build while one of OUR receivers runs: `for p in $(pgrep -x nr-uesoftmodem); do sudo -n readlink /proc/$p/exe; done | grep -q ^/home/nicola/` must be false. Never `pgrep -af | grep` in a wait loop. Never touch other users' processes. Bounded waits only.
- Agents never run OTA, uhd_*, X410, DPDK or firmware tools. While an OTA runs, python analysis runs as `nice -n 19 taskset -c 17 python3 ...`, one process at a time, with `timeout`.
- Private MPS dir for every CUDA program: `mkdir -p /tmp/nicola_nomps; export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps`.
- RAM never below 16 GB free. ISAC_RX_BRANCH_FO stays unset. sens6-frozen paths never edited (CLAUDE.md). New configs under tests/passive_rx/ use `.cfg`.
- `git add <explicit paths>`, never -A, never stash. Commit trailer for Sonnet agents: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` + `Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y`.
- Evidence label on every number: `[MEASURED|INFERRED|CODE-READ, DGX OTA <date>, <cell>, <arm>, n=<runs>]`. Never pool cells (V = Vodafone 3680.01 MHz, T = TIM 3759.99 MHz), hosts or bandwidths.
- The Write tool may refuse report files for subagents: return report text; the orchestrator saves and commits it.
- Runs live in `/home/nicola/NICOLA/ota_runs/<run>/run.log` (ANSI colour codes: strip with `re.sub(r'\x1b\[[0-9;]*m', '', line)`).

## Review Focus
1. Lines cut mid-way (a run stopped by SIGINT) or missing fields (`snr_l` absent, `e2e` absent) -> the parser skips the line and counts it in `skipped`, never crashes. Pinned by `test_truncated_and_missing_fields` (Task 1).
2. Retransmissions (rv > 0) and reserved MCS (table 0/2: mcs >= 29; table 1: mcs >= 28) have no defined code rate -> never SNR-qualified, counted as `unqualified_retx`. Pinned by `test_retx_never_qualified` (Task 1).
3. Multi-layer grants: the qualifying SNR is the minimum over the first `Nl` entries of `snr_l` (a `-99.0` slot beyond Nl is ignored). Pinned by `test_multilayer_uses_min_of_used_layers` (Task 1).
4. Several PDSCHQ / PUSCHQ2 counter lines per run (cumulative): the LAST one is the run total. Pinned by `test_drop_counters_use_last_line` (Task 1).
5. A run with zero qualified grants must report `crc_q = None`, not 0 % or a division error. Pinned by `test_no_qualified_grants` (Task 1).

---

### Task 1: `goal_metrics.py` — the G1/G2 scorecard per run

**Files:**
- Create: `tests/passive_rx/goal_metrics.py`
- Test: `tests/passive_rx/test_goal_metrics.py`

**Interfaces:**
- Produces: `required_snr_db(mcs:int, tbl:int) -> float|None`; `parse_run(path:str) -> dict` with keys `dl` and `ul`, each `{n_real, tot_p50, tot_p99, tot_p999, crc_all, n_q, crc_q, unqualified_low_snr, unqualified_retx, drops_pct, skipped}`; CLI `goal_metrics.py [--margin 2.0] [--json] run.log...` printing one line per run per direction plus PASS/FAIL against G1/G2 (tot_p99 <= 500, crc_q >= 80, drops_pct < 1).
- Log formats it reads (from the receiver, unchanged):
  - DL: `SENSING: EVMGRANT ... crc=<0|1> mcs=<m> tbl=<t> Qm=<q> Nl=<n> ... snr=<dB> ... probe=<0|1> ... rv=<r> ... snr_l=<a,b,c,d> ... e2e=<a2e,qw,pre,dec,first,bodies,tot,lag> ...`
  - DL drops: `SENSING: PDSCHQ queued=<Q> decoded=<D> crc_ok=<C> (<p>%) dropped[full=<F> stale=<S>] ... dropped_narrow=<N>`
  - UL decode: `SENSING: UL_PUSCH_CRC rnti=<r> ok=<0|1> mcs=<m> ... status=<s> ... snr_db=<dB> ...` (optional `probe=<0|1>` and `tbl=<t>` once the UL track adds them; default probe=0, tbl=0)
  - UL latency: `SENSING: UL_E2E rnti=<r> st=<s> prb=<p> mcs=<m> ... tot=<us> ...`
  - UL drops: `SENSING: PUSCHQ2 consumers=<c> real[queued=<Q> full=<F>] ...`

- [ ] **Step 1: Write the failing tests**

```python
# tests/passive_rx/test_goal_metrics.py
import os, tempfile, unittest
import goal_metrics as gm

def ev(crc=1, mcs=10, tbl=0, nl=1, snr_l="20.0,-99.0,-99.0,-99.0", probe=0, rv=0, tot=400):
    return (f"[PHY]    SENSING: EVMGRANT rnti=0x1 1.2 crc={crc} mcs={mcs} tbl={tbl} Qm=2 Nl={nl} rb=0+5 nsym=12 "
            f"evm=1 snr=20.0 probe={probe} rv={rv} ndi=0 pid=0 tbs=96 snr_l={snr_l} l2=1 zabs=0 "
            f"e2e=100,1,1,1,1,1,{tot},1 stg=fep:1/1/1 cb=1\n")

def write(lines):
    f = tempfile.NamedTemporaryFile('w', suffix='.log', delete=False)
    f.writelines(lines); f.close(); return f.name

class GoalMetricsTest(unittest.TestCase):
    def test_required_snr_monotonic_and_reserved(self):
        self.assertLess(gm.required_snr_db(0, 0), gm.required_snr_db(27, 0))
        self.assertIsNone(gm.required_snr_db(29, 0))
        self.assertIsNone(gm.required_snr_db(28, 1))
        self.assertAlmostEqual(gm.required_snr_db(8, 0), gm.required_snr_db(14, 2), places=6)  # same Qm/R

    def test_qualified_crc_and_latency(self):
        p = write([ev(crc=1, tot=300), ev(crc=0, tot=900), ev(crc=1, snr_l="-10.0,-99,-99,-99", tot=200),
                   "[PHY]    SENSING: PDSCHQ queued=1000 decoded=990 crc_ok=2 (0.2%) dropped[full=3 stale=4] max_lag_slots=1/2 slot_groups=0/0 multi-grant slots dropped_narrow=3\n"])
        d = gm.parse_run(p)['dl']
        self.assertEqual(d['n_real'], 3)
        self.assertEqual(d['n_q'], 2)
        self.assertAlmostEqual(d['crc_q'], 50.0)
        self.assertEqual(d['unqualified_low_snr'], 1)
        self.assertAlmostEqual(d['drops_pct'], 1.0)
        self.assertEqual(d['tot_p99'], 900)

    def test_probes_excluded(self):
        d = gm.parse_run(write([ev(probe=1), ev()]))['dl']
        self.assertEqual(d['n_real'], 1)

    def test_retx_never_qualified(self):
        d = gm.parse_run(write([ev(rv=2), ev(mcs=30, tbl=0), ev(mcs=28, tbl=1)]))['dl']
        self.assertEqual(d['n_q'], 0)
        self.assertEqual(d['unqualified_retx'], 3)

    def test_multilayer_uses_min_of_used_layers(self):
        # layer 2 at 0 dB disqualifies an MCS-27 grant; the -99 slots beyond Nl=2 are ignored
        d = gm.parse_run(write([ev(mcs=27, nl=2, snr_l="30.0,0.0,-99.0,-99.0")]))['dl']
        self.assertEqual(d['n_q'], 0)
        d = gm.parse_run(write([ev(mcs=5, nl=2, snr_l="30.0,25.0,-99.0,-99.0")]))['dl']
        self.assertEqual(d['n_q'], 1)

    def test_truncated_and_missing_fields(self):
        d = gm.parse_run(write([ev()[:60] + "\n", ev().replace("e2e=", "x2x="), ev()]))['dl']
        self.assertEqual(d['n_real'], 1)
        self.assertEqual(d['skipped'], 2)

    def test_drop_counters_use_last_line(self):
        q = "[PHY]    SENSING: PDSCHQ queued={} decoded=1 crc_ok=1 (1%) dropped[full={} stale=0] max_lag_slots=1/2 dropped_narrow=0\n"
        d = gm.parse_run(write([q.format(100, 50), q.format(1000, 5)]))['dl']
        self.assertAlmostEqual(d['drops_pct'], 0.5)

    def test_no_qualified_grants(self):
        d = gm.parse_run(write([ev(snr_l="-20.0,-99,-99,-99")]))['dl']
        self.assertIsNone(d['crc_q'])

    def test_ul_lines(self):
        p = write(["[PHY]    SENSING: UL_PUSCH_CRC rnti=0x1 ok=1 mcs=4 prb=1+3 ta=1 t=1 cpu_us=1 status=0 reason=ok snr_db=15.0 delay=0\n",
                   "[PHY]    SENSING: UL_PUSCH_CRC rnti=0x1 ok=0 mcs=4 prb=1+3 ta=1 t=1 cpu_us=1 status=1 reason=x snr_db=-12.0 delay=0\n",
                   "[PHY]    SENSING: UL_E2E rnti=0x1 st=1 prb=3 mcs=4 pick=1 arr2deq=1 wall=1 tot=700 cpu_us=1\n",
                   "[PHY]    SENSING: PUSCHQ2 consumers=2 real[queued=200 full=4] probe[queued=1 decoded=1 full=0]\n"])
        u = gm.parse_run(p)['ul']
        self.assertEqual(u['n_q'], 1)
        self.assertAlmostEqual(u['crc_q'], 100.0)
        self.assertEqual(u['tot_p99'], 700)
        self.assertAlmostEqual(u['drops_pct'], 2.0)

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd tests/passive_rx && python3 -m unittest test_goal_metrics -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'goal_metrics'`

- [ ] **Step 3: Write the implementation**

```python
#!/usr/bin/env python3
"""G1/G2 scorecard per run (roadmap 2026-10-16): DL/UL latency over all real grants, SNR-qualified CRC, queue drops.
A grant is SNR-qualified when min per-layer SNR >= required_snr_db(mcs, tbl) + margin (default 2 dB).
required_snr_db = 10*log10(2**SE - 1) + IMPL_LOSS_DB, SE = Qm*R/1024 (TS 38.214 tables 5.1.3.1-1/2/3).
shortcut: Shannon gap + fixed 1.5 dB loss, not a calibrated BLER curve; recalibrate from Task 2's CRC-vs-margin curve.
usage: goal_metrics.py [--margin 2.0] [--json] run.log..."""
import json, math, re, sys

IMPL_LOSS_DB = 1.5
# (Qm, R*1024) per MCS index; None = reserved (retransmission only)
T64 = [(2,120),(2,157),(2,193),(2,251),(2,308),(2,379),(2,449),(2,526),(2,602),(2,679),(4,340),(4,378),(4,434),(4,490),
       (4,553),(4,616),(4,658),(6,438),(6,466),(6,517),(6,567),(6,616),(6,666),(6,719),(6,772),(6,822),(6,873),(6,910),(6,948)]
T256 = [(2,120),(2,193),(2,308),(2,449),(2,602),(4,378),(4,434),(4,490),(4,553),(4,616),(4,658),(6,466),(6,517),(6,567),
        (6,616),(6,666),(6,719),(6,772),(6,822),(6,873),(8,682.5),(8,711),(8,754),(8,797),(8,841),(8,885),(8,916.5),(8,948)]
TLOW = [(2,30),(2,40),(2,50),(2,64),(2,78),(2,99),(2,120),(2,157),(2,193),(2,251),(2,308),(2,379),(2,449),(2,526),(2,602),
        (4,340),(4,378),(4,434),(4,490),(4,553),(4,616),(6,438),(6,466),(6,517),(6,567),(6,616),(6,666),(6,719),(6,772)]
TABLES = {0: T64, 1: T256, 2: TLOW}
ANSI = re.compile(r'\x1b\[[0-9;]*m')
KV = re.compile(r'(\w+)=(\S+)')

def required_snr_db(mcs, tbl):
    t = TABLES.get(tbl, T64)
    if mcs < 0 or mcs >= len(t):
        return None
    qm, r = t[mcs]
    return 10 * math.log10(2 ** (qm * r / 1024) - 1) + IMPL_LOSS_DB

def pct(v, p):
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(p * len(s)))]

def empty():
    return {'lat': [], 'crc_all': [0, 0], 'n_q': 0, 'ok_q': 0, 'low': 0, 'retx': 0, 'drops': None, 'skipped': 0}

def classify(acc, crc, mcs, tbl, rv, snr, margin):
    req = required_snr_db(mcs, tbl) if rv == 0 else None
    if req is None:
        acc['retx'] += 1
    elif snr >= req + margin:
        acc['n_q'] += 1; acc['ok_q'] += crc
    else:
        acc['low'] += 1

def summary(acc):
    lat = acc['lat']; n = acc['crc_all'][1]
    return {'n_real': n, 'tot_p50': pct(lat, .5), 'tot_p99': pct(lat, .99), 'tot_p999': pct(lat, .999),
            'crc_all': 100.0 * acc['crc_all'][0] / n if n else None, 'n_q': acc['n_q'],
            'crc_q': 100.0 * acc['ok_q'] / acc['n_q'] if acc['n_q'] else None,
            'unqualified_low_snr': acc['low'], 'unqualified_retx': acc['retx'],
            'drops_pct': acc['drops'], 'skipped': acc['skipped']}

def parse_run(path, margin=2.0):
    dl, ul = empty(), empty()
    with open(path, errors='replace') as fh:
        lines = fh.readlines()
    for raw in lines:
        if 'SENSING:' not in raw:
            continue
        line = ANSI.sub('', raw)
        kv = dict(KV.findall(line))
        try:
            if 'EVMGRANT' in line:
                if kv.get('probe', '0') != '0':
                    continue
                crc, mcs, tbl, nl, rv = (int(kv[k]) for k in ('crc', 'mcs', 'tbl', 'Nl', 'rv'))
                tot = int(kv['e2e'].split(',')[6])
                snr = min(float(x) for x in kv['snr_l'].split(',')[:max(1, nl)])
                dl['lat'].append(tot); dl['crc_all'][0] += crc; dl['crc_all'][1] += 1
                classify(dl, crc, mcs, tbl, rv, snr, margin)
            elif 'UL_PUSCH_CRC' in line:
                if kv.get('probe', '0') != '0':
                    continue
                ok, mcs = int(kv['ok']), int(kv['mcs'])
                ul['crc_all'][0] += ok; ul['crc_all'][1] += 1
                classify(ul, ok, mcs, int(kv.get('tbl', 0)), 0, float(kv['snr_db']), margin)
            elif 'UL_E2E' in line:
                ul['lat'].append(int(kv['tot']))
            elif 'PDSCHQ queued=' in line:
                m = re.search(r'queued=(\d+).*full=(\d+) stale=(\d+).*dropped_narrow=(\d+)', line)
                q, f, s, n = map(int, m.groups())
                dl['drops'] = 100.0 * (f + s + n) / q if q else None
            elif 'PUSCHQ2' in line:
                m = re.search(r'real\[queued=(\d+) full=(\d+)\]', line)
                q, f = map(int, m.groups())
                ul['drops'] = 100.0 * f / q if q else None
        except (KeyError, ValueError, IndexError, AttributeError):
            (dl if 'EVMGRANT' in line or 'PDSCHQ' in line else ul)['skipped'] += 1
    return {'dl': summary(dl), 'ul': summary(ul)}

def verdict(s):
    ok = (s['tot_p99'] is not None and s['tot_p99'] <= 500, s['crc_q'] is not None and s['crc_q'] >= 80,
          s['drops_pct'] is not None and s['drops_pct'] < 1)
    return 'PASS' if all(ok) else 'FAIL(' + ','.join(n for n, g in zip(('lat', 'crc', 'drops'), ok) if not g) + ')'

if __name__ == '__main__':
    args = sys.argv[1:]; margin = 2.0; as_json = False
    if '--margin' in args:
        i = args.index('--margin'); margin = float(args[i + 1]); del args[i:i + 2]
    if '--json' in args:
        args.remove('--json'); as_json = True
    for p in args:
        r = parse_run(p, margin)
        if as_json:
            print(json.dumps({'run': p, **r}))
            continue
        for d in ('dl', 'ul'):
            s = r[d]
            print(f"{p} {d.upper()} {verdict(s)} " + ' '.join(f"{k}={v:.1f}" if isinstance(v, float) else f"{k}={v}" for k, v in s.items()))
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd tests/passive_rx && python3 -m unittest test_goal_metrics -v`
Expected: 9 tests, OK

- [ ] **Step 5: Smoke-run on a real log**

Run: `nice -n 19 taskset -c 17 timeout 600 python3 tests/passive_rx/goal_metrics.py /home/nicola/NICOLA/ota_runs/ab_xTaXE1_1_142426/run.log`
Expected: two lines (DL, UL), `skipped` < 0.1 % of `n_real`, DL `tot_p99` ~7284 (matches `ALLON-OTA-20261016.md`).

- [ ] **Step 6: Commit**

```bash
git add tests/passive_rx/goal_metrics.py tests/passive_rx/test_goal_metrics.py
git commit -m "test(goals): goal_metrics.py, G1/G2 scorecard (latency over all real grants, SNR-qualified CRC, queue drops)"
```

---

### Task 2: Baseline scorecard + CRC-vs-SNR-margin calibration

**Files:**
- Create: `tests/passive_rx/crc_margin_curve.py`
- Report (returned as text, orchestrator commits): `docs/superpowers/reports/csd/SCORECARD-BASELINE-20261016.md`

**Interfaces:**
- Consumes: `goal_metrics.required_snr_db`, `goal_metrics.parse_run`, `goal_metrics.ANSI`, `goal_metrics.KV` (Task 1).
- Produces: `crc_margin_curve.py run.log...` printing, per 1-dB bin of (min-layer SNR - required SNR), the grant count and CRC-OK %, DL and UL separately; the margin at which CRC-OK crosses 80 % (`cross80_db`).

- [ ] **Step 1: Write the failing test** (append to `tests/passive_rx/test_goal_metrics.py`)

```python
import crc_margin_curve as cmc

class MarginCurveTest(unittest.TestCase):
    def test_cross80(self):
        # bins: margin -1 dB 0 % ok, +1 dB 50 %, +3 dB 90 % -> first bin >= 80 % is +3
        rows = {-1: [10, 0], 1: [10, 5], 3: [10, 9]}
        self.assertEqual(cmc.cross80(rows), 3)
        self.assertIsNone(cmc.cross80({0: [10, 1]}))
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd tests/passive_rx && python3 -m unittest test_goal_metrics.MarginCurveTest -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'crc_margin_curve'`

- [ ] **Step 3: Implement**

```python
#!/usr/bin/env python3
"""CRC-OK % per 1-dB bin of (min used-layer SNR - required SNR of the MCS), DL (EVMGRANT probe=0, rv=0) and UL (UL_PUSCH_CRC).
Calibrates goal_metrics.IMPL_LOSS_DB: on a correct receiver CRC-OK should reach 80 % near margin 0..+2 dB.
usage: crc_margin_curve.py run.log..."""
import math, sys
from collections import defaultdict
import goal_metrics as gm

def cross80(rows):
    for b in sorted(rows):
        n, ok = rows[b]
        if n >= 10 and 100.0 * ok / n >= 80:
            return b
    return None

def curve(paths):
    dl, ul = defaultdict(lambda: [0, 0]), defaultdict(lambda: [0, 0])
    for p in paths:
        with open(p, errors='replace') as fh:
            lines = fh.readlines()
        for raw in lines:
            if 'EVMGRANT' not in raw and 'UL_PUSCH_CRC' not in raw:
                continue
            kv = dict(gm.KV.findall(gm.ANSI.sub('', raw)))
            try:
                if 'EVMGRANT' in raw:
                    if kv.get('probe', '0') != '0' or kv['rv'] != '0':
                        continue
                    nl = int(kv['Nl']); snr = min(float(x) for x in kv['snr_l'].split(',')[:max(1, nl)])
                    req = gm.required_snr_db(int(kv['mcs']), int(kv['tbl'])); ok = int(kv['crc']); acc = dl
                else:
                    if kv.get('probe', '0') != '0':
                        continue
                    snr = float(kv['snr_db']); req = gm.required_snr_db(int(kv['mcs']), int(kv.get('tbl', 0))); ok = int(kv['ok']); acc = ul
            except (KeyError, ValueError, IndexError):
                continue
            if req is None:
                continue
            b = math.floor(snr - req); acc[max(-20, min(20, b))][0] += 1; acc[max(-20, min(20, b))][1] += ok
    return dl, ul

if __name__ == '__main__':
    for name, rows in zip(('DL', 'UL'), curve(sys.argv[1:])):
        print(name, 'cross80_db=%s' % cross80(rows))
        for b in sorted(rows):
            n, ok = rows[b]
            print(f"  margin {b:+d} dB: n={n} crc_ok={100.0 * ok / n:.1f}%")
```

- [ ] **Step 4: Run tests**

Run: `cd tests/passive_rx && python3 -m unittest test_goal_metrics -v`
Expected: 10 tests, OK

- [ ] **Step 5: Produce the baseline scorecard**

Run (one python at a time, nice'd, core 17):
```bash
cd /home/nicola/NICOLA/wt/rr-csd-int16
for r in /home/nicola/NICOLA/ota_runs/ab_a{V,T}{X,S,B}?_1_* /home/nicola/NICOLA/ota_runs/ab_x{V,T}aXE?_1_*; do
  nice -n 19 taskset -c 17 timeout 600 python3 tests/passive_rx/goal_metrics.py $r/run.log; done
for c in V T; do nice -n 19 taskset -c 17 timeout 900 python3 tests/passive_rx/crc_margin_curve.py /home/nicola/NICOLA/ota_runs/ab_a${c}X?_1_*/run.log; done
```
Write the report: per cell and arm, the G1/G2 table (tot p50/p99/p99.9, crc_all, n_q, crc_q, unqualified counts, drops) with PASS/FAIL, the two margin curves and `cross80_db`. If DL `cross80_db` is > +4 dB or None, state that the DL CRC loss is NOT explained by SNR (input for Task 5). If the 2026-10-16 ab_a* runs are missing, use ab_x* and say so.

- [ ] **Step 6: Commit**

```bash
git add tests/passive_rx/crc_margin_curve.py tests/passive_rx/test_goal_metrics.py
git commit -m "test(goals): CRC-OK vs SNR margin curve for calibrating the SNR-qualified CRC"
```

---

### Task 3: CPU/GPU inventory of the whole receiver (code reading + existing logs)

**Files:**
- Report (returned as text): `docs/superpowers/reports/csd/GPU-INVENTORY-20261016.md`

**Interfaces:**
- Consumes: source tree under `openair1/PHY/NR_UE_TRANSPORT/`, `openair1/PHY/CODING/`, `executables/nr-ue.c`, `common/utils/nr_core_plan.c`; logs of run `/home/nicola/NICOLA/ota_runs/ab_aVX1_1_*` and `ab_aTX1_1_*` (BTIM, PDTIM, SCANPASS, SCAN_E2E, GPU_* lines).
- Produces: one table per pipeline (DL PDCCH, DL PDSCH, UL PUSCH, discovery/background: NID sweep, joint-anon, DM-RS scan, CSI-RS discovery, USS tracker, radio/ring) with the columns below; Task 6 reads it.

- [ ] **Step 1: List every thread and its work.** Start from the live thread list in this session (`passivePdcch*`, `passivePdsch*`, `passivePusch*`, `pdcchJanon`, `pdcchUssHash`, `dmrsScan`, `dmrsIdBg`, `csirsDisc`, NID worker, `UEthread_0`, `usrpRing`, `uhd_ctrl*`, `Tpool*`, `pdschRecov`, `scan2Slow`, `gpuCheckBg`) and find each thread's creation site (`grep -rn "pthread_setname_np\|threadCreate\|nr_core_plan_create" openair1 executables common`).
- [ ] **Step 2: For each stage, fill the row:**

| stage | thread/core (X925 or A725) | file:function | CPU or GPU today (env switch) | cost per slot/grant [MEASURED from BTIM/PDTIM/stg lines, cell, run] | parallel-compute or control-logic | existing CUDA module to reuse | data in/out (sizes) and where it lives (host/pinned/managed/device) | syncs and CPU<->GPU round trips per slot | stream / priority / green-context partition used |
|---|---|---|---|---|---|---|---|---|---|

- [ ] **Step 3: List every CUDA module** (`find openair1 common -name '*.cu'`): kernels, streams, allocation type (`nr_gpu_allocate` kinds: DEVICE / STAGING / managed), which use the RT green-context partition (`ISAC_GPU_RT_ISOLATE`, `nr_gpu_green_stream`), which assume unified memory, which have a discrete path (`nr_gpu_model()`).
- [ ] **Step 4: Classify each CPU stage** as `PORT` (data-parallel, worth it), `PORT-PARTIAL` (compute part only, decisions stay on CPU), or `KEEP-CPU` (real-time I/O or control), with one line of reason each.
- [ ] **Step 5: Return the report text** with the tables, the module list, the classification, and the top 10 CPU consumers per cell ranked by CPU-core share. Every number with its evidence label.

---

### Task 4: On-air GPU timeline and the GPU stall root cause

**Files:**
- Create: `/home/nicola/NICOLA/wt/rr-orchestration/ota/run_nsys_20261017.sh` (orchestrator-owned; the agent drafts it as text in the report, the orchestrator saves and runs it)
- Modify: `tests/passive_rx/sysan_nsys.py` (add a stall finder)
- Test: `tests/passive_rx/test_sysan_nsys.py`
- Report (returned as text): `docs/superpowers/reports/csd/GPU-TIMELINE-<date>.md`

**Interfaces:**
- Consumes: nsys sqlite export (`nsys export -t sqlite`), tables `CUPTI_ACTIVITY_KIND_KERNEL`, `CUPTI_ACTIVITY_KIND_MEMCPY`, `CUPTI_ACTIVITY_KIND_RUNTIME`, `StringIds`, `ThreadNames`.
- Produces: `sysan_nsys.stalls(db_path:str, gap_ms:float=50.0) -> list[dict]` — every interval >= gap_ms during which no kernel of the PDSCH/PDCCH consumer threads started while such kernels were queued (launch API returned but kernel start pending), each with `{start_ns, dur_ms, running_kernels:[name...], blocking_api:[(thread, api, dur_ms)]}`; CLI flag `--stalls`.

- [ ] **Step 1: Write the failing test** with a tiny synthetic sqlite DB:

```python
# tests/passive_rx/test_sysan_nsys.py
import sqlite3, tempfile, unittest
import sysan_nsys as sn

def db():
    p = tempfile.NamedTemporaryFile(suffix='.sqlite', delete=False).name
    c = sqlite3.connect(p)
    c.executescript("""
    CREATE TABLE StringIds(id INTEGER, value TEXT);
    CREATE TABLE ThreadNames(globalTid INTEGER, nameId INTEGER, priority INTEGER);
    CREATE TABLE CUPTI_ACTIVITY_KIND_RUNTIME(start INTEGER, end INTEGER, globalTid INTEGER, correlationId INTEGER, nameId INTEGER);
    CREATE TABLE CUPTI_ACTIVITY_KIND_KERNEL(start INTEGER, end INTEGER, correlationId INTEGER, shortName INTEGER, streamId INTEGER, globalPid INTEGER);
    CREATE TABLE CUPTI_ACTIVITY_KIND_MEMCPY(start INTEGER, end INTEGER, correlationId INTEGER, bytes INTEGER);
    INSERT INTO StringIds VALUES (1,'passivePdsch0'),(2,'k_ldpc'),(3,'cudaLaunchKernel'),(4,'k_big'),(5,'nidWorker');
    INSERT INTO ThreadNames VALUES (10,1,0),(11,5,0);
    -- big kernel from another thread runs 0..200 ms; the consumer launches at 10 ms, its kernel starts at 200 ms
    INSERT INTO CUPTI_ACTIVITY_KIND_RUNTIME VALUES (0,1000,11,1,3),(10000000,10010000,10,2,3);
    INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES (0,200000000,1,4,7,1),(200000000,200100000,2,2,8,1);
    """); c.commit(); c.close(); return p

class StallTest(unittest.TestCase):
    def test_finds_stall_and_blocker(self):
        s = sn.stalls(db(), gap_ms=50.0)
        self.assertEqual(len(s), 1)
        self.assertGreater(s[0]['dur_ms'], 150)
        self.assertIn('k_big', s[0]['running_kernels'])

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd tests/passive_rx && python3 -m unittest test_sysan_nsys -v`
Expected: FAIL with `AttributeError: module 'sysan_nsys' has no attribute 'stalls'`

- [ ] **Step 3: Implement `stalls()` in `sysan_nsys.py`**

```python
def stalls(db_path, gap_ms=50.0, consumer_prefixes=('passivePdsch', 'passivePdcch', 'passivePusch')):
    """Consumer kernels whose launch->start delay is >= gap_ms: the GPU was busy elsewhere. Returns one dict per stall
    with the kernels running at the moment the consumer launched and the long CUDA API calls overlapping it."""
    c = sqlite3.connect(db_path)
    s = dict(c.execute('SELECT id, value FROM StringIds'))
    tname = {t: s.get(n, '') for t, n in c.execute('SELECT globalTid, nameId FROM ThreadNames')}
    launches = c.execute('SELECT r.start, r.globalTid, k.start, k.end, k.shortName FROM CUPTI_ACTIVITY_KIND_RUNTIME r '
                         'JOIN CUPTI_ACTIVITY_KIND_KERNEL k ON k.correlationId = r.correlationId').fetchall()
    kernels = c.execute('SELECT start, end, shortName FROM CUPTI_ACTIVITY_KIND_KERNEL').fetchall()
    api = c.execute('SELECT start, end, globalTid, nameId FROM CUPTI_ACTIVITY_KIND_RUNTIME').fetchall()
    out = []
    for r0, tid, k0, k1, kn in launches:
        if not tname.get(tid, '').startswith(consumer_prefixes) or (k0 - r0) < gap_ms * 1e6:
            continue
        running = sorted({s.get(n, str(n)) for a, b, n in kernels if a <= r0 < b})
        blocking = [(tname.get(t, str(t)), s.get(n, str(n)), (b - a) / 1e6) for a, b, t, n in api
                    if a < k0 and b > r0 and (b - a) >= gap_ms * 1e6 / 10]
        out.append({'start_ns': r0, 'dur_ms': (k0 - r0) / 1e6, 'running_kernels': running, 'blocking_api': blocking})
    c.close()
    return out
```
and in the CLI: `if '--stalls' in sys.argv: print(json.dumps(stalls(sys.argv[1]), indent=1)); sys.exit(0)`.

- [ ] **Step 4: Run tests** — `cd tests/passive_rx && python3 -m unittest test_sysan_nsys -v` — Expected: OK.
- [ ] **Step 5: Commit**

```bash
git add tests/passive_rx/sysan_nsys.py tests/passive_rx/test_sysan_nsys.py
git commit -m "test(sysan): find GPU stalls (consumer launch->start >= 50 ms) and what ran during them in nsys exports"
```

- [ ] **Step 6a (X410 unplugged from 2026-10-16 16:10): trace a replay instead.** Run one capture replay under nsys through the existing replay script (heavy.lock), e.g. `ota_runs/raw_pci415_12s_b` (V) and `ota_runs/raw_pci923_tim_12s_tile3` (T), env t26aXE; label results `[MEASURED, DGX replay <cap>]`, never mixed with OTA numbers. The on-air trace of Step 6 follows when the radio is back.
- [ ] **Step 6: Draft the OTA trace runner (text in the report, orchestrator runs it):** one V and one T run of env `t26aXE` with `nsys profile -t cuda,osrt --cuda-graph-trace=node -d 120 -o <run>/nsys` wrapping the receiver command used by `ota/starttest.sh` (`OTA_RUNNER` hook), plus one run each WITHOUT nsys back-to-back for overhead comparison. State the nsys overhead check: DL tot p50 with/without nsys within 20 %, else the trace is not representative.
- [ ] **Step 7 (after the orchestrator ran it):** `nsys export -t sqlite`, `sysan_nsys.py <db>` and `--stalls`; report per cell: GPU busy share per thread class, launch->start p50/p99 per class, every stall >= 50 ms with its blocker, one representative slot timeline (all kernels and memcpys from slot read to last decode, as a table of start/end offsets in us). Name the stall root cause with evidence, or the ranked candidates and the test that separates them.

---

### Task 5: DL CRC root cause (why real-grant CRC-OK is 36-59 %)

**Files:**
- Create: `tests/passive_rx/dl_crc_split.py`
- Test: `tests/passive_rx/test_goal_metrics.py` (append)
- Report (returned as text): `docs/superpowers/reports/csd/DLCRC-ROOTCAUSE-<date>.md`

**Interfaces:**
- Consumes: `goal_metrics.required_snr_db`, `goal_metrics.KV`, `goal_metrics.ANSI`; Task 2's `cross80_db` per cell.
- Produces: `dl_crc_split.split(paths:list[str], margin:float=2.0) -> dict[str, dict[str, list[int]]]` = for each dimension (`Nl`, `tbl`, `Qm`, `rnti_class`, `rb_bucket`, `nsym`, `cdm`, `cb_bucket`, `core`) a map value -> [n_qualified, ok_qualified]; `rnti_class` = `si` for 0xffff, `ra` for rnti < 0x100, `c` otherwise; `rb_bucket` = `<=24`, `25-96`, `97-200`, `>200`; `cb_bucket` = `1`, `2-7`, `8-15`, `16+`.

- [ ] **Step 1: Write the failing test** (append)

```python
import dl_crc_split as dcs

class DlCrcSplitTest(unittest.TestCase):
    def test_split_by_layers(self):
        p = write([ev(nl=1, snr_l="30,-99,-99,-99"), ev(nl=2, crc=0, snr_l="30,30,-99,-99"),
                   ev(nl=2, crc=1, snr_l="30,30,-99,-99")])
        s = dcs.split([p])
        self.assertEqual(s['Nl']['1'], [1, 1])
        self.assertEqual(s['Nl']['2'], [2, 1])
        self.assertEqual(s['cb_bucket']['1'], [3, 2])
```

- [ ] **Step 2: Run to verify it fails** — `python3 -m unittest test_goal_metrics.DlCrcSplitTest -v` — Expected: `ModuleNotFoundError`.
- [ ] **Step 3: Implement**

```python
#!/usr/bin/env python3
"""SNR-qualified DL CRC split by grant dimension, to find which grant class fails although the SNR suffices.
usage: dl_crc_split.py [--margin 2.0] run.log..."""
import sys
from collections import defaultdict
import goal_metrics as gm

def rb_bucket(n):
    return '<=24' if n <= 24 else '25-96' if n <= 96 else '97-200' if n <= 200 else '>200'

def cb_bucket(n):
    return '1' if n <= 1 else '2-7' if n <= 7 else '8-15' if n <= 15 else '16+'

def split(paths, margin=2.0):
    out = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    for p in paths:
        with open(p, errors='replace') as fh:
            lines = fh.readlines()
        for raw in lines:
            if 'EVMGRANT' not in raw:
                continue
            kv = dict(gm.KV.findall(gm.ANSI.sub('', raw)))
            try:
                if kv.get('probe', '0') != '0' or kv['rv'] != '0':
                    continue
                nl = int(kv['Nl']); snr = min(float(x) for x in kv['snr_l'].split(',')[:max(1, nl)])
                req = gm.required_snr_db(int(kv['mcs']), int(kv['tbl']))
                if req is None or snr < req + margin:
                    continue
                ok = int(kv['crc']); rnti = int(kv['rnti'], 16)
                dims = {'Nl': kv['Nl'], 'tbl': kv['tbl'], 'Qm': kv['Qm'], 'nsym': kv['nsym'], 'cdm': kv.get('cdm', '?'),
                        'rnti_class': 'si' if rnti == 0xffff else 'ra' if rnti < 0x100 else 'c',
                        'rb_bucket': rb_bucket(int(kv['rb'].split('+')[1])), 'cb_bucket': cb_bucket(int(kv.get('cb', 1))),
                        'core': kv.get('core', '?')}
            except (KeyError, ValueError, IndexError):
                continue
            for d, v in dims.items():
                out[d][v][0] += 1; out[d][v][1] += ok
    return {d: dict(v) for d, v in out.items()}

if __name__ == '__main__':
    a = sys.argv[1:]; m = 2.0
    if '--margin' in a:
        i = a.index('--margin'); m = float(a[i + 1]); del a[i:i + 2]
    for d, vals in split(a, m).items():
        print(d, ' '.join(f"{v}:{n}/{100.0 * ok / n:.0f}%" for v, (n, ok) in sorted(vals.items())))
```

- [ ] **Step 4: Run tests** — Expected: OK. **Step 5: Commit** (`git add tests/passive_rx/dl_crc_split.py tests/passive_rx/test_goal_metrics.py`; message `test(dlcrc): SNR-qualified DL CRC split by grant class`).
- [ ] **Step 6: Analyse** V and T separately (runs of Task 2). For every class with qualified CRC-OK < 80 % and n >= 200: read the decode path for that class (`nr_pdsch_passive_decode.c`, DM-RS/CDM handling, layer mapping, PRB-list/PRG grants, `nr_dl_channel_estimation.c`) and state the most likely cause with evidence (EVM, `alpha`, `rho`, `nvar_db` of failing vs passing grants of the class). Rank causes by the number of qualified grants they lose. Propose the single on-air or replay test that confirms the top cause.

---

### Task 6: Target pipeline, 500 us budget and multi-cell-ready GPU API (design report)

**Files:**
- Create: `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md`

**Interfaces:**
- Consumes: reports of Tasks 2-5.
- Produces: the spec the Phase 1 plan is written from.

- [ ] **Step 1: Per-slot timeline target.** From "slot fully read" (t=0) to "last grant decoded" for DL and UL: list every stage in order with its target start/end in us, CPU or GPU, stream and priority. The sum for the worst grant class seen on air (273 PRB, 4 layers, 16+ CBs) must end at <= 500 us; where today's measured cost does not fit, state the gap and the means to close it (port, batch, overlap, kernel change).
- [ ] **Step 2: PDCCH on GPU (B3) + short Phase-2 lock (B5).** Batch layout: all CORESETs and candidates of one slot (and of N cells: `cell` index in every job) in one submission; which existing kernels are reused (`nr_pdcch_gpu_exact_fep_symbol`, `npg_decode_vec`, NID sweep kernels); what remains CPU (DCI accept, RNTI tracking, grant build) and how the lock is split.
- [ ] **Step 3: Memory model.** For each buffer of the pipeline: unified/zero-copy on GB10 (`nr_gpu_model()` = UNIFIED) vs explicit pinned + async copies on discrete (sens6, sm_89, weak FP64): who allocates, who owns, lifetime per slot. One runtime switch, no compile-time fork.
- [ ] **Step 4: Streams, priorities, partitions.** One CUDA context; latency-critical decode streams at highest priority in the RT green context; background (NID sweep, discovery, sensing) at lowest priority in the other partition; rule that no background kernel may run longer than X us (X from Task 4's stall analysis).
- [ ] **Step 5: Port order for Phase 1** ranked by p99 gain per week, with an accuracy test for each port (CPU reference, tolerance) and the OTA A/B that validates it.
- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md
git commit -m "spec(gpu-pipeline): target per-slot GPU pipeline, 500 us budget, memory/stream model, Phase 1 port order"
```

Then the orchestrator writes the Phase 1 plan from this spec and asks the user to review it.
