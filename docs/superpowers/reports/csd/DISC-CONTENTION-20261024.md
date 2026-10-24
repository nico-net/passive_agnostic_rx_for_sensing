# Discovery GPU slowness: it is compute, not contention (2026-10-24)

Host: DGX Spark (GB10, 48 SMs), all runs through heavy.sh (one heavy job at a time, so no other agent's job shared the GPU), 4 RX, 217 PRB, SPEED=1, FROZEN_20261018 env minus radio flags, cfg sd_t26ul. Branch csd/disc-contention off main e9c874c308. Arms: base = main build (e9c874c308); new = main + this branch's libnr_csirs_disc_gpu_sweep.so only (every other binary identical). Captures never pooled: V = raw_pci415_12s_b (Vodafone), T = raw_pci923_tim_12s_tile3 (TIM). Driver: tests/passive_rx/disc_contention_replay.sh. Labels: [MEASURED, DGX replay V|T], [MEASURED, DGX idle bench], [INFERRED].

## 1. Where the on-air time goes

Temporary stderr line in run() (not committed) printed the real sweep parameters of every call over 50 ms. Result [MEASURED, DGX replay V, base]:

| hypothesis row | ports | n_re | slots x RX | host time of 1024-ID call | GPU time of the last 32-ID chunk |
|---|---|---|---|---|---|
| 12 | 16 | 13 888 | 4 x 4 | 51-67 ms | 0.6-1.5 ms |
| 17 | 32 | 27 776 | 4 x 4 | 100-117 ms | 3.0-3.2 ms |
| 18 | 32 | 55 552 | 4 x 4 | 206-221 ms | 6.3-6.6 ms |

The call time is linear in n_re (4x the REs, 4x the time). That is the signature of a kernel doing work, not of waiting. The 10-22 idle bench (DISC-GPU-TIMING-20261022) used a 651-RE map, 85x smaller than row 18, which is why it saw 3.5 ms and read the on-air 212 ms as contention.

Proof on an idle GPU [MEASURED, DGX idle bench, tests/passive_rx/gpuspan/disc_sweep_test.cu]: the same kernel with the row-18 shape (32 ports x 1736 RE, 4 slots, 4 RX, 1024 IDs) takes 212.05 ms with nothing else running; replay on air-like load: 206-221 ms. So the contention term is zero within noise for c_sweep. The cause of the slowness is the kernel itself: lane L owns the block of 32 REs starting at 32 L, so every warp load of map.re[] and rx[] touched 32 separate lines (L1-wavefront bound, about 8 % of the issue rate) [INFERRED from the 17 ms ideal instruction time vs 212 ms].

c_refine: 16 chunks x 16 ms (32 units x 0.5 ms) = 260 ms per call on replay, equal to the idle 0.46 ms/unit serial cost [MEASURED]. Also pure serial compute: a one-thread kernel, not contention. Because the chunk floor is 32 units, each chunk was a 16-23 ms single-thread kernel, which is exactly the long-kernel pattern that holds RT body launches (nr_gpu_chunk.h).

c_seg [MEASURED, DGX replay V and T, 4 base runs]: no c_seg CHUNK_SLOW line at all with the min-32 chunking (only c_sweep and c_refine appear), so the 10-22 chunk minimum holds under replay load. Not changed. The OTA c_seg/c_ls CHUNK_SLOW counts of the pre-min-32 builds are not reproduced here.

GPU priorities, green-context sizing and RT-idle gating were not pursued: they address a contention term that measured zero for c_sweep and c_refine. nsys was attempted (nsys 2025.3 -t cuda around the receiver) but produced no report (the wrapper was killed on SIGINT shutdown); the kernel-time evidence above did not need it. The OTA wait could still contain contention [INFERRED, not excluded]: replay has no radio, and OTA load differs.

## 2. Fixes

1. c_refine parallel (nr_csirs_disc_gpu_sweep.cu): one 128-thread CTA per unit (refine_extent_units, refine_trs_units), shared-memory exact integer sums (doubles holding integers, so atomic order is irrelevant), per-unit terms to a buffer, and refine_accumulate adds them to the evidence in unit order; the tail (span search) is the unchanged serial kernel run with an empty unit range. Bit-identical [MEASURED]: tests/passive_rx/gpuspan/disc_refine_test.cu compares accumulators (memcmp), result and refined hypothesis against the original refine_kernel for row 4 (densities 0/1/2, 1-4 ports, 51/106/217 RB, device and mapped ring, invalid and off-period slots) and the row-1 TRS branch, at chunkings 1, 3, 32 and whole: all BIT-EXACT. Idle time: 151 ms serial to 0.21 ms (304 units), 165 to 0.33 ms (416 units). Oversized jobs (more units than the term buffer) fall back to the serial kernel.
2. c_sweep staged loads (same file): for block_re == 32 each warp loads 8-RE tiles of its 32 blocks coalesced into shared memory laid out [k][block] (stride 33) and each lane reads its own block; the per-lane float operations and their order are unchanged. Bit-identical [MEASURED]: tests/passive_rx/gpuspan/disc_sweep_test.cu against the verbatim original kernel (disc_sweep_ref.inc): 7 shapes incl. a partial block and a block_re=16 fallback, all BIT-EXACT. Idle time for row 18, 1024 IDs: 212.05 ms to 57.4 ms (3.7x); row 12: 3.03 to 0.95 ms per 64 IDs. Chunking is unchanged (the min-32 floor now yields chunks of about 1.8 ms at row 18 instead of 6.3 ms).
3. No new flag, no config change, GPU-portable (plain CUDA, shared memory, no SM assumptions).

Not done: further c_sweep speedup (57 ms is still above the 1 ms chunk target for the largest rows; remaining cost is the per-RE gold lookup and arithmetic); c_seg per-CTA work (not a bottleneck on replay).

## 3. Replay A/B [MEASURED, DGX replay, SPEED=1, 2 reps per arm, interleaved base/new per capture]

V runs DUR 75 s and T runs DUR 110 s; every table row below consumed the whole capture (1.4745e9 samples), except V base_1 (1.449e9). Batch = CSIRS_DISC BATCH line. real p99 = PDTIMCLS real-grant p99 (us), sum p99 = PDTIMPCT sum p99 (us), final counters of the run.

| cap | arm | rep | batches | CHUNK_SLOW (c_sweep/c_refine) | batch mean / max (ms) | first CONFIRMED (slots) | DL sum p99 (us) | real p99 (us) | PDSCHQ drops full/stale | PDSCHQ crc_ok/decoded |
|---|---|---|---|---|---|---|---|---|---|---|
| V | base | 1 | 4 | 18 (10/8) | 382 / 1093 | 6 | 705 | 15 | 0/0 | 2255/8190 |
| V | base | 2 | 4 | 12 (6/6) | 362 / 1160 | 20 | 695 | 245 | 0/0 | 1969/8612 |
| V | new | 1 | 4 | 0 | 140 / 285 | 6 | 735 | 295 | 0/0 | 1311/5723 |
| V | new | 2 | 4 | 0 | 139 / 299 | 6 | 675 | 15 | 0/0 | 2229/8139 |
| T | base | 1 | 6 | 8 (0/8) | 309 / 1515 | 16 | 735 | 315 | 0/0 | 3698/7646 |
| T | base | 2 | 2 | 18 (0/18) | 703 / 1380 | 28 | 795 | 395 | 0/0 | 4130/7805 |
| T | new | 1 | 7 | 0 | 123 / 292 | 24 | 795 | 345 | 0/0 | 4051/8170 |
| T | new | 2 | 7 | 0 | 71 / 246 | 22 | 755 | 405 | 0/0 | 3735/8230 |

Reading, per criterion:
- CHUNK_SLOW: 12-18 per run to 0 on both captures.
- Mean and max discovery batch time: V 362-382 to 139-140 ms mean, 1.1 s to 0.3 s max; T 309-703 to 71-123 ms mean, 1.4-1.5 s to 0.25-0.29 s max. The number of batches is bounded by the 12 s capture: V 4 vs 4 (no change); T 2-6 vs 7. Batch counts per run are small, so the "batches per run" gain is only visible on T here; the OTA run (minutes long) is where it should show.
- Time to first CONFIRMED CSI-RS (slots): V 6/20 vs 6/6, T 16/28 vs 24/22: not worse within run-to-run spread (the two base reps already differ by 12 slots).
- RT DL sum p99: V 695-705 vs 675-735, T 735-795 vs 755-795: no difference. real-grant p99 swings 15-405 us between reps of the same arm, no arm effect.
- PDSCHQ drops: 0 in all 8 runs. The crc_ok/decoded counts differ between reps of the same arm with the number of grants processed; not an arm effect [INFERRED: V new_1 decoded 5723 vs 8139-8612 elsewhere; the run was slower overall, its sum n was 8600 vs 12200-12800; one run, not explained].
- Discovered IDs: V {249, 380, 415, 582, 713, 748} in all four V runs; T {923} in all four T runs. The individual MONITOR_ADD resource sets vary from run to run (base_1 vs base_2: 8/11 differing entries on V, 13/0 on T), the same amount as base vs new, so set identity at that granularity is not testable on replay; the ID sets are identical. No CORESET discovery line exists in these logs to compare.

Run logs (not committed): scratchpad session dirs V_*/T_* under the agent scratchpad; command: `CAP=.. CAPC=.. RXC=.. SSB=.. FO=.. B=<dir> OUT=<dir> DUR=75 tests/passive_rx/disc_contention_replay.sh V <arm>` (V: CAPC=RXC=3680010000 SSB=1171 FO=-4850; T: CAPC=3731910000 RXC=3759990000 SSB=233 FO=-4836).

## 4. OTA A/B for the orchestrator

- Arms: A = main e9c874c308 build; B = this branch (csd/disc-contention) build. Same FROZEN_20261018.env, interleaved A/B/A/B, n >= 3 per cell, daytime (the NO-OTA-at-night rule).
- Cells: Vodafone PCI 415 and TIM PCI 923 first (replayed here), then the other two operators; never pooled.
- Metrics (per run, from run.log): (1) count of `CHUNK_SLOW site=c_sweep` and `site=c_refine` lines: expect 0 for B; (2) `CSIRS_DISC BATCH` count and mean/max total_us (pass criterion of DISC-CONFIRM-20261021 section 7: no batch above 3 s); (3) FEP slots and feed_drops against A; (4) time to first CSIRS_BLIND CONFIRMED and number of CONFIRMED / MONITOR_ADD; (5) DL dec/tot p50/p99 (PDTIMPCT sum, PDTIMCLS real); (6) PDSCHQ dropped[full,stale] and crc_ok %. A `CHUNK_SLOW site=c_seg` count in B tells whether the remaining OTA slowness is c_seg or real contention.
- If B still shows c_sweep slow lines on air with last_chunk_gpu_us far above 2 ms for the large rows, that is genuine contention and the priority / RT-idle options (section 1) become the next step.
