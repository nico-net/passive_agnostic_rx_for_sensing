# Task 2 report: launch-only DL _dev entry points (commit 5ec3a4a871, branch csd/gpupass)

Status: DONE_WITH_CONCERNS

## Implementation (all in libpdsch_gpu.so; old entry points unchanged except the demod tail launch extracted to a shared static)
- `nr_pdsch_gpu_fep_dev(const nr_gpu_fep_job_t*, nr_gpu_buf_t *rxF, void *stream)` (fep.cu): output buffer model picks the path. DIRECT: k_fep_exact reads the host IQ ring in place, writes rxF->dev. DISCRETE: window memcpy into one of 4 pinned slots + cudaMemcpyAsync H2D into the matching device ring slot (4 slots allocated in allocate_init, before the fence), kernel reads the device slot. Layout rxF = [ant][14][4096] c16. New `nr_gpu_fep_job_t` in nr_pdsch_gpu_fep.h.
- `nr_pdsch_gpu_chest_dev(const nr_gpu_chest_job_t*, int n, est_dev, nvar_dev, stream)`: rows are copied into the tail of nvar_dev (host side) and H2D'd with nr_gpu_buf_h2d_async (no-op on DIRECT), so graph-safe per-caller storage; k_ls/k_delay_exact/k_fir on `stream`. nvar_dev layout (NR_GPU_CHEST_DEV_BYTES in nr_pdsch_gpu_chest.h): delay int[2048][3], nvar u32[2048], rows. Row pointers must be device-accessible; out==NULL -> est_dev + r*4096 c16. LS scratch is one module-global buffer (one pass in flight).
- `nr_pdsch_gpu_demod_dev(job, llr_dev, stream)` (demod.cu): gather/level/mask_pw/[align]/[mmse3_prep]/k_tail on `stream` using a private extra context g_ctx[CTX] (never handed out by acquire()); llr pointer overridden with llr_dev->dev; no D2H, no EVM (want_evm -> -1). Tail kernel selection extracted into `launch_tail()` shared with the old run.
- `nr_pdsch_gpu_ldpc_dev(jobs, n, llr_dev, bits_dev, stream)` (ldpc.cu): int8 LLRs already on device (stride NR_GPU_LDPC_DEV_IN_STRIDE per CB), snapshots+res into bits_dev (NR_GPU_LDPC_DEV_* in nr_pdsch_gpu_ldpc.h), memset-async of the -1 sentinels, cluster launch on `stream`, private extra context. Returns -2 below CC 9.
- Fixture `tests/nr_gpu_pass_fixture.h` (GpuPassFixture::small_grant, run_stages; tx_bits kept), test `GpuPassStages.DlStagesSameBitsBothModels`, CMake: test links to pdsch_gpu, runs in the build dir.

## TDD
RED (new test, old libpdsch_gpu.so copied aside, so `_dev` symbols are missing):
```
libpdsch_gpu.so has no nr_pdsch_gpu_fep_dev
nr_gpu_pass_test.cc:70: Failure  Value of: ok  Actual: false  Expected: true
[  FAILED  ] GpuPassStages.DlStagesSameBitsBothModels
```
(An earlier first draft SKIPped on missing symbols; changed to FAIL when the library loads.)
GREEN (GB10):
```
nrb=24 nl=2 qm=4: 25344 LLRs, BER vs tx 0.0000, LDPC rc 0 nsnap 7, parity checked (model 1: DIRECT)
nrb=12 nl=1 qm=6: 9504 LLRs, BER vs tx 0.0000, ...
nrb=24 nl=4 qm=2: 25344 LLRs, BER vs tx 0.0768, ...   (rank 4 ill-conditioned; sanity bound 0.15)
```
Full `ctest -j4` (build_t26): only the 5 known ARM failures (qam64, qam256, td_grantwork, dft_test, test_nr_modulation). test_nr_llr_norm, test_nr_pdsch_gpu_ldpc, test_nr_pusch_gpu_ldpc, test_nr_pdsch_gpu_demod, test_nr_pusch_gpu_fep, test_nr_gpu_fep_hygiene pass.

## Parity result
DIRECT (host ring read in place, host buffers) vs forced DISCRETE (device ring + H2D, device buffers): rxdataF, chest delay triples + nvar, demod LLRs (memcmp) and LDPC snapshots are bit-identical for all 3 shapes. Also all four stages are exercised on ONE stream with only the 3 intermediate syncs the test needs (nvar to the demod job).

## Concerns / deviations
1. cpu_reference_llr (OAI nr_slot_fep + nr_pdsch_channel_estimation + nr_rx_pdsch) is NOT implemented: it needs the PHY_NR_UE link set and a populated PHY_VARS_NR_UE (as test_nr_ssb_rate_match_prod), a different target and a multi-hundred-line harness. Replaced by (a) DIRECT vs DISCRETE bit parity, (b) hard decisions vs the transmitted bits (BER 0 for rank 1/2). So bit-exactness to the CPU chain on this fixture is NOT claimed here; it rests on the existing exact-path checks (ISAC_PDSCH_GPU_EXACT_CHECK, MmseExact.GpuMode3BitExact). Also the fixture uses its own synthetic DM-RS/QAM (as nr_pdsch_gpu_fep_test) instead of OAI nr_modulation/nr_pdsch_dmrs_rx. Needs a decision if the CPU reference is required for Stage 1.
2. ldpc_dev signature has an extra `int n` (jobs array) vs the brief; the brief's form has no count.
3. DIRECT test run on sens6 collapses to DISCRETE twice (platform model); fine. Not run on sm_89 (no such host here); the CC<9 path returns -2 by inspection only.
4. Shared scratch: chest LS buffer, demod context [CTX] and LDPC context [CTX] are single-instance: one pass in flight (documented in code). Demod/LDPC kernel args (job by value, DJob in mapped host memory) are captured by value, but x.dj is rewritten on each _dev call: Task 4 must keep one graph per module instance or copy DJobs per graph.
5. Demod job `nvar` is a by-value host number, so the graph pass will need the chest nvar before capture or a device-side read (not in scope here).
6. Memory: +4 IQ ring slots (pinned+device), +32 MB chest LS scratch, +1 demod and +1 LDPC context allocated at init.

## Fix report (controller rulings)
- Added `GpuPassStages.DevEqualsOldEntryPoints` (DIRECT model, GB10): same fixture inputs through nr_pdsch_gpu_exact_fep / nr_pdsch_gpu_exact_chest / nr_pdsch_gpu_demod_run (+release) / nr_pdsch_gpu_ldpc_run (own waits, host buffers) vs the four _dev functions; asserts identical rxdataF, chest delay + nvar, LLRs (memcmp), LDPC rc, nsnap, snapshots (K/8 defined bytes each) and syn0, for rank 1, 2, 4 shapes (24/2/4, 12/1/6, 24/4/2). Result: all identical, no differing field. Fixture: run_stages(llr, old_path=false); no cpu_reference function exists (the unimplemented one was never in the fixture, tx_bits is used for the BER check). Test unsets ISAC_GPU_FORCE_DISCRETE first (GpuMem.RoundTrip leaves it at 1; that made the first draft skip).
- DISCRETE-vs-DIRECT and BER checks kept; the LDPC snapshot comparison now covers only the defined K/8 bytes per snapshot (identical normalisation both paths).
- Commands: `ninja -j8 test_nr_gpu_pass`; `./test_nr_gpu_pass` -> 4 tests passed; ctest -R "test_nr_gpu_pass|llr_norm|gpu_ldpc|gpu_demod" all pass.
