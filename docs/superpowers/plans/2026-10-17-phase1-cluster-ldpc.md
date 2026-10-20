# Phase 1 Task 14: cluster-2/4 LDPC variant for R13 32-CB (stretch) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the DGX DL GPU LDPC time of a 32-code-block R13 launch on the 40-SM RT partition from today's ~441 us (nc) toward 200 us, with a new env-gated kernel family that stays bit-exact vs the `_nc` kernels.

**Architecture:** Generalise the two existing kernels (`k_ldpc_t`, `k_ldpc4_t`) from `template <bool NC>` to `template <int CB>` (CB = blocks per code block: 1 = today's `_nc`, 8 = today's cluster, new 2 and 4). `decide_nc`/`ldpc_launch` pick CB per launch from "does the launch fit in ONE wave of the RT partition". Because the SM arithmetic below shows cluster-2/4 cannot reach 200 us at C=32 (it only helps 4 <= C <= 20), Task 4 adds the lever that can: a shared-memory-resident single-block variant (fewer L2 round trips per block). Everything default off.

**Tech Stack:** CUDA C++ (sm_121 GB10 now, sm_89 sens6 must stay on a CB=1 family), cooperative_groups clusters, `test_nr_pdsch_gpu_ldpc` ctest binary, `heavy.sh`.

**Spec:** `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md` (section 3 row 6: "R13 32 CB does not fit (441 us, gap 240 us)"; section 8 NC row), `docs/superpowers/plans/2026-10-16-phase1-dl-g1-g3.md` Part C Task 14 (contract below, binding). Evidence: `docs/superpowers/reports/csd/LDPC-PERCB-20261016.md`, `T5-LDPC-REASONS-20261016.md`, `ALLON-OTA-20261016.md`, `GPU-INVENTORY-20261016.md`.

**Contract (binding, from the Phase 1 plan):** new kernel family selected by `decide_nc` for C >= 16 at low rate; bit-exact vs `_nc`; bench with `test_nr_pdsch_gpu_ldpc 2 bench`. Acceptance: R13 32 CB <= 200 us on the 40-SM RT partition.

## Feasibility verdict (read first)

All numbers `[MEASURED, DGX spark-74c3 GB10 bench, 2026-10-16, t26f env (RT_ISOLATE=1 DISC_SMS=8 POLL=200), BG1 Z=384 K=8448 it_max 8, run-wall p50, LDPC-PERCB]` unless marked.

| fact | value |
|---|---|
| R13 nc run-wall, C=1 / 8 / 16 / 32 | 347 / 352 / 358 / 441 us |
| R13 cluster-8 run-wall, C=1 / 4 / 8 / 16 / 32 | 114 / 278 / 352 / 646 / 1230 us |
| RT partition | 40 SMs; 128 regs x 512 threads = whole register file -> 1 block per SM [MEASURED cuobjdump, LDPC-PERCB] |

SM arithmetic [INFERRED from the table, to be confirmed by Task 1]:
- 200 us x 40 SMs = 8000 SM-us; for 32 CBs that is <= 250 SM-us per CB. nc spends 347-441 SM-us per CB (one SM for the whole kernel). Cluster-8 spends ~900.
- C=32 nc is already ONE wave (32 blocks <= 40 SMs). Its time is the latency of one block (347 us alone, 441 us with 32 concurrent: +27 % from L2/DRAM contention). A bigger cluster only helps by shortening one block's latency using more SMs:
  - cluster-2: 32 CBs x 2 = 64 blocks > 40 SMs -> 2 waves (20 + 12 clusters). Needs d2 <= 100 us (3.5x faster than nc on 2x the SMs: superlinear) to reach 200 us. Even with perfect linear scaling d2 = 174 us -> 2 waves = 347 us = no better than nc.
  - cluster-4: 10 clusters/wave -> 4 waves; needs d4 <= 50 us (7x on 4x SMs). Perfect scaling gives 347 us again.
  - Cluster-8 measured (above) confirms the trend: 3 concurrent clusters, 1230 us.
- Where cluster-2/4 DO help: when the launch leaves SMs idle, i.e. 4 <= C <= 10 (cluster-4, one wave) and 11 <= C <= 20 (cluster-2, one wave). Estimated one-wave time ~ 347 / (scaling 1.5-1.9 for cluster-2, 2.2-3 for cluster-4) = ~180-230 us (cluster-2, C=16..20) and ~115-160 us (cluster-4, C<=10), vs nc 350-360 us [INFERRED from the C=1 cluster-8 point: 3.04x on 8 SMs; NOT measured for 2/4].

**Conclusion:** the contract point "cluster-2/4 variant brings R13 C=32 to <= 200 us" is infeasible by occupancy arithmetic. What the plan delivers: (a) cluster-2/4 family, selected for C in [16, 20] (cl2) (and [4, 10] cl4 behind a lower `ISAC_GPU_LDPC_CL_MIN_CB`), bit-exact, measured; (b) Task 4, the only lever that attacks C=32: make the single block faster (shared-memory-resident `res`/`llr`, no `__ldcg` round trip per edge). Its gain is unmeasured; Task 4 starts with a go/no-go experiment and the final report states the achieved C=32 number, which may stay above 200 us. Other levers outside this task (so not here): CPU `prep_segment` (0.32-0.38 ms at CB16+, Task for P-DEMATCH) dominates wall time on air once LDPC is ~0.4 ms; in-kernel CRC early exit; `it_max` below 8 (needs the CPU-reference check).

## Dependency (state it in every handoff)

Another agent is editing `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu` (uncommitted at plan time, `git status` shows .cu/.h/_host.c/_test.c modified) to pad the byte-packed snapshot for K%8 != 0 (tiny BG2 CBs, T5-LDPC-REASONS). This plan builds ON TOP of that commit:
- Tasks 2-4 must not start until `git log --oneline -5 -- openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu` shows that padding commit. Task 1 (measure) may run before: Z=384 uses `k_ldpc4_t` (Z%4==0, K%8==0), untouched by padding; label the commit hash anyway.
- Our edits are kept off the snapshot code: we only change kernel template heads, `blk_rank`/`blk_bar`, `ldpc_launch`, `warm_common`, `decide_nc`. The padding agent changes the `KB`/`ktail` lines inside `k_ldpc_t` (scalar kernel); do NOT touch them, and after rebasing keep their version of those lines.
- The bit-exact tests must include their new K%8 != 0 cases (cl2/cl4 modes run the same body as modes 2/nc).

## Global Constraints
- Worktree `/home/nicola/NICOLA/wt/rr-csd-int16`, branch `csd/int16`. Builds only `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 <targets>` in `cmake_targets/ran_build/build_t26`, always with `< /dev/null`; agents cannot run cmake (Task 2 adds ctest lines: say in the report that the orchestrator must re-run cmake).
- Never build while one of OUR receivers runs: `for p in $(pgrep -x nr-uesoftmodem); do sudo -n readlink /proc/$p/exe; done | grep -q ^/home/nicola/` must be false. Bounded waits only. Never touch other users' processes.
- Every CUDA program/ctest: `mkdir -p /tmp/nicola_nomps; export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps` (shared host, other user's MPS hangs CUDA).
- Agents never run OTA, uhd_*, X410, DPDK. This task is bench/ctest only. No per-cell constants.
- New behaviour default off behind `ISAC_GPU_LDPC_CL_MIN_CB` (0 = off) and `ISAC_GPU_LDPC_SM` (0 = off); CPU path and existing `_nc`/cluster-8 kernels unchanged when off. sm_89 (sens6): cluster kernels never selected (`decide_nc` major < 9 -> st 2); the new CB=2/4 paths require `st == 1`; the smem variant (Task 4) is CB=1 and is allowed on sm_89 only after the smem opt-in size is checked at runtime.
- `git add <explicit paths>`, never -A, never stash. Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y`. Retry on `.git/index.lock` 5 s apart, max 5.
- Evidence label on every number, e.g. `[MEASURED, DGX GB10 bench, t26f env, commit <hash>, p50 of 100 reps]`. Never mix with OTA.
- Reports returned as text; the orchestrator saves them.

## Review Focus
1. A launch with n CBs must never pick a cluster size whose clusters cannot co-reside (n*CB > partition SMs is allowed only if the code accepts multi-wave; the selection function must return CB=1 for n=32 on 40 SMs). Pinned by `test_pick_cb_fits_one_wave` (Task 2, host-only unit test inside the test binary mode `pick`).
2. The cluster launch with CB=2/4 and 1 block/SM needs a free SM pair in the SAME GPC; if the launch fails (`cudaErrorInvalidConfiguration` / `cudaErrorLaunchOutOfResources`) the run must fall back to the `_nc` kernel in the same call, not to the CPU. Pinned by `LDPC_GPU_TEST cl fallback` (Task 2, forced via `ISAC_GPU_LDPC_CL_FORCE=3`, an invalid size).
3. sm_89 / `ISAC_GPU_LDPC_NOCLUSTER=1` must never launch a cluster of any size (`__trap` below sm_90). Pinned by running `test_nr_pdsch_gpu_ldpc_nc` with `ISAC_GPU_LDPC_CL_MIN_CB=4` set (Task 2 step 7).
4. Snapshots (nsnap, syn0, bytes) identical, not only the final bits: the verdict logic depends on iteration count. The existing CPU-reference comparison checks bits and syn0 per job; Task 2 also compares nc vs cl byte-for-byte via a direct A/B mode.
5. Mixed launches (UL pool `ul_exec` also calls `ldpc_launch`): UL must keep today's choice unless explicitly enabled. Pinned by `test_nr_pdsch_gpu_ldpc ul` unchanged and green with CL env set.

---

### Task 1: Measure the baseline on the 40-SM partition and calibrate the estimate (no kernel code)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_gpu_ldpc_test.c` (the `BENCH_CFG` printf in `bench()`, line ~118: print device facts)
- Report (returned as text): `docs/superpowers/reports/csd/CLLDPC-BASELINE-<date>.md`

**Interfaces:**
- Consumes: bench output lines `BENCH_GPU R<rate> it=8 decodbl C=<C> | kernel ... p50 ... | run-wall mean ... p50 ... p99 ...`.
- Produces: the measured baseline table that Tasks 2-4 compare against, plus `smem_optin`, `regs_per_sm`, `sm_count` facts.

- [ ] **Step 1: Add device facts to the bench header.** In `bench()` after the existing `BENCH_CFG` printf add (dlsym `cudaDeviceGetAttribute` from `g`):

```c
  int (*cAttr)(int *, int, int) = dlsym(g, "cudaDeviceGetAttribute");
  if (cAttr) { /* 16 = MultiProcessorCount, 97 = MaxSharedMemoryPerBlockOptin, 81 = MaxSharedMemoryPerMultiprocessor, 82 = MaxRegistersPerMultiprocessor */
    int smc = 0, so = 0, sm = 0, rg = 0;
    cAttr(&smc, 16, 0); cAttr(&so, 97, 0); cAttr(&sm, 81, 0); cAttr(&rg, 82, 0);
    printf("BENCH_DEV sm_count=%d smem_optin=%d smem_per_sm=%d regs_per_sm=%d\n", smc, so, sm, rg);
  }
```

(If `dlsym` of `cudaDeviceGetAttribute` is NULL, `libpdsch_gpu.so` does not re-export it: use `cudaDeviceGetAttribute` via `dlopen("libcudart.so", RTLD_NOW)` instead, same signature.)

- [ ] **Step 2: Build.**

Run: `cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build_t26 && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 test_nr_pdsch_gpu_ldpc pdsch_gpu ldpc < /dev/null`
Expected: links, no errors.

- [ ] **Step 3: Run the exact baseline bench, nc and cluster arms, 3 repeats each (variance).**

```bash
mkdir -p /tmp/nicola_nomps
OUT=/home/nicola/NICOLA/wt/rr-orchestration/replays/clldpc_base; mkdir -p $OUT
for arm in nc cl; do for r in 1 2 3; do
  NCV=0; [ $arm = nc ] && NCV=1
  /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh env CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps \
    ISAC_GPU_RT_ISOLATE=1 ISAC_GPU_DISC_SMS=8 ISAC_GPU_POLL_SLEEP_US=200 ISAC_BENCH_FINE=1 ISAC_BENCH_R=13 ISAC_GPU_LDPC_NOCLUSTER=$NCV \
    bash -c 'cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build_t26 && ./test_nr_pdsch_gpu_ldpc 2 bench 100' < /dev/null > $OUT/${arm}_$r.txt 2>&1
done; done
grep -h "BENCH_DEV\|BENCH_CFG" $OUT/nc_1.txt; grep -h "BENCH_GPU R13 it=8 decodbl" $OUT/nc_*.txt $OUT/cl_*.txt | awk '{print FILENAME, $0}' | head -80
```

(`ISAC_BENCH_FINE=1` gives C = 1..10,12,16,24,32; `ISAC_GPU_LDPC_NOCLUSTER=0` is read as "not 1" -> cluster family.) Idle host required (heavy.sh gates it). Expected: nc C=32 run-wall p50 ~441 us (LDPC-PERCB), cluster C=32 ~1230 us.

- [ ] **Step 4: Fill in the estimate.** From the measured rows compute per family and per C: waves = ceil(C*CB/40), SM-us per CB = kernel p50 x CB. Check the three claims of the feasibility verdict: (1) nc C=32 kernel p50 vs C=1 (the contention term); (2) cluster-8 per-CB steady state; (3) `smem_optin` (>= 52 KB needed for Task 4 level 2, >= 26.2 KB for level 1). Also `cuobjdump --dump-resource-usage` of `libpdsch_gpu.so` for `k_ldpc4_t` regs and static smem.

- [ ] **Step 5: Decision gate G0 in the report.** If nc C=32 p50 <= 250 us (i.e. the 441 us number does not reproduce), say so and re-plan; otherwise confirm "200 us unreachable with cluster-2/4 at C=32" with the measured numbers and proceed.

- [ ] **Step 6: Commit the test change only.**

```bash
cd /home/nicola/NICOLA/wt/rr-csd-int16 && git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_gpu_ldpc_test.c && git commit -m "test(ldpc): bench prints sm_count, smem opt-in, regs per SM

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y"
```

---

### Task 2: Cluster-2/4 kernel family, bit-exact, env-gated (needs the padding commit)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu` (`blk_rank`/`blk_bar` ~:135-160, `k_ldpc_t` :166, `k_ldpc4_t` :338, `decide_nc` :550-569, `ldpc_launch` :575-595, `warm_common` :598-621)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_gpu_ldpc_test.c` (modes `cl2`, `cl4`, `clab`, `pick`)
- Modify: `openair1/PHY/CODING/CMakeLists.txt:129` area (ctest lines; orchestrator re-runs cmake)

**Interfaces:**
- Consumes: padding commit; `Graph`, `DJob`, `Ctx`, `nr_green_smmask(cudaStream_t, unsigned *m)` (nr_gpu_green.h:28).
- Produces:
  - `template <int CB> __global__ void k_ldpc_t(...)`, `template <int CB> __global__ void k_ldpc4_t(...)`, same argument lists; CB in {1, 2, 4, 8}; CB=1 is today's `<true>`, CB=8 today's `<false>`.
  - `static int pick_cb(int state, int n, int rt_sms, int min_cb, int force)`; returns 1, 2, 4 or 8. Rules, in order: `force` in {1,2,4,8} -> force; state 2 (non-cluster) -> 1; `min_cb == 0 || n < min_cb` -> today's choice (state 1 -> 8, or 1 when `g_nc_min_cb && n >= g_nc_min_cb`); else `n*4 <= rt_sms` -> 4; `n*2 <= rt_sms` -> 2; else 1.
  - `static int g_rt_sms` (popcount of `nr_green_smmask(g_ctx[0].s)` in `warm_common`; device SM count if the mask query fails).
  - env `ISAC_GPU_LDPC_CL_MIN_CB` (default 0 = off; recommended trial value 16), `ISAC_GPU_LDPC_CL_FORCE` (test only).
  - extern "C" `void nr_pdsch_gpu_ldpc_cl_stats(uint64_t o[4])` = launches with CB 1, 2, 4, 8 since start.

- [ ] **Step 1: Failing tests first.** In `nr_pdsch_gpu_ldpc_test.c` main(), next to the `mix` line add:

```c
  if (argc > 2 && !strcmp(argv[2], "cl2")) setenv("ISAC_GPU_LDPC_CL_FORCE", "2", 1);
  if (argc > 2 && !strcmp(argv[2], "cl4")) setenv("ISAC_GPU_LDPC_CL_FORCE", "4", 1);
```

and after the `if (nc) {...}` stats check add a check that the family really ran (this fails until the export exists):

```c
  if (argc > 2 && (!strcmp(argv[2], "cl2") || !strcmp(argv[2], "cl4"))) {
    void (*cst)(uint64_t *) = (void (*)(uint64_t *))dlsym(g, "nr_pdsch_gpu_ldpc_cl_stats");
    atexit_cl_check = cst; /* static pointer; at the end of main(): o[cb==2?1:2] > 0 and o[3] == 0 */
  }
```

At the end of main(), before the final return: `if (atexit_cl_check) { uint64_t o[4] = {0}; atexit_cl_check(o); int i = !strcmp(argv[2], "cl2") ? 1 : 2; if (!o[i] || o[3]) { printf("LDPC_GPU_TEST %s: cb=%d launches %lu, cb=8 launches %lu\n", argv[2], i == 1 ? 2 : 4, (unsigned long)o[i], (unsigned long)o[3]); return 2; } }`. Declare `static void (*atexit_cl_check)(uint64_t *);` at file scope.

Host-only unit test, mode `pick` (no GPU needed, placed before the dlopen of libs; `pick_cb` is reachable through an exported thin wrapper `int nr_pdsch_gpu_ldpc_pick_cb(int state, int n, int sms, int min_cb, int force)`):

```c
  if (argc > 2 && !strcmp(argv[2], "pick")) {
    int (*pk)(int, int, int, int, int) = dlsym(g, "nr_pdsch_gpu_ldpc_pick_cb");
    const int exp[][6] = { /* state n sms min force -> cb */
      {1, 32, 40, 16, 0, 1}, {1, 20, 40, 16, 0, 2}, {1, 16, 40, 16, 0, 2}, {1, 15, 40, 16, 0, 8},
      {1, 21, 40, 16, 0, 1}, {1, 10, 40, 4, 0, 4}, {2, 18, 40, 16, 0, 1}, {1, 18, 40, 0, 0, 8}, {1, 6, 40, 16, 3, 8} /* 3 invalid: ignored */ };
    for (unsigned i = 0; i < sizeof(exp) / sizeof(exp[0]); i++)
      if (pk(exp[i][0], exp[i][1], exp[i][2], exp[i][3], exp[i][4]) != exp[i][5]) { printf("LDPC_GPU_TEST pick row %u wrong\n", i); return 3; }
    printf("LDPC_GPU_TEST pick OK\n"); return 0;
  }
```

- [ ] **Step 2: Run to verify failure.** Build (`ninja -j6 test_nr_pdsch_gpu_ldpc pdsch_gpu ldpc`, heavy.sh, `< /dev/null`), then `heavy.sh env CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps bash -c 'cd .../build_t26 && ./test_nr_pdsch_gpu_ldpc 2 pick' < /dev/null`. Expected: FAIL (`dlsym` NULL segfault or "setup failed").

- [ ] **Step 3: Generalise the kernels.** Replace in `blk_rank`/`blk_bar` the `template <bool NC>` with `template <int CB>` and `if constexpr (NC)` with `if constexpr (CB == 1)`. In `k_ldpc_t` and `k_ldpc4_t`: `template <int CB> __global__ void __launch_bounds__(THREADS) ...`, delete the line `constexpr int CB = NC ? 1 : CLB;`, replace `blk_rank<NC>()`/`blk_bar<NC>()` with `<CB>`. `gs = CB * THREADS` and `gt = b * THREADS + t` already generalise. Instantiate explicitly after the kernels (so lazy loading sees them):

```cpp
template __global__ void k_ldpc_t<1>(const DJob *, int8_t *, int8_t *, int8_t *, int *, uint8_t *, int *);
template __global__ void k_ldpc_t<2>(...same...);  template __global__ void k_ldpc_t<4>(...);  template __global__ void k_ldpc_t<8>(...);
// and the same four for k_ldpc4_t
```

(write the full argument lists in the file; no ellipsis.) Do not touch any other line of the kernel bodies (padding agent's lines).

- [ ] **Step 4: Selection and launch.** Add `pick_cb` exactly per the Interfaces rules (plain C++ function, no CUDA), export `nr_pdsch_gpu_ldpc_pick_cb` and `nr_pdsch_gpu_ldpc_cl_stats`. In `decide_nc` read `ISAC_GPU_LDPC_CL_MIN_CB` and `ISAC_GPU_LDPC_CL_FORCE` into `g_cl_min_cb`, `g_cl_force` (non-numeric/negative -> 0; invalid force values other than 1,2,4,8 -> 0). In `ldpc_launch`: `const int cb = pick_cb(nc, n, g_rt_sms, g_cl_min_cb, g_cl_force);` and

```cpp
  cfg.gridDim = dim3(n * cb);
  at[0].val.clusterDim.x = cb;
  cfg.numAttrs = cb == 1 ? 0 : 1;
  k = z4 ? (cb == 1 ? k_ldpc4_t<1> : cb == 2 ? k_ldpc4_t<2> : cb == 4 ? k_ldpc4_t<4> : k_ldpc4_t<8>)
         : (cb == 1 ? k_ldpc_t<1> : cb == 2 ? k_ldpc_t<2> : cb == 4 ? k_ldpc_t<4> : k_ldpc_t<8>);
  cudaError_t e = cudaLaunchKernelEx(&cfg, k, ddj, x.llr, x.llrRes, x.c2v, x.unsat, dsnap, dres);
  if (e != cudaSuccess && (cb == 2 || cb == 4)) { /* no free SM group of this size: the _nc kernel, same results */
    (void)cudaGetLastError(); g_cl_fallback.fetch_add(1);
    cfg.gridDim = dim3(n); cfg.numAttrs = 0; e = cudaLaunchKernelEx(&cfg, z4 ? k_ldpc4_t<1> : k_ldpc_t<1>, ddj, x.llr, x.llrRes, x.c2v, x.unsat, dsnap, dres);
  }
  g_cb_launches[cb == 1 ? 0 : cb == 2 ? 1 : cb == 4 ? 2 : 3].fetch_add(1);
  return e;
```

The `nc` argument keeps its meaning (1 cluster, 2 non-cluster). `ldpc_launch` signature unchanged, so `nr_pdsch_gpu_ldpc_run`, `_dev` and the UL `ul_exec` call sites need no edit; the UL pool gets the same `pick_cb` and therefore the CL env too; guard: in `ul_exec` nothing changes, and since `ISAC_GPU_LDPC_CL_MIN_CB` default is 0 UL stays as today (Review Focus 5).

In `warm_common`, after the existing `cudaFuncGetAttributes` checks: if `g_cl_min_cb || g_cl_force`, also `cudaFuncGetAttributes` on `k_ldpc_t<2>, k_ldpc_t<4>, k_ldpc4_t<2>, k_ldpc4_t<4>` (lazy-load stalls otherwise) and set `g_rt_sms`:

```cpp
  { unsigned m[4] = {0}; int sms = 0; cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, 0);
    g_rt_sms = (g_ctx[0].s && nr_green_smmask(g_ctx[0].s, m)) ? __builtin_popcount(m[0]) + __builtin_popcount(m[1]) + __builtin_popcount(m[2]) + __builtin_popcount(m[3]) : sms; }
```

(`g_ctx[0].s` exists only after the pool init: call this at the end of `nr_pdsch_gpu_ldpc_init` instead, after the context loop, guarded by the same condition; `warm_common` only checks the function attributes.)

- [ ] **Step 5: Run `pick` and the two bit-exact modes.**

Run: `./test_nr_pdsch_gpu_ldpc 2 pick`, `... 2 cl2`, `... 2 cl4` (same heavy.sh/env wrapper as Step 2).
Expected: `pick OK`; cl2/cl4 print the usual `mismatches=0` summary (CPU reference comparison, all Z/BG/rate cases including the padding agent's K%8 cases) and the launch counters non-zero.

- [ ] **Step 6: Direct nc-vs-cl A/B (byte-for-byte snapshots).** Add mode `clab`: for R in {13, 23, 89}, C in {1, 4, 9, 16, 20, 32}, decodable and noise rows (reuse the bench input generator), run `dev()` twice on the same `lb` (once with `ISAC_GPU_LDPC_CL_FORCE` unset + `ISAC_GPU_LDPC_NOCLUSTER=1` semantics via a second `ldpc_launch` is not possible in one process because the family latches), so instead run the binary twice: `./test_nr_pdsch_gpu_ldpc 2 clab dump nc.bin` with `ISAC_GPU_LDPC_NOCLUSTER=1`, and `./test_nr_pdsch_gpu_ldpc 2 clab dump cl2.bin` with `ISAC_GPU_LDPC_CL_FORCE=2` (and `4`), each writing the NR_GPU_LDPC_DEV_BITS_BYTES region per case with a fixed seed; compare with `cmp`. Expected: `cmp` silent for cl2 and cl4 (identical nsnap, syn0 and snapshot bytes).

- [ ] **Step 7: Review-focus tests.** (a) Forced bad size: `ISAC_GPU_LDPC_CL_FORCE=3 ./test_nr_pdsch_gpu_ldpc 2` must equal the default run (invalid ignored). (b) `ISAC_GPU_LDPC_NOCLUSTER=1 ISAC_GPU_LDPC_CL_MIN_CB=4 ./test_nr_pdsch_gpu_ldpc 2 nc` passes and `cl_stats` shows cb>1 launches == 0. (c) `ISAC_GPU_LDPC_CL_MIN_CB=16 ./test_nr_pdsch_gpu_ldpc 2 ul` passes (mismatches=0). (d) existing ctest names `test_nr_pdsch_gpu_ldpc`, `_nc`, `_mix` green with all new env unset.

- [ ] **Step 8: ctest registration** (lines after `openair1/PHY/CODING/CMakeLists.txt:129`, mirroring the `_mix` line):

```cmake
  add_test(NAME test_nr_pdsch_gpu_ldpc_pick COMMAND ./test_nr_pdsch_gpu_ldpc 2 pick WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME test_nr_pdsch_gpu_ldpc_cl2 COMMAND ./test_nr_pdsch_gpu_ldpc 2 cl2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME test_nr_pdsch_gpu_ldpc_cl4 COMMAND ./test_nr_pdsch_gpu_ldpc 2 cl4 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
```

Report that cmake must be re-run by the orchestrator before `ctest -R test_nr_pdsch_gpu_ldpc` sees them; until then run the binaries directly.

- [ ] **Step 9: Commit.** `git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_gpu_ldpc_test.c openair1/PHY/CODING/CMakeLists.txt`, message `perf(ldpc): cluster-2/4 kernel family (env-gated, default off), bit-exact vs nc`, trailer as in Global Constraints.

---

### Task 3: Bench the family and fix the selection thresholds

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_gpu_ldpc_test.c` (bench header prints `cl_min_cb`, `cl_force` next to `nocluster`)
- Report (text): `docs/superpowers/reports/csd/CLLDPC-BENCH-<date>.md`

**Interfaces:**
- Consumes: Task 2 family, Task 1 baseline.
- Produces: measured p50/p99 per (family, C) for R13/R23/R89 (decodable and noise); the `pick_cb` thresholds to ship (default `ISAC_GPU_LDPC_CL_MIN_CB` recommendation).

- [ ] **Step 1:** In the `BENCH_CFG` printf add `cl_min_cb=%s cl_force=%s` from `getenv("ISAC_GPU_LDPC_CL_MIN_CB")`, `getenv("ISAC_GPU_LDPC_CL_FORCE")`.

- [ ] **Step 2: Run the arms** (same wrapper and env as Task 1 Step 3, 3 repeats each, outputs `replays/clldpc_bench/<arm>_<r>.txt`): arm `nc` (`ISAC_GPU_LDPC_NOCLUSTER=1`), `cl2` (`ISAC_GPU_LDPC_CL_FORCE=2`), `cl4` (`ISAC_GPU_LDPC_CL_FORCE=4`), `sel` (`ISAC_GPU_LDPC_CL_MIN_CB=4`, `ISAC_GPU_LDPC_NC_MIN_CB=8`, i.e. the selection function). Drop `ISAC_BENCH_R` to cover R13, R23, R89.

- [ ] **Step 3: Tabulate** per C: run-wall p50/p99, kernel p50, waves, SM-us. Compare to the Task 1 estimate (cl2 expected ~180-230 us at C=16..20, cl4 ~115-160 us at C<=10; both 2 waves / 4 waves slower than nc beyond one wave).

- [ ] **Step 4: Decide gate G1.** (a) Set the shipped recommendation: `ISAC_GPU_LDPC_CL_MIN_CB` = the smallest C where `sel` beats `nc` p50 AND p99 in all three rates (R89/R23 may favour a different value: if so report per-rate and keep `pick_cb` rate-blind with the max of the thresholds). (b) State R13 C=32 `sel` vs `nc`: it is expected to be equal (pick_cb returns 1). If cl2 at C=32 ever beats nc, pick_cb needs a "waves <= 2" rule: only then change it, with a new row in the `pick` test first.

- [ ] **Step 5: Commit** the bench header change (`git add` the test file), message `test(ldpc): bench header prints the cluster-2/4 selection env`.

---

### Task 4: Shared-memory-resident single-block variant (the lever for C=32), gated on an experiment

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu` (`cn_item`, `cn_item4`, `k_ldpc_t`, `k_ldpc4_t`, `pick_cb`, `ldpc_launch`, `warm_common`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_gpu_ldpc_test.c` (modes `sm`, family check), `openair1/PHY/CODING/CMakeLists.txt` (ctest line)
- Report (text): `docs/superpowers/reports/csd/CLLDPC-SMEM-<date>.md`

**Interfaces:**
- Consumes: Task 1 `smem_optin`; Task 2 template/`pick_cb`.
- Produces: `template <int CB, int SM> __global__ ... k_ldpc4_t` (SM = 0 global state as today, 1 = `res` in smem, 2 = `res` + `llr` in smem); env `ISAC_GPU_LDPC_SM` (0 default, 1, 2); `pick_cb` unchanged (SM only applies when cb == 1); stats slot `o[4]` in `nr_pdsch_gpu_ldpc_cl_stats` becomes `uint64_t o[5]` (launches with SM > 0).

Rationale [INFERRED]: nc is latency-bound (347 us on 1 SM vs 114 us on 8; 128 regs x 512 threads = 16 warps/SM), and per edge the CN phase does 2 dependent global loads (`__ldcg(&res)`, `llr`) plus the `c2v` load; in the nc case nothing is shared between blocks, so `res` (<= 68x384 = 26112 B) and `llr` (26112 B) can live in shared memory (52 KB total; needs the dynamic-smem opt-in, `cudaFuncSetAttribute(..., cudaFuncAttributeMaxDynamicSharedMemorySize, bytes)`). `c2v` (<= 316x384 = 121 KB) stays in global/L2 unless Task 1 shows `smem_optin` >= ~150 KB. GB10 smem per block is probably 99 KB [UNVERIFIED, Task 1 measures it]; a cluster-2 variant with the whole state in DSMEM (173 KB / 2 blocks = 87 KB per block) is the next candidate if it fits, but cluster-2 at C=32 is 2 waves, so it is not pursued.

- [ ] **Step 1: Go/no-go experiment (throwaway, not committed).** Hack `k_ldpc4_t<1>` locally: `__shared__ int8_t sres[68*384+16]` (static, 26.2 KB), copy `res` in at init, replace `res` pointer in the CN and BN phases (`ld4(res + ...)` becomes a plain 32-bit shared read; `res[w] = rv` writes smem). Bench R13 C=1 and C=32 (Task 1 command, `ISAC_GPU_LDPC_NOCLUSTER=1`). Gate G2: if C=32 run-wall p50 does not drop below 380 us (< 15 % gain) STOP: write the report "smem residency of res does not move the needle; 200 us unreachable on the 40-SM partition with these kernels; remaining levers are in-kernel CRC exit and it_max, both outside this task". If it drops to <= 300 us continue with Step 2 (and also try `llr` in dynamic smem, level 2).

- [ ] **Step 2: Failing test.** Add mode `sm` to the test (sets `ISAC_GPU_LDPC_NOCLUSTER=1`, `ISAC_GPU_LDPC_SM=1`, same CPU-reference comparison as `nc`) and an assert that `nr_pdsch_gpu_ldpc_cl_stats(o)` returns `o[4] > 0` (smem launches counted). Run it (Task 2 wrapper): FAIL (`o[4]` is 0 / symbol absent).

- [ ] **Step 3: Implement level 1 properly.** Make the res access a template parameter so the global (`__ldcg`) and shared paths do not share a load: in `cn_item<D>` and `cn_item4<D>` add `template <int SM>` and `rr[k] = SM ? res[n] : __ldcg(&res[n])` (for the 4-wide form `ld4s(row, start, Z)` = `ld4` with plain loads, since `__ldcg` on a shared address is illegal). Never use the generic-address `__ldcg` on the smem copy. `k_ldpc_t<CB, SM>` / `k_ldpc4_t<CB, SM>`: `SM != 0` requires `CB == 1` (`static_assert`). Snapshot code (padding agent's) reads `rv` locally, unchanged. Instantiate `<1,1>` (and `<1,2>` if kept) for both kernels. In `ldpc_launch`: `sm = (cb == 1) ? g_sm_level : 0` where `g_sm_level` comes from `ISAC_GPU_LDPC_SM` and is forced to 0 if `cudaDevAttrMaxSharedMemoryPerBlockOptin` < the level's bytes (level 2 sets the dynamic size via `cfg.dynamicSmemBytes` and `cudaFuncSetAttribute` in `warm_common`, so no stall at the first launch). A launch failure falls back to `<1,0>`.

- [ ] **Step 4: Run to verify.** `./test_nr_pdsch_gpu_ldpc 2 sm`: `mismatches=0`, `o[4] > 0`. Re-run Task 2 Step 6 byte-for-byte A/B with `ISAC_GPU_LDPC_SM=1` against the nc dump: `cmp` silent. Also `ISAC_GPU_FORCE_DISCRETE=1 ./test_nr_pdsch_gpu_ldpc 2 sm` (functional, per Global rules for new GPU code on the DGX).

- [ ] **Step 5: Bench and decide.** Task 1 Step 3 command with `ISAC_GPU_LDPC_NOCLUSTER=1 ISAC_GPU_LDPC_SM=1` (and 2), 3 repeats. Acceptance: R13 C=32 run-wall p50 <= 200 us `[MEASURED, DGX GB10 bench, t26f env, commit <hash>, n=3x100 reps]`; also p99 and R23/R89 must not regress (> 5 % slower than nc at any C in {1,4,8,16,32} is a failure for that rate: then SM applies only for R13-like E, selection by `n_edges` is NOT allowed per-cell, so use code rate class from the graph, which is generic). If C=32 lands in (200, 300] report "closest achievable" with the number; do not weaken the contract silently.

- [ ] **Step 6: ctest line + commit.** `add_test(NAME test_nr_pdsch_gpu_ldpc_sm COMMAND ./test_nr_pdsch_gpu_ldpc 2 sm WORKING_DIRECTORY ${CMAKE_BINARY_DIR})` after the cl4 line (orchestrator re-runs cmake). `git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_gpu_ldpc_test.c openair1/PHY/CODING/CMakeLists.txt`, message `perf(ldpc): smem-resident single-block LDPC kernel (ISAC_GPU_LDPC_SM, default off)`.

---

### Task 5: Final report (no code)

- [ ] **Step 1:** Return one text report: Task 1 baseline table (nc/cluster, C=1..32, regs, smem_optin), Task 3 table (nc / cl2 / cl4 / sel), Task 4 result and gate outcomes G0/G1/G2, the final statement "R13 32 CB = X us on 40 SMs vs 200 us target", recommended default values of `ISAC_GPU_LDPC_CL_MIN_CB` and `ISAC_GPU_LDPC_SM` for the OTA A/B (orchestrator only, daytime, n >= 3 per cell, V and T separate), commit hashes, and the cmake re-run request. State what stays unverified: on-air multi-consumer contention (5 consumers share the 40 SMs, so cluster-2 pairs may not find free SM groups: the `g_cl_fallback` counter must be printed in the stats line before the OTA A/B), sm_89 (sens6) behaviour of the smem variant, A725 timing.

## Self-review
- Spec coverage: contract (family selected via `decide_nc`/`ldpc_launch`, C >= 16 default threshold, bit-exact vs nc by CPU reference and byte-for-byte A/B, bench command, 200 us acceptance with explicit infeasibility analysis and a best-achievable lever) all mapped to Tasks 1-4.
- Known simplification: `pick_cb` is rate-blind (selection by n and SM count only); rate-dependent thresholds would need the graph's rate class passed into `ldpc_launch` and are added only if Task 3 shows R23/R89 disagree.
- Type consistency: `pick_cb(int,int,int,int,int)`, `nr_pdsch_gpu_ldpc_cl_stats(uint64_t *)` is `o[4]` in Task 2 and `o[5]` in Task 4 (callers in the test must be updated together in Task 4 Step 2).
