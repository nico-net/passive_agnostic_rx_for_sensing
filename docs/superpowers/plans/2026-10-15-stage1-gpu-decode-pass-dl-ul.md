# Stage 1 — GPU decode pass for PDSCH (DL) and PUSCH (UL), DGX Spark + sens6 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One DL or UL grant decode pass runs on the GPU as a single captured chain (FEP → chest → demod → rate-dematch → LDPC → CRC) with one host wait per pass, recovery passes moved off the critical path, on both the DGX Spark (GB10, sm_121, shared memory) and sens6 (RTX 4070, sm_89, discrete PCIe, weak FP64).

**Architecture:** Keep today's kernels (they are bit-exact and tested) and remove the host between them: per-pass host work that only depends on the DCI (geometry, pilots, allocation tables) is computed before launch; host work that depends on GPU results (nvar/SFO/DM-RS FO after chest, rate dematching, CRC) becomes small kernels; the chain is recorded once per "shape class" as a CUDA graph and replayed with `cudaGraphExecKernelNodeSetParams`. A memory-model layer gives every buffer a device address on both platforms (direct host access on GB10, explicit pinned H2D/D2H on the 4070). Recovery arms (TDRA-S toggle first) run on a low-priority recovery worker that owns the IQ snapshot and may publish a late rescue. UL gets the same chain (its chest/equalizer/LLR move from CPU to GPU); UL timing and DM-RS-id discovery leave the per-grant path (per-RNTI cache; the full background lane is Stage 3).

**Tech Stack:** C11 + CUDA (nvcc, `--fmad=false` for bit-exact files), CUDA graphs (CUDA ≥ 12.5 driver API already used by `nr_gpu_green.h`), cuFFT, gtest, ctest, Ninja, Python 3 analysis scripts in `tests/passive_rx/`.

**Spec:** `docs/superpowers/reports/csd/SYSAN-20261015.md` on branch `csd/sysan` (worktree `/home/nicola/NICOLA/wt/rr-csd-sysan`, commit 207264ca9a) — read §2 (timeline), §3-4 (CPU/GPU budget), §6 (limiting problems), §7 (architecture), §9 (OTA). Design decisions: memory `gpu-decode-pipeline-plan` (sens6 = RTX 4070). Code map: this plan's "Code map" section (from the 2026-10-15 exploration of csd/otatest 26d3028b5d).

## Global Constraints

- CLAUDE.md hard rules apply verbatim: sens6 frozen paths untouched (`git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30`); new configs `.cfg`; gNB logs/configs are ground truth only; evidence labels (PROJECT_MEMORY §0.1) on every claim, naming the host (`DGX` or `sens6 x86_64 RTX 4070`); never build while one of our `nr-uesoftmodem` runs; SIGINT only; `ISAC_RX_BRANCH_FO` unset; `git add <explicit paths>`, never `-A`, never stash.
- Every new behaviour is behind a default-off flag; umbrella flag `ISAC_GPU_PASS=1` (DL) and `ISAC_UL_GPU_PASS=1` (UL). Flags off = csd/otatest behaviour, bit for bit.
- GB10 FP64 path: decoded bits, LLRs and CRC verdicts bit-exact vs today's GPU exact path (`ISAC_PDSCH_GPU_EXACT_*`) — proven by the existing self-check counters (`check_bad=0`) and new unit tests.
- FP32 path (required on sens6, optional on GB10, flag `ISAC_GPU_PASS_FP32=1`): no `double` in any hot kernel; LLR sign agreement ≥ 99.9 % and CRC-OK count within ±1 % of the FP64 path on the same replay; never presented as bit-exact.
- CUDA archs: `CMAKE_CUDA_ARCHITECTURES` stays `"89;121"` (CMakeLists.txt:10-15, `cmake_targets/nr_gpu_arch.cmake`); every new `.cu` file builds for both.
- All GPU allocations, streams, events and graph instantiations happen before the RX fence `nr_gpu_rx_started()` (wrapped by `NR_DL_GPU_ALLOC` in `openair1/PHY/NR_UE_TRANSPORT/nr_dl_gpu_memory.h`); pinned host buffers are first-touched at allocation (FATTAILS: GPU first touch of unwritten host pages stalls 20-900 ms).
- No busy spin: every host wait uses `nr_gpu_poll_relax()` (`openair1/PHY/TOOLS/nr_gpu_poll.h`) or an event + sleep.
- RT work stays in the RT green-context partition (`nr_gpu_green_stream(..., rt=1, ...)`, `ISAC_GPU_RT_ISOLATE=1`); no new RT priority above existing threads.
- Target (per operator, never pooled): DL whole-call p50 ≤ 300 µs and p99 ≤ 1.0 ms for grants ≤ 96 PRB; DL first pass p99 ≤ 500 µs for ≤ 96 PRB; UL locked-grant p50 ≤ 600 µs; CRC-OK not lower than `t26f` on the same captures.
- Host commands: build `cd <wt>/cmake_targets/ran_build/build_t26 && flock -s /home/nicola/NICOLA/wt/rr-orchestration/locks/heavy.lock nice -n 19 ninja -j8 nr-uesoftmodem raw_iq pdsch_gpu tests`; own-receiver check `for p in $(pgrep -x nr-uesoftmodem); do sudo -n readlink /proc/$p/exe; done | grep -q ^/home/nicola/` (never `pgrep -af … | grep` in a loop); all CUDA programs with `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps`.

## Review Focus

- Grant shapes never seen at graph-build time (new PRB count, Nl, Qm, DM-RS config, > 32 code blocks): expected → fall back to the existing non-graph GPU path for that grant, counted (`gpupass_fallback`), never a crash or a wrong decode. Test in Task 4.
- IQ ring overwritten while a pass or a recovery arm is in flight (sample lifetime): expected → the pass returns UNSUP exactly as `passive_decode_body_core` does today (lines 2546-2575 checks), recovery arm dropped and counted. Test in Task 6.
- Discrete GPU (4070) with the shared-memory model unavailable (`nr_gpu_storage()` returns UNAVAILABLE today): expected → the discrete path is chosen automatically, never the direct-host-access kernels. Test in Task 1 (forced discrete on GB10).
- Late rescue arriving after a newer decode of the same HARQ process / RNTI: expected → discarded by NDI/HARQ-id/slot check, counted, never overwriting a newer result. Test in Task 6.
- GPU self-check mismatch on the new path: expected → `check_bad` increments and `ISAC_GPU_PASS` turns itself off for the process (same policy as the exact demod today). Test in Task 5.

---

## Code map (verified 2026-10-15 on csd/otatest 26d3028b5d)

DL (`T = openair1/PHY/NR_UE_TRANSPORT`, `D = T/nr_pdsch_passive_decode.c`):
- Entry `nr_pdsch_passive_decode()` D:5447 → `passive_decode_retry()` D:5922 → `passive_decode_body()` D:5771 → `passive_decode_body_gated()` D:5749 → `passive_decode_body_core()` D:2353-5412.
- SSB-slot CPU FFT D:2546-2575 (`nr_slot_fep_ant_snapshot`); prototype `ISAC_SSB_FEP_GPU` on csd/sysan already moves it to GPU.
- FEP D:2951-2994 → `nr_pdsch_gpu_exact_fep_ranges()` T/nr_pdsch_gpu_exact.c:138 → `nr_pdsch_gpu_exact_fep2()` T/nr_pdsch_gpu_fep.cu:1088 → `k_fep_exact` (T/nr_pdsch_gpu_exact_fep.cuh:266); wait `ctx_wait` fep.cu:167 (sync #1).
- Chest D:3194-3280 → `nr_pdsch_gpu_chest_grant()` openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2185 → `nr_pdsch_gpu_exact_chest()` fep.cu:1307: `nrch::k_ls` → `nr_exact::k_delay_exact` → `nrch::k_fir`; wait fep.cu:1340 (sync #2); CPU nvar/SFO/DM-RS FO after it, `pdtim_add(PDTIM_CHEST)` D:3717.
- Demod D:4476-4531 → `nr_pdsch_gpu_demod_grant()` T/nr_pdsch_gpu_demod_host.c:1096 → CPU geometry `nr_dlsch_extract_geometry` (demod_host.c:583-595) → `nr_pdsch_gpu_demod_run()` T/nr_pdsch_gpu_demod.cu:1449: k_gather, k_level, k_mask_pw, k_align, [k_mmse3_prep], `k_tail<MM>`; EVM chain on `x.s2`; poll demod.cu:1554-1561 (sync #3). FP64: k_align 249-297, Gauss-Jordan in k_tail 333-560, mode 3 680-760 + `nr_mmse_exact.h`, EVM double2.
- LDPC D:4948 → `passive_ldpc_decode_core()` D:1756 → `nr_pdsch_gpu_ldpc_decode()` T/nr_pdsch_gpu_ldpc_host.c:284 (CPU `prep_segment` 114 = deinterleave + `nr_rate_matching_ldpc_rx`) → `nr_pdsch_gpu_ldpc_run()` T/nr_pdsch_gpu_ldpc.cu:556 (one `cudaLaunchKernelEx` k_ldpc4, poll 604 = sync #4); CRC on CPU `check_crc` ldpc_host.c:191, TB CRC D:1995.
- Retry arms D:6107-6150 (bitmask CSI 1 | TDRA-S 2 | CDM 4 | wide-MRC 8 | tinterp 16; `RY_ARMS` 128 at D:5875); yield rule D:6117-6121 (`ry_backlog()` D:5878 → `nr_pdsch_passive_queue_backlog_real` T/nr_pdsch_passive_queue.c:2793; `retry_budget_us()` D:5889).
- GPU infra: green contexts `T/nr_gpu_green.h:22`; budget `openair1/PHY/TOOLS/nr_gpu_budget.c/.h` (`nr_gpu_budget_rt_begin/_end` 67/79); platform/memory model `openair1/PHY/TOOLS/nr_gpu_platform.c:66` (`nr_gpu_storage()`: DEVICE / STAGING / SHARED_RW; only `NR_GPU_SHARED` allocates); direct mode requires pageable access fep.cu:785-789; context pools are 8-slot CAS bitmasks; existing CUDA-graph pattern in `openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:561, 640-650, 1165-1205`.
- Library `pdsch_gpu` (openair1/PHY/CODING/CMakeLists.txt:105: fep.cu + demod.cu + ldpc.cu, demod `--fmad=false`), dlopen'd (demod_host.c:95, exact.c:76).
- Tests: `test_nr_llr_norm` (CMakeLists.txt:3004, has `MmseExact.GpuMode3BitExact` at nr_llr_norm_test.cc:298, dlopens `./libpdsch_gpu.so`, GTEST_SKIP without GPU); `test_nr_pdsch_gpu_ldpc` / `test_nr_pusch_gpu_ldpc` (CODING/CMakeLists.txt:115-126); `test_nr_pusch_gpu_fep` (openair1/PHY/MODULATION/tests/CMakeLists.txt:11-18).

UL (`P = T/nr_pusch_passive_decode.c`):
- `nr_pusch_passive_decode()` P:2756 → `_inner()` P:1639-2741: HARQ gate 1699; TA sweep 1762; FEP 1785-1953 (`ulg_fep_symbols` 999 → `nr_pusch_gpu_fep_run` T/nr_pusch_gpu_fep.cuh:174, `k_ul_fep`, poll 203-212); timing search 1867-1920 (`score_impl` T/nr_pusch_ul_disc.c:114, CPU); chest + equalizer CPU (`nr_rx_pusch_group_tp` P:2058 → openair1/PHY/NR_TRANSPORT/nr_ulsch_demodulation.c:643, `nr_pusch_channel_estimation` openair1/PHY/NR_ESTIMATION/nr_ul_channel_estimation.c:468); LDPC `nr_ulsch_decoding()` P:2415 with `nr_pusch_gpu_ldpc_wrap()` ldpc_host.c:336 → `nr_pusch_gpu_ldpc_run` ldpc.cu:844; UCI search 2477-2570; HARQ store 2580.

## File structure

| file | responsibility |
|---|---|
| `openair1/PHY/TOOLS/nr_gpu_mem.h` / `.c` (new) | `nr_gpu_buf_t`: one buffer = device pointer + host pointer + model (DIRECT / DISCRETE); `nr_gpu_buf_alloc`, `_h2d_async`, `_d2h_async`; first-touch at alloc |
| `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass.h` (new) | shared descriptors: `nr_gpu_pass_shape_t`, `nr_gpu_pass_job_t`, `nr_gpu_pass_result_t`, API |
| `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu` (new) | shape-class graph cache: record, instantiate (pre-fence), update params, launch, wait |
| `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_post.cuh` (new) | new small kernels: `k_chest_post` (nvar/SFO/DM-RS FO), `k_rdm` (deinterleave + rate dematch), `k_crc` (CRC24A/24B/16 + TB CRC) |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu` (modify) | FP32 instantiations of k_align / k_tail / mode-3 behind a template parameter |
| `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c` (new) | host side of the DL pass: build job from DCI, choose shape class, call graph, publish result |
| `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c` (new) + `nr_gpu_pass_ul.cuh` (new) | UL pass: UL chest/equalizer/LLR kernels + same graph machinery |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c` / `.h` (new) | recovery worker: owned IQ snapshot, arm list, late-rescue publish with HARQ/NDI guard |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (modify) | call sites only: `ISAC_GPU_PASS` branch in `passive_decode_body_core`, recovery hand-off in `passive_decode_retry` |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` (modify) | call site `ISAC_UL_GPU_PASS` branch; per-RNTI timing/DM-RS-id cache use |
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc` (new) | gtest: memory model, graph cache, post kernels, DL/UL pass parity |
| `openair1/PHY/CODING/CMakeLists.txt` (modify) | add new `.cu` to `pdsch_gpu`; new test target `test_nr_gpu_pass` |
| `tests/passive_rx/gpupass_ab.sh` (new) | interleaved replay A/B driver (flags off vs on), per operator |

---

### Task 1: GPU memory model for both platforms (`nr_gpu_buf_t`)

**Files:**
- Create: `openair1/PHY/TOOLS/nr_gpu_mem.h`, `openair1/PHY/TOOLS/nr_gpu_mem.c`
- Modify: `openair1/PHY/TOOLS/nr_gpu_platform.c:66` (expose `nr_gpu_model()` used by the new layer; `ISAC_GPU_FORCE_DISCRETE=1` forces DISCRETE on GB10 for testing)
- Modify: `CMakeLists.txt` (add `nr_gpu_mem.c` to the `nr_gpu_platform` library, line ~1059)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc`

**Interfaces:**
- Produces:
```c
typedef enum { NR_GPU_MODEL_DIRECT = 0, NR_GPU_MODEL_DISCRETE = 1 } nr_gpu_model_t;
typedef struct { void *dev; void *host; size_t bytes; nr_gpu_model_t model; } nr_gpu_buf_t;
nr_gpu_model_t nr_gpu_model(void);                       /* DIRECT iff NR_GPU_SHARED && pageable access && !ISAC_GPU_FORCE_DISCRETE */
int  nr_gpu_buf_alloc(nr_gpu_buf_t *b, size_t bytes);     /* pre-fence only; host side pinned + memset(0) (first touch) */
int  nr_gpu_buf_h2d_async(nr_gpu_buf_t *b, size_t off, size_t n, void *stream); /* no-op on DIRECT */
int  nr_gpu_buf_d2h_async(nr_gpu_buf_t *b, size_t off, size_t n, void *stream); /* no-op on DIRECT */
void nr_gpu_buf_free(nr_gpu_buf_t *b);
```
On DIRECT `dev == host` (mapped/managed, as today). On DISCRETE `dev` = `cudaMalloc`, `host` = `cudaHostAlloc(cudaHostAllocDefault)`.

- [ ] **Step 1: Write the failing test**

```cpp
// nr_gpu_pass_test.cc
#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
static bool have_gpu() { return nr_gpu_model() >= 0 && getenv("NR_GPU_TEST_SKIP") == nullptr; }

TEST(GpuMem, RoundTripBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  for (int force : {0, 1}) {
    setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
    nr_gpu_buf_t b;
    ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 4096));
    EXPECT_EQ(force ? NR_GPU_MODEL_DISCRETE : nr_gpu_model(), b.model);
    for (int i = 0; i < 4096; i++) ((unsigned char *)b.host)[i] = (unsigned char)i;
    ASSERT_EQ(0, nr_gpu_buf_h2d_async(&b, 0, 4096, nullptr));
    memset(b.host, 0, 4096);
    ASSERT_EQ(0, nr_gpu_buf_d2h_async(&b, 0, 4096, nullptr));
    cudaDeviceSynchronize();
    if (b.model == NR_GPU_MODEL_DISCRETE)
      for (int i = 0; i < 4096; i++) ASSERT_EQ((unsigned char)i, ((unsigned char *)b.host)[i]);
    nr_gpu_buf_free(&b);
  }
}
TEST(GpuMem, AllocIsFirstTouched) {
  if (!have_gpu()) GTEST_SKIP();
  nr_gpu_buf_t b;
  ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 1 << 20));
  for (size_t i = 0; i < (1u << 20); i += 4096) ASSERT_EQ(0, ((unsigned char *)b.host)[i]);
  nr_gpu_buf_free(&b);
}
```
Register in `openair1/PHY/CODING/CMakeLists.txt` next to `test_nr_gpu_green` (line ~128):
```cmake
add_executable(test_nr_gpu_pass ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc)
target_link_libraries(test_nr_gpu_pass PRIVATE GTest::gtest_main nr_gpu_platform CUDA::cudart)
add_test(NAME test_nr_gpu_pass COMMAND test_nr_gpu_pass)
set_tests_properties(test_nr_gpu_pass PROPERTIES ENVIRONMENT "CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps" SKIP_RETURN_CODE 77)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd cmake_targets/ran_build/build_t26 && flock -s /home/nicola/NICOLA/wt/rr-orchestration/locks/heavy.lock nice -n 19 ninja -j8 test_nr_gpu_pass`
Expected: FAIL to compile — `nr_gpu_mem.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

```c
// nr_gpu_mem.c
#include "nr_gpu_mem.h"
#include "nr_gpu_platform.h"
#include <cuda_runtime.h>
#include <stdlib.h>
#include <string.h>
nr_gpu_model_t nr_gpu_model(void) {
  const char *f = getenv("ISAC_GPU_FORCE_DISCRETE");
  if (f && atoi(f)) return NR_GPU_MODEL_DISCRETE;
  return nr_gpu_platform_shared_direct() ? NR_GPU_MODEL_DIRECT : NR_GPU_MODEL_DISCRETE;
}
int nr_gpu_buf_alloc(nr_gpu_buf_t *b, size_t bytes) {
  if (nr_gpu_rx_started()) return -1;                 /* allocation fence */
  memset(b, 0, sizeof(*b)); b->bytes = bytes; b->model = nr_gpu_model();
  if (b->model == NR_GPU_MODEL_DIRECT) {
    if (cudaHostAlloc(&b->host, bytes, cudaHostAllocMapped) != cudaSuccess) return -1;
    memset(b->host, 0, bytes);
    return cudaHostGetDevicePointer(&b->dev, b->host, 0) == cudaSuccess ? 0 : -1;
  }
  if (cudaHostAlloc(&b->host, bytes, cudaHostAllocDefault) != cudaSuccess) return -1;
  memset(b->host, 0, bytes);
  if (cudaMalloc(&b->dev, bytes) != cudaSuccess) return -1;
  return cudaMemset(b->dev, 0, bytes) == cudaSuccess ? 0 : -1;
}
int nr_gpu_buf_h2d_async(nr_gpu_buf_t *b, size_t off, size_t n, void *s) {
  if (b->model == NR_GPU_MODEL_DIRECT) return 0;
  return cudaMemcpyAsync((char *)b->dev + off, (char *)b->host + off, n, cudaMemcpyHostToDevice, (cudaStream_t)s) == cudaSuccess ? 0 : -1;
}
int nr_gpu_buf_d2h_async(nr_gpu_buf_t *b, size_t off, size_t n, void *s) {
  if (b->model == NR_GPU_MODEL_DIRECT) return 0;
  return cudaMemcpyAsync((char *)b->host + off, (char *)b->dev + off, n, cudaMemcpyDeviceToHost, (cudaStream_t)s) == cudaSuccess ? 0 : -1;
}
void nr_gpu_buf_free(nr_gpu_buf_t *b) {
  if (b->model == NR_GPU_MODEL_DISCRETE && b->dev) cudaFree(b->dev);
  if (b->host) cudaFreeHost(b->host);
  memset(b, 0, sizeof(*b));
}
```
In `nr_gpu_platform.c` add `int nr_gpu_platform_shared_direct(void)` returning 1 iff the probed model is `NR_GPU_SHARED` and `pageableMemoryAccess && pageableMemoryAccessUsesHostPageTables` (the same test as fep.cu:785-789; move it here and call it from fep.cu).

- [ ] **Step 4: Run test to verify it passes**

Run: `cd cmake_targets/ran_build/build_t26 && CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps ctest -R test_nr_gpu_pass --output-on-failure`
Expected: PASS (2 tests) on GB10; both models exercised.

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/TOOLS/nr_gpu_mem.h openair1/PHY/TOOLS/nr_gpu_mem.c openair1/PHY/TOOLS/nr_gpu_platform.c openair1/PHY/TOOLS/nr_gpu_platform.h openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc openair1/PHY/CODING/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpupass): nr_gpu_buf_t memory model for shared (GB10) and discrete (RTX 4070) GPUs"
```

### Task 2: Existing DL kernels run on device buffers (discrete-capable FEP/chest/demod/LDPC)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu` (FEP 1088-1173: add DISCRETE branch = H2D of the slot window of the IQ ring into a device ring buffer, kernel writes device `rxdataF`; chest 1307-1340: inputs/outputs as `nr_gpu_buf_t`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu:1449-1561` (gather from device `rxdataF`/`est`, k_tail writes device LLR buffer; D2H of LLRs only when the CPU needs them — on the graph path it does not)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu:556-604` (accept LLRs already on device; skip the host copy)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc`

**Interfaces:**
- Consumes: `nr_gpu_buf_t`, `nr_gpu_model()` (Task 1).
- Produces: `int nr_pdsch_gpu_fep_dev(const nr_gpu_fep_job_t *j, nr_gpu_buf_t *rxF_dev, void *stream);`, `int nr_pdsch_gpu_chest_dev(const nr_gpu_chest_job_t *rows, int n, nr_gpu_buf_t *est_dev, nr_gpu_buf_t *nvar_dev, void *stream);`, `int nr_pdsch_gpu_demod_dev(const nr_gpu_demod_job_t *j, nr_gpu_buf_t *llr_dev, void *stream);`, `int nr_pdsch_gpu_ldpc_dev(const nr_gpu_ldpc_job_t *j, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream);` — each launches only (no wait), so Task 4 can capture them. Existing entry points keep their signature and wait as today.

- [ ] **Step 1: Write the failing test** — parity of each stage on DIRECT vs forced DISCRETE using the fixtures already used by `MmseExact.GpuMode3BitExact` (nr_llr_norm_test.cc:298) and `nr_pdsch_gpu_fep_test`:

```cpp
TEST(GpuPassStages, DlStagesSameBitsBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  GpuPassFixture fx = GpuPassFixture::small_grant(/*nrb=*/24, /*nl=*/2, /*qm=*/4, /*seed=*/7);
  std::vector<int16_t> llr_direct, llr_discrete;
  setenv("ISAC_GPU_FORCE_DISCRETE", "0", 1); ASSERT_EQ(0, fx.run_stages(&llr_direct));
  setenv("ISAC_GPU_FORCE_DISCRETE", "1", 1); ASSERT_EQ(0, fx.run_stages(&llr_discrete));
  ASSERT_EQ(llr_direct.size(), llr_discrete.size());
  EXPECT_EQ(0, memcmp(llr_direct.data(), llr_discrete.data(), llr_direct.size() * 2));
  EXPECT_EQ(fx.cpu_reference_llr(), llr_direct);           // today's CPU path = reference
}
```
`GpuPassFixture` (put it in `tests/nr_gpu_pass_fixture.h`, new) builds a synthetic slot: random QAM symbols mapped with OAI's own `nr_modulation` + DM-RS from `nr_pdsch_dmrs_rx`, a fixed 2×4 channel, AWGN at 20 dB; `run_stages` calls the four `_dev` functions on one stream and D2Hs the LLRs; `cpu_reference_llr` runs `nr_slot_fep` + `nr_pdsch_channel_estimation` + `nr_rx_pdsch` (the CPU path used at D:4481-4527).

- [ ] **Step 2: Run test to verify it fails** — `ctest -R test_nr_gpu_pass` → FAIL: undefined `nr_pdsch_gpu_fep_dev`.
- [ ] **Step 3: Implement** the four `_dev` entry points by splitting each existing function into "launch" (new `_dev`) + "wait/copy" (existing body). DISCRETE FEP: one `nr_gpu_buf_h2d_async` of the IQ window `[slot_start - cp, slot_end]` for `nant` antennas into a device ring of 4 slots (allocated in `api_init` before the fence, fep.cu:826), then `k_fep_exact` reads the device ring.
- [ ] **Step 4: Run test to verify it passes** — `ctest -R test_nr_gpu_pass` PASS; also `ctest -R "test_nr_llr_norm|test_nr_pdsch_gpu_ldpc|test_nr_pdsch_gpu_demod"` still PASS.
- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_fixture.h
git commit -m "feat(gpupass): launch-only device-buffer entry points for DL FEP/chest/demod/LDPC (shared + discrete)"
```

### Task 3: Device kernels for the host steps between stages

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_post.cuh`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu` (include the new header at the end, as `nr_pusch_gpu_fep.cuh` is included)
- Test: `nr_gpu_pass_test.cc`

**Interfaces:**
- Produces kernels (all `__global__`, launch-only wrappers `nr_gpu_pass_launch_*`):
  - `k_chest_post(const c16_t *est, int nsc, int nsym, int nl, int nant, float *nvar_out, float *sfo_out, float *fo_out)` — the CPU math after chest (D:~3280-3717: per-grant nvar, SFO, DM-RS FO) moved verbatim.
  - `k_rdm(const int16_t *llr, const nr_gpu_rdm_seg_t *segs, int nseg, int8_t *ldpc_in)` — deinterleave + `nr_rate_matching_ldpc_rx` (ldpc_host.c:114) per segment, one block per segment.
  - `k_crc(const uint8_t *bits, const nr_gpu_crc_seg_t *segs, int nseg, uint32_t tbs, uint8_t *seg_ok, uint8_t *tb_ok)` — CRC24B per CB and CRC24A/16 per TB, bit order as `check_crc` (ldpc_host.c:191, D:1995).
- [ ] **Step 1: Write the failing tests** — one per kernel against the CPU function it replaces, on random inputs (1000 seeds each):

```cpp
TEST(GpuPassPost, CrcMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  for (int seed = 0; seed < 1000; seed++) {
    CrcCase c = CrcCase::random(seed);            // random TB, random CB split, 10 % corrupted bits
    EXPECT_EQ(c.cpu_seg_ok(), c.gpu_seg_ok()) << seed;
    EXPECT_EQ(c.cpu_tb_ok(),  c.gpu_tb_ok())  << seed;
  }
}
TEST(GpuPassPost, RateDematchMatchesCpu) { /* nr_rate_matching_ldpc_rx vs k_rdm, BG1/BG2, rv 0..3, Qm 2..8 */ }
TEST(GpuPassPost, ChestPostMatchesCpu)   { /* nvar/SFO/FO: float, |diff| <= 1e-6 relative; FP64 build: bit-equal */ }
```
- [ ] **Step 2: Run** `ctest -R test_nr_gpu_pass` → FAIL (undefined kernels).
- [ ] **Step 3: Implement** the three kernels; `k_crc` uses a 256-entry table in `__constant__` memory built from the same polynomials as `check_crc`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `git add openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_post.cuh openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc && git commit -m "feat(gpupass): device kernels for chest post-processing, rate dematching and CRC"`

### Task 4: Shape-class graph cache and the DL pass

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass.h`, `nr_gpu_pass_graph.cu`, `nr_gpu_pass_dl.c`
- Modify: `openair1/PHY/CODING/CMakeLists.txt:105` (add `nr_gpu_pass_graph.cu` to `pdsch_gpu`), top `CMakeLists.txt` (add `nr_gpu_pass_dl.c` to the PHY UE library that holds `nr_pdsch_passive_decode.c`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` — in `passive_decode_body_core` after the sample-lifetime checks (≈ D:2575): `if (gpu_pass_on() && nr_gpu_pass_dl_try(&job, &res) == 0) goto gpu_pass_done;` — `gpu_pass_done` fills the same outputs the existing tail fills (decoded TB, CRC verdict, EVM/SNR fields, `pdtim_add`).
- Test: `nr_gpu_pass_test.cc`

**Interfaces:**
- Consumes: Tasks 1-3 `_dev` launchers and post kernels.
- Produces:
```c
typedef struct { uint16_t nrb_class, nsym, nl, qm, ndmrs, ncb_class; uint8_t fp32; } nr_gpu_pass_shape_t; /* nrb_class: 6,12,24,48,96,136,217,273; ncb_class: 1,2,4,8,16,32 */
typedef struct { /* everything the current body computes from the DCI before the first kernel */ ... } nr_gpu_pass_job_t;
typedef struct { uint8_t tb_ok; uint8_t seg_ok[32]; uint32_t tbs; float evm_db, snr_db, nvar_db; uint8_t *tb; uint32_t gpu_us; } nr_gpu_pass_result_t;
int  nr_gpu_pass_init(int max_classes);                         /* pre-fence: allocate buffers, record + instantiate one graph per warm class */
int  nr_gpu_pass_dl_try(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r); /* 0 = done; 1 = shape not cached -> caller uses the old path */
void nr_gpu_pass_stats(char *buf, int n);                       /* "GPUPASS launches= fallback= graph_hits= wait_us_p50= ..." */
```
Graph = [H2D window (DISCRETE only)] → k_fep_exact → k_ls → k_delay_exact → k_fir → k_chest_post → k_gather → k_level → k_mask_pw → k_align → [k_mmse3_prep] → k_tail → k_rdm → k_ldpc4 → k_crc → [D2H result block (≤ TBS bytes + 64 B header)]. EVM chain stays a parallel branch inside the same graph (fork/join nodes, as today with `x.s2`). Per launch only kernel params change (`cudaGraphExecKernelNodeSetParams`); one host wait (`nr_gpu_poll_relax` on an event) per pass. Warm classes are recorded at init from a synthetic job of that class; an unseen class returns 1 (old path, counted). Graphs are instantiated before `nr_gpu_rx_started()`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(GpuPassDl, GraphEqualsStagePath) {
  if (!have_gpu()) GTEST_SKIP();
  ASSERT_EQ(0, nr_gpu_pass_init(16));
  for (auto shape : GpuPassFixture::warm_shapes()) {           // 24/48/96/217 PRB x Nl 1..4 x Qm 2..8
    GpuPassFixture fx = GpuPassFixture::from_shape(shape, 11);
    nr_gpu_pass_result_t g{}, s{};
    ASSERT_EQ(0, nr_gpu_pass_dl_try(&fx.job, &g));
    ASSERT_EQ(0, fx.run_stage_path(&s));                        // Task 2 path + CPU CRC
    EXPECT_EQ(s.tb_ok, g.tb_ok);
    EXPECT_EQ(0, memcmp(s.tb, g.tb, g.tbs / 8));
  }
}
TEST(GpuPassDl, UnseenShapeFallsBack) {
  if (!have_gpu()) GTEST_SKIP();
  GpuPassFixture fx = GpuPassFixture::from_shape({/*nrb*/ 273, 14, 4, 8, 3, 32, 0}, 3); // not warmed
  nr_gpu_pass_result_t r{};
  EXPECT_EQ(1, nr_gpu_pass_dl_try(&fx.job, &r));
}
TEST(GpuPassDl, NoAllocAfterFence) { /* set the fence, call try on a warm class 1000x: allocation counter stays 0 */ }
```
- [ ] **Step 2: Run** → FAIL (undefined `nr_gpu_pass_init`).
- [ ] **Step 3: Implement** `nr_gpu_pass_graph.cu` following the existing pattern in `openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:561` (capture with `cudaStreamBeginCapture` on an RT green stream, `cudaGraphInstantiate` under `NR_DL_GPU_ALLOC`, key map as at 1165-1205); `nr_gpu_pass_dl.c` builds `nr_gpu_pass_job_t` from the same inputs `passive_decode_body_core` uses (geometry via `nr_dlsch_extract_geometry`, pilots, allocation) and computes them on the CPU before launch.
- [ ] **Step 4: Run** → PASS; full `ctest -j4` → only the known ARM failures (`test_nr_pusch_ra0_qam64/qam256`, `test_nr_td_grantwork`, `dft_test`, `test_nr_modulation`).
- [ ] **Step 5: Commit** `git add openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass.h openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc openair1/PHY/CODING/CMakeLists.txt CMakeLists.txt && git commit -m "feat(gpupass): one captured CUDA graph per DL shape class, one host wait per pass (ISAC_GPU_PASS, default off)"`

### Task 5: FP32 demod for the RTX 4070 (no FP64 in hot kernels)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu` (template parameter `typename R` = `double` | `float` on k_align 249-297, the Gauss-Jordan in k_tail 333-560, `k_mmse3_prep` 680-760, `k_tail<6/7/8>`; `nr_mmse_exact.h` gets a `float` instantiation `nr_mmse_exact_f`)
- Modify: `nr_gpu_pass_graph.cu` (shape field `fp32` selects the FP32 instantiation; `ISAC_GPU_PASS_FP32=1`, forced on when `cudaDeviceProp.major == 8 && minor == 9` i.e. sm_89)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c` (self-check: on the FP32 path compare LLR *signs* and CRC, not bits; mismatch policy: > 0.1 % sign disagreement on a checked grant → `check_bad++`, turn `ISAC_GPU_PASS` off)
- Test: `nr_gpu_pass_test.cc`

- [ ] **Step 1: Failing test**

```cpp
TEST(GpuPassFp32, SignAgreementAndCrc) {
  if (!have_gpu()) GTEST_SKIP();
  int crc64 = 0, crc32 = 0; size_t same = 0, total = 0;
  for (int seed = 0; seed < 400; seed++)
    for (auto shape : GpuPassFixture::warm_shapes()) {
      GpuPassFixture fx = GpuPassFixture::from_shape(shape, seed, /*snr_db=*/ 4 + seed % 20);
      auto a = fx.run_pass(/*fp32=*/0), b = fx.run_pass(/*fp32=*/1);
      crc64 += a.tb_ok; crc32 += b.tb_ok;
      fx.count_sign_agreement(a, b, &same, &total);
    }
  EXPECT_GE((double)same / total, 0.999);
  EXPECT_NEAR(crc32, crc64, 0.01 * crc64 + 1);
}
TEST(GpuPassFp32, NoDoubleInFp32Kernels) { /* cuobjdump -sass of the fp32 instantiations contains no DFMA/DADD/DMUL: run as a ctest COMMAND script */ }
```
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement** the templates (FP64 instantiation unchanged, so the GB10 bit-exact tests keep passing). **Step 4: Run** → PASS + `MmseExact.GpuMode3BitExact` still PASS. **Step 5: Commit** `git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu openair1/PHY/NR_UE_TRANSPORT/nr_mmse_exact.h openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc && git commit -m "feat(gpupass): FP32 demod/MMSE instantiation for consumer GPUs (ISAC_GPU_PASS_FP32; auto on sm_89)"`

### Task 6: Recovery off the critical path (late rescue)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.h`, `nr_pdsch_recovery.c`
- Modify: `nr_pdsch_passive_decode.c` `passive_decode_retry()` D:5922-6335: when `ISAC_PDSCH_RECOVERY_ASYNC=1`, after the first body fails CRC, publish the failed first-pass result immediately (as today when no arm rescues) and submit `{job, owned IQ snapshot ref, arm list}` to the recovery worker; the TDRA-S toggle arm (bitmask 2, 99 % of rescues per SYSAN §6.3) is first in the list.
- Modify: `nr_pdsch_passive_queue.c` (start the worker: thread `pdschRecov`, SCHED_OTHER nice 0, core list `ISAC_PDSCH_RECOVERY_CORES` default "3,4", queue depth `ISAC_PDSCH_RECOVERY_Q` default 32)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_recovery_test.cc` (new), CMake target `test_nr_pdsch_recovery` in the top `CMakeLists.txt` next to `test_nr_pdsch_gpu_demod` (~2489)

**Interfaces:**
- Produces:
```c
typedef struct { uint16_t rnti; uint8_t harq_pid, ndi; uint32_t abs_slot; uint32_t arm_mask; } nr_recov_key_t;
int  nr_pdsch_recovery_start(int depth, const char *cores);
int  nr_pdsch_recovery_submit(const nr_recov_key_t *k, void *job_owned /* freed by the worker */);
/* called by the worker on a rescue; returns 0 if published, 1 if stale (newer NDI/slot seen for (rnti,harq_pid)), 2 if IQ expired */
int  nr_pdsch_recovery_publish(const nr_recov_key_t *k, const uint8_t *tb, uint32_t tbs);
void nr_pdsch_recovery_stats(char *buf, int n);   /* "RECOV submitted= rescued= stale= expired= dropped_full= late_ms_p50/p99=" */
```
- [ ] **Step 1: Failing tests**

```cpp
TEST(Recovery, StaleRescueIsDiscarded) {
  nr_pdsch_recovery_start(4, "0");
  nr_recov_key_t k{0x4601, 3, 0, 1000, 2};
  nr_pdsch_recovery_note_newer(0x4601, 3, /*ndi=*/1, /*slot=*/1010);   // a newer TB on the same HARQ process
  uint8_t tb[16] = {0};
  EXPECT_EQ(1, nr_pdsch_recovery_publish(&k, tb, 128));
}
TEST(Recovery, ExpiredIqIsDropped) { /* job whose sample lifetime is past -> publish returns 2, counter expired++ */ }
TEST(Recovery, QueueFullDropsNotBlocks) { /* depth 4, submit 10 fast: 6 dropped_full, submit never blocks > 50 us */ }
TEST(Recovery, FirstArmIsTdraToggle) { /* arm order produced by nr_pdsch_recovery_arm_order(mask=0x1f) starts with 2 */ }
```
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement** (ring + condvar; `nr_pdsch_recovery_note_newer` is called from the queue on every enqueue). **Step 4: Run** → PASS. **Step 5: Commit** `git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.h openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_recovery_test.cc CMakeLists.txt && git commit -m "feat(recovery): recovery arms off the critical path with late-rescue guard (ISAC_PDSCH_RECOVERY_ASYNC, default off)"`

### Task 7: UL pass on the GPU (chest + equalizer + LLR + rate-dematch + LDPC + CRC)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.cuh` (kernels `k_ul_ls`, `k_ul_eq` (MMSE/ZF for Nl 1-2, reuse `k_tail` templates where the math is identical, CP-OFDM only; transform precoding → fall back), `k_ul_llr`), `nr_gpu_pass_ul.c` (host job builder)
- Modify: `nr_gpu_pass_graph.cu` (UL graph class: k_ul_fep → k_ul_ls → k_ul_eq → k_ul_llr → k_rdm → UL LDPC kernel (ldpc.cu:844 `ul_exec` kernel) → k_crc)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` — in `nr_pusch_passive_decode_inner` after the HARQ gate (P:1699): `if (ul_gpu_pass_on() && rnti_cache_hit(&hyp) && nr_gpu_pass_ul_try(&job, &res) == 0) goto ul_gpu_done;` where `hyp` = cached timing advance + DM-RS id for this RNTI (new per-RNTI cache `ul_hyp_cache` keyed by RNTI, filled by the existing timing/DM-RS-id search when it succeeds; the search itself stays inline until Stage 3 moves it to the background lane). Cache miss → existing path. UCI search stays on the CPU path (only when `ISAC_UL_UCI_BUDGET` > 0 and the GPU pass failed CRC).
- Test: `nr_gpu_pass_test.cc`

- [ ] **Step 1: Failing tests**

```cpp
TEST(GpuPassUl, MatchesCpuDecode) {
  if (!have_gpu()) GTEST_SKIP();
  for (int seed = 0; seed < 200; seed++) {
    UlFixture fx = UlFixture::random(seed);   // nr_ulsch_encoding + DM-RS + channel + AWGN, Nl 1..2, Qm 2..6, 6..217 PRB
    auto cpu = fx.cpu_decode();                // nr_rx_pusch_group_tp + nr_ulsch_decoding, as P:2058/2415
    auto gpu = fx.gpu_pass();
    EXPECT_EQ(cpu.tb_ok, gpu.tb_ok) << seed;
    if (cpu.tb_ok) EXPECT_EQ(0, memcmp(cpu.tb, gpu.tb, cpu.tbs / 8)) << seed;
  }
}
TEST(GpuPassUl, TransformPrecodingFallsBack) { /* DFT-s-OFDM grant -> nr_gpu_pass_ul_try returns 1 */ }
TEST(GpuPassUl, CacheMissUsesOldPath)        { /* unknown RNTI -> old path, counter ul_cache_miss++ */ }
```
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS (FP64 and FP32 instantiations). **Step 5: Commit** `git add openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.cuh openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc && git commit -m "feat(gpupass): UL PUSCH pass as one GPU graph with per-RNTI hypothesis cache (ISAC_UL_GPU_PASS, default off)"`

### Task 8: Replay A/B on the DGX (per operator) and the large-grant path

**Files:**
- Create: `tests/passive_rx/gpupass_ab.sh` (interleaved arms on `tests/passive_rx/p99_replay.sh`, captures `CAPSET=b` and `CAPSET=tim` separately, n ≥ 6 per arm)
- Modify (only if needed after measurement): `nr_pdsch_gpu_ldpc.cu` — code-block-parallel decode for > 96 PRB (one cluster per CB instead of 8-block clusters) behind `ISAC_GPU_PASS_LDPC_CBPAR=1`

- [ ] **Step 1:** Write `gpupass_ab.sh`:

```bash
#!/bin/bash
# usage: gpupass_ab.sh <capset b|tim> <n>   (runs inside heavy.lock per replay via p99_replay.sh)
set -e
S=$(cd "$(dirname "$0")" && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration/ota
for i in $(seq 1 "$2"); do
  for arm in OFF ON ONREC; do
    case $arm in
      OFF)   X="";;
      ON)    X="ISAC_GPU_PASS=1 ISAC_UL_GPU_PASS=1";;
      ONREC) X="ISAC_GPU_PASS=1 ISAC_UL_GPU_PASS=1 ISAC_PDSCH_RECOVERY_ASYNC=1";;
    esac
    CAPSET=$1 ENVFILE=$O/t26f.env "$S/p99_replay.sh" "gp_${1}_${arm}_$i" $X
  done
done
```
- [ ] **Step 2:** Run `tests/passive_rx/gpupass_ab.sh b 6` then `tests/passive_rx/gpupass_ab.sh tim 6`; summarise with `python3 -I tests/passive_rx/p99_stats.py --json <run>/run.log` (fields `dec_us.all`, per-PRB buckets, `pdschq`, `crc`, `bad_*`) and `tests/passive_rx/sysan_e2e.py` (from csd/sysan; merge or cherry-pick that tool first).
Expected (pass criteria, per capture, never pooled): `bad_chest = bad_demod = mismatch = 0`; CRC-OK (ON) ≥ CRC-OK (OFF) − 1 %; DL first pass p99 ≤ 500 µs for ≤ 96 PRB; whole call p50 ≤ 300 µs; UL locked-grant p50 ≤ 600 µs; `GPUPASS fallback` < 5 % of grants.
- [ ] **Step 3:** If > 96 PRB first-pass p99 > 1 ms, implement `ISAC_GPU_PASS_LDPC_CBPAR` and rerun Step 2 for that bucket only.
- [ ] **Step 4:** Write `docs/superpowers/reports/csd/GPUPASS-<date>.md` with the tables and evidence labels `[MEASURED, DGX replay <cap>, SPEED=1, n=6]`.
- [ ] **Step 5: Commit** `git add tests/passive_rx/gpupass_ab.sh docs/superpowers/reports/csd/GPUPASS-<date>.md [openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu] && git commit -m "test(gpupass): DGX replay A/B per operator"`

### Task 9: sens6 (x86_64, RTX 4070) build, tests and replay

**Files:**
- Create: `tests/passive_rx/sens6/gpupass_sens6.md` (exact commands used on sens6; host facts: CPU, cores, CUDA/driver versions, `nvidia-smi -q` summary)
- Modify: only if the build fails on x86 (keep fixes general; no `#ifdef` on host names)

- [ ] **Step 1:** On sens6, configure the cloud-x86 variant from CLAUDE.md plus CUDA: `cmake -S . -B cmake_targets/ran_build/build_s6 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_USRP=OFF -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89` (the user/orchestrator runs cmake; agents cannot).
- [ ] **Step 2:** `ninja nr-uesoftmodem rfsimulator params_libconfig nr-softmodem raw_iq pdsch_gpu tests && ctest -j4`. Expected: `test_nr_gpu_pass` PASS with `nr_gpu_model() == DISCRETE` and FP32 auto-selected; known ARM-only failures may pass on x86; list any new failure.
- [ ] **Step 3:** Copy the replay captures read-only (`raw_pci415_12s_b`, `raw_pci923_tim_12s_tile3`) and run `gpupass_ab.sh` on sens6 (n ≥ 6 per arm per capture). Pass criteria as Task 8 except: compare against the sens6 OFF arm (CPU path), and record H2D/D2H bytes and time per pass from `GPUPASS` stats.
- [ ] **Step 4:** Report section "sens6" in `GPUPASS-<date>.md` with `[MEASURED, sens6 x86_64 RTX 4070 replay <cap>, n=6]`.
- [ ] **Step 5: Commit** the report and any general build fix with explicit paths.

### Task 10: OTA (DGX, daytime) and merge readiness

- [ ] **Step 1:** Create env files `rr-orchestration/ota/t26g.env` = `t26f.env` + `ISAC_GPU_PASS=1 ISAC_UL_GPU_PASS=1 ISAC_PDSCH_RECOVERY_ASYNC=1` (single line, no duplicate keys: `head -1 t26g.env | tr ' ' '\n' | cut -d= -f1 | sort | uniq -d` empty) and `t26gF.env` = `t26g` + `ISAC_GPU_PASS_FP32=1`.
- [ ] **Step 2:** OTA window (daytime, `flock -x heavy.lock`, `HEAVY_HELD=1`, `ota_window_gen.sh`): specs interleaved `t26f / t26g / t26gF` × Vodafone 415 (`3680010000:1171:6900`) and TIM 923 (`3759990000:233:-4800`), n ≥ 3 per arm per cell, 150 s.
- [ ] **Step 3:** Analyse with `night_agg.py`-style aggregation per env × cell + `sysan_e2e.py`; pass criteria as Task 8 on air (allow first pass p99 ≤ 700 µs on air), `check_bad=0`, RFSTALL 0.
- [ ] **Step 4:** Report section "OTA" in `GPUPASS-<date>.md` with `[MEASURED, DGX OTA, X410 34B2AF1, <date> DAYTIME]`.
- [ ] **Step 5:** Commit; do not merge into csd/otatest without the user's go.

---

## Self-review notes

- Spec coverage (SYSAN §6-7): per-pass host round trips → Tasks 3-4; launches → Task 4 (graph); recovery passes → Task 6; large grants → Task 8 Step 3; SSB CPU FFT → already in csd/sysan (`ISAC_SSB_FEP_GPU`): cherry-pick 207264ca9a's SSB change before Task 4 and include the SSB symbols in the FEP node; UL per-grant discovery → Task 7 cache (full lane in Stage 3); sens6 discrete + FP32 → Tasks 1, 2, 5, 9.
- Type names used across tasks: `nr_gpu_buf_t`, `nr_gpu_model()`, `nr_gpu_pass_shape_t`, `nr_gpu_pass_job_t`, `nr_gpu_pass_result_t`, `nr_gpu_pass_dl_try`, `nr_gpu_pass_ul_try`, `nr_recov_key_t` — consistent.
