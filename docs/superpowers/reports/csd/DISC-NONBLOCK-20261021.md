# Discovery never blocks real-time DL/UL decode, 2026-10-21

Labels: [BENCH GB10] = tests/passive_rx/gpuspan/launchblock_bench.cu on the DGX GB10 (sm_121), 5 real-time threads (4 DL + 1 UL green context), 3 launches of a graph with a conditional (IF) node, a host-to-device and a device-to-host memcpy node, back to back every ~300 us, 20 s per row, 100 000 to 230 000 launches per row. [MEASURED nsys] = nsys_gsT2N / nsys_gsV2N from 2026-10-19 (merge19 build), re-read here with SQL. [OTA UNVERIFIED] = nothing on air yet with this change. sm_89 is not measured (no such host here); nothing in the fix depends on the SM count.

## 1. Mechanism (task 1)

Three separate things were isolated. Only the first two reproduce the on-air pattern.

| # | factor | result [BENCH GB10] |
|---|---|---|
| A | discovery thread waits for its kernel with a **blocking** cudaEventSynchronize (cudaEventBlockingSync, or a device scheduled BlockingSync as nrLDPC_cuda does) and the real-time graph has a **conditional node** | the IF-body launch of the real-time graphs is held until the wait returns: stall = rest of the kernel. 100 ms kernel: 620 of 136 155 passes > 1 ms, max 99.8 ms. 300 ms: 220 of 103 196, max 299.8 ms. 600 ms: 116 of 107 497, max 596 ms |
| B | a copy (H2D or D2H) is queued behind the long kernel in the discovery stream (the code comment "300-400 ms" in disc_copy) | graphs with copy nodes wait for the kernel end, also with polling and also without conditional nodes: 176 to 227 of 150 000 passes > 1 ms, max 100 ms |
| C | memory call (cudaMalloc/cudaFree/cudaHostAlloc/cudaFreeHost) while a kernel runs | the **launch call itself** blocks for the rest of the kernel (p50 99 ms, all threads). Not on air in the two nsys windows (no such call in either) |

Ruled out [BENCH GB10], each with a 100 ms single-thread kernel and plain (no conditional node) graphs: more work queued than 32 connections (64 streams), queue depth, cudaStreamSynchronize, host-pinned copies issued after a wait, discovery in the primary context instead of a green context, SM-wide kernel (64 blocks), RT stream priority. First launch of a kernel with a large local frame blocks the launch call once (5 calls, up to 72 ms; gone when the stack limit is pre-reserved, which lmem_prereserve() already does).

Link to the on-air data [MEASURED nsys gsV2N]:
- The first blocked launch of the 596 ms episode is a UL graph (k_gate_ul = conditional node). Its first kernel starts 596.5 ms after the call, i.e. only when refine_kernel ends. refine_kernel started 56 ms before the first blocked call; csirsDisc sat in the blocking cudaEventSynchronize for 652.6 ms. The release 0.2 to 1.1 ms after the kernel end is the waiter's wake-up.
- 44 of 45 (Vodafone) and 21 of 21 (TIM) graph launches > 5 ms overlap a contiguous busy period of the discovery streams (gaps < 0.3 ms); busy periods reach 652 ms (V2N) and 222 ms (T2N). The ~1 s episodes every ~5 min on Vodafone (UL FEP gpu_us 0.94 s at the same instant) are such a busy period: the same cause, not shown directly in a profile of that run. "Stall end matches CSI-RS output 5 of 47" is expected: any discovery kernel, also the PDCCH nID passes, ends a wait.
- Not reproduced in the bench: the host cudaGraphLaunch **call** blocking (bench calls stay < 0.4 ms). In the bench the graph is held on the device; on air the call blocked as well. Why is open (candidate: a later launch of a graph whose earlier launch is still held). The fix removes the hold, so it removes both; the launch-call time on air is the check.

Long discovery kernels in the two profiles (max / mean): refine_kernel 652 / 24 ms (1 thread), csirs sweep 146 / 1.0 ms, resident::segment 81 / 11-48 ms, resident::finish 56 / 6-35 ms (1 thread), lattice_screen 25 / 1-6 ms, normalize 16 ms, energies 8 ms / 1.9 ms mean, 6192 calls in 20 s (1 thread), PDCCH nID sweep 3.6 ms, cuFFT of the FEP batch 5 ms.

## 2. Fix (task 2)

1. **No interrupt-based wait in discovery** (TOOLS/nr_gpu_wait.h). All discovery host waits poll cudaEventQuery / cudaStreamQuery (8 quick polls, then 50 us sleeps): csirs disc_wait, csirs sweep complete() and pick_stream, PDCCH nID upload / statistics check, and the NID slice wait in nr_gpu_budget_cuda.h. The two events lose cudaEventBlockingSync. No new CPU core (the discovery thread sleeps).
2. **Bounded kernels** (TOOLS/nr_gpu_chunk.h). Every long discovery kernel runs in chunks of about 1 ms with the stream drained between chunks; the chunk size follows the measured time of the previous chunk, per call site (no constant to tune, same on sm_89 and sm_121). Bit-identical results because the blocks / units are independent and run in the same order:
   - blocks of normalize, pool, accumulate_kernel, lattice_pool, lattice_screen, segment, fep_unpack, fep_extract (block offset argument `bx0`, default 0);
   - csirs sweep over its candidate dimension (the existing `cand0`/`nctot` arguments);
   - refine_kernel over the existing unit range (`u0`,`u1`,`fin`) with the accumulators in device memory.
3. **Two single-thread kernels moved to the host**: resident::finish (merge + sort of the candidates; the same functions now `__host__ __device__`) and `energies` (the same `energy_score`, fed with the one or two symbol rows copied from the device grid). Single GPU threads ran these for up to 56 / 8 ms, 300 times per second for energies.
4. **Admission**, always on (no flag): before each chunk the existing RT counter (nr_gpu_budget_rt_active, the DL passes in NrGpuRtScope) is polled for at most 100 us. Measured benefit in the bench: none (always-busy RT threads, p99.9 97 to 77 us, max unchanged), so it is deliberately short; it costs at most 10 % of a chunk.
5. Not changed: cuFFT of the FEP batch (one plan call, ~0.7 to 5 ms), the PDCCH nID kernels (already event-sliced by the budget, max 3.6 ms), the pre-RX warm-up. Residual stall bound with the fix: about 5 ms.

## 3. Tests and bench (task 3)

Tests [GB10]: ctest after the change: only the known failures (ra0 x9, ssb_rate_match_prod and csi_rb_extent link, dft_test, test_nr_modulation, nr_td_sim_test); test_nr_spin_handoff failed once under -j4 load and passes alone (CPU timing test, unrelated). Discovery coverage that passed: test_nr_csirs_disc_sweep (GpuEqualsCpu, CumulativeGpuParity, GpuRefinementAndConfirmation: refine and confirm against the CPU reference), test_nr_csirs_disc_fp (resident detect against the CPU reference, 326 s; 262 to 270 s in earlier runs on other branches), _e2e, _td, _feed, _hyp, _rate, _dump, nr_csirs_blind_*. New: GpuChunk.CoversEveryUnitOnceInOrderAndBoundsChunkTime (every unit once, in order, chunk time bounded, 0 units, error pass-through).

Bench, real-time pass = launch of 3 graphs back to back, 5 threads, wait = discovery waiting for a kernel of the given length, repeated [BENCH GB10]. "before" = one kernel, blocking wait; "after" = the production chunker (nr_gpu_chunk.h) with the polling wait.

| kernel | | launch call p50 / p99 / max (us) | launch to done p50 / p99 / p99.9 / max | passes > 1 ms |
|---|---|---|---|---|
| 100 ms | before | 38 / 60 / 190 | 85 us / 114 us / 86.1 ms / 99.8 ms | 620 of 136 155 |
| 100 ms | after | 40 / 62 / 390 | 85 / 113 / 210 us / 0.78 ms | 0 of 227 263 |
| 300 ms | before | 39 / 60 / 192 | 85 us / 111 us / 269 ms / 299.8 ms | 220 of 103 196 |
| 300 ms | after | 40 / 61 / 313 | 82 / 104 / 211 us / 0.73 ms | 0 of 229 536 |
| 600 ms | before | 39 / 57 / 193 | 78 us / 101 us / 351 ms / 596 ms | 116 of 107 497 |
| 600 ms | after | 41 / 62 / 374 | 87 / 121 / 219 us / 1.01 ms | 1 of 227 386 |

The launch-call column is not blocked before either in the bench (see section 1); the stall shows in launch to done. Polling alone (no chunking): 600 ms kernel, 40 passes > 1 ms, max 5.0 ms. Chunking with a blocking wait: max 1.2 ms. Both: max 1.0 ms. Throughput of the discovery work is not measured here (chunk gaps add ~50 to 100 us per ~1 ms chunk).

## 4. OTA spec (task 6) [OTA UNVERIFIED]

Same environment as the gs/ab runs (FROZEN_20261018 env, this branch rebased on 3f0d9217fa), daytime. TIM 10 min, Vodafone 30 min, unprofiled. Pass criteria, per cell, never pooled:
- ULSTALL lines: gpu_us above 100 ms: 0 (before: 641 to 800 lines per run, ~0.94 s episodes on Vodafone every ~5 min).
- DECSLOW: no ~950 ms single-decoder waits; the others queue no more than their normal p99.
- EVMGRANT gq= / gp= fields: gp launch-call time p99 and max, and gspan > 10 ms share of grants (before 1.8 to 10.5 %; expect < 0.1 %), p99 of slot to result.
- Discovery still finds the same: CSI-RS discovery result lines (cell, layout, nID) and the time to first detection per cell not worse than a run of the previous build the same hour.
- If a short profile is wanted: nsys `-t cuda,nvtx --cuda-graph-trace=node --kill=none -y 60 -d 30` on a 150 s run (window ends 60 s before the stop), Vodafone.
