# Multi-RX Sensing Fusion into the Agnostic Passive Receiver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge sens6's `adaptive-rx-UL-DL` passive receiver into `feature/multirx-clean-adaptive`
(from `mygitlab/feature/multirx-clean-detector`). Feed the frozen multi-RX sensing chain with
per-channel Ĥ from dedicated DL/UL grants, gate it on discovered DL+UL flows, and add per-block
debugging, a live 3D/RD monitor, a one-command launcher and OTA validation.

**Architecture:**
- The receiver code stays byte-identical to adaptive, except for the CFR-producer glue.
- `openair1/PHY/NR_UE_ISAC/` is the feature branch's frozen chain. Engine stages [1]–[6] run
  in-process: C++ with CUDA-first, CPU fallback.
- Stages [7]–[10] run in `tools/realtime_chain.py` as a separate pinned process, tailing
  `reports.jsonl`.
- A browser monitor tails `reports.jsonl`, `tracks.jsonl` and the receiver log.

**Tech Stack:** C11/C++17 (OAI), CUDA 12.4 (sm_89, host compiler g++-13), CMake + Ninja,
Python 3 (numpy 2.3, scipy 1.16), Plotly.js 2.35.2 (vendored), bash.

**Spec:** `docs/superpowers/specs/2026-09-23-multirx-sensing-fusion-design.md` (read it first; it
states WHY for every rule below).

## Global Constraints

- Work ONLY on sens6, in `/home/sens/NICOLA/multirx-clean-adaptive`, on branch
  `feature/multirx-clean-adaptive`. The branch has no upstream on purpose; never `git push` without
  an explicit user instruction.
- Merge source is sens6 `adaptive-rx-UL-DL` at commit `51f7d3deac`.
- **Fully passive:** no TX path is added anywhere.
- **Fully agnostic:** no gNB/cell value is hardcoded, pinned in a conf, or passed by the launcher.
  - The ONLY exceptions are the front-end start values `-r 273 --numerology 1 -C 3450000000`
    (decided by the user).
  - The SSB is found by `--ue-scan-carrier` (`SCAN=1`).
  - The CFO seed is `INITIALFO=0`.
- **Receiver code is adaptive wholesale.** After the merge, every path except
  `openair1/PHY/NR_UE_ISAC/`, `tests/passive_rx/monitor/`, `docs/superpowers/` and
  `CMakeLists.txt` equals `51f7d3deac`, until a task below explicitly edits a producer.
- **Sensing never blocks or feeds back** into the receiver. `ENABLE_ISAC_SENSING=OFF` must build
  today's receiver.
- **Never build while a capture runs.** Check `pgrep -x nr-uesoftmodem` is empty before any
  `ninja`.
- **After killing a capture:** wait 60 s, then `ping -c1 192.168.20.2` and `uhd_find_devices`,
  before relaunching.
- **Build flags** (copied from the working adaptive build, plus CUDA sensing):
  `-GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVX2=ON -DOAI_USRP=ON -DOAI_RF_EMULATOR=ON
  -DOAI_SIMU=ON -DT_TRACER=ON -DENABLE_TESTS=ON -DENABLE_LDPC_CUDA=ON
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-13 -DASN1C_EXEC=/opt/asn1c/bin/asn1c`.
  - Sensing build adds: `-DENABLE_ISAC_SENSING=ON -DENABLE_CHANNEL_SIM_CUDA=ON
    -DCMAKE_CUDA_ARCHITECTURES=89`.
  - Receiver-only build adds: `-DENABLE_ISAC_SENSING=OFF`.
- **dlopen'd plugins** must be rebuilt explicitly: `ninja oai_usrpdevif` (USRP) whenever
  `radio/USRP` changes.
- **sens6 core map:**
  - 14 cores. 2–3 are isolated (the RF reader is pinned on 2). The softmodem runs on 0–7 with the
    thread pool on 0,1,4,5,6,7. NIC IRQs are on 8–13.
  - Sensing engine threads: `NR_ISAC_CPUS=3,12,13`, SCHED_OTHER.
  - `realtime_chain.py`: core 11. `monitor.py`: core 10.
  - Validated by Task 16's A/B (NIC `rx_missed_errors` must not rise).
- **Commit messages** end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

Shorthand used below: `W=/home/sens/NICOLA/multirx-clean-adaptive`,
`BON=$W/cmake_targets/ran_build/build_sense` (sensing ON),
`BOFF=$W/cmake_targets/ran_build/build_rx` (sensing OFF).

---

### Task 1: Merge with the conflict policy

**Files:**
- Modify: whole tree (merge commit)
- Create (scratch, not committed): `/tmp/merge_cmake.py`

**Interfaces:**
- Produces: a merge commit whose non-sensing paths equal `51f7d3deac`, and a hybrid
  `CMakeLists.txt`: adaptive's, with the feature's `NR_UE_ISAC` library block and ISAC test block.

- [ ] **Step 1: Preconditions**

```bash
cd $W && pgrep -x nr-uesoftmodem; git status --short; git log -1 --format=%h
```
Expected: no pid, empty status, HEAD = the plan commit.

- [ ] **Step 2: Start the merge (records both parents)**

```bash
cd $W && git merge --no-ff --no-commit 51f7d3deac || true
git status --short | grep -c '^UU\|^DU\|^UD\|^AA'
```
Expected: 16 conflicted paths (the list in the spec session: CMakeLists.txt, executables/nr-ue.c,
nr_isac.cc, 3 NR_UE_ISAC tests/tools, 7 NR_UE_TRANSPORT files, 3 tests/passive_rx files).

- [ ] **Step 3: Set the tree to adaptive wholesale, then take the sensing paths from the feature
  branch**

```bash
cd $W
git read-tree --reset -u 51f7d3deac
git rm -r -q -f openair1/PHY/NR_UE_ISAC tests/passive_rx/monitor
git checkout HEAD -- openair1/PHY/NR_UE_ISAC tests/passive_rx/monitor docs/superpowers
# receiver-side fixed-point test that happens to live under NR_UE_ISAC/tests (adaptive WIP):
git checkout 51f7d3deac -- openair1/PHY/NR_UE_ISAC/tests/dlsch_fixed_point_test.cc
test -f .git/MERGE_HEAD || test -f $(git rev-parse --git-dir)/MERGE_HEAD && echo merge-in-progress
```
Expected: `merge-in-progress`.

- [ ] **Step 4: Write the CMake hybridiser**

```python
# /tmp/merge_cmake.py -- run from $W. Base = adaptive CMakeLists.txt (already in the worktree).
import pathlib, re, subprocess, sys

cm = pathlib.Path("CMakeLists.txt")
ad = cm.read_text().split("\n")
fe = subprocess.check_output(["git", "show", "HEAD:CMakeLists.txt"], text=True).split("\n")

def lib_block(lines):
    s = next(i for i, l in enumerate(lines) if l.startswith("set(NR_UE_ISAC_SRC"))
    m = next(i for i in range(s, len(lines)) if "file sink only" in lines[i])
    e = next(i for i in range(m, len(lines)) if lines[i].strip() == "endif()")
    return s, e

# 1) library block: adaptive's -> feature's (sources, CUDA append, OFF stub list, CUDA defs, ZMQ)
a0, a1 = lib_block(ad); f0, f1 = lib_block(fe)
ad[a0:a1 + 1] = fe[f0:f1 + 1]

# 2) drop adaptive ISAC test/tool targets whose source file no longer exists
def statements(lines):
    i = 0
    while i < len(lines):
        m = re.match(r"\s*([A-Za-z_]+)\(", lines[i])
        if not m:
            i += 1; continue
        depth, j = 0, i
        while True:
            depth += lines[j].count("(") - lines[j].count(")")
            if depth <= 0: break
            j += 1
        yield i, j, m.group(1), " ".join(lines[i:j + 1])
        i = j + 1

dead = set()
for i, j, cmd, text in statements(ad):
    if cmd == "add_executable":
        name = re.match(r"\s*add_executable\(\s*([A-Za-z0-9_]+)", text).group(1)
        for src in re.findall(r"\$\{OPENAIR1_DIR\}/(PHY/NR_UE_ISAC/(?:tests|tools)/[A-Za-z0-9_]+\.cc)", text):
            if not pathlib.Path("openair1", src).exists():
                dead.add(name)
drop = set()
for i, j, cmd, text in statements(ad):
    for name in dead:
        if re.search(r"\(\s*(?:NAME\s+)?%s\b" % name, text) or re.search(r"add_dependencies\(\s*tests\s+%s\s*\)" % name, text):
            drop.update(range(i, j + 1))
ad = [l for k, l in enumerate(ad) if k not in drop]

# 3) insert the feature's `if(ENABLE_ISAC_SENSING) ... endif()` test block into adaptive's tests
s = next(i for i, l in enumerate(fe) if "add_executable(test_nr_isac_python_parity" in l) - 1
assert fe[s].strip() == "if(ENABLE_ISAC_SENSING)", fe[s]
depth, e = 0, s
for e in range(s, len(fe)):
    depth += len(re.findall(r"^\s*if\(", fe[e])) - len(re.findall(r"^\s*endif\(", fe[e]))
    if depth == 0: break
anchor = next(i for i, l in enumerate(ad) if "ota_sync_passive_ue.md Phase 6a" in l)
ins = next(i for i in range(anchor, len(ad)) if ad[i].strip() == "if(ENABLE_TESTS)") + 1
ad[ins:ins] = fe[s:e + 1]

out = "\n".join(ad)
missing = [p for p in re.findall(r"\$\{OPENAIR1_DIR\}/(PHY/NR_UE_ISAC/[A-Za-z0-9_/]+\.(?:cc|c|cu))", out)
           if not pathlib.Path("openair1", p).exists()]
ifs = len(re.findall(r"^\s*if\(", out, re.M)); endifs = len(re.findall(r"^\s*endif\(", out, re.M))
if missing or ifs != endifs:
    sys.exit(f"FAIL missing={missing} if={ifs} endif={endifs}")
cm.write_text(out)
print(f"ok: dropped targets {sorted(dead)}")
```

- [ ] **Step 5: Run it**

Run: `cd $W && python3 /tmp/merge_cmake.py`
Expected: `ok: dropped targets [...]`, listing only adaptive ISAC tests (e.g. `test_isac_sync`,
`isac_sync_replay`, `test_eca_clutter`, `test_target_tracker`, ...). It must never list
`test_dlsch_fixed_point` or any `test_nr_pdcch_*`.

- [ ] **Step 6: Verify the policy mechanically**

```bash
cd $W && git add -A
git diff --cached --stat 51f7d3deac -- . ':!openair1/PHY/NR_UE_ISAC' ':!tests/passive_rx/monitor' \
  ':!docs/superpowers' ':!CMakeLists.txt' | tail -1
git diff --cached --stat HEAD -- openair1/PHY/NR_UE_ISAC ':!openair1/PHY/NR_UE_ISAC/tests/dlsch_fixed_point_test.cc' \
  tests/passive_rx/monitor docs/superpowers | tail -1
```
Expected: both print nothing (empty diff).

- [ ] **Step 7: Commit the merge**

```bash
cd $W && git commit -q -F - <<'EOF'
Merge adaptive-rx-UL-DL (51f7d3deac) into the multi-RX sensing branch

Policy (docs/superpowers/specs/2026-09-23-multirx-sensing-fusion-design.md §4): every receiver
path is adaptive's version byte for byte; openair1/PHY/NR_UE_ISAC and tests/passive_rx/monitor are
the feature branch's frozen chain; CMakeLists.txt is adaptive's with the feature's NR_UE_ISAC
library and test blocks. The feature branch's parallel receiver lineage (P02-P13 multi-branch,
usrp_lib, rfsim Sionna harness) is intentionally not carried over.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
git log -1 --format='%h %p'
```
Expected: one hash plus two parent hashes.

---

### Task 2: Receiver-only build (sensing OFF) and stub completeness

**Files:**
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac_stub.c` (only if the link reports a missing `nr_isac_*`
  symbol)

**Interfaces:**
- Produces: a `$BOFF/nr-uesoftmodem` built from receiver code identical to adaptive.

- [ ] **Step 1: Configure and build**

```bash
pgrep -x nr-uesoftmodem && exit 1
mkdir -p $BOFF && cd $BOFF && cmake $W -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVX2=ON -DOAI_USRP=ON \
  -DOAI_RF_EMULATOR=ON -DOAI_SIMU=ON -DT_TRACER=ON -DENABLE_TESTS=ON -DENABLE_LDPC_CUDA=ON \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-13 -DASN1C_EXEC=/opt/asn1c/bin/asn1c -DENABLE_ISAC_SENSING=OFF \
  | grep -E "NR_UE_ISAC|Error" ; ninja -j8 nr-uesoftmodem oai_usrpdevif ldpc ldpc_cuda 2>&1 | tail -5
```
Expected: `NR_UE_ISAC: sensing pipeline DISABLED`, then the build succeeds.

- [ ] **Step 2: If the link fails with `undefined reference to nr_isac_<name>`**

Add a no-op of the exact signature from `nr_isac.h` to `nr_isac_stub.c`. Returns are 0 / `NULL`,
and `void` functions have an empty body. Example of the pattern:
```c
uint32_t nr_isac_rx_channels(void) { return 0; }
```
Rebuild until clean. Anything other than an `nr_isac_*` symbol is a merge defect: stop and report
it; do not patch receiver code.

- [ ] **Step 3: Run the receiver unit tests that exist in adaptive**

```bash
cd $BOFF && ninja -j8 test_nr_pdcch_blind_monitor test_nr_pdcch_blind_rnti_bootstrap test_dlsch_fixed_point \
  && ./test_nr_pdcch_blind_monitor && ./test_nr_pdcch_blind_rnti_bootstrap && ./test_dlsch_fixed_point
```
Expected: all PASS. They must behave exactly as on `adaptive-rx-UL-DL`; if one fails, run the same
test in `/home/sens/NICOLA/adaptive-rx-UL-DL`'s build to prove it is pre-existing before going on.

- [ ] **Step 4: Commit (only if Step 2 changed the stub)**

```bash
cd $W && git add openair1/PHY/NR_UE_ISAC/nr_isac_stub.c && git commit -q -m "isac stub: no-ops for the receiver-facing API the adaptive producers call

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Sensing build (ON + CUDA), API alignment, feature tests

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/csi_rx.c:967`, `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5846`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c:193`, `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1609-1610`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_data_aided.c:195`, `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:~970`
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac.cc` (one-shot rejection log)
- Modify: `openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc` (only if it fails to compile)

**Interfaces:**
- Consumes: the feature API in `nr_isac.h`: `nr_isac_rx_channels()`,
  `nr_isac_submit_cfr_multi_session(...)`.
- Produces: `$BON/nr-uesoftmodem` with the engine linked in. Every producer submits exactly
  `nr_isac_rx_channels()` antennas, and UL submits with `session_id = rnti`.

- [ ] **Step 1: Replace the removed AoA antenna query** (the feature's `nr_isac.cc` has no
  `nr_isac_aoa_antennas`, only its stub does, so a sensing-ON link would fail)

```bash
cd $W && grep -rln 'nr_isac_aoa_antennas()' openair1 | grep -v NR_UE_ISAC \
  | xargs sed -i 's/nr_isac_aoa_antennas()/nr_isac_rx_channels()/g'
grep -rn 'nr_isac_aoa_antennas' openair1 --include=*.c | grep -v NR_UE_ISAC
```
Expected: the last grep prints nothing.

- [ ] **Step 2: UL submissions carry the C-RNTI** (the feature ABI silently drops UL with
  `session_id == 0`)

In `nr_pusch_data_aided.c`, replace the final submit:
```c
  nr_isac_submit_cfr_multi_session(ul_slot_idx, 0.0f, NR_ISAC_SRC_PUSCH_DATA, &carrier, h_buf, nant, cap,
                                   k_buf, l_buf, nof_re, 1.0f, (uint64_t)g->rnti);
```
In `nr_pusch_passive_decode.c`, the `NR_ISAC_SRC_PUSCH_DMRS` submit (the `nr_isac_submit_cfr_multi(ul_slot_idx, 0.0f, NR_ISAC_SRC_PUSCH_DMRS, ...)` call) becomes:
```c
        nr_isac_submit_cfr_multi_session(ul_slot_idx, 0.0f,
                                         NR_ISAC_SRC_PUSCH_DMRS, &carrier, ul_h, nof_ant_cfr, cap,
                                         ul_k, ul_l, nof_re, 1.0f, (uint64_t)g->rnti);
```
Verify there is no other UL submit left: `grep -n 'NR_ISAC_SRC_PUSCH' openair1/PHY/NR_UE_TRANSPORT/*.c`
must show only `_session` calls.

- [ ] **Step 3: Make ABI rejections visible.** In `nr_isac.cc`, inside
  `nr_isac_submit_cfr_multi_session`, replace the silent
  `if(!expected_channels||antennas!=expected_channels||...)return;` with:

```cpp
  if(!expected_channels||antennas!=expected_channels||antennas>4||stride<n
      ||carrier->nof_prb<1||carrier->nof_prb>275||n>maximum_re){
    static std::atomic<bool> warned{false};
    if(!warned.exchange(true))
      LOG_E(PHY,"SENSING: CFR rejected at the ABI: antennas=%u expected=%u stride=%u n=%u nof_prb=%u "
            "(spatial mode needs --ue-nb-ant-rx 4); further rejections are silent\n",
            antennas,expected_channels,stride,n,carrier->nof_prb);
    return;
  }
```
And the UL-without-session drop gets the same one-shot `LOG_E` ("UL CFR without session_id").

- [ ] **Step 4: Configure and build**

```bash
pgrep -x nr-uesoftmodem && exit 1
mkdir -p $BON && cd $BON && cmake $W -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVX2=ON -DOAI_USRP=ON \
  -DOAI_RF_EMULATOR=ON -DOAI_SIMU=ON -DT_TRACER=ON -DENABLE_TESTS=ON -DENABLE_LDPC_CUDA=ON \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-13 -DASN1C_EXEC=/opt/asn1c/bin/asn1c \
  -DENABLE_ISAC_SENSING=ON -DENABLE_CHANNEL_SIM_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 | grep -E "NR_UE_ISAC|Error"
ninja -j8 nr-uesoftmodem oai_usrpdevif ldpc ldpc_cuda 2>&1 | grep -E "error|FAILED|warning: unused" | head -40
```
Expected: `sensing pipeline ENABLED`, `native CUDA sync and detector enabled`, and no errors.
- Fix each compile error with the smallest change, one commit per root cause.
- A missing receiver-facing symbol is fixed in the feature code (NR_UE_ISAC), never by editing
  the engine's algorithms.

- [ ] **Step 5: Build and run the feature tests**

```bash
cd $BON && ninja -j8 test_nr_isac_python_parity test_nr_isac_cuda_sync test_nr_isac_cuda_detector_parity \
  test_nr_isac_cuda_family_processing benchmark_nr_isac_cuda_detector 2>&1 | tail -3
./test_nr_isac_python_parity && ./test_nr_isac_cuda_sync && ./test_nr_isac_cuda_detector_parity \
  && ./test_nr_isac_cuda_family_processing && ./benchmark_nr_isac_cuda_detector
```
Expected: all pass, and the benchmark median is < 200 ms.
- If `python_parity_test.cc` fails to compile on `#include "detector.h"`: switch the include to
  `clean_detector.h`, and delete only the test cases that exercise the removed single-RX
  `detector.cc` API. List each deleted case in the commit message. Never restore `detector.cc`.

- [ ] **Step 6: Commit**

```bash
cd $W && git add -A openair1 && git commit -q -m "sensing: align the adaptive CFR producers with the multi-RX ABI

nr_isac_aoa_antennas() -> nr_isac_rx_channels() (the engine now owns the channel count); UL
producers submit through nr_isac_submit_cfr_multi_session with the C-RNTI, since the ABI drops
session-less UL; ABI rejections log once instead of vanishing.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: DL+UL flow gate, engine discard, thread pinning

**Files:**
- Create: `openair1/PHY/NR_UE_ISAC/flow_gate.h`
- Create: `openair1/PHY/NR_UE_ISAC/tests/flow_gate_test.cc`
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac.h`, `openair1/PHY/NR_UE_ISAC/nr_isac.cc`, `openair1/PHY/NR_UE_ISAC/nr_isac_stub.c`
- Modify: `openair1/PHY/NR_UE_ISAC/sensing_engine.h`, `openair1/PHY/NR_UE_ISAC/sensing_engine.cc`
- Modify: `CMakeLists.txt` (register the test)

**Interfaces:**
- Produces (C API, `nr_isac.h`):
  - `void nr_isac_flow_note(uint16_t rnti, int uplink);`
  - `int nr_isac_flow_admit(uint16_t rnti);`
  - `void nr_isac_request_discard(void);`
  - `int nr_isac_drained(void);`
- Produces (C++): `nr_isac::FlowGate` with `note/admit/poll/open`;
  `SensingEngine::request_discard_pending()`, `SensingEngine::gate_discarded_rows()`,
  `nr_isac::pin_current_thread_from_env()`.

- [ ] **Step 1: Write the failing test**

```cpp
// openair1/PHY/NR_UE_ISAC/tests/flow_gate_test.cc
#include "flow_gate.h"
#include <cstdio>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
int main() {
  nr_isac::FlowGate g(2.0);
  require(!g.open(), "starts closed");
  require(g.note(0x4601, false, 10.0) == 0, "DL alone does not open");
  require(!g.admit(0x4601, 10.0), "DL-only RNTI not admitted");
  require(g.note(0x4601, true, 10.5) == 1, "DL+UL opens");
  require(g.admit(0x4601, 10.6), "flow admitted");
  require(!g.admit(0x4602, 10.6), "unknown RNTI not admitted");
  require(g.note(0x4602, false, 11.0) == 0, "second RNTI DL-only: gate already open");
  require(!g.admit(0x4602, 11.0), "second RNTI is not a flow yet");
  require(g.poll(12.4) == 0, "UL 1.9 s old: still open");
  require(g.poll(12.6) == -1, "UL 2.1 s old: closes");
  require(!g.open() && !g.admit(0x4601, 12.6), "closed admits nothing");
  require(g.poll(13.0) == 0, "close reported once");
  require(g.note(0x4601, false, 20.0) == 0 && g.note(0x4601, true, 20.1) == 1, "re-opens");
  std::puts("flow_gate_test: PASS");
  return 0;
}
```

- [ ] **Step 2: Register it and run it to see it fail.** Append to the feature ISAC test block
  in `CMakeLists.txt`, just before its closing `endif()` (the one after `benchmark_nr_isac_cuda_family_processing`):

```cmake
    add_executable(test_nr_isac_flow_gate ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/tests/flow_gate_test.cc)
    target_include_directories(test_nr_isac_flow_gate PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_ISAC)
    add_dependencies(tests test_nr_isac_flow_gate)
    add_test(NAME test_nr_isac_flow_gate COMMAND ./test_nr_isac_flow_gate)
```
Run: `cd $BON && cmake . >/dev/null && ninja test_nr_isac_flow_gate`
Expected: FAIL with `flow_gate.h: No such file or directory`.

- [ ] **Step 3: Implement `flow_gate.h`**

```cpp
// openair1/PHY/NR_UE_ISAC/flow_gate.h
/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace nr_isac {

/** A C-RNTI is a flow while it produced a DL CFR AND a UL CFR within the last window_s.
 *  The sensing gate is open while at least one flow exists (spec §8). */
class FlowGate {
public:
  explicit FlowGate(double window_s) : window_s_(window_s) {}

  /** Returns 1 when this event opened the gate, else 0. */
  int note(uint16_t rnti, bool uplink, double now_s)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Flow& f = flows_[rnti];
    (uplink ? f.last_ul_s : f.last_dl_s) = now_s;
    const bool was_open = open_;
    open_ = any_flow(now_s);
    return (!was_open && open_) ? 1 : 0;
  }

  bool admit(uint16_t rnti, double now_s) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = flows_.find(rnti);
    return open_ && it != flows_.end() && is_flow(it->second, now_s);
  }

  /** Expires stale flows. Returns -1 when this poll closed the gate, else 0. */
  int poll(double now_s)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = flows_.begin(); it != flows_.end();)
      it = (now_s - std::max(it->second.last_dl_s, it->second.last_ul_s) > window_s_) ? flows_.erase(it) : std::next(it);
    const bool was_open = open_;
    open_ = any_flow(now_s);
    return (was_open && !open_) ? -1 : 0;
  }

  bool open() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
  }

private:
  struct Flow {
    double last_dl_s = -1e300;
    double last_ul_s = -1e300;
  };
  bool is_flow(const Flow& f, double now_s) const
  {
    return now_s - f.last_dl_s <= window_s_ && now_s - f.last_ul_s <= window_s_;
  }
  bool any_flow(double now_s) const
  {
    for (const auto& item : flows_)
      if (is_flow(item.second, now_s)) return true;
    return false;
  }
  double window_s_;
  mutable std::mutex mutex_;
  std::unordered_map<uint16_t, Flow> flows_;
  bool open_ = false;
};

} // namespace nr_isac
```

- [ ] **Step 4: Run the test**

Run: `cd $BON && ninja test_nr_isac_flow_gate && ./test_nr_isac_flow_gate`
Expected: `flow_gate_test: PASS`

- [ ] **Step 5: Engine discard + counter.** In `sensing_engine.h` add to the `public:` section:

```cpp
  /** Gate closed: drop every pending (not yet windowed) row before the next submission is consumed.
   *  Counted apart from discarded_pending_rows, which keeps meaning "loss". */
  void request_discard_pending() { discard_requested_.store(true, std::memory_order_release); }
  uint64_t gate_discarded_rows() const { return gate_discarded_rows_.load(std::memory_order_relaxed); }
```
and to `private:`:
```cpp
  void discard_pending_rows();
  std::atomic<bool> discard_requested_{false};
  std::atomic<uint64_t> gate_discarded_rows_{0};
```
In `sensing_engine.cc`, add after `SensingEngine::close_ready_windows`:
```cpp
void SensingEngine::discard_pending_rows()
{
  std::vector<int64_t> keys;
  keys.reserve(rows_.size());
  for (const auto& item : rows_) keys.push_back(item.first);
  gate_discarded_rows_.fetch_add(keys.size(), std::memory_order_relaxed);
  erase_rows(keys);
  active_plan_.reset();
}
```
In `SensingEngine::accumulation_run()`, immediately after `Snapshot* value = ready_.wait_pop();`
(and after its null/stop check), insert:
```cpp
    if (discard_requested_.exchange(false, std::memory_order_acq_rel)) discard_pending_rows();
```
Read `erase_rows()` first. It must also decrement `pending_row_bytes_`; if it does not, subtract
`pending_row_storage_bytes(rows_.at(key))` for each key before erasing, inside
`discard_pending_rows`.

- [ ] **Step 6: Thread pinning helper.** Add to `sensing_engine.cc` (top, in `namespace nr_isac`):

```cpp
/** Engine threads leave the PHY's SCHED_FIFO class and, when NR_ISAC_CPUS="3,12,13" is set, run
 *  only on those cores -- a std::thread created from a FIFO PHY thread inherits FIFO otherwise. */
void pin_current_thread_from_env()
{
  sched_param sp{}; sp.sched_priority = 0;
  pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
  const char* cpus = std::getenv("NR_ISAC_CPUS");
  if (!cpus || !*cpus) return;
  cpu_set_t set; CPU_ZERO(&set);
  std::stringstream s(cpus); std::string tok;
  while (std::getline(s, tok, ',')) if (!tok.empty()) CPU_SET(std::stoi(tok), &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}
```
(Add `#include <pthread.h>`, `#include <sched.h>`, `#include <sstream>` if absent.) Call
`pin_current_thread_from_env();` as the first statement of `accumulation_run()`,
`processing_run()`, and the worker loop of `SpatialDetectorExecutor::Worker` (find it with
`grep -n "struct Worker\|class Worker" sensing_engine.cc`).

- [ ] **Step 7: C API + watchdog in `nr_isac.cc`**

Add to `nr_isac.h`, before `#ifdef __cplusplus }`:
```c
/** Receiver: a CFR was extracted from a decoded grant of C-RNTI `rnti` (uplink != 0 for PUSCH). */
void nr_isac_flow_note(uint16_t rnti, int uplink);
/** 1 when `rnti` is currently a DL+UL flow and the sensing gate is open (spec §8). */
int nr_isac_flow_admit(uint16_t rnti);
/** Drop the engine's partial CPI (gate close, or a recorded close replayed offline). */
void nr_isac_request_discard(void);
/** 1 when every accepted submission has been consumed (offline replay). */
int nr_isac_drained(void);
```
In `nr_isac.cc`, add near the other file-scope globals (`engine`, `enabled`, `started`):
```cpp
#include "flow_gate.h"
#include <chrono>
#include <thread>
#include <time.h>
namespace {
double monotonic_s(){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+1e-9*ts.tv_nsec;}
nr_isac::FlowGate flow_gate(2.0);
std::atomic<uint64_t> gate_admitted{0},gate_rejected{0};
std::atomic<bool> gate_watchdog_run{false};
std::thread gate_watchdog;
}
extern "C" void nr_isac_flow_note(uint16_t rnti,int uplink)
{
  if(!enabled.load(std::memory_order_relaxed))return;
  if(flow_gate.note(rnti,uplink!=0,monotonic_s())>0)LOG_I(PHY,"SENSING_GATE open rnti=0x%04x\n",rnti);
}
extern "C" int nr_isac_flow_admit(uint16_t rnti)
{
  if(!enabled.load(std::memory_order_relaxed))return 0;
  const bool ok=flow_gate.admit(rnti,monotonic_s());
  (ok?gate_admitted:gate_rejected).fetch_add(1,std::memory_order_relaxed);
  return ok?1:0;
}
extern "C" void nr_isac_request_discard(void){if(engine)engine->request_discard_pending();}
extern "C" int nr_isac_drained(void){return engine&&engine->submissions_drained()?1:0;}
```
Replace `nr_isac_start()` / `nr_isac_stop()` with:
```cpp
extern "C" void nr_isac_start(void)
{
  bool expected = false;
  if (!enabled.load() || !engine || !started.compare_exchange_strong(expected, true)) return;
  try {
    engine->start();
  } catch (const std::exception& error) {
    started.store(false);
    enabled.store(false);
    LOG_E(PHY, "SENSING: startup failed before CFR admission: %s\n", error.what());
    return;
  }
  gate_watchdog_run.store(true);
  gate_watchdog = std::thread([] {
    double last_stats = monotonic_s();
    while (gate_watchdog_run.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const double now = monotonic_s();
      if (flow_gate.poll(now) < 0) {
        LOG_I(PHY, "SENSING_GATE close reason=no_dl_ul_flow_for_2s\n");
        nr_isac_request_discard();
        nr_isac_record_gate_close(); // Task 10; until then define it as an empty static function
      }
      if (now - last_stats >= 10.0) {
        last_stats = now;
        LOG_I(PHY, "SENSING_GATE stats open=%d admitted=%lu rejected=%lu gate_discarded_rows=%lu\n",
              flow_gate.open() ? 1 : 0, (unsigned long)gate_admitted.load(), (unsigned long)gate_rejected.load(),
              (unsigned long)(engine ? engine->gate_discarded_rows() : 0));
      }
    }
  });
}
extern "C" void nr_isac_stop(void)
{
  gate_watchdog_run.store(false);
  if (gate_watchdog.joinable()) gate_watchdog.join();
  if (engine && started.exchange(false)) engine->stop();
}
```
Until Task 10, add above it: `static void nr_isac_record_gate_close(void) {}`.

In `nr_isac_stub.c` add:
```c
void nr_isac_flow_note(uint16_t rnti, int uplink) { (void)rnti; (void)uplink; }
int nr_isac_flow_admit(uint16_t rnti) { (void)rnti; return 0; }
void nr_isac_request_discard(void) {}
int nr_isac_drained(void) { return 1; }
```

- [ ] **Step 8: Build both configurations**

Run: `cd $BON && ninja -j8 nr-uesoftmodem test_nr_isac_flow_gate && ./test_nr_isac_flow_gate && cd $BOFF && ninja -j8 nr-uesoftmodem`
Expected: both build; the test prints PASS.

- [ ] **Step 9: Commit**

```bash
cd $W && git add -A openair1/PHY/NR_UE_ISAC CMakeLists.txt && git commit -q -m "sensing: DL+UL flow gate, partial-CPI discard on close, engine threads off the PHY's FIFO class

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Producers call the gate (dedicated grants only)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (DL DM-RS submit ~5846-5895,
  data submit ~6024)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (data submit ~916)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` (DM-RS ~970, data ~1298)

**Interfaces:**
- Consumes: `nr_isac_flow_note`, `nr_isac_flow_admit` (Task 4).
- Produces: every `NR_ISAC_SRC_PDSCH_DMRS_BLIND` / `PDSCH_DATA` / `PUSCH_DMRS` / `PUSCH_DATA`
  submission happens only for `rnti_class == NR_BLIND_RNTI_CLASS_C` (DL) or a UL grant, of an
  admitted RNTI.

- [ ] **Step 1: Enumerate every submit site**

Run: `cd $W && grep -n 'NR_ISAC_SRC_PDSCH_DMRS_BLIND\|NR_ISAC_SRC_PDSCH_DATA\|NR_ISAC_SRC_PUSCH_DMRS\|NR_ISAC_SRC_PUSCH_DATA\|nr_isac_p[du]sch_data_aided_submit(' openair1/PHY/NR_UE_TRANSPORT/*.c openair1/SCHED_NR_UE/*.c`
Record the list; every site in `NR_UE_TRANSPORT` gets the pattern below. The
`phy_procedures_nr_ue.c` attached-UE site is not used by `--passive-rx` and stays unchanged.

- [ ] **Step 2: DL DM-RS producer** (`nr_pdcch_blind_monitor_rt.c`, the block that ends in
  `nr_isac_submit_cfr_multi(abs_slot, 0.0f, NR_ISAC_SRC_PDSCH_DMRS_BLIND, ...)`). Wrap the submit:

```c
          if (out.rnti_class == NR_BLIND_RNTI_CLASS_C) {
            nr_isac_flow_note(out.rnti, 0);
            if (nr_isac_flow_admit(out.rnti)) {
              nr_isac_submit_cfr_multi(abs_slot, 0.0f, NR_ISAC_SRC_PDSCH_DMRS_BLIND, &carrier, isac_h, nof_ant,
                                       273 * NR_NB_SC_PER_RB, isac_k, isac_l, nof_re, (float)nvar);
              g_cfr_submits++;
            }
          }
```

- [ ] **Step 3: UL DM-RS producer** (`nr_pusch_passive_decode.c`). Before the
  `nr_isac_submit_cfr_multi_session(ul_slot_idx, ..., NR_ISAC_SRC_PUSCH_DMRS, ...)` from Task 3:

```c
        nr_isac_flow_note(g->rnti, 1);
        if (nr_isac_flow_admit(g->rnti)) {
          nr_isac_submit_cfr_multi_session(ul_slot_idx, 0.0f,
                                           NR_ISAC_SRC_PUSCH_DMRS, &carrier, ul_h, nof_ant_cfr, cap,
                                           ul_k, ul_l, nof_re, 1.0f, (uint64_t)g->rnti);
        }
```
Keep the existing counters exactly where they are (they measure extraction, not admission).

- [ ] **Step 4: Data producers.** At the top of `nr_isac_pdsch_data_aided_submit` (after the
  `nr_isac_enabled` check) insert `if (!nr_isac_flow_admit(rnti)) return;`. At the top of
  `nr_isac_pusch_data_aided_submit` insert `if (!nr_isac_flow_admit(g->rnti)) return;`. Data paths
  never call `flow_note`: a flow is defined by the DM-RS extraction, which is CRC-independent.
  In `nr_pdsch_passive_queue.c`, the data submit additionally requires
  `job.rnti_class == NR_BLIND_RNTI_CLASS_C`.

- [ ] **Step 5: Build both, then commit**

Run: `cd $BON && ninja -j8 nr-uesoftmodem && cd $BOFF && ninja -j8 nr-uesoftmodem`
Expected: both build.
```bash
cd $W && git add -A openair1/PHY/NR_UE_TRANSPORT && git commit -q -m "passive rx: CFR producers submit only admitted DL+UL flows (C-RNTI grants)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: LLR confidence calibration module

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_llr_confidence.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_llr_confidence.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_llr_confidence_test.c`
- Modify: `CMakeLists.txt` (add the .c to the `PHY_NR_UE` source list next to
  `nr_pdsch_data_aided.c`; register the test)

**Interfaces:**
- Produces:
  - `void nr_llrconf_observe(uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);`
    for CRC-OK grants. `truth_bits` = one byte per coded bit, before scrambling; `llr` =
    descrambled LLRs.
  - `int nr_llrconf_threshold(uint8_t qm, float *tau_rel);` returns 1 when calibrated.
  - `uint32_t nr_llrconf_hard(uint8_t qm, const int16_t *llr, uint32_t G, float tau_rel, uint8_t *bits, uint8_t *keep);`
    `bits` = G bytes, `keep` = G/qm bytes; returns the kept count.
  - `void nr_llrconf_agreement(uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);`
  - `int nr_llrconf_disabled(void);` returns 1 once a sign/packing mismatch is detected.
  - `void nr_llrconf_stats_dump(void);`
  - `void nr_llrconf_reset(void);`

- [ ] **Step 1: Write the failing test**

```c
// openair1/PHY/NR_UE_TRANSPORT/tests/nr_llr_confidence_test.c
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../nr_llr_confidence.h"

static double gauss(void) { double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
  return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }
/* bit b -> LLR mean +A for b=0, -A for b=1 (OAI: positive LLR = bit 0) */
static void make(uint32_t G, double A, double sigma, uint8_t *truth, int16_t *llr) {
  for (uint32_t i = 0; i < G; i++) { truth[i] = rand() & 1; double v = (truth[i] ? -A : A) + sigma * gauss();
    if (v > 32767) v = 32767; if (v < -32767) v = -32767; llr[i] = (int16_t)v; } }

int main(void) {
  enum { G = 40000 }; static uint8_t truth[G], bits[G], keep[G / 2]; static int16_t llr[G];
  srand(1); nr_llrconf_reset();
  float tau;
  assert(nr_llrconf_threshold(2, &tau) == 0);                 /* uncalibrated */
  for (int k = 0; k < 6; k++) { make(G, 100.0, 60.0, truth, llr); nr_llrconf_observe(2, llr, truth, G); }
  assert(nr_llrconf_threshold(2, &tau) == 1 && tau > 0.0f);   /* 120000 symbols observed */
  assert(nr_llrconf_threshold(4, &tau) == 0);                 /* other Qm stays uncalibrated */
  nr_llrconf_threshold(2, &tau);
  make(G, 100.0, 60.0, truth, llr);
  uint32_t kept = nr_llrconf_hard(2, llr, G, tau, bits, keep), wrong = 0;
  for (uint32_t m = 0; m < G / 2; m++)
    if (keep[m] && (bits[2 * m] != truth[2 * m] || bits[2 * m + 1] != truth[2 * m + 1])) wrong++;
  assert(kept > G / 8);                                       /* the gate keeps a real fraction */
  assert((double)wrong / kept < 0.015);                       /* and meets the 1 % target (+slack) */
  assert(nr_llrconf_hard(2, llr, G, 0.0f, bits, keep) == G / 2); /* tau 0 keeps everything */
  /* sign-convention guard: inverted LLRs must disable the masked path */
  for (uint32_t i = 0; i < G; i++) llr[i] = (int16_t)-llr[i];
  for (int k = 0; k < 2; k++) nr_llrconf_agreement(2, llr, truth, G);
  assert(nr_llrconf_disabled() == 1);
  puts("nr_llr_confidence_test: PASS");
  return 0;
}
```
Register in `CMakeLists.txt` (inside the `if(ENABLE_TESTS)` block, next to
`test_nr_pdcch_blind_rnti_bootstrap`):
```cmake
  add_executable(test_nr_llr_confidence ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_llr_confidence_test.c
                                        ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_llr_confidence.c)
  target_link_libraries(test_nr_llr_confidence PRIVATE m)
  add_dependencies(tests test_nr_llr_confidence)
  add_test(NAME test_nr_llr_confidence COMMAND ./test_nr_llr_confidence)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd $BOFF && cmake . >/dev/null && ninja test_nr_llr_confidence`
Expected: FAIL, `nr_llr_confidence.h: No such file or directory`.

- [ ] **Step 3: Implement**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_llr_confidence.h
#ifndef NR_LLR_CONFIDENCE_H
#define NR_LLR_CONFIDENCE_H
#include <stdint.h>
/* Decision-directed X-hat for CRC-failed grants (spec §6). The keep threshold is LEARNED from
 * CRC-OK grants: per Qm, the smallest min|LLR|/median|LLR| at which kept hard decisions are wrong
 * < NR_LLRCONF_TARGET_ERR of the time. Scale-free, because OAI renormalises LLRs per TB. */
#define NR_LLRCONF_BINS 80
#define NR_LLRCONF_BIN_W 0.05f
#define NR_LLRCONF_TARGET_ERR 0.01
#define NR_LLRCONF_MIN_SYMBOLS 100000ULL
void nr_llrconf_observe(uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);
int nr_llrconf_threshold(uint8_t qm, float *tau_rel);
uint32_t nr_llrconf_hard(uint8_t qm, const int16_t *llr, uint32_t G, float tau_rel, uint8_t *bits, uint8_t *keep);
void nr_llrconf_agreement(uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);
int nr_llrconf_disabled(void);
void nr_llrconf_stats_dump(void);
void nr_llrconf_reset(void);
#endif
```

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_llr_confidence.c
#include "nr_llr_confidence.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t g_right[4][NR_LLRCONF_BINS], g_wrong[4][NR_LLRCONF_BINS];
static _Atomic uint64_t g_agree[4], g_disagree[4];
static _Atomic int g_disabled;
static _Atomic uint64_t g_kept_sym, g_total_sym; /* masked-path REs kept vs offered (spec §6 counters) */

static int qidx(uint8_t qm) { return (qm == 2 || qm == 4 || qm == 6 || qm == 8) ? qm / 2 - 1 : -1; }

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

/* Median |LLR| of the grant: the per-TB scale every ratio below is expressed in. */
static float median_abs(const int16_t *llr, uint32_t G) {
  static __thread uint16_t *buf = NULL; static __thread uint32_t cap = 0;
  if (cap < G) { free(buf); buf = malloc(sizeof(*buf) * G); cap = buf ? G : 0; if (!buf) return 0.0f; }
  for (uint32_t i = 0; i < G; i++) buf[i] = (uint16_t)(llr[i] < 0 ? -(int)llr[i] : llr[i]);
  qsort(buf, G, sizeof(*buf), cmp_u16);  /* ponytail: O(G log G); quickselect if it shows in profiles */
  return (float)buf[G / 2];
}

static int bin_of(float r) { int b = (int)(r / NR_LLRCONF_BIN_W); return b < 0 ? 0 : (b >= NR_LLRCONF_BINS ? NR_LLRCONF_BINS - 1 : b); }

void nr_llrconf_observe(uint8_t qm, const int16_t *llr, const uint8_t *truth, uint32_t G) {
  const int q = qidx(qm); if (q < 0 || !llr || !truth || G < qm) return;
  const float med = median_abs(llr, G); if (med <= 0.0f) return;
  for (uint32_t m = 0; m < G / qm; m++) {
    int ok = 1; int mn = 32767;
    for (int b = 0; b < qm; b++) { const int16_t v = llr[m * qm + b]; const int a = v < 0 ? -v : v;
      if (a < mn) mn = a; if ((uint8_t)(v < 0) != (truth[m * qm + b] & 1)) ok = 0; }
    atomic_fetch_add_explicit(ok ? &g_right[q][bin_of(mn / med)] : &g_wrong[q][bin_of(mn / med)], 1, memory_order_relaxed);
  }
}

int nr_llrconf_threshold(uint8_t qm, float *tau) {
  const int q = qidx(qm); if (q < 0 || atomic_load(&g_disabled)) return 0;
  uint64_t tot = 0, wr = 0;
  for (int b = 0; b < NR_LLRCONF_BINS; b++) { tot += g_right[q][b] + g_wrong[q][b]; }
  if (tot < NR_LLRCONF_MIN_SYMBOLS) return 0;
  uint64_t kept = tot;
  for (int b = 0; b < NR_LLRCONF_BINS; b++) wr += g_wrong[q][b];
  for (int b = 0; b < NR_LLRCONF_BINS; b++) {
    if (kept > 0 && (double)wr / (double)kept < NR_LLRCONF_TARGET_ERR) { *tau = b * NR_LLRCONF_BIN_W; return 1; }
    kept -= g_right[q][b] + g_wrong[q][b]; wr -= g_wrong[q][b];
  }
  return 0;
}

uint32_t nr_llrconf_hard(uint8_t qm, const int16_t *llr, uint32_t G, float tau, uint8_t *bits, uint8_t *keep) {
  if (qidx(qm) < 0 || !llr || G < qm) return 0;
  const float med = median_abs(llr, G); uint32_t kept = 0;
  for (uint32_t m = 0; m < G / qm; m++) {
    int mn = 32767;
    for (int b = 0; b < qm; b++) { const int16_t v = llr[m * qm + b]; const int a = v < 0 ? -v : v;
      if (a < mn) mn = a; bits[m * qm + b] = (uint8_t)(v < 0); }
    keep[m] = (uint8_t)(med > 0.0f && (float)mn / med >= tau);
    kept += keep[m];
  }
  atomic_fetch_add_explicit(&g_kept_sym, kept, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_total_sym, G / qm, memory_order_relaxed);
  return kept;
}

void nr_llrconf_agreement(uint8_t qm, const int16_t *llr, const uint8_t *truth, uint32_t G) {
  const int q = qidx(qm); if (q < 0 || !llr || !truth) return;
  uint64_t a = 0, d = 0;
  for (uint32_t i = 0; i < G; i++) ((uint8_t)(llr[i] < 0) == (truth[i] & 1)) ? a++ : d++;
  const uint64_t A = atomic_fetch_add(&g_agree[q], a) + a, D = atomic_fetch_add(&g_disagree[q], d) + d;
  /* Raw bit agreement must be far above 50 % on CRC-OK grants; below it, sign or packing is wrong. */
  if (A + D >= 10000 && A < D && !atomic_exchange(&g_disabled, 1))
    fprintf(stderr, "SENSING: LLRCONF DISABLED -- CRC-OK hard decisions disagree with the re-encoded bits "
            "(agree=%llu disagree=%llu): LLR sign convention or bit packing mismatch\n",
            (unsigned long long)A, (unsigned long long)D);
}

int nr_llrconf_disabled(void) { return atomic_load(&g_disabled); }

void nr_llrconf_stats_dump(void) {
  for (int q = 0; q < 4; q++) { float tau = -1.0f; const int cal = nr_llrconf_threshold((uint8_t)(2 * q + 2), &tau);
    const uint64_t A = g_agree[q], D = g_disagree[q];
    fprintf(stderr, "SENSING: LLRCONF qm=%d calibrated=%d tau_rel=%.2f crc_ok_bit_agreement=%.4f disabled=%d\n",
            2 * q + 2, cal, tau, (A + D) ? (double)A / (double)(A + D) : 0.0, (int)g_disabled); }
  fprintf(stderr, "SENSING: LLRCONF masked_re kept=%llu offered=%llu\n",
          (unsigned long long)g_kept_sym, (unsigned long long)g_total_sym);
}

void nr_llrconf_reset(void) {
  memset((void *)g_right, 0, sizeof g_right); memset((void *)g_wrong, 0, sizeof g_wrong);
  memset((void *)g_agree, 0, sizeof g_agree); memset((void *)g_disagree, 0, sizeof g_disagree); g_disabled = 0;
  g_kept_sym = 0; g_total_sym = 0;
}
```
Also add `${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_llr_confidence.c` to the `PHY_NR_UE` source list
(the list that contains `nr_pdsch_data_aided.c`).

- [ ] **Step 4: Run the test**

Run: `cd $BOFF && ninja test_nr_llr_confidence && ./test_nr_llr_confidence`
Expected: `nr_llr_confidence_test: PASS`

- [ ] **Step 5: Commit**

```bash
cd $W && git add -A openair1/PHY/NR_UE_TRANSPORT CMakeLists.txt && git commit -q -m "passive rx: learned LLR-confidence threshold for decision-directed CFR (spec §6)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: DL masked data path + calibration feed

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.{h,c}`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (~916), `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (~6024)
- Modify: `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:2320` (pass `NULL, 0` for the two new args)

**Interfaces:**
- Consumes: Task 6 API; `nr_pdsch_passive_last_llr(const int16_t **p)`, which returns G and points
  at the thread's last DESCRAMBLED LLRs.
- Produces: `nr_isac_pdsch_data_aided_submit(ue, proc, cw, dlsch_config, freq_alloc, rnti,
  tb_bytes, llr, llr_G, harq_pid_tag, rxdataF, nvar)`.
  - `tb_bytes != NULL`: re-encode, and calibrate if `llr` is given.
  - `tb_bytes == NULL && llr != NULL`: masked decision-directed path.

- [ ] **Step 1: Signature.** In `nr_pdsch_data_aided.h`, add
  `const int16_t *llr, uint32_t llr_G,` after `const uint8_t *tb_bytes,`, and document:
  "descrambled LLRs of this grant (nr_pdsch_passive_last_llr), or NULL". Update the definition to
  match.

- [ ] **Step 2: Coded-bit source.** In `nr_pdsch_data_aided.c`, replace the block from
  `// --- TB CRC-included payload` through the LDPC `nrLDPC_coding_encoder(...)` failure check with
  the following. It computes G first, then fills `coded_bits` either by re-encoding or from hard
  decisions:

```c
  TB_parameters.harq_unique_pid = harq_pid_tag;
  TB_parameters.BG = cw->ldpcBaseGraph;
  TB_parameters.nb_rb = freq_alloc->num_rbs;
  TB_parameters.Qm = cw->qamModOrder;
  TB_parameters.mcs = cw->mcs;
  TB_parameters.nb_layers = cw->Nl;
  TB_parameters.rv_index = cw->rv;
  TB_parameters.tbslbrm = dlsch_config->tbslbrm;
  const uint8_t  nb_re_dmrs = get_num_dmrs_re_per_rb(dlsch_config->dmrsConfigType, dlsch_config->n_dmrs_cdm_groups);
  const uint16_t dmrs_len   = get_num_dmrs(dlsch_config->dlDmrsSymbPos);
  TB_parameters.G = nr_get_G(freq_alloc->num_rbs, dlsch_config->number_symbols, nb_re_dmrs, dmrs_len,
                             0, cw->qamModOrder, cw->Nl);
  if (TB_parameters.G == 0)
    return;

  static __thread uint8_t coded_bits[(273 * 12 * 14 * 8 + 63) / 64 * 64 + 64] __attribute__((aligned(32)));
  static __thread uint8_t keep[273 * 12 * 14];
  memset(coded_bits, 0, sizeof(coded_bits));
  const bool masked = (tb_bytes == NULL);
  if (masked) {
    float tau = 0.0f;
    if (llr == NULL || llr_G != TB_parameters.G || !nr_llrconf_threshold(cw->qamModOrder, &tau))
      return; /* no calibration for this Qm yet: DM-RS only (spec §6) */
    nr_llrconf_hard(cw->qamModOrder, llr, TB_parameters.G, tau, coded_bits, keep);
  } else {
    const uint32_t A = cw->TBS;
    const unsigned int B = A + (A > NR_MAX_PDSCH_TBS ? 24 : 16);
    static __thread uint8_t seg_storage[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER][8448];
    static __thread uint8_t *c_segs[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER];
    for (int r = 0; r < MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER; r++)
      c_segs[r] = seg_storage[r];
    TB_parameters.A = A;
    TB_parameters.Kb = nr_segmentation((unsigned char *)tb_bytes, c_segs, B, &TB_parameters.C, &TB_parameters.K,
                                       &TB_parameters.Z, &TB_parameters.F, TB_parameters.BG);
    if (TB_parameters.C > MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER)
      return;
    TB_parameters.output = coded_bits;
    static __thread nrLDPC_segment_encoding_parameters_t segments[MAX_NUM_NR_DLSCH_SEGMENTS_PER_LAYER];
    memset(segments, 0, sizeof(segments));
    TB_parameters.segments = segments;
    for (uint32_t r = 0; r < TB_parameters.C; r++) {
      segments[r].c = c_segs[r];
      segments[r].E = nr_get_E(TB_parameters.G, TB_parameters.C, TB_parameters.Qm, TB_parameters.nb_layers, r);
      reset_meas(&segments[r].ts_interleave);
      reset_meas(&segments[r].ts_rate_match);
      reset_meas(&segments[r].ts_ldpc_encode);
    }
    nrLDPC_slot_encoding_parameters_t slot_parameters = {.frame = proc->frame_rx, .slot = proc->nr_slot_rx, .nb_TBs = 1,
                                                         .threadPool = &get_nrUE_params()->Tpool, .TBs = &TB_parameters};
    if (ue->nrLDPC_coding_interface.nrLDPC_coding_encoder(&slot_parameters) != 0)
      return;
    if (llr != NULL && llr_G == TB_parameters.G) { /* CRC-OK grant: this IS the calibration ground truth */
      nr_llrconf_observe(cw->qamModOrder, llr, coded_bits, TB_parameters.G);
      nr_llrconf_agreement(cw->qamModOrder, llr, coded_bits, TB_parameters.G);
    }
  }
```
Keep the existing `Nl != 1` guard above this block, and the scramble/modulate/RE-mapping code
below it, unchanged. Add `#include "nr_llr_confidence.h"` at the top.

- [ ] **Step 3: Honour the mask in the RE loop.** Replace
  `const c16_t x = mod_syms[mod_idx++];` with:

```c
      const uint32_t m = mod_idx;
      const c16_t x = mod_syms[mod_idx++];
      if (masked && !keep[m])
        continue; /* consumed (keeps the mapping aligned), not measured: low-confidence decision */
```
The `mod_idx != expected_syms` invariant stays exactly as it is. It counts consumed symbols, and
masked symbols are consumed.

- [ ] **Step 4: Callers.**
  - **CRC OK** (`nr_pdsch_passive_queue.c` ~916 and `nr_pdcch_blind_monitor_rt.c` ~6024): obtain
    the LLRs right after the decode returns, in the same thread and before any other decode on it:

```c
          const int16_t *da_llr = NULL;
          const uint32_t da_G = nr_pdsch_passive_last_llr(&da_llr);
```
    Pass `dec.tb, da_llr, da_G` in place of `dec.tb`.
  - **CRC FAIL:** in the same `if`/`else` chain, add an `else if` for the failure status. In the
    queue:
```c
      } else if (st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL && job.want_data && !job.layout_probe
                 && job.rnti_class == NR_BLIND_RNTI_CLASS_C) {
        const int16_t *da_llr = NULL;
        const uint32_t da_G = nr_pdsch_passive_last_llr(&da_llr);
        nr_isac_abs_slot_override = (uint64_t)job.absolute_slot;
        nr_isac_pdsch_data_aided_submit(ue, &proc, &dec.cw, &job.dlsch_pdu, &job.freq_alloc, job.rnti,
                                        NULL, da_llr, da_G, job.harq_pid_tag, rxdataF, (double)dec.nvar);
        nr_isac_abs_slot_override = 0;
      }
```
    In `rt.c`, the same with `out.rnti`, `out.rnti_class == NR_BLIND_RNTI_CLASS_C`, `&dlsch_pdu`,
    `&freq_alloc`, `rxdataF_pdsch` and `want_data`, mirroring its CRC-OK call.
  - **Before editing:** read `nr_pdsch_passive_decode.c` around line 2843 to confirm that `llr` is
    still alive (not freed or reused) when the caller submits. `nr_pdsch_passive_queue.c:716`
    already reads it at that point, so it is. If the decode path returns early before line 2843 on
    failure (`t_last_llr` stale from a previous grant), guard with `da_G == dec.G`.
  - `phy_procedures_nr_ue.c:2320`: pass `NULL, 0` after the TB argument.

- [ ] **Step 5: Stats.** Next to the existing `pdsch_decode[try=...]` periodic LOG, call
  `nr_llrconf_stats_dump();` at the same cadence. Find it with
  `grep -n 'pdsch_decode\[try' openair1/PHY/NR_UE_TRANSPORT/*.c`.

- [ ] **Step 6: Build both, then commit**

Run: `cd $BON && ninja -j8 nr-uesoftmodem && cd $BOFF && ninja -j8 nr-uesoftmodem test_nr_llr_confidence && ./test_nr_llr_confidence`
Expected: builds pass; the test prints PASS.
```bash
cd $W && git add -A openair1 && git commit -q -m "passive rx: DL data REs of CRC-failed grants via learned-confidence hard decisions

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: UL masked data path + calibration feed

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_data_aided.{h,c}`, `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c`

**Interfaces:**
- Produces: `nr_isac_pusch_data_aided_submit(ue, gnb, pdu, g, tb_bytes, llr, llr_G, harq_pid_tag, ul_slot_idx, nof_ant, slot)`,
  with the same two modes as DL.
  - `llr` = descrambled LLRs over ALL of the grant's REs (data + UCI), captured right after
    `nr_rx_pusch_group_tp()`.
  - `llr_G` = full G.

- [ ] **Step 1: Capture the full LLRs once per grant.** In `nr_pusch_passive_decode.c`, right
  after the (last) `nr_rx_pusch_group_tp(gnb, &pvp, &pdup, &unavp, 1, frame, slot);` that precedes
  the decode, and once `out->G` is known (line ~990), add:

```c
  static __thread int16_t *ul_llr_full = NULL;
  static __thread uint32_t ul_llr_cap = 0;
  const uint32_t ul_G_full = out->G;
  if (ul_llr_cap < ul_G_full) {
    free(ul_llr_full);
    ul_llr_full = malloc16((size_t)ul_G_full * sizeof(int16_t));
    ul_llr_cap = ul_llr_full ? ul_G_full : 0;
  }
  if (ul_llr_full)
    memcpy(ul_llr_full, pvp->llr, (size_t)ul_G_full * sizeof(int16_t));
```
It must come AFTER the point where `out->G` is assigned and BEFORE any
`nr_passive_uci_probe_demux` rewrites `pvp->llr`. Read lines 985-1100 and place it accordingly.

- [ ] **Step 2: Submit paths.** `nr_pusch_passive_decode_inner()` returns `false` early on a
  failed CRC, so the CRC-OK submit at ~1297 is never reached for failures. Define once, near the
  top of the function (after `nant` is known):

```c
#define UL_MASKED_SUBMIT()                                                                          \
  do {                                                                                              \
    if (ul_llr_full)                                                                                \
      nr_isac_pusch_data_aided_submit(ue, gnb, &pdu, g, NULL, ul_llr_full, ul_G_full,               \
                                      NR_PUSCH_PASSIVE_DA_TAG_BASE + (uint32_t)ctx,                 \
                                      passive_ul_slow_time_idx(fp, frame, slot, abs_slot),          \
                                      (uint32_t)nant, slot);                                        \
  } while (0)
```
Then:
- In the `if (rc != 0 || hp->b == NULL) {` branch (~1214) and in the `if (hp_crc_failed(ulsch)) {`
  branch (~1224), call `UL_MASKED_SUBMIT();` immediately before each `return false;`. The LLRs
  are valid in both: they come from the demodulator, not the decoder.
- The all-zero-payload guard branch submits nothing (it may be DTX).
- Replace the block at ~1297 with:
```c
  if (out->uci_ack_re == 0) {
    nr_isac_pusch_data_aided_submit(ue, gnb, &pdu, g, hp->b, ul_llr_full, ul_G_full,
                                    NR_PUSCH_PASSIVE_DA_TAG_BASE + (uint32_t)ctx,
                                    passive_ul_slow_time_idx(fp, frame, slot, abs_slot),
                                    (uint32_t)nant, slot);
  } else {
    /* UCI multiplexed: re-encoding the UL-SCH alone cannot place UCI REs; decision-directed covers every
     * RE, because descramble-then-rescramble with the same c(i) returns each transmitted bit's decision. */
    UL_MASKED_SUBMIT();
  }
```
- `#undef UL_MASKED_SUBMIT` at the end of the function.
- `ul_llr_full`/`ul_G_full` (Step 1) must be declared before the first use. If Step 1's natural
  place is inside a nested block, hoist the two declarations to function scope.

- [ ] **Step 3: Inside `nr_isac_pusch_data_aided_submit`.** Apply the same split as DL Task 7
  Step 2:
  - Compute `TB.G` from the grant as today.
  - **Masked** (`tb_bytes == NULL`): require `llr && llr_G == TB.G` and a calibration
    (`nr_llrconf_threshold(Qm, &tau)`), then `nr_llrconf_hard(Qm, llr, TB.G, tau, coded_bits, keep)`
    with `static __thread uint8_t keep[UL_DA_MAX_RE];`.
  - **Re-encode:** run the existing chain. Then, if `llr && llr_G == TB.G`, call
    `nr_llrconf_observe` and `nr_llrconf_agreement`.
  - In the RE loop, `const uint32_t m = mod_idx; const c16_t xs = mod_syms[mod_idx++]; if (tb_bytes == NULL && !keep[m]) continue;`.
  - Keep the `mod_idx != expected` invariant and the `g->nrOfLayers != 1` guard unchanged.
  - Remove the `tb_bytes == NULL` early return in the NULL check at the top (keep the other NULL
    checks).

- [ ] **Step 4: Build both, then commit**

Run: `cd $BON && ninja -j8 nr-uesoftmodem && cd $BOFF && ninja -j8 nr-uesoftmodem`
Expected: both build.
```bash
cd $W && git add -A openair1 && git commit -q -m "passive rx: UL data REs of CRC-failed/UCI grants via learned-confidence hard decisions

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Per-channel CFO loop default-on + race fix

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (BRANCHFO block, ~2155-2195)

**Interfaces:**
- Produces: the integrating per-branch FO correction runs whenever `nb_antennas_rx > 1`, unless
  `ISAC_RX_BRANCH_FO=0`. `s_fo_corr[]`/`s_fo_ema[]` updates are serialised.

- [ ] **Step 1: Edit.** In the BRANCHFO block:
  - Replace the per-branch `getenv("ISAC_RX_BRANCH_FO") != NULL && atoi(...) != 0` test with a
    cached default-on flag:
    ```c
    static int s_brfo = -1;
    if (s_brfo < 0) { const char *e = getenv("ISAC_RX_BRANCH_FO"); s_brfo = (e == NULL) ? 1 : (atoi(e) != 0); }
    ```
  - Move `static double s_fo_corr[NR_DL_CHEST_MAX_ANT];` to the block scope next to `s_fo_ema`.
  - Guard the whole EMA + integrate + `nr_ue_set_branch_fo_hz` update with
    `static pthread_mutex_t s_fo_lock = PTHREAD_MUTEX_INITIALIZER;` (`pthread_mutex_lock` before
    the `for (a ...)` update loop, unlock after it).

- [ ] **Step 2: Build both, then commit**

Run: `cd $BON && ninja -j8 nr-uesoftmodem && cd $BOFF && ninja -j8 nr-uesoftmodem`
```bash
cd $W && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c && git commit -q -m "passive rx: per-branch FO loop on by default at >1 RX, serialise its shared state

Several decode workers updated the static integrator concurrently. ISAC_RX_BRANCH_FO=0 disables.
Not yet validated on air (spec §10): pass = BRANCHFO d_vs_br0 |.| < 20 Hz on channels 1-3.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: CFR recorder, `isac_replay`, synthetic multistatic rows (deterministic offline test bed)

**Files:**
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac.cc` (recorder at the ABI; real `nr_isac_record_gate_close`)
- Create: `openair1/PHY/NR_UE_ISAC/tools/isac_replay.cc`
- Create: `openair1/PHY/NR_UE_ISAC/tools/make_synthetic_rows.py`
- Create: `openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh`
- Create: `tests/passive_rx/ota/sensing_synth.conf` (the `sensing` block the replay loads)
- Modify: `CMakeLists.txt` (the `isac_replay` target)

**Interfaces:**
- **`cfr_rows.bin` record** (little endian, repeated). Header:
  `char magic[4]="CFR1"; uint32 kind (0 row, 1 gate_close); uint32 slot; float frac; int32 source;
  uint32 nof_prb; uint32 scs_hz; uint64 dl_center_hz; uint16 pci; uint16 slots_per_frame;
  uint32 antennas; uint32 re; float noise; uint64 session; uint64 mono_ns`.
  - For kind 0, the header is followed by `float h[2*antennas*re]` (antenna-major), then
    `uint32 k[re]`, then `uint32 l[re]`.
  - For kind 1, `antennas = re = 0` and nothing follows.
- **Recording** is enabled when `NR_ISAC_DEBUG_DIR` is set; the file is
  `$NR_ISAC_DEBUG_DIR/cfr_rows.bin`.
- **`isac_replay -O <conf> --rows <cfr_rows.bin>`**: loads the conf, runs the same engine, submits
  every record, waits `nr_isac_drained()`, then stops.

- [ ] **Step 1: Recorder in `nr_isac.cc`.** Add:

```cpp
namespace {
std::mutex rec_mutex; FILE* rec_file=nullptr; bool rec_checked=false;
FILE* recorder(){ // caller holds rec_mutex
  if(!rec_checked){rec_checked=true;const char* d=std::getenv("NR_ISAC_DEBUG_DIR");
    if(d&&*d){std::string p=std::string(d)+"/cfr_rows.bin";rec_file=std::fopen(p.c_str(),"wb");
      if(!rec_file)LOG_E(PHY,"SENSING: cannot open %s for recording\n",p.c_str());}}
  return rec_file;}
uint64_t mono_ns(){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;}
void rec_header(FILE* f,uint32_t kind,uint32_t slot,float frac,int32_t source,const nr_isac_carrier_t* c,
                uint32_t ant,uint32_t re,float noise,uint64_t session){
  const uint32_t prb=c?c->nof_prb:0,scs=c?c->scs_hz:0;const uint64_t fc=c?c->dl_center_hz:0;
  const uint16_t pci=c?c->pci:0,spf=c?c->slots_per_frame:0;const uint64_t t=mono_ns();
  std::fwrite("CFR1",1,4,f);std::fwrite(&kind,4,1,f);std::fwrite(&slot,4,1,f);std::fwrite(&frac,4,1,f);
  std::fwrite(&source,4,1,f);std::fwrite(&prb,4,1,f);std::fwrite(&scs,4,1,f);std::fwrite(&fc,8,1,f);
  std::fwrite(&pci,2,1,f);std::fwrite(&spf,2,1,f);std::fwrite(&ant,4,1,f);std::fwrite(&re,4,1,f);
  std::fwrite(&noise,4,1,f);std::fwrite(&session,8,1,f);std::fwrite(&t,8,1,f);}
}
static void nr_isac_record_gate_close(void)
{std::lock_guard<std::mutex> l(rec_mutex);if(FILE* f=recorder()){rec_header(f,1,0,0.f,-1,nullptr,0,0,0.f,0);std::fflush(f);}}
```
Remove Task 4's empty placeholder of `nr_isac_record_gate_close`. In
`nr_isac_submit_cfr_multi_session`, immediately before `engine->submit(...)` (after all validation,
and using the ORIGINAL interleaved `h` with its `stride`):
```cpp
  {std::lock_guard<std::mutex> l(rec_mutex);if(FILE* f=recorder()){
    rec_header(f,0,slot,fraction,source,carrier,antennas,n,noise,session_id);
    for(uint32_t a=0;a<antennas;++a)std::fwrite(h+(size_t)2*a*stride,sizeof(float),(size_t)2*n,f);
    std::fwrite(k,4,n,f);std::fwrite(l,4,n,f);}}
```
The flush happens at `nr_isac_stop()`: add
`{std::lock_guard<std::mutex> l(rec_mutex);if(rec_file){std::fclose(rec_file);rec_file=nullptr;}}`
after the engine stop.

- [ ] **Step 2: `isac_replay.cc`**

```cpp
// openair1/PHY/NR_UE_ISAC/tools/isac_replay.cc
/* Offline replay of a recorded cfr_rows.bin through the SAME engine and conf as the live receiver.
 * Built with NR_ISAC_FIXED_WORK_REPLAY so admission blocks instead of dropping. */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "common/config/config_load_configmodule.h"
#include "common/utils/LOG/log.h"
#include "nr_isac.h"
configmodule_interface_t *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int a)
{ std::fprintf(stderr, "exit_function %s:%d %s: %s\n", file, line, function, s ? s : ""); std::exit(a ? 1 : 0); }
}
int main(int argc, char **argv)
{
  std::string rows; std::vector<char *> args{argv[0]};
  for (int i = 1; i < argc; ++i) { if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) rows = argv[++i]; else args.push_back(argv[i]); }
  if (rows.empty()) { std::fprintf(stderr, "usage: isac_replay -O <ue.conf> --rows <cfr_rows.bin>\n"); return 2; }
  if (!(uniqCfg = load_configmodule((int)args.size(), args.data(), 0))) { std::fprintf(stderr, "config load failed\n"); return 2; }
  logInit();
  nr_isac_init(); nr_isac_start();
  if (!nr_isac_enabled()) { std::fprintf(stderr, "sensing not enabled by the conf\n"); return 3; }
  FILE *f = std::fopen(rows.c_str(), "rb"); if (!f) { std::perror(rows.c_str()); return 2; }
  std::vector<float> h; std::vector<uint32_t> k, l; uint64_t n_rows = 0, n_close = 0;
  for (;;) {
    char magic[4]; uint32_t kind, slot, prb, scs, ant, re; float frac, noise; int32_t source; uint64_t fc, session, t; uint16_t pci, spf;
    if (std::fread(magic, 1, 4, f) != 4) break;
    if (std::memcmp(magic, "CFR1", 4)) { std::fprintf(stderr, "bad record magic at row %llu\n", (unsigned long long)n_rows); return 4; }
    std::fread(&kind, 4, 1, f); std::fread(&slot, 4, 1, f); std::fread(&frac, 4, 1, f); std::fread(&source, 4, 1, f);
    std::fread(&prb, 4, 1, f); std::fread(&scs, 4, 1, f); std::fread(&fc, 8, 1, f); std::fread(&pci, 2, 1, f);
    std::fread(&spf, 2, 1, f); std::fread(&ant, 4, 1, f); std::fread(&re, 4, 1, f); std::fread(&noise, 4, 1, f);
    std::fread(&session, 8, 1, f); std::fread(&t, 8, 1, f);
    if (kind == 1) { nr_isac_request_discard(); ++n_close; continue; }
    h.resize((size_t)2 * ant * re); k.resize(re); l.resize(re);
    if (std::fread(h.data(), sizeof(float), h.size(), f) != h.size() || std::fread(k.data(), 4, re, f) != re
        || std::fread(l.data(), 4, re, f) != re) { std::fprintf(stderr, "truncated record\n"); break; }
    nr_isac_carrier_t c{prb, scs, fc, pci, spf};
    nr_isac_submit_cfr_multi_session(slot, frac, source, &c, h.data(), ant, re, k.data(), l.data(), re, noise, session);
    ++n_rows;
  }
  std::fclose(f);
  while (!nr_isac_drained()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  nr_isac_stop();
  std::printf("isac_replay: rows=%llu gate_closes=%llu\n", (unsigned long long)n_rows, (unsigned long long)n_close);
  return 0;
}
```
Register it (inside the feature ISAC test block; it needs the engine sources compiled with the
replay define):
```cmake
    add_executable(isac_replay ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/tools/isac_replay.cc ${NR_UE_ISAC_SRC})
    target_compile_definitions(isac_replay PRIVATE NR_ISAC_FIXED_WORK_REPLAY
                               $<$<BOOL:${ENABLE_CHANNEL_SIM_CUDA}>:NR_ISAC_CUDA_ACCELERATION NR_ISAC_CUDA_DETECTOR>)
    target_include_directories(isac_replay PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_ISAC)
    target_link_libraries(isac_replay PRIVATE UTIL CONFIG_LIB)
    if(ENABLE_CHANNEL_SIM_CUDA)
      target_link_libraries(isac_replay PRIVATE CUDA::cufft CUDA::cudart)
      set_target_properties(isac_replay PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
    endif()
    if(ZMQ_LIB)
      target_compile_definitions(isac_replay PRIVATE ENABLE_ZEROMQ)
      target_link_libraries(isac_replay PRIVATE ${ZMQ_LIB})
    endif()
    add_dependencies(tests isac_replay)
```
If `NR_ISAC_FIXED_WORK_REPLAY` blocks on `diagnostic_clean::select` or other replay-only hooks
that need extra files, read `sensing_engine.cc`'s `#ifdef NR_ISAC_FIXED_WORK_REPLAY` sites and
add the minimal missing definitions to `isac_replay.cc`. Never change the engine's non-replay path.

- [ ] **Step 3: Synthetic multistatic rows**

```python
#!/usr/bin/env python3
# openair1/PHY/NR_UE_ISAC/tools/make_synthetic_rows.py
"""Physically consistent 4-receiver CFR rows (cfr_rows.bin) for offline tests: a direct path and one
constant-velocity point target per receiver, per-channel cable delay and phase, optional mid-run
silence with a recorded gate close. Survey geometry comes from the same survey.json the launcher uses."""
import argparse, json, struct
import numpy as np

C = 299792458.0

def header(kind, slot, frac, source, prb, scs, fc, pci, spf, ant, re, noise, session, t_ns):
    return (b"CFR1" + struct.pack("<IIfiIIQHHIIfQQ", kind, slot, frac, source, prb, scs, fc, pci, spf,
                                  ant, re, noise, session, t_ns))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--survey", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=10.0); ap.add_argument("--gap", nargs=2, type=float, default=None)
    ap.add_argument("--target", nargs=6, type=float, default=[60, 40, 1.5, 3.0, -2.0, 0.0], help="x y z vx vy vz")
    ap.add_argument("--cable-delay-ns", nargs=4, type=float, default=[0, 0, 0, 0])
    ap.add_argument("--snr-db", type=float, default=25.0); ap.add_argument("--ue-rnti", type=lambda s: int(s, 0), default=0x4601)
    a = ap.parse_args()
    s = json.load(open(a.survey)); gnb = np.array(s["gnb_m"], float)
    rx = [np.array(s["rx_antennas_m"][f"ch{i}"], float) for i in range(4)]
    prb, scs, fc, spf = 273, 30000, 3450000000, 20
    k = np.arange(0, prb * 12, 2, dtype=np.uint32)               # DM-RS comb-2, full band
    f = (k.astype(float) - prb * 6) * scs                         # baseband frequency of each RE
    rng = np.random.default_rng(7); p0 = np.array(a.target[:3]); v = np.array(a.target[3:])
    ue = gnb + np.array([30.0, -20.0, -gnb[2] + 1.5])             # static UE for the UL leg
    lam = C / fc; sigma = 10 ** (-a.snr_db / 20)
    with open(a.out, "wb") as out:
        closed = False
        for n in range(int(a.seconds / 0.0005)):
            t = n * 0.0005
            if a.gap and a.gap[0] <= t < a.gap[1]:
                if not closed and t >= a.gap[0] + 2.0:
                    out.write(header(1, 0, 0.0, -1, 0, 0, 0, 0, 0, 0, 0, 0.0, 0, int(t * 1e9))); closed = True
                continue
            closed = False
            pt = p0 + v * t
            for leg, src, session, tx, every in (("dl", 3, 0, gnb, 1), ("ul", 4, a.ue_rnti, ue, 5)):
                if n % every: continue
                h = np.empty((4, k.size), np.complex64)
                for i in range(4):
                    d_los = np.linalg.norm(rx[i] - tx); d_tgt = np.linalg.norm(pt - tx) + np.linalg.norm(rx[i] - pt)
                    tau_c = a.cable_delay_ns[i] * 1e-9
                    los = np.exp(-2j * np.pi * f * (d_los / C + tau_c))
                    tgt = 0.1 * np.exp(-2j * np.pi * f * (d_tgt / C + tau_c)) * np.exp(-2j * np.pi * d_tgt / lam)
                    h[i] = (los + tgt + sigma * (rng.standard_normal(k.size) + 1j * rng.standard_normal(k.size)) / np.sqrt(2)) \
                           * np.exp(1j * 0.7 * i)
                inter = np.empty((4, 2 * k.size), np.float32); inter[:, 0::2] = h.real; inter[:, 1::2] = h.imag
                out.write(header(0, n % (1024 * spf), 0.0, src, prb, scs, fc, 2, spf, 4, k.size, sigma ** 2, session, int(t * 1e9)))
                out.write(inter.tobytes()); out.write(k.tobytes()); out.write(np.full(k.size, 2, np.uint32).tobytes())
    print(json.dumps({"out": a.out, "target_start": list(p0), "velocity": list(v)}))

if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Offline test conf and runner.** `tests/passive_rx/ota/sensing_synth.conf` holds
  only the `sensing = { ... };` block. Its keys are produced by Task 13's
  `survey.py --apply` on the same survey, plus `enable = 1; sources =
  "pdsch_dmrs_blind,pdsch_data,pusch_dmrs,pusch_data"; capture = 1; report_path =
  "/tmp/isac_synth/reports.jsonl";`. Until Task 13 exists, write the `spatial_rx_positions` and
  `tx_pos_*` lines by hand from the survey below; Task 13 Step 5 regenerates the file. Test survey
  (`openair1/PHY/NR_UE_ISAC/tools/survey_synth.json`):

```json
{"frame": "ENU metres, origin at the X410",
 "gnb_m": [120.0, 80.0, 25.0],
 "rx_antennas_m": {"ch0": [0.0, 0.0, 2.0], "ch1": [40.0, 0.0, 2.0], "ch2": [0.0, 40.0, 2.0], "ch3": [40.0, 40.0, 6.0]},
 "max_range_m": 600}
```

`openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh`:
```bash
#!/bin/bash
# Deterministic end-to-end offline test: synthetic rows -> isac_replay -> reports.jsonl.
set -euo pipefail
BUILD=${BUILD:?set BUILD to the sensing build dir}; T=$(cd "$(dirname "$0")" && pwd); O=/tmp/isac_synth
rm -f $O/reports.jsonl; mkdir -p $O
python3 $T/make_synthetic_rows.py --survey $T/survey_synth.json --out $O/rows.bin --seconds 12 --gap 5 8 \
  --cable-delay-ns 0 35 80 120 > $O/truth.json
(cd $BUILD && ./isac_replay -O $T/../../../../tests/passive_rx/ota/sensing_synth.conf --rows $O/rows.bin)
python3 - "$O" "$T/survey_synth.json" <<'EOF'
import json, sys, numpy as np
O, survey = sys.argv[1], json.load(open(sys.argv[2]))
reps = [json.loads(l) for l in open(f"{O}/reports.jsonl") if l.strip()]
assert len(reps) >= 20, f"too few CPIs: {len(reps)}"
tr = json.load(open(f"{O}/truth.json")); p0, v = np.array(tr["target_start"]), np.array(tr["velocity"])
gnb = np.array(survey["gnb_m"]); ok = 0; n = 0
for r in reps:
    t = r["midpoint_air_time_s"]; pt = p0 + v * t
    if 5.0 <= t < 8.0:                     # gate gap: no CPI may be built from rows inside it
        raise SystemExit(f"CPI inside the gap at t={t}")
    for i, sr in enumerate(r["spatial_receivers"]):
        rxp = np.array(sr["receiver_position_enu_m"])
        dR = np.linalg.norm(pt - gnb) + np.linalg.norm(rxp - pt) - np.linalg.norm(rxp - gnb)
        dets = sr.get("detections") or []
        n += 1
        if any(abs(d["bistatic_range_m"] - (dR + np.linalg.norm(rxp - gnb))) <= sr["range_res_m"]
               or abs(d["bistatic_range_m"] - dR) <= sr["range_res_m"] for d in dets): ok += 1
print(f"receiver-CPIs with the target within 1 range cell: {ok}/{n}")
assert ok >= 0.8 * n, "engine misses the synthetic target (or cable delay did not cancel)"
EOF
echo "test_offline_chain: PASS"
```
Before relying on the check: read `report_writer.cc` for the exact per-receiver detection key (the
field list includes `bistatic_range_m` and `delta_path_range_m`) and whether `detections` sits
inside each `spatial_receivers[i]` object. Adjust the two field names in the checker to what the
writer emits, and pick the ONE range convention it uses (absolute bistatic or excess over the
baseline). Do not accept both.

- [ ] **Step 5: Run it**

Run: `cd $BON && ninja -j8 isac_replay && BUILD=$BON bash $W/openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh`
Expected: `test_offline_chain: PASS`.
- **If the cable-delay assertion fails** while a run with `--cable-delay-ns 0 0 0 0` passes: the
  range reference is not the measured per-receiver direct path (spec §7 and the engine's own
  "valid only while the CFR delay origin is absolute" note). Set `dl_reference_measured_los` true
  for OTA in `nr_isac.cc`'s config parsing (find its default with
  `grep -n dl_reference_measured_los *.h *.cc`), and re-run. If it still fails, stop and report;
  do not tune thresholds.

- [ ] **Step 6: Commit**

```bash
cd $W && git add -A openair1/PHY/NR_UE_ISAC tests/passive_rx/ota CMakeLists.txt && git commit -q -m "sensing: record the CFR stream at the ABI, replay it offline, synthetic multistatic test bed

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Per-block debug dumps + viewer

**Files:**
- Modify: `openair1/PHY/NR_UE_ISAC/sensing_engine.cc` (the `dump_window` lambda)
- Create: `openair1/PHY/NR_UE_ISAC/tools/show_block.py`
- Modify: `openair1/PHY/NR_UE_ISAC/README.md` (section "Debugging the chain block by block")

**Interfaces:**
- Dump files: `$NR_ISAC_DEBUG_DIR/seq%06llu_rx%u_<stage>.bin` with stages
  `raw` → `sync` → `pre` → `post`, and `ul<rnti>_raw` → `ul<rnti>_post` for UL.
- Format (unchanged from the existing dump):
  `uint32 antennas, rows, subcarriers; double row_time_slots[rows]; uint8 observed[...]; complex64 values[...]`.

- [ ] **Step 1: Generalise the dump.** In the `dump_window` lambda, replace
  `std::getenv("NR_ISAC_CLUTTER_DUMP_DIR")` with:
```cpp
        const char* dir = std::getenv("NR_ISAC_DEBUG_DIR");
        if (!dir) dir = std::getenv("NR_ISAC_CLUTTER_DUMP_DIR");
```
  Add `dump_window(corrected, "raw");` immediately after
  `CfrWindow corrected = independent_receiver_view(dl_window, receiver);`, and
  `dump_window(corrected, "sync");` right after `apply_sync_correction(...)`. For UL, find the
  existing `dump_window(ul_corrected, "ulpost")` and add the matching `"ulraw"` dump where
  `ul_corrected` is first built. Suffix both with the session:
  `(std::string("ul") + std::to_string(window.session_id) + "_raw").c_str()`.

- [ ] **Step 2: Viewer**

```python
#!/usr/bin/env python3
# openair1/PHY/NR_UE_ISAC/tools/show_block.py
"""Plot one CFR-stage dump: |H| (antenna x row x subcarrier), the per-row range profile, and slow-time
phase at the strongest range bin. Usage: show_block.py DUMP.bin [--png out.png]"""
import argparse, numpy as np

def load(path):
    with open(path, "rb") as f:
        ant, rows, sub = np.frombuffer(f.read(12), np.uint32)
        t = np.frombuffer(f.read(8 * rows), np.float64)
        rest = f.read()
    nval = ant * rows * sub
    obs = np.frombuffer(rest[: len(rest) - 8 * nval], np.uint8)
    val = np.frombuffer(rest[len(rest) - 8 * nval:], np.complex64).reshape(ant, rows, sub)
    return int(ant), int(rows), int(sub), t, obs, val

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("dump"); ap.add_argument("--png")
    a = ap.parse_args(); ant, rows, sub, t, obs, H = load(a.dump)
    import matplotlib; matplotlib.use("Agg" if a.png else matplotlib.get_backend()); import matplotlib.pyplot as plt
    prof = np.abs(np.fft.ifft(H, axis=2)) ** 2
    fig, ax = plt.subplots(3, ant, figsize=(4 * ant, 9), squeeze=False)
    for i in range(ant):
        ax[0, i].imshow(20 * np.log10(np.abs(H[i]) + 1e-12), aspect="auto"); ax[0, i].set_title(f"ch{i} |H| dB")
        ax[1, i].imshow(10 * np.log10(prof[i] + 1e-12), aspect="auto"); ax[1, i].set_title("range profile per row")
        b = int(np.argmax(prof[i].mean(0))); ax[2, i].plot(t, np.unwrap(np.angle(np.fft.ifft(H[i], axis=1)[:, b])))
        ax[2, i].set_title(f"slow-time phase @bin {b}")
    fig.suptitle(f"{a.dump}: {ant} ant x {rows} rows x {sub} sc, observed={obs.mean():.3f}")
    fig.tight_layout(); fig.savefig(a.png) if a.png else plt.show()

if __name__ == "__main__":
    main()
```
Before trusting it: confirm the `values` memory order against `CfrWindow` in `pipeline_types.h`.
If it is `[row][antenna][subcarrier]`, change the reshape to `(rows, ant, sub)` and transpose to
`(ant, rows, sub)`.

- [ ] **Step 3: README section.** Add to `openair1/PHY/NR_UE_ISAC/README.md`:

```markdown
## Debugging the chain block by block

Set `NR_ISAC_DEBUG_DIR=<dir>` (the launcher does this with `--debug`). Every artifact maps to one block:

| block | artifact | view with |
|---|---|---|
| input (ABI) | `cfr_rows.bin` — every admitted row, per antenna, plus gate closes | `isac_replay -O <conf> --rows cfr_rows.bin` |
| gate | `SENSING_GATE open/close/stats` receiver log lines | monitor Pipeline tab |
| per-channel CFO | `BRANCHFO d_vs_br0=[...]` log line | monitor Pipeline tab |
| [1] sync | `seqN_rxI_raw.bin` → `seqN_rxI_sync.bin`; report `spatial_receivers[i].sync` | `show_block.py` |
| [2] families + clutter | `seqN_rxI_pre.bin` → `seqN_rxI_post.bin`; report `causal_clutter` | `show_block.py` |
| [3] CPI formation | report `cpi_plan`, `dropped_cpis`, `discarded_pending_*` | monitor Pipeline tab |
| [4]/[5] RD map, CLEAN, CFAR | report `rvm_blob` (pre-CLEAN), `rvm_final_blob`, components, `detections` (capture = 1) | monitor DL/UL tabs |
| [6] long dwells | report `long_dwells` | report JSON |
| UL per UE | `seqN_rxI_ul<rnti>_raw/post.bin`; report `uplink_sessions[]` | `show_block.py`, monitor UL tab |
| [7]–[10] | `realtime_chain.py --debug-dir`: `stage8.jsonl`, `localiser.jsonl`, `stage9.jsonl`, `tracks.jsonl` | monitor 3D tab |

Offline: record once over the air, then replay deterministically as often as needed.
```

- [ ] **Step 4: Verify with the offline test bed**

Run: `rm -rf /tmp/isac_dbg && mkdir /tmp/isac_dbg && cd $BON && ninja isac_replay && NR_ISAC_DEBUG_DIR=/tmp/isac_dbg BUILD=$BON bash $W/openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh && ls /tmp/isac_dbg | sed 's/seq[0-9]*_//' | sort | uniq -c && python3 $W/openair1/PHY/NR_UE_ISAC/tools/show_block.py $(ls /tmp/isac_dbg/*_rx0_sync.bin | head -1) --png /tmp/isac_dbg/rx0_sync.png`
Expected: PASS; counts for `rx0_raw/sync/pre/post` … `rx3_*` and `ul17921_*`-style files;
`rx0_sync.png` shows one bright direct path near a constant range bin.

- [ ] **Step 5: Commit**

```bash
cd $W && git add -A openair1/PHY/NR_UE_ISAC && git commit -q -m "sensing: per-block CFR dumps under NR_ISAC_DEBUG_DIR, block viewer, debugging map

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: Report map crop/decimation (makes live maps affordable)

**Files:**
- Modify: `openair1/PHY/NR_UE_ISAC/pipeline_types.h` (two config fields), `openair1/PHY/NR_UE_ISAC/nr_isac.cc` (parse), `openair1/PHY/NR_UE_ISAC/report_writer.cc`

**Interfaces:**
- **New `[sensing]` keys:**
  - `rvm_period_s` (real, default 0.5): maps are emitted at most this often; other CPIs carry no
    blobs.
  - `rvm_max_range_m` (real, default 0 = full axis): crops each map's range axis to
    `ceil(rvm_max_range_m / range_res_m)` bins.
- **Every emitted map** gets `rvm_range_bins`, `rvm_rate_bins`, `rvm_range_res_m` and
  `rvm_rate_res_mps` next to its blob, for DL spatial receivers and UL sessions alike.

- [ ] **Step 1: Failing check.** Append to `test_offline_chain.sh`, before `echo PASS`:

```bash
python3 - "$O" <<'EOF'
import json, math, sys
reps = [json.loads(l) for l in open(f"{sys.argv[1]}/reports.jsonl") if l.strip()]
with_map = [r for r in reps if any("rvm_blob" in s for s in r["spatial_receivers"])]
assert with_map, "no maps emitted"
assert len(with_map) < len(reps), "maps not decimated"
for r in with_map:
    for s in r["spatial_receivers"]:
        if "rvm_blob" not in s: continue
        nb, nr = s["rvm_range_bins"], s["rvm_rate_bins"]
        assert len(s["rvm_blob"]) == nb * nr
        assert nb <= math.ceil(600.0 / s["rvm_range_res_m"]) + 1, (nb, s["rvm_range_res_m"])
print("map crop/decimation ok")
EOF
```
and set `rvm_period_s = 0.5; rvm_max_range_m = 600.0;` in `sensing_synth.conf`.
Run the test. Expected: FAIL (`KeyError: 'rvm_range_bins'`).

- [ ] **Step 2: Implement.**
  - `pipeline_types.h`: add `double rvm_period_s = 0.5; double rvm_max_range_m = 0.0;` to
    `PipelineConfig`.
  - `nr_isac.cc`: add two `real(...)` params next to `capture`, and assign them to `pipeline`.
  - `report_writer.cc`:
    - Add `double last_rvm_emit_ns_ = -1e18;` to `ReportWriter`.
    - In `emit()`, compute `const bool emit_maps = c.capture_rvm && (now_ns - last_rvm_emit_ns_) >= config_.rvm_period_s * 1e9;`
      (now from `std::chrono::steady_clock`) and pass `emit_maps` into `build_report_json` in
      place of `c.capture_rvm` for the map branches only. The component lists keep
      `c.capture_rvm`.
    - At each of the three blob writers (spatial DL ~382, UL session ~434, top-level ~702), bound
      the range loop with
      `const uint32_t nb = (c.rvm_max_range_m > 0 && axes.range_res_m > 0) ? std::min<uint32_t>(axes.range_bins, (uint32_t)std::ceil(c.rvm_max_range_m / axes.range_res_m)) : axes.range_bins;`,
      iterate `q < nb`, and write
      `"rvm_range_bins":nb,"rvm_rate_bins":axes.rate_bins,"rvm_range_res_m":axes.range_res_m,"rvm_rate_res_mps":axes.rate_res_mps`
      before the blob.
    - Update `last_rvm_emit_ns_` when maps were written.
  - Read the three sites first: they index `initial_likelihood[q*rate_bins + d]` with the loop
    order `d` outer, `q` inner. Keep that order (layout `doppler_major_range_minor`).

- [ ] **Step 3: Run the offline test**

Run: `cd $BON && ninja isac_replay && BUILD=$BON bash $W/openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh`
Expected: `map crop/decimation ok` … `test_offline_chain: PASS`.

- [ ] **Step 4: Commit**

```bash
cd $W && git add -A openair1/PHY/NR_UE_ISAC tests/passive_rx/ota && git commit -q -m "sensing: crop and rate-limit report RD maps so live capture stays affordable

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: Survey tool (single source of geometry)

**Files:**
- Create: `tests/passive_rx/survey.py`, `tests/passive_rx/test_survey.py`
- Create: `tests/passive_rx/ota/survey.json` (template for the user to fill)

**Interfaces:**
- `python3 survey.py SURVEY.json --geometry OUT.json --apply TEMPLATE.conf OUT.conf [--report-path P]`
  - Validates, writes `realtime_chain`'s geometry (`transmitter_position_m`,
    `receiver_positions_m.rx0..rx3`, `max_range_m`).
  - Writes the conf with the `sensing` keys `spatial_rx_positions`, `tx_pos_x/y/z`,
    `rvm_max_range_m`, and optionally `report_path`.
  - Exit code 2 plus a message on any violation.

- [ ] **Step 1: Failing test**

```python
# tests/passive_rx/test_survey.py
import json, subprocess, sys, tempfile, pathlib
HERE = pathlib.Path(__file__).parent
CONF = 'uicc0 = { imsi = "0"; };\nsensing = {\n  enable = 1;\n  tx_pos_x = 0.0;\n};\n'

def run(survey, tmp):
    s = tmp / "s.json"; s.write_text(json.dumps(survey)); t = tmp / "t.conf"; t.write_text(CONF)
    return subprocess.run([sys.executable, str(HERE / "survey.py"), str(s), "--geometry", str(tmp / "g.json"),
                           "--apply", str(t), str(tmp / "o.conf"), "--report-path", "/x/r.jsonl"],
                          capture_output=True, text=True)

GOOD = {"gnb_m": [120, 80, 25], "rx_antennas_m": {"ch0": [0, 0, 2], "ch1": [40, 0, 2], "ch2": [0, 40, 2], "ch3": [40, 40, 6]},
        "max_range_m": 600}

def test_good():
    with tempfile.TemporaryDirectory() as d:
        d = pathlib.Path(d); r = run(GOOD, d); assert r.returncode == 0, r.stderr
        g = json.loads((d / "g.json").read_text())
        assert g["transmitter_position_m"] == [120, 80, 25] and g["receiver_positions_m"]["rx3"] == [40, 40, 6]
        conf = (d / "o.conf").read_text()
        assert 'spatial_rx_positions = "0,0,2;40,0,2;0,40,2;40,40,6";' in conf
        assert "tx_pos_x = 120;" in conf and conf.count("tx_pos_x") == 1
        assert 'report_path = "/x/r.jsonl";' in conf and "rvm_max_range_m = 600;" in conf
        assert conf.index("spatial_rx_positions") < conf.index("};", conf.index("sensing"))

def test_colocated_rejected():
    bad = json.loads(json.dumps(GOOD)); bad["rx_antennas_m"]["ch1"] = [0.3, 0, 2]
    with tempfile.TemporaryDirectory() as d:
        r = run(bad, pathlib.Path(d)); assert r.returncode == 2 and "co-located" in r.stderr

def test_missing_channel_rejected():
    bad = json.loads(json.dumps(GOOD)); del bad["rx_antennas_m"]["ch3"]
    with tempfile.TemporaryDirectory() as d:
        r = run(bad, pathlib.Path(d)); assert r.returncode == 2 and "ch3" in r.stderr

if __name__ == "__main__":
    test_good(); test_colocated_rejected(); test_missing_channel_rejected(); print("test_survey: PASS")
```
Run: `python3 tests/passive_rx/test_survey.py`. Expected: FAIL (survey.py missing).

- [ ] **Step 2: Implement**

```python
#!/usr/bin/env python3
# tests/passive_rx/survey.py
"""Site survey -> engine conf keys + realtime_chain geometry, from ONE file (spec §7).
Frame: ENU metres, origin at the X410. ch<i> = X410 RX channel i = --ue-nb-ant-rx order."""
import argparse, json, math, re, sys

def die(msg):
    print(f"survey: {msg}", file=sys.stderr); sys.exit(2)

def fmt(x):
    return f"{x:g}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("survey"); ap.add_argument("--geometry", required=True)
    ap.add_argument("--apply", nargs=2, metavar=("TEMPLATE", "OUT"), required=True)
    ap.add_argument("--report-path")
    a = ap.parse_args()
    s = json.load(open(a.survey))
    gnb = s.get("gnb_m"); ants = s.get("rx_antennas_m") or {}
    rx = []
    for i in range(4):
        p = ants.get(f"ch{i}")
        if p is None: die(f"missing ch{i}")
        if len(p) != 3 or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in p): die(f"ch{i} is not [x,y,z]")
        rx.append([float(v) for v in p])
    if not gnb or len(gnb) != 3: die("gnb_m must be [x,y,z]")
    for i in range(4):
        for j in range(i + 1, 4):
            if math.dist(rx[i], rx[j]) < 1.0: die(f"ch{i} and ch{j} are co-located (<1 m): stage 9 needs separated receivers")
        if math.dist(rx[i], gnb) < 1.0: die(f"ch{i} co-located with the gNB")
    max_range = float(s.get("max_range_m", 0.0))
    json.dump({"transmitter_position_m": gnb, "receiver_positions_m": {f"rx{i}": rx[i] for i in range(4)},
               "max_range_m": max_range or None}, open(a.geometry, "w"), indent=1)
    keys = {"spatial_rx_positions": '"' + ";".join(",".join(fmt(v) for v in p) for p in rx) + '"',
            "tx_pos_x": fmt(gnb[0]), "tx_pos_y": fmt(gnb[1]), "tx_pos_z": fmt(gnb[2])}
    if max_range: keys["rvm_max_range_m"] = fmt(max_range)
    if a.report_path: keys["report_path"] = f'"{a.report_path}"'
    text = open(a.apply[0]).read()
    m = re.search(r"^\s*sensing\s*=\s*\{", text, re.M)
    if not m: die("template has no 'sensing = {' block")
    end = text.index("};", m.end())
    body = text[m.end():end]
    for k, v in keys.items():
        line = f"  {k} = {v};"
        body, n = re.subn(rf"^\s*{k}\s*=.*?;\s*$", line, body, flags=re.M)
        if n == 0: body = body.rstrip("\n") + "\n" + line + "\n"
    open(a.apply[1], "w").write(text[:m.end()] + body + text[end:])

if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Run the test**

Run: `python3 tests/passive_rx/test_survey.py`
Expected: `test_survey: PASS`.

- [ ] **Step 4: Template for the user** (`tests/passive_rx/ota/survey.json`, values to be measured
  on site; the launcher refuses the all-zero template because of the co-location check):

```json
{"frame": "ENU metres (east, north, up), origin at the X410. Measure each position on site.",
 "gnb_m": [0.0, 0.0, 0.0],
 "rx_antennas_m": {"ch0": [0.0, 0.0, 0.0], "ch1": [0.0, 0.0, 0.0], "ch2": [0.0, 0.0, 0.0], "ch3": [0.0, 0.0, 0.0]},
 "max_range_m": 600}
```

- [ ] **Step 5: Regenerate Task 10's synthetic conf with the tool**, and re-run the offline test:

Run: `cd $W && python3 tests/passive_rx/survey.py openair1/PHY/NR_UE_ISAC/tools/survey_synth.json --geometry /tmp/isac_synth/geometry.json --apply tests/passive_rx/ota/sensing_synth.conf tests/passive_rx/ota/sensing_synth.conf && BUILD=$BON bash openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
cd $W && git add tests/passive_rx/survey.py tests/passive_rx/test_survey.py tests/passive_rx/ota && git commit -q -m "passive rx: one survey file drives the engine conf and the realtime chain geometry

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 14: `realtime_chain.py` — agnostic inputs, dynamic UEs, debug dir, status sidecar

**Files:**
- Modify: `openair1/PHY/NR_UE_ISAC/tools/realtime_chain.py`
- Create: `openair1/PHY/NR_UE_ISAC/tools/test_realtime_chain_agnostic.py`

**Interfaces:**
- **CLI:** `realtime_chain.py --follow reports.jsonl --geometry geometry.json --out tracks.jsonl
  [--status status.jsonl] [--debug-dir DIR] [--idle-stop-s S]`.
  - `--ul-sessions` becomes optional (default: discover). The `RANGE_RES_M`/`RATE_RES_MPS`
    constants are removed.
- **`status.jsonl`**, one line per CPI:
  `{"t": midpoint_air_time_s, "wall": unix_s, "ms": per_cpi_ms, "sessions": [rnti...],
  "ue": {"<rnti>": {"position_m": [...], "position_sigma_m": [...]}}, "range_res_m": x, "rate_res_mps": y}`.
- **Debug dir files:** `stage8.jsonl` (queued stage-8 objects), `stage9.jsonl` (stage-9 outputs),
  `localiser.jsonl` (UE estimates).

- [ ] **Step 1: Failing test (stubs replace the frozen stages; only the new plumbing is tested)**

```python
# openair1/PHY/NR_UE_ISAC/tools/test_realtime_chain_agnostic.py
import sys, pathlib, types
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import realtime_chain as rc

made = {"loc": [], "s8": [], "s9": []}
class S8:
    def __init__(self, *a, **k): made["s8"].append(k.get("ul_session"))
    def feed(self, rec): return []
class Loc:
    def __init__(self, rx, rr, vr, w, ul_session=None, tx_position=None, max_range_m=None, ul_advance=None):
        made["loc"].append((rr, vr, ul_session, max_range_m))
    def feed(self, rec): return {"position_m": [1, 2, 3], "position_sigma_m": [1, 1, 1]}
class S9:
    def __init__(self, tx, ue, rx, rate): made["s9"].append(rate)
    def step(self, t, legs): return None
class T10:
    def feed(self, m): return None
    def flush(self): return []
rc.Stage8Stream, rc.LocaliserStream, rc.Stage9Stream, rc.TrackerStream = S8, Loc, S9, T10

def rep(t, sessions):
    return {"midpoint_air_time_s": t,
            "spatial_receivers": [{"range_res_m": 2.0, "vel_res_mps": 0.5, "range_max_m": 800.0} for _ in range(4)],
            "uplink_sessions": [{"pusch_session_id": s} for s in sessions]}

ch = rc.RealtimeChain([0, 0, 0], [[0, 0, 0]] * 4, None, max_range_m=None)
ch.feed(rep(0.1, []))
assert made["s9"] == [0.5], made
assert made["loc"] == [], "no UE sessions yet"
ch.feed(rep(0.2, [0x4601]))
assert made["loc"] == [(2.0, 0.5, 0x4601, 800.0)], made["loc"]
assert 0x4601 in made["s8"]
ch.feed(rep(0.3, [0x4601, 0x4602]))
assert [x[2] for x in made["loc"]] == [0x4601, 0x4602]
assert set(ch.status()["sessions"]) == {0x4601, 0x4602}
print("test_realtime_chain_agnostic: PASS")
```
Run: `python3 openair1/PHY/NR_UE_ISAC/tools/test_realtime_chain_agnostic.py`
Expected: FAIL (the constructor rejects `None` sessions / there is no `status()`).

- [ ] **Step 2: Implement (the frozen stages stay untouched; only the chain wiring changes)**

Replace `RealtimeChain.__init__`, add `_ensure` / `_ensure_session` / `status`, and prepend them
to `feed`:
```python
    def __init__(self, tx, rx_positions, ul_sessions=None, ul_advance=None, max_speed_mps=50.0, max_range_m=None,
                 window_cpis=60, debug_dir=None):
        self.tx = np.asarray(tx, float); self.rx = [np.asarray(r, float) for r in rx_positions]
        self.max_speed_mps, self.max_range_m, self.window_cpis = max_speed_mps, max_range_m, window_cpis
        self.ul_advance = ul_advance or {}
        self.s8_dl = Stage8Stream(max_speed_mps, leg="dl")
        self.s8_ul, self.loc, self.ue_tracks = {}, {}, {}
        self.s9 = None; self.s10 = TrackerStream(); self.res = None
        self.pending, self.times = {}, []
        self.stats = {"cpis": 0, "s9": 0, "tracks": 0, "ms": []}
        self._fixed_sessions = list(ul_sessions) if ul_sessions else None
        self.dbg = None
        if debug_dir:
            import os; os.makedirs(debug_dir, exist_ok=True)
            self.dbg = {k: open(f"{debug_dir}/{k}.jsonl", "a") for k in ("stage8", "stage9", "localiser")}

    def _ensure(self, rec):
        """Axes come from the report itself (spec §9): nothing cell-specific is compiled in."""
        if self.res is not None: return
        sr = next((s for s in rec.get("spatial_receivers") or [] if (s.get("range_res_m") or 0) > 0), None)
        if sr is None: return
        self.res = (float(sr["range_res_m"]), float(sr["vel_res_mps"]))
        if not self.max_range_m: self.max_range_m = float(sr.get("range_max_m") or 0) or None
        self.s9 = Stage9Stream(self.tx, None, self.rx, self.res[1])
        for k in self._fixed_sessions or []: self._ensure_session(k)

    def _ensure_session(self, k):
        if k in self.loc or self.res is None: return
        self.s8_ul[k] = Stage8Stream(self.max_speed_mps, leg="ul", ul_session=k)
        self.loc[k] = LocaliserStream(self.rx, self.res[0], self.res[1], self.window_cpis, ul_session=k,
                                      tx_position=self.tx, max_range_m=self.max_range_m, ul_advance=self.ul_advance.get(k))
        self.ue_tracks[k] = []

    def status(self):
        return {"sessions": sorted(self.loc), "range_res_m": self.res and self.res[0], "rate_res_mps": self.res and self.res[1],
                "ue": {str(k): {kk: v[-1][kk] for kk in ("position_m", "position_sigma_m") if kk in v[-1]}
                       for k, v in self.ue_tracks.items() if v}}
```
At the top of `feed(self, rec)`, before the existing body:
```python
        self._ensure(rec)
        if self.s9 is None: return []
        if self._fixed_sessions is None:
            for s in rec.get("uplink_sessions") or []:
                if s.get("pusch_session_id"): self._ensure_session(int(s["pusch_session_id"]))
```
In the existing body:
- After `est = loc.feed(rec)`, when `est` is not None and `self.dbg`: write
  `json.dumps(dict(est, session=k)) + "\n"` to `self.dbg["localiser"]`.
- In `_queue`: write each queued `o` to `self.dbg["stage8"]`.
- After `s9 = self.s9.step(...)`, when `s9` is not None: write
  `json.dumps(s9, default=float) + "\n"` to `self.dbg["stage9"]`.
- `flush()`: return `[]` when `self.s9 is None`.

In `main()`:
- Make `--ul-sessions` default `None` and parse the `--ul-sessions 0x...` values with
  `int(x, 0)` (`type=lambda s: int(s, 0)`).
- Add `--status` and `--debug-dir`.
- Construct with `RealtimeChain(g["transmitter_position_m"], rx, a.ul_sessions, ul_advance=adv, max_range_m=g.get("max_range_m"), debug_dir=a.debug_dir)`.
- After each `chain.feed(rec)`, append to the status file:
  `json.dumps(dict(chain.status(), t=rec.get("midpoint_air_time_s"), wall=time.time(), ms=chain.stats["ms"][-1] if chain.stats["ms"] else None))`.
- Delete the `RANGE_RES_M` / `RATE_RES_MPS` constants, and remove the ZeroMQ `--pub` option
  (JSONL is the only transport, spec §5).

In `ue_localiser_dfs.py`, make `samples_per_bin` default `None`, and raise
`ValueError("samples_per_bin is required with ul_advance")` where it is used with `ul_advance`.
That removes the 273-PRB `4096/3276` default without changing behaviour when `ul_advance` is unset.

- [ ] **Step 3: Run the test + the frozen tools' own test**

Run: `cd $W/openair1/PHY/NR_UE_ISAC/tools && python3 test_realtime_chain_agnostic.py && python3 test_ue_localiser_dfs.py`
Expected: `test_realtime_chain_agnostic: PASS`, and the localiser test passes unchanged.

- [ ] **Step 4: End-to-end on the synthetic bed.** Append to `test_offline_chain.sh`:

```bash
python3 $T/realtime_chain.py --follow $O/reports.jsonl --geometry /tmp/isac_synth/geometry.json \
  --out $O/tracks.jsonl --status $O/status.jsonl --debug-dir $O/chain_dbg --idle-stop-s 3 > $O/chain_summary.json
python3 - "$O" <<'EOF'
import json, sys
O = sys.argv[1]; st = [json.loads(l) for l in open(f"{O}/status.jsonl") if l.strip()]
assert st and st[-1]["range_res_m"], "status sidecar empty"
assert 0x4601 in st[-1]["sessions"], "UL session not discovered"
summ = json.load(open(f"{O}/chain_summary.json"))
print("chain per-CPI ms", summ["per_cpi_ms"])
EOF
```
(Tracks on synthetic data are reported, not asserted: the frozen tracker's accuracy is a Sionna
result, and this bed is only for plumbing.)
Run: `BUILD=$BON bash $W/openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh`
Expected: PASS, printing per-CPI ms.

- [ ] **Step 5: Commit**

```bash
cd $W && git add -A openair1/PHY/NR_UE_ISAC/tools && git commit -q -m "realtime chain: axes from the reports, UEs discovered by C-RNTI, status sidecar and per-stage debug

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 15: Monitor — file tailing, per-receiver DL/UL RD maps, rotatable 3D tracks, pipeline

**Files:**
- Create: `tests/passive_rx/monitor/sensing_view.py` (pure extraction, tested)
- Create: `tests/passive_rx/monitor/test_sensing_view.py`
- Create: `tests/passive_rx/monitor/sensing.html`
- Create: `tests/passive_rx/monitor/vendor/plotly-2.35.2.min.js` (downloaded once, committed)
- Modify: `tests/passive_rx/monitor/monitor.py`

**Interfaces:**
- `sensing_view.py`:
  - `class SensingState` with methods `add_report(rep)`, `add_tracks(rec)`, `add_status(rec)`,
    `add_log_line(line)` and `snapshot() -> dict`.
  - `snapshot()` returns: `{"dl": [{"rx": id, "pos": [..], "nb", "nr", "range_res_m",
    "rate_res_mps", "map_db": [...], "dets": [[range_m, rate_mps], ...]}] x4,
    "ul": {"<rnti>": [same per receiver]}, "tracks": {"<id>": {"trail": [[x,y,z]...],
    "confirmed": bool}}, "ue": {...}, "geometry": {"gnb": [..], "rx": [[..]x4]},
    "pipeline": {...}}`.
- `monitor.py` routes:
  - `/` → `sensing.html`
  - `/receiver` → the legacy `monitor.html`
  - `/sensing` → `SensingState.snapshot()` JSON
  - `/vendor/<file>`
- New monitor.py flags: `--reports FILE`, `--tracks FILE`, `--status FILE`, `--geometry FILE`
  (all tailed). `--connect` stays optional; zmq is imported lazily.

- [ ] **Step 1: Failing test**

```python
# tests/passive_rx/monitor/test_sensing_view.py
import math, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from sensing_view import SensingState

def spatial(i):
    return {"receiver_id": f"rx{i}", "receiver_position_enu_m": [i, 0, 2], "rvm_range_bins": 3, "rvm_rate_bins": 2,
            "rvm_range_res_m": 3.05, "rvm_rate_res_mps": 1.1, "rvm_blob": [1, 10, 100, 1000, 0, 1],
            "detections": [{"bistatic_range_m": 6.1, "range_rate_mps": 1.1}]}

def test_maps_and_ul():
    s = SensingState(); s.set_geometry({"transmitter_position_m": [9, 9, 9], "receiver_positions_m": {f"rx{i}": [i, 0, 2] for i in range(4)}})
    rep = {"midpoint_air_time_s": 1.0, "spatial_receivers": [spatial(i) for i in range(4)],
           "uplink_sessions": [{"pusch_session_id": 17921, "receivers": [spatial(i) for i in range(4)]}]}
    s.add_report(rep); snap = s.snapshot()
    assert len(snap["dl"]) == 4 and snap["dl"][0]["nb"] == 3 and snap["dl"][0]["nr"] == 2
    assert snap["dl"][0]["map_db"][1] == 10.0 and snap["dl"][0]["map_db"][4] is None   # 10*log10; zero -> None
    assert snap["dl"][2]["dets"] == [[6.1, 1.1]]
    assert list(snap["ul"]) == ["17921"] and len(snap["ul"]["17921"]) == 4
    assert snap["geometry"]["gnb"] == [9, 9, 9]

def test_map_persists_when_report_has_none():
    s = SensingState(); s.add_report({"spatial_receivers": [spatial(0)]})
    s.add_report({"spatial_receivers": [{"receiver_id": "rx0", "detections": []}]})
    assert s.snapshot()["dl"][0]["nb"] == 3          # decimated reports keep the last map

def test_tracks_trail_and_expiry():
    s = SensingState()
    for t in range(3):
        s.add_tracks({"time_s": t, "tracks": [{"track_id": 5, "confirmed": True, "position": [t, 0, 1]}]})
    assert s.snapshot()["tracks"]["5"]["trail"] == [[0, 0, 1], [1, 0, 1], [2, 0, 1]]
    s.add_tracks({"time_s": 3, "tracks": []})
    assert "5" not in s.snapshot()["tracks"]

def test_pipeline_from_log():
    s = SensingState()
    s.add_log_line("[PHY] SENSING_GATE open rnti=0x4601")
    s.add_log_line("[PHY] SENSING_GATE stats open=1 admitted=10 rejected=2 gate_discarded_rows=0")
    s.add_log_line("[PHY] SENSING: BRANCHFO d_vs_br0=[0.0 3.2 -1.1 4.0] Hz (EMA")
    p = s.snapshot()["pipeline"]
    assert p["gate_open"] is True and p["admitted"] == 10 and p["branchfo_hz"] == [0.0, 3.2, -1.1, 4.0]

if __name__ == "__main__":
    test_maps_and_ul(); test_map_persists_when_report_has_none(); test_tracks_trail_and_expiry(); test_pipeline_from_log()
    print("test_sensing_view: PASS")
```
Run: `python3 tests/passive_rx/monitor/test_sensing_view.py`. Expected: FAIL (module missing).

- [ ] **Step 2: Implement `sensing_view.py`**

```python
# tests/passive_rx/monitor/sensing_view.py
"""Pure extraction from the frozen chain's outputs into what the page draws. No I/O here."""
import math, re, threading, time

TRAIL = 120
RE_GATE_OPEN = re.compile(r"SENSING_GATE open rnti=(0x[0-9a-fA-F]+)")
RE_GATE_CLOSE = re.compile(r"SENSING_GATE close")
RE_GATE_STATS = re.compile(r"SENSING_GATE stats open=(\d) admitted=(\d+) rejected=(\d+) gate_discarded_rows=(\d+)")
RE_BRFO = re.compile(r"BRANCHFO d_vs_br0=\[([^\]]*)\]")
RE_LLRCONF = re.compile(r"LLRCONF qm=(\d) calibrated=(\d) tau_rel=([-\d.]+) crc_ok_bit_agreement=([\d.]+)")

def _db(v):
    return round(10.0 * math.log10(v), 2) if v and v > 0 else None

def _receiver_view(s, keep):
    out = dict(keep or {})
    out.update({"rx": s.get("receiver_id"), "pos": s.get("receiver_position_enu_m") or (keep or {}).get("pos")})
    if "rvm_blob" in s and s.get("rvm_range_bins"):
        out.update({"nb": s["rvm_range_bins"], "nr": s["rvm_rate_bins"], "range_res_m": s.get("rvm_range_res_m"),
                    "rate_res_mps": s.get("rvm_rate_res_mps"), "map_db": [_db(v) for v in s["rvm_blob"]]})
    out["dets"] = [[d.get("bistatic_range_m"), d.get("range_rate_mps")] for d in (s.get("detections") or [])]
    return out

class SensingState:
    def __init__(self):
        self._lock = threading.Lock()
        self.dl, self.ul, self.tracks, self.ue, self.geometry = [], {}, {}, {}, {}
        self.pipeline = {"gate_open": None, "last_report_wall": None, "cpis": 0}

    def set_geometry(self, g):
        with self._lock:
            self.geometry = {"gnb": g.get("transmitter_position_m"),
                             "rx": [g["receiver_positions_m"][f"rx{i}"] for i in range(4)] if g.get("receiver_positions_m") else []}

    def add_report(self, rep):
        with self._lock:
            srs = rep.get("spatial_receivers") or []
            self.dl = [_receiver_view(s, self.dl[i] if i < len(self.dl) else None) for i, s in enumerate(srs)]
            for sess in rep.get("uplink_sessions") or []:
                k = str(sess.get("pusch_session_id")); old = self.ul.get(k, [])
                self.ul[k] = [_receiver_view(s, old[i] if i < len(old) else None) for i, s in enumerate(sess.get("receivers") or [])]
            p = self.pipeline
            p.update({"cpis": p["cpis"] + 1, "last_report_wall": time.time(),
                      "dropped_cpis": rep.get("dropped_cpis"), "discarded_pending_rows": rep.get("discarded_pending_rows"),
                      "dropped_submissions": rep.get("dropped_submissions"), "cpi_plan": rep.get("cpi_plan")})

    def add_tracks(self, rec):
        with self._lock:
            live = set()
            for t in rec.get("tracks") or []:
                k = str(t["track_id"]); live.add(k)
                e = self.tracks.setdefault(k, {"trail": [], "confirmed": False})
                e["trail"] = (e["trail"] + [list(t["position"])])[-TRAIL:]; e["confirmed"] = bool(t.get("confirmed"))
            for k in [k for k in self.tracks if k not in live]:
                del self.tracks[k]

    def add_status(self, rec):
        with self._lock:
            self.ue = rec.get("ue") or {}
            self.pipeline.update({"chain_ms": rec.get("ms"), "chain_lag_s": time.time() - rec["wall"] if rec.get("wall") else None,
                                  "ul_sessions": rec.get("sessions")})

    def add_log_line(self, line):
        with self._lock:
            p = self.pipeline
            if RE_GATE_OPEN.search(line): p["gate_open"] = True; p["gate_rnti"] = RE_GATE_OPEN.search(line).group(1)
            elif RE_GATE_CLOSE.search(line): p["gate_open"] = False
            m = RE_GATE_STATS.search(line)
            if m: p.update({"gate_open": m.group(1) == "1", "admitted": int(m.group(2)), "rejected": int(m.group(3)),
                            "gate_discarded_rows": int(m.group(4))})
            m = RE_BRFO.search(line)
            if m: p["branchfo_hz"] = [float(x) for x in m.group(1).split()]
            m = RE_LLRCONF.search(line)
            if m: p.setdefault("llrconf", {})[m.group(1)] = {"calibrated": m.group(2) == "1", "tau_rel": float(m.group(3)),
                                                             "agreement": float(m.group(4))}
            if "CUDA" in line and "SENSING" in line: p["backend_line"] = line.strip()[-160:]

    def snapshot(self):
        with self._lock:
            return {"dl": self.dl, "ul": self.ul, "tracks": self.tracks, "ue": self.ue, "geometry": self.geometry,
                    "pipeline": dict(self.pipeline), "now": time.time()}
```
Run: `python3 tests/passive_rx/monitor/test_sensing_view.py`. Expected: `test_sensing_view: PASS`.
Before relying on it, check `report_writer.cc` for the exact per-detection keys (`bistatic_range_m`,
`range_rate_mps`) and fix `_receiver_view` to match. The test's detection dict must use the same
keys.

- [ ] **Step 3: Tailing + routes in `monitor.py`.**
  - Move `import zmq` inside `sub_thread`.
  - Add:
```python
def tail_file(path, on_line, poll_s=0.2):
    """Follow a file from its start (fresh per run), surviving its late creation."""
    import os
    while not os.path.exists(path): time.sleep(poll_s)
    with open(path, "r", errors="replace") as f:
        buf = ""
        while True:
            chunk = f.read()
            if not chunk: time.sleep(poll_s); continue
            buf += chunk
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if line.strip(): on_line(line)

def json_lines(cb):
    def on(line):
        try: cb(json.loads(line))
        except ValueError: pass   # torn line while the writer is mid-flush
    return on
```
  - In `main()`, add the flags `--reports`, `--tracks`, `--status` and `--geometry`. Create
    `sens = SensingState()`.
    - If `--geometry`: `sens.set_geometry(json.load(open(args.geometry)))`.
    - Start daemon threads:
      - `tail_file(args.reports, json_lines(lambda r: (sens.add_report(r), store.add("file", r))))`
      - `tail_file(args.tracks, json_lines(sens.add_tracks))`
      - `tail_file(args.status, json_lines(sens.add_status))`
      - and, when `--log`, a second `tail_file(args.log, sens.add_log_line)`.
    - Only start `sub_thread`s when `--connect` was given.
  - In `make_handler`, pass `sens` and route:
    - `/sensing` → `json.dumps(sens.snapshot())`
    - `/` → `sensing.html`
    - `/receiver` → `monitor.html`
    - `/vendor/<name>` → serve `Path(__file__).with_name("vendor") / name` with
      `application/javascript`, rejecting names containing `/` or `..`.

- [ ] **Step 4: Vendor Plotly**

Run: `mkdir -p $W/tests/passive_rx/monitor/vendor && curl -fsSL -o $W/tests/passive_rx/monitor/vendor/plotly-2.35.2.min.js https://cdn.jsdelivr.net/npm/plotly.js-dist-min@2.35.2/plotly.min.js && head -c 120 $W/tests/passive_rx/monitor/vendor/plotly-2.35.2.min.js`
Expected: a minified JS banner mentioning plotly.js v2.35.2.

- [ ] **Step 5: `sensing.html`**

```html
<!doctype html>
<html><head><meta charset="utf-8"><title>Passive ISAC — sensing</title>
<script src="/vendor/plotly-2.35.2.min.js"></script>
<style>
:root{--bg:#0f1115;--fg:#e6e6e6;--mut:#8a8f98;--card:#171a21;--ok:#3fb950;--bad:#f85149}
body{margin:0;background:var(--bg);color:var(--fg);font:13px system-ui,sans-serif}
nav{display:flex;gap:4px;padding:8px;border-bottom:1px solid #222}
nav button{background:var(--card);color:var(--fg);border:1px solid #2a2f3a;padding:6px 12px;cursor:pointer}
nav button.on{border-color:#58a6ff}
section{display:none;padding:10px}section.on{display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(420px,1fr));gap:10px}
.card{background:var(--card);border:1px solid #2a2f3a;padding:8px}
.kv td{padding:2px 10px 2px 0}.mut{color:var(--mut)}
#tracks3d{height:78vh}
</style></head><body>
<nav><button data-t="dl" class="on">DL detections</button><button data-t="ul">UL detections</button>
<button data-t="trk">3D tracks</button><button data-t="pipe">Pipeline</button>
<a href="/receiver" style="margin-left:auto;color:#58a6ff;padding:6px">receiver health →</a></nav>
<section id="dl" class="on"><div class="grid" id="dlgrid"></div></section>
<section id="ul"><div id="ulbox"></div></section>
<section id="trk"><div id="tracks3d" class="card"></div></section>
<section id="pipe"><div class="card"><table class="kv" id="pipetab"></table></div></section>
<script>
const $=q=>document.querySelector(q);
document.querySelectorAll("nav button").forEach(b=>b.onclick=()=>{
  document.querySelectorAll("nav button,section").forEach(e=>e.classList.remove("on"));
  b.classList.add("on");$("#"+b.dataset.t).classList.add("on");if(b.dataset.t==="trk")Plotly.Plots.resize("tracks3d");});

function rdMap(div, v, title){
  if(!v.map_db){div.innerHTML=`<div class="mut">${title}: no map yet</div>`;return;}
  const z=[]; for(let d=0;d<v.nr;d++) z.push(v.map_db.slice(d*v.nb,(d+1)*v.nb));
  const x=[...Array(v.nb).keys()].map(q=>q*v.range_res_m), y=[...Array(v.nr).keys()].map(d=>(d-Math.floor(v.nr/2))*v.rate_res_mps);
  const traces=[{type:"heatmap",x,y,z,colorscale:"Viridis",colorbar:{title:"dB"}},
    {type:"scatter",mode:"markers",x:v.dets.map(d=>d[0]),y:v.dets.map(d=>d[1]),marker:{color:"#ff4d4d",size:9,symbol:"x"},name:"det"}];
  Plotly.react(div,traces,{title:{text:title,font:{size:13}},paper_bgcolor:"#171a21",plot_bgcolor:"#171a21",
    font:{color:"#e6e6e6"},xaxis:{title:"bistatic range (m)"},yaxis:{title:"range rate (m/s)"},margin:{l:55,r:10,t:30,b:40},
    height:330,showlegend:false,uirevision:title},{displayModeBar:false});
}
function ensure(parent,id){let e=document.getElementById(id);if(!e){e=document.createElement("div");e.id=id;e.className="card";parent.appendChild(e);}return e;}

let trkInit=false;
function tracks3d(s){
  const g=s.geometry||{}, tr=[];
  if(g.gnb) tr.push({type:"scatter3d",mode:"markers+text",x:[g.gnb[0]],y:[g.gnb[1]],z:[g.gnb[2]],text:["gNB"],marker:{size:6,color:"#f0883e",symbol:"diamond"},name:"gNB"});
  if(g.rx&&g.rx.length) tr.push({type:"scatter3d",mode:"markers+text",x:g.rx.map(p=>p[0]),y:g.rx.map(p=>p[1]),z:g.rx.map(p=>p[2]),
    text:g.rx.map((_,i)=>"ch"+i),marker:{size:5,color:"#58a6ff",symbol:"square"},name:"RX antennas"});
  for(const [k,u] of Object.entries(s.ue||{})) if(u.position_m) tr.push({type:"scatter3d",mode:"markers+text",
    x:[u.position_m[0]],y:[u.position_m[1]],z:[u.position_m[2]],text:["UE "+(+k).toString(16)],marker:{size:5,color:"#d2a8ff"},name:"UE "+k});
  for(const [id,t] of Object.entries(s.tracks||{})){const p=t.trail;tr.push({type:"scatter3d",mode:"lines+markers",
    x:p.map(q=>q[0]),y:p.map(q=>q[1]),z:p.map(q=>q[2]),line:{width:4,color:t.confirmed?"#3fb950":"#8a8f98"},
    marker:{size:p.map((_,i)=>i===p.length-1?6:1.5)},name:"track "+id});}
  Plotly.react("tracks3d",tr,{paper_bgcolor:"#171a21",font:{color:"#e6e6e6"},margin:{l:0,r:0,t:10,b:0},
    uirevision:"keep-camera",   // rotation/zoom survive every live update
    scene:{aspectmode:"data",xaxis:{title:"east (m)"},yaxis:{title:"north (m)"},zaxis:{title:"up (m)"},dragmode:"orbit"}},
    {responsive:true});
}
function pipeline(p){
  const rows=[["gate",p.gate_open===true?'<b style="color:var(--ok)">OPEN</b> '+(p.gate_rnti||""):p.gate_open===false?'<b style="color:var(--bad)">CLOSED</b>':"—"],
    ["admitted / rejected",`${p.admitted??"—"} / ${p.rejected??"—"}`],["gate-discarded rows",p.gate_discarded_rows??"—"],
    ["CPIs",p.cpis],["dropped CPIs",p.dropped_cpis??"—"],["dropped submissions",p.dropped_submissions??"—"],
    ["discarded pending rows",p.discarded_pending_rows??"—"],["BRANCHFO (Hz, vs ch0)",p.branchfo_hz?p.branchfo_hz.join("  "):"—"],
    ["LLR confidence",p.llrconf?JSON.stringify(p.llrconf):"—"],["UL sessions",(p.ul_sessions||[]).map(x=>"0x"+x.toString(16)).join(" ")||"—"],
    ["chain ms / lag s",`${p.chain_ms?.toFixed?.(1)??"—"} / ${p.chain_lag_s?.toFixed?.(1)??"—"}`],
    ["last report",p.last_report_wall?((Date.now()/1000-p.last_report_wall).toFixed(1)+" s ago"):"never"],["backend",p.backend_line||"—"]];
  $("#pipetab").innerHTML=rows.map(r=>`<tr><td class="mut">${r[0]}</td><td>${r[1]}</td></tr>`).join("");
}
async function poll(){
  try{const s=await (await fetch("/sensing",{cache:"no-store"})).json();
    s.dl.forEach((v,i)=>rdMap(ensure($("#dlgrid"),"dl"+i),v,`DL ${v.rx||("rx"+i)} (combined over its CFR, this receiver)`));
    for(const [k,rxs] of Object.entries(s.ul)){const box=ensure($("#ulbox"),"ulbox"+k);box.className="";
      if(!box.dataset.h){box.innerHTML=`<h3>UE C-RNTI 0x${(+k).toString(16)}</h3><div class="grid" id="ulg${k}"></div>`;box.dataset.h=1;}
      rxs.forEach((v,i)=>rdMap(ensure($("#ulg"+k),`ul${k}_${i}`),v,`UL 0x${(+k).toString(16)} ${v.rx||("rx"+i)}`));}
    tracks3d(s); pipeline(s.pipeline);
  }catch(e){console.log(e);}
  setTimeout(poll,1000);
}
poll();
</script></body></html>
```

- [ ] **Step 6: Try it on the synthetic run** (no radio needed)

Run: `cd $W/tests/passive_rx/monitor && timeout 20 python3 monitor.py --port 8090 --reports /tmp/isac_synth/reports.jsonl --tracks /tmp/isac_synth/tracks.jsonl --status /tmp/isac_synth/status.jsonl --geometry /tmp/isac_synth/geometry.json & sleep 4; curl -s localhost:8090/sensing | python3 -c "import json,sys;s=json.load(sys.stdin);print(len(s['dl']),[d.get('nb') for d in s['dl']],list(s['ul']),s['geometry'].get('gnb'))"; curl -s -o /dev/null -w '%{http_code} ' localhost:8090/ localhost:8090/vendor/plotly-2.35.2.min.js localhost:8090/receiver; echo`
Expected: `4 [N, N, N, N] ['17921'] [120.0, 80.0, 25.0]`, then `200 200 200`. Then open
`http://localhost:8090/` through `ssh -L 8090:localhost:8090 sens6`. Check that:
- four DL maps and four UL maps render,
- the 3D view rotates with the mouse,
- the rotation is KEPT across the 1 s refresh.

- [ ] **Step 7: Run both monitor tests, then commit**

Run: `cd $W/tests/passive_rx/monitor && python3 test_sensing_view.py && python3 test_monitor.py`
Expected: both PASS.
```bash
cd $W && git add -A tests/passive_rx/monitor && git commit -q -m "monitor: per-receiver DL and per-UE UL range-Doppler maps, rotatable live 3D tracks, pipeline tab

Tails the JSONL files the chain writes; ZeroMQ is optional. Plotly vendored for offline use.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 16: One-command launcher

**Files:**
- Create: `tests/passive_rx/run_sensing.sh`
- Create: `tests/passive_rx/ota/sensing_ota.conf.template`
- Modify: `tests/passive_rx/captures/run_arm.sh` (line 118: `cd $REPO/cmake_targets`, instead of the
  hardcoded other tree)

**Interfaces:**
- `tests/passive_rx/run_sensing.sh [--dur S] [--rxg DB] [--survey FILE] [--debug] [--port P] [--run-dir DIR]`
  - Defaults: `--dur 600 --rxg 43 --survey tests/passive_rx/ota/survey.json --port 8080
    --run-dir /home/sens/NICOLA/sensing_runs/<ts>`.
  - Writes into the run dir: `ue.conf`, `geometry.json`, `reports.jsonl`, `tracks.jsonl`,
    `status.jsonl`, `run_verdict.json` and `capture/` (run_arm's output); plus `debug/` with
    `--debug`.

- [ ] **Step 1: Conf template.** Create `tests/passive_rx/ota/sensing_ota.conf.template` by copying
  `/home/sens/NICOLA/captures/agnostic_ota.conf`, then in its `sensing` block:
  - Set `sources = "pdsch_dmrs_blind,pdsch_data,pusch_dmrs,pusch_data"; capture = 1; rvm_period_s = 0.5;`.
  - Delete `out_path`, `report_path` (the launcher sets it) and `report_endpoint`. JSONL is the
    only transport (spec §5).
  - **Audit every positional `pdcch_blind_monitor_*` string.** For each key, read its parser
    (`grep -n '"<key>"' openair1/PHY/NR_UE_TRANSPORT/*.c openair1/PHY/NR_UE_ISAC/*.cc`) and write,
    as a comment above the key, what each colon field means. Any field that encodes a cell value
    (a DCI length, TDA, CORESET, RNTI or bandwidth) is set to its "auto/derive" value as documented
    by the parser. List each changed field in the commit message. A thread/queue/core count is not
    a cell value and stays.
  - `run_arm.sh` also passes `--ntn-initial-time-drift -4.25` and `-A 90`. The first is the
    X410's own measured oscillator drift, a receiver property, so it stays. Look up what `-A 90`
    is in `executables/nr-uesoftmodem.h` and note it in the commit; if it encodes a cell value,
    make it overridable and pass the neutral value.

- [ ] **Step 2: Fix run_arm's tree**

Run: `cd $W && sed -i 's#^cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets || exit 1#cd "$REPO/cmake_targets" || exit 1#' tests/passive_rx/captures/run_arm.sh && grep -n '^cd ' tests/passive_rx/captures/run_arm.sh`
Expected: `cd "$REPO/cmake_targets" || exit 1`. Confirm `REPO` is assigned before that line
(`grep -n 'REPO=' ...`); if it is assigned only inside `preflight()`, add `export REPO` there.

- [ ] **Step 3: Launcher**

```bash
#!/bin/bash
# tests/passive_rx/run_sensing.sh -- ONE command: preflight, 30 s radio wait, passive receiver + sensing
# engine (in-process), realtime chain, monitor; supervised; verdict at the end. Spec §12.
set -uo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
DUR=600 RXG=43 SURVEY=$W/tests/passive_rx/ota/survey.json DEBUG= PORT=8080 RUN=/home/sens/NICOLA/sensing_runs/$(date +%Y%m%d_%H%M%S)
while [ $# -gt 0 ]; do case $1 in
  --dur) DUR=$2; shift;; --rxg) RXG=$2; shift;; --survey) SURVEY=$2; shift;; --debug) DEBUG=1;;
  --port) PORT=$2; shift;; --run-dir) RUN=$2; shift;; *) echo "unknown arg $1"; exit 2;; esac; shift; done
BUILD=$W/cmake_targets/ran_build/build_sense
SENSE_CPUS=${SENSE_CPUS:-3,12,13} CHAIN_CPU=${CHAIN_CPU:-11} MON_CPU=${MON_CPU:-10}
mkdir -p "$RUN" && cd "$RUN" || exit 2
log(){ echo "[run_sensing $(date +%T)] $*" | tee -a "$RUN/launcher.log"; }

# ---- preflight (refuse on any failure) ----
pgrep -x nr-uesoftmodem >/dev/null && { log "ABORT: a receiver is already running"; exit 3; }
pgrep -f 'ninja|cmake --build|make -j' >/dev/null && { log "ABORT: a build is running (never build during a capture)"; exit 3; }
ping -c1 -W2 192.168.20.2 >/dev/null || { log "ABORT: X410 data plane 192.168.20.2 unreachable"; exit 3; }
timeout 20 uhd_find_devices --args "type=x4xx,addr=192.168.20.2" 2>/dev/null | grep -q x4xx || { log "ABORT: uhd_find_devices does not see the X410"; exit 3; }
[ -x "$BUILD/nr-uesoftmodem" ] || { log "ABORT: no sensing build at $BUILD"; exit 3; }
strings "$BUILD/nr-uesoftmodem" | grep -q 'SENSING_GATE open' || { log "ABORT: binary lacks the sensing gate (stale build?)"; exit 3; }
python3 "$W/tests/passive_rx/survey.py" "$SURVEY" --geometry "$RUN/geometry.json" \
  --apply "$W/tests/passive_rx/ota/sensing_ota.conf.template" "$RUN/ue.conf" --report-path "$RUN/reports.jsonl" \
  || { log "ABORT: survey rejected (see above)"; exit 3; }
log "preflight ok; run dir $RUN"

# ---- 30 s radio wait (run_arm itself waits 180 s more if it has to restart MPM) ----
log "waiting 30 s for the radio"; sleep 30

# ---- receiver (+ in-process engine) through the qualified run_arm harness ----
export NR_ISAC_CPUS=$SENSE_CPUS NR_ISAC_REQUIRE_CUDA=1
[ -n "$DEBUG" ] && { mkdir -p "$RUN/debug"; export NR_ISAC_DEBUG_DIR=$RUN/debug; }
( REPO=$W BIN=$BUILD/nr-uesoftmodem ARM=sense CONF=$RUN/ue.conf DUR=$DUR TRIES=${TRIES:-3} RXG=$RXG NANT=4 \
  SCAN=1 PRB=273 CARRIER=3450000000 INITIALFO=0 \
  XENV="NR_ISAC_CPUS=$SENSE_CPUS NR_ISAC_REQUIRE_CUDA=1 ${NR_ISAC_DEBUG_DIR:+NR_ISAC_DEBUG_DIR=$NR_ISAC_DEBUG_DIR}" \
  bash "$W/tests/passive_rx/captures/run_arm.sh" ) > "$RUN/run_arm.out" 2>&1 &
ARM_PID=$!
# run_arm writes captures under its BASE; find this run's newest run.log once it appears
for _ in $(seq 1 600); do RL=$(ls -1t /home/sens/NICOLA/captures/sense_*/run.log 2>/dev/null | head -1); \
  [ -n "$RL" ] && [ "$RL" -nt "$RUN/ue.conf" ] && break; sleep 1; done
[ -n "${RL:-}" ] || { log "ABORT: receiver never produced a run.log"; kill $ARM_PID; exit 4; }
ln -sfn "$(dirname "$RL")" "$RUN/capture"; log "receiver log $RL"

# ---- realtime chain + monitor ----
taskset -c $CHAIN_CPU python3 "$W/openair1/PHY/NR_UE_ISAC/tools/realtime_chain.py" --follow "$RUN/reports.jsonl" \
  --geometry "$RUN/geometry.json" --out "$RUN/tracks.jsonl" --status "$RUN/status.jsonl" \
  ${DEBUG:+--debug-dir "$RUN/debug/chain"} > "$RUN/chain.out" 2>&1 &
CHAIN_PID=$!
taskset -c $MON_CPU python3 "$W/tests/passive_rx/monitor/monitor.py" --port "$PORT" --reports "$RUN/reports.jsonl" \
  --tracks "$RUN/tracks.jsonl" --status "$RUN/status.jsonl" --geometry "$RUN/geometry.json" --log "$RL" > "$RUN/monitor.out" 2>&1 &
MON_PID=$!
log "monitor: ssh -L $PORT:localhost:$PORT sens6  then  http://localhost:$PORT/"

stop_all(){ log "stopping"; sudo pkill -TERM -x nr-uesoftmodem 2>/dev/null; wait $ARM_PID 2>/dev/null
  sleep 3; kill $CHAIN_PID $MON_PID 2>/dev/null; }
trap 'stop_all; verdict; exit 130' INT TERM

verdict(){ python3 - "$RUN" "$RL" <<'EOF'
import json, os, re, sys
run, rl = sys.argv[1], sys.argv[2]
log = open(rl, errors="replace").read() if os.path.exists(rl) else ""
reps = [json.loads(l) for l in open(f"{run}/reports.jsonl")] if os.path.exists(f"{run}/reports.jsonl") else []
last = reps[-1] if reps else {}
v = {"cpis": len(reps), "gate_opened": "SENSING_GATE open" in log,
     "cuda": bool(re.search(r"CUDA.*(enabled|warm)", log)), "cfo_mislock": os.path.exists(os.path.join(os.path.dirname(rl), "cfo_mislock")),
     "dropped_cpis": last.get("dropped_cpis"), "discarded_pending_rows": last.get("discarded_pending_rows"),
     "dropped_submissions": last.get("dropped_submissions"),
     "arm_verdict": open(os.path.join(os.path.dirname(rl), "verdict.txt")).read().strip() if os.path.exists(os.path.join(os.path.dirname(rl), "verdict.txt")) else None}
v["valid"] = bool(v["gate_opened"] and v["cpis"] > 0 and not v["cfo_mislock"]
                  and not (v["dropped_cpis"] or v["discarded_pending_rows"] or v["dropped_submissions"]))
json.dump(v, open(f"{run}/run_verdict.json", "w"), indent=1); print(json.dumps(v))
EOF
}

# ---- supervision: death AND live-but-silent ----
T0=$(date +%s); LAST_SIZE=0; LAST_GROWTH=$T0
while kill -0 $ARM_PID 2>/dev/null; do
  sleep 10; now=$(date +%s); size=$(stat -c%s "$RL" 2>/dev/null || echo 0)
  [ "$size" -gt "$LAST_SIZE" ] && { LAST_SIZE=$size; LAST_GROWTH=$now; }
  [ $((now - LAST_GROWTH)) -gt 60 ] && log "WARN: receiver log silent for $((now - LAST_GROWTH)) s"
  kill -0 $CHAIN_PID 2>/dev/null || log "WARN: realtime_chain died (see chain.out)"
  kill -0 $MON_PID 2>/dev/null || log "WARN: monitor died (see monitor.out)"
done
sleep 3; kill $CHAIN_PID $MON_PID 2>/dev/null
verdict; log "done: $RUN"
```

- [ ] **Step 4: Supervisor self-test without the radio.** `--dry` is not built in (YAGNI).
  Verify the preflight refusals instead:

```bash
cd $W && bash tests/passive_rx/run_sensing.sh --survey tests/passive_rx/ota/survey.json --run-dir /tmp/rs_t1; echo "exit=$?"
```
Expected, depending on the rig state:
- X410 unplugged (current state): `ABORT: X410 data plane ... unreachable`, `exit=3`.
- X410 up with the all-zero template survey: `survey: ch0 and ch1 are co-located`,
  `ABORT: survey rejected`, `exit=3`.
Both refusals must happen BEFORE the 30 s wait.

- [ ] **Step 5: Commit**

```bash
cd $W && chmod +x tests/passive_rx/run_sensing.sh && git add tests/passive_rx/run_sensing.sh \
  tests/passive_rx/ota/sensing_ota.conf.template tests/passive_rx/captures/run_arm.sh && git commit -q -m "passive rx: one-command sensing run (preflight, 30 s radio wait, receiver+engine, chain, monitor, verdict)

run_arm.sh now cds into REPO's cmake_targets instead of a hardcoded other tree, so the plugins
loaded are the ones built with the binary under test.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 17: OTA validation (needs the X410 back, the survey filled, and the lab gNB up with 2 UEs + iperf)

**Files:**
- Create: `validation_runs/multirx_sensing_2026-09-XX/NOTES.md` (results; outside the repo, under
  `/home/sens/NICOLA`)

- [ ] **Step 0: Rig checks, in this order; stop at the first failure and report it.**
  1. **X410:** `ping -c1 192.168.20.2 && timeout 20 uhd_find_devices --args type=x4xx,addr=192.168.20.2`.
  2. **RF path:** `/usr/local/lib/uhd/examples/benchmark_rate --args "addr=192.168.20.2,type=x4xx" --rx_rate 122880000 --channels 0,1,2,3 --duration 10`.
     Expected: 0 overruns, 0 drops.
  3. **gNB alive and scheduling:** on sens4, `pgrep -a gnb` → read `-c`, then the yaml's
     `log: filename:`. `stat -c%s` that log twice, 5 s apart; it must grow. Re-read the current
     C-RNTIs there (they change on every re-attach).
  4. **Survey:** `tests/passive_rx/ota/survey.json` filled by the user; `survey.py` accepts it.

- [ ] **Step 1 (DEFERRED by the user 2026-09-23: lab gNB unavailable; do not run until asked): 4-channel CORESET finding, with the CURRENT adaptive receiver** (the user's
  item 3; before anything about sensing). Same rig, same config, alternating arms, `DUR=180`, 2
  runs per arm:

```bash
cd /home/sens/NICOLA/adaptive-rx-UL-DL/tests/passive_rx/captures
for arm in n1 n4 n1 n4; do N=${arm#n}; REPO=/home/sens/NICOLA/adaptive-rx-UL-DL ARM=coreset_$arm CONF=/home/sens/NICOLA/captures/agnostic_ota.conf \
  DUR=180 TRIES=2 RXG=43 NANT=$N SCAN=1 PRB=273 CARRIER=3450000000 INITIALFO=0 bash run_arm.sh; sleep 60; \
  ping -c1 192.168.20.2 && timeout 20 uhd_find_devices --args type=x4xx,addr=192.168.20.2 | grep -c x4xx; done
```
Per run, score:
- **Common (CORESET#0):** `SIB1 decoded` count and SI-RNTI hits (`grep -c 'SICENSUS\|SIB1 decoded' run.log`).
- **Dedicated:** DCI 1_1 and 0_1 lengths locked (`grep 'dci_length locked\|dci01\[accepts'`), the
  C-RNTIs recovered compared with the gNB log, PDSCH CRC (`pdsch_decode[try=`) and PUSCH CRC
  (`pusch_passive[try=`).
- **Verdict:** is the 2026-09-15 finding (0 SI hits at 4 RX) reproduced or refuted on the current
  code? Record `BRANCHFO d_vs_br0` from the n4 runs.
- **If n4 reproduces the failure:** STOP the validation here and report. Sensing at 4 channels is
  blocked on the receiver, not on this work.

- [ ] **Step 2: Sensing smoke run**

Run: `cd /home/sens/NICOLA/multirx-clean-adaptive && bash tests/passive_rx/run_sensing.sh --dur 300 --debug`
Check (every number from the run's own files):
- `SENSING_GATE open` appears only after both DCI 1_1 and 0_1 decode for one RNTI.
- `SENSING_GATE stats` shows `admitted > 0`.
- `BRANCHFO d_vs_br0` channels 1–3 converge to |·| < 20 Hz.
- The log shows the CUDA backend enabled, and `run_verdict.json` has `"cuda": true`.
- `dropped_cpis`, `discarded_pending_rows` and `dropped_submissions` are all 0.
- The monitor shows 4 DL maps, per-UE UL maps and a rotatable 3D view.
- `LLRCONF qm=.. calibrated=1` for at least the UL Qm, with `crc_ok_bit_agreement` > 0.95.
- `chain.out`: `per_cpi_ms.p95` below the CPI period, 75 ms (spec §5; above it means a stage port
  is needed).

- [ ] **Step 3: Gate close.** During a run, stop iperf on both UEs for 10 s.
  Expected: `SENSING_GATE close` within ~2 s of the last dedicated grant, `gate_discarded_rows`
  rises, the gate re-opens when traffic resumes, and no report has a CPI spanning the silence.

- [ ] **Step 4: Replay parity.** `isac_replay -O $RUN/ue.conf --rows $RUN/debug/cfr_rows.bin`,
  written to a fresh `report_path` (edit a copy of the conf). Compare with the live
  `reports.jsonl`:
  - the same CPI count,
  - per CPI, the same number of detections,
  - every detection's range and rate within 1 % of a resolution cell.
  CUDA reduction order may differ, so no bitwise equality is expected.

- [ ] **Step 5: Non-interference A/B.** Arms: `$BOFF` (sensing OFF) vs `$BON` (sensing ON),
  alternating, **≥ 5 runs per arm**, `DUR=300`, same rig and traffic.
  - For the OFF arm use `run_arm.sh` directly with `REPO=$W BIN=$BOFF/nr-uesoftmodem`, since the
    launcher refuses a binary without the gate.
  - Score each run: DCI accepts/s, PDSCH and PUSCH CRC rate, NIC `rx_missed_errors` delta, and
    the run_arm verdict. Discard runs marked VOID.
  - Welch t-test per metric. Acceptance: no significant drop (p > 0.05, or a difference below
    one run-to-run SD).

- [ ] **Step 6: Record results.** Write `NOTES.md` with every number above and its source file.
  Update the project `CLAUDE.md` "Implemented so far" with a short entry and the caveat:
  OTA without ground truth validates plumbing, timing and non-interference, not track accuracy.
  Commit the CLAUDE.md change in the repo it lives in, if any; `/home/sens/NICOLA/CLAUDE.md` is
  not in a git repo, so edit it in place.
