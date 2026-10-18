# DL GPU data path v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every body of every DL decode call (first body, retry arm, async recovery, solver probe) runs FEP -> chest -> demod -> descramble -> LLR stats -> rate dematch -> LDPC -> CRC on the GPU with one host wait. The output is bit-identical to today's t26gE stage path. It sits behind `ISAC_PDSCH_GPU_GRAPH=1` (default off) and builds for sm_89 and sm_121.

**Architecture:** Build on GPUPASS v1 (`nr_gpu_pass_graph.cu`), which is already a bit-exact single-graph DL pass. Do not start a new module. The plan changes four things, the ones the 2026-10-17 measurement showed cost the tail (spec 10.0):
1. Coverage: v1's gated-off bodies are 64-82 % of its tail.
2. A decode call holds its instance, and retries enter at the first changed stage.
3. The graph is split into slot / chest / demod / decode sub-graphs keyed separately: the key count becomes a sum instead of a product, and FEP runs once per slot.
4. A lean readback contract.
The CPU keeps control: DCI, solver, job build, launch, commit.

**Tech Stack:** C / CUDA 13.0 (OAI nr-uesoftmodem fork, `libpdsch_gpu.so`), CUDA graphs (stream capture, `cudaGraphInstantiate`, `cudaGraphLaunch`), gtest (`test_nr_gpu_pass`), Python stdlib (`goal_metrics.py`, new `gpugraph_tail.py`), nsys.

**Spec:** `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md`, section 10 ("DL GPU data path v2"). Read 10.0 (what v1 showed), 10.2 (host step table), 10.3 (sub-graphs), 10.5 (readback), 10.7 (retries), 10.8 (budget) before Task 1. Evidence: `docs/superpowers/reports/csd/{GPUPASS-20261016,GPUPASS-8B-20261016,GPUPASS-OTA-20261016,HOST-GAP-20261017,LARGE-GRANT-TAIL-20261017}.md`, `git show csd/int16-gpuc:docs/superpowers/reports/csd/GPU-CONTENTION-20261017.md`, `git show d5c9f9e0bb` (P-DEMATCH), `git show d15f9820c8` (P-FUSE).

## Global Constraints
- Worktree `/home/nicola/NICOLA/wt/rr-csd-int16gpu`, branch `csd/int16-gpupath`, build dir `cmake_targets/ran_build/build_t26` (`CMAKE_CUDA_ARCHITECTURES=89;121`, already configured). Run every build, test, replay and nsys run through `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ... < /dev/null`, with `ninja -j6` at most. Example: `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 -C cmake_targets/ran_build/build_t26 nr-uesoftmodem pdsch_gpu test_nr_gpu_pass < /dev/null`. Agents do not run cmake; if a new ctest target is needed, the orchestrator re-runs cmake. Say so in the report.
- Every CUDA program, ctest and replay runs with `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps` (`mkdir -p /tmp/nicola_nomps`). Test binaries run from the build dir, because they `dlopen("./libpdsch_gpu.so")`.
- Never build while one of OUR receivers runs. This must be false: `for p in $(pgrep -x nr-uesoftmodem); do sudo -n readlink /proc/$p/exe; done | grep -q ^/home/nicola/`. Another user's docker UE also matches pgrep; never touch it. Keep >= 16 GB RAM free. Processes may be SIGSTOPped during radio windows; wait, do not kill.
- No radio. Agents never run OTA, uhd_*, X410, DPDK or firmware tools. The orchestrator runs the final on-air A/B vs t26gE.
- Stop receivers with SIGINT only. ISAC_RX_BRANCH_FO stays unset.
- Frozen paths are never edited: `tests/passive_rx/captures/*`, `tests/passive_rx/*.conf`, `tests/passive_rx/sens6_host_snapshot_2026-09-30/*`. New configs use `.cfg`.
- Flags: master `ISAC_PDSCH_GPU_GRAPH=1`, plus one sub-flag per task (`_COVER`, `_SPLIT`, `_RETRY`, `_SLOT`, `_LEAN`), all default off. With the master flag off, the receiver is byte-identical in behaviour to today (t26gE): no new log line, no allocation, no thread. `ISAC_GPU_PASS=1` (v1) keeps its current meaning. The master flag selects the v2 mode of the same module and refuses to start together with `ISAC_GPU_PASS=1` (log one line, v2 off).
- Bit-exact means the v2 path and the flag-off stage path give identical bytes for all of these: TB, `seg_ok[]`, CRC verdict, `need_cpu`, final LLRs (int16), nvar and per-antenna nvar, every EVMGRANT field (evm, snr, snr_l, br_snr, nvar_db, kappa, alpha, llr_* stats, cfo/sfo). The FP32 demod instantiation is out of scope: every graph in this plan is the FP64 / bit-exact one.
- Unit tests compare against `GpuPassFixture::run_stage_path` (`openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_fixture.h`), which is today's stage path. Each test runs in both models: DIRECT, and `ISAC_GPU_FORCE_DISCRETE=1` / `NR_GPU_PASS_DISCRETE=1` (the existing discrete ctest variant of `test_nr_gpu_pass`).
- Replay protocol (every task): `tests/passive_rx/gpugraph_ab.sh <task> <ON flags>` (created in Task 1).
  - It runs `tests/passive_rx/p99_replay.sh` with `ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26gE.env TMO=90` on `CAPSET=b` (Vodafone `raw_pci415_12s_b`) and `CAPSET=tim` (`raw_pci923_tim_12s_tile3`).
  - Arms: OFF = t26gE, PREV = the previous task's flags, ON = PREV + this task's flag. n=3 per arm per capture, interleaved, captures never pooled.
  - Then it runs `tests/passive_rx/goal_metrics.py` and `tests/passive_rx/gpugraph_tail.py` on every run.log.
- Replay gate, every task (failures stop the task and go to the report):
  - (a) `GPUGRAPH` census `shadow_bad=0`, no `GPUGRAPH MISMATCH` / `DISABLED` line.
  - (b) crc_q ON >= min(OFF runs) - 1 pp.
  - (c) n_real ON median >= 0.8 x OFF median.
  - (d) drops < 1 %.
  - (e) tot p99 ON median <= PREV median x 1.05.
  - (f) the task's own metric, as stated in the task.
  - Label every number `[MEASURED, DGX replay <cap>, <arm>, n=3]`.
- New GPU code:
  - The cell index goes in every parameter block, bounds-checked against `NR_GPU_GRAPH_CELL_MAX` (= 2); single-cell code passes 0.
  - Allocate only through `nr_gpu_buf_alloc`, pre-fence.
  - Call `nr_gpu_buf_h2d_async` / `d2h_async` unconditionally.
  - One CUDA context; RT green partition streams.
  - Wait with `cudaEventQuery` + `nr_gpu_poll_relax_pass`, never `cudaStreamSynchronize` on the hot path.
  - No conditional graph nodes, no device graph launch, no feature that only GB10 has.
- `git add <explicit paths>`; never -A, never stash. Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. If `.git/index.lock` blocks a commit, retry 5 s apart, at most 5 times.
- Evidence label on every claim in commit messages and reports: `[CODE-READ file:line]`, `[MEASURED ...]`, `[ESTIMATE]`.
- Process: Opus implements, Sonnet reviews each task. A task is done only when its unit tests and its replay gate pass and the reviewer has signed off.

## Review Focus
1. **Retry arm that changes the DM-RS hypothesis (CDM arm) but is launched at entry D:** this would decode with a stale estimate. Expected: entry C. Pinned by `GpuGraphRetry.EntryFollowsChangedInput` (Task 4), which builds each arm's job and checks the chosen entry plus the bytes against a full stage-path body.
2. **Two grants of the same slot on two consumer threads plus the recovery worker, all on the shared slot grid S:** a later slot's S must not overwrite the grid while an older body still reads it. Expected: a body whose slot has left the 3-slot ring re-runs S on its own instance, and never reads a newer slot. Pinned by `GpuGraphSlot.RingReuseNeverServesWrongSlot` (Task 5).
3. **Unseen key, busy instance pool or launch error in the middle of a call (after the first body ran on the GPU):** expected: the remaining bodies run the stage path with the host-side state the stage path needs (rxdataF, LLRs), and the call's result is identical. Pinned by `GpuGraphRetry.MidCallFallbackEqualsStagePath` (Task 4).
4. **DISCRETE model (sens6):** with `_LEAN`, a consumer that needs rxdataF / est / LLR on the host (shadow check, CSI, sensing, stage-path fallback) must request it before launch and get identical bytes. Expected: same bytes as DIRECT. Pinned by `GpuGraphLean.RequestedCopiesEqualDirect` in the discrete ctest (Task 6).
5. **A solver probe (CB0 only, symbol horizon) that hits a sub-graph key never warmed:** expected: stage path for that probe, counted `fb_unseen`, no stall and no allocation after the fence. Pinned by `GpuGraphCover.ProbeUnseenKeyFallsBackWithoutAlloc` (Task 2).

## File map
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass.h`: v2 call API (`nr_gpu_call_t`, entry enum, `nr_gpu_llr_stats_t`, result fields).
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu`: v2 mode, `k_llr_stats`, `k_brfo`, `k_tb_pack`, sub-graph caches, call hold, slot graph.
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c` / `nr_gpu_pass_dl.h`: flags, warm set, pre-fence buffer pool, census line `GPUGRAPH`.
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c`: gates (around :3142-3220), LLR-stats consumption (:5168-5270), retry entry (:6402-6760), call begin/end.
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c`: slot launch of S at slot read (Task 5), worker pool claim (Task 1).
- Modify `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc`: `GpuGraph*` tests.
- Modify `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_fixture.h`: probe / retry-arm job builders.
- Create `tests/passive_rx/gpugraph_ab.sh`: replay A/B.
- Create `tests/passive_rx/gpugraph_tail.py` + `tests/passive_rx/test_gpugraph_tail.py`: tail-path classifier.
- Create `docs/superpowers/reports/csd/GPUGRAPH-T<n>-<date>.md`: one per task.

---

### Task 1: v2 mode, LLR stats on the device, prewarmed buffer pool (coverage of the recovery worker)

Tail impact: removes the `nomem` gate (945-2471 bodies per replay run, the recovery worker) and makes every logged field bit-exact, which later shadow checks need. [MEASURED spec 10.0]

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass.h` (result struct)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu` (`k_llr_stats`, census `GPUGRAPH`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c` (flag, pool)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2390-2404` (`gp_thread_prewarm`), `:5168-5177` (gp_ok branch)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc`
- Create: `tests/passive_rx/gpugraph_ab.sh`, `tests/passive_rx/gpugraph_tail.py`, `tests/passive_rx/test_gpugraph_tail.py`

**Interfaces:**
- Consumes: `nr_gpu_pass_dl_try`, `GpuPassFixture::run_stage_path`, `nr_gpu_pass_dl_prewarm`.
- Produces:
```c
/* nr_gpu_pass.h */
typedef struct {
  uint64_t llr_n, llr_absum, llr_zero, llr_sat, llr_clip8, llr_pos; /* before descrambling, stride 32 (D:5179-5235) */
  int64_t llr_sgnsum;                                               /* after descrambling, stride 32 (D:5255-5268) */
  uint64_t posbit[2], nbit[2];
} nr_gpu_llr_stats_t;
/* appended to nr_gpu_pass_result_t */
nr_gpu_llr_stats_t llr_stats;
int nr_gpu_graph_on(void);              /* ISAC_PDSCH_GPU_GRAPH=1 and the module is up (v2 mode) */
int nr_gpu_graph_flag(const char *sub); /* "COVER", "SPLIT", "RETRY", "SLOT", "LEAN": ISAC_PDSCH_GPU_GRAPH_<sub>=1 and the master on */
/* nr_gpu_pass_dl.c: pre-fence pool of NR_GPU_GRAPH_POOL (8) prewarmed nr_gpu_pass_dl_out_t + shadow buffers */
nr_gpu_pass_dl_out_t *nr_gpu_graph_pool_claim(uint8_t **shadow); /* NULL = pool empty (counted) */
```

- [ ] **Step 1: Write the failing test** (append to `nr_gpu_pass_test.cc`)
```cpp
/* v2: the pass's LLR statistics equal the stage path's host loops (D:5179-5268) for every warm shape, CRC ok and CRC fail. */
TEST(GpuGraph, LlrStatsEqualStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok && pass_ready(m));
  for (auto shape : GpuPassFixture::warm_shapes()) {
    for (double snr : {20.0, -3.0}) { /* decodes / fails */
      GpuPassFixture::noise_db = snr;
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, 13);
      fx.bind();
      std::vector<uint8_t> gb(fx.tb.A / 8 + 64), sb(fx.tb.A / 8 + 64);
      nr_gpu_pass_result_t g{}, s{};
      g.tb = gb.data(); s.tb = sb.data();
      ASSERT_EQ(0, m.pass_try(fx.job, &g));
      ASSERT_EQ(0, fx.run_stage_path(&s));
      const nr_gpu_llr_stats_t ref = fx.host_llr_stats(); /* the D:5179-5268 loops over the stage path's pre/post-descramble LLRs */
      EXPECT_EQ(0, memcmp(&ref, &g.llr_stats, sizeof(ref))) << "nrb " << shape.nrb_class << " nl " << shape.nl << " qm " << shape.qm << " snr " << snr;
    }
  }
  GpuPassFixture::noise_db = 20.0;
}
```
In `nr_gpu_pass_fixture.h`, add `host_llr_stats()`. It copies the two loops of `nr_pdsch_passive_decode.c:5179-5235` (before descrambling: absum, zero, sat >= 32767, clip8 > 127, stride 32) and `:5255-5268` (after descrambling: posbit / nbit over 4 LLRs per 32, sgnsum, pos). It runs them over the stage path's LLRs, which `run_stage_path` keeps before (`llr_pre`) and after (`llr_post`) the unscramble. Store both vectors in the fixture.

- [ ] **Step 2: Run it to make sure it fails**
Run: `cd cmake_targets/ran_build/build_t26 && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh env CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps ./test_nr_gpu_pass --gtest_filter=GpuGraph.LlrStatsEqualStagePath < /dev/null`
Expected: compile error (`nr_gpu_llr_stats_t` / `llr_stats` undefined).

- [ ] **Step 3: Implement `k_llr_stats`** in `nr_gpu_pass_graph.cu`. It needs the LLRs before AND after descrambling, so it runs in two halves: `k_llr_stats_pre` between the demod and `k_unscr`, and `k_llr_stats_post` right after `k_unscr`.
```cpp
/* D:5179-5235 on the demod output (before k_unscr) and D:5255-5268 after it; one block of 1024 threads, integer sums: order-independent, so bit-equal. */
__global__ void k_llr_stats_pre(const int16_t *llr, const PassBlk *pb, ResBlk *rb)
{
  __shared__ unsigned long long s[5][32];
  unsigned long long n = 0, ab = 0, z = 0, sat = 0, c8 = 0;
  for (uint32_t i = threadIdx.x * 32u; i < pb->G; i += blockDim.x * 32u) {
    const int v = llr[i] < 0 ? -llr[i] : llr[i];
    n++; ab += (unsigned)v; z += v == 0; sat += v >= 32767; c8 += v > 127;
  }
  /* warp + block reduce of the five sums into rb->llr_stats.{llr_n, llr_absum, llr_zero, llr_sat, llr_clip8} */
  for (int o = 16; o; o >>= 1) { n += __shfl_down_sync(~0u, n, o); ab += __shfl_down_sync(~0u, ab, o); z += __shfl_down_sync(~0u, z, o); sat += __shfl_down_sync(~0u, sat, o); c8 += __shfl_down_sync(~0u, c8, o); }
  const int w = threadIdx.x >> 5, l = threadIdx.x & 31;
  if (!l) { s[0][w] = n; s[1][w] = ab; s[2][w] = z; s[3][w] = sat; s[4][w] = c8; }
  __syncthreads();
  if (threadIdx.x < 5) { unsigned long long t = 0; for (int i = 0; i < (int)(blockDim.x >> 5); i++) t += s[threadIdx.x][i]; (&rb->llr_stats.llr_n)[threadIdx.x] = t; }
}
__global__ void k_llr_stats_post(const int16_t *llr, const PassBlk *pb, ResBlk *rb); /* posbit[2], nbit[2], llr_sgnsum, llr_pos: same shape, i + 3 < G, stride 32 */
```
`ResBlk` gains `nr_gpu_llr_stats_t llr_stats`. Enqueue order in `enqueue()`: demod -> `k_llr_stats_pre` -> `k_unscr` -> `k_llr_stats_post` -> `k_norm_sum` -> ... Field order: `llr_n, llr_absum, llr_zero, llr_sat, llr_clip8` must stay contiguous at the start of the struct (the kernel indexes them). Add `static_assert(offsetof(nr_gpu_llr_stats_t, llr_clip8) == 4 * sizeof(uint64_t))`. `dl_try` copies `RB()->llr_stats` into `r->llr_stats`.

- [ ] **Step 4: Run the test.** Expected: PASS in DIRECT. Then run with `NR_GPU_PASS_DISCRETE=1 ISAC_GPU_FORCE_DISCRETE=1`: PASS.

- [ ] **Step 5: Host consumes the stats.** In `nr_pdsch_passive_decode.c:5168-5177` (`if (gp_ok)` branch), before `goto gp_llr_done`, assign `llr_n ... nbit` from `t_gpp->r.llr_stats`, so EVMGRANT and `g_llr_sat` see the same values as the stage path. Gate this on `nr_gpu_graph_on()` so v1 (`ISAC_GPU_PASS=1` alone) is unchanged.

- [ ] **Step 6: Write the failing pool test**
```cpp
/* v2: a thread started after the fence (the async recovery worker) claims a prewarmed buffer set instead of hitting the nomem gate. */
TEST(GpuGraph, LateThreadClaimsPrewarmedPool) {
  setenv("ISAC_PDSCH_GPU_GRAPH", "1", 1);
  ASSERT_EQ(0, nr_gpu_graph_pool_init_for_test(2)); /* 2 sets, pre-fence */
  nr_gpu_fence_for_test();                          /* allocations now refused */
  uint8_t *sh = nullptr;
  std::thread t([&] { EXPECT_NE(nullptr, nr_gpu_graph_pool_claim(&sh)); EXPECT_NE(nullptr, sh); });
  t.join();
  uint8_t *sh2 = nullptr, *sh3 = nullptr;
  EXPECT_NE(nullptr, nr_gpu_graph_pool_claim(&sh2));
  EXPECT_EQ(nullptr, nr_gpu_graph_pool_claim(&sh3)) << "pool exhausted -> NULL, counted, never malloc after the fence";
  unsetenv("ISAC_PDSCH_GPU_GRAPH");
}
```
`nr_gpu_graph_pool_init_for_test` / `nr_gpu_fence_for_test` are thin test hooks over the pool init and the existing `nr_gpu_allocations_allowed()` fence (`PHY/TOOLS/nr_gpu_mem.c:19`). Use the fence hook only if the module already exposes one. Otherwise test through the `GpuPassFence.*` pattern (`nr_gpu_pass_test.cc:1677`).

- [ ] **Step 7: Implement the pool.** In `nr_gpu_pass_dl.c`, `nr_gpu_pass_warmup()` allocates `NR_GPU_GRAPH_POOL` = 8 sets (`nr_gpu_pass_dl_out_t` via `aligned_alloc(64, ...)` + `GP_SHADOW_CAP` shadow), prefaults them with `nr_gpu_pass_dl_prewarm`, and keeps them on a lock-free stack (`_Atomic` index, CAS). `gp_thread_prewarm()` (D:2390) changes as follows: when `!nr_gpu_allocations_allowed()` and `nr_gpu_graph_on()`, it calls `nr_gpu_graph_pool_claim(&t_gp_shadow)`, which sets `t_gpp`, instead of returning. Buffers are never returned; a thread keeps its set for life.

- [ ] **Step 8: Run all GpuGraph and GpuPass tests**, DIRECT and discrete: `./test_nr_gpu_pass --gtest_filter='GpuGraph*:GpuPass*'`. Expected: all PASS, and the existing tests unchanged.

- [ ] **Step 9: Create the replay tools.**
`tests/passive_rx/gpugraph_tail.py`, usage `gpugraph_tail.py run.log...`:
- It parses the EVMGRANT `probe=0` lines.
- From each it takes `e2e=a2e,qw,pre,dec,first,bodies,tot,lag` and `stg=fep:a/..,chest:b/..,demod:c/..,ldpc:d`.
- It prints the tail (tot >= p99) mean of each component.
- It counts tail grants decoded by the stage path: stage timers fep + chest + demod + ldpc > 200 us. The pass bypasses those timers, so they read ~5 us in a pass body.
- It prints the stage-path share among all grants.
Test `tests/passive_rx/test_gpugraph_tail.py`: two synthetic lines (one pass body, one stage body) -> counts 1 / 1, and the p99 index rule. Pattern: `tests/passive_rx/test_goal_metrics.py`.
`tests/passive_rx/gpugraph_ab.sh <tag> "<PREV flags>" "<ON flags>"`:
```bash
#!/bin/bash
# GPUGRAPH replay A/B: OFF = t26gE, PREV, ON; n=3 interleaved per capture; never pooled. Each replay takes heavy.lock via p99_replay.sh.
set -u
S=$(cd "$(dirname "$0")" && pwd); E=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26gE.env
tag=$1; prev=$2; on=$3
for i in 1 2 3; do for cap in b tim; do for arm in OFF PREV ON; do
  case $arm in OFF) X="";; PREV) X="$prev";; ON) X="$on";; esac
  [ "$arm" = PREV ] && [ -z "$prev" ] && continue
  CAPSET=$cap ENVFILE=$E TMO=90 "$S/p99_replay.sh" "gg_${tag}_${cap}_${arm}_$i" $X < /dev/null
  d=$(cat "$S/../../replays/last"); echo "$cap $arm $i $d" >> "$S/../../replays/gg_${tag}.idx"
done; done; done
while read cap arm i d; do echo "== $cap $arm $i"; python3 "$S/goal_metrics.py" "$d/run.log" | head -1; python3 "$S/gpugraph_tail.py" "$d/run.log"; grep -a "GPUGRAPH " "$d/run.log" | tail -1; done < "$S/../../replays/gg_${tag}.idx"
```

- [ ] **Step 10: Replay gate.** Run `tests/passive_rx/gpugraph_ab.sh t1 "" "ISAC_PDSCH_GPU_GRAPH=1"`. The task metric (f): the `nomem` gate count is 0 in ON, and the stage-path share of tail grants (`gpugraph_tail.py`) in ON is below v1's 64-82 % (spec 10.0). Write `docs/superpowers/reports/csd/GPUGRAPH-T1-<date>.md`.

- [ ] **Step 11: Commit**
```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass.h openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.h openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_fixture.h tests/passive_rx/gpugraph_ab.sh tests/passive_rx/gpugraph_tail.py tests/passive_rx/test_gpugraph_tail.py docs/superpowers/reports/csd/GPUGRAPH-T1-*.md
git commit -m "feat(gpugraph): T1 v2 mode, LLR stats on device, prewarmed pool for late threads (ISAC_PDSCH_GPU_GRAPH, default off)"
```

### Task 2: Coverage of probes and slot-share bodies (`ISAC_PDSCH_GPU_GRAPH_COVER=1`)

Tail impact: the probe gate kept 2989-3249 bodies per run on the stage path; probes share consumers with real grants, so their stage-path time is queue time for real grants. The share gate cost another 35 bodies. [MEASURED spec 10.0]

**Files:**
- Modify: `nr_pdsch_passive_decode.c:3147-3151` (gates PROBE, SHARE), the gp_ok / commit path for probe outcomes (`:1979-2011`)
- Modify: `nr_gpu_pass_graph.cu` (probe job: `C = 1` decoded of the full-C parameters, symbol horizon)
- Modify: `nr_gpu_pass_dl.c` (job build for probes; warm the ncb_class-1 decode keys for every nrb class)
- Test: `tests/nr_gpu_pass_test.cc`, `tests/nr_gpu_pass_fixture.h`

**Interfaces:**
- Consumes: Task 1 `nr_gpu_graph_flag("COVER")`, `nr_gpu_pass_result_t::llr_stats`.
- Produces: `nr_gpu_pass_job_t` gains `uint8_t probe; uint8_t probe_last_sym; uint32_t n_decode;` (n_decode = segments to decode, 1 for a probe, `C` otherwise). A probe pass decodes block 0 with the full-C E / K / Z (as `nb_segments_to_decode = 1`, D:1908) and reports `seg_ok[0]`; there is no TB CRC.

- [ ] **Step 1: Failing test**
```cpp
/* a layout probe (CB0 only, horizon to CB0's last symbol) through the pass equals the stage path's probe body: seg_ok[0], CB0 LLRs, llr_stats. */
TEST(GpuGraphCover, ProbeEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP();
  ASSERT_TRUE(ok && pass_ready(m));
  for (auto shape : GpuPassFixture::warm_shapes()) {
    GpuPassFixture fx = GpuPassFixture::from_shape(shape, 17);
    fx.make_probe(); /* n_decode = 1, probe_last_sym = the symbol holding CB0's last RE (D:3105-3125 rule) */
    fx.bind();
    std::vector<uint8_t> gb(fx.tb.A / 8 + 64), sb(fx.tb.A / 8 + 64);
    nr_gpu_pass_result_t g{}, s{};
    g.tb = gb.data(); s.tb = sb.data();
    ASSERT_EQ(0, m.pass_try(fx.job, &g));
    ASSERT_EQ(0, fx.run_stage_path(&s));
    EXPECT_EQ(s.seg_ok[0], g.seg_ok[0]);
    EXPECT_EQ(0, memcmp(&s.llr_stats, &g.llr_stats, sizeof(s.llr_stats)));
  }
}
TEST(GpuGraphCover, ProbeUnseenKeyFallsBackWithoutAlloc) {
  /* a probe whose key (nrb 6, nl 4, ndmrs 4) is not warmed: dl_try returns 1, fb_unseen + 1, no allocation after the fence (GpuPassFence pattern, :1677) */
}
```
Write the second test body the way `GpuPassFence.NoAllocAfterFence` (`nr_gpu_pass_test.cc:1677`) counts allocations: `o[2]` (unseen) grows by 1, and the allocation counter stays the same.

- [ ] **Step 2: Run, expect FAIL** (`make_probe` undefined).
- [ ] **Step 3: Implement.**
  - Fixture `make_probe()`: sets `job->probe = 1`, `n_decode = 1`, `probe_last_sym`, and runs the stage path with the same horizon (the fixture's stage path takes `n_decode` in its LDPC call).
  - Module side, `prepare_tb`: blocks `s >= n_decode` become padding (C = 0 rows of `pb.rdm`, which `k_rdm` already zero-fills), and `k_pick` / TB CRC see `ncb = n_decode`.
  - The FEP and demod symbol masks stop at `probe_last_sym`; the rows that `fj` and `dj` already carry express this.
  - Host side: the PROBE gate is removed when `nr_gpu_graph_flag("COVER")`. The SHARE gate is removed too: the pass computes its own FEP/chest, so the slot-share cache is not consulted; the cache stays valid for stage-path bodies.
  - Probe outcome handling stays in today's code (`D:1979-2011`), fed by `seg_ok[0]`.
  - Probes launch on the low-priority twin stream (`nr_pdsch_gpu_prio.cuh`, `ISAC_PDSCH_GPU_PRIO`). Give the instance a second stream at the least priority and choose per job: `job->probe ? I.s_lo : I.s`. A captured graph can be launched on either stream.
- [ ] **Step 4: Run both tests, DIRECT + discrete. Expect PASS.**
- [ ] **Step 5: Replay gate** `gpugraph_ab.sh t2 "ISAC_PDSCH_GPU_GRAPH=1" "ISAC_PDSCH_GPU_GRAPH=1 ISAC_PDSCH_GPU_GRAPH_COVER=1"`. Metric (f): gate counts `probe=0 share=0`, and the solver feedback counts (`PDSCH_SOLVER` census line: confirmations, probes, CRC-ok probes) are within OFF's run spread. The solver depends on probe outcomes; the H-arm lesson (GHC-OTA: solver timing cost TIM n_real -25 %) makes condition (c) of the gate binding here.
- [ ] **Step 6: Commit** (explicit paths as in Task 1, plus the report `GPUGRAPH-T2-<date>.md`): `feat(gpugraph): T2 probes and slot-share bodies on the pass (ISAC_PDSCH_GPU_GRAPH_COVER, default off)`.

### Task 3: Split into sub-graphs C / D / L with separate keys (`ISAC_PDSCH_GPU_GRAPH_SPLIT=1`)

Tail impact: removes the class cap that sends unseen classes (195 per tim run) and > 16-segment grants (250-389 bodies) to the stage path. Cuts device memory from 5.7-6.8 GB and warm-up from 8.6 s. It is the prerequisite for retry entries (Task 4) and sm_89 (8-12 GB card). [MEASURED spec 10.0]

**Files:**
- Modify: `nr_gpu_pass_graph.cu` (`enqueue` -> `enqueue_C`, `enqueue_D`, `enqueue_L`; `Key` -> `KeyC`, `KeyD`, `KeyL`; three caches; `build` per sub-graph)
- Modify: `nr_gpu_pass_dl.c` (warm set per sub-key, `ISAC_GPU_PASS_CLASSES` meaning per sub-graph, nseg class 32)
- Modify: `nr_gpu_pass.h` (`NR_GPU_PASS_MAX_SEG` 16 -> 32 for the C key only; `nr_gpu_pass_nseg_class` up to 32)
- Test: `tests/nr_gpu_pass_test.cc`

**Interfaces:**
- Consumes: v1 `Inst`, `prepare`, the launchers in `enqueue()` (`nr_gpu_pass_graph.cu:450-483`).
- Produces:
```cpp
struct KeyC { uint16_t nrb, nl, ndmrs; uint8_t nseg; uint64_t pack() const; };          /* until Task 5, C also runs the slot FEP (v1 behaviour) */
struct KeyD { uint16_t nrb, nl; uint8_t qmode, align, evm, fp32; uint64_t pack() const; };
struct KeyL { uint16_t ncb; uint8_t nc; uint64_t pack() const; };                       /* nc = decide_nc() family for this ncb */
static int enqueue_C(Inst &I, const KeyC &k, cudaStream_t s);  /* [FEP] + chest + seg compose + chest post */
static int enqueue_D(Inst &I, const KeyD &k, cudaStream_t s);  /* demod + EVM fork + llr_stats + unscr + norm */
static int enqueue_L(Inst &I, const KeyL &k, cudaStream_t s);  /* rdm + LDPC + crc + pick + crc + result D2H */
/* census line */
"GPUGRAPH split C=<n> D=<n> L=<n> execs=<n> mb=<n> warm_ms=<n> unseen[C,D,L]=<a,b,c>"
```
Until Task 5, C contains the FEP (v1 behaviour). The three launches go back to back on `I.s` with no host wait in between: `cudaGraphLaunch(C) ; cudaGraphLaunch(D) ; cudaGraphLaunch(L) ; cudaEventRecord(I.done)`.

- [ ] **Step 1: Failing test**
```cpp
/* SPLIT: three launches equal the single v1 graph and the stage path, every warm shape and the segmented cases up to 32 segments. */
TEST(GpuGraphSplit, ThreeLaunchesEqualSingleGraphAndStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  setenv("ISAC_PDSCH_GPU_GRAPH", "1", 1); setenv("ISAC_PDSCH_GPU_GRAPH_SPLIT", "1", 1);
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP();
  ASSERT_TRUE(ok && pass_ready(m));
  for (auto shape : GpuPassFixture::warm_shapes()) {
    GpuPassFixture fx = GpuPassFixture::from_shape(shape, 11);
    fx.bind();
    std::vector<uint8_t> gb(fx.tb.A / 8 + 64), sb(fx.tb.A / 8 + 64);
    nr_gpu_pass_result_t g{}, s{};
    g.tb = gb.data(); s.tb = sb.data();
    ASSERT_EQ(0, m.pass_try(fx.job, &g));
    ASSERT_EQ(0, fx.run_stage_path(&s));
    EXPECT_EQ(s.tb_ok, g.tb_ok); EXPECT_EQ(s.nvar, g.nvar);
    EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8)); EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok)));
    EXPECT_EQ(0, memcmp(&s.llr_stats, &g.llr_stats, sizeof(s.llr_stats)));
  }
  uint64_t o[16]; m.pass_counters(o);
  EXPECT_EQ(0u, o[2] + o[3] + o[4] + o[5]) << "no fallback";
  unsetenv("ISAC_PDSCH_GPU_GRAPH_SPLIT"); unsetenv("ISAC_PDSCH_GPU_GRAPH");
}
TEST(GpuGraphSplit, ThirtyTwoSegmentsEqualStagePath) {
  /* seg_layout(n=273, start=0, prg=0, interleave=2, bwp=273) gives > 16 segments: run through SegmentedGraphEqualsStagePath's comparison (:155) with the C key nseg 32 */
}
TEST(GpuGraphSplit, WarmSetFitsBudget) {
  /* after nr_gpu_pass_init in split mode with the production warm set: GPUGRAPH census mb <= 1024 and execs <= 4 * (C + D + L) */
}
```
Copy the comparison loop of `SegmentedGraphEqualsStagePath` (`nr_gpu_pass_test.cc:155-230`) into the second test, with `seg_layout(273, 0, 0, 2, 273)` and an assertion that the case yields more than 16 segments.

- [ ] **Step 2: Run, expect FAIL** (the split flag has no effect yet; the third test sees v1's ~5.7 GB).
- [ ] **Step 3: Implement.**
  - Split `enqueue()` at its two natural seams: after `nr_gpu_pass_launch_chest_post_p`, and after `k_norm_apply`.
  - `build()` captures each sub-graph separately per sub-key, using v1's capture code (`nr_gpu_pass_graph.cu:695-744`) unchanged in structure.
  - `dl_try` looks up three entries. A miss on any of them is `fb_unseen`, with the missing sub-key logged under `ISAC_GPU_PASS_LOG_UNSEEN`.
  - Warm set: the union of keys over nrb {273,217,136,96,48,24,12,6} x nl {1..4} x ndmrs {1..4} x nseg {0,1,2,4,8,16,32} for C; nrb x nl x the variants `nr_pdsch_gpu_demod_pass_variant` can return x evm {0,1} for D; ncb {1..32} x family for L. Measure `cudaMemGetInfo` before and after and print it in the census.
  - If the total exceeds `ISAC_PDSCH_GPU_GRAPH_MB` (default 1024), stop warming at the cap (largest nrb first) and log it.
- [ ] **Step 4: Run tests, DIRECT + discrete.** Expect PASS. Record `mb`, `execs`, `warm_ms` in the report.
- [ ] **Step 5: nsys check** (one run per capture): `WRAP="nsys profile -t cuda,nvtx --cuda-graph-trace=node -o nsys_trace" CAPSET=<cap> ENVFILE=.../t26gE.env tests/passive_rx/p99_replay.sh gg_t3ns_<cap> ISAC_PDSCH_GPU_GRAPH=1 ISAC_PDSCH_GPU_GRAPH_COVER=1 ISAC_PDSCH_GPU_GRAPH_SPLIT=1`, then `nsys export -t sqlite`. Report the span between the end of C and the start of D, and between D and L (expected <= 10 us p50 [ESTIMATE spec 10.8]).
- [ ] **Step 6: Replay gate** `gpugraph_ab.sh t3 "<T2 flags>" "<T2 flags> ISAC_PDSCH_GPU_GRAPH_SPLIT=1"`. Metric (f): `unseen` = 0 and `seg` gate = 0 on both captures; `mb` <= 1024; warm-up wall < 3 s (receiver start to the `GPUGRAPH enabled` line, polled).
- [ ] **Step 7: Commit** `feat(gpugraph): T3 C/D/L sub-graphs with separate keys, 32-segment class, memory cap (ISAC_PDSCH_GPU_GRAPH_SPLIT, default off)`.

### Task 4: Retry arms and recovery on the held instance (`ISAC_PDSCH_GPU_GRAPH_RETRY=1`)

Tail impact:
- Today every retry arm is a full body (FEP + chest + demod + LDPC). The slowest 1 % of b calls run 7.4-7.9 bodies. [MEASURED spec 10.1/10.0]
- An arm entering at D costs ~290-340 us instead of a full body [ESTIMATE spec 10.8].
- Recovery latency (`recovered_tot_p99` 5.7-8.1 ms in the v1 replays) and consumer occupancy both drop.

**Files:**
- Modify: `nr_gpu_pass.h` (call API)
- Modify: `nr_gpu_pass_graph.cu` (instance hold, entry launch)
- Modify: `nr_gpu_pass_dl.c` / `.h` (`nr_gpu_pass_dl_run` takes the call and the entry)
- Modify: `nr_pdsch_passive_decode.c`: call begin/end in `passive_decode_retry_impl` (D:6402), the entry choice per arm (D:6700-6760), removal of the CONT_FAIL LLR memcpy (D:5174) when the arms run on the device
- Modify: `nr_pdsch_passive_queue.c` (worker: `recov_run`, :2675)
- Test: `tests/nr_gpu_pass_test.cc`, `tests/nr_gpu_pass_fixture.h` (arm job builders)

**Interfaces:**
- Consumes: Task 3 `enqueue_C/D/L`, `KeyC/KeyD/KeyL`.
- Produces:
```c
typedef enum { NR_GPU_ENTRY_C = 0, NR_GPU_ENTRY_D = 1, NR_GPU_ENTRY_L = 2 } nr_gpu_entry_t;
typedef struct { int8_t inst; uint8_t has_est, has_llr; uint8_t cell; uint32_t slot_abs; } nr_gpu_call_t; /* inst -1 = none held */
int  nr_gpu_pass_call_begin(nr_gpu_call_t *c, uint8_t cell, uint32_t slot_abs); /* 0 = instance held, 1 = busy (counted fb_busy) */
int  nr_gpu_pass_dl_try2(nr_gpu_call_t *c, const nr_gpu_pass_job_t *j, nr_gpu_entry_t entry, nr_gpu_pass_result_t *r); /* 0 done, 1 not handled, <0 error */
void nr_gpu_pass_call_end(nr_gpu_call_t *c);
/* host side: which entry an arm needs (pure, unit-tested) */
nr_gpu_entry_t nr_gpu_graph_arm_entry(unsigned arm_bits, int cv, int tint_override, int rv_only);
```
Rule for `nr_gpu_graph_arm_entry` (spec 10.7):
- `arm & 4` (CDM) or `tint_override > 0` -> C.
- `rv_only` -> L.
- Everything else (S toggle `arm & 2`, CSI `arm & 1`, CSI population `cv > 0`, wide MRC `arm & 8`, second TDA, static CSI, PDCCH RM) -> D.
- The first body is always C.

- [ ] **Step 1: Failing tests**
```cpp
TEST(GpuGraphRetry, ArmEntryRule) {
  EXPECT_EQ(NR_GPU_ENTRY_C, nr_gpu_graph_arm_entry(4, 0, 0, 0));
  EXPECT_EQ(NR_GPU_ENTRY_C, nr_gpu_graph_arm_entry(2 | 4, 0, 0, 0));
  EXPECT_EQ(NR_GPU_ENTRY_D, nr_gpu_graph_arm_entry(2, 0, 0, 0));
  EXPECT_EQ(NR_GPU_ENTRY_D, nr_gpu_graph_arm_entry(1, 0, 0, 0));
  EXPECT_EQ(NR_GPU_ENTRY_D, nr_gpu_graph_arm_entry(1, 2, 0, 0));
  EXPECT_EQ(NR_GPU_ENTRY_D, nr_gpu_graph_arm_entry(8, 0, 0, 0));
  EXPECT_EQ(NR_GPU_ENTRY_C, nr_gpu_graph_arm_entry(16, 0, 1, 0));
  EXPECT_EQ(NR_GPU_ENTRY_L, nr_gpu_graph_arm_entry(0, 0, 0, 1));
}
/* every arm kind launched at its entry on the held instance equals a full stage-path body of the same arm job */
TEST(GpuGraphRetry, EntryFollowsChangedInput) {
  if (!have_gpu()) GTEST_SKIP();
  setenv("ISAC_PDSCH_GPU_GRAPH", "1", 1); setenv("ISAC_PDSCH_GPU_GRAPH_SPLIT", "1", 1); setenv("ISAC_PDSCH_GPU_GRAPH_RETRY", "1", 1);
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP();
  ASSERT_TRUE(ok && pass_ready(m));
  GpuPassFixture fx = GpuPassFixture::from_shape({/*nrb_class*/ 48, /*nsym*/ 14, /*nl*/ 1, /*qm*/ 2, /*ndmrs*/ 2, /*ncb_class*/ 1, /*fp32: float EVM, FP64 demod*/ 1}, 21); /* Nl 1, QPSK: every arm kind applies */
  fx.bind();
  nr_gpu_call_t c{-1};
  ASSERT_EQ(0, m.call_begin(&c, 0, 1000));
  std::vector<uint8_t> b0(fx.tb.A / 8 + 64);
  nr_gpu_pass_result_t r0{}; r0.tb = b0.data();
  ASSERT_EQ(0, m.dl_try2(&c, fx.job, NR_GPU_ENTRY_C, &r0));
  for (unsigned arm : {2u, 1u, 4u, 8u}) {
    GpuPassFixture ax = fx.with_arm(arm); /* the job the body builds for this arm: S toggle / CSI cleared / CDM 3-cdm / wide MRC (D:6637-6725) */
    std::vector<uint8_t> gb(ax.tb.A / 8 + 64), sb(ax.tb.A / 8 + 64);
    nr_gpu_pass_result_t g{}, s{};
    g.tb = gb.data(); s.tb = sb.data();
    ASSERT_EQ(0, m.dl_try2(&c, ax.job, nr_gpu_graph_arm_entry(arm, 0, 0, 0), &g));
    ASSERT_EQ(0, ax.run_stage_path(&s));
    EXPECT_EQ(0, memcmp(s.seg_ok, g.seg_ok, sizeof(s.seg_ok))) << "arm " << arm;
    EXPECT_EQ(0, memcmp(&s.llr_stats, &g.llr_stats, sizeof(s.llr_stats))) << "arm " << arm;
    EXPECT_EQ(s.nvar, g.nvar) << "arm " << arm;
  }
  m.call_end(&c);
}
TEST(GpuGraphRetry, MidCallFallbackEqualsStagePath) {
  /* hold all instances but the call's; force the arm's D key unseen (test hook nr_gpu_pass_test_drop_key(KeyD)); dl_try2 returns 1; the fixture then runs the
   * stage path for that arm from the host rxdataF it requested (want_rxF = 1) and the bytes equal a stage path run from scratch */
}
```
Add `call_begin`, `dl_try2` and `call_end` to `GpuPassModule` (`nr_gpu_pass_fixture.h:35-96`) with `dlsym`. Add `GpuPassFixture::with_arm(unsigned)`: it rebuilds the job with the same changes the body makes (D:6637-6725). For a full arm it also returns a fresh `run_stage_path` reference.

- [ ] **Step 2: Run, expect FAIL** (symbols missing).
- [ ] **Step 3: Implement the module side.**
  - `call_begin` takes an instance with v1's CAS loop (`nr_gpu_pass_graph.cu:910-916`) and does not release it at the end of `dl_try2`; `call_end` releases it.
  - `dl_try2` with entry D or L checks `has_est` / `has_llr`; if the needed state is missing, it upgrades the entry to C.
  - It writes only the parameter blocks of the sub-graphs it launches.
  - It launches from the entry to L, then waits once.
  - Raise `NR_GPU_PASS_RING` from 4 to 6 (`nr_pdsch_gpu_fep.h:71`): 5 consumers + 1 worker. The per-instance `cudaMemGetInfo` delta goes in the census.
- [ ] **Step 4: Implement the body side.**
  - In `passive_decode_retry_impl` (D:6402), call `nr_gpu_pass_call_begin` once per call when `nr_gpu_graph_flag("RETRY")`, and `call_end` on every return path. Use a `goto out` cleanup, the pattern the function already uses for its restore body.
  - Each arm body passes its entry from `nr_gpu_graph_arm_entry`.
  - The CONT_FAIL branch (D:5168-5177) no longer copies the LLRs to the host when RETRY is on; `t_last_llr` stays NULL, and the RV retry arm (off in t26gE) uses entry L.
  - The worker (`recov_run`, nr_pdsch_passive_queue.c:2675) calls `call_begin` with its own snapshot.
  - Keep the arm order and stop rule unchanged. Do not touch `nr_gpu_pass_retry_to_old`; with RETRY on, it is simply not consulted.
- [ ] **Step 5: Run the GpuGraph* tests, DIRECT + discrete. Expect PASS.**
- [ ] **Step 6: Replay gate** `gpugraph_ab.sh t4 "<T3 flags>" "<T3 flags> ISAC_PDSCH_GPU_GRAPH_RETRY=1"`. Metric (f): `recovered_tot_p99` (goal_metrics) ON median < PREV median; RECOV `rescued` count within PREV spread (no lost rescues); `fb_busy` < 1 % of launches.
- [ ] **Step 7: Commit** `feat(gpugraph): T4 retry arms and async recovery enter at the first changed sub-graph on a held instance (ISAC_PDSCH_GPU_GRAPH_RETRY, default off)`.

### Task 5: Slot FEP shared per (cell, slot) (`ISAC_PDSCH_GPU_GRAPH_SLOT=1`)

Tail impact: k_fep_exact is 53-56 us mean and 66-102 us in tail bodies, on every body including retries and probes [MEASURED spec 10.0]. S runs once per slot when the slot is read, off the grant's critical path (a2e p50 380-584 us comes first).

**Files:**
- Modify: `nr_gpu_pass_graph.cu` (slot grid ring `[NR_GPU_GRAPH_CELL_MAX][3 slots]`, sub-graph S, event per ring entry; C no longer contains the FEP when SLOT is on)
- Modify: `nr_pdsch_passive_queue.c` (dispatcher: launch S when a slot is read, before its jobs are enqueued; where the IQ snapshot of the slot group is taken, near :1642)
- Modify: `nr_gpu_pass.h` (API)
- Test: `tests/nr_gpu_pass_test.cc`

**Interfaces:**
- Consumes: Task 3 sub-graphs, Task 4 call hold.
- Produces:
```c
/* launch the FEP of (cell, slot_abs) into the slot grid ring; returns the ring index or -1 (ring busy: bodies then run their own FEP in C, counted) */
int nr_gpu_graph_slot_fep(uint8_t cell, uint32_t slot_abs, const int16_t *const *rxdata, uint32_t ring_len, uint32_t ring_offset, const int16_t *symbol_rot);
/* nr_gpu_pass_job_t gains: uint8_t cell; uint32_t slot_abs; -- C waits on the ring entry's event (cudaStreamWaitEvent) when it holds slot_abs, else runs its own FEP variant */
```
The ring entry holds `slot_abs`; a body checks `entry.slot_abs == job.slot_abs` under the entry's sequence counter (seqlock: read seq, compare, wait event, re-read seq). A mismatch means the slot left the ring: use C's own-FEP variant (the KeyC field `own_fep = 1`).

- [ ] **Step 1: Failing tests**
```cpp
TEST(GpuGraphSlot, SharedFepEqualsOwnFep) {
  /* two grants of the same fixture slot (different PRBs, fx.second_grant()), S launched once, both decoded with C(own_fep = 0): bytes equal to each one's stage path */
}
TEST(GpuGraphSlot, RingReuseNeverServesWrongSlot) {
  /* launch S for slots 1000..1003 (ring of 3); a job for slot 1000 after 1003 was launched must take own_fep = 1 (counter slot_miss + 1) and equal its stage path */
}
```
Write both with the comparison block of `GpuGraphSplit.ThreeLaunchesEqualSingleGraphAndStagePath`. `fx.second_grant()` is a new fixture method: the same IQ ring, a second set of PRBs and its own TB, built by the same `small_grant` code with `start` moved past the first grant.

- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement.** `k_fep_exact` writes the slot grid: per (cell, ring entry) `rxF` of 4 x 14 x N c16, allocated pre-fence for `NR_GPU_GRAPH_CELL_MAX` cells. v1's per-instance `rxF` stays for `own_fep`. In the DIRECT model, the caller's `rxdataF` mirror is written only when the job sets `want_rxF` (Task 6 makes that the only way); until Task 6, keep v1's write-in-place.
- [ ] **Step 4: Tests, DIRECT + discrete. PASS.**
- [ ] **Step 5: Replay gate** `gpugraph_ab.sh t5 "<T4 flags>" "<T4 flags> ISAC_PDSCH_GPU_GRAPH_SLOT=1"`. Metric (f): `slot_miss` < 1 % of bodies; nsys (one run per capture, as in Task 3 Step 5): k_fep_exact launches per slot <= 1.05 x slots read.
- [ ] **Step 6: Commit** `feat(gpugraph): T5 slot FEP once per (cell, slot) at slot read, 3-slot grid ring (ISAC_PDSCH_GPU_GRAPH_SLOT, default off)`.

### Task 6: Readback contract, BRANCHFO and TB pack on the device, debug mode (`ISAC_PDSCH_GPU_GRAPH_LEAN=1`)

Tail impact: host after the graph is p50 53-54 us, p99 619-704 us, and host before is p50 65-108, p99 252-337 [MEASURED spec 10.0]. v1's DISCRETE path copies est + rxF (917 KB each) and the LLR buffer every pass [CODE-READ nr_gpu_pass_graph.cu:476-480]; that matters for sens6.

**Files:**
- Modify: `nr_gpu_pass_graph.cu` (`k_brfo`, `k_tb_pack`, request-only D2H nodes, NOGRAPH mode)
- Modify: `nr_gpu_pass.h` (`want_*` request bits)
- Modify: `nr_pdsch_passive_decode.c` (`gp_brfo_measure` D:2464 uses the device result; the requests are set before launch from the consumers that need them: shadow check, CSI / sensing rxdataF consumers, stage-path fallback)
- Test: `tests/nr_gpu_pass_test.cc`

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces:
```c
/* nr_gpu_pass_job_t */
uint8_t want_rxF;  /* copy the slot grid symbols in copy_mask to the caller's rxdataF (DISCRETE D2H; DIRECT write in place) */
uint8_t want_est;  /* est rows to the host (brfo_rows consumers other than BRANCHFO) */
uint8_t want_llr;  /* final LLRs to llr_out (shadow, stage-path fallback); v1's "LLRs on CRC fail" becomes this request */
/* nr_gpu_pass_result_t */
int32_t brfo[4][2]; /* per antenna sum H1 conj(H0) (re, im) as gp_brfo_measure computes it on the host today */
```
Env `ISAC_PDSCH_GPU_GRAPH_NOGRAPH=1`: the sub-graph enqueue functions run stream-ordered without capture, for compute-sanitizer and nsys kernel attribution. Results must be identical.

- [ ] **Step 1: Failing tests**
```cpp
TEST(GpuGraphLean, BrfoEqualsHost) { /* k_brfo vs gp_brfo_measure's loop (D:2464) on the fixture's est rows: equal int32 sums */ }
TEST(GpuGraphLean, RequestedCopiesEqualDirect) {
  /* run in the discrete ctest: want_rxF / want_est / want_llr each on and off; with the bit on, bytes equal the DIRECT run; with it off, the caller buffer is untouched (poisoned 0xA5 stays) */
}
TEST(GpuGraphLean, NoGraphModeEqualsGraphMode) { /* ISAC_PDSCH_GPU_GRAPH_NOGRAPH=1 vs 0: identical result blocks for every warm shape */ }
```
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement.**
  - `k_brfo` is one block per antenna: an integer reduction over N of `h1 * conj(h0)` from the est rows of the first and last DM-RS symbol, the same arithmetic and accumulation width as `gp_brfo_measure`. Read that function first and copy its integer types.
  - `k_tb_pack` writes the reassembled TB (v1 does this as a host memcpy at `nr_gpu_pass_graph.cu:964`) into the result block. DIRECT: the host reads the result block in place.
  - Every `nr_gpu_buf_d2h_async` in `enqueue_L` except the result block is guarded by its `want_*` bit, read from the parameter block at capture time. Capture two L variants (want_llr 0/1) instead of a runtime branch: the KeyL field `cp` = request bits.
  - The body sets `want_llr` when a shadow is due or when a stage-path fallback follows; it sets `want_rxF` when a CSI / sensing consumer reads rxdataF for this slot (`copy_mask`, as v1 does).
- [ ] **Step 4: Tests, DIRECT + discrete. PASS.**
- [ ] **Step 5: Replay gate** `gpugraph_ab.sh t6 "<T5 flags>" "<T5 flags> ISAC_PDSCH_GPU_GRAPH_LEAN=1"`, plus one ON run per capture with `ISAC_GPU_FORCE_DISCRETE=1` (functional: gate items a-d only). Metric (f): from the `GPUGRAPH` stamps, host post p99 <= 150 us and host pre p99 <= 200 us.
- [ ] **Step 6: Commit** `feat(gpugraph): T6 lean readback (request-only copies), BRANCHFO and TB pack on device, NOGRAPH debug mode (ISAC_PDSCH_GPU_GRAPH_LEAN, default off)`.

### Task 7: Final gate, multi-cell hook check, sm_89 build, OTA hand-off

**Files:**
- Test: `tests/nr_gpu_pass_test.cc` (`GpuGraphCell.CellIndexBoundsAndIsolation`)
- Create: `docs/superpowers/reports/csd/GPUGRAPH-FINAL-<date>.md`
- Create: `/home/nicola/NICOLA/wt/rr-orchestration/ota/t26gG.env` (= t26gE + all GPUGRAPH flags), proposed to the orchestrator, not used by the agent.

- [ ] **Step 1: Failing test**
```cpp
TEST(GpuGraphCell, CellIndexBoundsAndIsolation) {
  /* job.cell = NR_GPU_GRAPH_CELL_MAX -> dl_try2 returns 1 (fb_unsup + 1), nothing launched; cell 0 and cell 1 slot grids for the same slot_abs hold different IQ
   * (fixture rings with different seeds) and two grants, one per cell, decode to their own stage-path bytes */
}
```
- [ ] **Step 2: Run, expect FAIL until the bound check is in `dl_try2` and the slot ring is indexed by cell; implement; PASS (DIRECT + discrete).**
- [ ] **Step 3: Build check for both archs.**
  - Run `cuobjdump --list-elf cmake_targets/ran_build/build_t26/libpdsch_gpu.so | grep -c sm_89` and the same with `sm_121`; both must be >= 1.
  - Run the full ctest: `cd cmake_targets/ran_build/build_t26 && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh env CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps ctest -j4 < /dev/null`. Expected failures are only the known ARM ones (dft_test, test_nr_modulation, test_nr_pusch_ra0_qam64/256, test_nr_td_grantwork).
- [ ] **Step 4: Final replay A/B.** `gpugraph_ab.sh final "" "<all GPUGRAPH flags>"` with n=6 per arm per capture: edit the loop count for this run only, or call it twice.
  - Report goal_metrics (tot p50 / p99 / p99.9, crc_q, drops, n_real, recovered_tot_p99) and `gpugraph_tail.py` (tail component means, stage-path share of the tail).
  - Run nsys once per capture with spec 10.1's scripts: real-body wall / busy / gaps p50 / p99, against spec 10.1 (stage path) and 10.0 (v1).
- [ ] **Step 5: Compare with the budget.** Fill a table "estimate (spec 10.8) vs measured" for body wall p50 / p99 and tot p99 per capture. If tot p99 misses the estimate by > 20 %, use nsys to say which component misses: host pre, gaps, kernels, a2e, qw. Do not tune.
- [ ] **Step 6: sens6 instructions.** Append a section to the report in the style of `docs/superpowers/plans/SENS6-STAGE1-TASK9.md`: build with `-DCMAKE_CUDA_ARCHITECTURES="89;121"`, `test_nr_gpu_pass --gtest_filter='GpuGraph*:GpuPass*'`, one replay per capture with `t26gG`, and record `nvidia-smi` (4060 Ti 8 GB vs 4070 12 GB), the GPUGRAPH `mb` and the k_tail<double> time. The orchestrator runs it.
- [ ] **Step 7: Commit**
```bash
git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu docs/superpowers/reports/csd/GPUGRAPH-FINAL-*.md
git commit -m "test(gpugraph): T7 final replay gate, cell isolation, sm_89+sm_121 build check; t26gG proposed for the on-air A/B"
```
The env file lives in the rr-orchestration worktree; the orchestrator commits it there.

## After the plan (not tasks here)
- Orchestrator: on-air A/B, t26gG vs t26gE, daytime, n >= 3 per cell, both cells. Pass criteria: p99 < 3 ms, crc_q >= 80 %, drops < 1 %.
- If V misses (spec 10.8 predicts it may), the choice comes from Task 7's attribution and goes to the user (spec 10.12): LDPC cluster-2/4, FP32 demod, a2e.
- Next for the multi-cell goal: a cross-grant L launch, i.e. CB jobs of several grants in one L (spec 10.9).
- Then CPU core reduction: consumer count vs p99 at 10 % tolerance.
