# GPU4 — GB10 decode follow-up (2026-10-09)

## Result

**The CPU parity, zero-lost-DCI and 500 µs full-slot gates remain open.** On the recorded inputs, PDSCH LLR sign agreement is 94.640% (`b`) and 98.616% (`20261008`). PDCCH LLR sign agreement is 88.283% and 77.195%; its GPU LLRs are still diagnostic and never feed polar search. The optimized standalone `b` PDSCH FEP + chest/LLR path measures 156/355 µs p50/p99 with an opt-in paired asynchronous path, **before LDPC**. The `20261008` p99 is 714 µs in the corresponding 20-repeat run. These are not full-replay slot times. [MEASURED GB10]

## Stage isolation and changes

The first-DM-RS CPU/GPU FEP correlation remains 0.99999284 on `b` and 0.99997732 on `20261008`, with the exact same raw GPU LLR hash before and after the Gold rewrite. The remaining PDSCH mismatch is not mostly CPU zero quantization: when both LLRs are nonzero, sign agreement is only 189,009/197,416 (95.742%) on `b` and 90,302/91,379 (98.821%) on `20261008`. A global negative dead zone of 1..1024 does not approach 99.9%. The estimator/equalizer chain remains divergent after FEP: CPU uses fixed-point pilot products, delay de-rotation and FIR smoothing, then per-RNTI MRC with `ISAC_RX_MRC_ALIGN=2`; CUDA uses float pilot LS, linear interpolation and whitened MMSE. The first differing *numerical substage* and the required per-RE chest error ≤ −50 dB were not measured. [MEASURED GB10 + CODE-READ; gate FAIL]

The existing `NR_GPU_FORCE_ANT` diagnostic narrows the combining question. With only antenna 0 in the GPU equalizer, `b` sign agreement is 192,615/202,776 (94.989%); antennas 1–3 give 87.828%, 89.746% and 90.535%. On `20261008`, antennas 0–3 give 97.757%, 95.578%, 94.987% and 97.560%, all below the combined 98.616%. The mismatch therefore persists without multi-antenna GPU combining; a CPU-equivalent pilot and channel-estimation comparison is still needed. [MEASURED GB10; gate FAIL]

For PDCCH, nonzero CPU/GPU pairs agree in sign on 67,973/67,975 (`b`) and 33,343/33,345 (`20261008`). CPU produces 40,612/76,745 zero LLRs versus GPU 8,626/13,893. Sweeping a global negative dead zone improves `20261008` at best to 83.681% (threshold 4) and worsens `b`; the CPU branch gate, fixed-point compensation, symbol P90 autoscale and clipping still need matching. No GPU LLR is consumed by polar search. [MEASURED GB10 + CODE-READ; gate FAIL]

The PDSCH Gold sequence generator now uses 31-bit recurrence jump images so CUDA lanes generate independent 32-bit words. `nsys` reports 26.87 → 8.21 µs mean kernel duration across 67 recorded `b` launches. The complete raw LLR FNV64 is unchanged (`836e8791953cf018`), including the recorded 16-grant slot. `ISAC_PDSCH_GPU_BATCH_ASYNC=1` optionally queues FEP and LLR on the same stream with one completion wait; the blocking API remains default. The self-check copies only one FFT symbol when all jobs share the same first DM-RS symbol, and charges that smaller readback to the transfer pacer; mixed-symbol batches retain the full readback. The focused CUDA test passes and the recorded `b` LLR hash and sign counts are unchanged. [IMPLEMENTED + MEASURED GB10]

`ISAC_PDSCH_GPU_CSI_DISJOINT=1` optionally admits grants with CSI-RS configuration only when the CPU extractor's own `nr_dlsch_csi_unav_res()` reports zero excluded REs for that allocation. Row, BWP and symbol bounds are checked; true overlap and PT-RS still fall back. The flag defaults off. The GPU self-check comparison buffer now rounds `G` up to 32 elements because `nr_codeword_unscrambling()` writes full 32-element SIMD blocks. The `20261008` replay previously crashed in `free()` after a self-check with `G=840`, which is not divisible by 32. After the allocation fix, its full replay reached `RAW_IQ_EOF`. [IMPLEMENTED + MEASURED GB10]

## Recorded-input parity and latency

Both captures use the fresh GPU3 CPU dumps from this branch, which exclude decodes recovered with changed geometry. The GPU4 Gold and async runs preserve the raw GPU LLR hash on `b`; the `20261008` async hash is `5ed31337b92ab6da`. Timing is 20 repetitions on GB10. The optional async FEP column is host enqueue time; the total column includes GPU completion. Neither total includes LDPC. [MEASURED GB10]

| Capture / mode | Slot FEP + chest/LLR p50 / p99 µs | CPU LLR signs | Nonzero signs | Gate |
|---|---:|---:|---:|---|
| `b`, GPU3 blocking | 248 / 802 | 191,907/202,776 | — | fail |
| `b`, GPU4 blocking | 231 / 538 | 191,907/202,776 | 189,009/197,416 | fail |
| `b`, GPU4 async | 156 / 355 | 191,907/202,776 | 189,009/197,416 | fail |
| `20261008`, GPU3 blocking | 194 / 612 | 90,577/91,848 | — | fail |
| `20261008`, GPU4 async | 163 / 714 | 90,577/91,848 | 90,302/91,379 | fail |

The `nsys` profile (`replays/gpu4_jump_profile.nsys-rep`, exported `replays/gpu4_jump_profile.sqlite`) of the 67-slot `b` batch puts the new Gold kernel at 8.21 µs, cuFFT at 6.23 µs and LLR at 5.50 µs mean per launch. It records 203 host-to-device copies (11.1 µs mean) and 335 device-to-host copies (7.2 µs mean); the profiler includes diagnostic readbacks outside the timed slot call. Host launch, copy and synchronization overhead dominate the new kernel cost. [MEASURED GB10]

## Full offline replay and admission

The normal t26f 250 µs GPU RT deadline admitted **zero** PDSCH LLR jobs in GPU3's full `b` and `20261008` replays. Offline `ISAC_GPU_RT_DEADLINE_US=500` admitted four `b` and three `20261008` jobs; 1 ms admitted six and three. Every sampled GPU CRC outcome matched its CPU rerun, but two LLR tolerance failures in each replay disabled GPU decoding. GPU EVM is invalid (`evm_gpu=-1`) on all sampled jobs. These tiny samples cannot establish identical CRC for every RNTI or 0.1 dB EVM. [MEASURED GB10; full parity gate FAIL]

| Diagnostic deadline | Capture | RNTI | GPU CRC pass / sampled | CPU CRC pass / sampled | GPU EVM valid |
|---|---|---|---:|---:|---|
| 500 µs | `b` | `0x007f` | 0/2 | 0/2 | no |
| 500 µs | `b` | `0xffff` | 2/2 | 2/2 | no |
| 500 µs | `20261008` | `0xffff` | 3/3 | 3/3 | no |
| 1 ms | `b` | `0x007f` | 0/3 | 0/3 | no |
| 1 ms | `b` | `0xffff` | 3/3 | 3/3 | no |
| 1 ms | `20261008` | `0xffff` | 3/3 | 3/3 | no |

The 250 µs deadline spans FEP and LLR, and the eligibility check excludes every configured CSI-RS grant by default, including zero-overlap configurations. The PDSCH admission function also charges three copies of the 4-antenna slot IQ plus LLR and optional self-check readback against a 100 MB/s per-module transfer pacer. Full `b` replays at the unchanged 250 µs deadline admitted zero jobs with async FEP alone, async plus CSI-disjoint eligibility, and async plus CSI-disjoint eligibility plus the one-symbol self-check readback. Extending the deadline to 1 ms admitted work, while self-check showed the LLR parity gate still fails. A 1 ms CSI-disjoint replay admitted five jobs versus six without that option, within run-to-run variation; it does not establish an admission benefit. Aggregate transfer-refusal metrics include other DL GPU modules and cannot be attributed wholly to PDSCH. [CODE-READ + MEASURED GB10]

| Capture / arm | SCANTIME p50 / p99 µs | All-grant span p50 / p99 µs | Slots / jobs | GPU PDSCH LLR jobs | Outcome |
|---|---:|---:|---:|---:|---|
| `b`, GPU3 CPU | 507 / 2,189 | 11,068 / 101,271 | 2,770 / 6,634 | 0 | reference replay |
| `20261008`, GPU3 CPU | 324 / 1,504 | 3,309 / 89,348 | 488 / 714 | 0 | reference replay |
| `b`, GPU4 async 250 µs | 633 / 3,098 | 15,940 / 93,552 | 5,159 / 10,767 | 0 | deadline gate still blocked GPU LLR |
| `b`, GPU4 async 250 µs + CSI + short readback | 625 / 3,251 | 8,929 / 81,682 | 4,984 / 10,930 | 0 | deadline gate still blocked GPU LLR |
| `b`, GPU4 async 500 µs + CSI + short readback | 615 / 2,914 | 16,579 / 86,797 | 5,329 / 10,521 | 4 | self-check disabled GPU after two failures |
| `b`, GPU4 1 ms diagnostic | 619 / 3,143 | 11,188 / 93,103 | 5,065 / 10,727 | 6 | self-check disabled GPU after two failures |
| `b`, GPU4 1 ms + CSI + short readback | 504 / 2,419 | 15,702 / 87,369 | 3,461 / 7,497 | 5 | final rebuilt receiver reached EOF; self-check disabled GPU |
| `20261008`, GPU4 1 ms diagnostic | 309 / 1,512 | 3,174 / 78,188 | 492 / 718 | 3 | replay completed; self-check disabled GPU after two failures |
| `20261008`, GPU4 async 500 µs + CSI + short readback | 343 / 2,326 | 11,122 / 65,095 | 889 / 2,763 | 3 | self-check disabled GPU after two failures |

The all-grant span is first-to-last consumer activity per absolute slot, including queue time, retries and LDPC. It is not a CUDA launch latency. The CPU/GPU runs discover different hypotheses, and the replay analyzer's coarse key lacks scrambling identity, RV and HARQ history; differences in whole-run CRC or EVM totals cannot be assigned to GPU demodulation. [CODE-READ + MEASURED GB10]

## Validation commands and environment

All builds, CUDA tests, benchmarks and replays use `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh`, with `ninja -j6`, `ISAC_MLOCK=0`, no radio/capture/OTA, and no core 15–19 pinning. CMake is the GPU3 RelWithDebInfo build with CUDA `sm_121`, four RX antennas, USRP and passive sensing enabled. The benchmark commands are: [OFFLINE VERIFIED GB10]

```sh
O=/home/nicola/NICOLA/wt/rr-orchestration
B=/home/nicola/NICOLA/wt/rr-csd-x/cmake_targets/ran_build/build
$O/heavy.sh env CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp ninja -C "$B" -j6 pdsch_gpu nr-uesoftmodem nr_pdsch_gpu_fep_test nr_gpu_slot_bench nr_pdcch_slot_bench
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_pdsch_gpu_fep_test && ctest -R test_nr_gpu_fep_hygiene --output-on-failure' _ "$B"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench "$2" --gpu 20' _ "$B" "$PWD/replays/gpu_cpu_b.bin"
$O/heavy.sh env ISAC_PDSCH_GPU_BATCH_ASYNC=1 LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench "$2" --gpu 20' _ "$B" "$PWD/replays/gpu_cpu_b.bin"
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare_budget1ms b
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare_async500_csi old
```

The replay script passes `CAP_SAMPLES=1474560000`, `ISAC_RAW_IQ_SPEED=0.25`, `REPLAY_PREFIX=""`, `ISAC_MLOCK=0`, `ISAC_PDSCH_CORES=10,11,12,13,14`, and the t26f flags listed in the script. The two raw captures are `/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b` (`CAP_TICK=208029025`) and `/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008` (`CAP_TICK=210036277`). `gpu_compare_budget1ms` adds `ISAC_PDCCH_GPU_FEP=1 ISAC_PDSCH_GPU=1 ISAC_GPU_SELFCHECK=128 ISAC_GPU_SELFCHECK_ENFORCE=1 ISAC_GPU_RT_DEADLINE_US=1000 ISAC_SEGV_BT=1`. [CODE-READ]

The exact proposed OTA feature environment is reproduced below from the `t26f.env` candidate, with the three GPU decode flags appended as in GPU3. It was not used for OTA here; radio-only variables are omitted from the offline replay. The GPU4 opt-in flags are absent from this candidate, and the parity and latency gates fail. [USER-SUPPLIED CONFIGURATION + CODE-READ]

```text
ISAC_USRP_CATCHUP_X=1.05 ISAC_GPU_BUDGET=1 ISAC_GPU_DISCOVERY_BUDGET_PCT=30 ISAC_CSIRS_DISC_CUMULATIVE=1 ISAC_CSIRS_DISC_TRS_PRIORITY=1 ISAC_GPU_SELFCHECK=0 ISAC_CORE_PLAN=0 ISAC_USRP_NO_TX=1 ISAC_UHD_CTRL_CORES=15 NR_GPU_FEP=1 ISAC_TD_CB0_BACKEND=gpu ISAC_GPU_WORKER_CORE=10 ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_CORE=13 ISAC_PDCCH_NID_GPU_CAPTURE_EVERY=2 ISAC_PDCCH_NID_GPU_DUTY=1 ISAC_COREMAP_IDSWEEP_DUR=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1 ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_SCAN_TIMING=1 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_PDSCH_SOLVER_REUSE=1 ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1 ISAC_PDSCH_SOLVER_PROBE_RATE=0.5 ISAC_PDSCH_SOLVER_PROBE_FILL=25 ISAC_PDSCH_SOLVER_PROBE_BURST=16 ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_SOLVER_SCR=1 ISAC_PDCCH_JANON_ALL_CORESETS=1 ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1 ISAC_PDCCH_USS_CORE=12 ISAC_PDSCH_EVM_GRANT=1 ISAC_USRP_CHPOW_S=1 ISAC_RX_RECOVER=1 ISAC_RX_RECOVER_MAX=20 ISAC_USRP_RING_ZEROCOPY=1 ISAC_PDSCH_SOLVER_BACKOFF=200 ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1 ISAC_PDCCH_JOINT_ANON_RATE=0.1 ISAC_PDCCH_DED_BUDGET_SHARE=2 ISAC_PDCCH_TIMING=1 ISAC_SEGV_BT=1 ISAC_PDSCH_CORES=7,8,9,3,4 ISAC_RX_MRC_MODE=1 ISAC_RX_MRC_PER_RNTI=1 ISAC_RX_MRC_ALIGN=2 ISAC_CHEST_DELAY_SHARE=0 ISAC_CHEST_PDP_DIAG=0 ISAC_PDSCH_SOLVER_THREAD_CTX=1 ISAC_PDCCH_JOINT_ANON_STABLE=1 ISAC_PDSCH_SOLVER_SCR_EXPLICIT=1 ISAC_PDSCH_DMRS_ID_REFRESH=64 ISAC_PDSCH_SOLVER_DMRS_DOMAIN=1 ISAC_PDSCH_SOLVER_COVERAGE=1 ISAC_PDSCH_SOLVER_LAYOUT_RECOVER=32 ISAC_PDSCH_SOLVER_SCR_CLEAN=0 ISAC_PDCCH_JOINT_ANON_DISCOVERY=1 ISAC_PDSCH_SOLVER_NL_PREF=1 ISAC_PDCCH_JOINT_ANON_RATE=1 ISAC_PDCCH_NID_GPU_RECHECK_MS=250 ISAC_PDSCH_TDRA_RETRY=1 ISAC_PDSCH_CSI_RETRY=1 ISAC_PDSCH_DMRS_CDM_RETRY=1 ISAC_PDSCH_CSI_POPULATION_RETRY=1 ISAC_PDSCH_WIDE_MRC_RETRY=1 ISAC_PDCCH_GPU_FEP=1 ISAC_PDSCH_GPU=1 ISAC_PDSCH_GPU_LDPC=1
```
