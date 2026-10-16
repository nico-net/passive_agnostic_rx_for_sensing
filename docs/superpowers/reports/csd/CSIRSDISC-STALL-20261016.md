# CSI-RS discovery GPU stalls: partition check and kernel chunking (stall plan Task 3 prep), 2026-10-16

Sonnet agent, commit 02c37dced9 (nr_gpu_green.h, nr_csirs_disc_gpu_sweep.cu, tests/nr_csirs_disc_sweep_test.cc); saved by the orchestrator.

## 1. Partitions work at SM level
[MEASURED, DGX replay b, t26aXE, ISAC_GPU_GREEN_LOG=1, n=1] csirsDisc gets the discovery green stream (3 translation units rt=0: 8 SMs, mask 0x03030303); PDSCH/PDCCH RT streams (rt=1, idx 0-7) 40 SMs, mask 0xfffffcfcfcfc; disjoint. nsys: csirsDisc on greenContextId 5/6/7, PDSCH on 2/3/4. Each TU builds its own green contexts (static in a static inline header function), all on the same SM sets: isolation not broken by that. RT_PARTS unset: all RT consumers share one 40-SM split.
Not SM contention [MEASURED, earlier V nsys trace]: in the 194 ms stall, PDSCH streams 1003/1004 (green ctx 2) kept running; only consumers 2-4's streams sat idle and restarted within ~0.5 ms of `sweep` ending -> hardware work-queue head-of-line blocking (CUDA_DEVICE_MAX_CONNECTIONS default 8, many streams) [INFERRED]. CUDA_DEVICE_MAX_CONNECTIONS=32: inconclusive (baseline nsys run had 0 stalls too); replay p99.9 16.3/13.9 ms (C) vs 14.7/44.2 ms (A), n=2.

## 2. Chunking (default off): ISAC_CSIRS_DISC_CHUNK_US=<us> (ISAC_CSIRS_DISC_CHUNK_UNITS=<n> for tests); ISAC_GPU_GREEN_LOG=1 diagnostic
sweep split by candidates; refine_kernel split by (valid slot, antenna, port) units with device accumulators; chunks async on one stream, no sync; chunk size learned from measured time per unit. Test CsirsDiscSweep.GpuChunkedBitExact (11 rows, units 1/3/1000, bit-exact) + all 9 GPU/Cumulative tests pass (private MPS dir).

## 3. Replay A/B [MEASURED, DGX replay, t26aXE + GREEN_LOG, interleaved A,B,A,B, n=2] DL tot p50/p99/p99.9 ms
| cell | arm | run 1 | run 2 |
|---|---|---|---|
| V (b) | A | 1.65 / 5.29 / 14.7 | 1.71 / 8.40 / 44.2 |
| V (b) | B CHUNK_US=200 | 1.57 / 5.80 / 16.5 | 1.51 / 5.37 / 16.5 |
| T (tim) | A | 0.98 / 3.11 / 9.5 | 1.04 / 2.83 / 8.4 |
| T (tim) | B | 1.08 / 14.8 / 25.0 (drops 3.7 %) | 1.06 / 5.10 / 19.3 (drops 0.4 %) |
Discovery output: same confirmed scrambling IDs both arms (V 415, 748, 380, 713, 249, 582; T 923); resource string counts vary A-vs-A too (real-time replay non-deterministic).
nsys cuda-only, b, n=1 per arm (replay_cdNA_182924 / cdNB_183035 / cdNC_183246): stalls >= 50 ms A 0, B 1, C 0; refine_kernel max A 273 ms, B 0.89 ms, C 599 ms; sweep max 218 / 5.3 / 213 ms. B's stall: passivePdsch3 k_evm_zero 330 ms behind a back-to-back chunk chain (chunks queued without sync act as one FIFO block). Chunk targets missed (sweep up to 5.3 ms).

## Verdict
Chunking is bit-exact but does not remove the stalls; T got worse (p99 3-5x, drops). Do not enable. X not set yet.
Candidates, in order: (a) hardware-queue head-of-line blocking (bound the chunk queue: enqueue k+1 only after k-1 completed; MAX_CONNECTIONS); (b) cudaMalloc/cudaFree in the fold_gpu path of nr_csirs_disc_gpu.cu (device-synchronising calls) [CODE-READ]; (c) copy-engine blocking behind long kernels (ISAC_GPU_COPY_FENCE=1 not tested). Unchunked long kernels remain: energies (1 thread, 7.9 ms), segment 80-110 ms, finish 50 ms, lattice_screen 46 ms.
Caveat: stall base rate per replay is low (0-3); n=1-2 cannot separate small effects; the on-air trace decides.
