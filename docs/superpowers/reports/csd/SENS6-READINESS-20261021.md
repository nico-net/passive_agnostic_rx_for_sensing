# SENS6-READINESS-20261021: read-only audit of csd/int16-grantsize for sens6 (x86, RTX 4060 Ti, discrete)

Branch csd/int16-grantsize @ 84d5e9e7c0 (= main a378f7dc5a + disc-nonblock + corepool + grant-sized GPU work). Nothing was built, run or measured for this audit; no hardware touched.

Evidence labels: [CODE-READ] read in this tree (file:line). [MEASURED, sens6 snapshot 2026-09-30] from `tests/passive_rx/sens6_host_snapshot_2026-09-30/` (read only, frozen). [MEASURED, DGX GB10 ...] / [Bench, cloud x86] copied from the named report, not re-measured. [INFERRED] reasoning or public-spec knowledge not in the repo. [UNVERIFIED] nothing in the repo settles it; the checklist in section 9 settles it.

## 0. sens6 facts used

| fact | value | label |
|---|---|---|
| CPU | Intel Core Ultra 5 235, 14 cpus, 1 thread/core, 1 NUMA node, AVX2+FMA+GFNI, no AVX-512 | [MEASURED, sens6 snapshot] env_sens6.txt |
| Kernel cmdline | `isolcpus=domain,managed_irq,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1,4-13 hugepages=2048` (4 GiB of hugepages reserved) | [MEASURED, sens6 snapshot] |
| `nproc` | 12 (cpus 2-3 are not in the default affinity mask; an explicit pin to them works) | [MEASURED, sens6 snapshot] via PROJECT_MEMORY 4.1 |
| RAM | 14 GiB total, 9 GiB available at snapshot time, 8 GiB swap | [MEASURED, sens6 snapshot] |
| GPU | GeForce RTX 4060 Ti, 8188 MiB, driver 580.178.04, discrete PCIe, sm_89; confirmed again 2026-10-17 | [MEASURED, sens6 snapshot] + PROJECT_MEMORY memory file |
| SM count | **not in the snapshot or the repo.** AD106 has 34 SMs (public spec) | [INFERRED] |
| nvcc | 12.4.131 (snapshot). Driver 580 supports CUDA 13.0, so a newer toolkit can be installed | [MEASURED, sens6 snapshot] |
| P/E core split | not in the snapshot; Arrow Lake enumerates 6 P cores first (0-5), then 8 E cores (6-13) | [INFERRED] |
| Radio | X410 serial 327C1F2 on a ConnectX-5 (enp2s0f1np1, MTU 9000, rings 8192), UHD 4.10 UDP transport (no `use_dpdk` in any sens6 launcher), data 192.168.20.2, mgmt 192.168.1.140; historical launcher pins `ISAC_UE_RT_CORE=2`, NIC IRQs on 8-13, `taskset -c 0-7`, `--ntn-initial-time-drift -4.25`, `--ue-rxgain 50` | [CODE-READ] snapshot launchers/run_sw.sh, host_network/ |
| Last sens6 GPU evidence | ctest 2026-09-28 on code 621bdc32a2, which predates every GPU module of this branch. No GPU code of this branch has ever run on sens6 | [MEASURED, sens6 snapshot] ctest_head_equivalent; SENS6-STAGE1-TASK9.md still pending |

DGX reference: GB10, sm_121, 48 SMs, unified memory, 20 cores (15-19 isolated, 17-19 DPDK) [MEASURED, DGX GB10, PROJECT_MEMORY 4.2].

## 1. GPU green contexts, SM partition and other SM constants

Current behaviour [CODE-READ]:
- `nr_gpu_green.h:44` `const int sms = 8` is hard-coded. `ISAC_GPU_DISC_SMS` and `ISAC_GPU_RT_ISOLATE` in FROZEN_20261018.env / POOL_20261021.env are **not read by any source** (no-ops); `ISAC_GPU_LDPC_SM`, `_NC_MIN_CB`, `_CL_MIN_CB` are frozen constants inside `nr_pdsch_gpu_ldpc.cu:617-620` (also no-ops).
- Split 1: `cuDevSmResourceSplitByCount` with min count 8 gives one discovery group (rounded up by the driver granularity) and the remainder as the real-time (RT) partition (`nr_gpu_green.h:56-72`). Any failure prints `[GPU] green contexts unavailable ... all streams use all SMs` (:73) and every stream is a plain priority stream.
- Split 2 only when the RT partition has >= 32 SMs (`nr_gpu_green.h:80`): DL real-grant context (4 groups + remainder, 32 SMs on GB10), UL context overlapping the DL SMs, probe context on one group. Below 32 the comment at :86 says it all: **one RT context, UL / DL / probes yield by stream priority only.**
- The whole body is under `#if CUDART_VERSION >= 12050` (:42). With a toolkit below 12.5 the function compiles to `return false`: no green context at all.
- Each translation unit (pdsch_gpu, pdcch_gpu, csirs_disc_gpu, csirs sweep, nid_gpu, ldpc) makes its own contexts over the same split (`nr_gpu_green.h` header comment).
- Pool sizes are compile-time: `NR_GPU_PASS_DL 4`, `UL 2`, `PROBE 1` (`nr_pdsch_gpu_fep.h:75-78`), each instance owns 2-3 streams (`nr_gpu_pass_graph.cu:1247-1253`).
- LDPC kernel family: `st = major < 9 ? 2 : 1` (`nr_pdsch_gpu_ldpc.cu:617`): sm_89 always uses the non-cluster (`_nc`) kernels, no thread-block-cluster launch is attempted. Tested on the DGX via `test_nr_pdsch_gpu_ldpc_nc` only (cluster capable GPU) [CODE-READ, docs CLUSTER-LDPC-20261017].
- `CUDA_DEVICE_MAX_CONNECTIONS=32` appears only in env files and replay scripts; **no source sets it** (grep over *.c, *.cu, *.h). A run with no env gets the CUDA default of 8 connections for 7 instances x 2-3 streams plus the LDPC / discovery streams.

What happens on sens6:
- 34 SMs [INFERRED]: discovery 8 SMs, RT partition 26 SMs, which is < 32, so the split-2 branch is skipped: no DL/UL SM separation, UL real work and probes share the 26 SMs with DL and rely on stream priority. The DGX bench that justified the layout (UL span 376 -> 288 us, P99-UL-20261019 DlSmShare) does not transfer. **Silently degraded, not a failure.**
- The driver granularity on sm_89 is not stated in the repo; if it is 2 (typical for Ampere/Ada) the groups are 8 / 26, if the call fails the all-SM fallback runs [UNVERIFIED]. Read the log line `[GPU] discovery on N SMs, real-time on the other SMs (M of K SMs)`.
- nvcc 12.4 (snapshot): green contexts compiled out (section 5 shows `nr_gpu_pass_graph.cu` does not compile at all on 12.4). **Build-time failure** if the toolkit was not upgraded.
- The pass kernels were sized as "56 FEP blocks fit one wave of the 32 DL SMs" (GRANT-SIZED-20261021 section 4). On 26 shared SMs that is more than one wave [INFERRED]; solo p50 will not match the DGX figures.
- `CUDA_DEVICE_MAX_CONNECTIONS` unset: possible false stream serialisation [INFERRED].

Proposed fix (design):
1. Derive the partition from the device: query SM count, take `disc = clamp(round(SM/6), 2*gran, 8)`, `rt = SM - disc`; split-2 condition `rt >= 4 * gran * (1 + 1 + 1)` or a ratio rule (DL gets >= 60 % of rt, UL context overlapping, probe = one granule), never a literal 32. Log the derived numbers once (the existing `[GPU]` lines).
2. Remove the dead env keys from the env files; keep one place that holds the ratios.
3. `setenv("CUDA_DEVICE_MAX_CONNECTIONS", "32", 0)` at the top of `main()` before the first CUDA call, so no env is needed.
4. Number of pool instances (DL 4 / UL 2 / probe 1) from the same SM and memory budget (section 3), not constants.
5. Add a unit test that feeds `nr_gpu_green_stream` a fake 34-SM device (the function calls driver entry points; wrap them behind a small table as `nr_gpu_platform_test.cc` already does for the platform).

## 2. Memory model: unified-memory assumptions and the DISCRETE paths

### 2.1 Gate: discrete devices are refused before the DISCRETE code is reached [CODE-READ]

- `nr_gpu_platform.c:54-58` `nr_gpu_choose_model`: SHARED if integrated or (pageableMemoryAccess && pageableMemoryAccessUsesHostPageTables), else DISCRETE. A PCIe RTX 4060 Ti is DISCRETE (HMM gives attribute 88 = 1 but 100 = 0 on PCIe).
- `nr_gpu_platform.c:171-172` logs `GPU_PLATFORM DISCRETE backend is not implemented (phase 2); GPU paths use CPU fallback`. `nr_gpu_platform_ready()` (:184-187) is true only for SHARED. `nr_gpu_storage()` (:64) returns UNAVAILABLE for every kind on DISCRETE. The unit test `DiscreteWarnsOnceAcrossModules` and `KindMappingAndCorrectFree` (nr_gpu_platform_test.cc:78-103) assert exactly this.
- Every CUDA module built with `nr_gpu_module.c` (`openair1/PHY/CODING/CMakeLists.txt:283-289`: pdcch_nid_gpu, ldpc_cuda, td_cb0_gpu, pdsch_gpu, pdcch_gpu, polar_sc_cuda, csirs_disc_gpu, nr_csirs_disc_gpu_sweep) gets `nr_gpu_module_ready() == 0`. Consumers that test it: `nr_pdsch_gpu_fep.cu:632` (`allocate_init`) and `:972` (`nr_gpu_fep_api()` returns NULL), `nr_pdcch_gpu_fep.cu:196,346`, `nr_ldpc_cuda.c:61`, `nrLDPC_coding_cuda_decoder.c:316`, `ldpc_decoder.cu:750`, `nr_polar_sc_cuda.cu:31,260`, `nr_td_cb0_batch.cu:84`, `nr_pusch_gpu_fep.cuh:132-137` (also demands `host_page_tables`: "no coherent host memory access -- CPU FEP").
- **The DL graph pass is only warmed if the exact FEP warm-up succeeded:** `nr_pdsch_passive_queue.c:2247-2250` `if (nr_pdsch_gpu_exact_warmup(ue)) nr_gpu_pass_warmup(ue);` and `nr_pdsch_gpu_exact.c:95` returns 0 when `api` is NULL or `api->init` fails (`GPU_EXACT_FEP requested but libpdsch_gpu.so is unavailable -- CPU FEP`). On a discrete device `api` is NULL (previous bullet). Therefore **on sens6 the pass, its DISCRETE branches (`nr_gpu_mem.c`, `nr_gpu_pass_graph.cu` copy-backs) and the grant-sized work of this branch are unreachable**: the receiver runs the CPU path for DL decode. [CODE-READ; the final proof is the log, checklist C4.]
- Modules NOT gated by `nr_gpu_module_ready()` and therefore live on sens6: CSI-RS discovery GPU (`nr_csirs_disc_gpu.cu`, loaded through `nr_csirs_disc_backend.c` with a breaker and CPU fallback, `ISAC_CSIRS_DISC_BACKEND` unset = auto) and PDCCH nID GPU (`nr_pdcch_nid_gpu.cu`, plain `cudaHostAlloc` pinned staging). These are the only GPU consumers that would run, and they have never run on a discrete device [UNVERIFIED].

### 2.2 Assumptions of host-visible memory, by place [CODE-READ]

| place | assumption | DISCRETE branch? | tested on a real discrete GPU? |
|---|---|---|---|
| `nr_gpu_mem.c` `nr_gpu_buf_t` | DIRECT = mapped pinned host, dev == host, copies are no-ops; DISCRETE = pinned host + `cudaMalloc` + explicit async copies | yes | no. Forced via `nr_gpu_mem_test_force_discrete(1)` on the GB10 only |
| DL graph pass (`nr_gpu_pass_graph.cu`) | host reads `I.est/rxF/llr/res` after the event; DISCRETE adds H2D of parameter blocks and the IQ window and D2H of result block, est, rxF, llr (lines 617, 727, 1730, 1859 region) | yes | forced-DISCRETE on GB10 (`test_nr_gpu_pass_discrete`, filter `GpuPassDl.*:GpuPassUl.*:GpuPassFence.*` only, CMake:165) [MEASURED, DGX GB10, GPUGRAPH-T1/T3] |
| `ulwin` PUSCH IQ window, `ULW_LEN` | DISCRETE copies 4 x 66368 c16 per UL pass | yes | forced only |
| standalone LDPC contexts (`nr_pdsch_gpu_ldpc.cu:724-727, 918-935`) | `cudaHostAllocMapped` + `cudaHostGetDevicePointer`: kernels read `in`, `snap`, `res`, `dj` through PCIe (zero copy) | no: works through UVA, slow | no |
| `nr_pdsch_gpu_fep.cu:1231-1233` `nr_pdsch_gpu_exact_pin` (`cudaHostRegister`, ISAC_PDSCH_GPU_HOST_PIN=1 in the env) and `:1174` (`!shared_direct` returns -1) | host pageable buffers read by kernels | refuses on discrete | no |
| `nr_csirs_disc_gpu.cu:230-237` `nr_csirs_disc_gpu_alloc_unified` | discovery ring and TD front-end buffers are `cudaHostAllocMapped\|Portable`; `accumulate_host` uses the pointer directly when its type is Host/Managed (:512-517) | "explicit-copy baseline" for ordinary malloc rings (:146-186, 517) | no |
| `ldpc_cb0.cu:516-521, 1126` | CB0 GPU backend has an EXPLICIT copy mode for discrete devices and refuses UNIFIED if the platform is not ready | yes | no (and gated by `nr_td_cb0_batch.cu:84`) |
| `nr_td_cb0_batch.h:23` | CB0 batch needs managed memory "elsewhere" | partly | no |
| `nr_pdcch_nid_gpu.cu:542-544` | pinned (non-mapped) staging + explicit copies | yes (natural) | no |

### 2.3 Staleness risks introduced by grant sizing and discovery chunking

- Grant-sized FEP (`k_fep_exact` skips symbols outside `copy_mask`): in DISCRETE the D2H copies the whole device `rxF` (14 symbols x nbRx, still unconditional, GRANT-SIZED-20261021 section 5) into the instance's pinned buffer, and the host copies **only** the `copy_mask` symbols into the caller's `rxdataF` (`nr_gpu_pass_graph.cu:1730-1733`, mask built at `nr_gpu_pass_dl.c:580-590`). Untransformed symbols in the device buffer are stale (a previous grant), but they are never copied to the caller. `nr_dmrs_ref_db` reads the last DM-RS symbol, which is in the mask (:1713). So no stale read inside the grant's window [CODE-READ]. The held-instance retry skips C only when its symbol set is inside `Inst::fep_run` (`nr_gpu_pass_graph.cu` Inst struct), same rule on both models.
- **Not tested in DISCRETE:** GRANT-SIZED-20261021 section 3 says explicitly that none of the new tests (`GpuPassSized.*`) were run with `NR_GPU_PASS_DISCRETE`, and `test_nr_gpu_pass_discrete` (CMake:165) does not include `GpuPassSized.*` or `GpuGraph*` in its filter. [CODE-READ]
- A CPU consumer added later that reads the thread's `rxdataF` outside the grant window would see stale data in both models (GRANT-SIZED section 6); `t_fep_cache` is not marked valid after a sized pass (`out->fep_mask != 0x3fff`).
- Discovery chunking (`nr_gpu_chunk.h`, `disc_chunks`): accumulators live in device memory, the stream is drained between chunks (`disc_wait`), so chunking has no host-visible state. `finish_host` and `energies` copy their inputs explicitly with `disc_copy` (D2H into static host arrays, nr_csirs_disc_gpu_cluster.cuh diff) and are therefore discrete-safe in form. One exposure: with the ring in mapped host memory the normalisation kernels read the ring through PCIe in 1 ms chunks, and each chunk boundary adds ~50-100 us (DISC-NONBLOCK section 3), so discovery throughput on sens6 is lower than the DGX [INFERRED].
- `lmem_prereserve` (`nr_csirs_disc_gpu.cu:657-668`): the driver grows the device local-memory reservation by the largest per-thread frame (~21 KB, `resident::segment`) times resident threads. On the GB10 this was ~2 GB [MEASURED, DGX GB10, comment in code]; on sm_89 34 SMs x 1536 threads x 21 KB = about 1.05 GiB [INFERRED].

### 2.4 Proposed fix (design)

1. Make the gate per module, not per platform: add `nr_gpu_module_ready()` accepting DISCRETE for modules that declare `NR_GPU_MODULE_DISCRETE_OK` (pass, exact FEP front end, nID, csirs, ldpc plugin in EXPLICIT mode). Implement `nr_gpu_storage(DISCRETE, kind)` as DEVICE -> `cudaMalloc`, STAGING/SHARED_RW -> pinned (non-mapped) host + the explicit copies the callers already have, not mapped host memory read by kernels.
2. Replace every `cudaHostAllocMapped` + `cudaHostGetDevicePointer` read by a kernel (standalone LDPC contexts, discovery ring, td front end) by `nr_gpu_buf_t` so the copy happens once on a copy stream.
3. In DISCRETE, make the rxF / est / llr D2H request-only (as the spec of 2026-10-16 section 10.4 already says) and size the IQ-window H2D and the rxF D2H by `copy_mask` (per-symbol-range memcpy nodes or `cudaGraphExecMemcpyNodeSetParams`); this is the item GRANT-SIZED section 5 lists as "probably a bigger item than the FEP block count" for the discrete GPU.
4. Test: run the full `test_nr_gpu_pass` (all suites incl. `GpuPassSized`, `GpuGraph*`) under `NR_GPU_PASS_DISCRETE=1` on the DGX as a CMake test, then on sens6 natively.

PCIe load estimate for the pass on sens6 [INFERRED from the buffer sizes in `alloc_inst`]: per DL grant roughly IQ window H2D 4 ant x (61440 + 4096) c16 = 1.0 MB, est D2H 3.5 MB, rxF D2H 0.9 MB, plus llr and result block: about 5-6 MB per grant. The 4060 Ti is PCIe 4.0 x8 (about 14 GB/s usable). At 1000-1500 grants/s that is 5-9 GB/s, i.e. 40-60 % of the link and ~0.4 ms of pure copy time per pass at the 217-PRB shape; the 500 us pass target cannot hold until item 3 is done.

## 3. GPU memory budget at 8 GB (8188 MiB)

Per-instance buffers (`alloc_inst`, `nr_gpu_pass_graph.cu:1238-1275`; computed from the constants: N = 4096, EXTMAX = 3296, LLR_CAP = 1.5 M, 160 code blocks): rxF 0.9 MiB, est 3.5, geo 0.35, scratch 5.6, llr 2.9, LDPC in 4.0, LDPC bits 2.6, pil 0.2, scr 0.2, segsc 16.0 (split mode, 1024 rows), ulwin 1.0, small blocks: about **37 MiB per instance**, 7 instances (4 DL + 2 UL + 1 probe) = **about 260 MiB of device memory, and in DISCRETE the same amount again as pinned host memory** (every `nr_gpu_buf_t` has both sides). [INFERRED, arithmetic on code constants]

| item | size | label |
|---|---|---|
| pass instances (7) | ~260 MiB device (+ ~260 MiB pinned host in DISCRETE) | [INFERRED] |
| DL HARQ soft store `g_hq` (2048 x 66 x 384 x 2 B) | 99 MiB | [CODE-READ] `nr_gpu_pass.h:311-312`, `nr_gpu_pass_graph.cu` init |
| split graphs (C/D/L executables, ~0.55 MB each) | capped by `split_cap_auto()` = a quarter of device memory clamped to [2048, 3072] MB, so **2048 MB on 8 GB** (`nr_gpu_pass_graph.cu:1066-1071`); derived from the device, good | [CODE-READ] |
| whole pass on the GB10 after P99-UL | 1924 MB (instances + graphs + 104 HQ + 190 LDPC contexts); report says on the 8 GB card "the same set would stop warming near the end" (unseen classes counted) | [MEASURED, DGX GB10, P99-UL-20261019 section 5] |
| device local-memory reservation (`lmem_prereserve`) | ~1.0-1.5 GiB | [INFERRED] (GB10 ~2 GB measured) |
| CUDA context, module code (7 modules), cuFFT plans (`nr_pdsch_gpu_fep.cu:654` plan N=4096 x nant x 14, discovery plans), driver | ~0.5-0.8 GiB | [INFERRED] |
| standalone LDPC contexts + UL LDPC pool (2 contexts) + discovery working sets + nID | a few hundred MiB | [UNVERIFIED] |
| display / other processes | 18 MiB idle at snapshot; the other user's docker/MPS on the DGX is not known on sens6 | [MEASURED, sens6 snapshot] |

Sum: about 4.5-5.5 GiB of 8 GiB, so it fits once the DISCRETE path exists, with limited headroom [INFERRED]. If the GB10's nvidia-smi figure (11.1 GiB per process with the pass off, T3 report) were VRAM it would not fit, but on a unified-memory device that figure includes pinned host buffers; the split between host and device must be re-measured on sens6 [UNVERIFIED].

**Host RAM is the tighter budget on sens6**: 14 GiB total, 4 GiB in hugepages (cmdline), 9 GiB available at snapshot time, `mlockall(MCL_CURRENT|MCL_FUTURE)` is on by default (`common/utils/system.c:395-409`, `ISAC_MLOCK` default 1). Pinned mirrors of the pass (~260 MiB), the CSI-RS FD rings + detector state ~957 MiB, TD pool ~1138 MiB (T8, [ANALYTICAL]), the USRP ring ~1 GB when `ISAC_USRP_RING_MS=500`, per-consumer IQ snapshot sets (8 x ring-sized, `nr_pdsch_passive_queue.c` RECOV), rfsim RSS 0.9-1.1 GB [MEASURED, DGX rfsim, PROJECT_MEMORY section 14]. The OTA launcher requires 48 GB free (`ota.sh:19`), meaningless on sens6; a host-derived guard is needed.

Proposed fix (design): size pool counts and the graph cap from `cudaMemGetInfo` at start-up (instances <= 10 % of free, graphs <= 25 %, floor 1 GB, ceiling 3 GB), print one `GPU_BUDGET device=... instances=... graphs=...` line, and make the warm order explicit (small classes first) so an 8 GB card drops the widest classes, never the common ones. Add the host-pinned total to that line.

## 4. CPU cores, pins and scheduling

### 4.1 What is hard-coded [CODE-READ]

| where | what | on sens6 (cpus 0-13, 2-3 isolated) |
|---|---|---|
| `nr_core_plan.h:67-72` `nr_env_core` defaults: PDSCH 6,7,8,9,3,4; PUSCH 8,9; GPU worker 10; UHD ctrl 15; USS 12; nID 13 | DGX site map baked in the binary | see 4.2 |
| `nr-uesoftmodem.c:293-295` (`--passive-rx`): `ISAC_UHD_CTRL_CORES=15`, `ISAC_FLOAT_CORES=10-14` | DGX numbers | float lane = 10-13 (the mask is intersected with *online*, not *allowed*, `nr_core_plan.c:366-374`); core 15 does not exist: `sched_setaffinity` fails, `UHD_CTRL_REPIN: 0 thread(s) moved` (`usrp_lib.cpp:928-975`); harmless in simulation and with a non-DPDK transport, [INFERRED] a floating FIFO busy-poller on air with DPDK |
| `nr_pdcch_nid_gpu.c:208` | nID worker pinned to core **13** by default; `if (!CPU_ISSET(core, &allowed)) rc = -1` -> `PDCCH_NID_GPU init failed; capture disabled` | works with 14 cpus; silently disabled if the process is started with a mask that excludes 13 (e.g. `taskset -c 0-7`) |
| `nr_pdcch_uss_tracker.c:360-368` | USS worker pinned to 12 | exists on sens6; it is in the float lane, but if 12 were a pool core it is a non-member and the pool strips it |
| `nr_pdsch_gpu_demod_host.c:738-740` | `ISAC_GPU_CHECK_CORES` default `10-14` | 14 absent, rest fine |
| `nr_bg_lane.c:18` `ISAC_BG_CORES` default `2,11,12,13,14` (lane is off: `nr_bg_lane_enter` is a no-op, :36) | dead code | none |
| `nr_dashboard.c:42` dashboard default core **2** (SCHED_IDLE, nice 19) | on sens6 core 2 is an isolated core, the one the historical launcher gave the RT thread | runs, but it breaks the isolation of the RT core |
| `nr-ue.c:2227-2252` RT slot thread: `ISAC_UE_RT_CORE`, default **unpinned** | not set by the env files; `starttest.sh` sets 5 (DGX) | unpinned = float lane 10-13, shared with IRQs (irqaffinity covers 4-13) -> RT drop risk on air |
| `usrp_lib.cpp:1088` ring producer: `ISAC_USRP_RING_CORE` (unset = unpinned), ring off unless `ISAC_USRP_RING_MS` set (default 0, `usrp_lib.cpp:988-998`); `starttest.sh` sets MS=500, CORE=16 | DGX | ring core 16 does not exist |
| `nr_pdsch_passive_queue.c:2330-2340`, `nr_pusch_passive_queue.c:741-750`, `ISAC_UL_SNAP_CORE` default 8 | consumers take `core` only if the pool is absent (`nr_pool_site() ? -1 : core`); the pool is always on | none |
| `--thread-pool 8,9,15`, `-O ...dgx.cfg` `scan_thread "2:32:6"` in starttest.sh / p99 replay | flags of the DGX launcher; core 15 and 8,9 meaningless / colliding on sens6 | not used by the rfsim scripts |
| FROZEN_20261018.env | `ISAC_PDSCH_CORES=7,3,4,0,1`, `ISAC_PUSCH_CORES=8,9`, `ISAC_UL_SNAP_CORE=8`, `ISAC_UHD_CTRL_CORES=15`, `ISAC_GPU_WORKER_CORE=10`, `ISAC_PDCCH_NID_GPU_CORE=13`, `ISAC_PDCCH_USS_CORE=12`, `ISAC_FLOAT_CORES=10-14`, `ISAC_GPU_CHECK_CORES=10-14`, `ISAC_DMRS_SCAN_CORE=-1`, `ISAC_CORE_PLAN=0` | `ISAC_CORE_PLAN` is read by no source (grep) | the three `PDSCH/PUSCH/SNAP` lines define the pool in the old A arm |
| POOL_20261021.env | FROZEN minus `ISAC_PDSCH_CORES`, `ISAC_PUSCH_CORES`, `ISAC_UL_SNAP_CORE`; all other pins stay (diff verified) | pool = built-in defaults |
| default.env (`ota/default.env`) | `PDSCH 7,8,9,3,4`, `PUSCH 6`, `DMRS_SCAN_CORE=2`, `ISAC_UHD_CTRL_CORES=15`, `GPU_WORKER 10`, `NID 13`, `USS 12` | `DMRS_SCAN_CORE=2` is sens6's isolated core |
| `starttest.sh`, `p99_replay_clean.sh` | `ISAC_CSIRS_DISC_CORE=11`, `DETECT_CORES=12,13,14` (read by no source except the recovery busy-list, `nr_pdsch_recovery.c:143-147`), `ISAC_UHD_CTRL_CORES=2,3,4` (overridden by the env file), `ISAC_USRP_RING_CORE=16`, `ISAC_UE_RT_CORE=5`, `HEAVY_CPUS=0-16` | DGX |
| `rfsim_arm.sh:19` | `nproc <= 5` -> unpinned scan override; the default RXCONF pins `scan_thread` core 5 | fine at 12 cpus (the pool ignores the cfg core) |

SCHED settings: UE RT thread `OAI_PRIORITY_RT_MAX` (FIFO), ring producer FIFO 95 (`ISAC_USRP_RING_PRIO`), PDSCH / PUSCH / scan consumers SCHED_OTHER under the pool (`prio` 0; FIFO 40/30/50 only without the pool), discovery / USS / nID workers SCHED_OTHER, dashboard SCHED_IDLE. Needs root or `rtprio` (sens6 has `@usrp rtprio 99`; receiver is run through sudo) [MEASURED, sens6 snapshot].

### 4.2 How the pool picks its cores today and what it gives on sens6 [CODE-READ]

`nr_core_pool.c:484-545` `site_init`: cores = union of `nr_env_core("ISAC_PDSCH_CORES")` and `("ISAC_PUSCH_CORES")` (defaults 6,7,8,9,3,4 and 8,9, i.e. **the DGX site map**), minus `ISAC_UE_RT_CORE`, `ISAC_USRP_RING_CORE`, `ISAC_UHD_CTRL_CORES` (default 15), minus cores not in the inherited affinity mask (`nr_core_inherited_mask`, saved before the float lane moved the main thread). Only if fewer than 3 survive, it falls back to "every allowed non-RT core minus one" (:505-514), not capacity-aware. Capacity comes from `cpu_capacity` only (:518); x86 does not expose it, so every core gets 0 and the "big cores to the urgent classes" rule of `pick_token` (:152-160) is inert [INFERRED]. (`nr_core_machine_read` in `nr_core_plan.c:218-249` already falls back to `cpuinfo_max_freq`; the pool does not use it.)

Result on sens6, default start without taskset (inherited mask {0,1,4-13}): core 3 dropped (log `CORE_POOL: core 3 not usable ... dropped`), **pool = 6,7,8,9,4, N = 5** [INFERRED from [MEASURED, sens6 snapshot] nproc=12]. Under the historical `taskset -c 0-7`: pool = 6,7,4, N = 3, and no fallback fires because n = 3 is not < 3. Pool cores 6-9 are E cores on a 6P+8E part [INFERRED] and 8-13 carry NIC IRQs in the historical sens6 setup [CODE-READ run_sw.sh].

Pool sizing evidence [Bench, cloud x86, CORE-POOL-20261020]: N = 2 saturates DL from offered/N = 0.5; N = 3 decode token wait p99 1-4 ms (target 0.2 ms); N = 6 is the smallest that holds the typical DGX load (3.4 cores offered at scale 1) and the 6.4-core peak needs N of about 10. The pool derives its caps from N only (class_ok: DL <= N-1, scan reserve 1, low <= N-2). N = 5 is therefore marginal, and on sens6 the DL decode is on the CPU (section 2.1), which offers more load than the DGX figure [INFERRED].

Threads outside the pool [CODE-READ]: UE RT slot thread, USRP ring producer, UHD ctrl threads, GPU pass poll waits (the token is held across GPU waits, CORE-POOL section 3), float lane (GPU worker, csirsDisc, detect, USS, nID worker, pdcchJanon, dmrsScan, pdcchDisc, pdschRecov, ulBgDisc, td_cb0_pool, nid_solver, gpuCheckBg, OAI default 8-thread pool `-1,...`), dashboard, main thread, ITTI tasks.

Cores left for the pool on sens6 if sized from the host: 14 online - 2 isolated (RT thread + ring producer) = 12; a float lane of 3 leaves a pool of 9 [INFERRED plan below]. With a DPDK transport the lcores (dpdk_corelist) and the uhd_ctrl pollers would also come out of the 12.

### 4.3 Proposed host-derived core plan (design, no fixed numbers)

The pieces exist but are unused: `nr_core_machine_read()` (online, allowed, isolated, nohz_full, IRQ mask, capacity with a frequency fallback, DPDK lcores from `uhd.conf` when `use_dpdk=1`) and `nr_core_plan_build()` (scoring by capacity / irq / isolation), covered by `test_nr_core_plan`, but nothing in the executables calls them. Proposal: one `nr_core_site_plan()` called first in `main()` (where `nr_core_float_apply()` is called now), with these rules:

1. Candidate cores C = online and (allowed or isolated) minus DPDK lcores minus SMT siblings of those. Capacity = `cpu_capacity`, else `cpuinfo_max_freq`, bucketed at 10 % (the builder's rule).
2. RT slot thread: the highest-capacity core among isolated or nohz_full cores that carry no IRQ (else the highest-capacity core without IRQ affinity). Ring producer (only when the ring is on): the next one. UHD control: only if `use_dpdk=1` in the usrp args, k = number of control endpoints (the existing repin counting), taken from the same isolated set. On sens6: RT = 2, ring = 3, UHD none (UDP).
3. Float lane: `F = clamp(ceil(0.25 x (|C| - reserved)), 2, 4)` cores, the lowest-capacity cores left (E cores, IRQ cores). All "site" pins (GPU worker, csirsDisc, detect, USS, nID, dmrsScan, janon, dashboard, check workers, BG lane) become *float lane, unpinned*; the numeric env keys are dropped (kept only as explicit overrides).
4. Pool: everything left, ordered by capacity, `N_pool = min(left, 8)` as the default ceiling (the bench shows no harm from more, no data above 6). If `N_pool < 6`: log `CORE_POOL undersized`, shrink the float lane to 1, then lend one isolated core, then continue. The pool list is passed to `nr_pool_create()` with the real capacities.
5. Export with `setenv(..., 0)` the legacy keys that existing code reads (`ISAC_UE_RT_CORE`, `ISAC_USRP_RING_CORE`, `ISAC_UHD_CTRL_CORES`, `ISAC_FLOAT_CORES`) so no consumer needs to change, and delete the DGX literals in `nr_env_core`, `nr-uesoftmodem.c:293-295`, `nr_pdcch_nid_gpu.c:208`, `nr_pdsch_gpu_demod_host.c:740`, `nr_bg_lane.c:18`, `nr_dashboard.c:42`.
6. Log one line `CORE_PLAN host=... rt=... ring=... uhd=... float=... pool=... (capacity source)`; a run launched under `taskset` uses the *inherited* mask as C, and the plan warns when that makes N_pool < 6 (today it silently gives N = 3).
7. Expected result on sens6: RT 2, ring 3, float {11,12,13}, pool {0,1,4,5,6,7,8,9,10} (N = 9, P cores first by capacity). Expected on the DGX: the same rules give RT/ring on isolated 15/16 (DPDK 17-19 excluded), pool capped at 8 from the big cores; this changes the DGX map (pool 6 -> up to 8), so it must be A/B'd on the DGX before merge [UNVERIFIED].

## 5. x86 build

- Known link failures, not x86 specific: `nr_pusch_ra0sim`, `test_nr_ssb_rate_match_prod`, `test_nr_csi_rb_extent` (undefined `nr_srs_passive_enabled`, `nr_pusch_grant_book_*`; GPUGRAPH-DMRS2-20261017 item 5; seen on the DGX and on cloud x86 in every report). `ninja ... tests` stops at the first failure: use `ninja -k 0` [MEASURED, DGX GB10 and Bench, cloud x86 reports]. ra0 ctests (9) do not run.
- CUDA architectures: `CMakeLists.txt:10-15` default `89;121`; `nr_gpu_arch.cmake` queries `nvcc --list-gpu-code` and **FATAL_ERRORs at configure** if an entry is unsupported. On nvcc 12.4 (snapshot) `sm_121` is unsupported, so a default configure fails; `-DLDPC_CUDA_ARCH=89` is required (CLAUDE.md/PROJECT_MEMORY say so; "one config" does not allow it). The list is normalised to `89-real;89-virtual` [CODE-READ].
- **nvcc 12.4 cannot compile `nr_gpu_pass_graph.cu`**: `cudaGetDriverEntryPointByVersion` (CUDA 12.5 API) is called unguarded at `nr_gpu_pass_graph.cu:1099`; `nr_gpu_green.h` guards the same call with `CUDART_VERSION >= 12050` (:42). Build failure of `pdsch_gpu` unless the toolkit is >= 12.5 (the 13.0 toolkit matches driver 580). [CODE-READ; snapshot toolkit may have been upgraded since: UNVERIFIED]
- CPU code: the DGX cache has `AVX2=OFF AVX512=OFF GFNI=OFF` (SIMDe emulation of x86 intrinsics on NEON, `build_t26/CMakeCache.txt`); on x86 `CMakeLists.txt:165-200` auto-detects AVX2 / GFNI from `/proc/cpuinfo`, uses `-march=native -mno-avx512f`. sens6 cache in the snapshot: `AVX2=ON GFNI=ON AVX512=OFF` [MEASURED, sens6 snapshot]. Native x86 code is the more common path in OAI, no fallback is missing.
- aarch64-only code and fallbacks [CODE-READ]: `nr_spin_handoff.h:155-159` has `yield` for aarch64 and `pause` for x86_64; `nr_csirs_disc_confirm_internal.h:220-224` and `nr_csirs_disc_td.c:291-293` set `rotation_mode` 2/1 on aarch64 and 0 ("SSE truncation", `nr_csirs_disc.h:184`) elsewhere: **the discovery front end is not bit-identical between ARM and x86** (parity tests compare GPU against CPU on the same host; results across hosts must not be pooled, which the project rule already says). NEON polar (`nr_polar_fssc.c:2` includes `simde/arm/neon.h`) compiles on x86 through SIMDe emulation; functional, speed on x86 unknown [UNVERIFIED]. No SVE intrinsics are used in the receiver (grep). `nr_scrambling.c`, `nr_compute_llr.c`, `nr_phy_common.c`, `ldpc_encode_parity_check.c` have `__aarch64__` branches with generic or SIMDe alternatives.
- Build recipe in CLAUDE.md for x86 omits `oai_usrpdevif` and sets `-DOAI_USRP=OFF` (cloud). On sens6 `OAI_USRP=ON` with UHD 4.10 in /usr/local is the historical cache [MEASURED, sens6 snapshot]; gcc 15.2 / cmake 4.2.3 are newer than anything this tree was built with [UNVERIFIED].
- `ENABLE_LDPC_CUDA=OFF` builds a CPU-only receiver and sidesteps every CUDA item; the discovery GPU backend then falls back to CPU (auto).

## 6. Radio and front end on sens6

- Radio: X410 327C1F2 over UHD UDP on a ConnectX-5, MTU 9000, rings 8192, sysctls and IRQ affinity from `launchers/run_sw.sh` / `host_network/` [CODE-READ snapshot]; no DPDK. Available for OTA again is not stated; "No OTA from sens6" (SENS6-STAGE1-TASK9.md, user 2026-10-15) applies to the Stage-1 task, not to the later goals.
- DGX/X410-34B2AF1/DPDK-specific things in the default path: `nr-uesoftmodem.c:293-294` `ISAC_UHD_CTRL_CORES=15`, `ISAC_USRP_START_TIMEOUT_MS=2200` (measured with DPDK), `ISAC_USRP_CATCHUP_X=1.05`, `ISAC_RX_RECOVER=1`, `ISAC_USRP_RING_ZEROCOPY=1` (needs the ring); the ring producer / `uhd_ctrl` repin machinery (`usrp_lib.cpp:916-975, 1088-1130`) assumes DPDK busy-pollers; `nr_core_machine_read` only reads dpdk lcores when `use_dpdk=1` is in the args.
- Per-unit / per-site values passed as flags, which "one config, no flags" has to replace: `--ntn-initial-time-drift` (DGX unit 0, sens6 unit -4.25 [CODE-READ run_sw.sh; PROJECT_MEMORY line 315 says the two units differ]), `--ue-rxgain` (38 DGX, 50 sens6), `--initial-fo`, `--ssb`, `-C`, `-r`, `--band`, `--numerology`, `--ue-nb-ant-rx 4`, `--usrp-args` (addresses 192.168.10.2 / 192.168.30.2 DGX versus 192.168.20.2 / 192.168.1.140 sens6, in `ota.sh`/`starttest.sh`, not in the receiver), `-O` cfg with `rx_id = "rx_x410_dgx"` and `scan_thread "2:32:6"`.
- Proposed: a host profile resolved from the hostname or from `uhd_find_devices` (addresses, transport, rx gain default, drift estimate) in a file outside the frozen paths, selected automatically, plus an estimator for sample-clock drift at start (the DGX unit measures about 0, the sens6 unit does not) instead of a flag.

## 7. Dashboard / monitor and orchestration scripts

- Dashboard (`nr_dashboard.c`): started always on in passive mode, detached non-root, `python3` from `/usr/bin/python3` or `/usr/local/bin/python3`, `monitor.py` found above the executable, port 8080, **core 2 default** (:42), SCHED_IDLE, RLIMIT_AS 4 GB. Only the core is host-bound; its failure never affects the receiver. On sens6 core 2 is the isolated RT core: put it on the float lane (design 4.3) or pass `.core = -1` meaning "any float core".
- Scripts with DGX paths, all outside the frozen set: `/home/nicola/NICOLA/wt/rr-orchestration/...` in about 20 `tests/passive_rx/*replay*.sh`, `*_ab.sh`, `dtq_arm.sh`, `p99_replay*.sh`; raw captures `/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b` and `raw_pci923_tim_12s_tile3` (not on sens6; they are the only way to exercise 217-PRB GPU paths offline); `heavy.sh` (`HEAVY_CPUS=0-16`, default `taskset 0-4,8-14` includes sens6's isolated cores, 48 GB free gate, thermal gate); `ota.sh` (DGX addresses, Resource Guardian, `rtemap` hugepage cleanup, 48 GB gate, `ollama`); `p99_replay_clean.sh` (`ISAC_UE_RT_CORE=5`, `--thread-pool 8,9,15`, `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps`); CMake test properties (`openair1/PHY/CODING/CMakeLists.txt:166`) set `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps` (a user-specific path that may not exist on sens6, [UNVERIFIED] whether CUDA tolerates that).
- Simulation scripts that are usable as they are: `tests/passive_rx/dgx/rfsim_arm.sh` and `rfsim_regress.sh` (repo-relative paths, `gawk` with the `time` extension, `/usr/bin/time`, V4 shim auto-detect). **Their gate thresholds are DGX calibrations** (ttc tda0 <= 39 s, tda2 <= 77 s, CRC floor 93.4, drop_full <= 2.5 %); sens6 numbers must be labelled `[MEASURED, sens6 ...]` and not compared as pass/fail without recalibration (CLAUDE.md rule: never merge results across hosts).
- **The default rfsim gate uses 106 PRB: N_FFT = 2048 at 61.44 MS/s. The GPU pass and the exact FEP need N = 4096** (`nr_gpu_pass_dl.c:284`, `nr_pdsch_gpu_exact.c:73`: "needs N=4096 ... CPU FEP"). A 106-PRB sens6 simulation therefore exercises no GPU decode code on any host. GPU paths need `-r 217` / `-r 273`: `GNBCONF=tests/passive_rx/gnb.sa.rfsim.100mhz.conf RXCONF=tests/passive_rx/dgx/ue.passive.auto.100mhz.dgx.cfg CELL="-C 3750000000 -r 273 --ssb 1478"` (PROJECT_MEMORY section 14.1 measured 273 PRB rank 1 on the DGX with these) or raw-IQ replay.

## 8. Prioritised list

### A. Blockers for a sens6 simulation run

1. **Build configuration**: configure fails with the default `89;121` on nvcc 12.4; `nr_gpu_pass_graph.cu` needs a >= 12.5 toolkit (`cudaGetDriverEntryPointByVersion`, :1099). Needs `-DLDPC_CUDA_ARCH=89` and `nvcc --version` >= 12.5, or `-DENABLE_LDPC_CUDA=OFF` for a CPU-only simulation. Use `ninja -k 0` (3 known link failures).
2. **A discrete GPU is refused at the platform gate** (`nr_gpu_platform.c:171-187`, section 2.1): every GPU decode module reports not ready and the DL pass is never warmed, so a sens6 simulation runs the CPU decode path and cannot test any GPU item of this branch (grant-sized work, DISCRETE copies, green contexts). Not a crash; the logs say `DISCRETE backend is not implemented` and `GPU_EXACT_FEP requested but libpdsch_gpu.so is unavailable -- CPU FEP`. If the sens6 test is meant to validate the GPU pass, this is THE blocker.
3. **The default 106-PRB rfsim bed uses no GPU path at all** (N = 2048). Use 217/273 PRB (section 7) for anything GPU; DGX gate thresholds do not apply.
4. Launching under `taskset` or a cgroup shrinks the pool silently (`taskset -c 0-7` gives N = 3) and can disable the nID GPU worker (core 13 hard default). Run the simulation without taskset, or fix section 4.3 first.

### B. Blockers for sens6 on air

5. All of A.2 (DISCRETE gate) and the PCIe cost of the unconditional est / rxF D2H and the whole-slot IQ H2D (section 2.4 item 3).
6. Core plan: RT thread unpinned in a float lane that shares cores with NIC IRQs; ring producer unpinned; `ISAC_UHD_CTRL_CORES=15` and ring core 16 nonexistent; dashboard on the isolated core 2 (sections 4.1, 7). The fixed numbers of the DGX cannot be reused; the host-derived plan of 4.3 is needed.
7. Per-unit radio values passed as flags (drift, gain, addresses, transport; section 6) conflict with "no flags"; the sens6 unit's drift (-4.25 ppm historical) is not 0.
8. Green-context split below 32 RT SMs degrades to one context without the DL/UL separation measured on the DGX; DL p99 on sens6 is unmeasured (section 1).
9. Host RAM: 14 GiB, 4 GiB hugepages, `mlockall` on, pinned mirrors in DISCRETE; no sens6 RSS figure exists for the GPU receiver (section 3).

### C. Nice to have

10. Derive pool instance counts and the graph cap from SM count and `cudaMemGetInfo` (the cap already follows the device memory).
11. `setenv("CUDA_DEVICE_MAX_CONNECTIONS", "32", 0)` in `main()`.
12. Remove dead env keys and dead code (`ISAC_CORE_PLAN`, `ISAC_GPU_DISC_SMS`, `ISAC_GPU_RT_ISOLATE`, `ISAC_GPU_LDPC_*`, `ISAC_BG_CORES`, `DETECT_CORES`) so the env files stop implying behaviour.
13. Pool capacity source on x86 (`cpuinfo_max_freq` as `nr_core_machine_read` does) so big cores go to urgent classes.
14. Add `GpuPassSized.*` and `GpuGraph*` to the DISCRETE ctest; add a fake-device test for the SM split.
15. Replace `/home/nicola/...` literals in test scripts by `$ORCH`/`$CAPDIR` variables with defaults.
16. Recalibrate the rfsim gate thresholds per host (`GATE_*` overrides exist) and label sens6 results.

## 9. Checklist for the sens6-side agent (simulation only, no radio)

Run from the repo root on sens6; copy the logs next to the report; label results `[MEASURED, sens6 <date> <commit>]`. Do not edit frozen paths.

C0. Host facts. `nproc; lscpu -e=CPU,CORE,MAXMHZ,ONLINE | head -20; cat /sys/devices/system/cpu/isolated /sys/devices/system/cpu/nohz_full; ls /sys/devices/system/cpu/cpu0/cpu_capacity; nvidia-smi --query-gpu=name,compute_cap,memory.total,pcie.link.gen.current,pcie.link.width.current --format=csv; nvcc --version | tail -2; free -g; grep -i huge /proc/meminfo | head -3`. Expected: nproc 12, isolated `2-3`, compute_cap 8.9, 8188 MiB, `cpu_capacity` missing (confirms the pool's capacity is 0), P cores = the highest MAXMHZ (cpus 0-5 expected).
C1. Configure and build. `cmake -S . -B cmake_targets/ran_build/build_s6 -G Ninja -DENABLE_TESTS=ON -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DOAI_USRP=OFF` (no arch) then again with `-DLDPC_CUDA_ARCH=89`. Expected: the first fails with `GPU architecture 121 is unsupported by ...nvcc` on nvcc < 12.9; the second prints `GPU modules: CUDA architectures 89-real;89-virtual`. Then `ninja -k 0 nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests`. Expected: `nr_pusch_ra0sim`, `test_nr_ssb_rate_match_prod`, `test_nr_csi_rb_extent` fail to link, nothing else; with nvcc 12.4 additionally `nr_gpu_pass_graph.cu` fails on `cudaGetDriverEntryPointByVersion` (confirms section 5). Never build while a receiver runs (`pgrep -x nr-uesoftmodem`).
C2. GPU unit tests (no receiver). `ctest -R "gpu_green|gpu_platform|gpu_pass|pdsch_gpu_ldpc" --output-on-failure`. Expected on sens6: `test_nr_gpu_platform` passes (mocked); `test_nr_gpu_pass` either SKIPs (return 77) or runs with `model DISCRETE`. Record which; a pass in DISCRETE means the pass is fine once the gate is opened, a skip confirms finding 2.1. Also run once by hand `./test_nr_gpu_pass --gtest_filter='GpuPassSized.*:GpuGraph*'` natively and record (never tested in DISCRETE).
C3. Run the 273-PRB rfsim arm (GPU-capable shape) without taskset: `GNBCONF=tests/passive_rx/gnb.sa.rfsim.100mhz.conf RXCONF=tests/passive_rx/dgx/ue.passive.auto.100mhz.dgx.cfg CELL="-C 3750000000 -r 273 --ssb 1478" RXEXTRA="--ue-nb-ant-rx 4" tests/passive_rx/dgx/rfsim_arm.sh /tmp/s6_273 180` (flock `/tmp/td_measure.lock` for timing runs; `LD_LIBRARY_PATH` or cwd = the build dir so `libpdsch_gpu.so` is found). Then grep `/tmp/s6_273/rx/rx.log`:
 - `grep -a "GPU_PLATFORM" ...` Expected: `device=0 name="NVIDIA GeForce RTX 4060 Ti" cc=8.9 integrated=0 ... model=discrete` and the warning `DISCRETE backend is not implemented (phase 2); GPU paths use CPU fallback`.
 - `grep -a "GPU_EXACT_FEP\|GPUPASS" ...` Expected: `GPU_EXACT_FEP requested but libpdsch_gpu.so is unavailable -- CPU FEP`; **no** `GPUPASS enabled` line (confirms 2.1).
 - `grep -a "\[GPU\]" ...` Expected: `[GPU] discovery on N SMs, real-time on the other SMs (M of K SMs)` with K = 34 (record N, M: granularity); **no** `real-time SMs split` line when M < 32; `green contexts unavailable` if the toolkit/driver refuses; `lmem prereserve ... bytes`.
 - `grep -a "GPU LDPC\|GPU_LDPC" ...` Expected: `non-cluster kernels (sm_89)` if the LDPC GPU context is created.
C4. Core plan facts (same log). `grep -a "CORE_POOL\|FLOAT_CORES\|PDCCH_NID_GPU\|UHD_CTRL_REPIN\|DASHBOARD\|dashboard" rx.log`. Expected: `CORE_POOL: core 3 not usable ... dropped`, `CORE_POOL: 5 cores: 6 7 8 9 4`, `FLOAT_CORES=10-14 (4 CPUs)`, `PDCCH_NID_GPU ready: core=13` (or `init failed` when taskset excludes 13). Repeat the 106-PRB `rfsim_arm.sh` under `taskset -c 0-7` once to confirm the silent shrink: expected `CORE_POOL: 3 cores: 6 7 4`.
C5. Thread placement. While the arm runs: `ps -eLo tid,psr,cls,rtprio,pri,ni,comm | sort -k2n | head -80`. Expected: the dashboard python (`SCHED_IDLE`, class `IDL`) on cpu 2 (isolated); UE RT thread unpinned (psr in 10-13); pool consumers on 6-9,4 only.
C6. GPU memory and host RAM. `nvidia-smi --query-compute-apps=pid,used_memory --format=csv` every 10 s, and `grep -E "VmRSS|VmLck|VmHWM" /proc/$(pgrep -x nr-uesoftmodem)/status` at the end of the 273-PRB arm. Record separately: device MiB (expected < 2 GiB with only discovery + nID live, no pass) and RSS/VmLck (watch against 9 GiB available). This is the baseline for section 3; the pass-on figure only exists after the gate is opened.
C7. CPU-only baseline and gate. `tests/passive_rx/dgx/rfsim_regress.sh 2` (106 PRB, 4 RX, 420 s) under the exclusive lock. Record ttc, CRC, drop_full, CPU cores, RSS as `[MEASURED, sens6]`; do not judge against the DGX thresholds. Also run once with `ENABLE_LDPC_CUDA=OFF` build to separate CUDA effects.
C8. Optional, only when A.2 is fixed: repeat C3 and expect `GPU_PLATFORM ... model=discrete` followed by `GPUPASS enabled: ... DISCRETE memory model, demod FP32`, a `GPUGRAPH split ... mb=` line (memory cap 2048 on 8 GB, `memory cap ... reached` is allowed, count `unseen`), `nvidia-smi` used memory < 7 GiB, and a PCIe rate: `nvidia-smi dmon -s t -d 1` (rx/tx MB/s) versus the 5-6 MB/grant estimate of section 2.4.

## 10. What this audit did not do

Nothing was run, so every sens6 behaviour above is derived from code and the 2026-09-30 snapshot. Not verified: the SM count and the driver's split granularity on sm_89, the sens6 toolkit version today, CUDA's behaviour with a non-existent `CUDA_MPS_PIPE_DIRECTORY`, the P/E core numbering, the x86 speed of the SIMDe NEON polar decoder, whether a UDP-transport UHD still creates `uhd_ctrl` busy-poll threads, and every memory figure marked [INFERRED]. The GPU graph pass was read only down to the allocation, copy and gating level, not kernel by kernel.
