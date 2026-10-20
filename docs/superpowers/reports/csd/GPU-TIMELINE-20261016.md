# GPU timeline: replay traces and nsys overhead (stall plan Task 2), 2026-10-16

Sonnet agent; saved by the orchestrator. Binary b14c35eeb4, env t26aXE. Label [MEASURED, DGX replay <cap>, t26aXE, nsys, n=1]; V = raw_pci415_12s_b, T = raw_pci923_tim_12s_tile3, never pooled. Dirs under /home/nicola/NICOLA/wt/rr-csd-int16/replays/: V plain replay_stallV_plain_173829; V nsys cuda,osrt replay_stallV_nsys_173909 (NOT representative); V nsys cuda-only replay_stallV_nsysC_174314 (used); T plain replay_stallT_plain2_174111 (the first T plain run overlapped an export and was discarded); T nsys cuda,osrt replay_stallT_nsys_174147. Helper for kernel max / timeline: /tmp/claude-1001/q.py (not in repo).

## Plain vs nsys (DL tot over real grants, us)
| run | n_real | p50 | p99 | p99.9 | p50 vs plain | representative |
|---|---|---|---|---|---|---|
| V plain | 4315 | 1854 | 9207 | 31410 | | |
| V nsys cuda,osrt | 2807 | 2557 | 60409 | 464635 | +38 % | NO |
| V nsys cuda | 1857 | 1966 | 16376 | 47153 | +6 % | yes (p50) |
| T plain | 2962 | 1380 | 8931 | 27549 | | |
| T nsys cuda,osrt | 4484 | 1605 | 12865 | 42919 | +16 % | yes (p50) |
nsys raises p99 1.4-1.8x: stalls found are a lower bound. n_real varies a lot between runs of the same capture.

## GPU busy share per launching thread class (window V 23.1 s, T 22.6 s)
| class | V | T |
|---|---|---|
| csirsDisc (CSI-RS discovery, background) | 53.1 % | 39.8 % |
| nr-uesoftmodem (NID sweep/score, background) | 10.8 % | 7.8 % |
| passivePdsch (RT) | 4.0 % | 9.5 % |
| pdschRecov | 2.4 % | 1.6 % |
| passivePusch | 0.1 % | 1.2 % |
| union | 59.6 % | 49.9 % |
Launch->start p50/p99 (us): csirsDisc 78/5164 (V), 106/27558 (T); passivePdsch 18.5/1742 (V), 17.2/1729 (T); pdschRecov 6/1501, 23/1903; passivePusch -4/2404, -4/2001.

## Stalls >= 50 ms (sysan_nsys.py --stalls)
V: passivePdsch1 k_evm_zero 194 ms behind csirsDisc `sweep` (blocker csirsDisc cudaEventSynchronize 253 ms); passivePdsch4 k_gather 103 ms and k_evm_zero 103 ms behind `sweep` (csirsDisc 128 ms, nr-uesoftmodem 126 ms cudaEventSynchronize). T: passivePdsch4 k_evm_zero 228 ms behind csirsDisc `refine_kernel` (csirsDisc cudaEventSynchronize 379 ms). No cudaMalloc/cudaFree in any stall.

## Longest kernel per class (max, p99)
| class | V | T |
|---|---|---|
| csirsDisc | 787 ms refine_kernel, p99 6.0 ms | 1146 ms refine_kernel, p99 16.2 ms |
| NID sweep | 8.05 ms sweep, p99 0.50 ms | 6.65 ms, p99 0.46 ms |
| passivePdsch | 3.06 ms k_delay_exact, p99 0.16 ms | 2.99 ms k_ldpc4_t, p99 0.19 ms |
| pdschRecov | 2.96 ms, p99 0.14 ms | 2.96 ms, p99 0.26 ms |
| passivePusch | 0.89 ms, p99 0.41 ms | 3.39 ms, p99 0.34 ms |
csirsDisc top kernels V: sweep 3234 ms total, energies 2821, refine_kernel 2576, segment 2267; T: refine_kernel 3894 ms. No dmrsIdBg / gpuCheckBg kernels in these replays.

## Representative PDSCH burst (median length; consecutive kernels of one consumer with gaps < 3 ms)
V: 3 grants, 51 kernels, 2.97 ms, first k_ldpc4_t at +150 us, 3 LDPC kernels 233 us total. T: 7 grants, 119 kernels, 3.20 ms, first k_ldpc4_t at +612 us, 7 LDPC kernels 531 us. Kernel time per grant (k_ls -> last LDPC) well under 1 ms; the rest of the burst is grants decoded in series.

## Cause
The 100-230 ms replay stalls are CSI-RS discovery background kernels (refine_kernel up to 1.15 s, sweep) holding the GPU while a PDSCH kernel waits; csirsDisc alone occupies 40-53 % of GPU time. The blocking API is csirsDisc's own cudaEventSynchronize (waiting for its kernels), not a host lock. Not shown: why this blocks the RT consumer instead of sharing SMs (whether csirsDisc really runs on the 8-SM partition, or the partition does not isolate). Replay stalls do not reach the on-air 0.1-1.9 s (replay has no DPDK/USRP load; on-air trace = Task 4).
Separating test: replay with refine_kernel/sweep capped (chunked) below X vs uncapped, p99.9 and stall count; ISAC_CSIRS_DISC=0 is not a clean arm (async recovery and the enqueue stamp depend on it).
=> X must be set from the csirsDisc kernels (NID kernels are fine: max 8 ms, p99 < 0.5 ms).

## Not checked
Stalls 10-50 ms not counted; timeline is a merged burst; n=1 per trace; helper script not in the repo.
