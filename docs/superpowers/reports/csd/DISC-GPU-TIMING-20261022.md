# Discovery GPU timing: one chunk on an idle GPU (2026-10-22)

Question: is the OTA slowness of the chunked CSI-RS discovery (CHUNK_SLOW: median 646 ms per call, last_chunk_gpu_us median 33 ms, p90 54 ms; run ab_aVF2_1_004741) GPU contention or slow kernels?
Host: DGX Spark spark-74c3 (GB10, 48 SMs), idle (no nr-uesoftmodem, GPU 0 %), branch csd/int16-disctime off csd/int16-floatpool @199893c892. Evidence labels: [MEASURED, DGX bench idle] unless stated.

## What one unit is (code, [READ])

All three sites run on the discovery stream: priority 0 (default, no priority set anywhere), non-blocking, inside the discovery green context of 8 SMs (nr_gpu_green_stream(.., rt=false); GB10 split: discovery 8 SMs, RT 40 SMs: DL 32 / UL up to 40 / probes 8). main (rr-main-merge e7456dbd7d) uses the same stream and green context, also priority 0, and launches the same kernels once per call, unchunked. So the chunking, not the stream or SM limit, is what differs from main.

| site | unit | kernel, grid / block | main launches |
|---|---|---|---|
| c_seg (nr_csirs_disc_gpu_cluster.cuh, resident::segment) | one 32-thread CTA = 32 (offset,symbol) threads; each thread loops over all RBs and densities with ~21 KB local frame; total = ceil(p*14/32), 9 for p=20, 280 for p=640 | segment<<<k,32>>> | one launch of all units |
| c_sweep (nr_csirs_disc_gpu_sweep.cu, sweep) | one scrambling-ID candidate x all slots = (1 x S) CTAs of 128 threads; total = 1024 candidates | sweep<<<(n,slots),128>>> | one launch (1024, slots) |
| c_refine (same file, refine_kernel) | one (valid slot, antenna, port) triple; total = slots x ant x ports (512 here) | refine_kernel<<<1,1>>> (ONE thread, doubles, ring read from mapped host memory) | one launch of the whole job |

## Bench

tests/passive_rx/gpuspan/disc_unit_bench.cu (new; it #includes the production .cu so the real kernels, stream and green context are used; no production change). Sizes: 217 PRB, 4 RX, 8 slots, 1024 IDs, 651 RE (sweep); 32 slots x 4 RX x 4 ports = 512 units (refine); p = 20 / 80 / 640 (seg). Times are cudaEvent GPU times, median of 8-15 runs after warm-up. Load modes: RTgrn = memory-streaming kernel flood in the real-time green context (40 SMs); plain = same flood on a full-GPU stream; disc100 = a 100 ms 8-CTA kernel in the discovery green context on another stream. Seg data is synthetic (random history, all masks set), so seg times are indicative; the kernel cost depends on the masks (see variants).

## Results (ms)

| site | units | (a) one chunk at per_end | (a) one unit | (b) whole call chunked as F (idle) | (c) whole call one launch (main, idle) | (d) chunk under load |
|---|---|---|---|---|---|---|
| c_sweep | 1024 | 0.062 (16 IDs = 128 CTAs) | 0.009 | 3.8 (5 chunks) | 3.5 | RTgrn 0.48, plain 1.29, disc100 **94.9** |
| c_refine | 512 | 3.6 (8 units); 5.9 (per_end 2, as the chunker settles) | 0.46 | 234 (64 chunks) | 231 | RTgrn 25 (7x), plain 73 (20x), disc100 3.6 (no effect) |
| c_seg, one mask per tile, p=20 | 9 | 23.4 (1 unit) | 23.4 | 213 | 27.5 | RTgrn 88, plain 88 (queue stall, see below) |
| c_seg, one mask, p=80 | 35 | 23.0 | 23.0 | 816 | 34.3 | RTgrn 98 (launch, not chunk) |
| c_seg, one mask, p=640 | 280 | 17.9 | 17.9 | 1215 (64-chunk cap) | 123.8 | RTgrn 187 (one launch) |
| c_seg, all six masks, p=20 | 9 | 125.4 | 125.4 | 1131 | 142.9 | RTgrn 190 |
| c_seg, all six masks, p=80 | 35 | 125.3 | 125.3 | 4387 | 187.4 | RTgrn 251 |

Extra c_seg point: 32 units in one launch cost 27.3 ms (p=640) and 33.4 ms (p=80), against 17.9 / 23.0 ms for one unit: one CTA per unit runs in parallel across the 8 SMs.

## Findings

1. c_sweep is NOT a slow kernel [MEASURED]: a 16-ID chunk takes 0.06 ms idle, the whole call 3.5-3.8 ms either way (chunked or not). The OTA value (210 ms for 64 chunks, last chunk tens of ms) is 3 orders of magnitude above it, so it is waiting, not computing. In the bench the only thing that produced a number of that size is another long kernel in the SAME discovery green context: a sweep chunk waits for the whole remaining run time of a 100 ms kernel of another stream (94.9 ms; with a 21 KB local frame the same). RT-side load costs only 0.4-1.3 ms for the same chunk. [MEASURED] for the effect, mechanism [INFERRED] (the discovery SMs are used by kernels of the other discovery streams: c_seg 23-125 ms per unit, the FEP, PDCCH NID).
2. c_refine is slow by construction [MEASURED]: one thread, 0.46 ms per unit, 231 ms for the call on an idle GPU, with or without chunking. Under a memory flood it is 7x (RT-side) to 20x (full GPU) slower per chunk, because it reads the ring through mapped host memory; this is the one site where RT-side load matters. An idle 8-unit chunk (3.6 ms) is far below OTA's 268 ms, so OTA saw contention here, but main would also be slow (231 ms idle).
3. c_seg is a slow kernel, and chunking makes it much worse [MEASURED]: one unit (one 32-thread CTA) is 18-125 ms alone, depending on how many masks are set; this is already above the 33 ms OTA median, so (a) is "close to / above 33 ms" = SLOW KERNEL. The chunker cannot cut below one unit, so it launches 1 CTA per 23-125 ms kernel, one after the other, while 7 of the 8 discovery SMs idle: whole call 213 ms (p=20) / 816 ms (p=80) / 1215 ms (p=280 units, 64-chunk cap) against 27 / 34 / 124 ms as one launch. That is 8x-24x overhead from the chunking alone, idle GPU. OTA 238 ms for 9 units is within the 27-143 ms idle range plus load.
4. Under the flood load every c_seg launch is delayed by about 64 ms (unit 23 -> 88, 125 -> 190), i.e. roughly the whole queued flood, as in the launch-hold effect documented in nr_gpu_chunk.h; the flood is a back-to-back queue of 60 kernels, so treat the value as an upper bound, not as a measure of real RT load. [MEASURED effect, magnitude test-specific.]

## Verdict

- c_sweep: CONTENTION ((a) 0.06 ms against 33 ms). The contender is mostly the long c_seg/c_refine kernels of the other discovery streams on the same 8 SMs, not the RT graphs. [MEASURED + INFERRED]
- c_seg: SLOW KERNEL ((a) 18-125 ms, above the 33 ms median), aggravated 8x-24x by serial 1-CTA chunks. [MEASURED]
- c_refine: SLOW KERNEL by design (single thread, 231 ms), plus load sensitivity through host-memory reads. [MEASURED]
- So "OTA slowness is GPU contention or slow kernels" has no single answer: the two dominant calls (seg, refine) are slow kernels, and the chunking multiplies seg by the number of units; the sweep is fast and is only slow when queued behind them.

## Recommended single fix

Floor the c_seg chunk at 32 units (one launch for p <= 80, ceil(units/32) launches above) instead of adapting down to 1. Measured idle: p=640 280 units = 9 chunks of about 27 ms = about 250 ms against 1215 ms now; p=80 one 33 ms launch against 816 ms; no kernel is longer than the one-unit kernel already is (18-23 ms) plus about 10 ms. Implementation: a minimum `per` in NrGpuChunker (or max(per,32) at the c_seg call site), no new flag. Expected effect: the discovery SMs are busy 4-30x less long per call, so sweep and refine chunks queue behind far less seg work. Not measured: the RT-side latency effect of 27 ms seg kernels versus 23 ms ones (should be nil, same order), and OTA. Check with one OTA CHUNK_SLOW run.

(Not recommended first: raising the global target, or a priority change. Priority is irrelevant to the dominant effect: a running kernel is not preempted, and the contenders are in the same green context.)

## Caveats

- Seg data are synthetic; the number of set masks moves the kernel by 5x (23 vs 125 ms). The OTA median 33 ms matches the light end.
- The load kernels are memory floods, not the real RT graphs. The disc100 mode is a spin kernel, not a real seg kernel.
- c_refine is shown with ring in mapped host memory (as production); the OTA per-unit cost (33 ms) is 70x the idle bench value (0.46 ms), so OTA refine had either more REs per port or heavy host-memory contention; not resolved here.
- The first seg run (all modes, all variants) was cut by the 550 s timeout after the all-masks p=640 unit line; remaining rows were not needed.
