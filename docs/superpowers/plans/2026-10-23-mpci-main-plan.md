# mpci-main — Multi-PCI, single process, one CUDA context — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Date:** 2026-10-23. **Status:** plan only. No code, build, receiver run or hardware use was done to write it.
**Branch:** `mpci-main`, created from `adaptive-rx-UL-DL` @ `e9c874c308` (worktree `/home/nicola/NICOLA/wt/rr-mpci-main`). `adaptive-rx-UL-DL` stays the single-PCI main. `mpci-main` is the new main for multi-PCI: it receives `adaptive-rx-UL-DL` by merge (main into mpci-main), never the reverse, and the old multi-PCI branches are inputs ported onto it, never the base.

**Goal:** One `nr-uesoftmodem` process, one CUDA context, N cells (>= 2 PCIs and >= 2 operators at once) decoded from one wideband radio stream, each cell at the single-cell goals (DL p50 about 1 ms, p99 < 5 ms, ALL grants decoded including big, full-PRB and multi-layer ones, PDSCH payload never traded for latency), with the fewest cores, GPU-portable (DGX GB10 integrated, sens6 RTX 4060 Ti discrete), one config, no flags, no per-cell tuning, cells mainly NSA. 200 MHz first; 400 MHz only after 200 MHz works.

**Architecture (one paragraph):** The radio (usrp_lib, one X410 claim) feeds one wideband ring. An in-process channelizer (kernels ported from `csd/mpcifix`, re-hosted as a library and a thread inside the receiver, same CUDA context) produces one narrow ring per cell; a zero-shift cell reads the ring directly. Each cell is a `nr_cell_t` slot with its own receive-path state, selected by a thread-local cell index and cell-indexed statics. All CPU work of all cells leases from the ONE core pool (`common/utils/nr_core_pool.c`, already on main) with a per-cell fair share inside each priority class. All GPU work of all cells uses the ONE context, the existing graph pass instances (shared, with per-cell quotas), one discovery green context with a process-wide single-flight ticket, and a memory ledger that sizes per-cell state from grant-sized buffers. The core list and the SM partition are derived from the host (`nr_core_machine_read` / `nr_core_plan_build`, device SM query); no hardcoded cores or SM counts remain. The blind front end (separate agent, branch `csd/blind-front`) is the cell inventory; this plan only fixes its interface.

**Tech stack:** C11, pthreads, CUDA graphs and green contexts (existing), gtest/ctest, Python (planner model, scoring), OAI rfsimulator.

**Inputs read for this plan:** PROJECT_MEMORY.md (section 0.1, header, section 25 blocks 2026-10-17, 2026-10-18/19, 2026-10-20/21, 2026-10-22, multi-PCI greps); the four old plans (2026-10-13 multipci prompt, stage1, stage3, stage4); branches `csd/mpci-int` @ 2e2d7de40c, `csd/mpcifix` @ 9ece4808aa, `csd/mpci` @ 6437eb9f32, `csd/mcell-spec` @ a585d1bb98, `origin/csd/dashboard` @ 1040882805, `csd/int16-disctime` @ eb082ca709 (read-only, git log/diff/show/merge-tree); reports CORE-POOL-20261020, FLOAT-LANE-POOL-20261021, GRANT-SIZED-20261021, DISC-NONBLOCK-20261021, DISC-CONFIRM-20261021, DISC-GPU-TIMING-20261022, SENS6-READINESS-20261021, AUTOFRONT-20261012, FRONTLIVE-20261013, MPCI-OTA-20261016, MPCI-DASH-REVIEW-20261015, MPCIFIX-20261015.

Evidence labels are PROJECT_MEMORY section 0.1. Labels in this file: `[CODE-READ, mpci-main @e9c874c308]` = I read the tree or ran git; `[MEASURED, ...]`, `[OFFLINE VERIFIED, ...]`, `[Bench, ...]`, `[INFERRED]` are copied from the named report with its host and n; nothing here is a new measurement.

---

## Global Constraints

1. CLAUDE.md hard rules verbatim:
   - sens6 FROZEN: never edit `tests/passive_rx/captures/*`, `tests/passive_rx/*.conf`, `tests/passive_rx/sens6_host_snapshot_2026-09-30/*`. Gate: `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` before every commit.
   - New config files under `tests/passive_rx/` use `.cfg`.
   - gNB logs and configs are validation ground truth only, never receiver input. The cell inventory comes from the blind front end only.
   - Evidence labels on every claim, naming host (`cloud x86`, `DGX GB10`, `sens6 x86_64 RTX 4060 Ti`), cell (PCI and centre frequency), bandwidth. Never merge results across hosts, cells, operators or bandwidths: every table has one row per cell.
   - Never build while `nr-uesoftmodem` runs (`pgrep -x nr-uesoftmodem`, and on the shared DGX check the exe path, not `pgrep -af` in a loop). Stop receivers with SIGINT, never SIGKILL. `ISAC_RX_BRANCH_FO` stays unset. `git add <explicit paths>`; never `-A`, never stash.
   - Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
2. Branch rules: all work lands on `mpci-main` through task branches `mpci/<group>-<id>`. Nothing is pushed to `adaptive-rx-UL-DL`. `adaptive-rx-UL-DL` is merged into `mpci-main` (not the reverse) after each of its pushes, by the orchestrator, with the N=1 equivalence suite (Task A9) as the post-merge check. Pushing `mpci-main` and any old branch is the user's decision.
3. Goals the work is judged against (user 2026-10-21, 2026-10-16, PROJECT_MEMORY section 25 block 2026-10-20/21): DL p50 about 1 ms and p99 < 5 ms per cell; decode ALL grants, big and multi-layer first; PDSCH payload is used for sensing so CRC is never traded for latency; drops <= 3-4 % are acceptable if they save cores or GPU work; fewest cores; GPU-portable; one config, no flags, no per-cell tuning; cells mainly NSA (RRC/Msg4 never mandatory); single process, one CUDA context; 400 MHz only after 200 MHz works.
4. **No flags.** No new `ISAC_*` product flag. Cell count, layout, core list, SM split, pool sizes and budgets are derived from the host, the inventory and the admission model. Seams for tests use the `NR_TEST_` prefix, are documented in the task that adds them, and are never set in OTA env files. Defaults that need `setenv` go in the existing `--passive-rx` block of `executables/nr-uesoftmodem.c` (line ~294 pattern), not in env files.
5. **N=1 equivalence.** With one cell, `mpci-main` must equal `adaptive-rx-UL-DL` behaviour: same decisions on the same input (unit and replay level), and the single-cell regression `tests/passive_rx/dgx/rfsim_regress.sh` 4-RX gate unchanged (106 PRB `--ue-nb-ant-rx 4`, 420 s arms, idle host, `flock -x /tmp/td_measure.lock`; postconv 2 contexts / 0 reopens / CRC >= 99.8 over >= 5000 grants; ttc tda0 <= 39 s / tda2 <= 77 s; CRC floor 93.4; drop_full <= 2.5 %). All DGX tests use 4 RX.
6. **Per-cell acceptance (used by Section D gates).** Measured per cell, same session, same hour, interleaved with the solo run of that cell:
   - P1 relative: DL p50 <= 1.2 x solo, DL p99 <= 1.5 x solo, UL locked p50 <= 1.2 x solo, CRC_q >= 95 % of solo, big-grant CRC-OK (>= 8 CB or Nl >= 3) not below solo minus 2 points, drops <= 4 % and <= solo + 1 point. (Relative limits are the Stage 4 targets of `2026-10-15-stage4-multicell-gpu-batching.md`.)
   - P2 absolute: DL p50 about 1 ms, p99 < 5 ms, all grants decoded, drops <= 4 %. P2 is reported per cell and is binding for merging into a release branch; a cell whose solo run already misses P2 is judged on P1 and the miss is stated.
   - P3 cores: busy cores of the N=2 process <= 1.25 x the N=1 process (user 2026-10-17: two cells within today's single-cell core set plus about 25 %). Measured with the CORE_POOL line and `sysan_threads.py`.
7. **Partition by files** (Section "File ownership"): cloud tasks (group C) and DGX tasks (group G) never edit the same file. Shared headers are created by the cloud group first and changed only by the cloud group; the DGX group codes against them.
8. Host rules (DGX): builds and ctest under `flock -s`, rfsim and benchmarks under `flock -x /tmp/td_measure.lock` (measure-lock policy); one heavy job host-wide via the orchestrator's `heavy.sh`; CUDA programs with a private `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps` (another user's root MPS daemon hangs CUDA); replays with `ISAC_MLOCK=0`; MemAvailable >= 24 GB for replays, >= 48 GB for OTA. DGX is run by the orchestrator only; cloud and sens6 agents never touch the X410.
9. Build pitfall (PROJECT_MEMORY section 25, 2026-10-22 block 6, `[MEASURED, DGX]`): a fresh DGX build dir needs `-DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON "-DLDPC_CUDA_ARCH=89;121"`, `ninja -k 0`, and a `liboai_device.so` -> `liboai_usrpdevif.so` symlink. The cloud x86 build is `-DOAI_USRP=OFF -DENABLE_TESTS=ON` and `ninja nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests && ctest -j4`. Known ctest failures on main (block 7): `nr_td_sim_test`, `test_nr_ssb_rate_match_prod`, `test_nr_modulation`, `dft_test`, the `test_nr_pusch_ra0_*` family; `test_nr_gpu_pass` fails only under `ctest -j4` on the DGX. A new failure outside this list is a regression.

## Review Focus

Reviewers check these first; each states the expected behaviour and where it is tested.

1. **Shared statics between cells** (silent, wrong decodes). Expected: a CI audit fails on any file-scope mutable static in the receive path that is not classified PER_CELL / PROCESS / CONST, and the two-cell interleave test shows every module producing the same output for cell B whether or not cell A runs. Tasks A2, A3-A8.
2. **A burst in one cell starves the other** (CPU pool or GPU instances). Expected: cell B (quiet) keeps its solo wait p99 within 1.2 x while cell A runs 3 x its grants; work-conserving when B is idle. Tasks B1, G3.
3. **Discovery of N cells blocks real-time graphs.** Expected: process-wide single-flight discovery ticket; 0 `ULSTALL gpu_us > 100 ms`; RT launch gap bound shown by the launch-block bench with the 32-unit chunk floor at N=2. Task G4.
4. **GPU allocation after the RX fence.** The pass requires all allocations, streams, graph instantiations and first-touch before `nr_gpu_rx_started()` (Stage 1 constraint, `nr_dl_gpu_memory.h`). Expected: cells are activated into pre-sized slots, never allocated at runtime; a cell that does not fit is refused with a log line, the other cells are unaffected. Tasks B3, D1, G1.
5. **Channelizer overrun cascade** (MPCI-OTA 2026-10-16: 22 overruns, abort). Expected: 0 overruns over 10 min at 2 cells decoding, ring sized RAM-aware, a retune/discontinuity does not need a full re-sync. Tasks C1-C2, G5.
6. **Log and metrics attribution** (dashboard and scoring assume one cell). Expected: every receiver line is attributable to one cell; `goal_metrics.py` and the dashboard never mix cells. Task D2.
7. **Hardcoded DGX numbers on other hosts** (cores 13/15/16/2, 8 SMs, 32-SM split). Expected: a synthetic-topology test for DGX (20 cpus), sens6 (14 cpus, isolcpus 2-3) and a 4-cpu cloud host; `taskset` shrink is logged, never silent. Tasks B2, B4.
8. **N=1 drift.** Expected: every PR runs the N=1 equivalence suite; a diff of the N=1 CORE_POOL/summary lines vs `adaptive-rx-UL-DL` is explained or rejected. Task A9.

---

## A. Inventory of the old multi-PCI work (keep / port / rewrite / drop)

Base facts [CODE-READ, git]: `csd/mpci` (19 commits ahead of main-then, merge-base fcf6c6fc2e) is an ancestor of `csd/mpci-int` (35 ahead / 327 behind current main, merge-base 26d3028b5d), which is an ancestor of `csd/mpcifix` (45 ahead / 327 behind). `csd/mpcifix` is therefore the superset of the process-per-cell line. Trial merges into current main [CODE-READ, `git merge-tree`]: `csd/mpci` conflicts in `CMakeLists.txt`, `nr_pdcch_blind_monitor.c`, `nr_pdcch_coreset_map.c`, `nr_gpu_budget.c`, `phy_procedures_nr_ue.c`; `csd/mpcifix` adds conflicts in `nr_passive_acq_state.c`, its test, and the five monitor files (add/add against the always-on dashboard of main); `csd/int16-disctime` merges clean (2 new files). None of `csd/mpci`, `csd/mpci-int`, `csd/mpcifix`, `csd/mcell-spec` is on `origin` [CODE-READ, `git branch -r`]; `origin/csd/dashboard` and `csd/int16-disctime` are as stated.

Already on `mpci-main` (= main) [CODE-READ, mpci-main @e9c874c308], so NOT part of the port: core pool with `cell` argument and round-robin across cells inside a class (`nr_core_pool.h:8-9,44`, 16 cells); admission (`nr_admit`), `nr_gpu_mem`, grant-sized GPU work, polled discovery waits, 32-unit discovery chunk floor (`nr_gpu_chunk.h`), always-on dashboard (`tests/passive_rx/monitor`, `nr_dashboard.c`), blind-front tools (`tests/passive_rx/autofront`), `nr_core_plan.c` (with `nr_core_machine_read` / `nr_core_plan_build` implemented and unit-tested but called from no production code), `NB_UE_INST` in `nr-uesoftmodem.c` (OAI multi-UE arrays; the passive path uses instance 0 only).

Not on main and never implemented anywhere [CODE-READ, grep]: GPU memory accounting (`GPUMEM`/`memstat` of Stage 4 Task 1); per-cell receive-path state in one process; in-process channelizer; per-cell fair share in the pool (call sites pass cell 0 except the PDCCH scan, which passes `job.gNB_id`).

| # | Old piece (branch, files) | Decision | Reason |
|---|---|---|---|
| 1 | M1 shm IQ ring: `radio/COMMON/shm_iq_ring.[ch]`, `shm_iq.c` (consumer device), `shm_iq_pub.c`, tests `test_shm_iq.c` (csd/mpcifix) | **PORT** (transport abstraction) | Clean new files. Keep as the ring/consumer layer; add an anonymous-memory backing so the ring lives inside the process; keep POSIX shm backing for test tools (file publisher) and for a later out-of-process publisher. Consumer passband fix H3 (`num_rb_dl` x 12 x 30 kHz, MPCI-DASH-REVIEW) is kept. |
| 2 | `iq_ddc.[ch]` CPU channelizer incl. NEON/fused mixer + `tests/iq_ddc_ref.*` equivalence test (mpcifix) | **PORT** | Bit-exact vs frozen reference [OFFLINE VERIFIED, DGX aarch64, MPCIFIX]; becomes the CPU backend of the in-process channelizer (cloud tests, sens6 fallback). x86 build of NEON paths must be guarded (cloud task C2). |
| 3 | `shm_iq_chan.c` (channelizer process), `shm_iq_chan_gpu.cu/.h`, `shm_iq_chan_kern.h` (mpcifix, ABI 2: pinned slots, fused mixer+stage0, top stream priority) | **REWRITE host driver, PORT kernels and tests** | Separate process = separate CUDA context: time-sliced against the cells (7x slower under 2 live receivers; every context multiplies the decode body 4-9x) [MEASURED, DGX OTA 2026-10-16 daytime, n=1 per row, MPCI-OTA-20261016]. Kernels (bit-identical fused versions, idle 2 cells 245.76 MS/s = 4.37x real time [MEASURED, DGX GB10 bench, MPCIFIX]) are kept; the driver becomes a thread in the receiver, same context, stream/SM placement chosen by Task G5. |
| 4 | `shm_iq_uhd_pub.cpp`, `shm_iq_uhd_core.[ch]` (out-of-process UHD publisher, own X410 claim) | **DROP as product path; PORT the findings** | One process holds the radio via `usrp_lib`. Findings to re-check in `usrp_lib` at 245.76 MS/s: rxshift 2 (H1), frozen X410 timestamps (H2, `USRP_TS_FROZEN`), multi-streamer count mismatch must carry the excess, not restart (O4), chunk 131072 at 245.76 MS/s, ring default 2^27 RAM-aware (O3). Task G6 owns the re-check; nothing from the publisher is compiled into the receiver. |
| 5 | M2 `common/utils/nr_cell_instance.h` + `nr_cell_instance_test.cc` + hooks in `nr-ue.c`, `dci_nr.c`, `nr_passive_cfg_sources.c`, `nr_pdcch_coreset_map.c`, `nr_passive_acq_state.c`, `nr_passive_metrics*.c` | **REWRITE** (keep the path-tag header idea) | It isolates host-global paths per PROCESS via `ISAC_CELL_INSTANCE`; process-per-cell isolation of statics and TLS is exactly what is being given up. New API takes the cell index (`nr_cell_path(cell, def, out, n)`); header stays stateless; invalid tag stays fatal. Hunks in main-edited files are re-done by hand (conflicts). |
| 6 | M3 `nr_mpci_sched.[ch]` shm cross-process discovery arbiter + `nr_gpu_budget.c` hooks + `mpci_sched_stat.c` | **DROP** (keep the policy facts) | Cross-process mechanism; single process needs none. Facts kept: ACQUIRE-state cells get slices first; global discovery budget 30 % of wall time respected [MEASURED, DGX replay, n=1, MPCI-DASH-REVIEW 3.6]; "any other cell's RT in flight blocks discovery" grows with N and caused 7.25 M busy refusals (O6) so it is NOT reused. Replaced by the single-flight ticket (G4) and pool fairness (B1). |
| 7 | M4 multi-window planning in `mpci_auto.py` (dedicated/time-shared windows, per-ring LO) | **DROP for now** | Needed only for cells outside one window (400 MHz phase / second radio). Design text stays in `csd/mcell-spec`. |
| 8 | `mpci_auto.py` supervisor (plan/launch/pass2/watch/auto), `mpci_launch.sh`, `test_mpci_launch.py`, `test_mpci_auto.py` | **REWRITE** | Process launching is moot. Kept as logic: RAM admission with 16 GB floor, per-cell RAM/GPU cost model, ring-rate choice from measured channelizer capacity, pass-2 retune from SIB1. Re-homed in C (`nr_cell_admit`, `nr_cell_mgr`) with a Python model kept only as the offline planner used in tests. |
| 9 | Test tools: `shm_iq_file_pub.c`, `fanout_replay.sh`, `rfsim_shm_fanout.sh`, `ota_mpci.sh`, `mpci_runsum.py` | **PORT** (adapt) | File publisher is the cloud's multi-cell source (no radio, no captures needed with the mixer of C3). Scripts are adapted from "N receivers" to "one receiver, N cells"; `mpci_runsum.py` reads per-cell logs. |
| 10 | Dashboard multi-cell fixes: `1040882805` (= `f86ad494d1`): receiver-wide cards, per-cell solver count, multi-cell screenshots; `fa582777e6` (dashboard base) and `213cee5bbc` (report) | **PORT by hand** the multi-cell logic only; **DROP** the other two | Main already carries the dashboard (`restored/adapted` in stage2, newer than `fa582777e6`); trial merge conflicts add/add in `monitor.py`, `monitor.html`, `rxparse.py`, `test_monitor.py`, `isac_flags.json`. |
| 11 | mpci-int review fixes: H4 launcher args, H5 dashboard process attribution, M1 P99AGE parser | **DROP H4**, **CHECK H5/M1 on main**, **PORT only what is missing** | H4 is launcher-only. Main's dashboard is spawned by the receiver; verify H5 cannot recur. `rxparse.py` on main has no `P99AGE` match [CODE-READ, grep count 0]: port the parser (Task D2). |
| 12 | mpcifix: SIB1 sibling file written temp+rename (`nr_passive_acq_state.c`) | **PORT** | Main still uses `fopen("wb")` [CODE-READ, no `rename(` in `nr_passive_acq_state.c`]; a second cell reading the shared sibling file can see a truncated file (O9). Now two threads in one process: same race. Conflict file, hand port. |
| 13 | mpcifix: publisher side channel (`/dev/shm/<ring>.radio`, CHPOW/RFCHAN/RFSTALL into every cell's log) | **REWRITE small** | Radio facts are in the single process log; each cell's dashboard still needs the Radio tile. Task D2 mirrors the radio lines into every cell stream. |
| 14 | `csd/mcell-spec` (2 docs: design, plan; 400 MHz profile, F1-F8 gates, 12 points) | **KEEP** (cherry-pick the 3 doc commits) | Docs only, no code conflict. Governs the 400 MHz phase (after 200 MHz works). X410 facts there are `[CODE-READ]/[QUESTION]` until measured. |
| 15 | Stage 4 plan tasks (`2026-10-15-stage4-...`): T1 `GPUMEM` accounting | **PORT** (Task G1) | Never implemented. Needed for the per-cell ledger. |
| 16 | Stage 4 T2 `mpci_auto.py` GPU admission | **REWRITE** (Task B3) | Becomes `nr_cell_admit`. |
| 17 | Stage 4 T3 private MPS daemon + decision gate | **DROP** | The gate was answered by the 2026-10-16 decision (single process); MPS adds a second mechanism and the DGX has another user's root MPS daemon [shared-host memory]. |
| 18 | Stage 4 T4 cross-process EDF RT arbitration | **REWRITE** in-process (Task G3) | Per-cell instance quotas with usage-based fair share instead of cross-process shm. |
| 19 | Stage 4 T5 single process N cells (`ISAC_CELL_INSTANCES`) | **REWRITE** | This plan's groups A, D. The env var is a flag; replaced by inventory-driven slots. |
| 20 | Stage 4 T6 cross-cell batched graph launch | **DEFER** | Only if Task G7 shows per-cell graph launch overhead is the limit. Batch key (SCS, N_RB class, shape class) kept as the design. |
| 21 | Stage 3 background lane (`nr_bg_lane`, `nr_admit`) | **SUPERSEDED** | `nr_bg_lane_enter` is a no-op on main [CODE-READ, SENS6-READINESS 4.1]; the pool DISC/HOUSE classes carry the role. `nr_admit` stays. |
| 22 | Core pool (main, CORE-POOL-20261020) | **KEEP and EXTEND** | Add per-cell fair share (B1). Known fails carried over: `nested` > 0 (3 inline discovery passes), GPU waits hold the token [CODE-READ, CORE-POOL 3]. |
| 23 | FLOAT-LANE-POOL design (not implemented) | **PORT as design, implement** (Task B5) | The float lane has about 8 threads per cell (janon, nID, USS, dmrsScan, dmrsId, csirsDisc, ulBgDisc, UL NID) = 1.8-1.9 cores [INFERRED, FLOAT-LANE-POOL 1]; per cell this multiplies by N. Decision for mpci-main: they become pool DISC/HOUSE leases. |
| 24 | `csd/int16-disctime` bench `tests/passive_rx/gpuspan/disc_unit_bench.cu` + DISC-GPU-TIMING report | **PORT** (cherry-pick `eb082ca709`) | Merges clean; the bench is the base of the multi-cell RT-protection bench (G4). |
| 25 | `csd/int16-corepool` local WIP `8f03e06cbb` | **DROP** | Unbuilt/untested WIP; main has `5f5373ed94`-lineage code (merge `96a2097740`) [CODE-READ, PROJECT_MEMORY 10-20/21 item 5 and 10-22 block]. Do not quote numbers from it. |
| 26 | 2026-10-13 multipci prompt (M1-M4) | **SUPERSEDED** | M1 option (a) chosen (single process); M2 -> groups A/D; M3 -> B1/G3/G4; M4 -> deferred. |
| 27 | csd/blind-front (separate agent) | **INTERFACE ONLY** | Section B.7. No code in this plan touches `tests/passive_rx/autofront/*` or the front-end branch. |

---

## B. Architecture on the current main

### B.1 Process model

```
X410 (usrp_lib, 1 claim, 4 RX, 245.76 MS/s first, sc16)
   -> wideband ring (usrp_lib ring producer, RAM-aware size)
        -> in-process channelizer thread (iq_chan: CPU backend | GPU backend, same CUDA context)
             -> per-cell ring (anonymous shm_iq_ring)  [zero-shift cell: reads the wideband ring directly]
                  -> cell slot k: nr_cell_t { RU reader, slot thread (UEthread), scan/decode/UL queues, state }
all cells -> ONE core pool (fair share per cell) ; ONE CUDA context (pass instances, graphs, disc green ctx)
nr_cell_mgr <- cell inventory (blind front end) ; admission model sizes N_max and slots at start
dashboard (1 instance) reads per-cell log streams
```

Why this and not the alternatives:
- Separate processes: each extra CUDA context multiplies the decode body, GPU time is time-sliced between contexts, and green contexts do not isolate across processes [MEASURED, DGX OTA 2026-10-16 daytime, n=1 per row: T first-body p50 0.79 ms direct, 1.77 ms with a channelizer process, 4.3-6.8 ms with channelizer + second cell; p99 up to 100 ms; MPCIFIX section 2 [CODE-READ + MEASURED]]. Decided by the user 2026-10-16.
- Channelizer inside the process: removes the 2.5x time-slicing loss factor the channelizer saw on air (1.18 ms of work per 1.0 ms block at 2 cells, against 0.47 ms idle) [MEASURED, DGX OTA 2026-10-16 and bench, MPCIFIX 2]. It becomes one more stream of the same context, so SM placement and priority are controllable (G5).
- The shm ring layer is kept, because it gives a clean consumer-device interface (`shm_iq` device = a UE RU) and lets the cloud feed the receiver from a file publisher without radio.

### B.2 Per-cell state

Scope of the work [CODE-READ, mpci-main @e9c874c308, approximate regex counts]: about 700 `static g_*` globals across `nr_*.c` and `openair1/PHY/TOOLS/*.c`; file-scope non-function statics per file (>= 5 each): `nr_pdcch_blind_monitor_rt.c` 166, `nr_pdsch_passive_decode.c` 143, `nr_pdsch_passive_queue.c` 84, `nr_pdcch_blind_monitor.c` 67, `nr_gpu_pass_graph.cu` 37, `nr_pusch_passive_decode.c` 35, `nr_pusch_passive_queue.c` 29, `nr_pdcch_uss_tracker.c` 18, `nr_pusch_ul_bgdisc.c` 10, `nr_passive_acq_state.c` 8; about 180 `printf(` and 380 `LOG_[IWE](` sites in those files. Task A2 replaces these estimates with an exact audited table.

Mechanism (recommended; Decision F1):
- `nr_cell_t` slot array of fixed length `N_max` (derived at start, <= `NR_POOL_MAX_CELLS` = 16 of the pool). Slot = index 0..N_max-1, plus a descriptor `nr_cell_desc_t` (centre frequency, SSB, initial CFO, grid N_RB, SCS, PCI when known, window id) filled by the inventory.
- A thread-local `int nr_cell_idx()` (one int of static TLS; the 3 MB static-TLS limit [MEASURED, readelf 0x269100 = 2.41 MiB, MPCI-DASH-REVIEW] is not touched). Every thread of a cell (slot thread, scan, PDSCH, PUSCH consumers, workers) is created through `nr_cell_thread_create()` which inherits or sets the index. GPU host callbacks and CUDA event-handler threads do NOT use TLS: the job carries `cell`.
- Cell-indexed statics: `NR_CELL_STATIC(type, name)` declares `type name_cells[NR_CELL_MAX]` (heap-allocated lazily for large types) and the accessor `name` expands to `name_cells[nr_cell_idx()]`. Conversion is mechanical, reviewable with a diff that only touches declarations plus the audit table. Shared-by-design state (GPU pass instances, graph cache, core pool, dashboard) is marked PROCESS in the audit and stays global.
- Queues stay per cell (scan queue, PDSCH queue, PUSCH queue, recovery, CSI-RS disc feed); consumer threads stay per cell and lease from the shared pool (idle threads cost nothing; active ones are bounded by tokens). This keeps every module's internal logic unchanged.
- Host-global files become `nr_cell_path(cell, default_path)`; metrics JSON carries `"cell"` always (not only when tagged).
- Alternatives rejected in F1: threading a `cell*` argument through all functions (a rewrite of about 25 kLOC of passive receive path with high N=1 drift risk); keeping process-per-cell for the CPU side (reintroduces a second CUDA context, the thing being removed).

### B.3 IQ path and the channelizer

- Wideband ring: `usrp_lib` ring (`ISAC_USRP_RING_*` defaults are set in `--passive-rx`, nr-uesoftmodem.c:294), size RAM-aware (publisher rule `pub_default_capacity`: 2^27 samples, halved when RAM is short; 1.09 s ring gave 0 overruns where the 273 ms ring cascaded [MEASURED, DGX OTA 2026-10-15 03:32, LOW LOAD, O3]).
- Channelizer inputs per cell from the inventory: shift = cell centre - ring centre, decimation from the cell grid, passband = `num_rb_dl` x 12 x SCS (H3 rule; the NR UE never sets `rx_bw`). One channelizer thread serves all cells in one block loop (batching over channels is already `grid.y`).
- Backends: CPU (`iq_ddc`, for cloud and sens6 fallback; 4.49x real time mixer-only per 4-channel cell on one GB10 thread, 1.49x for 245.76 -> 122.88 MS/s decim 2 [MEASURED, DGX GB10, MPCIFIX 1]), GPU (kernels as MPCIFIX 2). Backend chosen by capacity at start (the measured table), not by flag.
- 200 MHz first: 245.76 MS/s x 4 RX on one QSFP link: publisher 0 overflows / 0 timeouts / 0 resyncs [MEASURED, DGX OTA 2026-10-16 09:24-09:41, daytime, n=1]. 400 MHz (491.52 MS/s x 4 = 7.86 GB/s sc16) is out of scope until Gate O3 passes; the cost model in `csd/mcell-spec` applies then.
- Known GPU numbers to beat (idle bench, 4 ch): 245.76 MS/s, 2 cells 4.37x real time (0.23 ms of work per ms of signal); 4 cells 2.56x [MEASURED, DGX GB10 bench, MPCIFIX 2]. On air the same work was 2.5x slower under context time-slicing; in-process the factor is unknown and is the first measurement of G5.

### B.4 One core pool, fair share per cell

Today [CODE-READ, CORE-POOL-20261020 and `nr_core_pool.h`]: classes SCAN > DECODE > RETRY > DISC > HOUSE, strict priority; inside a class round-robin across cells and oldest deadline inside a cell; reserve token (scan cannot take the last decode token; leases capped so decode keeps one); DL cap N-1 (N-2 while a UL decode waits); age promotion (RETRY 4 ms, DISC 20 ms, HOUSE 100 ms); no preemption; default 6 tokens (6,7,8,9,3,4); all pool users SCHED_OTHER. Round-robin across cells is not a share: a cell with 5 DL consumer threads can hold 5 tokens while another cell's single consumer waits.

Changes (Task B1, cloud, unit + bench):
1. Within a class, grant to the waiting cell with the lowest decayed token-time usage `U_c` (exponential decay, half-life a constant in `nr_core_pool.c`, 50 ms to start; equal weights; no per-cell parameter), ties by round-robin, inside the cell oldest deadline as today. Work conserving: an idle cell's share goes to the others at once.
2. Guarantees per active cell (active = a lease or waiter in the last 100 ms): at least one DECODE token is reachable within one release; the scan reserve token is shared but granted by rule 1 so a busy cell cannot hold it. DL cap becomes `max(1, N-1-(A-1))` per cell with A active cells, global sum still <= N-1; the UL rule (UL waiting caps DL at N-2) stays global, the freed token goes to the UL waiter chosen by rule 1.
3. DISC / HOUSE age promotion is switched off when the pool has fewer than 6 tokens (FLOAT-LANE-POOL 3: at T <= 5 cpus promotion made DL p99 +2.5 ms and UL miss 57 % at overload [Bench, DGX, synthetic spin jobs]).
4. All call sites pass the real cell (`nr_cell_idx()`); today six of seven pass 0 [CODE-READ].
5. Stats: the CORE_POOL line gains per-cell lease/wait counters (one extra block per cell, same grammar) so the dashboard and scorer split cells.
6. Open items carried from CORE-POOL: `nested` > 0 (discovery occasion, autodiscover, blind CSI-RS slot still run inline under the SCAN lease) and GPU waits holding the token. These are fixed for all cells in Task B1b (inline discovery -> DISC lease on its own thread, as already done for `pdcchDisc`) and G3 (explicit give scope at the GPU wait sites once the pass owns them).
Tests: strict priority and reserve invariants unchanged (the 22 existing tests stay green); new: two cells equal demand -> each gets within 10 % of half of the tokens; burst cell A (3 x load) vs quiet cell B -> B wait p99 <= 1.2 x its solo; A alone -> A uses all tokens (work conserving); no deadlock at N = 1..3 with 1..3 cells; TSAN/ASAN clean. Bench: extend `common/utils/tests/bench_nr_core_pool.cpp` with a cell dimension (mix per cell as in CORE-POOL 4: scan 225 us/slot, DL 600 us x1.2, UL 400 us x0.4, retry 1.5 ms x0.1) [Bench, cloud x86] — cloud numbers prove logic only; the token wait target (decode wait p99 < 200 us) is NOT claimed from a 4-cpu host (CORE-POOL: N=3 misses it, 1-4 ms).

### B.5 Host-derived core plan (remove hardcoded cores)

Hardcoded today [CODE-READ, SENS6-READINESS 4.1]: `nr_env_core` defaults PDSCH 6,7,8,9,3,4 / PUSCH 8,9 / GPU worker 10 / UHD ctrl 15 / USS 12 / nID 13 (`nr_core_plan.h:67-72`); `--passive-rx` sets `ISAC_UHD_CTRL_CORES=15`, `ISAC_FLOAT_CORES=10-14` (nr-uesoftmodem.c:293-295, intersected with online not allowed cpus); nID worker pinned to 13 (fails if 13 is outside the mask); USS worker 12; `ISAC_GPU_CHECK_CORES` default 10-14; dashboard default core 2 (an isolated core on sens6); RT slot thread unpinned unless `ISAC_UE_RT_CORE`; ring producer core 16 only via launcher.

Plan (Task B2, cloud; Task G2 wires and measures on the DGX):
- `nr_core_machine_read()` (online, allowed = process cpuset, packages/clusters, SMT, cpu_capacity, isolated, nohz_full, IRQ affinity, DPDK lcores) is called once in `main()` before any thread is created; `nr_core_plan_build()` produces the role table: RT set (slot thread per cell, ring producer, UHD ctrl, DPDK) -> pool cores (W - H) -> home set H for unleasable threads (OAI actors, CUDA callbacks, timers, dashboard) per FLOAT-LANE-POOL 3 and 5: H = 1 cpu for W < 12 else 2, taken from small cores when `cpu_capacity` is known; W = allowed online cpus minus the RT set.
- RT slot thread per cell: N cells need N slot threads (UEthread). They are RT-critical and so come from the RT set, not the pool; the RT set size is therefore N-dependent (derived from N_max). On the DGX today one RT core (5) [CODE-READ, starttest]. Whether a slot thread per cell is needed or one slot thread can serve several cells (they run the same slot clock for the same radio, different channelizer outputs) is Decision F2 and the answer decides the core cost of the second cell.
- Pool token count: `W - H` (as today's 6 on the DGX layout, up to 9 with the merged float lane, FLOAT-LANE-POOL 5 [Bench, DGX]). Float lane rule by host size W (FLOAT-LANE-POOL section 5): W >= 8 merged (pool = W - 1 or 2); 6 <= W <= 7 merged with DISC/HOUSE promotion off; W <= 5 keep a dedicated 1-cpu lane at nice 19 / SCHED_IDLE (merged + no-promotion is the alternative).
- `taskset` / cgroup shrink: logged once ("allowed N cpus, pool N tokens"); never silent; if W < 3 the receiver refuses to start with a clear line.
- Removal list (each a one-line deletion plus a test): the `nr_env_core` DGX defaults, `ISAC_UHD_CTRL_CORES=15` and `ISAC_FLOAT_CORES=10-14` in `--passive-rx`, nID core 13, USS core 12, `ISAC_GPU_CHECK_CORES` default, dashboard core 2, plus the dead env keys named in SENS6-READINESS section 1 (`ISAC_GPU_DISC_SMS`, `ISAC_GPU_RT_ISOLATE`, `ISAC_GPU_LDPC_SM`, `_NC_MIN_CB`, `_CL_MIN_CB`, `ISAC_CORE_PLAN`: read by no source) from the OTA env files in the new env set.
- Synthetic-topology tests: DGX (20 cpus, 15-19 isolated, 17-19 DPDK, X925/A725 capacities), sens6 (14 cpus, `isolcpus=2-3`, `nproc`=12, 6P + 8E [INFERRED]), cloud (4 cpus). Invariants: pool disjoint from RT set; no role on an isolated cpu unless it is an RT role; every thread has a home; plan is deterministic.

### B.6 One CUDA context: GPU sizing per cell

Current layout [CODE-READ, SENS6-READINESS 1]: discovery green context hard-coded to 8 SMs (`nr_gpu_green.h:44`), RT = remainder (GB10: 48 SMs -> 8 / 40); second split only when RT >= 32 SMs (DL real-grant 32 SMs, UL up to 40 overlapping DL, probes 8); below 32 one RT context and stream priority only; whole body compiled out under CUDA < 12.5; pass instance pool DL 4 / UL 2 / probe 1 compile-time; `CUDA_DEVICE_MAX_CONNECTIONS` set only in env files.

**GPU work scales with grants, not with cells or N_RB.** After grant sizing, FEP symbols and LDPC padding blocks are proportional to the grant (equal bit-for-bit to the full-slot pass [OFFLINE VERIFIED, DGX GB10 unit, `GpuPassSized.GrantSizedEqualsFullSlot`]), solo GPU span is unchanged within +-1 us and 4-way sharing p50 -3 to -10 % [Bench, DGX GB10, synthetic grants, not on air, GRANT-SIZED 4]. So the per-cell GPU demand is a rate model: `busy_c = lambda_c x E[span of its grants]`, with E[span] about 170 us of compute on the median path [MEASURED, DGX OTA 2026-10-19, GPU-SPAN-ROOTCAUSE / PROJECT_MEMORY 10-18/19 block] and about 4 ms on the heavy tail; admission and quotas use this, not an SM-per-cell carve-out.

Decisions:
1. **No per-cell SM partition.** Bursty 0.2-4 ms jobs would idle a private carve-out. SMs are split by CLASS only (discovery / RT-DL / RT-UL / probe / channelizer), derived from the device: `disc = clamp(round(SM/6), 2*gran, 8)`, `rt = SM - disc`, split-2 by ratio (DL >= 60 % of rt, UL overlapping, probe one granule) instead of the literal 32 (SENS6-READINESS 1 proposal). Fake-device unit test: GB10 (48), RTX 4060 Ti (34 [INFERRED, public spec; read the real count at start]), a 24-SM device. `CUDA_DEVICE_MAX_CONNECTIONS=32` is `setenv`'d at the top of `main()` before the first CUDA call.
2. **Channelizer placement** is chosen by measurement (G5): (a) top-priority stream in the RT context, (b) its own small group carved out of RT (`ceil(SM/12)` SMs as starting hypothesis, [HYPOTHESIS]). Criterion: 0 overruns over 10 min at 2 cells decoding and cell p99 within +10 % of the same replay with the channelizer replaced by a pre-channelized file.
3. **Pass instances and graphs are shared by all cells; per-cell quota.** Pool counts derive from SM and memory budgets (SENS6-READINESS 1 item 4, 3). Each cell has a quota `Q_c` of DL/UL instances by the same usage-based fair share as the CPU pool (work conserving; floor 1 per active cell). Shape-class graphs are keyed by (SCS, N_RB class, shape class), independent of the cell, so two cells with the same shapes reuse graphs (the old process-per-cell design paid 11.0 and 9.1 GB per process [MEASURED, DGX replay, csd/mpci-int build 2026-10-15, MPCI-DASH-REVIEW 3.4]).
4. **Memory ledger** (Task G1): per-module GPU and pinned-host accounting by tag (`GPUMEM` line as in Stage 4 Task 1), split SHARED vs PER_CELL. Per-cell items known from the code [CODE-READ, SENS6-READINESS 3, INFERRED sizes]: DL HARQ soft store 99 MiB (2048 x 66 x 384 x 2 B) — to become one slab keyed (cell, rnti, pid) sized by `sum over active cells` of an admission-time HARQ budget; CSI-RS FD rings + detector about 957 MiB (host), TD pool about 1138 MiB [ANALYTICAL, T8], per-consumer IQ snapshot sets (8 x ring-sized), UL IQ retention, USRP ring. Shared items: 7 pass instances about 260 MiB device (+ the same pinned in DISCRETE), split-graph executables capped at `split_cap_auto()` = a quarter of device memory clamped to [2048, 3072] MB, whole pass on the GB10 1924 MB [MEASURED, DGX GB10, P99-UL-20261019 s.5], local-memory reservation about 1-1.5 GiB. Sizing rule: per-slot buffers (rxF, est, LLR) are sized from the cell's N_RB and `nb_antennas_rx` (Stage 4 Task 1 `nr_pdsch_gpu_llr_bytes_for`), pass instances stay class-sized. The output of G1 is the first real per-cell GPU number; until then the figure in the admission model is the measured single-process total divided by nothing: it must be measured, not assumed.
5. **Discovery budget across cells** (Task G4). Facts: discovery runs on the 8-SM green context at priority 0; `c_seg` unit = 18-125 ms alone (a 32-thread CTA), chunking to 1 unit made a call 8x-24x slower on an idle GPU, now floored at 32 units (about 27 ms per chunk, p=640: 9 chunks, about 250 ms instead of 1215 ms); `c_sweep` is 0.06 ms per chunk idle but 94.9 ms behind a 100 ms kernel of another stream in the SAME discovery context (RT-side load cost only 0.4-1.3 ms); `c_refine` is one thread, 231 ms per call, 7x-20x slower under flood [MEASURED, DGX bench idle, DISC-GPU-TIMING-20261022]. On air `c_sweep` ran as one launch of about 212 ms against 3.5 ms idle [MEASURED, DGX+X410 34B2AF1, LOW LOAD, n=1, 2026-10-22 final OTA]; Vodafone discovery throughput about 60 % of main there. N cells multiply this load, and the contenders of a sweep chunk are the other discovery kernels. Therefore: one process-wide single-flight discovery ticket (a discovery kernel chain of any cell holds it; granted ACQUIRE-state cells first, then usage-based fair share), the 32-unit floor kept, `c_refine` parallelised over its (slot, antenna, port) units (512 independent units) as an optional follow-up if the ticket wait dominates, global discovery duty budget initially 30 % of the discovery context wall time (as M3; to be re-derived), per-cell budget = budget / cells in ACQUIRE-or-MAINTAIN needing discovery.
6. **RT protection from discovery across cells.** (a) All waits polled (`nr_gpu_poll_relax`); a unit test greps for `cudaEventSynchronize` / `cudaStreamSynchronize` / `cudaDeviceSynchronize` in the receive path outside an allowlist. (b) Launch-block bench (from DISC-NONBLOCK: 1 of 227 386 passes > 1 ms after the fix with 1 ms chunks, residual bound about 5 ms [Bench, GB10]) re-run with the 32-unit floor (about 27 ms kernels) and with two cells' tickets: this is a gap in the evidence, since the 5 ms bound was shown for 1 ms chunks. (c) The ticket removes the all-cells-RT-idle admission rule of M3 (O6: grows with N). (d) Stop condition in G4: `ULSTALL gpu_us > 100 ms` count 0 and no RT graph launch > 5 ms overlapping a discovery busy period on a 2-cell run.

### B.7 Interface to the blind front end (plan only)

The front end (`csd/blind-front`, separate agent) owns discovery of cells from a wideband capture; mpci-main consumes an inventory. Contract, versioned, file-based first (an in-process queue later behind the same header):
- `docs/superpowers/specs/mpci-cell-inventory-v1.md` + `common/utils/nr_cell_inventory.h`: a JSON file `cells.json` (schema `schema:1`) written atomically (temp + rename), re-read on change, with `window {centre_hz, rate_msps, n_rx}` and `cells[]` of `{id, centre_hz (receiver -C), gscn/ssb_hz, ssb_index, pci, cfo_hz, n_rb_hint|null, scs_khz, sss_gap, dmrs_ratio, first_seen_ns, state}`. Fields map 1:1 to the current `fastscan.py` / `autofront.py` `cells.json` (FRONTLIVE: PCI, i_ssb, CFO, GSCN, tuning triple `-C/--ssb/--initial-fo`) so the existing tools are already conformant except the `schema`, `window` and `id` keys.
- Semantics: add (new `id`) -> `nr_cell_mgr` activates a free slot; update (same `id`, new CFO or centre) -> retune of that cell's channelizer entry; remove/`state:lost` -> retire after a hysteresis. Co-channel cells (WindTre 508 + PCI 4 share one SSB frequency) are two entries with equal `centre_hz` and `ssb_hz`.
- The receiver never trusts a field it can verify: PCI and MIB are confirmed by its own sync; pass-2 retune (k_SSB -> `--ssb`, `-C`) is applied from the receiver's own log by `nr_cell_mgr` (no `live_chain.sh pass2` run by hand).
- No gNB log or config is an input. A fixture inventory built from the AUTOFRONT/FRONTLIVE `[MEASURED]` tables (415, 923, 403, 508, 4, 675 with their triples) lets cloud tests run without the front end.

### B.8 Observability

- Receiver output attributable per cell: a sink-level tag (the LOG layer reads `nr_cell_idx()`; raw `printf` sites go through a shim macro `nr_cprintf` only where the file is in the per-cell set) writes `[c<idx>] ` on every line, and a per-cell log file is cut from the stream by the launcher/dashboard. Decision F5 settles prefix vs split files; recommendation: prefix, because it keeps one stdout and the existing parsers need only a regex front.
- Metrics JSON always has `"cell"`; `goal_metrics.py`, `score_rx.py`, `mpci_runsum.py` take `--cell`.
- Dashboard (single instance, SCHED_IDLE) shows per-cell pages and receiver-wide cards once (port of `1040882805`).
- A `MPCI_CELL` line per cell per 10 s: state (ACQUIRE/MAINTAIN), tokens used, GPU instances used, GPU busy fraction, discovery ticket wait, channelizer lag.

### B.9 Admission and lifecycle

- At start: `nr_cell_admit(host, inventory)` computes `N_max` = min(inventory cells wanted, RT cores for slot threads, pool tokens / 2 (fair floor), RAM budget, GPU memory budget, channelizer capacity), pre-allocates N_max slots (CPU state, per-cell GPU state) BEFORE the RX fence, logs `MPCI_ADMIT cells=N_max reason=<binding limit> ram=... gpu=...`. No runtime allocation after the fence.
- Cells beyond `N_max` are listed as `unserved reason=<limit>` (not an error).
- Free RAM never below 16 GB (rule carried from `mpci_auto.py`); known data point: about 2 full 217-PRB cells per DGX process-per-cell (anon RSS about 13 GB per 217-PRB cell, 3.6 GB per 51-PRB cell [MEASURED, DGX replay + OTA, csd/mpci-int build, O5]) — single process is expected to be cheaper, measured in G1.

---

## File structure and ownership (cloud / DGX / sens6 never collide)

CMake: to avoid the `CMakeLists.txt` conflict every old merge hit, Task T0 adds two include seams, `cmake_targets/mpci_cloud.cmake` (owned by group C) and `cmake_targets/mpci_gpu.cmake` (owned by group G), included once from `CMakeLists.txt`; no other task edits the top-level `CMakeLists.txt`.

| Group | Owns (create/modify) | Does NOT touch |
|---|---|---|
| **T0** orchestrator, once | branch bootstrap, doc imports, cmake seams, `docs/.../INDEX.md` rows | any code |
| **C** cloud x86 | `common/utils/nr_cell*.{h,c}`, `nr_cell_mgr*`, `nr_cell_admit*`, `nr_cell_inventory.h`, `nr_core_pool.{c,h}`, `nr_core_plan.{c,h}`, `radio/COMMON/{shm_iq*,iq_ddc*,iq_chan*}` (host side, CPU backend), `executables/nr-uesoftmodem.c` (main(), `--passive-rx` block), `executables/nr-ue.c` (slot-thread per cell), all CPU-path files in `openair1/PHY/NR_UE_TRANSPORT/` (`nr_pdcch_*.c`, `nr_pdsch_passive_*.c`, `nr_pusch_passive_*.c`, `nr_pusch_ul_*.c`, `nr_passive_*.c`, `nr_csirs_disc_feed.c`, `nr_csirs_disc_td.c`, `dci_nr.c`, `nr_dl_traffic_probe*`), `openair1/SCHED_NR_UE/`, `tests/passive_rx/{mpci,monitor}/`, `tests/passive_rx/dgx/` scripts, `tools/mpci/`, tests under those dirs | every `.cu`, `.cuh`, `nr_gpu_*` file, `nr_pdsch_gpu_*`, `nr_pdcch_nid_gpu*`, `nr_csirs_disc_gpu*`, `nr_gpu_budget*`, `nr_gpu_green.h`, `radio/COMMON/shm_iq_chan_gpu.*`, `radio/USRP/*` |
| **G** DGX (orchestrator only) | all `.cu/.cuh`, `nr_gpu_*.{c,h}`, `nr_pdsch_gpu_*`, `nr_pdcch_nid_gpu*`, `nr_csirs_disc_gpu*`, `nr_gpu_budget*`, `nr_gpu_green.h`, `nr_gpu_pass_*`, `radio/COMMON/shm_iq_chan_gpu.*`, `radio/USRP/usrp_lib.cpp`, `tests/passive_rx/gpuspan/`, OTA env files and DGX scripts for its own gates | everything in C's column |
| **S** sens6 | its own task branch; only test/bring-up scripts under `tests/passive_rx/sens6_mpci/` (new dir, `.cfg`/`.sh`, never `tests/passive_rx/*.conf`) and a discrete-GPU backend branch later (own plan) | frozen paths; C and G files except via the rebase queue |

Seams (the only shared files; created by C, frozen after Task A1, changed only by C with a handover note):
- `common/utils/nr_cell.h` — `nr_cell_idx()`, `NR_CELL_STATIC`, `nr_cell_thread_create`, `nr_cell_path`, `NR_CELL_MAX`.
- `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_cell.h` — plain-C contract G implements: `int nr_gpu_cell_attach(int cell, const nr_gpu_cell_params_t *p)`, `void nr_gpu_cell_detach(int cell)`, `nr_gpu_job_t.cell`, `size_t nr_gpu_cell_bytes(const nr_gpu_cell_params_t*, int kind)`, `int nr_gpu_disc_ticket_acquire(int cell, int state)` / `_release`, `int nr_gpu_quota(int cell)`. C ships `nr_gpu_cell_stub.c` (no GPU) so the cloud build links and tests run.
- `common/utils/nr_cell_admit.h` — pure function `nr_cell_admit_plan(const nr_host_caps_t*, const nr_cell_desc_t*, int n, nr_admit_plan_t*)`; G supplies `nr_host_caps_t` GPU fields from the device (SM count, free memory).

Collisions found and resolved: (1) per-cell conversion of GPU host files (`nr_gpu_pass_dl.c`, `nr_gpu_pass_graph.cu`) is G's, behind `nr_gpu_cell.h`; (2) UL tasks C1-C4 touch `nr_pdcch_ul_layout_lock.c` and `nr_pusch_*`, which group C also converts: C4 (log split) goes before the UL conversion, C1 right after it, in the same group, serialized; (3) `usrp_lib.cpp` stays G's (the radio fixes of row 4).

---

## C. Tasks and stop conditions

Notation: each task lists Files, Interfaces, Tests, Steps, STOP condition (the task is done only when it holds; if a stop condition cannot be met the agent stops and reports, it does not widen scope). Group order inside a phase is a dependency order.

### Phase (i) — cloud x86, no GPU (group C)

#### Task T0 (orchestrator, DGX or any host, not a cloud task): bootstrap
**Files:** none edited by hand beyond imports. **Steps:**
- [ ] `git -C /home/nicola/NICOLA/wt/rr-mpci-main log -1` shows the plan commit on `mpci-main`; push policy is the user's.
- [ ] Import, as one commit "import: old multi-PCI sources for porting (no behaviour change)": write the files of `csd/mpcifix` that do not exist on main into `ref/mpci-old/` with `git -C /home/nicola/NICOLA/wt/rr-main-merge archive csd/mpcifix radio/COMMON radio/USRP common/utils/nr_cell_instance.h openair1/PHY/NR_UE_TRANSPORT/tests/nr_cell_instance_test.cc openair1/PHY/TOOLS/nr_mpci_sched.c openair1/PHY/TOOLS/nr_mpci_sched.h tests/passive_rx/mpci | tar -x -C ref/mpci-old/` (reference text, NOT built, excluded from the static audit; paths that exist on main, such as `radio/COMMON/common_lib.c`, are deleted from the copy). Copy the old reports `docs/superpowers/reports/csd/MPCI-*.md`, `MPCIFIX-20261015.md` (and `MPCI-DASH-REVIEW-20261015.md` from `csd/mpci-int`) into the live reports directory with `git show <branch>:<path>`. Cherry-pick the three `csd/mcell-spec` doc commits and `eb082ca709` (disctime bench + report; `git merge-tree` is clean). Add `cmake_targets/mpci_cloud.cmake`, `cmake_targets/mpci_gpu.cmake` (empty) and the two `include()` lines.
- [ ] Frozen gate passes. Build of main unchanged (cloud: ninja target list; DGX: not required).
**STOP:** `git diff --stat adaptive-rx-UL-DL..mpci-main` shows only docs, `ref/mpci-old/`, bench, two cmake seams. This import makes pushing `csd/mpci-int` unnecessary (see Section F, F9).

#### Task A1: cell identity core (the seam header)
**Files:** Create `common/utils/nr_cell.h`, `common/utils/nr_cell.c`, `common/utils/tests/test_nr_cell.cpp`; extend `cmake_targets/mpci_cloud.cmake`.
**Interfaces:**
```c
#define NR_CELL_MAX 16
int  nr_cell_idx(void);                                   /* TLS; 0 when never set (single-cell path) */
void nr_cell_bind(int idx);
int  nr_cell_thread_create(pthread_t*, const pthread_attr_t*, void *(*fn)(void*), void *arg, int cell); /* child inherits idx */
void nr_cell_path(int cell, const char *def, char *out, size_t n);   /* cell 0 + single-cell mode: unchanged path */
#define NR_CELL_STATIC(type, name)  /* declares name_cells[] + accessor */
```
**Tests:** index inheritance through nested thread creation; default 0; `nr_cell_path` ports the `nr_cell_instance_test.cc` cases (extension, directory, invalid tag fatal, single-cell unchanged byte for byte); static TLS size check script (`readelf -lW` on the test binary, < 3 MB).
**Steps:** [ ] failing tests, [ ] implement, [ ] pass, [ ] commit `feat(mpci): nr_cell core (cell index, cell-indexed statics, cell paths)`.
**STOP:** tests green on cloud x86 under ASAN+UBSAN; header reviewed and frozen (note appended to this plan's Task log).

#### Task A2: static audit tool and the classification table
**Files:** Create `tools/mpci/static_audit.py`, `tools/mpci/mpci_statics.tsv` (one row per file-scope mutable object: file, symbol, class PER_CELL / PROCESS / CONST / TLS, owner task), `tools/mpci/test_static_audit.py`; ctest hook in `mpci_cloud.cmake`.
**Steps:** [ ] parse C/C++ with a conservative scanner (file-scope `static` non-const objects, non-static globals, function-local statics, `__thread`) over the receive-path directory set in the File ownership table plus `openair1/PHY/TOOLS`, `common/utils` pool/plan, `radio/COMMON`; [ ] first run writes the table with class `UNCLASSIFIED`; [ ] the audit fails on any UNCLASSIFIED or unknown new object; [ ] classify the module-independent objects (pool, plan, GPU pass instance pool and graph cache = PROCESS; tables and const arrays = CONST).
**STOP:** the audit runs in < 60 s, the table exists with an exact count per file (replaces the regex estimates in B.2), and every row has an owner task among A3-A8/G3. The count of PER_CELL objects is written back into this plan (Review item 1).

#### Task A3: convert acquisition, config, metrics, paths (module group 1)
**Files:** `nr_passive_acq_state.c` (+ SIB1 sibling file temp+rename, inventory row 12), `nr_passive_cfg_sources.c`, `nr_passive_metrics*.c` (`"cell"` always), `nr_pdcch_coreset_map.c`, `dci_nr.c`, `nr-ue.c` host paths; tests `nr_passive_acq_state_test.cc` etc.
**Tests:** two-cell interleave test per module (cell A fed trace X, cell B trace Y, alternating; each equals its solo output); SIB1 sibling race test (writer loop vs reader never sees a short file).
**STOP:** all PER_CELL rows of these files converted; N=1 equivalence suite (A9) green; interleave tests green; ctest only known failures.

#### Task A4: convert PDCCH scan and blind monitor
**Files:** `nr_pdcch_blind_monitor.c`, `nr_pdcch_blind_monitor_rt.c` (166 statics), `nr_pdcch_passive_queue.c`, `nr_pdcch_uss_tracker.c`, `nr_pdcch_nid_chunks.h` host parts, `nr_dl_traffic_probe*`.
**Notes:** the scan queue already carries `job.gNB_id` as cell for the pool; align it with `nr_cell_idx()`. Dedicated-CORESET discovery host side (`pdcchDisc`, DISC lease) is per cell.
**STOP:** as A3, plus: PDCCH accept sets of cell A unchanged when cell B runs (replay-free: synthetic slot traces from `tests/passive_rx/dgx/fixtures` and the nr_dlsim-style generators already in tree).

#### Task A5: convert PDSCH queue, decode, recovery
**Files:** `nr_pdsch_passive_queue.c` (84), `nr_pdsch_passive_decode.c` (143), `nr_pdsch_recovery.c`, DL HARQ state host side (key (cell, rnti, pid)), CPU decode path (the sens6 path).
**STOP:** as A3; HARQ isolation test (same RNTI and HARQ id in two cells never combine); CPU decode of a fixed grant set is byte-identical across cells and equals the N=1 output.

#### Task A6: convert PUSCH/UL
**Files:** `nr_pusch_passive_queue.c`, `nr_pusch_passive_decode.c`, `nr_pusch_ul_bgdisc.c`, `nr_pdcch_ul_layout_lock.c`, UL snapshot copy.
**STOP:** as A3; UL layout lock per cell test (two cells with different BWP, locks never leak).

#### Task A7: convert CSI-RS discovery host side
**Files:** `nr_csirs_disc_feed.c`, `nr_csirs_disc_td.c`, footprint/detector host state.
**Notes:** GPU calls go through `nr_gpu_cell.h` (stub on cloud).
**STOP:** as A3; per-cell feed rings sized from the cell's N_RB (hook for the G1 ledger).

#### Task A8: cell slots in the receiver main and slot threads
**Files:** `executables/nr-uesoftmodem.c`, `executables/nr-ue.c`, `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c`, `nrPHY_vars_UE_g[inst][CC]` use (OAI already allocates `NB_UE_INST` instances; the passive path uses `[0][0]`; line 532/556 `disc_ue`, replay input).
**Steps:** [ ] decision F2 resolved first (one slot thread per cell vs shared); [ ] `NB_UE_INST` = N_max from the admission plan; [ ] each cell gets its RU device reader (`shm_iq` consumer on its cell ring), its UE instance, its threads through `nr_cell_thread_create`; [ ] removal of the cell-0 assumptions listed by A2 (`PHY_vars_UE_g[0][0]` users).
**STOP:** a 2-cell cloud run from file publisher IQ (C3) syncs both cells; N=1 path byte-identical in startup log lines (diff vs main).

#### Task A9: N=1 equivalence suite (runs on every PR, including merges of main)
**Files:** `tests/passive_rx/mpci/n1_equiv.sh`, `tests/passive_rx/mpci/test_n1_equiv.py`.
**Steps:** [ ] unit-level: the module interleave tests from A3-A7 with a single cell; [ ] log-level: the summary and CORE_POOL grammar of a short rfsim/replay run is parsed and compared with a stored main reference on the same host class (cloud x86 reference is cloud-only; the DGX reference is created by the orchestrator); [ ] exits non-zero on drift.
**STOP:** suite green on cloud for the small bed (see C3 for the largest cell the cloud host can lock); the DGX 4-RX `rfsim_regress.sh` is the final N=1 gate (Section D, V2).

#### Task B1: pool fair share per cell (see B.4) and B1b inline discovery -> DISC lease
**Files:** `common/utils/nr_core_pool.{c,h}`, `common/utils/tests/test_nr_core_pool.cpp`, `bench_nr_core_pool.cpp`, call sites in the converted modules (cell = `nr_cell_idx()`), `nr_pdcch_blind_monitor_rt.c` sites at disc_due / autodiscover / blind CSI-RS slot.
**Tests/Bench:** B.4 list; bench rows in `docs/superpowers/reports/csd/core-pool-bench-mpci-<date>/` labelled `[Bench, cloud x86, 4 cpus, n=3 medians]`.
**STOP:** all existing + new pool tests pass 10x and under TSAN; burst-vs-quiet test passes; `nested` = 0 in the bench with inline discovery moved; no hang at N = 1..3. No claim about 200 us decode token wait from this host.

#### Task B2: wire `nr_core_machine_read` / `nr_core_plan_build`; remove hardcoded cores (B.5)
**Files:** `common/utils/nr_core_plan.{c,h}`, `common/utils/nr_core_pool.c` (`site_init`), `executables/nr-uesoftmodem.c` (`--passive-rx` block lines 293-295), worker pin sites (`nr_pdcch_nid_gpu.c` is G's: the pin call goes through a plan lookup `nr_core_role_cpu(role)` declared in `nr_core_plan.h`; G removes the literal 13 in G2), dashboard core.
**Tests:** `test_nr_core_plan.cpp` gains DGX / sens6 / 4-cpu synthetic machines and the invariants of B.5; `taskset` shrink logging.
**STOP:** on cloud x86 the receiver starts with the derived plan (4 cpus: refuses cleanly with the W < 3 rule or runs with pool 2 and a logged warning, as decided by the test); no literal DGX core number remains in non-test, non-comment code of group C files (grep gate in the audit tool).

#### Task B3: admission model `nr_cell_admit` (CPU/RAM side, GPU side injected)
**Files:** `common/utils/nr_cell_admit.{h,c}`, `tests/test_nr_cell_admit.cpp`, `tests/passive_rx/mpci/admit_model.py` (offline planner, replaces the process-launching `mpci_auto.py`).
**Interfaces:** `nr_cell_admit_plan(caps, cells, n, &plan)`; plan per cell: `served | unserved(reason in {cores, ram, gpu_mem, channelizer, rt_cores})`.
**Tests (ports of the Stage 4 tests, host numbers injected):** sens6 profile (14 GiB RAM, 8188 MiB VRAM): two 106-PRB cells served, two 217-PRB cells -> one `unserved gpu_mem` (these expectations are `[CODE-READ]` estimates until G1 supplies measured per-cell bytes; the test fixtures carry them as named constants with the label); DGX profile; RAM floor 16 GB.
**STOP:** tests pass; every constant used in the model is traceable to a report line or marked `[HYPOTHESIS]` with the task (G1) that will replace it.

#### Task B4: derived SM plan (pure function, fake device)
**Files:** `openair1/PHY/TOOLS/nr_gpu_sm_plan.h` (pure, header-only, no CUDA) + test; G consumes it in G2.
**Tests:** 48 -> 8/40 with split-2; 34 -> `disc = 6` or 8 per granularity, RT 26-28, one RT context, ratio rule; 24; granularity 2 vs 4; CUDA < 12.5 stub returns "no green".
**STOP:** tests pass; plan documents the DL/UL/probe ratios in one place.

#### Task B5: float lane into the pool (FLOAT-LANE-POOL section 3 design)
**Files:** `nr_pdcch_blind_monitor_rt.c` (janon), `nr_pdcch_uss_tracker.c`, `nr_pdsch_passive_queue.c` (dmrsScan, `nr_bgq_run_one`), `nr_csirs_disc_feed.c` (per-batch lease around CPU parts), `nr_pusch_ul_bgdisc.c` (chunk with `nr_pool_yield` between steps), `nr_ul_nid_bg.c`; nID GPU worker lease (CPU parts only) is a G2 hook.
**Rule:** DISC for janon / nID / USS / dmrsScan / dmrsId / csirs; HOUSE for ulBgDisc and UL NID bg; per job a lease, never across a cond wait; no FIFO/RR.
**STOP:** pool bench with N cells' float jobs (bench knob `BENCH_FLOAT=merged`, cell dimension): decode wait p99 within +0.3 ms of the dedicated lane at T >= 8 and within +0.5 ms at T4/T5 with promotion off [Bench, cloud x86]; floats keep >= 90 % offered CPU at T >= 8. The DGX confirmation is gate V2.

#### Task C1: transport layer port (ring with anonymous backing, consumer device)
**Files:** `radio/COMMON/shm_iq_ring.{c,h}`, `shm_iq.c`, `shm_iq_pub.c`, tests (`test_shm_iq.c`), `mpci_cloud.cmake`. Sources come from `ref/mpci-old/` (T0).
**Interfaces:** `shm_iq_ring_create(anon|posix_shm, capacity_samples, n_rx, n_consumers)`; consumer device registers like `raw_iq`.
**Tests:** port all `test_shm_iq` cases incl. the passband case (H3), heartbeat unsigned-wrap fix (fa4e444678), overrun accounting; add anonymous-backing and N-consumer cases; ring capacity RAM-aware default test.
**STOP:** green on cloud x86, ASAN/UBSAN; no `shm_open` use in the anonymous path.

#### Task C2: CPU channelizer thread (`iq_chan`) with a backend interface
**Files:** `radio/COMMON/iq_chan.{c,h}` (new host driver, from `shm_iq_chan.c` logic: block loop, per-cell entries, retune without full re-sync, discontinuity handling), `radio/COMMON/iq_ddc.{c,h}` (+ NEON guarded for x86), tests `test_iq_chan.c`, `test_iq_ddc_equiv.c`.
**Interfaces:** `typedef struct { int (*run)(...); size_t (*capacity)(...); } iq_chan_backend_t;` CPU backend here; GPU backend registered by G5 behind the same struct.
**Tests:** bit-exact vs frozen reference (decim 1/2/4/8, shifts 0, -19.81 MHz, 7.3 MHz, block sizes not multiples of 4096); cell added/removed live; zero-shift cell is a memcpy; overrun then recover keeps filter state.
**STOP:** equivalence 0 LSB; two-cell run through an anonymous ring delivers continuous, bit-exact narrow streams to two consumers for 60 s on cloud (rate: whatever real time the cloud CPU sustains; state it, do not extrapolate to the DGX).

#### Task C3: synthetic multi-cell wideband IQ for the cloud
**Files:** `tests/passive_rx/mpci/mix_cells.c` (sum of per-cell IQ shifted to chosen offsets at a wideband rate), `tests/passive_rx/mpci/make_two_cell_fixture.sh`, `shm_iq_file_pub` (port).
**Steps:** [ ] first measure the largest rfsim cell the cloud host can lock end-to-end (51 PRB? 24 PRB?) using the existing rfsim beds `[MEASURED, cloud x86]` (CORE-POOL records that a 4-cpu cloud host was too slow to lock PDCCH on its bed — this step sets the bed size or declares the cloud unable); [ ] record N gNB rfsim IQ with the existing recorder (`record_player.*`) and mix to a 2-cell wideband capture with known offsets (the ground truth is the generator's own parameters, never a gNB log); [ ] publish through the file publisher.
**STOP:** a documented fixture where both cells' SSB/MIB/SIB1 are decodable after channelization (checked with the single-cell receiver, N=1 path). If the cloud host cannot do this at any size, the stop is "cloud rfsim gate impossible; Section D V1 moves to a larger cloud host or to the DGX" and the agent says so.

#### Task D1: cell manager and the inventory interface (B.7, B.9)
**Files:** `common/utils/nr_cell_mgr.{h,c}`, `nr_cell_inventory.{h,c}` (parser, atomic-file watcher), `docs/superpowers/specs/mpci-cell-inventory-v1.md`, fixtures `tests/passive_rx/mpci/inventory/*.json` (415, 923, 403, 508, 4, 675 from the AUTOFRONT/FRONTLIVE `[MEASURED]` tables), tests.
**Tests:** add / update / remove / lost; co-channel pair; refused beyond `N_max` with reason; pass-2 retune from a stored log excerpt (k_SSB 14 -> `--ssb 1171`, from the AUTOFRONT table); slot activation never allocates (allocation hook counts, fails if > 0 after the fence stub).
**STOP:** a fake front end drives a 3-cell inventory through the manager on cloud; schema document reviewed by the front-end agent (the user passes it; no code in `tests/passive_rx/autofront/*` is changed here).

#### Task D2: log/metrics attribution, dashboard multi-cell, scoring
**Files:** LOG sink tag (`common/utils/LOG/log.c` hunk, minimal), `nr_cprintf` shim header, `tests/passive_rx/monitor/{monitor.py,monitor.html,rxparse.py,test_monitor.py}` (hand port of `1040882805` multi-cell logic; P99AGE parser; per-cell radio lines), `goal_metrics.py`, `score_rx.py`, `mpci_runsum.py`.
**Tests:** monitor suite stays green (47+ tests); new multi-cell fixtures (screenshots are not required); a log with two cells' lines yields two independent metric sets; receiver-wide cards shown once.
**STOP:** every line type the dashboard parses is attributable in a 2-cell cloud log; scoring scripts refuse a mixed log without `--cell`.

#### Task E1: multi-cell bed on cloud (functional, `[SIM VERIFIED]` class)
**Files:** `tests/passive_rx/mpci/run_two_cell_cloud.sh`, `.cfg` (never `.conf`).
**Steps:** [ ] run the A8 process on the C3 fixture; [ ] collect per-cell sync, MIB, SIB1, PDSCH CRC from the log; [ ] compare each cell with its solo run on the same fixture (N=1 path).
**STOP:** both cells reach SIB1; per-cell CRC-OK within the solo run-to-run spread; evidence labelled `[SIM VERIFIED, cloud x86 ...]` and `rfsim CRC is an upper bound`. If C3 declared the bed impossible, E1 is skipped and the gap is reported.

#### Tasks UL-C4, UL-C1, UL-C2, UL-C3: UL items C1-C4 (cloud, CPU reference side)
Source: `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md` section 6 and ULCAUSE-20261016.
- **C4** UL probe/real log split (`probe=`, `tbl=` in `UL_PUSCH_CRC`): log only; first, before A6 touches those files; `goal_metrics.py` already reads the optional fields.
- **C1** UL BWP look-alike lock (216@1 vs DL 217@0): after A6; per-cell lock table; verification = brute-force replay of the ULCAUSE separating test where the data exist, otherwise unit fixtures.
- **C2** UL IQ retention + 1024 data-ID search: cloud writes the CPU reference sweep and the retention ring per cell; the GPU batch (1024 items) is G-side (G8).
- **C3** UL HARQ soft combining: cloud writes the keyed slab contract (cell, rnti, pid, cleared on NDI toggle); GPU implementation is G8.
**First step of each (mandatory):** check against `mpci-main` whether the item already exists (`git log -S`), because UL work landed on main after the spec was written (`csd/int16-ul12`, `ulown`, `ulst` lines); report "already done" instead of re-implementing.
**STOP per item:** unit tests green; UL numbers are never claimed on cloud (no UL air data): `[OFFLINE VERIFIED, cloud x86]` only.

### Phase (ii) — DGX GPU and OTA (group G; run by the orchestrator only)

All G tasks start with: `git merge` of the latest cloud work already on `mpci-main`, DGX build per Global Constraint 9, the N=1 gate V2a green. Radio windows follow the rules of Global Constraint 8 and the 2026-10-09 hang lessons (one heavy job host-wide, thermal gate 80 C, no agent builds during radio).

#### Task G1: GPU memory ledger (`GPUMEM`) and per-cell sizing
**Files:** Create `openair1/PHY/TOOLS/nr_gpu_memstat.{h,c}`, `.../tests/nr_gpu_memstat_test.cc`; modify every allocator listed in Stage 4 Task 1 (`nr_pdsch_gpu_fep.cu`, `nr_pdsch_gpu_demod.cu`, `nr_pdsch_gpu_ldpc.cu`, `nr_csirs_disc_gpu.cu`, `nr_pdcch_nid_gpu.c`, `ldpc_decoder.cu`, `ldpc_cb0.cu`, `nr_td_cb0_batch.cu`, `nr_gpu_mem.c`, `nr_gpu_pass_graph.cu` `alloc_inst`), `nr_gpu_cell.h` implementation of `nr_gpu_cell_bytes`.
**Steps:** [ ] tag every allocation, SHARED vs PER_CELL; [ ] size per-slot buffers from N_RB and nb_ant; [ ] measure on the DGX replay (c3610 pair separately and together; b 217 PRB) `GPUMEM` and `/proc/<pid>/status` RssAnon (unified memory) with 1 and 2 cells; [ ] feed the constants to `nr_cell_admit` (B3), replacing the `[HYPOTHESIS]` values.
**STOP:** report `docs/superpowers/reports/csd/MPCI-GPUMEM-<date>.md` with `[MEASURED, DGX GB10 replay, cell PCI .. BW ..]` rows per cell; per-cell increment of the second cell stated in MiB (device, pinned host, RSS); same CRC-OK with and without sizing.

#### Task G2: derived SM plan, pool counts, core roles on the DGX
**Files:** `nr_gpu_green.h` (use `nr_gpu_sm_plan.h`; remove literal 8 and 32; log the derived numbers), `nr_pdsch_gpu_fep.h` (instance counts from budgets), `nr-uesoftmodem.c` (`setenv CUDA_DEVICE_MAX_CONNECTIONS=32` before CUDA), `nr_pdcch_nid_gpu.c` (core 13 literal -> plan role), `nr_pdcch_uss_tracker.c` is C's.
**STOP:** on the DGX the derived plan equals today's (8/40, 32/40/8) bit for bit [CODE-READ + log line equality]; N=1 gate V2a green; the sens6-sized fake device test passes (B4).

#### Task G3: per-cell GPU state, instance quotas, HARQ slab
**Files:** `nr_gpu_pass_dl.c`, `nr_gpu_pass_ul.c`, `nr_gpu_pass_graph.cu` (`job.cell`; quota; HARQ store keyed (cell, rnti, pid); per-cell rings), `nr_gpu_pass.h`, `nr_pdsch_gpu_*`.
**Tests:** `test_nr_gpu_pass` gains two-cell cases (same shapes share a graph; same RNTI/pid in two cells never combine; quota floor 1; burst cell cannot take the last instance of a quiet cell); the GPU pass explicit give scope at wait sites if the pool bench says it is safe (CORE-POOL 3).
**STOP:** `test_nr_gpu_pass` passes serially; N=1 gate green; with two cells on a replay, per-cell LLR/TB bytes equal the solo run (bit-exact, as GrantSized equality).

#### Task G4: discovery across cells (single-flight ticket) and RT protection
**Files:** `nr_csirs_disc_gpu*.cu`, `nr_gpu_chunk.h`, `nr_gpu_budget.c` (ticket + budget), `nr_pdcch_nid_gpu.c`, `tests/passive_rx/gpuspan/disc_unit_bench.cu` (add 2-cell launch-block mode).
**Steps:** [ ] first re-run the launch-block bench with the 32-unit floor (about 27 ms chunks) at 1 and 2 tickets: record the RT launch gap p99 and the count > 1 ms / > 5 ms `[Bench, DGX GB10 idle]`; [ ] implement the ticket (ACQUIRE first, then fair share), duty budget; [ ] optional `c_refine` over 512 units only if the ticket wait dominates in the 2-cell replay; [ ] replay 2 cells (c3610 pair) and compare discovery totals with the solo runs.
**STOP:** bench: no RT graph launch > 5 ms attributable to discovery at 2 cells; replay: FEP slots (sum over cells) >= 80 % of the sum of the solo runs, no detect batch > 3 s per cell, `CHUNK_SLOW` lines near zero (criteria of DISC-CONFIRM section 7), `ULSTALL gpu_us > 100 ms` = 0. On air confirmation is gate O2.

#### Task G5: GPU channelizer in the receiver context
**Files:** `radio/COMMON/shm_iq_chan_gpu.{cu,h}`, `shm_iq_chan_kern.h` (kernels from T0 import), registration into `iq_chan_backend_t`, SM/stream placement per B.6 item 2.
**Steps:** [ ] correctness: `--selftest --gpu` max 1 LSB / 129 of 786432 differ was the old figure; require the same or better; [ ] idle bench 245.76 MS/s 1, 2, 4 cells vs the MPCIFIX table; [ ] in-context bench under a 2-cell decode replay; [ ] choose placement (a) or (b) by the criterion in B.6.
**STOP:** 0 overruns over 10 min with 2 cells decoding on replay; cell p99 within +10 % of the pre-channelized-file control; the chosen placement and the measured work-per-ms-of-signal recorded `[MEASURED, DGX GB10 ...]`.

#### Task G6: radio at 245.76 MS/s in the single process (`usrp_lib`)
**Files:** `radio/USRP/usrp_lib.cpp` and the ring producer; the findings of inventory row 4.
**Steps:** [ ] 4 RX at 245.76 MS/s over one QSFP link, receiver alone (no cells), 150 s then 10 min: overflows / timeouts / resyncs; [ ] frozen X410 stamp handling; [ ] ring size by RAM; [ ] multi-channel count mismatch carries excess.
**STOP:** 0 overflows / timeouts / resyncs over 10 min `[MEASURED, DGX+X410 34B2AF1, ...]`. (The second QSFP link is not configured on the host [MEASURED 2026-10-16]; not needed at 200 MHz.)

#### Task G7: 2-cell replay on the DGX, single process (gate V3)
**Files:** `tests/passive_rx/dgx/mpci_replay.sh` (new, DGX group).
**Steps:** [ ] capture pair: c3610 = WindTre PCI 508 + Iliad PCI 403 at 122.88 MS/s (the one existing capture with two operators simultaneously [MEASURED, DGX replay csd/mpci-int, l0942_c3610x8]); [ ] arms interleaved: solo 508, solo 403, both in one process; n >= 3 per arm; [ ] per-cell P1/P3 evaluation; [ ] GPU `check_bad` = 0.
**STOP:** P1 and P3 pass for both cells, or the report names the binding resource (cores, GPU instances, channelizer, discovery) with the CORE_POOL / MPCI_CELL lines.

#### Task G8: UL GPU items for multi-cell (C2/C3 GPU halves) and GPU PDCCH hooks
**Files:** `nr_gpu_pass_ul.*`, `nr_gpu_pass_graph.cu`, `nr_pusch_gpu_*`: the 1024-item data-ID batch and the keyed UL HARQ slab (contract from UL-C2/UL-C3). Timing is judged only on air. PDCCH on GPU (B3/B5 of target-gpu-pipeline) is NOT in this plan: PDCCH stays on the CPU (PROJECT_MEMORY 2026-10-17 note 6) and its per-cell cost is measured in V3.

#### Task G9: OTA campaign (gates O1-O3)
Orchestrator-run; see Section D. Env file: the frozen `FROZEN_20261018.env` family is not reused; a new OTA env derived from `POOL_20261021.env` minus the dead keys and minus every hardcoded core, named `MPCI_<date>.env`, passed explicitly.

### Phase (iii) — sens6 (group S; after A-groups merged, own host, no X410 work by cloud)

Facts [CODE-READ, SENS6-READINESS-20261021]: the discrete GPU is refused at the platform gate (`nr_gpu_platform.c:171-187`), so every GPU decode module reports not ready and the receiver runs the CPU DL decode path there; the default `89;121` architecture list fails on nvcc 12.4.131 (`cudaGetDriverEntryPointByVersion` needs >= 12.5, `nr_gpu_pass_graph.cu:1099`; green contexts compiled out < 12.5); the 34-SM split skips split-2; hardcoded DGX cores break the pool (`taskset -c 0-7` gives N = 3); 14 GiB RAM with `mlockall` and 4 GiB hugepages is the tighter budget. sens6 stage numbers: [MEASURED, sens6 snapshot 2026-09-30] CPU 14 cpus `isolcpus=2-3`, RTX 4060 Ti 8188 MiB, driver 580.178.04.
- **S1** (after B2, B4): sens6 build (CPU-only: `ENABLE_LDPC_CUDA=OFF`, or nvcc >= 12.5 with `LDPC_CUDA_ARCH=89`), ctest, the 106-PRB rfsim bed 4 RX with the derived core plan (log `CORE_POOL` N, `GPU_PLATFORM DISCRETE ... CPU fallback`). STOP: N=1 equivalence on sens6 rfsim; pool N logged as derived (not 3-5 by accident).
- **S2** (after A8, E1): 2-cell CPU-path simulation on sens6 at <= 106 PRB per cell (file publisher fixture, no radio). STOP: both cells sync and decode in one process within RAM; P3 reported; labelled `[SIM VERIFIED, sens6 x86_64 RTX 4060 Ti, CPU decode path]`.
- **S3** (after DGX V3, own plan): discrete-GPU backend for the pass (DISCRETE memory model, per-grant PCIe copies, fake-device tests). Out of this plan's merge gate unless Decision F7 says otherwise.

---

## D. Validation gates (each stage must pass before the next; results never merged across cells/hosts/BW)

| Gate | Host / bed | What must hold | Evidence label |
|---|---|---|---|
| V0 | cloud x86 unit | all A/B/C/D unit tests, audit tool clean, TSAN/ASAN clean on pool, ring, channelizer; ctest only the known failures | `[OFFLINE VERIFIED, cloud x86]` |
| V1 | cloud x86 multi-cell bed (if C3 allows) | 2 cells in one process reach SIB1; per-cell CRC-OK within solo spread; N=1 equivalence suite green | `[SIM VERIFIED, cloud x86, bed size ...]` (rfsim CRC is an upper bound) |
| V2a | DGX rfsim, 4 RX, 106 PRB, idle host | the unchanged single-cell gate of `rfsim_regress.sh` (Global Constraint 5): mandatory after every merge of A/B groups | `[MEASURED, DGX rfsim 106 PRB 4 RX, host idle, @<commit>]` |
| V2b | DGX rfsim, 2 gNB x 106 PRB (cells at distinct PCI), idle host | both cells meet the same per-cell gate as V2a (postconv, ttc, CRC floor, drop_full); P3 cores | same label per cell |
| V3 | DGX replay, c3610 pair (508 + 403), then (b, tile3 pairing only as separate solos: 415 and 923 captures are not simultaneous) | P1 and P3 per cell; discovery criteria of G4; `check_bad` 0; 0 channelizer overruns | `[MEASURED, DGX replay, cell PCI .. ]` per cell |
| O1 | DGX OTA, **single operator, 2 PCIs** | WindTre PCI 508 + PCI 4 (co-channel, one SSB frequency, zero shift, no channelizer) if PCI 4 is confirmed real by the front end (its evidence is the weakest: SSS gap 2.15, DMRS ratio 2.24 on one capture [MEASURED, DGX, FRONTLIVE]); otherwise the inventory decides the pair. Per-cell P1 against that cell's own interleaved solo; both cells decode; 30 min; daytime not night | `[MEASURED, DGX+X410 34B2AF1, daytime, cell PCI .. n=..]` |
| O2 | DGX OTA, **2 operators, 122.88 MS/s** | WindTre 508 + Iliad 403 (55 MHz apart, fit one window [MEASURED, FRONTLIVE capture c3610]); channelizer in context; P1, P3; discovery criteria; then add the third cell if the inventory has one | same |
| O3 | DGX OTA, **2 operators, 200 MHz (245.76 MS/s)** | Vodafone 415 (3680.01) + TIM 923 (carrier centre 3759.99): carriers span about 168 MHz [INFERRED from -C and N_RB], so 245.76 MS/s; the heavy pair; P1 per cell, P2 reported, P3; 0 publisher overflows and 0 channelizer overruns over 30 min; daytime | same |
| O4 | later | 400 MHz: only after O3; `csd/mcell-spec` F1-F8 | not in this plan |
| S-gate | sens6 | S1, S2 as in phase (iii) | `[SIM VERIFIED, sens6 ...]` |

Rules for every gate: interleave A/B arms in the same hour (cell load varies by hour: night runs are LOW LOAD and provisional; daytime numbers are not compared with night numbers); n >= 2 per arm per cell, 3 for decisions; per-cell table with PCI, centre frequency, bandwidth, host, load class; no OTA validation at night load for a pass/fail (user policy `no-cell-tuning`); no tuning on a specific cell, a fix must be validated on all cells/captures. OTA arms follow the X410 hygiene: SIGINT only, `/dev/shm` clean, the claim released.

Stop-the-line conditions (any gate): N=1 gate fails; a new ctest failure; `check_bad` > 0; any cell's CRC_q < 95 % of its solo outside spread; a cloud agent edited a G-owned file; a frozen sens6 path changed.

---

## F. Open decisions for the user (each with a recommendation)

| # | Decision | Options | Recommendation |
|---|---|---|---|
| F1 | Per-cell state mechanism | (a) thread-local cell index + cell-indexed statics + per-cell threads (B.2); (b) pass a `cell*` through every function; (c) keep process-per-cell for CPU and share only GPU | (a). (b) is a 25 kLOC rewrite with high N=1 drift risk; (c) reintroduces a second CUDA context. The audit tool (A2) is the safety net; if it shows far more PER_CELL objects than expected, revisit before A3. |
| F2 | Slot thread per cell or one shared | (a) one RT slot thread per cell (own core each); (b) one RT thread drives all cells' slot clock | Start with (a) because it keeps each cell's logic unchanged; measure P3. Switch to (b) only if RT cores are the binding limit at N=2 (each RT core is a core of the 25 % budget). |
| F3 | Float lane merge into the pool in mpci-main | merge now / keep the dedicated lane / defer | Merge now (Task B5): the lane is about 8 threads per cell and would scale by N. Keep the optional DGX follow-up (home set 13,14) out of the first gate; use the host-size rule of FLOAT-LANE-POOL 5. |
| F4 | Channelizer placement | RT context top priority / own SM group / separate stream priority only | Decide by measurement in G5; default to the top-priority stream in the RT context until it fails the criterion. |
| F5 | Log attribution | prefix every line with `[c<idx>]` / split per-cell files / structured JSON only | Prefix (one stdout, parsers need a regex front); the launcher can still split files. |
| F6 | Cloud validity of multi-cell rfsim | accept small beds / larger cloud host / DGX only | Ask for a >= 16 vCPU cloud x86 host for V1 if C3's first step shows the 4-cpu host cannot lock even a 51-PRB bed (CORE-POOL: cloud host "too slow to lock PDCCH" on its bed). Otherwise V1 is skipped and V2b carries the 2-cell functional gate. |
| F7 | sens6 scope | CPU-path 2 cells <= 106 PRB only / discrete-GPU backend now | CPU-path only for the first merge gate (S1-S2). The discrete backend (S3) is a separate stage with its own plan, after DGX goals; sens6 8 GB leaves room for roughly one process of one to two small cells [INFERRED, SENS6-READINESS 3]. |
| F8 | Which pair for O1 (single operator, 2 PCIs) | WindTre 508 + 4 co-channel / TIM 923 + 675 / another from the inventory | 508 + 4: same grid, zero shift, no channelizer, tests per-cell state in isolation. TIM 923 (3731.52) and 675 (3426.24) are about 305 MHz apart and cannot share a 200 MHz window [CODE-READ, site table]. If PCI 4 is not confirmed real, use the inventory's pair and say so. |
| F9 | Push of old branches for the cloud session | push `csd/mpci-int` / push `csd/mpcifix` (superset, 45 commits) / import files (T0) and push only `mpci-main` | T0 import: only `mpci-main` needs pushing; the cloud session works from `origin/mpci-main` and finds the old sources under `ref/mpci-old/` and the old reports under `docs/superpowers/reports/csd/`. `csd/mpci-int` is NOT required. If the user wants the history for blame, push `csd/mpcifix`, not `csd/mpci-int` (it is a strict ancestor of mpcifix; `csd/mpci` is an ancestor of both). |
| F10 | Merging main into mpci-main | each main push / weekly / at gates | After each push of `adaptive-rx-UL-DL`, by the orchestrator, followed by A9 and V2a; conflicts resolved on `mpci-main` only. |
| F11 | Per-cell acceptance numbers | P1 relative limits as written / absolute only | Both, as in Global Constraint 6; P2 binding for merge to a release branch. |
| F12 | Discovery duty budget | 30 % (M3) / derived | Start at 30 % of the discovery context wall time summed over cells, re-derive from G4 measurements; no per-cell value. |

---

## Self-review notes

- Spec coverage: inventory (A) covers csd/mpci, mpci-int, mpcifix, mcell-spec, dashboard, disctime, the four old plans, the 2026-10-13 prompt; architecture (B) covers per-cell state, one shared pool with per-cell fair share, one context, GPU sizing, memory from grant-sized buffers, discovery budget and RT protection across cells, host-derived cores; tasks (C) split by group with stop conditions; gates (D); prompt (E, appendix); decisions (F).
- Known gaps stated, not hidden: the single-process refactor size is estimated by regex counts until A2; the 5 ms RT-launch bound was shown for 1 ms chunks, not for the 32-unit floor (G4 first step); no per-cell GPU number exists until G1; the 200 us decode-token-wait target is not met at N=3 on cloud and N=6 is an extrapolation [Bench, cloud x86, CORE-POOL]; channelizer-in-context cost is unmeasured; the cloud host may not be able to lock any rfsim bed (C3 first step).
- Names consistent across tasks: `nr_cell_idx`, `NR_CELL_STATIC`, `nr_cell_path`, `nr_gpu_cell.h`, `nr_cell_admit`, `nr_cell_mgr`, `iq_chan_backend_t`, `nr_gpu_sm_plan.h`, `MPCI_ADMIT`, `MPCI_CELL`.

---

## E. Appendix — ready-to-paste prompt for the cloud session (task group (i), group C)

```
You are working on the passive, fully blind, cell-agnostic 5G NR receiver (OAI UE fork), repository passive_agnostic_rx_for_sensing.
Host: cloud x86 (no GPU, no X410, no IQ captures). Every number you report is labelled with the host name "cloud x86".

BASE: origin/mpci-main. Create task branches mpci/c-<task> from it and merge them back into mpci-main only through the
orchestrator. Do NOT push to adaptive-rx-UL-DL, ever. Do not push mpci-main yourself unless the orchestrator says so.

READ FIRST, in this order and completely: CLAUDE.md; PROJECT_MEMORY.md sections 0.1 (evidence labels), the header, and section 25
(latest blocks 2026-10-17, 2026-10-18/19, 2026-10-20/21, 2026-10-22); docs/superpowers/plans/2026-10-23-mpci-main-plan.md (YOUR
PLAN: inventory, architecture, file ownership, tasks A1-A9, B1-B5, C1-C3, D1-D2, E1, UL-C1..C4 and their STOP conditions);
docs/superpowers/reports/csd/CORE-POOL-20261020.md, FLOAT-LANE-POOL-20261021.md, GRANT-SIZED-20261021.md,
SENS6-READINESS-20261021.md; ref/mpci-old/ (the old multi-PCI sources, reference only, not built) and the MPCI-*.md,
MPCIFIX-20261015.md reports in docs/superpowers/reports/csd/.

HARD RULES
- Plan group C only (cloud). You may edit ONLY the files in the "Group C" row of the plan's File ownership table. Never edit any
  .cu/.cuh file, nr_gpu_*, nr_pdsch_gpu_*, nr_pdcch_nid_gpu*, nr_csirs_disc_gpu*, nr_gpu_budget*, nr_gpu_green.h, radio/USRP/*,
  radio/COMMON/shm_iq_chan_gpu.*. Shared seams (common/utils/nr_cell.h, nr_gpu_cell.h, nr_cell_admit.h) are created by you in
  Task A1/B3 and then frozen; further changes need a handover note in the plan.
- sens6 is FROZEN: never edit tests/passive_rx/captures/*, tests/passive_rx/*.conf, tests/passive_rx/sens6_host_snapshot_2026-09-30/*.
  Check before every commit: git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30
- New config files under tests/passive_rx/ use the .cfg extension.
- gNB logs and configs are validation ground truth only, never receiver input.
- Evidence labels (PROJECT_MEMORY section 0.1) on every claim, and the host name: [OFFLINE VERIFIED, cloud x86], [SIM VERIFIED, cloud x86, ...],
  [Bench, cloud x86, 4 cpus ...], [CODE-READ]. Never claim a DGX, sens6 or OTA result. Never merge results across cells, hosts or bandwidths.
  rfsim CRC is an upper bound.
- No flags: no new ISAC_* product flags. Test-only seams use the NR_TEST_ prefix and are documented in the task.
- N=1 equivalence: with one cell the behaviour must equal adaptive-rx-UL-DL. Run the Task A9 suite on every PR.
- Never build while nr-uesoftmodem runs (pgrep -x nr-uesoftmodem). Stop receivers with SIGINT, never SIGKILL. ISAC_RX_BRANCH_FO stays unset.
- git add <explicit paths>; never -A; never stash. Commit trailer: Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
- Build (cloud x86): cmake_targets/ran_build/build, Ninja, -DOAI_USRP=OFF -DENABLE_TESTS=ON:
  ninja nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests && ctest -j4
  Known ctest failures on main: nr_td_sim_test, test_nr_ssb_rate_match_prod, test_nr_modulation, dft_test, test_nr_pusch_ra0_*;
  nid_worker, pdsch_gpu_demod and similar GPU tests fail on a host without a GPU. Anything else failing is yours.

ORDER (stop at the first failed STOP condition and report; do not widen scope)
 1. Task A1 (nr_cell seam), A2 (static audit + table; write the exact counts back into the plan), UL-C4 (UL log split) before A6.
 2. B1/B1b (pool fair share), B2 (host-derived cores), B3 (admission model), B4 (SM plan pure function), C1 (ring), C2 (CPU channelizer),
    C3 (synthetic two-cell IQ; FIRST measure the largest rfsim bed this host can lock and report it).
 3. A3..A8 (module conversion in dependency order), A9 on each, D1 (cell manager + inventory), D2 (attribution + dashboard).
 4. E1 (two-cell cloud bed) if C3 allows; UL-C1..C3 (UL CPU reference) last, checking first whether each item already exists on mpci-main.

DELIVERABLES per task: commits on its branch, unit tests, a short report docs/superpowers/reports/csd/MPCI-<task>-<date>.md with the
evidence labels, the STOP-condition result, files changed, and the exact steps the DGX orchestrator must run next (commands,
expected log lines). Keep a running "Task log" appended to the plan's end only for seam changes.
```

---

## R. Revision 2026-10-25 (baseline `adaptive-rx-UL-DL` @ `c300718765`, merged into `mpci-main` @ `2f752307f2`)

Why: main received the sens6 lab-OTA work (`a383a08785`): topology core layout, host-RAM budget, GPU elastic sizing, per-path GPU/CPU choice, Q4 discrete-GPU entries, blind DCI 1_1 fixes. Operator rule (CLAUDE.md, PROJECT_MEMORY §26, 2026-10-25): **full agnosticity and full portability** — every size, core, path and transport is derived from the device at run time and exploits its peak; host values are configuration, never code; a fix on one host must not change the others. Facts below are `[CODE-READ, mpci-main @2f752307f2]` unless labelled.

R1. **Branches.** `adaptive-rx-UL-DL` -> `mpci-main` by merge only; multi-PCI code never goes to `adaptive-rx-UL-DL`. Main @ `c300718765` is NOT validated on the DGX (PROJECT_MEMORY 15.00): any DGX N=1 reference is taken on main and mpci-main in the same session, interleaved.

R2. **N=1 gate.** The postconv gate ("Technique D CONVERGED") is unreachable on the baseline on both hosts (PROJECT_MEMORY 10-23/24 §2; 15.00 `[MEASURED, sens6 rfsim]`). V2a becomes RELATIVE: same host, same bed (106 PRB, 4 RX, 420 s), arms main vs mpci-main interleaved, n >= 2, scored with `tests/passive_rx/dgx/score_rx_solver.py` (CRC, drops, DL p50/p99) plus the unit-level equivalence of A9. If a re-based gate is adopted on main, it replaces this.

R3. **B2 rewritten: extend `nr_core_layout`, retire `nr_core_plan`.** `common/utils/nr_core_layout.{c,h}` (SEP / COMPACT / SMALL by W, topology, isolcpus, capacity, L3, GPU NUMA, DPDK lcores, `SENSING: CORE_PLAN` line, `nr_core_layout_init` in `main()`) already removes the hardcoded cores. `nr_core_plan.{c,h}` is called from no production code. B2 = (a) add the cell count to the pure builder: `nr_cl_build_n(topo, over, n_cells, layout)` with one RT reader per cell (Decision F2 (a)) taken from the RT/isolated set, then big CPUs; pool and lane shared; N=1 output byte-identical to `nr_cl_build`; (b) per-cell RT readers in the plan line; (c) `nr_core_plan.*` removed only after a grep shows no user (its tests move to `nr_core_layout` fixtures). The B.5 "removal list" is mostly done by the layout; B2 removes what remains (grep gate for literal cores in group-C files).

R4. **B3 admission builds on the budgets on main.** RAM: `nr_host_membudget` (B = 0.8 x min(MemAvailable, cgroup) - reserve; fixed commits first, scalable shares, HOST_MEM REFUSE). GPU: `nr_gpu_membudget` + `nr_gpu_elastic`. Admission adds the CELL dimension: a cell is served iff its fixed commits + floors of its scalable modules fit what is left; the scalable share is split across served cells by weight; a refused cell is `unserved reason=ram|gpu_mem|...`, the process never exits because of cell k > 0. No constant of the DGX or sens6 in the model; fixtures inject both hosts.

R5. **GPU tier and per-path choice are process-wide.** `nr_gpu_platform` (SHARED / DISCRETE / NONE) and `nr_gpu_path` (one choice per path per process, trial samples from any cell) are shared by all cells; no per-cell path choice. The channelizer GPU backend registers as one more path (`CHAN`) in `nr_gpu_path` (SHARED: GPU; DISCRETE: trial vs the CPU backend; NONE: CPU), replacing the "capacity table" of B.3.

R6. **SM plan.** `nr_gpu_green.h` already derives discovery SMs = max(1, SMs/6); the literal `>= 32` split threshold and the probe group remain. B4 stays a pure function; G2 replaces the literal by the ratio rule. On DISCRETE without green contexts (CUDA < 12.5) the plan returns "no green".

R7. **sens6 scope (F7 changed).** sens6 now has a DISCRETE GPU path (Q4, per-path trials) and the topology core plan. S1/S2 run whatever the per-path choice picks (not CPU-only). S3 is covered by `2026-10-23-discrete-gpu-backend.md` on main.

R8. **Radio (G6).** Transport differs per host (DGX: DPDK; sens6: kernel sockets, NIC IRQs on housekeeping CPUs only, preflight `IRQ_CPUS`); the ring size comes from the host-RAM plan (`nr_host_membudget_usrp_ring_ms`); ADC full scale from the device (`32b9ad5e71`). The multi-cell wideband ring uses the same plan, never a fixed 2^27.

R9. **Files under active decode work.** `nr_pdcch_blind_monitor_rt.c` (+931 lines), `nr_pdcch_dci11_layout_sweep.c` (new), `nr_pdsch_solver.c`, `nr_pusch_passive_decode.c` changed in the merge and the user is still working on decoding. Order: A1, A2, B1 (pool only), B2, B3, B4, C1, C2, D1 first (new files or pool/layout only); A3-A7, B1b, B5 after the decode work settles; the conversion stays declaration-only (`NR_CELL_STATIC` keeps names) to keep merges small.

R10. **OTA gates.** WindTre/Iliad had 0 C-RNTIs solved on main before the blind DCI fixes (PROJECT_MEMORY 10-23/24 §3); whether `c9e8858e4b`/`5432bc95a1` fix that on the DGX is unknown: O1/O2 start with a solo check of each cell on the baseline.

R11. **Hosts.** Group C runs locally on the DGX (Sonnet agents in worktrees `wt/rr-mpci-<task>`, own build dirs, `nice -n 19`, `flock -s`); labels name `DGX GB10`, not cloud. Commit trailer of agents: their own model.

### A2 result (2026-10-25, branch mpci/c-a1a2, baseline mpci-main @9b2d7c3f6e) `[OFFLINE VERIFIED, DGX GB10 aarch64 unit; CODE-READ scanner]`

`tools/mpci/static_audit.py` scans 97 files in < 1 s (limit 60 s) and `tools/mpci/mpci_statics.tsv` holds one row per static/global/thread-local object (file, line, symbol, kind, type, class, owner, note). These exact counts replace the regex estimates of B.2 (B.2 text unchanged). `--check` (new object => fail) is in ctest (`static_audit_check`); `--strict` (UNCLASSIFIED => fail) is implemented but NOT enabled in ctest yet, it becomes the gate once A3-A8 are done. A converted object (`NR_CELL_STATIC*`) stays in the table as `[converted]`.

Totals: 2169 objects; by class: PER_CELL=0, PROCESS=80, CONST=343, TLS=181, UNCLASSIFIED=1565; by kind: file_static=1466, func_static=274, global=248, tls=181.

**PER_CELL candidates = UNCLASSIFIED rows owned by A3-A8: 1259** (A3=130, A4=303, A5=272, A6=200, A7=74, A8=280); G-owned UNCLASSIFIED (read-only for group C, converted behind `nr_gpu_cell.h`): G1=58, G3=199, G4=49. Not every candidate is PER_CELL: the A3-A8 owners classify each row (PER_CELL / PROCESS / CONST) as they convert. Classes already assigned automatically: PROCESS=80 (core pool, core layout, host/GPU membudget, GPU platform/path registry, GPU pass instance pool/graph cache by name, log/dashboard by name), CONST=343 (truly const objects only; `static const char *p` stays mutable), TLS=181 (existing `__thread`). Owner is by file; A8 also takes the generic `openair1/PHY/TOOLS` DSP/scope files; `radio/COMMON` is G5.

Exact count per file (total | PER_CELL PROCESS CONST TLS UNCLASSIFIED):

```
common/utils/nr_core_layout.c                                              13 | 0 11 2 0 0
common/utils/nr_core_pool.c                                                12 | 0 6 1 5 0
executables/nr-ue.c                                                        46 | 0 0 1 0 45
executables/nr-uesoftmodem.c                                               16 | 0 0 1 0 15
openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu                                41 | 0 0 12 0 29
openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu                            41 | 0 0 12 4 25
openair1/PHY/NR_UE_TRANSPORT/dci_nr.c                                      31 | 0 2 0 18 11
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c                               36 | 0 1 0 0 35
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_backend.c                        7 | 0 0 0 0 7
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c                           6 | 0 0 1 0 5
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_fp.c                             8 | 0 0 1 0 7
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu                          25 | 0 0 1 1 23
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_cluster.cuh                  6 | 0 0 3 0 3
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_sweep.cu                     5 | 0 0 1 1 3
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_hyp.c                            7 | 0 0 2 0 5
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_sweep.c                          3 | 0 0 2 1 0
openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c                            15 | 0 0 0 0 15
openair1/PHY/NR_UE_TRANSPORT/nr_dl_traffic_probe.c                         12 | 0 0 0 0 12
openair1/PHY/NR_UE_TRANSPORT/nr_dl_traffic_probe_rt.c                       3 | 0 0 0 0 3
openair1/PHY/NR_UE_TRANSPORT/nr_gpu_isa_ref.c                               4 | 0 0 0 0 4
openair1/PHY/NR_UE_TRANSPORT/nr_gpu_isa_twin.cuh                            1 | 0 0 1 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c                              21 | 0 3 11 3 4
openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu                          54 | 0 4 20 5 25
openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_post.cuh                           4 | 0 0 0 0 4
openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c                              15 | 0 0 3 0 12
openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.cuh                             2 | 0 0 2 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c                        12 | 0 0 1 0 11
openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c                        24 | 0 0 0 1 23
openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c                           6 | 0 0 0 0 6
openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics_json.c                      4 | 0 0 4 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c                              19 | 0 0 0 0 19
openair1/PHY/NR_UE_TRANSPORT/nr_passive_reconf_rules.c                      6 | 0 0 2 0 4
openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c                   10 | 0 0 0 0 10
openair1/PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.c                           34 | 0 0 1 0 33
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.c                             1 | 0 0 1 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c                      86 | 0 1 16 2 67
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c                  157 | 0 1 3 19 134
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_phase2.c                        8 | 0 0 0 0 8
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c                3 | 0 0 0 0 3
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.c                       12 | 0 0 0 0 12
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c                        15 | 0 0 2 0 13
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci01_layout_sweep.c                 15 | 0 0 15 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.c                 33 | 0 0 31 0 2
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c                    4 | 0 0 0 0 4
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_fep.cu                           24 | 0 0 8 0 16
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_parity.cuh                        2 | 0 0 2 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_joint_live.c                          7 | 0 0 0 0 7
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c                             4 | 0 0 0 0 4
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.cu                            7 | 0 0 7 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.c                      30 | 0 0 1 0 29
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_sib1_prior.c                          2 | 0 0 0 0 2
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_config_disc.c                     13 | 0 0 1 0 12
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_discovery.c                        6 | 0 0 0 0 6
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c                      2 | 0 0 2 0 0
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.c                     7 | 0 0 3 0 4
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c                     32 | 0 0 2 0 30
openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_tracker.c                        22 | 0 0 2 0 20
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu                         28 | 0 0 16 0 12
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c                     37 | 0 0 1 8 28
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c                          26 | 0 0 0 2 24
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh                     8 | 0 0 3 0 5
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu                           77 | 0 0 17 0 60
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu                          35 | 0 0 14 0 21
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c                       6 | 0 0 1 1 4
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c                    205 | 0 0 10 80 115
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c                     135 | 0 4 2 7 122
openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c                             48 | 0 0 12 1 35
openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_fep.cuh                           3 | 0 0 2 0 1
openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c                     87 | 0 0 6 19 62
openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.c                 16 | 0 0 0 0 16
openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c                      40 | 0 0 0 0 40
openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_ul_ids.c                     11 | 0 0 0 0 11
openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.c                          12 | 0 0 0 0 12
openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_disc.c                             8 | 0 0 0 2 6
openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_batch.cu                             3 | 0 0 1 0 2
openair1/PHY/TOOLS/calibration_scope.c                                      2 | 0 0 2 0 0
openair1/PHY/TOOLS/dB_routines.c                                            6 | 0 0 6 0 0
openair1/PHY/TOOLS/dfts_load.c                                              4 | 0 0 0 0 4
openair1/PHY/TOOLS/get_sin_cos.c                                            1 | 0 0 0 0 1
openair1/PHY/TOOLS/invSqrt.c                                                1 | 0 0 1 0 0
openair1/PHY/TOOLS/lte_enb_scope.c                                          7 | 0 0 0 0 7
openair1/PHY/TOOLS/lte_phy_scope.c                                          6 | 0 0 0 0 6
openair1/PHY/TOOLS/lte_phy_scope_tm4.c                                      8 | 0 0 0 0 8
openair1/PHY/TOOLS/lte_ue_scope.c                                           7 | 0 0 0 0 7
openair1/PHY/TOOLS/nr_gpu_budget.c                                         20 | 0 19 0 1 0
openair1/PHY/TOOLS/nr_gpu_mem.c                                             2 | 0 2 0 0 0
openair1/PHY/TOOLS/nr_gpu_module.c                                          3 | 0 2 1 0 0
openair1/PHY/TOOLS/nr_gpu_path.c                                            9 | 0 7 2 0 0
openair1/PHY/TOOLS/nr_gpu_platform.c                                       14 | 0 13 1 0 0
openair1/PHY/TOOLS/nr_host_membudget.c                                      4 | 0 3 1 0 0
openair1/PHY/TOOLS/nr_phy_scope.c                                           5 | 0 0 3 0 2
openair1/PHY/TOOLS/oai_dfts.c                                              43 | 0 0 38 0 5
openair1/PHY/TOOLS/oai_dfts_neon.c                                        193 | 0 0 18 0 175
openair1/PHY/TOOLS/phy_scope_interface.c                                    1 | 0 0 0 0 1
openair1/SCHED_NR_UE/fapi_nr_ue_l1.c                                        2 | 0 0 2 0 0
openair1/SCHED_NR_UE/phy_procedures_nr_ue.c                                 4 | 0 1 1 0 2
radio/COMMON/common_lib.c                                                   1 | 0 0 1 0 0
```

R12. **NO FLAGS (operator, 2026-10-25).** Supersedes Global Constraint 4's test-seam clause: no new env var of any prefix (`ISAC_*`, `NR_TEST_*`), no new CLI option of the receiver or of run/scoring scripts, no behaviour-changing build switch. Tests use function hooks and injected inputs. Scorers on a multi-cell log score every cell automatically, one independent block per cell (no `--cell`). Existing env vars on main are untouched by this plan.

## Task log (seam changes only)

### D2 (2026-10-25, branch mpci/c-d2): process-wide cell count, LOG tag, nr_cprintf shim
- `common/utils/nr_cell.h/.c` (seam change, additive): `int nr_cell_count_` (default 1), `static inline int nr_cell_count(void)` (relaxed atomic load),
  `void nr_cell_set_count(int n)` (n outside 1..NR_CELL_MAX is fatal). The cell manager calls `nr_cell_set_count(N)` once, before the cell threads start.
- `common/utils/LOG/log.c` (one hunk in `log_output_memory`) and the new header `common/utils/nr_cprintf.h`: when `nr_cell_count() > 1`, every line of a
  record is prefixed `[c<nr_cell_idx()>] ` (before the colour start sequence; the colour end sequence of the previous record may therefore precede the tag on a
  line: parsers strip ANSI first). N=1: no code path changes, output byte-identical.
- `common/utils/LOG/CMakeLists.txt`: `nr_cell.c` is now compiled INTO the `LOG` library (log.c needs the TLS index and the count). Other targets must link `LOG`
  and must not list `nr_cell.c` again (duplicate symbols); `test_nr_cell` keeps compiling it itself (it does not link LOG).
- Threads never bound to a cell (radio threads) read index 0 and are tagged `[c0]`; GPU callback/CUDA event threads likewise. The dashboard parses the receiver-wide
  radio line types (RFCHAN, CHPOW, RFSTALL, RFTSDISC, RFCENSUS) by grammar, regardless of the prefix. Per-cell job lines printed from callbacks must name the cell
  explicitly (the job carries it).
- Metrics JSON (`nr_passive_metrics_emit`): `"cell":<k>` is inserted as the first key only when `nr_cell_count() > 1` (adding `"cell":0` at N=1 would change bytes).

### Cross-task sites found by A7 (2026-10-25, MPCI-A7-20261025.md) — must be closed by the named task
- A8: call `nr_csirs_disc_cell_prealloc(cell, fp, n_ant)` per active cell before the RX fence (replaces `nr_csirs_disc_start`, nr-uesoftmodem.c:583; release replaces the stop calls at :615/:728); the slot thread that calls `nr_csirs_disc_td_push` (phy_procedures_nr_ue.c:1523) must be bound to its cell.
- A3: the cfg-epoch authority must raise epoch listeners on the cell's own thread (A7's `epoch_listener` uses `nr_cell_idx()`).
- A3/D2: `nr_passive_metrics.c:241` -> `nr_csirs_disc_get_metrics_cell(k)` per cell.
- B-group (B1b/B5): `ISAC_CSIRS_DISC_CORE` / `nr_bg_lane_enter` are process-wide: N cell discovery workers would pin to one core; workers must lease from the pool (DISC class) per cell.
- G3/G4: one GPU discovery workspace (`gpu->prepare`, `sweep_prepare`) re-prepared by a second cell with another N_RB -> per-cell workspaces; `nr_gpu_membudget_commit(NR_GPU_MEM_CSIRS)` must sum the per-cell `gpu_bytes`; the GPU circuit breaker is process-wide (one cell's prealloc failure disables GPU discovery for all cells) — decide per-cell vs process; history arrays allocated lazily via CUDA unified memory possibly after the fence (pre-existing at N=1).

### Cross-task sites found by A3 (2026-10-25, MPCI-A3-20261025.md)
- Inventory row 12 (SIB1 sibling temp+rename) is OBSOLETE: main 50955696a8 removed the sibling-file feature.
- A8 must: pre-allocate per active cell (incl. cell 0) `nr_passive_reconf_rules_cell_prealloc(cell)` and `nr_passive_ue_ctx_cell_prealloc(cell)` (and A7's `nr_csirs_disc_cell_prealloc`) before `nr_cell_alloc_fence()`; call `nr_passive_metrics_set_pci()` per cell (nr-ue.c:1210 sets the cell-0 global only); open obs / ue_ctx / replay-capture sessions on the cell's bound thread; derive per-cell paths with `nr_cell_path`.
- Threads that call converted per-cell APIs and must be created with `nr_cell_thread_create` (or carry `cell` in the job): nr_dl_traffic_probe_rt.c:227 worker; PDSCH queue threads (nr_pdsch_passive_queue.c:2351, A5); PUSCH queue threads (nr_pusch_passive_queue.c:745, A6); PDCCH queue threads (nr_pdcch_passive_queue.c:355, A4); nid_worker / janon_worker / disc_worker in nr_pdcch_blind_monitor_rt.c (A4); nr_csirs_blind_rt.c, nr_prach_passive.c:447, nr_srs_passive.c:273 workers (A8 owner unless reassigned); cb0 pool workers nr_td_cb0_batch.c (G3, process-wide pool -> job carries cell); nr_sense_api.c writer (A8).
- Audit scanner limits seen in A3/A7: block-scope converted statics are not tabled; `NR_CELL_STATIC*` needs a trailing `;` or the scanner merges declarations. Follow-up for the tool (A2 owner): table block-scope objects and accept the macro without `;`.

### G1 (2026-10-25, branch mpci/g-g1): GPU memory ledger, nr_gpu_cell.h seam, one hand-over hunk in nr_cell_admit.c
- New `openair1/PHY/TOOLS/nr_gpu_memstat.{h,c}` (in libnr_gpu_platform) + `nr_gpu_memstat_cuda.cuh`: every GPU / pinned-host allocation is booked under (module, SHARED|PER_CELL, cell). `GPUMEM` line at the RX fence and at exit (grammar in the header).
- New seam header `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_cell.h` + `nr_gpu_cell.c` (pure C, no CUDA, so the cloud build gets real numbers instead of a stub): `nr_gpu_cell_bytes(params, kind)` / `nr_gpu_cell_mem` ({dev, pin}); the GPU allocators call the same inline sizing functions.
- HAND-OVER into C-owned files (minimal, B3 re-verified): `common/utils/nr_cell_admit.c` (G_HARQ_BYTES and the nID window/fixed model now call `nr_gpu_cell_mem`; G_NID_* hypotheses removed), `tests/passive_rx/mpci/admit_model.py` (same formulas mirrored), `common/utils/tests/test_nr_cell_admit.cpp` (sens6 nID window 6 -> 7), `cmake_targets/mpci_cloud.cmake` (nr_gpu_cell.c in the nr_cell_admit library).
- A cell-less PER_CELL scope resolves the cell by name (`nr_cell_tls_idx_`) on the calling thread: the module inits must run on the cell's own thread (A8) or the cell is passed explicitly. The nID resize runs on the worker thread and uses the cell stored at create.

### R13. Tasks added 2026-10-29 (from DL-HEALTH-20261029, PROJECT_MEMORY §31.1; operator decision)
- **A10 cell-identity check at sync:** a cell's sync must lock the PCI (and SSB index when known) of its cells.json descriptor: set the existing rx_sss_nr PCI pin from the descriptor (nr-ue.c:208 sets target_Nid_cell=-1 today), re-scan on mismatch; evidence 2/24 cell starts locked a co-channel PCI (415->380, 403->27) and decoded nothing. STOP: unit test with two SSBs; OTA 10/10 starts match cells.json.
- **A11 PDSCH recovery worker per cell:** `[c1] RECOV worker FAILED to start` in 8/8 two-cell runs (nr_pdsch_passive_queue.c:2604) -> no retry arms for cell > 0; log the failing step, make the worker per-cell correct, run the arms inline when no worker. STOP: "RECOV worker started" for every cell; DECODETDRA lines for c1; Vodafone as c1 crc_q within run-to-run spread of c0.
- **B6 pool vs N-cell demand (with admission):** 2-cell starvation (stale PDSCH 5.8-28 %, scan drop_full 8-49 %, wait p50 0.5-2.4 ms); size the pool by the admitted cells' demand within the host's cores, split long DMRS_SCAN jobs, drop low-value work first, admission weighs decode demand vs pool, CHAN demand recomputed on pause/resume. STOP: 2-cell OTA stale <= 2 %, drop_full <= 2.5 %, per-cell p50 within +30 % of solo; replays at SPEED 1.0.
- **A12 live cell changes applied:** cell add/remove/retune from cells.json applied at run time (today logged only), radio retune when a cell is outside the capture span.

### R14. Order after the integration OTA (operator decision 2026-10-29)
1. Integration OTA of mpci/int-1029 (cellfeat + A12 live + G4): 2-cell runs alternated with solo, live add/remove on air.
2. Admission vs pool: B6, with A10 (cell-identity check) and A11 (per-cell recovery worker) from R13.
3. Audit cleanup: widen the audit file list, classify the remaining G1/G4 rows, bind leftover pool tasks to their cell, GPU-check queue cell.
4. Validation gates: A9 N=1 suite, V3 2-cell replay gate, O1-O4 on air, S1-S3 sens6 / CPU-only / discrete GPU.
5. Push mpci-main (operator OK required).
6. Re-measure and fix PROJECT_MEMORY §31 (DL health, UL, DCI 1_1).
7. C3/G8 UL HARQ soft combining: only after the §31.2 UL fixes give a working UL pipeline (UL CRC decoding OTA); combining needs first transmissions that decode.
Update 2026-10-29 15:xx (operator): mpci/int-1029 merged into mpci-main (3110334ccb) on replay evidence; the integration OTA (item 1) is
deferred (host needed by another user; captures around 17:40 or later) and must run before item 6 (§31 fixes). Items 2-4 proceed now.

### Audit cleanup (2026-10-29, branch mpci/c-audit, MPCI-AUDIT-20261029.md) — seam changes and closed cross-task sites
- `common/utils/threadPool/task.h`: `task_t` gains `int cell` (additive; `pushTpool` stamps `nr_cell_idx()`, the worker binds before `func`). `thread-pool` links `LOG` (nr_cell.c). Closes "threads that call converted per-cell APIs ... cb0 pool workers" of the A3 list for the shared Tpool and the CB0 pool.
- `openair1/PHY/TOOLS/nr_gpu_bgq.h`: each queued item carries the pusher's cell; the worker binds to it and leases for it. Closes the "GPU-check queue takes its first caller's cell" item.
- `nr_csirs_disc_fp_prealloc/release` (history arrays before the fence); DCI 1_1/0_1 resolvers in `nr_pdcch_cell_prealloc`. Closes the A7 "history arrays allocated lazily ... possibly after the fence" item.
- `tools/mpci/mpci_audit_files.txt` + `static_audit.py --coverage/--add-coverage`: the audit covers every source of nr-uesoftmodem and its runtime libraries, and headers with state. `static_audit_check` in ctest is `--strict`.
