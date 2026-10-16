# Where DL/UL p99 becomes large — daytime OTA 2026-10-16 (single PCI)

Runs: TfE1-4, TaE1-4, VfE1-4, VaE1-4 (/home/nicola/NICOLA/ota_runs/ab_*E*_1_08*/09*), csd/int16 build_t26, t26fE / t26aE (= t26f / t26a + ISAC_E2E_TRACE=1), shuffled order.
Label for every number unless marked: [MEASURED, DGX spark-74c3 GB10 + X410, daytime OTA 2026-10-16 08:24-09:25, per run]. T and V never pooled; arms never pooled.
Analysis: tests/passive_rx/p99_bottleneck.py (a3019bb4bf). Written by the analysis agent (Sonnet); saved by the orchestrator.

## Conclusion
p99 is large because the FIRST DECODE BODY of heavy grants (TBS > 20 kbit: > 150 PRB and/or Nl >= 3; 19-50 % of real grants) takes 5-20 ms
(rarely 20-60 ms), and in bursts these bodies saturate the 5-consumer PDSCH pool so that every other grant queues up to the 9 ms stale limit.
In V with fE, inline recovery retries are a second, equally large contributor.
- 64-95 % of the grants above each run's p99 are heavy; 60-95 % of them are CRC-fail.
- Cost follows size, not CRC verdict (TfE4): TBS > 60 kbit first body p50 2.34 ms CRC-ok (n=75) vs 2.46 ms CRC-fail (n=2554); TBS > 200 kbit p50 6.4 ms, p99 52 ms; TBS <= 3 kbit p50 0.56 ms. > 150 PRB, TBS 200-300 kbit: Nl=2 p50 3.5 ms vs Nl=3 8.0 ms.
- Inside the body (PDTIMCLSST aggregates, all 16 runs), 1-5 ms real bodies: LDPC 42-64 %, demod 18-25 %, chest 12-23 %, FEP 9-14 %.
- DL GPU LDPC ~95 us wall (75 us GPU) per code block (interval regression of the GPU_EXACT_LDPC counters: TfE4 17 + 97 us/cb, VfE2 6 + 95 us/cb): a 250 kbit TB (~30 CBs) is ~3 ms of LDPC alone. [Aggregate, not a per-grant proof: per-grant stage times do not exist.]
- Queueing comes from these bodies, not from load: mean consumer utilisation 9-23 %; with >= 5 slot groups in flight 47-66 % of new groups wait > 2 ms vs 3-7 % with <= 2.
- Jobs older than 18 slots (9 ms) are dropped as stale [CODE nr_passive_sample_lifetime.h]: qwait never exceeds ~8.8 ms and 0.2-12.7 % of queued jobs never get a latency number; the true p99 is worse than the tables.

Estimated p99 after a change [REPLAY: FIFO replay of measured timings on 5 consumers; reproduces measured p99 within 2-14 %]:
| Change | Real p99 |
|---|---|
| heavy real grants removed | T 5.2-6.9 ms, V 7.2-12.8 ms |
| 10 consumers instead of 5 | TfE4 14.8 -> 11.5 ms (T fE 9.0-11.5): the single body is the limit, not the pool |
| all solver probes removed | -0.2 to -2.7 ms (<= 10 %) |
The 5-7 ms floor left without heavy grants = PDCCH scan latency (arr2enq p50 0.6, p99 1.8-4.6 ms) + GPU FEP group wait + a ~0.5 ms body.

## Ranked contributors (share of grants above each run's p99 whose largest component is X; median of 4 runs)
| Arm | body | qwait | extra bodies | pre | arr2enq |
|---|---|---|---|---|---|
| T fE | 53 | 29 | 9 | 3 | 4 |
| T aE | 62 | 23 | 0 | 6 | 5 |
| V fE | 44 | 12 | 36 | 15 | 0 |
| V aE | 59 | 25 | 3 | 15 | 0 |
1. First body of heavy grants. 2. Queue wait behind them (dominant in 44-47 % of grants > 5 ms). 3. Inline recovery (fE only; V multi-body 53-63 % fE vs 4-8 % aE). 4. Slot-group serialisation: one consumer decodes all jobs of a slot serially, widest first [CODE nr_pdsch_passive_queue.c ~1500-1600, 1729]; pre of non-first members correlates 0.99-1.00 with earlier members' decode time. 5. PDCCH scan latency (floor). 6. Global stalls (p99.9/max/stale, not p99).

## What the numbers measure [CODE, openair1/PHY/NR_UE_TRANSPORT]
Wall clock (CLOCK_MONOTONIC). EVMGRANT e2e (nr_pdsch_passive_queue.c ~2166): a2e = slot read -> job enqueued; qwait = enqueue -> slot group dequeued (group level); pre = dequeue -> decode start (IQ copy, stale check, GPU FEP group, and for non-first members the earlier members' decodes); dec = decode call incl. retries; first = first body; tot = arrival -> decode end (-1 if > 64 ms old: 0.1-0.7 %, excluded). Only evm_valid decodes are logged. UL_E2E: arr2deq, wall, tot, cpu_us; pick=1 = probe-ring jobs. PDTIMCLS: histogram of fep+chest+demod+ldpc per body per class (real/retry/probe); PDTIMCLSST: stage ms per class and body band; stage walls include GPU wait (poll sleep 200 us). PDTIMSLOW/DECSLOW: per-call details for > 20 ms (capped 200/300 lines).

## Per-run table (us)
| Run | tot99 | tot99.9 | heavy % | CRC ok % | stale % | stall clusters / s | sim_noheavy99 |
|---|---|---|---|---|---|---|---|
| TfE1 | 10541 | 51521 | 20.3 | 66.4 | 0.2 | 1 / 0.8 | 6138 |
| TfE2 | 9700 | 35600 | 29.2 | 66.4 | 0.3 | 0 / 0.0 | 6105 |
| TfE3 | 11362 | 20619 | 45.9 | 32.3 | 0.2 | 1 / 0.2 | 6724 |
| TfE4 | 14806 | 48015 | 32.1 | 48.4 | 1.5 | 4 / 3.2 | 6415 |
| TaE1 | 13653 | 29790 | 43.5 | 38.6 | 0.5 | 1 / 0.2 | 6535 |
| TaE2 | 26676 | 49141 | 50.0 | 48.0 | 6.7 | 1 / 1.6 | 6854 |
| TaE3 | 9525 | 38450 | 20.9 | 55.9 | 0.4 | 0 / 0.0 | 5151 |
| TaE4 | 8086 | 18806 | 28.9 | 53.2 | 0.2 | 0 / 0.0 | 5314 |
| VfE1 | 17348 | 51760 | 19.1 | 71.4 | 2.6 | 9 / 2.6 | 7360 |
| VfE2 | 23687 | 53812 | 39.5 | 57.2 | 12.7 | 17 / 4.5 | 12792 |
| VfE3 | 19019 | 45792 | 31.9 | 72.1 | 3.8 | 12 / 3.8 | 9348 |
| VfE4 | 19514 | 52816 | 34.5 | 67.3 | 4.8 | 8 / 1.8 | 9188 |
| VaE1 | 18778 | 52427 | 36.6 | 46.0 | 8.1 | 18 / 3.9 | 8275 |
| VaE2 | 14799 | 49434 | 42.3 | 39.0 | 2.9 | 10 / 2.1 | 7189 |
| VaE3 | 14072 | 47320 | 26.9 | 46.8 | 3.3 | 13 / 3.4 | 7953 |
| VaE4 | 13009 | 31534 | 28.8 | 48.8 | 1.6 | 11 / 2.6 | 5621 |

Same run order (TfE1-4, TaE1-4, VfE1-4, VaE1-4):
- real grants: 10418 11280 17635 14403 21704 25182 16015 11987 36355 49481 35730 40008 46178 46970 47091 27167
- probes: 73415 79065 71705 57047 87140 53841 64105 69827 11618 9980 12540 8072 13850 10928 8979 12825
- a2e p99: 4103 4512 4566 4042 3878 3545 4080 3621 2862 3553 2999 1843 2385 2586 3538 2460
- qwait p99: 3092 2785 4491 7709 7471 8151 2246 1605 6999 8084 7809 8033 8061 7832 7274 6636
- pre p99: 1327 1432 1605 1892 2195 2092 546 1371 1501 6750 4848 5752 3855 3109 3778 1805
- first p99: 6825 5942 6393 9665 7140 20046 5665 5858 8018 9105 6999 7563 12047 8136 8223 8152
- multi-body %: 10.9 8.6 11.9 9.4 0 0 0 0 57.4 63.1 52.8 56.6 5.3 4.4 5.2 7.8
- p99_heavy %: 91 92 89 93 86 95 85 80 84 78 83 81 75 84 64 80
- p99_crcfail %: 86 82 90 95 88 91 79 71 82 77 80 73 71 84 60 78
- sim_all99: 10087 9492 11022 14001 13007 25867 9320 7772 16110 22683 18422 18936 17184 13387 12756 12167
- sim_noprobe99: 9388 9006 10055 12573 10896 24398 8281 7554 15919 22252 18008 18711 16698 13284 12647 11987
- sim_10cons99: 9007 8538 9338 11522 8887 20421 7571 7395 14660 18636 14648 15645 12627 9589 10648 9790
- GPU discovery slices > 100 ms: T 7-38 per run, V 99-129
- UL real jobs: 0 135 10830 7540 0 3410 0 190 62454 36722 53545 50154 53151 21615 76075 42176; UL CRC-ok 0-7 per run; UL real wall p99 13.5-26.1 ms; UL arr2deq p99 10.6-51.5 ms

## Candidate explanations tested
- Solver probes (4:1 over real in T): NOT the p99 driver (66 % of DL decode wall in TfE4, 5 % in VfE2; removal -10 % at most; light-grant p99 vs probes/s quartile has no consistent sign; per-second correlation 0.06 / -0.06).
- CRC-fail grants: 26-62 % of decode wall and 60-95 % of p99 grants, but the cost is size: CRC-ok grants of equal TBS cost the same.
- GPU vs CPU: (a) typical 1-20 ms bodies: GPU+CPU, no per-grant split; (b) > 20 ms CPU-bound bodies: GPU chest refused (reason 8, rows > NR_GPU_CHEST_MAX_ROWS 2048), CPU chest 36-40 ms (46-163 refusals per 66-90 k calls, 0.1-0.5 % of real grants: beyond p99.5); A725 consumers (cores 3,4) 15-20 % slower per PRB*Nl than X925 (7,8,9); (c) blocked bodies (cpu < 30 % of wall, GPU poll): see global stalls.
- Core sharing: env pins disjoint; ivcsw > 0 in 47/229 DECSLOW calls (TfE4). Unpinned threads (producer, PDCCH scan, janon) not tested.
- Page migration / kworker: not implicated (majflt 0, minflt <= 800, no FATTAILS lines). Log volume not tested.
- PDCCH scan: the floor (arr2enq p99 1.8-4.6 ms); scan queue drops drop_full 477 / drop_stale 1322 (TfE4), 131 / 858 (VfE2).
- First body is not bimodal: log-normal-like scaled by TBS and Nl; sparse 25-150 ms mode = CPU chest refusals + global stalls.
- CSI-RS discovery / DM-RS scan / janon: no timestamps, not correlated.

## Global stalls
All consumers released in the same ms after 0.1-1.9 s (TfE4 t=53264.925: four decodes 1.89-1.92 s, 80-85 k voluntary switches, 0.17-0.24 s cpu). Clusters (>= 3 consumers each >= 100 ms): T 0-4 per run (0-3.2 s), V 8-18 (1.8-4.5 s, lower bound: DECSLOW capped). ~1/3 end at the same instant as a PDCCH-NID discovery GPU slice (222-236 ms). [INFERRED] GPU-wide pause; hog vs victim not decided. They drive p99.9/max/stale, not p99. UL shows the same signature (ULSTALL gpu_us dominant in 100/139 FEP, 33/72 LDPC stalls TfE4; 327/349 FEP VfE2).

## fE vs aE
- V: aE ~25 % better: fE tot p99 17.3/23.7/19.0/19.5 (median 19.3) vs aE 18.8/14.8/14.1/13.0 (median 14.4); extra-body share of p99 27-43 % -> 2-4 %; stale 2.6-12.7 % -> 1.6-8.1 %; stalls unchanged.
- T: no reliable difference (fE median 11.0, aE 11.6; TaE2 outlier 50 % heavy, 6.7 % stale).
- p99.9 18.8-53.8 ms everywhere.

## UL
Real UL jobs essentially always fail CRC (0-7 ok per run of 0-76 k). Slow UL decodes are CPU-bound (wall p99 13.5-26.1 ms, cpu/wall 0.87-0.94; TfE4 PRB > 60 wall p99 34.8 ms). The 2-consumer UL pool (cores 0,1, A725) queues behind itself: arr2deq p99 10.6-51.5 ms; VfE2 80 % of UL tot > 5 ms is queue wait. GPU bypass: UL_GPU_LDPC over_cap 5639 + cpu_refused 6105 of 71 k TBs, UL_GPU_FEP over_cap 699 of 113 k (TfE4) [INFERRED link to the 20-35 ms CPU decodes].

## Caveats
No per-grant stage times; stale-dropped jobs and scan-queue drops absent from percentiles; replay ignores stale drops, GPU contention, wake order; slot axis +-0.6 ms; DECSLOW/PDTIMSLOW caps; arms confounded by load (n=4 per arm, no significance test).

Reproduce: `python3 -I tests/passive_rx/p99_bottleneck.py <run>/run.log > rep/<run>.txt`; `python3 -I tests/passive_rx/p99_bottleneck.py --table rep/*.txt`.
