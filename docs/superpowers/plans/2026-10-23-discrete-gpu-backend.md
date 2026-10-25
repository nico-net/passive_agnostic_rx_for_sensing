# Discrete-GPU backend (3-tier GPU runtime: SHARED / DISCRETE / CPU) — Implementation Plan

Date: 2026-10-23. Base: `adaptive-rx-UL-DL` @ `e9c874c308`. Author: architect agent (plan only; no code changed, nothing committed).
Every statement below is `[CODE-READ @e9c874c308]` unless labelled otherwise. sens6 facts come from the 2026-10-23 sens6 session
(build/ctest logs in that session's scratchpad) and are labelled `[MEASURED, sens6 x86_64 RTX 4060 Ti, @e9c874c308]`.

**Goal (user, 2026-10-23):** the full DL and UL GPU decode that runs on the DGX Spark (GB10, integrated) also runs on any discrete
NVIDIA GPU on any host. The code picks the behaviour from the device's runtime properties. There is no per-host tuning, env or config.
The GB10 path must stay bit-identical. **Binding user requirement (relayed by the coordinator):** a 3-tier selection per device at init, logged
in the `SENSING: GPU_PLATFORM` line:
(1) **SHARED** whenever the device proves safe and fast direct host access;
(2) else **DISCRETE** (device buffers, pinned staging, explicit async copies);
(3) **CPU fallback** kept intact in every module (no GPU, module missing, breaker, OOM, CUDA error -> CPU, as today).
Each tier gets tests: shared, forced-discrete on an integrated GPU, and CPU.

Hard rules: CLAUDE.md applies verbatim (frozen sens6 paths, `.cfg`, labels, no build while `nr-uesoftmodem` runs, SIGINT only,
`ISAC_RX_BRANCH_FO` unset, `git add <explicit paths>`).

---

## 1. Where we are (survey)

### 1.1 Platform layer (`openair1/PHY/TOOLS/`)
- `nr_gpu_platform.c:54-58` `nr_gpu_choose_model`: SHARED iff `integrated || (pageableMemoryAccess && pageableMemoryAccessUsesHostPageTables)`, else DISCRETE.
- `:64` `nr_gpu_storage`: any model other than SHARED -> `NR_GPU_ALLOC_UNAVAILABLE`. So every `nr_gpu_alloc()` / `nr_gpu_allocate<>()` fails on discrete.
- `:171-172` warning "DISCRETE backend is not implemented (phase 2)".
- `:184-187` `nr_gpu_platform_ready()` requires SHARED. So `nr_gpu_module_check()` and therefore `nr_gpu_module_ready()` (`nr_gpu_module.c`) return 0 for **every** module linked with `nr_gpu_module.c`.
  That covers pdcch_nid_gpu, ldpc_cuda, td_cb0_gpu, pdsch_gpu, pdcch_gpu, polar_sc_cuda, csirs_disc_gpu and nr_csirs_disc_gpu_sweep (`openair1/PHY/CODING/CMakeLists.txt` last foreach).
- `:300` `nr_gpu_register_host` also refuses non-SHARED. `:310-324` copy/sync need `platform_ready`.
- `nr_gpu_mem.c` (`nr_gpu_buf_t`, Stage 1 Task 1, `701926ce41`) **already implements the DISCRETE buffer model**: pinned host plus `cudaMalloc`, with `_h2d_async/_d2h_async`.
  - `nr_gpu_model()` = DIRECT iff `nr_gpu_platform_shared_direct()` (SHARED && pageable && host tables), unless `nr_gpu_mem_test_force_discrete(1)`.
  - Two notions coexist today. The platform model is SHARED for any integrated device, but the buffer model is DIRECT only when pageable access is available.
    An integrated device without pageable access (Jetson-class) is "SHARED" for the platform and "DISCRETE" for the buffers.
- sens6 probe line `[MEASURED, sens6 …]`: `integrated=0 pageableMemoryAccess=1 pageableMemoryAccessUsesHostPageTables=0 canMapHostMemory=1 concurrentManagedAccess=1 model=discrete`.
  So **HMM is active on sens6**: a kernel *can* dereference `malloc` memory, but every access crosses PCIe and migrates pages.
  This is the trap the tier rule must avoid.

### 1.2 Per module (data path, what breaks on discrete, what must change)
| Module (lib) | How data reaches the GPU | On discrete today | Needed |
|---|---|---|---|
| **GPUPASS DL+UL graph** (`nr_gpu_pass_graph.cu`, host `nr_gpu_pass_dl.c`, `nr_gpu_pass_ul.c`) | `nr_gpu_buf_t` per instance (`alloc_inst` :1243-1263). DISCRETE branches exist: IQ window staged and H2D-captured in the graph (`fep_stage_impl` fep.cu:1104-1146, `nr_pdsch_gpu_fep_dev` :1159-1191; UL `ulwin` :871-878, :947); D2H of llr/nvar/est/rxF/res captured (:760-790, :954-968); host copies back to the caller (:1730, :1859) | Gate refuses. **Forced-discrete on GB10 is already tested** (`test_nr_gpu_pass_discrete`, CMake CODING:177) | Open the gate; fix 4 parity gaps (Task 5); CUDA 12.4 shims (Task 1) |
| exact FEP standalone (`nr_pdsch_gpu_exact_fep2`, fep.cu:1004) | DIRECT: kernel reads the host IQ ring in place; else a **staged** path (`upload_window` :776, one stream, one lock) | Staged path is discrete-ready | Nothing beyond the gate |
| exact chest / delay standalone (fep.cu:1202, :1326) | Host pointers in the kernel; guarded by `g_exact_direct` | Returns -1 -> CPU chest (correct) | Keep CPU on DISCRETE (bypass grants only); the pass uses `_dev` chest |
| demod standalone (`nr_pdsch_gpu_demod_run`, demod.cu:1588) | Kernels read `j->rxdataF/est/geo/comp/mag/llr` **host pointers directly**; **no model guard** | Not reached (gate). **After the gate opens, on an HMM host it would "work" through page migration, and on a non-HMM host it would fault the context: must be guarded** | Refuse unless `nr_gpu_model()==DIRECT` (Task 4) |
| demod `_dev` (pass) demod.cu:1722 | `nr_gpu_buf_t` | ok | — |
| PDSCH/PUSCH GPU LDPC (`nr_pdsch_gpu_ldpc.cu`) | `cudaHostAllocMapped` in/snap/res (:724-727, :918-921) + memcpy, device pointer via `cudaHostGetDevicePointer` | Zero-copy over PCIe: correct, small volumes | Nothing (gate only) |
| UL FEP standalone (`nr_pusch_gpu_fep.cuh:133-137`) | Host IQ in place | Refuses itself on non-direct -> CPU | Keep (the UL pass uses `nr_gpu_pass_ul_fep_dev` :301 with the exact-FEP tables, independent of this init) |
| G1 LDPC pool (`ldpc_decoder.cu` :729-848, :1060) | `nr_gpu_allocate` STAGING / SHARED_RW + DEVICE, explicit copies | Gate (`test_ldpc_cuda_pool` SEGFAULT on sens6 `[MEASURED]`: the test does not handle "not ready") | Gate; test skip on CPU tier |
| CB0 (`ldpc_cb0.cu`) | Always `MODE_EXPLICIT` (:522), staged 2D copies | Gate (`test_ldpc_cb0_cuda`: `A.init() = -6` `[MEASURED]`) | Gate only — reference pattern for EXPLICIT |
| td_cb0 (`nr_td_cb0_batch.cu:84-100`) | SHARED_RW/STAGING + DEVICE, `nr_gpu_copy_async` | Gate (test failed `[MEASURED]`) | Gate only |
| pdcch_gpu (`nr_pdcch_gpu_fep.cu:196-292`) | STAGING + DEVICE, `nr_gpu_copy_async` | Gate | Gate only |
| polar (`nr_polar_sc_cuda.cu:31, 260-266`) | DEVICE + STAGING | Gate (`nr_polar_sc_cuda_test` failed `[MEASURED]`) | Gate only |
| pdcch_nid_gpu (`nr_pdcch_nid_gpu.cu:527-611`) | Own `cudaMalloc`/`cudaHostAlloc` + explicit async copies; **does not call `nr_gpu_module_ready()`** | Already runs on sens6 ("PDCCH_NID_GPU ready: core=13") | Add the readiness gate (CPU tier) |
| csirs_disc_gpu (+sweep) (`nr_csirs_disc_gpu.cu:224-245, 514-519`; sweep :567) | Own `cudaMalloc` + explicit copies; ring storage `cudaHostAllocMapped`; `accumulate_host` reads the ring **in place** when its pointer is `cudaMemoryTypeHost` | Not gated; in-place read = full ring over PCIe every batch on discrete | Gate; in-place only when tier SHARED |
| pdsch exact host pin (`nr_pdsch_gpu_exact.c:280-311`, called `nr_pdsch_passive_decode.c:216-254`, `nr_dl_channel_estimation.c:58`) | `cudaHostRegister` of large pageable buffers (for the GB10 page-fault fix) | Useless on DISCRETE; pins hundreds of MB of host RAM | Pin only when DIRECT |

### 1.3 CUDA 12.4 compile breaks `[MEASURED, sens6 ninja log]`
- `nr_gpu_pass_graph.cu:1099`: `cudaGetDriverEntryPointByVersion` (12.5+).
- `:1130`, `:1138`: 7-arg `cudaStreamGetCaptureInfo`. In 12.3/12.4 this is `cudaStreamGetCaptureInfo_v3`; in 13.x it is the plain name.
- `:1150`: `cudaStreamUpdateCaptureDependencies` with edge data. In 12.3/12.4 this is `_v2`.
- `cuStreamGetCtx` with a green-context out-parameter is 12.5 (`cuStreamGetCtx_v2`). On 12.4 the 2-argument form must be used.
- `nr_gpu_green.h:42` already has `#if CUDART_VERSION >= 12050` (12.4 compiles it out, so green contexts are off).
- `libpdsch_gpu.so` is therefore missing on sens6. All its ctests are "Not Run" (test_nr_pdsch_gpu_ldpc*, test_nr_pusch_gpu_ldpc, test_nr_gpu_pass, test_nr_gpu_pass_discrete, test_nr_pusch_gpu_fep).

---

## 2. Design

### 2.1 Tier selection (one decision per device, at probe, logged)
New `nr_gpu_choose_model()` (pure, unit-tested on mocked caps), applied in `probe_locked()` (`nr_gpu_platform.c:132`):
1. `!available` (no runtime, no device, `cudaMemGetInfo` fails) or `!unified_addressing` -> **NR_GPU_NONE (CPU)**.
2. **SHARED candidate** iff `pageable_memory_access && (integrated || host_page_tables)`.
   - Integrated GPUs with pageable access (GB10) and ATS / C2C-coherent parts (`host_page_tables=1`, e.g. Grace-Hopper) qualify.
   - **HMM on PCIe** (`pageable=1, host_page_tables=0`, sens6) does **not** qualify: correct but every access is a PCIe transaction plus page migration, so it is not "fast".
   - An integrated GPU without pageable access (Jetson-class) does not qualify either. It runs DISCRETE: copies DRAM->DRAM, correct, cheaper than PCIe.
   - Today it is SHARED/platform + DISCRETE/buffers. Unifying removes that split; no production host is affected.
3. **Safety probe for a SHARED candidate.**
   - Run once, after `set_flags`, only when `pageable_memory_access==1`. That attribute guarantees a kernel read of `malloc` memory cannot fault.
   - Load a 20-line embedded PTX kernel through `libcuda` (`cuModuleLoadData`, `cuLaunchKernel` on the current primary context; `dlopen("libcuda.so.1")` as the name lookup already does at :156).
     This keeps `libnr_gpu_platform` free of any CUDA link.
   - The kernel reads a 64 KiB `malloc`'d host pattern, writes its checksum into the same buffer and writes a device-side flag.
   - Host checks the checksum. Timeout 1 s via `cuStreamQuery` polling.
   - Pass -> SHARED. Fail or timeout -> DISCRETE, with reason `probe_fail`.
   - Probe infrastructure unavailable (no `libcuda` symbol, JIT error) -> trust the properties (`probe=skipped`), so GB10 cannot regress on a driver quirk.
4. Otherwise, if `can_map_host_memory` -> **NR_GPU_DISCRETE**; else NONE.
5. **Log line.** Keep the existing fields and the `model=` values (`shared|discrete|unavailable`; parsers rely on them). Append `tier=shared|discrete|cpu reason=<integrated|ats|hmm_pcie|no_pageable|probe_fail|no_device|no_uva> probe=ok|fail|skipped`.
   Delete the "phase 2" warning. Add one info line for DISCRETE: `SENSING: GPU_PLATFORM DISCRETE: device buffers + pinned staging + explicit copies`.
6. **Test hook (no env in the library).**
   - `void nr_gpu_platform_test_force_model(nr_gpu_platform_model_t m)`, exported from `libnr_gpu_platform`, takes effect before or at the next probe; NONE forces the CPU tier.
   - `nr_gpu_mem_test_force_discrete()` becomes a thin wrapper that keeps its present semantics, so existing tests are unchanged.
   - Test binaries read their own env (`NR_GPU_TEST_MODEL=discrete|cpu`, the `NR_GPU_PASS_DISCRETE` pattern) and call the hook.
   - The receiver binary never reads it. Open question Q1 covers an rfsim override.

GB10 invariant: integrated=1, pageable=1, host tables=1 gives a SHARED candidate, the probe passes, and the result is SHARED.
Every later branch (`nr_gpu_model()==DIRECT`, `g_exact_direct`, storage kinds) is unchanged, so the code path is the same.
The only added work at start-up is one probe kernel (~ms).

### 2.2 Memory model per tier (allocation API is model-agnostic)
Callers already hold `(host, dev)` pairs: `nr_gpu_buf_t {dev, host, bytes, model}` and `nr_gpu_alloc()` + `nr_gpu_device_ptr()`. Keep both. Do not invent a third API.

| Kind / class | SHARED (unchanged) | DISCRETE | Why for DISCRETE |
|---|---|---|---|
| `NR_GPU_DEVICE` (work buffers, tables, HARQ store, graphs) | `cudaMalloc` | `cudaMalloc` | GPU-only data, highest bandwidth |
| `NR_GPU_STAGING` (copy source/target, parameter blocks) | mapped pinned (`cudaHostAlloc` portable\|mapped) | **same**: mapped pinned | DMA engine source/target. The mapping also allows zero-copy of tiny parameter blocks |
| `NR_GPU_SHARED_RW` (small results the host polls: LDPC bits/res, CB0 res, td_cb0 L/D, PDCCH LLR) | mapped pinned | **mapped pinned (zero-copy)** | Read or written once per launch and small (KB to a few MB): one PCIe pass, no extra copy node. Users already pair it with explicit copies where the volume is large |
| `nr_gpu_buf_t` (graph pass) | `host == dev` mapped | pinned host mirror + `cudaMalloc`; H2D/D2H captured in the graph on the instance stream | Large per-pass volumes (IQ window 1.05 MB, LLR up to 3 MB) must be DMA bursts, not zero-copy kernel reads |
| **new** `nr_gpu_buf_alloc_dev()` (device-only, no host mirror) | = `nr_gpu_buf_alloc` (mapped, so GB10 is unchanged) | `cudaMalloc` only, `host = NULL` | Internal intermediates never read by the host (`scratch`, `lin`, `segsc`): about 18 MB of pinned host RAM saved per instance, 7 instances |
| `nr_gpu_register_host` | mapped register | **allowed** (pinned, mapped) | Only for staging sources; never as a kernel-read path on DISCRETE |
| Caller pageable buffers read in place by kernels | DIRECT entries | **forbidden**: the entry refuses -> CPU (Task 4) | Would fault (non-HMM) or migrate pages (HMM) |

`nr_gpu_storage(DISCRETE, kind)` returns the same mapping as SHARED. The only differences live in `nr_gpu_buf_t` and in the
DIRECT-only entries, which are already keyed on `nr_gpu_model()`.

### 2.3 Readiness
- `nr_gpu_platform_ready()` = `available && model != NONE && unified_addressing && can_map_host_memory`.
- `nr_gpu_module_check()` is unchanged otherwise: it adds the arch check.
- Modules that need DIRECT for one entry refuse **that entry** (and log once). They do not refuse the whole module.
- `nr_gpu_platform_shared_direct()` stays the DIRECT predicate. It becomes `model==SHARED`, because SHARED now implies pageable direct access and the probe.

### 2.4 CUDA 12.3 .. 13.x compatibility (`openair1/PHY/TOOLS/nr_gpu_compat.h`, new, CUDA TUs only)
- `nr_drv_sym(name, ver)`: `CUDART_VERSION >= 12050` -> `cudaGetDriverEntryPointByVersion(name, &f, ver, cudaEnableDefault, &q)`.
  Otherwise `cudaGetDriverEntryPoint(name, &f, cudaEnableDefault, &q)`, which returns the ABI of the compile-time toolkit.
  Every caller passes the matching typedef. `NULL` on failure.
- `nr_capture_info(s, &st, &id, &g, &deps, &edges, &n)`: plain call on 13.x; `cudaStreamGetCaptureInfo_v3` on 12.3-12.9; `edges=NULL` + v2 call below 12.3.
- `nr_capture_set_deps(s, nodes, edges, n)`: plain call on 13.x; `cudaStreamUpdateCaptureDependencies_v2` on 12.3-12.9.
- `nr_stream_ctx(s, &ctx, &green)`: `cuStreamGetCtx_v2` on 12.5+; `cuStreamGetCtx` + `green=NULL` below 12.5.
- **Conditional nodes** (graph gate, Fix A+D) need 12.3 toolkit **and** driver. Below 12.3 compile, or when `gate_drv().ok==0` at run time:
  - `nr_gpu_pass_init` logs once `GPUPASS: graph conditional nodes unavailable (driver/toolkit) -- pass off` and returns -1.
  - The receiver then runs today's non-pass GPU/CPU path. There is no per-class capture failure storm (today it prints at `capture_gated` :1191 for every class).
- **Green contexts**: already compiled out below 12.5 and fall back at run time (`nr_gpu_green.h:63-67`).
  On GeForce with 13.x they may exist (Task 7 sizes them from the device).
- CMake (`cmake_targets/nr_gpu_arch.cmake`): requested architectures that nvcc does not support are **dropped with a WARNING** when at least one supported arch remains (today: FATAL_ERROR).
  The highest kept arch is always added as `-virtual` (PTX JIT forward-compat), which the function already does.
  `pdcch_nid_gpu` uses `LDPC_CUDA_ARCH` instead of `NR_GPU_MODULE_ARCHS` (CODING/CMakeLists.txt:192); align it.

### 2.5 Memory budget (sizes from `cudaMemGetInfo`, never from GB10's 128 GB)
`[ANALYTICAL, from the allocation sizes in code]`, 4 RX, 273 PRB:
- GPUPASS: 7 instances x ~30-38 MB (`est` 3.7, `scratch` 5.9, `llr` 3.0, `lin` ~4.2, `segsc` 8/16, `rxF` 0.9, `ulwin` 1.1, …) ≈ 0.21-0.27 GB.
- HARQ store 104 MB (`:1330`).
- Split graphs capped by `split_cap_auto` (`:1066`) = clamp(total/4, 2048, 3072) MB -> 2048 MB on 8 GB.
- Demod contexts ~10 MB each; LDPC contexts and pools ~50-100 MB; td_cb0 ~60 MB + 8x L buffer; cuFFT plans; CSI-RS discovery buffers (lazy `Buffer::reserve`); the CUDA context and modules (~0.3-0.5 GB).
- Total ≈ 3-3.5 GB device on an 8 GB card. **Fits**, with ~4 GB headroom.
- DISCRETE host pinned: mirrors of the `nr_gpu_buf_t` (~0.25 GB, about 0.12 GB after `buf_alloc_dev`) + module staging (~0.2 GB). Pinned memory is not limited by `ulimit -l`, but it is unswappable on a 14 GB host where the rfsim receiver RSS is already 6.5 GB `[MEASURED, sens6 106 PRB]`.

Degradation rule (Task 6):
- (a) The platform records `free_at_probe`.
- (b) `split_cap_auto()` = `min(clamp(total/4, 2048, 3072), free_at_probe * 3 / 10)`, floor 256 MB. On GB10 the first term wins, so behaviour is unchanged. On 8 GB: min(2048, ~2280) = 2048. On a 4 GB card: ~1100.
- (c) When the cap is hit, warming stops: this path exists (`split_cap_hit`) and logs. If the instances alone exceed the cap, split mode goes off (`:1340`).
- (d) Any module allocation failure returns "not ready" -> CPU (already true; Task 8 tests it with an OOM injection on DISCRETE).
- (e) A new info line `SENSING: GPU_MEM tier=… total=… free_at_probe=… pass_inst_mb=… graph_cap_mb=… pinned_host_mb=…` after warm-up.

---

## 3. Tasks (each sized for one Sonnet implementer; file:function; acceptance; parallelism)

Waves: **A** = T1, T2, T3, T4 in parallel (disjoint files). T3 and T4 **merge together** because T3 opens the gate and T4 guards the DIRECT-only entries.
**B** = T5 (after T1), T7 (after T1), T8 (after T2, T3). **C** = T6 (after T3, T5). **D** = T9 sens6, T10 DGX regression. **E** = T11 (only if T9 shows a PCIe-bound tail).
Every task: build on the DGX (13.0) **and** sens6 (12.4) once T1/T2 are in. On the DGX: full `ctest -j1`, with zero new failures vs the known list (PROJECT_MEMORY §25 2026-10-22 item 7).

**T0 (prerequisite, owned by another agent, not in this plan):** the x86 AVX2 32-byte alignment assert `oai_dfts.c:3441` that crashes 106-PRB rfsim on sens6 at ~10 s.
Its fix must land before T9's rfsim (273 PRB may hit the same buffer; unknown) `[HYPOTHESIS]`.

**T1 — CUDA compat shims** (`nr_gpu_compat.h` new; `nr_gpu_pass_graph.cu` `GateDrv`/`gate_drv` :1083-1104, `capture_gated` :1108-1158, `nr_gpu_pass_init` :1322; `nr_gpu_green.h` :61 -> `nr_drv_sym`).
- Use the four wrappers of §2.4. Version for the `cuGraphAddNode` lookup: 12030 on 12.x and 13000 on 13.x, matching the `CUgraphEdgeData` signature.
- Runtime "no conditional nodes" -> pass off once.
- Acceptance:
  - `libpdsch_gpu.so` compiles on sens6 (12.4.131) and DGX (13.0).
  - DGX: `test_nr_gpu_pass`, `test_nr_gpu_pass_discrete`, `test_nr_gpu_green` pass, with bit-identical LLR/bits/CRC to before (the existing tests compare to CPU references).
  - New unit `GpuPassCompat.NoGateDriverDisablesPassOnce` (inject `gate_drv().ok=0` through a test setter): `nr_gpu_pass_init` returns -1 and logs exactly one line.

**T2 — CMake arch handling** (`cmake_targets/nr_gpu_arch.cmake` `nr_gpu_architectures`; `openair1/PHY/CODING/CMakeLists.txt:192`).
- Drop unsupported archs with a WARNING; FATAL only if none remain.
- Acceptance: configure with the default `89;121` on nvcc 12.4 gives `89-real;89-virtual` and a warning; on 13.0 gives `89-real;121-real;121-virtual` (unchanged).
- Parallel with T1/T3/T4.

**T3 — Platform tiers** (`nr_gpu_platform.c` `nr_gpu_choose_model`, `probe_locked`, `nr_gpu_storage`, `nr_gpu_platform_ready`, `nr_gpu_register_host`, `nr_gpu_platform_shared_direct`; new `nr_gpu_platform_test_force_model`, `probe_shared_access()` (embedded PTX via libcuda), `nr_gpu_platform_free_at_probe()`; `nr_gpu_platform.h`; `nr_gpu_mem.c` `nr_gpu_model`, new `nr_gpu_buf_alloc_dev`; `nr_gpu_mem.h`; `openair1/PHY/TOOLS/tests/nr_gpu_platform_test.cc`).
- Implements §2.1-2.3.
- Update the mocked tests that encode "DISCRETE fails closed":
  - `NoDeviceAndDiscreteFailClosed` :69 -> device-less stays closed; DISCRETE is now ready.
  - `DiscreteWarnsOnceAcrossModules` :79 -> info line once.
  - `KindMappingAndCorrectFree` :102 -> DISCRETE mapping = SHARED mapping.
  - `HardwareCoherentDiscreteProbesAsShared` :86 -> stays SHARED (ATS).
  - New `HmmOnPcieIsDiscrete` (pageable=1, host tables=0, integrated=0 -> DISCRETE, `reason=hmm_pcie`).
  - New `IntegratedWithoutPageableIsDiscrete`.
  - New `ProbeFailFallsBackToDiscrete` and `ProbeSkippedTrustsProperties` (mock runtime: probe hook injectable under `NR_GPU_PLATFORM_TEST`).
  - New `ForceModelNoneIsCpuTier`.
- Acceptance:
  - `test_nr_gpu_platform` passes on DGX and sens6.
  - DGX `SENSING: GPU_PLATFORM … model=shared tier=shared reason=integrated probe=ok`.
  - sens6 `model=discrete tier=discrete reason=hmm_pcie probe=skipped` (the probe runs only for SHARED candidates).

**T4 — Guard the DIRECT-only entries; gate the ungated modules** (disjoint from T3):
- `nr_pdsch_gpu_demod.cu` `nr_pdsch_gpu_demod_run` :1588: `if (nr_gpu_model()!=NR_GPU_MODEL_DIRECT) return refusal` (new reason `RF_MODEL`, counted in the `refused[...]` census).
- `nr_pdsch_gpu_demod_host.c` `nr_pdsch_gpu_demod_warmup` :77: log once `GPU_EXACT_DEMOD standalone needs the SHARED tier -- pass/CPU demodulation` and leave `g_run=NULL` on DISCRETE.
- `nr_pdsch_gpu_exact.c` `pin_init` :283 / `nr_pdsch_gpu_host_pin` :294: no-op unless `nr_gpu_model()==DIRECT`.
- `nr_csirs_disc_gpu.cu` `accumulate_host` :514-517: treat `cudaMemoryTypeHost` as "in place" only when `nr_gpu_platform_shared_direct()`.
- `nr_csirs_disc_gpu.cu` `nr_csirs_disc_gpu_ops` :224, `nr_csirs_disc_gpu_sweep.cu` `nr_csirs_disc_gpu_sweep_entry` :567 / `_prepare` :584, `nr_pdcch_nid_gpu.cu` `nr_pdcch_nid_gpu_ops` :841: return NULL / -1 when `!nr_gpu_module_ready()`. The callers already have CPU paths (ops NULL); verify each caller.
- Acceptance:
  - DGX: unchanged counters on the 4-RX rfsim gate arm (`GPU_EXACT_DEMOD calls` > 0, `check_bad=0`).
  - Unit `test_nr_pdsch_gpu_demod`: forced DISCRETE -> `nr_pdsch_gpu_demod_run` refuses with `RF_MODEL` and no CUDA call is made. It must also no longer SEGFAULT when the module is not ready (sens6 today `[MEASURED]`): exit 77.

**T5 — GPUPASS DISCRETE parity fixes** (`nr_gpu_pass_graph.cu`, `nr_gpu_pass_dl.c`), after T1:
- (a) `nr_gpu_pass_dl.c:311-321`: the delay-table H2D runs on the legacy NULL stream, and pass streams are `cudaStreamNonBlocking` (no implicit sync). Add `nr_gpu_sync(NULL)` before `cfgf`.
- (b) **Rank > 1 DM-RS SNR on DISCRETE reads stale host memory.** `dsnr` reads `est` rows of layer `nl-1` (:1708), but the D2H copies only `nbRx*NSYM*N*4` = layer 0 (:772, :789).
  Fix: a small captured gather kernel. It writes the DM-RS-symbol rows of layer 0 (brfo :1720-1725) and layer `nl-1` (dsnr) into a compact `nr_gpu_buf_t estd` (≤ 2 x 4 x 4 x N c16 = 512 KB), D2H'd instead of `est`.
  Host readers switch to it on DISCRETE only. DIRECT keeps reading `I.est.host` (bit-identical GB10).
- (c) `ldpc_info` :444 (`li` LDPC iteration stats) and `qm_row` :1718 are DIRECT-only.
  Add D2H of the `RES_OFF..TS_OFF+…` tail of `I.bits` (≤ 160 x 24 B) and of the `qm_sym` row of `scratch` (≤ 16 KB, fixed size, captured), and drop the `DIRECT` condition.
- (d) `alloc_inst` :1254-1262: `scratch`, `lin`, `segsc` -> `nr_gpu_buf_alloc_dev` (T3).
- (e) Pass `model` already logged in `GPUPASS enabled` (`nr_gpu_pass_dl.c:405`); add the tier word.
- Acceptance:
  - New `GpuPassDl.DiscreteEqualsDirectFullResult` (DGX, in `test_nr_gpu_pass`). Same synthetic grants (nl 1..4, nseg 0/2, probe and real) under DIRECT then forced DISCRETE (separate processes via the existing ctest split, results serialized to a temp file).
    Bit-identical `nr_gpu_pass_result_t` (llr, tb bits, crc, nvar_ant, dsnr incl. layer nl-1, li, qm_row, rxF copy_mask rows).
  - `test_nr_gpu_pass_discrete` and `test_nr_gpu_pass` pass on DGX and sens6.

**T6 — Memory budget** (`nr_gpu_pass_graph.cu` `split_cap_auto` :1066 -> pure `nr_gpu_pass_cap_mb(total, free_at_probe)`; `nr_gpu_platform.c` `free_at_probe`; `nr_gpu_pass_dl.c` GPU_MEM line), after T3+T5.
- Acceptance:
  - Unit `GpuPassCap.Table`: (128 GB, 120 GB) -> 3072; (8 GB, 7.6 GB) -> 2048; (4 GB, 3.5 GB) -> 1050; (2 GB, 1.2 GB) -> 360.
  - DGX `GPUGRAPH` warm counts are unchanged vs before (same log numbers).
  - OOM injection test `GpuPassFence.OomFallsBackToCpu` (force `nr_gpu_buf_alloc` failure after N calls): init returns -1, the host logs "off", and decode is on the CPU path.

**T7 — Green-context sizing from the device** (`nr_gpu_green.h` `nr_gpu_green_stream`), after T1.
- Discovery SMs = round up `max(1, SMs/6)` to the split granularity (the driver's `cuDevSmResourceSplitByCount` rounds; read the result). GB10: 48/6 = 8, unchanged.
- The ≥32-RT-SM three-way split rule is unchanged.
- Acceptance: `test_nr_gpu_green` prints the same split on GB10. On sens6 with 12.4 it is skipped (77), as today. With a 13.x toolkit it reports a split or "unavailable" without error.

**T8 — Tier tests** (`openair1/PHY/CODING/CMakeLists.txt`, root `CMakeLists.txt` test blocks :2496, :2958, :3503, `openair1/PHY/MODULATION/tests/CMakeLists.txt:17`; the test sources), after T2+T3.
- Each GPU test binary calls `nr_gpu_platform_test_force_model()` from `NR_GPU_TEST_MODEL`.
- Add ctest variants:
  - `_discrete`: `test_ldpc_cuda_pool_discrete`, `test_ldpc_cb0_cuda_discrete`, `test_nr_td_cb0_batch_discrete`, `nr_polar_sc_cuda_test_discrete`, `test_nr_pdsch_gpu_ldpc_discrete`, `test_nr_pusch_gpu_ldpc_discrete`, `test_nr_pdsch_gpu_demod_discrete`.
  - `_cpu`: same names with `_cpu`; expect either exit 77 or the CPU-reference result, and **never** a crash.
  - Fix the two tests that crash when the module is not ready: `test_ldpc_cuda_pool` (SEGFAULT at `HealthyPathUsesCudaAndDecodes`) and `test_nr_pdsch_gpu_demod`.
  - `test_nr_gpu_pass_cpu` (pass init must return -1, no CUDA call).
- Bit-exact requirements:
  - `_discrete` outputs equal the CPU reference exactly where the base test requires it (LDPC vs `libldpc`, CB0 vs G1 pool, td_cb0 vs CPU dematch, polar vs CPU SC), and equal the DIRECT run.
- Acceptance:
  - DGX: all `_discrete` and `_cpu` variants pass (this is the **forced-discrete regression on the integrated GPU**).
  - sens6: the base names pass natively as DISCRETE. Base names that **currently fail on sens6** must pass: test_nr_gpu_pass, test_nr_gpu_pass_discrete, test_nr_pdsch_gpu_ldpc*, test_nr_pusch_gpu_ldpc, test_ldpc_cuda_pool, test_ldpc_cb0_cuda, test_nr_td_cb0_batch, nr_polar_sc_cuda_test, test_nr_pdsch_gpu_demod, test_nr_pusch_gpu_fep.
    `test_nr_pusch_gpu_fep` expects the documented "no coherent host memory access" skip (77) on DISCRETE.

**T9 — sens6 validation** (no code). Prerequisites: T0 fixed, T1-T8 merged, idle host, `pgrep -x nr-uesoftmodem` empty, frozen-gate check.
- Build: fresh dir, `cmake -G Ninja -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DOAI_USRP=OFF -DENABLE_TESTS=ON` (T2: default archs now work), then `ninja -k 0 nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests`.
- `ctest -j1` (GPU tests contend under `-j4`, §25 2026-10-22 item 7).
- rfsim 273 PRB (106 PRB never uses the GPU: FEP/pass need N=4096, `nr_gpu_pass_dl.c:284`, `nr_pdsch_gpu_exact.c:73`), using the scratch discovery commands (BUILD, GNBCONF/RXCONF/CELL/GNBARGS), each under `flock -x /tmp/td_measure.lock`, 420 s, 4 RX:
  - rank 1: `gnb.sa.rfsim.100mhz.conf`, `-M 273 -l 1`, `dgx/ue.passive.auto.100mhz.dgx.cfg`.
  - rank 4: `gnb.sa.rfsim.100mhz.rank4.conf`, `-m 25 -n 1 -M 273 -l 4`, `ue.passive.autor4.100mhz.conf`, `--ue-nb-ant-rx 4 --ue-nb-ant-tx 4`.
  - Two arms each, alternated: **GPU** (default) and **CPU** (`CUDA_VISIBLE_DEVICES=` empty -> no device -> CPU tier; a standard CUDA variable, not our config).
- Record:
  - `GPU_PLATFORM` tier line; `GPUPASS enabled … DISCRETE`; GPUPASS census (`fb[]`, `gate[]`, `shadow_bad`, `fp32_flip`).
  - `GPU_EXACT_* check_bad`; LDPC `decoder_used` split; `score_rx.py --json`; `goal_metrics.py` (tot_p50/p99/p999, crc_all, crc_q, drops); `GPU_MEM` line; RSS/CPU from time.txt; `nvidia-smi --query-gpu=memory.used,pcie.link.gen.current,pcie.link.width.current`.
- Pass criteria (provisional, sens6-only, never pooled with DGX):
  - `shadow_bad=0`, `check_bad=0`, no CUDA error.
  - GPU-arm CRC (post-conv, stratified by `decoder_used`) within 1 % absolute of the CPU arm.
  - Decode p99 no worse than the CPU arm.
  - Device memory < 50 % of total.
- Label `[MEASURED, sens6 x86_64 RTX 4060 Ti, rfsim 273 PRB 4 RX rank N, @<commit>]` / `[SIM VERIFIED]`.

**T10 — DGX regression of the GB10 path** (no code).
- Same build before/after the merge.
- `ctest -j1`, plus all `_discrete`/`_cpu` variants.
- rfsim 106-PRB 4-RX gate (CLAUDE.md thresholds).
- One 273-PRB 4-RX rank-4 arm with GPUPASS: `GPUPASS enabled … DIRECT`, census counts and CRC within run-to-run spread of the pre-merge arm.
- `tier=shared reason=integrated probe=ok`.
- Optional, user call: one OTA A/B night run.

**T11 — PCIe diet (conditional on T9 showing a PCIe-bound tail; measure first)** (`nr_gpu_pass_graph.cu`, `nr_pdsch_gpu_fep.cu`):
- (a) One H2D of the slot's IQ window per slot, shared by all instances (device slot ring keyed by slot index), instead of one per instance.
- (b) LLR D2H only for failed TBs or `llr_out_all`: host-issued after the verdict instead of captured. `[HYPOTHESIS]` memcpy nodes inside conditional bodies may need ≥ 12.4; prefer host-issued.
- (c) rxF D2H limited to `copy_mask` symbols via a gather into a compact buffer.
- Acceptance: sens6 p99 improvement with unchanged CRC/census; GB10 untouched (DIRECT branches only).

---

## 4. Bit-exactness contract
- SHARED vs DISCRETE: **bit-identical** outputs for identical inputs. The kernels are the same; only buffer placement and copies differ (T5/T8 tests).
- vs CPU: unchanged from today's tests:
  - exact FEP bit-exact; LDPC GPU vs `libldpc` per `test_nr_pdsch_gpu_ldpc`; CB0 = G1 pool algorithm.
  - Pass FP32 demod is sign/CRC shadow-checked, not bit-exact (existing contract, `nr_gpu_pass_dl.c:405`).
- CRC evidence (K34): the decoder set does not change, so `decoder_used` stays the stratum. **Tier is an additional stratum**: never pool DISCRETE (sens6) with SHARED (DGX) numbers. The `GPU_PLATFORM`/`GPUPASS enabled` lines carry it.
- Breakers and fallbacks unchanged: LDPC 200 ms timeout, 4-consecutive breaker, CB0 breaker, exact-FEP deadline -> CPU, pass `nr_gpu_pass_disable`. On DISCRETE more grants may be late -> CPU. These are counted, never silent.

## 5. Risks
1. **PCIe latency vs the DL p99 budget (goal < 3-5 ms)** `[ANALYTICAL]`.
   - Per 273-PRB 4-RX pass: H2D ≈ 1.05 MB IQ + params; D2H ≈ LLR ≤ 3 MB + est 0.9 + rxF 0.9 MB ≈ 5-6 MB.
   - RTX 4060 Ti link is PCIe 4.0 x8 (~12-13 GB/s effective) `[HYPOTHESIS: check nvidia-smi link fields in T9]`, so ≈ 0.45 ms per pass.
   - Several grants per slot can exceed link bandwidth (4 DL instances x 5 MB per 0.5 ms slot ≈ 40 GB/s demand). T11 addresses it; T9 measures first.
2. FP64 kernels on GeForce (FP64:FP32 = 1/64): the pass already defaults to FP32 demod. FP64 EVM / MMSE3 paths (`k_evm*<double>`, `GPU_EXACT_DEMOD_MMSE3`) are slow there. Not tuned per device here (GB10 must stay identical). T9 measures; Q3.
3. Pinned host memory on small-RAM hosts (sens6 14 GB, RSS 6.5 GB at 106 PRB): `buf_alloc_dev` and the GPU_MEM line bound it. A rank-4 273-PRB gNB+RX may swap. Watch RSS in T9.
4. HMM hosts: any remaining in-place kernel read of pageable memory would silently migrate pages (CPU stalls). Audit grep in T4: `j->rxdataF`, `rxdata[`, `cudaHostRegister`, `MemoryTypeHost`. Add a debug assert that `nr_gpu_model()==DIRECT` in every entry taking a raw host pointer.
5. Conditional-node driver support on GeForce + 12.4: runtime-probed (T1). If missing, the pass is off and decode falls to the standalone GPU entries + CPU, so not all DL/UL decode runs on GPU. Logged.
6. Green contexts absent with 12.4: the discovery kernels share SMs with RT work. The known 2026-10-21/22 discovery contention (c_sweep ~212 ms on air) can be worse on 34 SMs. Upgrading sens6 to CUDA 13 is a host decision (Q2).
7. The probe kernel itself on exotic drivers: guarded by `pageable_memory_access==1`, with a timeout and a "skipped" fallback.

## 6. Open questions for the user
- Q1: Allow a diagnostic override for full-receiver runs (e.g. `NR_GPU_FORCE_MODEL=discrete|cpu`, logged as FORCED), so the DGX can run rfsim/OTA on the DISCRETE path? Without it, DGX coverage of DISCRETE is unit/ctest only. sens6 CPU arms can use `CUDA_VISIBLE_DEVICES=`.
- Q2: Upgrade sens6 to a CUDA 13.x toolkit (driver 580 supports it)? That brings green contexts and the same toolkit as the DGX. The plan still keeps 12.4 building.
- Q3: Should numeric mode (FP64 vs FP32 EVM/MMSE3) follow `cudaDevAttrSingleToDoublePrecisionPerfRatio`? That would be a device-property decision, but it changes results on devices where it flips; GB10 would be checked to stay identical.
- Q4: Should the standalone DIRECT-only entries (chest, demod, UL FEP) get staged DISCRETE variants? Without them, grants that bypass the pass decode on the CPU on discrete hosts. Today's census says how many bypass; decide after T9.
- Q5: Should the deadlines (exact-FEP deadline, LDPC 200 ms) scale with the measured H2D/D2H time on DISCRETE, or stay fixed? Fixed is safest for comparability.

## 7. User decisions (2026-10-23)
- T0 (alignment crash): fixed in `9aef537ffb` (aligned DFT outputs in the DL traffic probe and SRS FEP; probe started once).
- Q1: YES. Add `NR_GPU_FORCE_MODEL=discrete|cpu`, diagnostic only, logged as FORCED in the GPU_PLATFORM line; unset = automatic tier selection (default behaviour unchanged).
- Q2: NO. sens6 stays on CUDA 12.4; the code must build and run on 12.4 .. 13.x.
- Q3: NO. Numeric mode is not chosen from the FP64:FP32 ratio (GB10 bit-identical).
- Q4: YES. Staged DISCRETE variants of the standalone chest, demod and UL FEP entries: new wave after T9, so all DL and UL decode can run on a discrete GPU.
- Q5: Deadlines stay FIXED; T9 measures the H2D/D2H copy time and the deadline hit rate on DISCRETE.
- Tier rule (user): SHARED when the device proves it (runtime check in code), else DISCRETE, else CPU; the CPU fallback stays in every module.
