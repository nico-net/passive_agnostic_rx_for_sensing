# On-air A/B: CUDA_DEVICE_MAX_CONNECTIONS=32, 2026-10-16

Runs 22:21-22:51 CEST (LOW LOAD label), nsys -t cuda --delay 40 -d 90, X = t26aXE (default 8 connections) vs M = t26aXME (+ CUDA_DEVICE_MAX_CONNECTIONS=32), n=2 per arm per cell, interleaved; runner rr-orchestration/ota/run_m_20261016.sh. Sonnet agent analysis, saved by the orchestrator. All 8 runs synced, SIGINT exit.

**Summary**
1. M removes almost all >= 50 ms RT stalls: V X 38 / 36 -> M 8 / 0; T X 21 / 29 -> M 0 / 0 per 90 s. No overlap between arms. [MEASURED, DGX OTA 2026-10-16 LOW LOAD, V|T, nsys, n=2 per arm]
2. Background load comparable (csirsDisc busy and long-kernel counts); normalised stalls per 100 csirsDisc kernels >= 50 ms: V X 19.4/10.6, M 4.0/0; T X 9.3/9.5, M 0/0.
3. Residual waits of 10-20 ms remain in M (>= 10 ms events V 45/25, T 6/52): HoL is bounded, not gone.
4. Head-of-line blocking on shared hardware queues is confirmed; the env var applies to the green-context streams [INFERRED from behaviour; the queue count is not visible in the trace].
5. The DL scorecard tail (p99/p99.9, drops) did not move consistently under nsys: next test is a plain (no nsys) daytime A/B, n >= 3 per cell, judged on p99.9 and drops.

## Stall table
| cell | run | unexplained >=10/20/50/100 ms | clusters >=50 | max ms | coincide with csirsDisc end | csirsDisc busy % | csirsDisc kernels >=10/>=50 ms | longest |
|---|---|---|---|---|---|---|---|---|
| V | X1 | 90/63/38/19 | 21 | 308 | 32/38 | 57.4 | 466/196 | refine 686 |
| V | X2 | 62/50/36/18 | 23 | 423 | 32/36 | 72.2 | 568/339 | refine 671 |
| V | M1 | 45/10/8/8 | 1 | 138 | 8/8 | 57.8 | 428/201 | refine 786 |
| V | M2 | 25/3/0/0 | 0 | 26 | n/a | 61.8 | 513/216 | refine 740 |
| T | X1 | 60/38/21/9 | 20 | 499 | 21/21 | 66.6 | 446/227 | sweep 276 |
| T | X2 | 95/54/29/6 | 20 | 240 | 28/29 | 73.9 | 616/306 | sweep 260 |
| T | M1 | 6/0/0/0 | 0 | 18 | n/a | 68.7 | 537/281 | sweep 265 |
| T | M2 | 52/2/0/0 | 0 | 22 | n/a | 63.7 | 153/50 | sweep 275 |
Method: sysan_nsys.stalls() logic reimplemented (agent scratch an.py) with extra columns; consumers passivePdsch*/Pdcch*/Pusch*/pdschRecov.

## DL scorecard (goal_metrics.py, nsys on, tot us)
| run | p50 | p99 | p99.9 | crc_q | drops % | n_real |
|---|---|---|---|---|---|---|
| V X1 | 1901 | 22377 | 44318 | 89.7 | 5.4 | 19083 |
| V X2 | 1492 | 17649 | 46889 | 92.7 | 1.0 | 18671 |
| V M1 | 1807 | 22318 | 36505 | 93.3 | 2.3 | 19142 |
| V M2 | 1658 | 23440 | 44770 | 92.8 | 2.4 | 23716 |
| T X1 | 1254 | 8650 | 18741 | 94.4 | 0.5 | 38198 |
| T X2 | 1262 | 13983 | 30802 | 95.3 | 1.9 | 39343 |
| T M1 | 1319 | 7787 | 19469 | 93.8 | 0.3 | 24399 |
| T M2 | 1534 | 17220 | 29370 | 94.9 | 1.9 | 48185 |
No consistent M benefit on the scorecard: the >= 50 ms stalls are not what sets p99 here (nsys overhead 1.3-2x on p99, residual 10-20 ms waits, other causes) [INFERRED].

## Next
Plain daytime A/B X vs M, n >= 3 per cell; adopt M only if p99.9/drops improve. csirsDisc chunking (stall plan Task 3) becomes secondary if so; the V p99 is dominated by async rescues (COREMAP-NC4-OTA), i.e. Phase 1 T7.

## Not checked
Queue mapping / env propagation not visible in the trace; UL not analysed; stall logic reimplemented, not the sysan function itself; 8 sqlite files (~25 GB) left next to the traces.
