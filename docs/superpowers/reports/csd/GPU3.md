# GPU3 — GB10 decode parity and slot timing (2026-10-09)

## Result

**The requested CPU parity and 500 µs p99 gates are not met.** The first reproducible PDSCH divergence on freshly recorded input was the OFDM FEP scale: cuFFT was unnormalised while OAI's ARM fixed-point DFT has net `1/sqrt(N)` scale. That is corrected. The next unresolved divergence is downstream of FEP: the CUDA pilot/channel estimator, antenna combining, equalizer and LLR scaling do not implement the CPU chain. The fresh `b` and `old` PDSCH dumps reach only **94.64%** and **98.62%** LLR sign agreement. The standalone GPU slot p99 is **802/612 µs before LDPC** on those captures. The PDCCH GPU LLRs remain diagnostic only, with **88.3%/77.2%** sign agreement, and cannot be switched into polar search without risking lost DCIs. [MEASURED GB10]

The full t26f-feature replays complete through `RAW_IQ_EOF`, but the normal 250 µs GPU budget admits **zero PDSCH GPU LLR jobs** in either GPU-flag run. Those runs cannot establish identical CRC, 0.1 dB EVM or zero lost DCIs. A budget-off diagnostic admitted seven `b` GPU jobs before the self-check disabled the path after two failures. The safety fallback remains active and `ISAC_PDCCH_GPU_FEP` / `ISAC_PDSCH_GPU` remain default off. [MEASURED GB10]

## Changes and stage isolation

* Rebuilt both CPU reference files from this tree, including SOLVE2 and DECODE-FIX. The v2 PDSCH dump records CPU `rxdataF` on the first DM-RS symbol for every receive antenna. It excludes grants whose CPU decode succeeded only with a recovered geometry, so the recorded job and reference LLR refer to the same grant configuration. The only queue change is a four-line recorder signature/call hook. The QAM-owned demodulation and passive decode files were not edited. [IMPLEMENTED]
* `k_fep_rotate` now applies `rsqrtf(N)` after cuFFT. On the `b` DM-RS allocation samples, CPU/GPU FEP correlation moved from **0.75246484** with GPU/CPU RMS **20.382** to **0.99999284** with RMS **1.000405**. The final least-squares fit error is **−48.44 dB** on `b` and **−43.43 dB** on `old`; int16 readback and OAI fixed-point rounding remain visible. LLR signs did not change after this FEP correction, because the float estimator/equalizer normalises much of a common input scale. [MEASURED GB10]
* The Gold kernel now generates only words needed by the allocation's largest DM-RS sequence index, bounded by `4 × pilots_per_RB × (start_RB + RB_count − DMRS_reference_RB)`. `nsys` kernel mean dropped **119.2 → 26.9 µs** (4.4×) on the recorded `b` batch; its share of CUDA kernel time fell **83.9% → 54.5%**. The remaining FFT is about 6.3 µs and LLR kernel about 5.5 µs per launch. Host upload, readback, synchronization and repeated slot launch sets still dominate wall latency. [MEASURED GB10]
* The slot benchmark reports per-grant LLR sign rates and first-DMRS FEP correlation. An opt-in `NR_GPU_FORCE_ANT` probe in the CUDA kernel isolated each receive antenna; no forced antenna resolves the `b` sign mismatch (best antenna 0: 94.99%). The PDCCH benchmark counts CPU/GPU zero LLRs separately from nonzero sign disagreements. [IMPLEMENTED + MEASURED GB10]

The CPU channel estimator makes fixed-point pilot products and OCC despreading, delay IDFT/rotation, FIR smoothing and noise estimates. Its decoder can select a receive branch or combine with per-RNTI MRC and ALIGN=2. The CUDA path uses float pilot LS, linear frequency/time interpolation, adjacent-pilot variance and whitened MMSE over all antennas. The corrected FEP establishes that **the remaining mismatch begins after FEP**, but this dump does not contain CPU pilot LS or per-RE final channel estimates, so the first differing *substage* among pilot LS, delay transform and FIR is not measured. The required final-channel **≤ −50 dB per-RE** gate is therefore unmeasured and unpassed. LLR scaling is also visibly different in the budget-off replay: GPU mean absolute LLR is about 6–318× CPU on the seven admitted checks. No claim of layer-map, descrambling, CRC or EVM parity follows from the QPSK-only bounded dump. [CODE-READ + MEASURED GB10]

For PDCCH, when **both** CPU and GPU LLR are nonzero, signs agree on 67,973/67,975 (`b`) and 33,343/33,345 (`old`). The large all-bit mismatch comes mostly from CPU fixed-point zeros: CPU has 40,612 (`b`) / 76,745 (`old`) zero LLRs versus GPU 8,626 / 13,893. This isolates a quantization/scale discrepancy at or before LLR output; it does not prove the PDCCH pilot LS and antenna gate match. The live hook still computes and consumes the CPU LLR. [MEASURED GB10 + CODE-READ]

## Recorded-input parity and latency

Fresh CPU dumps: `replays/gpu_cpu_b.bin` has 82 eligible jobs, 67 slot groups, maximum 16 grants per slot and 51 CPU CRC passes; `gpu_cpu_old.bin` has 98 jobs in 98 slots and 96 CPU CRC passes. Each fresh PDCCH dump has 128 passes in 128 windows. Those bounded eligibility samples do not cover every full-replay grant. Timings are host wall time on GB10, 20 repetitions; standalone PDSCH excludes LDPC and standalone PDCCH excludes polar search. The benchmark returns nonzero when its parity gate fails. [MEASURED GB10]

| Capture / stage | p50 / p99 µs | CPU parity on recorded input | Gate |
|---|---:|---|---|
| `b` PDSCH FEP + chest/LLR | 248.0 / 802.1 | 191,907/202,776 signs (94.64%); FEP corr 0.99999284; no length mismatch; 0 split slots | Fail |
| `old` PDSCH FEP + chest/LLR | 194.4 / 612.1 | 90,577/91,848 signs (98.62%); FEP corr 0.99997732; no length mismatch | Fail |
| `b` PDCCH upload + GPU LLR | 251.2 / 818.2 | 97,634/110,592 signs (88.3%); exact 11,286 | Fail |
| `old` PDCCH upload + GPU LLR | 282.5 / 853.3 | 85,371/110,592 signs (77.2%); exact 14,971 | Fail |

The recorded 16-grant `b` slot remains one launch set. This proves batch capacity for that sample only. p99 varies with host scheduling: another 20-repeat `b` run measured 604 µs; both results miss the full-slot-with-LDPC 500 µs requirement. [MEASURED GB10]

## Full offline replay: t26f features

`gpu_replay.sh` runs the same 12-second `raw_pci415_12s_b` and `raw_pci415_12s_20261008` raw-IQ files for each arm, with t26f feature flags, host-safe PDSCH cores 10–14, scanfast replay config and discovered CORESET handoff. `ISAC_MLOCK=0`; no radio, capture or OTA operation was performed. Both arms set CUDA LDPC and GPU discovery; `CPU` forces the two decode GPU switches off, while `GPU flags` enables them with `ISAC_GPU_SELFCHECK=128 ISAC_GPU_SELFCHECK_ENFORCE=1`. SCANTIME is dedicated-only over 20,000 occasions. Decode is the first-to-last consumer span for each absolute slot, including queue waits, grant retries and decoder work; it is not GPU kernel time. [MEASURED GB10]

| Capture | Arm | Scan p50 / p99 µs | All-grant span p50 / p99 µs | Slots / jobs | GPU LLR jobs |
|---|---|---:|---:|---:|---:|
| `b` | CPU | 507 / 2,189 | 11,068 / 101,270.5 | 2,770 / 6,634 | 0 |
| `b` | GPU flags | 618 / 3,187 | 9,063.3 / 83,926.7 | 4,909 / 10,580 | 0 |
| `20261008` | CPU | 324 / 1,504 | 3,308.5 / 89,347.6 | 488 / 714 | 0 |
| `20261008` | GPU flags | 327 / 1,487 | 5,042.1 / 217,296.4 | 441 / 482 | 0 |

The normal budget is `ISAC_GPU_BUDGET=1`, with a roughly 250 µs real-time GPU deadline. The full GPU runs report `dl_gpu_transfer_refused=23` (`b`) / 353 (`old`) and two RT GPU fallbacks each; eligible jobs do not reach the GPU LLR path. CPU/GPU-flag replays discover different sets of DCI and grant hypotheses, so their CRC and EVM totals are **not a paired GPU-decoder loss estimate**. [MEASURED GB10]

The replay analyzer's coarse EVMGRANT key `(RNTI, frame, slot, MCS, layers, RB allocation)` finds 931 common rows on `b` and 286 on `old`; CRC differs on 28 and 4, and SNR differs by >0.1 dB on 432 and 221 (max 9.8 and 13.8 dB). Scrambling hypothesis, RV and history are absent from that key. Since GPU LLR jobs are zero in all four normal rows, these differences are run-to-run discovery/solver variation, not GPU demodulation effects. [MEASURED GB10]

With `ISAC_GPU_BUDGET=0` only for diagnosis, the `b` GPU arm admits seven PDSCH LLR jobs. The first five strong-bit self-checks pass; the next two report 0.9910 and 0.9989 strong-bit sign agreement and disable the path, then the replay completes on CPU. These seven checks are not a full parity sample; no identical-CRC or 0.1 dB EVM conclusion is drawn. [MEASURED GB10, `replays/replay_gpu_gpu_compare_nobudget_b_211917/run.log`]

GPU2's **54–70 ms** full-replay all-grant p99 is the `PDSCH_SLOT_TIME` wall span across consumer groups for an absolute slot. It includes queue residence, scheduling gaps, CPU fallbacks, multiple grants, LDPC and retries. GPU2's standalone ~1.3 ms p99 was *before LDPC*. The present CPU-only span reaches 89–101 ms p99, and the normal GPU-flag runs use zero GPU PDSCH LLR jobs, confirming the tens-of-ms figure is replay/queue/decoder latency rather than one CUDA launch set. [CODE-READ + MEASURED GB10]

### Per-RNTI replay census

Each cell below is **DCIRAW / EVMGRANT CRC-OK / median EVMGRANT SNR dB**; `—` means no EVMGRANT sample. These are separate replay observations, not matched grant outcomes. [MEASURED GB10]

| Capture `b` RNTI | CPU | GPU flags |
|---|---:|---:|
| `0x007f` | 0 / 0 / 4.8 | 0 / 0 / 4.7 |
| `0x0211` | 0 / 0 / — | 39 / 1 / 5.2 |
| `0x0a51` | 0 / 0 / — | 10 / 0 / — |
| `0x0db1` | 4 / 0 / — | 145 / 43 / 3.7 |
| `0x101b` | 157 / 140 / 7.0 | 134 / 81 / 7.0 |
| `0x1a61` | 0 / 0 / — | 222 / 0 / 2.85 |
| `0x1e0e` | 0 / 0 / — | 2 / 0 / — |
| `0x2152` | 84 / 57 / 5.4 | 84 / 30 / 5.4 |
| `0x3383` | 71 / 44 / 4.7 | 65 / 6 / 4.5 |
| `0x365e` | 0 / 0 / — | 150 / 0 / — |
| `0x3699` | 0 / 0 / — | 35 / 4 / 3.5 |
| `0x37f3` | 500 / 338 / 12.1 | 462 / 177 / 12.1 |
| `0x4433` | 37 / 3 / 3.35 | 35 / 2 / 3.6 |
| `0x4e0a` | 72 / 20 / 8.1 | 53 / 5 / 4.2 |
| `0x5186` | 150 / 83 / 4.35 | 1144 / 441 / 4.4 |
| `0x59ab` | 225 / 50 / 3.3 | 209 / 5 / 3.9 |
| `0x5aea` | 125 / 15 / 3.4 | 109 / 10 / 3.5 |
| `0x5d93` | 272 / 82 / 4.3 | 237 / 51 / 4.5 |
| `0x621e` | 0 / 0 / — | 36 / 3 / 3.75 |
| `0x62a6` | 0 / 0 / — | 4 / 0 / — |
| `0x65d3` | 93 / 0 / — | 86 / 0 / — |
| `0x73de` | 6 / 0 / — | 34 / 0 / 2.6 |
| `0x7c8a` | 5 / 0 / — | 9 / 0 / — |
| `0x9539` | 42 / 0 / — | 0 / 0 / — |
| `0x9f86` | 0 / 0 / — | 444 / 50 / 4.7 |
| `0xa07a` | 227 / 95 / 4.4 | 173 / 14 / 3.7 |
| `0xac03` | 17 / 4 / 3.45 | 26 / 8 / 6.0 |
| `0xacd6` | 12 / 0 / — | 84 / 0 / — |
| `0xb2a3` | 25 / 0 / — | 0 / 0 / — |
| `0xb3bb` | 20 / 3 / 3.65 | 12 / 0 / — |
| `0xb3d3` | 174 / 118 / 5.95 | 170 / 5 / 7.15 |
| `0xc57a` | 9 / 0 / — | 9 / 0 / — |
| `0xc61e` | 0 / 0 / — | 29 / 1 / -0.7 |
| `0xcbb6` | 0 / 0 / — | 83 / 1 / 3.7 |
| `0xce76` | 326 / 0 / — | 3429 / 21 / 3.8 |
| `0xd026` | 9 / 0 / — | 69 / 11 / 2.0 |
| `0xd093` | 9 / 0 / 8.1 | 10 / 0 / — |
| `0xd76e` | 0 / 0 / — | 149 / 0 / 2.0 |
| `0xdb3e` | 0 / 0 / — | 227 / 50 / 4.9 |
| `0xdbc2` | 88 / 0 / — | 70 / 0 / — |
| `0xe113` | 20 / 9 / 3.7 | 16 / 2 / 3.8 |
| `0xe169` | 83 / 10 / 4.1 | 191 / 66 / 4.0 |
| `0xe2de` | 0 / 0 / — | 114 / 4 / 3.3 |
| `0xe513` | 248 / 96 / 4.05 | 233 / 53 / 4.1 |
| `0xe6aa` | 526 / 72 / 3.7 | 528 / 30 / 3.7 |
| `0xea0b` | 19 / 6 / 4.8 | 14 / 0 / — |
| `0xffff` | 0 / 18 / 14.4 | 0 / 10 / 13.5 |

| Capture `20261008` RNTI | CPU | GPU flags |
|---|---:|---:|
| `0x8f5a` | 461 / 166 / 13.35 | 465 / 170 / 9.1 |
| `0xffff` | 0 / 15 / 12.1 | 0 / 24 / 16.7 |


## Validation and reproduction

Build: RelWithDebInfo, `ENABLE_TESTS=ON`, `OAI_USRP=ON`, `OAI_SIMU=ON`, `ENABLE_ISAC_SENSING=ON`, four RX antennas, `ENABLE_LDPC_CUDA=ON`, `LDPC_CUDA_ARCH=121`, UHD paths `/usr/local/lib/libuhd.so` and `/usr/local/include`; writable CPM cache `/tmp/csd_gpu3_cpm`, read-only ccache. All builds, tests and replays ran through `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh`. The synthetic CUDA PDSCH FEP cases all passed, the CUDA PDCCH FEP test passed, and the focused `test_nr_pdcch_scan_fast` / `test_nr_gpu_fep_hygiene` CTests passed 2/2. The four recorded-input parity benchmarks fail their configured sign/correlation thresholds as shown above. [OFFLINE VERIFIED GB10]

```sh
O=/home/nicola/NICOLA/wt/rr-orchestration
B=/home/nicola/NICOLA/wt/rr-csd-x/cmake_targets/ran_build/build
bash tests/passive_rx/dgx/gpu_replay.sh cpu_dump b
bash tests/passive_rx/dgx/gpu_replay.sh cpu_dump old
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench "$2" --gpu 20' _ "$B" "$PWD/replays/gpu_cpu_b.bin"
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_pdcch_slot_bench "$2" --gpu 20' _ "$B" "$PWD/replays/pdcch_cpu_b.bin"
bash tests/passive_rx/dgx/gpu_replay.sh cpu_compare b
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare b
bash tests/passive_rx/dgx/gpu_replay.sh cpu_compare old
bash tests/passive_rx/dgx/gpu_replay.sh gpu_compare old
python3 tests/passive_rx/dgx/gpu2_analyze.py --pair replays/replay_gpu_cpu_compare_b_210837/run.log replays/replay_gpu_gpu_compare_b_211136/run.log
python3 tests/passive_rx/dgx/gpu2_analyze.py --pair replays/replay_gpu_cpu_compare_old_211443/run.log replays/replay_gpu_gpu_compare_old_211739/run.log
```

The exact **candidate OTA feature environment** supplied as `/home/nicola/NICOLA/wt/rr-orchestration/ota/t26f.env` is reproduced below, with the two default-off decode switches appended. It was **not OTA validated** here; the recorded parity and speed gates fail. Radio-only variables in this line were omitted from offline replay; `gpu_replay.sh` shows the exact host-safe replay substitutions. [USER-SUPPLIED CONFIGURATION + CODE-READ]

```text
ISAC_USRP_CATCHUP_X=1.05 ISAC_GPU_BUDGET=1 ISAC_GPU_DISCOVERY_BUDGET_PCT=30 ISAC_CSIRS_DISC_CUMULATIVE=1 ISAC_CSIRS_DISC_TRS_PRIORITY=1 ISAC_GPU_SELFCHECK=0 ISAC_CORE_PLAN=0 ISAC_USRP_NO_TX=1 ISAC_UHD_CTRL_CORES=15 NR_GPU_FEP=1 ISAC_TD_CB0_BACKEND=gpu ISAC_GPU_WORKER_CORE=10 ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_CORE=13 ISAC_PDCCH_NID_GPU_CAPTURE_EVERY=2 ISAC_PDCCH_NID_GPU_DUTY=1 ISAC_COREMAP_IDSWEEP_DUR=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1 ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_SCAN_TIMING=1 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_PDSCH_SOLVER_REUSE=1 ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1 ISAC_PDSCH_SOLVER_PROBE_RATE=0.5 ISAC_PDSCH_SOLVER_PROBE_FILL=25 ISAC_PDSCH_SOLVER_PROBE_BURST=16 ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_SOLVER_SCR=1 ISAC_PDCCH_JANON_ALL_CORESETS=1 ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1 ISAC_PDCCH_USS_CORE=12 ISAC_PDSCH_EVM_GRANT=1 ISAC_USRP_CHPOW_S=1 ISAC_RX_RECOVER=1 ISAC_RX_RECOVER_MAX=20 ISAC_USRP_RING_ZEROCOPY=1 ISAC_PDSCH_SOLVER_BACKOFF=200 ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1 ISAC_PDCCH_JOINT_ANON_RATE=0.1 ISAC_PDCCH_DED_BUDGET_SHARE=2 ISAC_PDCCH_TIMING=1 ISAC_SEGV_BT=1 ISAC_PDSCH_CORES=7,8,9,3,4 ISAC_RX_MRC_MODE=1 ISAC_RX_MRC_PER_RNTI=1 ISAC_RX_MRC_ALIGN=2 ISAC_CHEST_DELAY_SHARE=0 ISAC_CHEST_PDP_DIAG=0 ISAC_PDSCH_SOLVER_THREAD_CTX=1 ISAC_PDCCH_JOINT_ANON_STABLE=1 ISAC_PDSCH_SOLVER_SCR_EXPLICIT=1 ISAC_PDSCH_DMRS_ID_REFRESH=64 ISAC_PDSCH_SOLVER_DMRS_DOMAIN=1 ISAC_PDSCH_SOLVER_COVERAGE=1 ISAC_PDSCH_SOLVER_LAYOUT_RECOVER=32 ISAC_PDSCH_SOLVER_SCR_CLEAN=0 ISAC_PDCCH_JOINT_ANON_DISCOVERY=1 ISAC_PDSCH_SOLVER_NL_PREF=1 ISAC_PDCCH_JOINT_ANON_RATE=1 ISAC_PDCCH_NID_GPU_RECHECK_MS=250 ISAC_PDSCH_TDRA_RETRY=1 ISAC_PDSCH_CSI_RETRY=1 ISAC_PDSCH_DMRS_CDM_RETRY=1 ISAC_PDSCH_CSI_POPULATION_RETRY=1 ISAC_PDSCH_WIDE_MRC_RETRY=1 ISAC_PDCCH_GPU_FEP=1 ISAC_PDSCH_GPU=1 ISAC_PDSCH_GPU_LDPC=1
```

## Remaining gate

To meet GPU3, record CPU pilot LS and final per-RE channel estimates on identical grants, reproduce CPU delay/FIR and branch/MRC semantics in CUDA, then compare LLR scaling/layer mapping/descrambling and paired CRC/EVM. PDCCH needs the CPU antenna gate and fixed-point zero/scale behavior, then a deterministic zero-lost-DCI run before its LLRs are consumed. After parity, reduce upload/synchronization and integrate CUDA LDPC timing into a complete per-slot p99; the current standalone pre-LDPC result already exceeds 500 µs. [INFERRED FROM MEASUREMENTS]
