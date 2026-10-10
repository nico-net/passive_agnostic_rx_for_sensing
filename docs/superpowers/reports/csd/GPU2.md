# GPU2 — GB10 decode path and slot budget (2026-10-09)

## Verdict

**The 500 µs scan and all-grants decode gates are not met. CPU equivalence is not established.** On the GB10, the recorded PDSCH batch takes 654/1,195 µs p50/p99 before LDPC, and its LLR sign agreement with the CPU reference is 70.3%. The initial PDCCH GPU window takes 261/609 µs p50/p99 and has 81.9% sign agreement. These recorded-input measurements rule out claiming either zero CRC loss or zero DCI loss with the present kernels. [MEASURED GB10, initial `replays/gpu2_device_smoke.log`, 128 PDSCH jobs in 51 slots and 128 PDCCH passes in 75 slots]

The measured floor is the final recorded PDSCH FEP+chest/LLR p99 of **1,287 µs before LDPC** and the final PDCCH GPU window p99 of **553 µs before polar search**. The full replay p99 is much larger (PDSCH all-grants 54–70 ms; scan 1.2–3.6 ms). The dominant PDSCH cost is FEP plus chest/LLR followed by multiple LDPC TBs per slot; the scan still computes CPU LLRs and polar candidates for each pass. [MEASURED GB10 + CODE-READ]

The CUDA LDPC CB0 test passes 13/13 on the GB10. The synthetic FEP tests pass, but those fixtures are insufficient for CPU equivalence on the recorded channels. [MEASURED GB10]

## Implementation

* `nr_pdsch_passive_queue.c`: the GPU path can collect a head grant plus up to 64 more grants from one slot; the flag-off CPU path retains its prior eight extra-job grouping. The per-consumer LLR buffer holds 32 million `int16_t` values. `ISAC_PDSCH_SLOT_TIMING=1` logs the start and end time of each group so offline analysis can measure all grants in a slot. [CODE-READ]
* `nr_pdsch_gpu_fep.cu`: one launch set can hold up to 65 jobs if their aggregate output fits the 32 million LLR device buffer; larger outputs still chunk. An exported launch-set count verifies that the recorded 16-job slot is not split. [CODE-READ + MEASURED GB10]
* A GPU version of the CPU PDCCH roughness antenna gate was tested and reverted: on the recorded sample it changed no LLR signs and made the window slower. [MEASURED GB10]
* `nrLDPC_coding_interface_load.c`: `ISAC_PDSCH_GPU_LDPC=1` selects the existing `_cuda` decoder through the normal receiver loader when the caller passes a null version. It defaults off. [CODE-READ]
* `gpu_replay.sh` and `gpu2_analyze.py`: paired offline replay modes use the T26 recommended receive settings, SOLVE flags, and a host-safe core map. The analyzer reports SCANTIME, all-grants slot timing, DCIRAW per RNTI, CRC passes and EVMGRANT SNR. [CODE-READ]

## GB10 recorded-input measurements

| Gate / sample | p50 / p99 (µs) | CPU comparison | Result |
|---|---:|---|---|
| PDSCH GPU FEP + chest/LLR, 51 recorded slots, initial kernel | 654 / 1,195 | LLR sign 305,692/435,140 (70.3%); correlation 0.324; 21 length mismatches | Fail |
| PDCCH GPU upload + LLR, 75 recorded slot windows, initial kernel | 261 / 609 | LLR sign 306,098/373,680 (81.9%); correlation 0.434 | Fail |
| PDCCH GPU upload + LLR, final 100-repeat run | 244 / 553 | Same 306,098/373,680 sign agreement; 45 multi-pass windows per repeat | Fail |
| CUDA LDPC CB0 unit suite | — | 13/13 passed | Pass |
| PDSCH GPU FEP + chest/LLR, final 100-repeat batch run | 564 / 1,287 | LLR sign 305,692/435,140 (70.3%); correlation 0.324; 21 length mismatches; **0 split slots**, including the 16-job slot | Fail |
| Experimental PDCCH roughness gate, 100-repeat run (reverted) | 357 / 1,034 | Same 306,098/373,680 sign agreement | Fail |
| Old-capture PDSCH dump, 107 jobs in 107 slots, 100 repeats | 687 / 1,704 | LLR sign 107,826/108,936 (99.0%); correlation 0.388; 0 length mismatches, 0 split slots | Fail |
| Old-capture PDCCH dump, 128 one-pass slots, 100 repeats | 240 / 781 | LLR sign 85,440/110,592 (77.3%); correlation 0.503 | Fail |

The PDSCH slot benchmark excludes LDPC, so the final 564 µs p50 is already above the 500 µs all-grants target. Its two dominant stages are FEP (221/650 µs p50/p99) and chest/LLR (318/770 µs). The full-replay all-grants floor is higher because several grants and LDPC decodes run for each slot. [MEASURED GB10, 100-repeat recorded benchmark and full replays]

The old-capture dump was produced by `bash tests/passive_rx/dgx/gpu_replay.sh cpu_dump old` and read by both `nr_*_slot_bench` binaries under `heavy.sh`. It contains 107 eligible PDSCH jobs and 128 PDCCH passes; no old-capture slot in this bounded dump has multiple recorded jobs. [MEASURED GB10, `replays/gpu2_old_device.log`]

The two focused CPU tests `test_nr_pdcch_scan_fast` and `test_nr_gpu_fep_hygiene` passed 2/2. The frozen sens6 diff gate and `git diff --check` passed. [OFFLINE VERIFIED GB10]

The full `heavy.sh ninja -j6 tests` built, and `heavy.sh ctest -j4 -E '^nr_td_sim_test$'` ran in 340.60 s: 209 passed, six failed, 15 disabled. Failures: the two known ARM PUSCH RA0 QAM cases, `test_nr_td_grantwork` (also failed in GPU.md), known ARM `dft_test` and `test_nr_modulation`, and `nr_polar_sc_cuda_test` not found because the `tests` target does not depend on its binary. Building `nr_polar_sc_cuda_test` explicitly and rerunning its ctest passed 1/1. [OFFLINE VERIFIED GB10, `replays/gpu2_ctest.log` and `replays/gpu2_polar_test.log`; CODE-READ]

## Full replay comparison

All-grants decode time is the wall span from the first consumer group start to the last group end for the same absolute slot. The CPU path can split a slot across groups, so this span includes their overlap or gaps. The GPU path can batch up to 65 queued jobs. `ISAC_PDSCH_SLOT_TIMING=1` is used in both runs. [CODE-READ]

| Capture | Receiver | Scan p50 / p99 µs | All-grants decode p50 / p99 µs | DCIRAW | EVMGRANT CRC OK | GPU LLR jobs |
|---|---|---:|---:|---:|---:|---:|
| `raw_pci415_12s_b` | CPU front end + CUDA LDPC | 486 / 1,243 | 5,701 / 55,584 (3,158 slots) | 6,434 | 739 | 0 |
| `raw_pci415_12s_b` | GPU flags + CUDA LDPC | 486 / 1,608 | 5,749 / 64,978 (3,394 slots) | 6,360 | 465 | 2 |
| `raw_pci415_12s_20261008` | CPU front end + CUDA LDPC | 349 / 1,743 | 3,993 / 69,799 (448 slots) | 524 | 114 | 0 |
| `raw_pci415_12s_20261008` | GPU flags + CUDA LDPC | 302 / 3,599 | 3,307 / 54,143 (400 slots) | 453 | 109 | 2 |

Both GPU runs' safety gate disabled GPU LLRs after two self-check tolerance failures, then completed via CPU fallback. The CRC/DCI total differences are **run-to-run discovery and solver differences**, not a paired loss estimate: the runs admitted different grants. The deterministic recorded LLR gate already fails. All four rows reached `RAW_IQ_EOF`; the matched pairs used the same CUDA LDPC decoder. [MEASURED GB10, replays `replay_gpu_cpu_compare_b_175652`, `replay_gpu_gpu_compare_b_175352`, `replay_gpu_cpu_compare_old_175957`, `replay_gpu_gpu_compare_old_175027`]

On the old capture, the GPU run decoded 1,304 TBs with CUDA LDPC, with zero reported CUDA errors or fallbacks. The matched CPU run decoded 1,857 TBs with CUDA LDPC and zero reported errors. Both runs had 453 DCIRAW for 0x8f5a; CPU found 71 for 0xb876 while GPU found none. Since the PDCCH GPU LLRs are diagnostic-only, this difference is scan/discovery nondeterminism and cannot be attributed to consumed GPU LLRs. [MEASURED GB10]

Per-RNTI replay census below is **DCIRAW / EVMGRANT CRC-OK / median EVMGRANT SNR dB**. `—` means no EVMGRANT sample. It reports observations, not paired grant outcomes. [MEASURED GB10, matched final pairs]

For a closer replay check, matching EVMGRANT rows by `(RNTI, frame, slot, MCS, Nl, RB allocation)` gives 1,132 common rows on `b` and 164 on `20261008`. CRC differs on 29 and 2 rows respectively; SNR differs by more than 0.1 dB on 438 and 76 rows, with maximum absolute differences 8.6 and 3.1 dB. This key omits scrambling hypothesis, RV and HARQ history, so it is a coarse replay comparison; it still cannot certify the requested identical per-grant CRC and 0.1 dB EVM gates. [MEASURED GB10, offline log join]

| Capture `b` RNTI | CPU front end | GPU flags |
|---|---:|---:|
| `0x007f` | 0 / 0 / 4.8 | 0 / 0 / 4.65 |
| `0x0211` | 3 / 0 / — | 3 / 0 / — |
| `0x3383` | 7 / 0 / — | 6 / 0 / — |
| `0x37f3` | 393 / 0 / — | 370 / 0 / — |
| `0x5186` | 1,121 / 513 / 4.4 | 1,126 / 345 / 4.4 |
| `0x59ab` | 117 / 0 / — | 79 / 0 / — |
| `0x5aea` | 27 / 0 / — | 26 / 0 / — |
| `0x5d93` | 136 / 0 / — | 146 / 0 / — |
| `0x9f86` | 461 / 106 / 5.0 | 466 / 31 / 5.0 |
| `0xa07a` | 0 / 0 / — | 82 / 0 / — |
| `0xb3d3` | 103 / 0 / — | 0 / 0 / — |
| `0xce76` | 3,267 / 10 / 9.0 | 3,275 / 9 / 9.2 |
| `0xd76e` | 96 / 26 / 4.8 | 95 / 0 / — |
| `0xdb3e` | 100 / 65 / 6.1 | 104 / 64 / 4.7 |
| `0xdbc2` | 11 / 0 / — | 0 / 0 / — |
| `0xe169` | 108 / 6 / 4.1 | 108 / 6 / 4.1 |
| `0xe513` | 177 / 0 / — | 145 / 0 / — |
| `0xe6aa` | 307 / 0 / — | 329 / 0 / — |
| `0xffff` | 0 / 13 / 7.2 | 0 / 10 / 10.25 |

| Capture `20261008` RNTI | CPU front end | GPU flags |
|---|---:|---:|
| `0x8f5a` | 453 / 90 / 14.3 | 453 / 88 / 14.35 |
| `0xb876` | 71 / 0 / — | 0 / 0 / — |
| `0xffff` | 0 / 24 / 12.75 | 0 / 21 / 14.1 |

## Equivalence and limits

The PDSCH kernel still uses float pilot interpolation and adjacent-pilot noise. The CPU uses fixed-point pilots, an IDFT delay estimate, delay compensation, FIR smoothing, and per-RNTI combining with ALIGN=2. The recorded dump has CPU IQ and LLRs but no CPU per-RE channel estimates; the requested −50 dB channel-estimate gate cannot be measured from that file. Neither CRC identity nor 0.1 dB EVM agreement follows from the synthetic FEP tests. [CODE-READ + MEASURED GB10]

The single-launch result covers the bounded recorded 16-job slot. Live receiver batching of every grant is unproved because its GPU path disabled after two self-checks, and a slot exceeding 65 queued grants or 32 million aggregate LLRs still splits. [CODE-READ + MEASURED GB10]

The PDCCH receiver still computes CPU LLRs before its GPU comparison and only batches identical symbol windows in the standalone benchmark. It does not consume GPU LLRs across all CORESET passes. With the recorded 81.9% sign agreement, switching directly to those LLRs would risk lost DCIs. [CODE-READ + MEASURED GB10]

The required next implementation is an integer-equivalent PDSCH channel estimator with CPU delay/IDFT/FIR and per-RNTI combining, plus a dump of both CPU and GPU per-RE channel estimates. PDCCH needs a slot-wide upload and batch that preserves the CPU branch gate, equaliser and LLR scaling, then a deterministic zero-lost-DCI gate before the receiver consumes those LLRs. Neither implementation is claimed here. [INFERRED FROM CODE AND MEASUREMENTS]

## Exact offline commands and OTA feature environment

All builds, tests, and replays used `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh`; replays used recorded raw IQ, `ISAC_MLOCK=0`, and no radio. Build options: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_USRP=ON -DOAI_SIMU=ON -DENABLE_ISAC_SENSING=ON -DNB_ANTENNAS_RX=4 -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121 -DUHD_LIBRARIES=/usr/local/lib/libuhd.so -DUHD_INCLUDE_DIRS=/usr/local/include`. The writable CPM cache was `/tmp/csd_gpu2_cpm`; ccache was read-only. [MEASURED GB10]

The exact offline validation commands, from this worktree, are below. The replay script invokes `heavy.sh` and supplies `CAP_SAMPLES=1474560000`, capture ticks 208029025 and 210036277, speed 0.25, and the replay-only CORESET handoff. The `b` recorded files were copied read-only from the prior GPU worktree into this worktree's `replays/`; the `old` files were recorded here. [CODE-READ + MEASURED GB10]

```sh
O=/home/nicola/NICOLA/wt/rr-orchestration
B=/home/nicola/NICOLA/wt/rr-csd-s/cmake_targets/ran_build/build
$O/heavy.sh env CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp ninja -C "$B" -j6 nr-uesoftmodem nr_gpu_slot_bench nr_pdcch_slot_bench nr_pdsch_gpu_fep_test nr_pdcch_gpu_fep_test test_ldpc_cb0_cuda ldpc_cuda params_libconfig raw_iq
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_gpu_slot_bench" replays/gpu_cpu_b.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdcch_slot_bench" replays/pdcch_cpu_b.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdsch_gpu_fep_test" "$B/libpdsch_gpu.so"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdcch_gpu_fep_test" "$B/libpdcch_gpu.so"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./test_ldpc_cb0_cuda' _ "$B"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench /home/nicola/NICOLA/wt/rr-csd-s/replays/gpu_cpu_b.bin --gpu 100' _ "$B"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_pdcch_slot_bench /home/nicola/NICOLA/wt/rr-csd-s/replays/pdcch_cpu_b.bin --gpu 100' _ "$B"
bash tests/passive_rx/dgx/gpu_replay.sh cpu_compare b
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare b
bash tests/passive_rx/dgx/gpu_replay.sh cpu_compare old
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare old
bash tests/passive_rx/dgx/gpu_replay.sh cpu_dump old
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_gpu_slot_bench" replays/gpu_cpu_old.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" "$B/nr_pdcch_slot_bench" replays/pdcch_cpu_old.bin --cpu-only
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench /home/nicola/NICOLA/wt/rr-csd-s/replays/gpu_cpu_old.bin --gpu 100' _ "$B"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_pdcch_slot_bench /home/nicola/NICOLA/wt/rr-csd-s/replays/pdcch_cpu_old.bin --gpu 100' _ "$B"
$O/heavy.sh env CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp ninja -C "$B" -j6 tests
$O/heavy.sh ctest --test-dir "$B" -j4 -E '^nr_td_sim_test$' --output-on-failure
$O/heavy.sh env CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp ninja -C "$B" -j6 nr_polar_sc_cuda_test
$O/heavy.sh ctest --test-dir "$B" --output-on-failure -R '^nr_polar_sc_cuda_test$'
python3 tests/passive_rx/dgx/gpu2_analyze.py --pair replays/replay_gpu_cpu_compare_b_175652/run.log replays/replay_gpu_gpu_compare_b_175352/run.log
python3 tests/passive_rx/dgx/gpu2_analyze.py --pair replays/replay_gpu_cpu_compare_old_175957/run.log replays/replay_gpu_gpu_compare_old_175027/run.log
```

The exact **candidate OTA feature environment** is below. Config: `tests/passive_rx/dgx/ue.passive.ota.t26e.dgx.cfg`; normal 217-PRB n78/30-kHz X410 arguments from T26b §OTA. This is a description for the operator, not an OTA run; the GPU path failed its recorded parity gates. [INFERRED; NOT OTA-VALIDATED]

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
ISAC_PDSCH_SOLVER_NL_PREF=32 ISAC_PDSCH_SOLVER_SCR_CLEAN=1
ISAC_PDSCH_SOLVER_THREAD_CTX=1 ISAC_PDCCH_JOINT_ANON_STABLE=1
ISAC_PDCCH_GPU_FEP=1 ISAC_PDSCH_GPU=1 ISAC_PDSCH_GPU_LDPC=1
ISAC_TD_CB0_BACKEND=gpu
```

The actual replays used the host-safe PDSCH cores 10–14, the scanfast replay config, `ISAC_PDCCH_DISCOVERED_FILE=tests/passive_rx/dgx/scanfast_coresets.txt`, `ISAC_MLOCK=0`, `ISAC_PDSCH_SLOT_TIMING=1` and `ISAC_TD_CB0_BACKEND=gpu` in both arms. Only `ISAC_PDCCH_GPU_FEP` and `ISAC_PDSCH_GPU` differed between matched final CPU/GPU pairs; `ISAC_GPU_SELFCHECK=128 ISAC_GPU_SELFCHECK_ENFORCE=1` additionally protected the GPU arm. [CODE-READ]
