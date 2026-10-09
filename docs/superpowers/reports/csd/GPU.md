# GPU slot-budget implementation (2026-10-09, csd/gpu)

## Verdict

**The requested 500 us/slot scan and decode gates have not been established.** This sandbox has no GPU device, and the existing CUDA PDSCH estimator does not implement the CPU delay transform or per-RNTI combining. Enabling its LLRs as if they were equivalent would risk changed SNR and CRC. The PDCCH CUDA kernel had a pilot-conjugation error fixed here, but the live scan still computes the CPU LLR and only compares the CUDA result, so `ISAC_PDCCH_GPU_FEP=1` does not yet shorten the scan. [CODE-READ; GPU execution DEFERRED]

The CPU-only replay on `raw_pci415_12s_b` reported dedicated-only scan p50 455 us and p99 1,140 us over 20,000 occasions; it decoded 2,520 PDSCH jobs with 173 CRC passes at the last queue report. These are replay measurements on DGX aarch64, not GPU or OTA measurements. [OFFLINE VERIFIED, replay `replay_scanfast_baseline_b_145152`]

## Implemented

Implementation points: `nr_pdcch_gpu_fep.cu:122` (pilot product), `nr_pdsch_gpu_fep.h:91` (new loader flag), `nr_pdsch_gpu_record.c:11` and `nr_pdcch_gpu_record.c:6` (recorders), `tests/nr_gpu_slot_bench.cc:1` and `tests/nr_pdcch_slot_bench.cc:1` (benchmark readers), `nr_pdsch_passive_queue.c:1479` and `nr_pdcch_blind_monitor_rt.c:5251` (small hooks), `executables/nr-uesoftmodem.c:232` (core planning). [CODE-READ]

* `ISAC_PDSCH_GPU=1` independently loads the existing `libpdsch_gpu.so`; `NR_GPU_FEP` remains its legacy alias. Both are off by default. The core planner recognizes the new flag. The existing PDSCH queue batches eligible grants within each consumer slot group into one FEP call and one LLR call, then passes LLRs to the decoder. The CUDA TB decoder still requires selecting `libldpc_cuda.so` in the loader. [CODE-READ]
* Corrected PDCCH DM-RS LS multiplication: `nr_pdcch_dmrs_ref` and the CUDA Gold helper both return `conj(X)`, so the LS estimate must multiply `Y * conj(X)` once. The synthetic fixture now transmits `X` rather than `conj(X)`. [CODE-READ; GPU unit run DEFERRED]
* `ISAC_PDSCH_GPU_DUMP=<file>` records bounded CPU-replay slot IQ, symbol rotation, FO, eligible grant parameters, CPU descrambled LLRs, CRC verdict and CPU EVM for `nr_gpu_slot_bench`. `ISAC_PDCCH_GPU_DUMP=<file>` records bounded CPU-replay CORESET IQ, geometry and CPU LLRs for `nr_pdcch_slot_bench`. `_MAX` defaults to 128 for both. The binary readers validate record sizes and group all recorded grants/passes by absolute slot. GPU mode reports per-slot p50/p99 and LLR agreement. CPU-only mode validates recorded inputs without a GPU. [IMPLEMENTED]

Hooks in files owned by other agents: `nr_pdsch_passive_queue.c` has one include, one recorder declaration, one call after the CPU decode and one loader-error wording change; `nr_pdcch_blind_monitor_rt.c` has one include and one two-line recorder call after CPU LLR generation. No changes were made to SOLVE's DCI sweep or solver files, or GENIE's decode/demodulation files. [CODE-READ]

## Validation

* [OFFLINE VERIFIED, DGX aarch64 sandbox] Configured `cmake_targets/ran_build/build` with CUDA sm_121 and the options in `CSD_COMMON_1009.txt`; built `nr-uesoftmodem`, `libpdsch_gpu.so`, `libpdcch_gpu.so`, both existing synthetic GPU test binaries, `params_libconfig` and `raw_iq` using `heavy.sh ninja -j6`. The CUDA modules compiled but could not execute without a GPU device.
* [OFFLINE VERIFIED replay, DGX aarch64 sandbox] `ISAC_PDSCH_GPU_DUMP=/home/nicola/NICOLA/wt/rr-csd-q/replays/gpu_cpu.bin ISAC_PDSCH_GPU_DUMP_MAX=128 bash tests/passive_rx/dgx/scanfast_replay.sh baseline b` reached `RAW_IQ_EOF` and produced a 130 MiB reference file. That script uses the known CORESET handoff and `ISAC_PDCCH_NID_GPU=0` because this sandbox has no device; it never touched a radio.
* [OFFLINE VERIFIED replay, DGX aarch64 sandbox] `bash tests/passive_rx/dgx/gpu_replay.sh cpu_dump b` reached `RAW_IQ_EOF` and wrote `replays/gpu_cpu_b.bin` (131 MiB) and `replays/pdcch_cpu_b.bin` (8.8 MiB). This diagnostic run's final scan p50/p99 were 443/1,282 us over 20,000 dedicated-only occasions; dump I/O makes these timing data unsuitable as a speed comparison.
* [OFFLINE VERIFIED, DGX aarch64 sandbox] `heavy.sh nr_gpu_slot_bench replays/gpu_cpu.bin --cpu-only` parsed 128 PDSCH jobs in 44 recorded slot groups, 4 CPU CRC passes and 113 CPU EVM samples.
* [OFFLINE VERIFIED, DGX aarch64 sandbox] After rebuilding the final readers, the second dump's CPU-only PDSCH reader parsed 128 jobs in 51 slot groups (13 CPU CRC passes, 100 CPU EVM samples). The largest recorded slot had 16 jobs; 3 slots exceeded the current CUDA module's 9-job launch-set limit and would be chunked. The PDCCH reader parsed 128 passes in 75 slots, at most 3 passes per slot (none over the 64-job module limit), and covered DM-RS IDs 415, 10163 and 62922. These are structural and CPU-reference checks, not GPU LLR equivalence.
* [OFFLINE VERIFIED, DGX aarch64 sandbox] `ctest -R '^(test_nr_pdcch_scan_fast|test_nr_gpu_fep_hygiene)$'` passed 2/2 focused CPU tests.
* [OFFLINE VERIFIED, DGX aarch64 sandbox] The required full `heavy.sh ctest -j4 -E '^nr_td_sim_test$'` completed in 266.51 s: 185 passed, 14 failed, 15 disabled (199 tests ran). The failures were `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `test_nr_td_grantwork`, `test_nr_td_cb0_batch`, `time_management_tests`, `dft_test`, `test_ldpc_cuda_pool`, `test_ldpc_cb0_cuda`, `nr_polar_sc_cuda_test` (binary missing), `test_nr_modulation`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb` and `nr_cuup_functional_test`. CUDA tests that require a device reported it unavailable; the other failures are outside this change's focused tests. The full output is `replays/gpu_ctest.log` (local diagnostic, not committed).

## Missing gates and limits

* No device-side PDCCH LLR equivalence or zero-lost-DCI audit was possible. The live PDCCH hook still launches once per eligible duration-1 pass and retains CPU LLRs; it does not batch all passes or shorten scan p99. The standalone PDCCH benchmark can measure batching on recorded windows, but the receiver must be refactored to consume those batches before the latency target can be claimed.
* `libpdsch_gpu.so` currently uses float linear interpolation and adjacent-pilot noise, while the CPU uses fixed-point pilot processing, `nr_passive_est_delay()`/IDFT, delay compensation and FIR smoothing. It does not implement per-RNTI MRC, ALIGN2 or IRC modes. Per-RE channel equivalence <= -50 dB, CRC equality and SNR equality are therefore unproved, and likely fail with the current kernel. The standalone benchmark compares LLRs but has no TB decoder or post-EQ SNR estimator; use the receiver replay with `ISAC_GPU_SELFCHECK` and EVMGRANT for those comparisons. No GPU performance claim is made.
* Recorded dumps are diagnostics only and can slow a replay. For slot batching, all jobs must share the same IQ, FO and rotation; `nr_gpu_slot_bench` rejects a slot when they differ. The PDCCH module supports one symbol window per upload.
* The PDSCH queue groups at most nine jobs per consumer request, and the CUDA module has a nine-job launch-set limit. The recorded 16-job slot proves that the requested single batch for every grant of a slot is not implemented. [OFFLINE VERIFIED recorded input]

## DEFERRED GPU MEASUREMENTS

Run all commands from `/home/nicola/NICOLA/wt/rr-csd-q` on the GB10 host, after rebuilding the committed tree. Each command is under `heavy.sh`; `LD_LIBRARY_PATH` selects this build's CUDA modules. These commands use offline replay only.

```sh
O=/home/nicola/NICOLA/wt/rr-orchestration
B=/home/nicola/NICOLA/wt/rr-csd-q/cmake_targets/ran_build/build
$O/heavy.sh ninja -C "$B" -j6 nr-uesoftmodem nr_gpu_slot_bench nr_pdcch_slot_bench nr_pdsch_gpu_fep_test nr_pdcch_gpu_fep_test test_ldpc_cb0_cuda ldpc_cuda params_libconfig raw_iq
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_gpu_slot_bench" /home/nicola/NICOLA/wt/rr-csd-q/replays/gpu_cpu_b.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdcch_slot_bench" /home/nicola/NICOLA/wt/rr-csd-q/replays/pdcch_cpu_b.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdsch_gpu_fep_test" "$B/libpdsch_gpu.so"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdcch_gpu_fep_test" "$B/libpdcch_gpu.so"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./test_ldpc_cb0_cuda' _ "$B"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench /home/nicola/NICOLA/wt/rr-csd-q/replays/gpu_cpu_b.bin --gpu 100' _ "$B"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_pdcch_slot_bench /home/nicola/NICOLA/wt/rr-csd-q/replays/pdcch_cpu_b.bin --gpu 100' _ "$B"
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare b
rg 'GPU_SELFCHECK|EVMGRANT|SCANTIME_FINE|PDSCHQ|DCIRAW' "$(cat replays/last)/run.log" > replays/gpu_compare_b_summary.txt
bash tests/passive_rx/dgx/gpu_replay.sh cpu_dump old
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_gpu_slot_bench" replays/gpu_cpu_old.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdcch_slot_bench" replays/pdcch_cpu_old.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench /home/nicola/NICOLA/wt/rr-csd-q/replays/gpu_cpu_old.bin --gpu 100' _ "$B"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_pdcch_slot_bench /home/nicola/NICOLA/wt/rr-csd-q/replays/pdcch_cpu_old.bin --gpu 100' _ "$B"
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare old
rg 'GPU_SELFCHECK|EVMGRANT|SCANTIME_FINE|PDSCHQ|DCIRAW' "$(cat replays/last)/run.log" > replays/gpu_compare_old_summary.txt
```

If the recorded dumps are absent, create them with the exact CPU command `bash tests/passive_rx/dgx/gpu_replay.sh cpu_dump b` before the commands above. `gpu_compare` sets `ISAC_PDCCH_GPU_FEP=1 ISAC_PDSCH_GPU=1 ISAC_GPU_SELFCHECK=128 ISAC_GPU_SELFCHECK_ENFORCE=1 ISAC_PDSCH_EVM_GRANT=1` and the same capture, known CORESET handoff and safe core map. Compare per-RNTI DCIRAW counts, TB CRC and EVMGRANT SNR with the CPU replay log. Do not treat differences between nondeterministic blind-search runs as a strict no-drop proof; the recorded LLR comparisons are the deterministic gate. [DEFERRED]

## Exact OTA feature environment (operator candidate; not run)

Config: `tests/passive_rx/dgx/ue.passive.ota.t26e.dgx.cfg`, build `cmake_targets/ran_build/build`, normal 217-PRB n78/30-kHz X410 arguments from T26b §OTA. The T26d recommended per-RNTI mode and delay settings supersede earlier T26c settings. This environment is **not validated**, and the two new GPU flags should be used on air only after the deferred recorded-input and replay gates pass.

```text
ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_JOINT=1 ISAC_CORE_PLAN=0
ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_DUTY=1
ISAC_COREMAP_IDSWEEP_DUR=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_CSIRS_DISC=1
ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1
ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1
ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300
ISAC_PDCCH_SCAN_TIMING=1 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1
ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 ISAC_PDSCH_SOLVER_REUSE=1
ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1
ISAC_PDSCH_SOLVER_PROBE_RATE=0.5 ISAC_PDSCH_SOLVER_PROBE_BURST=16
ISAC_PDSCH_SOLVER_PROBE_FILL=25 ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_EVM_GRANT=1
ISAC_PDSCH_SOLVER_SCR=1 ISAC_PDCCH_JANON_ALL_CORESETS=1
ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1
ISAC_PDSCH_SOLVER_BACKOFF=200 ISAC_PDSCH_SOLVER_SCR_CSIRS=1
ISAC_PDSCH_DMRS_ID_PER_CORESET=1 ISAC_PDCCH_JOINT_ANON_RATE=0.1
ISAC_PDCCH_DED_BUDGET_SHARE=2 ISAC_PDSCH_CORES=7,8,9,3,4
ISAC_RX_MRC_PER_RNTI=1 ISAC_RX_MRC_ALIGN=2
ISAC_CHEST_DELAY_SHARE=0 ISAC_CHEST_PDP_DIAG=0
ISAC_PDCCH_GPU_FEP=1 ISAC_PDSCH_GPU=1 ISAC_GPU_SELFCHECK=128
ISAC_GPU_SELFCHECK_ENFORCE=1 ISAC_TD_CB0_BACKEND=gpu
```

Leave `ISAC_RX_BRANCH_FO`, `ISAC_PDSCHQ_SIGNAL_TIME`, experimental `*_AB`, `ISAC_RX_IRC`, `ISAC_RX_ML_2L` and `ISAC_CHEST_OCC` unset. TB CUDA decoding additionally needs `--loader.ldpc.shlibversion _cuda`; the present replay wrapper does not pass that option. [INFERRED from T26d and code]
