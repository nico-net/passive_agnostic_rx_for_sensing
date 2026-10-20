# LDPC-PERCB-20261016: why DL GPU LDPC cost grows linearly with code blocks

Bench: d7fb570067 (`./test_nr_pdsch_gpu_ldpc 2 bench [reps]`, nr_pdsch_gpu_ldpc_test.c; ISAC_BENCH_FINE=1, ISAC_BENCH_R=<13|23|89>). Analysis agent (Sonnet); saved by the orchestrator.
Label for all numbers: [MEASURED, DGX spark-74c3 GB10 bench, 2026-10-16 09:40-10:00, idle host under heavy.sh, core 8 (X925), private MPS dir, BG1 Z=384 K=8448, it_max 8, 100 reps, p50] unless marked.
Caveat: synthetic LLRs (decodable 12*(1+0.9*gauss), noise blocks pure noise); on-air LLR statistics and row-set mix not reproduced.

## Mechanism (definite)
Cost is linear in C because the CLUSTER kernel runs only ~3-4 code blocks at a time; everything else is flat.
- One launch, no per-iteration syncs: `nr_pdsch_gpu_ldpc_run` copies inputs, one `cudaLaunchKernelEx`, polls `cudaStreamQuery`, snapshots via zero-copy mapped pinned memory. Host overhead beyond the kernel: 2 us (C=1) to ~20 us (C=32).
- Launch geometry (nr_pdsch_gpu_ldpc.cu): 8 blocks x 512 threads per code block as one 8-block cluster (CLB=8); 128 registers/thread (cuobjdump) -> one block per SM -> each CB holds 8 SMs for its whole duration; a call is a series of waves.
- Kernel time, R13, all 48 SMs (RT_ISOLATE unset): C=1 113 us, 4 120, 5 208, 8 224, 9 308, 16 451, 32 875 -> 4 concurrent clusters, ~28 us/CB steady state.
- ISAC_GPU_RT_ISOLATE=1 + ISAC_GPU_DISC_SMS=8 (t26f): RT green context has 40 SMs -> 3 concurrent clusters. Run-wall p50: C=1-3 115 us, 4-6 278, 8 352, 16 646, 32 1230 (~38 us/CB + ~160 us step at C>=4). 1.4x worse than no isolation (29 us/CB at C=32).
- Poll sleep 200 vs 0: no difference (<= 6 us). Not a cause.
- Iterations: passive default ISAC_LDPC_ITERS=8 (nr_pdsch_passive_decode.c:1828); CRC-fail blocks run all iterations; the GPU decodes the later blocks of a failed TB anyway. Noise vs decodable at C=1: R13 117 vs 113 us, R23 88 vs 59 us.

## Second linear component: CPU pre-step
Serial on the consumer thread (`prep_segment`, nr_pdsch_gpu_ldpc_host.c): R13 (E=25344) 18.9 us/CB on X925 (0.6 ms at C=32), R23 9.9, R89 7.6. Per-snapshot CRC 0.08 us. A725 not measured.

## Why OTA shows ~95 us/CB vs 38 in the bench
[INFERRED, not reproduced] On air 5 PDSCH consumers (8 pool contexts) plus FEP/chest/demod kernels share the same 40-SM partition; only ~3 clusters fit across all of them, so a CB also waits for other consumers' clusters. The GPU_EXACT_LDPC counter includes this wait.

## Other CUDA contexts (time slicing)
- In one process, green contexts share the primary context [CODE nr_gpu_green.h; not verified at runtime]: hardware SM sharing, no time slicing.
- Across processes (MPS off): a second process burning 1.3 ms of every 4.27 ms on all SMs (mimics the channelizer): p50 unchanged, p99 115-900 us -> 2.4-3.5 ms (one ~2.3 ms slice). Two burners: every call waits, run-wall p50 4.0-5.7 ms for any C (vs 0.11-1.2 ms alone). Matches MPCI-OTA-20261016.md (first body 0.79 -> 1.77 ms with the channelizer). Separate, additive mechanism, independent of C; needs one CUDA context (or MPS) to remove.

## Fix to try first (not implemented): non-cluster kernels for larger C
`_nc` kernels (one 512-thread block per CB) exist, are bit-identical (ctest test_nr_pdsch_gpu_ldpc_nc); today ISAC_GPU_LDPC_NOCLUSTER=1 selects them globally.
Run-wall p50, t26f env (RT_ISOLATE=1, DISC_SMS=8, POLL=200), us:
| rows / blocks | C=1 | C=4 | C=8 | C=16 | C=24 | C=32 |
|---|---|---|---|---|---|---|
| R13 decodable, cluster | 114 | 278 | 352 | 646 | 937 | 1230 |
| R13 decodable, nc | 347 | 350 | 352 | 358 | 364 | 441 |
| R23 decodable, cluster | 59 | 113 | 176 | 355 | 503 | 651 |
| R23 decodable, nc | 108 | 129 | 133 | 139 | 148 | 158 |
| R23 noise, cluster | 88 | 171 | 281 | 574 | 724 | 945 |
| R23 noise, nc | 167 | 173 | 179 | 191 | 205 | 222 |
nc is flat to ~30 CBs (40 single-SM CBs fit at once): 32-CB R13 0.44 vs 1.23 ms (2.8x), R23 0.16 vs 0.65 (4x), R23 noise 0.22 vs 0.95 (4.3x). nc is slower at C=1 (347 vs 114 us R13): crossover C ~3-4.
Minimal change: in `ldpc_launch` pick nc for n >= 4, cluster below; warm both kernel families in `warm_common` (lazy-load stalls); same selection in the UL pool (`ul_exec` -> `ldpc_launch`); rerun test_nr_pdsch_gpu_ldpc and _nc.
Expected for a 30-CB TB (~250 kbit): GPU LDPC ~0.40-0.45 ms (R13) / 0.15 ms (R23) vs 1.2 / 0.65 ms bench and ~2.9 ms on air; the CPU pre-step (0.3-0.57 ms, serial) then dominates -> parallelise it next. On air the gain should be larger (nc frees the shared partition) [INFERRED].
Cluster size 2 or 4: not measured (nc is ~3x more SM-efficient: 310 vs 904 SM-us per CB).

## Not done
No demod/chest bench; no receiver run with the fix; A725, Nl 2-4, R89 fine sweep, nc under mixed multi-consumer load not measured.
