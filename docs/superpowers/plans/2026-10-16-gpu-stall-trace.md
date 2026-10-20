# GPU stall trace (Phase 0 Task 4, standalone) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Find what stops the GPU for 0.1-1.9 s (the p99.9 driver) and fix the three provisional numbers of the target spec: the maximum background kernel length X, the priority rules, and whether the 40/8 SM split is right.

**Architecture:** A stall finder over nsys sqlite exports (unit-tested on a synthetic DB), then traces of replays now (X410 unplugged) and of OTA runs when the radio is back, then a report that sets X and the rules, and finally a CI-style check that every background kernel stays under X.

**Tech Stack:** nsys (`/usr/local/cuda/bin/nsys`), Python 3 stdlib sqlite3 + unittest, `tests/passive_rx/sysan_nsys.py`, `tests/passive_rx/p99_replay.sh` (`WRAP=` hook).

**Spec:** `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md` sections 4.1-4.3 and 8 item 5; Phase 0 plan `docs/superpowers/plans/2026-10-16-phase0-gpu-inventory-and-metrics.md` Task 4 (this plan supersedes it); evidence `ROOTCAUSE-20261016.md` (global GPU stalls 0.1-1.9 s drive p99.9), `GPU-INVENTORY-20261016.md` (NID kernels max ~1.9 ms, stage-1 594-691 ms host side).

## Global Constraints
Same as the Phase 1 plan `docs/superpowers/plans/2026-10-16-phase1-dl-g1-g3.md` "Global Constraints" (worktree, builds, receivers, replays one at a time through p99_replay.sh, private MPS dir, RAM, commits, evidence labels, no OTA by agents). In addition: nsys runs only inside a replay started by p99_replay.sh (`WRAP="nsys profile ..."`), never attached to a live OTA receiver by an agent; nsys output goes under the replay dir; exports (`.sqlite`) larger than 2 GB are deleted after analysis.

## Review Focus
1. A consumer kernel queued behind a long kernel of ANOTHER thread is a stall; a consumer kernel that simply waits for its own previous kernel on the same stream is not. Pinned by `test_same_stream_wait_is_not_a_stall` (Task 1).
2. Stalls caused by host-side blocking (a CUDA API call such as cudaMalloc/cudaFree/sync holding a lock) must be reported with that API, even when no kernel was running. Pinned by `test_api_blocker_without_kernel` (Task 1).
3. nsys overhead can create stalls of its own: a trace whose DL tot p50 is > 20 % above the same replay without nsys is not representative and must be flagged. Pinned in Task 2 Step 3.

---

### Task 1: Stall finder in sysan_nsys.py

**Files:**
- Modify: `tests/passive_rx/sysan_nsys.py`
- Test: `tests/passive_rx/test_sysan_nsys.py`

**Interfaces:**
- Consumes: nsys sqlite tables `CUPTI_ACTIVITY_KIND_KERNEL(start, end, correlationId, shortName, streamId, globalPid)`, `CUPTI_ACTIVITY_KIND_RUNTIME(start, end, globalTid, correlationId, nameId)`, `StringIds(id, value)`, `ThreadNames(globalTid, nameId, priority)`.
- Produces: `stalls(db_path, gap_ms=50.0, consumer_prefixes=('passivePdsch','passivePdcch','passivePusch')) -> list[dict]`, each `{start_ns, dur_ms, thread, kernel, running_kernels:[name], blocking_api:[(thread, api, dur_ms)]}`, where a stall = a consumer-thread kernel whose launch->start delay >= gap_ms AND whose start is not explained by an earlier kernel on the SAME stream still running at launch time; CLI flag `--stalls [--gap-ms N]` printing JSON.

- [ ] **Step 1: Write the failing tests**

```python
# tests/passive_rx/test_sysan_nsys.py
import sqlite3, tempfile, unittest
import sysan_nsys as sn

SCHEMA = """
CREATE TABLE StringIds(id INTEGER, value TEXT);
CREATE TABLE ThreadNames(globalTid INTEGER, nameId INTEGER, priority INTEGER);
CREATE TABLE CUPTI_ACTIVITY_KIND_RUNTIME(start INTEGER, end INTEGER, globalTid INTEGER, correlationId INTEGER, nameId INTEGER);
CREATE TABLE CUPTI_ACTIVITY_KIND_KERNEL(start INTEGER, end INTEGER, correlationId INTEGER, shortName INTEGER, streamId INTEGER, globalPid INTEGER);
INSERT INTO StringIds VALUES (1,'passivePdsch0'),(2,'k_ldpc'),(3,'cudaLaunchKernel'),(4,'k_big'),(5,'nidWorker'),(6,'cudaFree');
INSERT INTO ThreadNames VALUES (10,1,0),(11,5,0);
"""
MS = 1000000

def db(rows):
    p = tempfile.NamedTemporaryFile(suffix='.sqlite', delete=False).name
    c = sqlite3.connect(p); c.executescript(SCHEMA + rows); c.commit(); c.close(); return p

class StallTest(unittest.TestCase):
    def test_finds_stall_and_blocker(self):
        p = db(f"""INSERT INTO CUPTI_ACTIVITY_KIND_RUNTIME VALUES (0,1000,11,1,3),({10*MS},{10*MS+10000},10,2,3);
                  INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES (0,{200*MS},1,4,7,1),({200*MS},{200*MS+100000},2,2,8,1);""")
        s = sn.stalls(p, gap_ms=50.0)
        self.assertEqual(len(s), 1)
        self.assertGreater(s[0]['dur_ms'], 150)
        self.assertIn('k_big', s[0]['running_kernels'])

    def test_same_stream_wait_is_not_a_stall(self):
        # the consumer's own long kernel on stream 8 delays its next kernel on stream 8: not a stall
        p = db(f"""INSERT INTO CUPTI_ACTIVITY_KIND_RUNTIME VALUES (0,1000,10,1,3),({10*MS},{10*MS+10000},10,2,3);
                  INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES (0,{200*MS},1,2,8,1),({200*MS},{200*MS+100000},2,2,8,1);""")
        self.assertEqual(sn.stalls(p, gap_ms=50.0), [])

    def test_api_blocker_without_kernel(self):
        # nothing runs on the GPU, but another thread sits in cudaFree for 300 ms while the consumer kernel waits
        p = db(f"""INSERT INTO CUPTI_ACTIVITY_KIND_RUNTIME VALUES (0,{300*MS},11,9,6),({10*MS},{10*MS+10000},10,2,3);
                  INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES ({300*MS},{300*MS+100000},2,2,8,1);""")
        s = sn.stalls(p, gap_ms=50.0)
        self.assertEqual(len(s), 1)
        self.assertEqual(s[0]['running_kernels'], [])
        self.assertTrue(any(api == 'cudaFree' for _, api, _ in s[0]['blocking_api']))

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run** — `cd tests/passive_rx && python3 -m unittest test_sysan_nsys -v` — Expected: FAIL, `AttributeError: module 'sysan_nsys' has no attribute 'stalls'`.
- [ ] **Step 3: Implement** (add above the CLI block of sysan_nsys.py; the CLI keeps its existing behaviour when `--stalls` is absent)

```python
def stalls(db_path, gap_ms=50.0, consumer_prefixes=('passivePdsch', 'passivePdcch', 'passivePusch')):
    """Consumer kernels whose launch->start delay is >= gap_ms and is NOT explained by an earlier kernel on the same stream:
    the GPU (or the driver) was held by someone else. Returns the kernels running at launch time and the long CUDA API calls
    of any thread overlapping the wait."""
    c = sqlite3.connect(db_path)
    s = dict(c.execute('SELECT id, value FROM StringIds'))
    tname = {t: s.get(n, '') for t, n in c.execute('SELECT globalTid, nameId FROM ThreadNames')}
    launches = c.execute('SELECT r.start, r.globalTid, k.start, k.end, k.shortName, k.streamId FROM CUPTI_ACTIVITY_KIND_RUNTIME r '
                         'JOIN CUPTI_ACTIVITY_KIND_KERNEL k ON k.correlationId = r.correlationId').fetchall()
    kernels = c.execute('SELECT start, end, shortName, streamId FROM CUPTI_ACTIVITY_KIND_KERNEL').fetchall()
    api = c.execute('SELECT start, end, globalTid, nameId FROM CUPTI_ACTIVITY_KIND_RUNTIME').fetchall()
    c.close()
    gap = gap_ms * 1e6
    out = []
    for r0, tid, k0, k1, kn, st in launches:
        th = tname.get(tid, '')
        if not th.startswith(consumer_prefixes) or (k0 - r0) < gap:
            continue
        own = [b for a, b, n, sid in kernels if sid == st and a < k0 and b > r0]
        if own and max(own) >= k0 - gap:   # it waited for its own stream
            continue
        running = sorted({s.get(n, str(n)) for a, b, n, sid in kernels if a <= r0 < b and sid != st})
        blocking = [(tname.get(t, str(t)), s.get(n, str(n)), (b - a) / 1e6) for a, b, t, n in api
                    if t != tid and a < k0 and b > r0 and (b - a) >= gap / 10]
        out.append({'start_ns': r0, 'dur_ms': (k0 - r0) / 1e6, 'thread': th, 'kernel': s.get(kn, str(kn)),
                    'running_kernels': running, 'blocking_api': blocking})
    return out
```
CLI: before the existing argument handling add
```python
    if '--stalls' in sys.argv:
        g = float(sys.argv[sys.argv.index('--gap-ms') + 1]) if '--gap-ms' in sys.argv else 50.0
        print(json.dumps(stalls(sys.argv[1], g), indent=1)); sys.exit(0)
```
(read the existing `if __name__ == '__main__':` block and place it as its first statement).
- [ ] **Step 4: Run** — 3 tests OK; `python3 tests/passive_rx/sysan_nsys.py -h` still behaves as before.
- [ ] **Step 5: Commit**

```bash
git add tests/passive_rx/sysan_nsys.py tests/passive_rx/test_sysan_nsys.py
git commit -m "test(sysan): GPU stall finder in nsys exports (cross-stream waits, blocking CUDA API calls)"
```

### Task 2: Replay traces (now) and nsys overhead check

**Files:** none in the repo; report `docs/superpowers/reports/csd/GPU-TIMELINE-<date>.md` (returned as text).

- [ ] **Step 1:** Replay without nsys, then with nsys, for each capture (one at a time; ~90 s each):
```bash
cd /home/nicola/NICOLA/wt/rr-csd-int16
export ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env TMO=90
CAPSET=b   tests/passive_rx/p99_replay.sh stallV_plain
CAPSET=b   WRAP="/usr/local/cuda/bin/nsys profile -t cuda,osrt -s none --cuda-graph-trace=node -d 80 -o nsys_trace" tests/passive_rx/p99_replay.sh stallV_nsys
CAPSET=tim tests/passive_rx/p99_replay.sh stallT_plain
CAPSET=tim WRAP="/usr/local/cuda/bin/nsys profile -t cuda,osrt -s none --cuda-graph-trace=node -d 80 -o nsys_trace" tests/passive_rx/p99_replay.sh stallT_nsys
```
nsys writes `nsys_trace.nsys-rep` into the replay dir (p99_replay.sh cds there). Export: `/usr/local/cuda/bin/nsys export -t sqlite -o <dir>/nsys_trace.sqlite <dir>/nsys_trace.nsys-rep`.
- [ ] **Step 2:** `goal_metrics.py` on plain vs nsys run.log of each capture: DL tot p50/p99.
- [ ] **Step 3:** If nsys tot p50 > 1.2 x plain tot p50, mark the trace "not representative" in the report and retry once with `-t cuda` only (no osrt).
- [ ] **Step 4:** `sysan_nsys.py <db>` (budget per thread class, launch->start p50/p99) and `sysan_nsys.py <db> --stalls` per capture.
- [ ] **Step 5: Report** per capture (label `[MEASURED, DGX replay <cap>, t26aXE, nsys, n=1]`): GPU busy share per thread class; launch->start p50/p99 per class; every stall >= 50 ms with its blockers; the longest kernel of each background class (NID sweep, CSI-RS discovery, DM-RS check, UL pool); one representative slot timeline (kernels and copies from slot read to last decode, start/end offsets in us). Name the stall cause with evidence, or the ranked candidates and the single test that separates them. Note: replay has no DPDK/USRP load; the on-air trace (Task 4) decides.

### Task 3: Set X, the priority rules and the SM split (spec update)

**Files:**
- Modify: `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md` sections 4.1, 4.2, 4.3, 8 item 5 (replace each "TO BE FIXED BY TASK 4" with the measured value and its evidence label)

- [ ] **Step 1:** X = the largest background kernel duration that does not delay an RT kernel start by more than 50 us in the trace (from Task 2's launch->start of RT consumer kernels overlapping BG kernels); if the stall cause is host-side (API/lock), say so and keep X provisional.
- [ ] **Step 2:** Priority rules: keep, change or drop each of 4.3 items 1-5 with the trace evidence.
- [ ] **Step 3:** SM split: compare RT kernel durations when BG kernels run concurrently vs not (same kernel names); if RT kernels slow down > 20 % with BG active on the other partition, record "partition does not isolate (memory bandwidth)" and propose the BG duty cycle instead.
- [ ] **Step 4: Commit** `spec(gpu-pipeline): background kernel bound X, priority rules and SM split from the stall trace`.

### Task 4: On-air trace (orchestrator, when the X410 is back)

- [ ] **Step 1:** Orchestrator runs one V and one T OTA window with the receiver wrapped by nsys (same flags as Task 2, through `OTA_RUNNER`/`WRAP` in `ota/starttest.sh`), plus a plain run each, interleaved, daytime.
- [ ] **Step 2:** An agent repeats Task 2 Steps 2-5 on the OTA exports (label OTA) and Task 3 if the values differ from the replay ones.

### Task 5: Background-kernel bound check

**Files:**
- Modify: `tests/passive_rx/sysan_nsys.py` (function `bg_over_x(db_path, x_us, bg_prefixes=('nidWorker', 'csirsDisc', 'dmrsIdBg', 'gpuCheckBg')) -> list[(thread, kernel, dur_us)]`)
- Test: `tests/passive_rx/test_sysan_nsys.py` (`test_bg_over_x`: one 300 us kernel launched by `nidWorker` with x_us=200 -> one hit; a 300 us kernel launched by `passivePdsch0` -> none)

- [ ] **Step 1:** failing test; **Step 2:** run FAIL; **Step 3:** implement (join RUNTIME->KERNEL by correlationId, filter launching thread name by prefix, keep `end-start > x_us*1000`); **Step 4:** tests OK; **Step 5:** commit `test(sysan): flag background kernels longer than X`. This is the regression gate of spec section 8 item 5 for every later port.
