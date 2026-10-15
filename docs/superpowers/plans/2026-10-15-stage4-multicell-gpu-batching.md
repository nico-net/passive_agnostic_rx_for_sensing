# Stage 4 — Multi-cell GPU sharing and batching (multi-PCI) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two or more cells (≥ 2 operators) decode simultaneously with each cell keeping its single-cell decode latency (Stage 1 targets) and CRC-OK, on the DGX Spark (GB10, 128 GB shared memory) and with at least two cells on sens6 (RTX 4070, 12 GB VRAM).

**Architecture:** Today each cell is its own process with its own CUDA context, so the driver time-slices whole contexts and RT work of different cells is not coordinated (green contexts do not isolate across processes; MPCIFIX-20261015 §2). Stage 4 first measures and cuts per-cell GPU memory, then evaluates the cheap option (CUDA MPS with per-client SM limits and a cross-process RT arbiter) against the structural option (one process hosting N cells sharing one context and batching the same-slot grants of all cells into one Stage-1 graph launch). The structural option is implemented only if MPS cannot meet the latency target; the decision is a gate in Task 3.

**Tech Stack:** C11, CUDA graphs (Stage 1), CUDA MPS (`nvidia-cuda-mps-control` with a private pipe dir), POSIX shm (`nr_mpci_sched`), Python (`mpci_auto.py`), gtest.

**Spec:** `docs/superpowers/reports/csd/MPCI-DASH-REVIEW-20261015.md` (branch csd/mpci-int 2e2d7de40c) and `docs/superpowers/reports/csd/MPCIFIX-20261015.md` (branch csd/mpcifix 9ece4808aa); SYSAN-20261015.md §10 ("multi-operator probably needs the fused design or a separate discovery host"); memory `gpu-decode-pipeline-plan`, `single-operator-first` (multi-PCI is merged into csd/otatest only after single-operator decoding is solved). Prerequisites: Stage 1 (GPU pass) and Stage 3 (background lane) merged into the branch used here.

## Global Constraints

- CLAUDE.md hard rules verbatim (frozen sens6 paths, `.cfg`, gNB ground truth only, evidence labels naming host and cells, never build while one of our receivers runs, SIGINT only, `ISAC_RX_BRANCH_FO` unset, explicit `git add`, never stash).
- Work on branch `csd/mpcifix` (or a child); never merge into csd/otatest without the user's go.
- Per-cell results never pooled across cells or operators; every table names PCI and centre frequency.
- Free RAM never below 16 GB (`mpci_auto.py` admission already enforces it; keep it).
- MPS: never use the system/root MPS daemon of another user; always a private `CUDA_MPS_PIPE_DIRECTORY` and `CUDA_MPS_LOG_DIRECTORY` under `/tmp/nicola_mps_<tag>`; stop it (`echo quit | nvidia-cuda-mps-control`) at the end of every run.
- Single-cell regression: with one cell, the multi-cell build must equal the Stage-1 single-cell results (same CRC-OK within spread, same p50/p99 within 5 %).
- Targets per cell (2 cells, DGX, daytime): DL whole-call p50 ≤ 1.2 × single-cell p50 and p99 ≤ 1.5 × single-cell p99; UL locked p50 ≤ 1.2 × single-cell; 0 shm overruns; CRC-OK ≥ 95 % of single-cell. sens6: 2 cells ≤ 106 PRB each fit in 12 GB VRAM with ≥ 1 GB headroom.

## Review Focus

- GPU memory exhaustion when a new cell starts (cudaMalloc fails at init): expected → that cell refuses to start with a clear `MPCI_ADMIT gpu_mem` log line, other cells unaffected; `mpci_auto.py` must have predicted it. Test in Task 1 and Task 2.
- One cell's burst (e.g. 30 grants in one slot) next to a quiet cell: expected → the quiet cell's p99 stays within target (RT arbitration fairness), the busy cell degrades first. Test in Task 4 (and Task 6 for the batched path).
- MPS daemon dies mid-run: expected → clients get errors; each cell process exits cleanly (SIGINT path), no X410 claim leak, publisher keeps running; restart script recovers. Test in Task 3.
- Cells with different numerology/bandwidth in one batch: expected → never batched together (batch key includes SCS, N_RB class, shape class); counted as separate launches. Test in Task 6.
- One cell's receiver crash in the single-process design: expected → only that cell's state is torn down (per-cell arenas), others continue. Test in Task 6.

---

## File structure

| file | responsibility |
|---|---|
| `openair1/PHY/TOOLS/nr_gpu_memstat.h/.c` (new) | per-module GPU allocation accounting (bytes per tag), dump line `GPUMEM` |
| `openair1/PHY/NR_UE_TRANSPORT/*gpu*` (modify) | tag every `cudaMalloc`/`cudaHostAlloc`/`nr_gpu_buf_alloc` with a module name; size pools from the cell's N_RB instead of max |
| `tests/passive_rx/mpci/mpci_mps.sh` (new) | start/stop a private MPS daemon, per-client `CUDA_MPS_ACTIVE_THREAD_PERCENTAGE` |
| `openair1/PHY/TOOLS/nr_mpci_sched.c/.h` (modify, from csd/mpcifix) | cross-process RT arbitration (slot-deadline ordering), not only discovery slices |
| `tests/passive_rx/mpci/mpci_auto.py` (modify) | GPU-memory admission from `GPUMEM` measurements; MPS option; sens6 profile |
| `executables/nr-uesoftmodem-mc` path (decision-gated, Task 5-6) | single process with N cell instances sharing one CUDA context; cross-cell batched Stage-1 graph |

---

### Task 1: Per-module GPU memory accounting and per-cell sizing

**Files:** Create `openair1/PHY/TOOLS/nr_gpu_memstat.h/.c`; modify the allocators in `T/nr_pdsch_gpu_fep.cu` (8 × `g_ctx_ls` 2048×4096×4 B, LLR 32 M int16, `g_d_ls`/`g_d_gold`, cuFFT plan), `T/nr_pdsch_gpu_demod.cu` (8 contexts), `T/nr_pdsch_gpu_ldpc.cu` (8 DL ctx + UL pool), `T/nr_csirs_disc_gpu.cu` + `T/nr_csirs_disc_feed.c:469-475` (ring), `T/nr_pdcch_nid_gpu.c` (staging), `openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:729`, `ldpc_cb0.cu:479-562`, `T/nr_td_cb0_batch.cu:98-102`, Stage-1 `nr_gpu_mem.c` — each call goes through `nr_gpu_memstat_add(tag, bytes)`. Size the LLR buffer, `g_ctx_ls` and the CSI-RS ring from the cell's `N_RB_DL` and `nb_antennas_rx` instead of the 273-PRB maximum (env override `ISAC_GPU_SIZE_MAXRB=1` keeps today's sizes).
**Interfaces:** `void nr_gpu_memstat_add(const char *tag, size_t bytes); void nr_gpu_memstat_dump(char *buf, int n); /* "GPUMEM total=… fep_ls=… llr=… demod=… ldpc=… csirs_ring=… nid=… cb0=… ctx_overhead=…" */`
- [ ] **Step 1: Failing test** `openair1/PHY/TOOLS/tests/nr_gpu_memstat_test.cc`:

```cpp
TEST(GpuMemstat, SumsByTag) {
  nr_gpu_memstat_reset_for_test();
  nr_gpu_memstat_add("llr", 100); nr_gpu_memstat_add("llr", 50); nr_gpu_memstat_add("ldpc", 7);
  char b[256]; nr_gpu_memstat_dump(b, sizeof b);
  EXPECT_NE(nullptr, strstr(b, "llr=150")); EXPECT_NE(nullptr, strstr(b, "ldpc=7")); EXPECT_NE(nullptr, strstr(b, "total=157"));
}
TEST(GpuSizing, LlrBufferScalesWithNrb) { EXPECT_LT(nr_pdsch_gpu_llr_bytes_for(51, 4), nr_pdsch_gpu_llr_bytes_for(217, 4)); EXPECT_EQ(nr_pdsch_gpu_llr_bytes_for(273, 4), nr_pdsch_gpu_llr_bytes_maxrb()); }
```
- [ ] **Step 2-4:** FAIL → implement → PASS; full ctest only known ARM failures.
- [ ] **Step 5:** Replay one cell (b, 217 PRB; c3610 51-PRB cell) with and without `ISAC_GPU_SIZE_MAXRB=1`, record `GPUMEM` and `nvidia-smi --query-compute-apps=used_memory` (DGX: `cat /proc/<pid>/status | grep -E 'VmRSS|RssAnon'` since memory is unified) in `docs/superpowers/reports/csd/MCGPU-<date>.md`; same CRC-OK both ways. Commit with explicit paths: `git add openair1/PHY/TOOLS/nr_gpu_memstat.h openair1/PHY/TOOLS/nr_gpu_memstat.c openair1/PHY/TOOLS/tests/nr_gpu_memstat_test.cc <the modified allocator files> docs/superpowers/reports/csd/MCGPU-<date>.md && git commit -m "feat(mcgpu): per-module GPU memory accounting and N_RB-sized pools"`

### Task 2: GPU-memory admission in `mpci_auto.py` (DGX + sens6 profiles)

**Files:** Modify `tests/passive_rx/mpci/mpci_auto.py` (RAM admission at lines ~121-147: add `gpu_mem_per_cell(nrb, nant)` from Task 1 measurements, profile `--host dgx|sens6` with `vram_bytes` = unified MemAvailable (dgx) or 12 GiB − 1 GiB headroom (sens6)); its tests `tests/passive_rx/mpci/test_mpci_auto.py`.
- [ ] **Step 1: Failing tests**

```python
def test_sens6_two_cells_106prb_fit():
    plan = mpci_auto.plan(cells=[cell(106), cell(106)], host="sens6")
    assert [c.status for c in plan.cells] == ["served", "served"]
    assert plan.gpu_mem_used <= 11 * 2**30

def test_sens6_two_217prb_one_refused():
    plan = mpci_auto.plan(cells=[cell(217), cell(217)], host="sens6")
    assert [c.status for c in plan.cells].count("unserved") == 1
    assert plan.unserved[0].reason == "gpu_mem"
```
- [ ] **Step 2-4:** `python3 -m pytest tests/passive_rx/mpci/test_mpci_auto.py -q` FAIL → implement → PASS (35 existing tests still pass).
- [ ] **Step 5:** `git add tests/passive_rx/mpci/mpci_auto.py tests/passive_rx/mpci/test_mpci_auto.py && git commit -m "feat(mpci): GPU-memory admission with dgx/sens6 profiles"`

### Task 3: MPS option and the decision gate

**Files:** Create `tests/passive_rx/mpci/mpci_mps.sh`:

```bash
#!/bin/bash
# usage: mpci_mps.sh start <tag> | stop <tag>   — private MPS daemon, never the system one
set -e
D=/tmp/nicola_mps_$2; mkdir -p "$D"
export CUDA_MPS_PIPE_DIRECTORY=$D CUDA_MPS_LOG_DIRECTORY=$D
case $1 in
  start) nvidia-cuda-mps-control -d; echo "MPS up at $D";;
  stop)  echo quit | nvidia-cuda-mps-control || true; rm -rf "$D";;
esac
```
Modify `tests/passive_rx/mpci/ota_mpci.sh` and `fanout_replay.sh`: option `MPS=1` → start the daemon, export the same pipe dir to every cell, set per-cell `CUDA_MPS_ACTIVE_THREAD_PERCENTAGE` from `mpci_auto.py` (`plan.cells[i].sm_pct`), stop it in the exit trap.
- [ ] **Step 1:** `bash -n tests/passive_rx/mpci/mpci_mps.sh` and a dry-run test in `test_mpci_auto.py` that `plan(..., mps=True)` assigns `sm_pct` summing to ≤ 100.
- [ ] **Step 2:** Replay matrix (fanout_replay.sh, captures c3610 = WindTre 508 + Iliad 403, and b+tile3 through the file publisher), arms: `SEP` (separate processes, today), `MPS` (separate processes + private MPS + per-client SM %), each with Stage 1 + Stage 3 flags on, n ≥ 4 per arm. Measure per cell: DL whole-call p50/p99, UL p50, CRC-OK, overruns, `GPUMEM`.
- [ ] **Step 3: DECISION GATE.** If `MPS` meets the Global-Constraints targets for 2 cells on the DGX → skip Tasks 5-6 (record "MPS adopted" in the report) and go to Task 4 + 7. Otherwise → Tasks 5-6 (single process).
- [ ] **Step 4:** Robustness check (Review Focus 3): kill the MPS daemon (SIGTERM to `nvidia-cuda-mps-server` of our private dir only) during a replay → each cell exits via SIGINT path, publisher survives, `/dev/shm` cleaned by `ota_mpci.sh` trap.
- [ ] **Step 5:** Report section "MPS vs separate" in `MCGPU-<date>.md`; commit `git add tests/passive_rx/mpci/mpci_mps.sh tests/passive_rx/mpci/ota_mpci.sh tests/passive_rx/mpci/fanout_replay.sh tests/passive_rx/mpci/test_mpci_auto.py docs/superpowers/reports/csd/MCGPU-<date>.md && git commit -m "feat(mpci): private MPS option with per-cell SM share; MPS vs separate contexts measured"`

### Task 4: Cross-process RT arbitration (`nr_mpci_sched`)

**Files:** Modify `openair1/PHY/TOOLS/nr_mpci_sched.c/.h` (csd/mpcifix: today only discovery slices; `rt_inflight` published, never throttled, nr_mpci_sched.c:147), `openair1/PHY/TOOLS/nr_gpu_budget.c` (`rt_begin` asks the arbiter for a launch slot ordered by slot deadline); known issue O6 (busy refusals without back-off) fixed with exponential back-off capped at 50 µs.
**Interfaces:** `int nr_mpci_rt_admit(int cell, uint64_t deadline_ns); /* 1 = launch now, 0 = wait (caller sleeps ≤ 20 us and retries) */ void nr_mpci_rt_done(int cell);` — earliest-deadline-first across cells, max 2 RT passes in flight per cell, max 4 total (env `ISAC_MPCI_RT_MAX`).
- [ ] **Step 1: Failing tests** `openair1/PHY/TOOLS/tests/nr_mpci_sched_test.cc`:

```cpp
TEST(MpciRt, EarliestDeadlineFirst) {
  nr_mpci_test_init(2);
  ASSERT_EQ(1, nr_mpci_rt_admit(0, 1000)); ASSERT_EQ(1, nr_mpci_rt_admit(0, 1100));  // cell 0 at its cap (2)
  EXPECT_EQ(0, nr_mpci_rt_admit(0, 1200));                                            // cap
  EXPECT_EQ(1, nr_mpci_rt_admit(1, 1300));                                            // other cell still admitted
}
TEST(MpciRt, BusyCellCannotStarveQuietCell) { /* cell 0 submits 1000 passes, cell 1 one pass every 100: cell 1 always admitted within 1 retry */ }
TEST(MpciRt, BackoffBounded) { /* refused admits retry with back-off; never > 50 us between tries */ }
```
- [ ] **Step 2-4:** FAIL → implement (shm segment layout versioned; old readers refuse a new version) → PASS.
- [ ] **Step 5:** Replay burst test (one cell fed 3× its grants) → quiet cell p99 within target; commit `git add openair1/PHY/TOOLS/nr_mpci_sched.c openair1/PHY/TOOLS/nr_mpci_sched.h openair1/PHY/TOOLS/nr_gpu_budget.c openair1/PHY/TOOLS/tests/nr_mpci_sched_test.cc && git commit -m "feat(mpci): EDF cross-cell RT arbitration with bounded back-off"`

### Task 5 (only if the Task 3 gate fails): single process, N cells, one CUDA context

**Files:** Modify `common/utils/nr_cell_instance.h` (csd/mpcifix: `ISAC_CELL_INSTANCE` makes paths per-cell) to support `ISAC_CELL_INSTANCES=tagA,tagB` in one process: per-cell `PHY_VARS_NR_UE`, per-cell queues/threads (PDCCH scan, PDSCH/PUSCH consumers), shared GPU pools (Stage-1 graph cache, LDPC contexts) with per-cell arenas for state; `executables/nr-uesoftmodem.c` starts N RU readers on the shm rings (`ISAC_SHM_IQ_NAME_<tag>`).
- [ ] **Step 1: Failing test** `openair1/PHY/NR_UE_TRANSPORT/tests/nr_multicell_test.cc`: two synthetic cells (different PCI, N_RB 51 and 106) fed from two in-memory rings in one process decode exactly what each decodes alone (same TB bytes per grant), and stopping cell A leaves cell B decoding.
- [ ] **Step 2-4:** FAIL → implement → PASS; one-cell regression = Stage-1 results.
- [ ] **Step 5:** commit with explicit paths, message `feat(mc): N cells in one process sharing one CUDA context (ISAC_CELL_INSTANCES)`.

### Task 6 (only if the Task 3 gate fails): cross-cell batched graph launch

**Files:** Modify Stage-1 `nr_gpu_pass_graph.cu`: batch key = (SCS, N_RB class, shape class); a per-slot collector gathers ready jobs of all cells for ≤ `ISAC_MC_BATCH_WAIT_US` (default 30) and launches one graph with K job slots (graphs recorded for K = 1, 2, 4); results demuxed per cell.
- [ ] **Step 1: Failing tests** in `nr_gpu_pass_test.cc`: `BatchOfTwoEqualsTwoSingles` (bit-equal TBs), `DifferentScsNeverBatched`, `CollectorNeverWaitsPastBudget` (job latency ≤ wait + single pass).
- [ ] **Step 2-4:** FAIL → implement → PASS.
- [ ] **Step 5:** commit `feat(mc): cross-cell batched GPU pass per slot`.

### Task 7: OTA validation (DGX daytime, then sens6) and report

- [ ] **Step 1:** DGX OTA with `ota_mpci.sh`: (a) WindTre 508 + Iliad 403 (or 249 if 403 is off air), (b) Vodafone 415 + TIM 923 at the window chosen by `mpci_auto.py` (expected 122.88 MS/s per MPCIFIX), each n ≥ 3, alternated with the single-process baseline of each cell; X410 claim hygiene: SIGINT only, `/dev/shm` cleaned, `flock -x heavy.lock` held for the whole window.
- [ ] **Step 2:** Per cell: DL/UL p50/p99 vs single-cell, CRC-OK ratio, overruns, `GPUMEM`, MemAvailable minimum; pass criteria from Global Constraints.
- [ ] **Step 3:** sens6 (RTX 4070): 2 cells ≤ 106 PRB through the file publisher replay (and OTA only if an SDR is attached to sens6 — ask the user), `GPUMEM` ≤ 11 GiB.
- [ ] **Step 4:** Final `MCGPU-<date>.md` with `[MEASURED, DGX OTA … DAYTIME, cells …]` and `[MEASURED, sens6 x86_64 RTX 4070 replay …]`.
- [ ] **Step 5:** Commit; do not merge into csd/otatest without the user's go (memory `single-operator-first`).

## Self-review notes

- Spec coverage: GPU memory per cell (MPCI review "13 GB + 11 GB per cell", sens6 12 GB) → Tasks 1-2; GPU time-slicing across processes / MPS decision (MPCIFIX §2) → Task 3; RT not coordinated across cells (nr_mpci_sched only arbitrates discovery) → Task 4; batching across cells (SYSAN §10) → Tasks 5-6 behind the gate; on-air validation → Task 7.
- Names consistent: `nr_gpu_memstat_add/_dump`, `nr_mpci_rt_admit/_done`, `ISAC_CELL_INSTANCES`, `ISAC_MC_BATCH_WAIT_US`.
