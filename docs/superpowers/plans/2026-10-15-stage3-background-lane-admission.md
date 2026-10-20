# Stage 3 — Background lane and admission control for speculative/discovery work — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Speculative and discovery work (anonymous DCI solver, USS tracker, nID GPU sweep, CSI-RS discovery, DM-RS id scans, PDSCH solver probes, UL timing/DM-RS-id discovery) never delays a real grant: it runs only on the background cores/GPU partition, only when the critical-lane queues are idle, and is dropped at its deadline — on both the DGX Spark and sens6.

**Architecture:** One small admission module (`nr_admit`) owns the "is the critical lane idle?" decision from the backlog getters that already exist, plus per-job deadlines; every background producer asks it before doing work and reports drops. Threads of the background lane get one core list and SCHED_OTHER/nice; the anonymous solver (`pdcchJanon`) stops floating onto the producer/PDSCH cores. UL discovery leaves the per-grant path and runs on a snapshot in a background worker, feeding the per-RNTI hypothesis cache introduced in Stage 1 Task 7. The GPU discovery partition becomes dynamic (fewer SMs while a large real grant is in flight).

**Tech Stack:** C11, pthreads, existing CUDA green contexts (`nr_gpu_green.h`), gtest, Python analysis scripts.

**Spec:** `docs/superpowers/reports/csd/SYSAN-20261015.md` (branch csd/sysan, 207264ca9a) §3 (CPU budget), §4 (GPU budget), §5 (speculation vs critical path), §7 points 1, 2, 5, 6, §9 (daytime OTA: scan cores 68-83 %, `pdcchJanon` 71 % floating onto cores 5/8/9, UL cores ~70 % with half inline discovery, 7-13 % of Vodafone DL grants waiting > 500 µs, UL queue wait p50 1.6 ms). Depends on Stage 1 Task 7 (`ul_hyp_cache`) for the UL part; all other tasks are independent of Stage 1.

## Global Constraints

- CLAUDE.md hard rules verbatim (frozen sens6 paths, `.cfg`, gNB logs ground truth only, evidence labels naming the host, never build while one of our receivers runs, SIGINT only, `ISAC_RX_BRANCH_FO` unset, explicit `git add`, never stash).
- Every change default-off; umbrella `ISAC_BG_LANE=1`. Flags off = csd/otatest bit for bit.
- No discovery loss beyond measurement noise: per operator, time-to-lock of CORESETs, RNTIs found, BWP lock target, CSI-RS confirmed resources must stay within the OFF arm's run-to-run spread (n ≥ 6 replays, n ≥ 3 OTA runs per arm per cell); report time-to-lock p50/p90 explicitly.
- Background threads: SCHED_OTHER, nice ≥ 10, cores from one list `ISAC_BG_CORES` (default DGX `"2,10,11,12,13,14"`; sens6 default = the cores not used by the critical lane, computed by `nr_core_plan`); never on the slot-producer core or a PDSCH/scan core.
- Critical lane is untouched in priority and placement except where a task says otherwise.
- Host commands as in Stage 1 (build under `flock -s heavy.lock`, own-receiver check by exe path, `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps`).
- Pass criteria on air (DGX daytime, per operator): fraction of DL grants with queue wait > 500 µs ≤ 2 % (today 7-13 % on Vodafone); UL real-grant queue wait p50 ≤ 300 µs (today 1.6 ms); slot→DCI-enqueued p50 not higher than t26f; discovery time-to-lock within spread.

## Review Focus

- Critical lane never idle for long (busy cell at peak): expected → background work still makes progress through a minimum duty floor (`ISAC_BG_MIN_DUTY_PCT`, default 5 %), so discovery cannot starve forever; time-to-lock bounded. Test in Task 1.
- Background job whose IQ snapshot expires while queued: expected → dropped before running, counted (`bg_expired`), no read of overwritten samples. Test in Task 1 and Task 5.
- Cell reconfiguration (ISAC_RECONF / SUSPECT RNTIs) while discovery is throttled: expected → reconfiguration-triggered discovery bypasses the idle gate (priority token), so recovery after reconfiguration is not delayed. Test in Task 2.
- Empty/unknown core list or a list overlapping critical cores: expected → refuse that core, log once, fall back to unpinned SCHED_OTHER nice 19. Test in Task 3.
- GPU large grant in flight when discovery SMs are reduced: expected → discovery kernels already launched finish (no preemption, no corruption); new discovery launches wait or run with fewer SMs. Test in Task 6.

---

## Code map (csd/otatest 26d3028b5d)

Backlog/busy getters that already exist: `nr_pdsch_passive_queue_backlog_real()` (T/nr_pdsch_passive_queue.c:2793, takes `g_lock`), `nr_pdcch_passive_queue_backlog()` (T/nr_pdcch_passive_queue.c:270), `nr_pusch_passive_queue_real_pending()` (T/nr_pusch_passive_queue.c:819; 0 unless `ISAC_UL_PRIO_QUEUE`), `nr_csirs_disc_td_pending()` (T/nr_csirs_disc_td.c:170), `nr_gpu_budget_rt_active()` (openair1/PHY/TOOLS/nr_gpu_budget.c:81).
Background producers: `pdcchJanon` (T/nr_pdcch_blind_monitor_rt.c:2840-2856, `janon_start` 3039, env `ISAC_PDCCH_JOINT_ANON_CORE`, `_Q`); `pdcchUssHash` (T/nr_pdcch_uss_tracker.c:350-380, `ISAC_PDCCH_USS_CORE`); nID GPU sweep worker (T/nr_pdcch_nid_gpu.c:255-285, `ISAC_PDCCH_NID_GPU_CORE`); `csirsDisc` (T/nr_csirs_disc_feed.c:470-495, `ISAC_CSIRS_DISC_CORE`); `dmrsScan` (T/nr_pdsch_passive_queue.c:522); `dmrsIdBg` (queue.c:1305, `nr_bgq_start`); PDSCH solver probes (`pdsch_gate_admit` T/nr_pdcch_blind_monitor_rt.c:1301-1330, `ISAC_PDSCH_SOLVER_PROBE_RATE/_BURST/_FILL`); UL discovery inline in `nr_pusch_passive_decode_inner` (T/nr_pusch_passive_decode.c:1867-1920 timing search, `ISAC_UL_DMRS_DISC` 1660; scorer `score_impl` T/nr_pusch_ul_disc.c:114). GPU discovery partition: `ISAC_GPU_DISC_SMS`, `nr_gpu_green_stream()` (T/nr_gpu_green.h:22); discovery slices `NrGpuDiscoverySlice` (openair1/PHY/TOOLS/nr_gpu_budget_cuda.h:10). Thread creation: `threadCreate` (common/utils/system.c, prio 0 → SCHED_OTHER), core plan `common/utils/nr_core_plan.c`.

## File structure

| file | responsibility |
|---|---|
| `openair1/PHY/TOOLS/nr_admit.h` / `.c` (new) | idle gate, deadlines, min-duty floor, priority tokens, counters |
| `openair1/PHY/TOOLS/tests/nr_admit_test.cc` (new) | unit tests of the gate |
| `common/utils/nr_bg_lane.h` / `.c` (new) | background core list, thread attributes, validation against critical cores |
| producers (modify, call sites only) | `nr_pdcch_blind_monitor_rt.c` (janon, probes), `nr_pdcch_uss_tracker.c`, `nr_pdcch_nid_gpu.c`, `nr_csirs_disc_feed.c`, `nr_pdsch_passive_queue.c` (dmrsScan, dmrsIdBg) |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.c` / `.h` (new) | UL discovery worker on snapshots → `ul_hyp_cache` |
| `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h` (modify) | dynamic discovery SM count |
| `tests/passive_rx/bglane_ab.sh` (new) | replay A/B per operator incl. time-to-lock extraction |

---

### Task 1: Admission module `nr_admit`

**Files:** Create `openair1/PHY/TOOLS/nr_admit.h`, `nr_admit.c`, `openair1/PHY/TOOLS/tests/nr_admit_test.cc`; modify `openair1/PHY/TOOLS/tests/CMakeLists.txt` (add `test_nr_admit` next to `nr_gpu_budget_test`).

**Interfaces (produces):**
```c
typedef enum { NR_ADMIT_JANON, NR_ADMIT_USS, NR_ADMIT_NID, NR_ADMIT_CSIRS, NR_ADMIT_DMRS_SCAN,
               NR_ADMIT_DMRS_ID, NR_ADMIT_PROBE, NR_ADMIT_UL_DISC, NR_ADMIT_N } nr_admit_class_t;
typedef int (*nr_admit_backlog_fn)(void);            /* >0 = critical work waiting */
void nr_admit_register_backlog(nr_admit_backlog_fn f); /* up to 8: PDSCH real, PDCCH scan, PUSCH real, GPU RT active */
int  nr_admit_on(void);                               /* ISAC_BG_LANE */
/* returns 1 = run now, 0 = skip (counted). deadline_ns = sample lifetime end; now_ns from CLOCK_MONOTONIC */
int  nr_admit_try(nr_admit_class_t c, uint64_t now_ns, uint64_t deadline_ns, int priority_token);
void nr_admit_done(nr_admit_class_t c, uint64_t busy_ns); /* feeds the duty accounting */
void nr_admit_stats(char *buf, int n);                /* "ADMIT janon[run= skip_busy= expired= floor=] ..." */
```
Rules: expired (`now ≥ deadline`) → 0 always; `priority_token` (reconfiguration) → 1; any backlog > 0 → 0 unless the class's duty over the last 100 ms < `ISAC_BG_MIN_DUTY_PCT` (default 5) → 1 (floor); else 1.

- [ ] **Step 1: Write the failing tests**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "PHY/TOOLS/nr_admit.h"
}
static int g_backlog = 0;
static int fake_backlog(void) { return g_backlog; }
class Admit : public ::testing::Test { protected: void SetUp() override {
  setenv("ISAC_BG_LANE", "1", 1); setenv("ISAC_BG_MIN_DUTY_PCT", "5", 1);
  nr_admit_reset_for_test(); nr_admit_register_backlog(fake_backlog); g_backlog = 0; } };
TEST_F(Admit, IdleRuns)      { EXPECT_EQ(1, nr_admit_try(NR_ADMIT_USS, 1000, 2000, 0)); }
TEST_F(Admit, BusySkips)     { g_backlog = 3; nr_admit_note_duty_for_test(NR_ADMIT_USS, 10); EXPECT_EQ(0, nr_admit_try(NR_ADMIT_USS, 1000, 2000, 0)); }
TEST_F(Admit, ExpiredDropped){ EXPECT_EQ(0, nr_admit_try(NR_ADMIT_NID, 3000, 2000, 1)); }
TEST_F(Admit, TokenBypasses) { g_backlog = 3; EXPECT_EQ(1, nr_admit_try(NR_ADMIT_JANON, 1000, 2000, 1)); }
TEST_F(Admit, FloorPreventsStarvation) {
  g_backlog = 5; int ran = 0;
  for (uint64_t t = 0; t < 100000000ull; t += 100000) {          // 100 ms of 100 us steps, always busy
    if (nr_admit_try(NR_ADMIT_CSIRS, t, t + 1000000, 0)) { ran++; nr_admit_done(NR_ADMIT_CSIRS, 100000); }
  }
  EXPECT_GE(ran, 40); EXPECT_LE(ran, 60);                         // ~5 % of 1000 steps
}
TEST_F(Admit, OffMeansAlwaysRun) { setenv("ISAC_BG_LANE", "0", 1); nr_admit_reset_for_test(); g_backlog = 9; EXPECT_EQ(1, nr_admit_try(NR_ADMIT_PROBE, 0, 1, 0)); }
```
- [ ] **Step 2:** `ninja test_nr_admit && ctest -R test_nr_admit` → FAIL (missing header).
- [ ] **Step 3:** Implement (atomics only, no locks on the hot path; duty = ring of 10 × 10 ms buckets per class; `nr_admit_reset_for_test` / `nr_admit_note_duty_for_test` exported under `#ifdef NR_ADMIT_TEST` via a test-only compile definition).
- [ ] **Step 4:** `ctest -R test_nr_admit` → 6 PASS.
- [ ] **Step 5:** `git add openair1/PHY/TOOLS/nr_admit.h openair1/PHY/TOOLS/nr_admit.c openair1/PHY/TOOLS/tests/nr_admit_test.cc openair1/PHY/TOOLS/tests/CMakeLists.txt CMakeLists.txt && git commit -m "feat(admit): idle-gated admission with deadlines and duty floor for background work (ISAC_BG_LANE, default off)"`

### Task 2: Register critical backlogs and gate the PDCCH-side producers

**Files:** Modify `T/nr_pdsch_passive_queue.c` (register `nr_pdsch_passive_queue_backlog_real` at pool start; make it lock-free: read an atomic counter maintained at enqueue/dequeue instead of `g_lock`), `T/nr_pdcch_passive_queue.c` (register scan backlog), `T/nr_pusch_passive_queue.c` (register `real_pending`), `openair1/PHY/TOOLS/nr_gpu_budget.c` (register `rt_active`); gate call sites: `pdcchJanon` job pop (rt.c ~2840), USS tracker round start (uss_tracker.c ~350), PDSCH probe admission (`pdsch_gate_admit` rt.c:1301 — `&& nr_admit_try(NR_ADMIT_PROBE, …)`), ISAC_RECONF SUSPECT path passes `priority_token=1`.
Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc` (existing target) — add:

- [ ] **Step 1: Failing tests**

```cpp
TEST(BgLaneGate, JanonSkipsWhenPdschBacklog) {
  setenv("ISAC_BG_LANE", "1", 1); nr_admit_reset_for_test();
  test_set_pdsch_backlog(4);
  EXPECT_EQ(0, janon_should_run_for_test(/*now*/1000, /*deadline*/50000000, /*suspect*/0));
  EXPECT_EQ(1, janon_should_run_for_test(1000, 50000000, /*suspect*/1));   // reconfiguration bypass
}
TEST(BgLaneGate, BacklogGetterIsLockFree) { /* 4 threads enqueue/dequeue 1e5 times; getter never blocks > 5 us (rdtsc) and ends at 0 */ }
```
- [ ] **Step 2:** run → FAIL. **Step 3:** implement (`janon_should_run_for_test` = thin wrapper over the new gate in rt.c). **Step 4:** `ctest -R test_nr_pdcch_blind_monitor` PASS. **Step 5:** commit with explicit paths: `git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.c openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c openair1/PHY/TOOLS/nr_gpu_budget.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_tracker.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc && git commit -m "feat(admit): gate anonymous solver, USS tracker and solver probes on critical backlog"`

### Task 3: Background core list and thread attributes

**Files:** Create `common/utils/nr_bg_lane.h/.c`; modify `common/utils/nr_core_plan.c` (expose `nr_core_plan_is_critical(int core)`), and the creation sites of `pdcchJanon`, `pdcchUssHash`, nID worker, `csirsDisc`, `dmrsScan`, `dmrsIdBg` to call `nr_bg_lane_thread_attr(&attr, idx)` when `ISAC_BG_LANE=1`.
**Interfaces:** `int nr_bg_lane_cores(int *cores, int max);` (parses `ISAC_BG_CORES`, drops critical cores, logs once), `void nr_bg_lane_thread_attr(pthread_attr_t *a, int idx);` (SCHED_OTHER, affinity = whole background list, nice applied in-thread by `nr_bg_lane_enter()`), `void nr_bg_lane_enter(void);` (`setpriority(PRIO_PROCESS, gettid(), 10)`).
Test: `common/utils/tests/test_nr_core_plan.cpp` (existing) — add:

- [ ] **Step 1: Failing tests**

```cpp
TEST(BgLane, DropsCriticalCores) {
  setenv("ISAC_BG_CORES", "2,5,7,10,11", 1);   // 5 = slot producer, 7 = PDSCH/scan
  test_mark_critical({5, 6, 7, 8, 9});
  int c[8]; int n = nr_bg_lane_cores(c, 8);
  EXPECT_EQ(3, n); EXPECT_EQ(2, c[0]); EXPECT_EQ(10, c[1]); EXPECT_EQ(11, c[2]);
}
TEST(BgLane, EmptyListFallsBackUnpinned) { setenv("ISAC_BG_CORES", "5,6", 1); test_mark_critical({5,6}); int c[8]; EXPECT_EQ(0, nr_bg_lane_cores(c, 8)); }
```
- [ ] **Step 2-4:** run FAIL → implement → `ctest -R test_nr_core_plan` PASS.
- [ ] **Step 5:** `git add common/utils/nr_bg_lane.h common/utils/nr_bg_lane.c common/utils/nr_core_plan.c common/utils/tests/test_nr_core_plan.cpp CMakeLists.txt <the six thread-creation files> && git commit -m "feat(bglane): one background core list; janon/USS/nID/CSI-RS/DM-RS threads off critical cores"`

### Task 4: Gate the GPU-side discovery producers (nID sweep, CSI-RS discovery, DM-RS id)

**Files:** Modify `T/nr_pdcch_nid_gpu.c` (worker loop ~255-285), `T/nr_csirs_disc_feed.c` (~470-495: batch pop), `T/nr_pdsch_passive_queue.c` (dmrsScan 522, dmrsIdBg 1305): call `nr_admit_try(class, now, sample_deadline, 0)` before each batch; on skip keep the newest batch only (ring overwrite policy) so the next admitted run uses fresh samples.
- [ ] **Step 1: Failing test** in `openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_disc_fp_test.cc` (existing): `TEST(BgLaneGate, CsirsKeepsNewestBatchWhenSkipped)` — feed 5 batches with backlog>0 and duty above floor, then backlog=0: exactly one run, on batch 5.
- [ ] **Step 2-4:** FAIL → implement → PASS (plus `GpuMatchesCpu` still PASS).
- [ ] **Step 5:** commit with explicit paths: `git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_disc_fp_test.cc && git commit -m "feat(admit): gate nID sweep, CSI-RS discovery and DM-RS id estimators; keep newest batch"`

### Task 5: UL discovery off the per-grant path

**Files:** Create `T/nr_pusch_ul_bgdisc.h/.c`; modify `T/nr_pusch_passive_decode.c` (1660 `ISAC_UL_DMRS_DISC`, 1867-1920 timing search: when `ISAC_UL_BGDISC=1` and the RNTI's `ul_hyp_cache` entry is missing/stale → submit `{rnti, snapshot ref (csd/ulq snapshot pool, ISAC_UL_SNAP_POOL), grant}` to the worker and decode the grant with the best cached/cell-default hypothesis or skip it (counted `ul_nohyp`); never run the search inline).
**Interfaces:** `int nr_ul_bgdisc_start(int depth); int nr_ul_bgdisc_submit(uint16_t rnti, void *snap_ref, const void *grant); /* worker: score_impl/search -> ul_hyp_cache_put(rnti, ta, dmrs_id, score) */`
Depends on Stage 1 Task 7 `ul_hyp_cache` (`int ul_hyp_cache_get(uint16_t rnti, ul_hyp_t *h); void ul_hyp_cache_put(uint16_t rnti, const ul_hyp_t *h);`). If Stage 1 is not merged yet, create `ul_hyp_cache` here with exactly that signature in `T/nr_pusch_ul_hypcache.h/.c`.
- [ ] **Step 1: Failing tests** `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pusch_ul_disc_test.cc` (existing `test_nr_pusch_ul_disc`):

```cpp
TEST(UlBgDisc, GrantNeverWaitsForSearch) { /* submit 100 grants for an unknown RNTI with the worker stopped: each decode returns within 1 ms and counter ul_nohyp == 100 */ }
TEST(UlBgDisc, WorkerFillsCache)         { /* run worker on a synthetic snapshot with known TA/DM-RS id: cache gets the true (ta, id) */ }
TEST(UlBgDisc, ExpiredSnapshotDropped)   { /* snapshot ref released before the worker runs -> bg_expired++ , no read */ }
```
- [ ] **Step 2-4:** FAIL → implement → PASS.
- [ ] **Step 5:** `git add openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.h openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.c openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pusch_ul_disc_test.cc CMakeLists.txt [nr_pusch_ul_hypcache.*] && git commit -m "feat(ul): UL timing/DM-RS-id discovery in a background worker feeding a per-RNTI cache (ISAC_UL_BGDISC)"`

### Task 6: Dynamic GPU discovery partition

**Files:** Modify `T/nr_gpu_green.h` (two discovery green contexts built at init: `ISAC_GPU_DISC_SMS` and `ISAC_GPU_DISC_SMS_LOW` (default half); `nr_gpu_green_disc_stream(int idx)` returns the LOW one while `nr_gpu_budget_rt_large_active() > 0`), `openair1/PHY/TOOLS/nr_gpu_budget.c` (`nr_gpu_budget_rt_large_begin/_end` called by the PDSCH path for grants > 96 PRB).
- [ ] **Step 1: Failing test** `openair1/PHY/CODING/tests/nr_gpu_green_test.cu` (existing `test_nr_gpu_green`, exit 77 = skip): `large_active` toggled → `nr_gpu_green_disc_stream` switches context; a kernel launched on the old stream before the switch completes and its output is intact.
- [ ] **Step 2-4:** FAIL → implement → PASS.
- [ ] **Step 5:** commit `git add openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h openair1/PHY/TOOLS/nr_gpu_budget.c openair1/PHY/TOOLS/nr_gpu_budget.h openair1/PHY/CODING/tests/nr_gpu_green_test.cu openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c && git commit -m "feat(gpu): fewer discovery SMs while a large real grant is in flight"`

### Task 7: Replay A/B, OTA, report

- [ ] **Step 1:** Create `tests/passive_rx/bglane_ab.sh` (copy of Stage 1 `gpupass_ab.sh` with arms `OFF = t26f`, `ON = t26f + ISAC_BG_LANE=1 ISAC_UL_BGDISC=1`, and the DGX `ISAC_BG_CORES=2,10,11,12,13,14`), plus `tests/passive_rx/bglane_ttl.py` extracting time-to-lock per run: first `CORESET_DISC state=LOCKED`, first `JOINT_ANON_HIT`, number of RNTIs at 30/60/150 s, `ULLAYOUT ... LOCKED` time, `CSIRS_BLIND CONFIRMED` count.
- [ ] **Step 2:** Replays n ≥ 6 per arm per capture (`b`, `tim`) → `docs/superpowers/reports/csd/BGLANE-<date>.md` with `[MEASURED, DGX replay <cap>, SPEED=1, n=6]`: queue waits (`sysan_e2e.py qwait`), `ADMIT` counters, time-to-lock p50/p90.
- [ ] **Step 3:** DGX daytime OTA, `t26f` vs `t26b` (= t26f + bg lane) env files in `rr-orchestration/ota/`, Vodafone 415 and TIM 923, n ≥ 3 per arm per cell, profiled run with `sysan_threads.py`. Pass criteria from Global Constraints.
- [ ] **Step 4:** sens6 replay (same script, sens6 core list from `nr_core_plan`), `[MEASURED, sens6 x86_64 RTX 4070 replay]`.
- [ ] **Step 5:** commit report and scripts with explicit paths; do not merge without the user's go.

## Self-review notes

- Spec §7.1 two lanes → Tasks 3, 4; §7.2 admission with deadlines → Tasks 1, 2, 4; §7.5 partition plan / dynamic discovery SMs → Task 6; §7.6 UL discovery off the grant path → Task 5; §9 on-air queue waits → Task 7 criteria.
- Names consistent: `nr_admit_try`, `nr_admit_done`, `nr_admit_register_backlog`, `nr_bg_lane_cores`, `nr_ul_bgdisc_submit`, `ul_hyp_cache_get/_put`.
