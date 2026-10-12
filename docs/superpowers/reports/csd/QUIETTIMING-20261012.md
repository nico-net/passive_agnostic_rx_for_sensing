# QUIETTIMING-20261012 -- clean timing baseline of the PDSCH decode (branch csd/quiet-timing, base csd/otatest 388e11ae02)

Evidence labels: [M-replay] = DGX GB10 aarch64, 4 RX, `timacq_replay.sh SPEED=1` via heavy.sh, binary `rr-csd-b/.../build_t26` (CUDA, csd/otatest), env `ota/t26z.env` + `ISAC_PDTIM_CLASS=1` (t26z already has PDTIM_PCT=1, PDCCH_TIMING=1). b = Vodafone raw_pci415_12s_b (CAP_TICK 208029025, CAPC=RXC 3680010000, SSB 1171, FO 6900); tim = raw_pci923_tim_12s_tile3 (CAP_TICK 195322029, CAPC 3731910000, RXC 3759990000, SSB 233, FO -4800). Never merged across captures. [INF] = inference. No build, no code change, no OTA. Data: `quiettiming_data/` (`runs.txt` pre/post host record, `runs.json` all parsed values, `summary.txt` full tables, `drive.sh` driver, `parse.py`, `analyze.py`).

## Protocol and host quietness
24 runs, strictly one at a time, interleaved per round (b-A, b-B, b-C, tim-A) x 6. Before each run: no `nr-uesoftmodem`, GPU temp and SM clock recorded (42-51 C, idle clock 208 MHz; 3 runs started with a 325/2411 MHz clock left from the previous run). Every run finished by itself (rc=0, ~85 s), no SIGINT needed, no background job left. During runs a 5 s watcher saw no other process above 50 % CPU and no second receiver (only the receiver's own parent/child PID pair). **Deviation:** the load-average gate "<2" cannot be met on this host: three tasks stuck in uninterruptible D state for days (`mstflint`, `mlxlink_ext`, 0 CPU) hold loadavg at a phantom 3.00 with the machine 100 % idle. The gate used effective load = loadavg1 - D-state tasks < 2; recorded value at start 0.0-2.6 (b-A5 2.61, tim-A5 2.53, all other < 2.0, absolute loadavg never > 4.65 before a start). No run was discarded (none had another receiver or a hog), but run-to-run spread below is the real quiet-host spread.

## Per-arm results, Vodafone b (6 runs/arm, mean +- sd across runs) [M-replay]
| metric (us unless noted) | A t26z | B +GPU_BATCH=1 | C +RT_PARTS=2 |
|---|---|---|---|
| **real-grant mean** | **849 +- 86** | 830 +- 155 | 835 +- 165 |
| real p50 / p90 | 417 +- 12 / 950 +- 76 | 420 +- 14 / 962 +- 114 | 422 +- 10 / 957 +- 142 |
| real p99 | 11797 +- 4205 | 9187 +- 6140 | 7628 +- 5524 |
| real >5 ms share, >20 ms count | 1.8 % +- 0.5, 12.2 +- 5.6 | 1.4 % +- 0.6, 14.7 +- 3.2 | 1.3 % +- 0.9, 14.5 +- 4.2 |
| real max (ms) | 65 +- 23 | 68 +- 23 | 134 +- 70 |
| real mean excluding >5 ms bodies / >20 ms bodies | 536 +- 18 / 667 +- 52 | 547 +- 24 / 626 +- 49 | 544 +- 17 / 630 +- 73 |
| retry mean / p99 | 525 +- 37 / 2770 +- 254 | 523 +- 20 / 2513 +- 583 | 560 +- 32 / 2472 +- 529 |
| probe mean / p99 | 609 +- 55 / 3757 +- 1087 | 565 +- 132 / 4625 +- 2977 | 584 +- 74 / 4290 +- 1260 |
| all bodies mean (real+retry+probe) | 617 +- 34 | 588 +- 42 | 620 +- 22 |
| per-decode sum fep+chest+demod+ldpc p50/p90/p99 | 363 / 720 / 3825 | 367 / 748 / 3292 | 375 / 783 / 3358 |
| stage mean fep / chest / alloc / demod / ldpc | 113 / 162 / 2.1 / 179 / 163 | 114 / 159 / 2.1 / 146 / 168 | 108 / 157 / 1.9 / 179 / 176 |
| stage sd fep / chest / demod / ldpc | 1028 / 1207 / 2573 / 307 | 1121 / 1235 / 738 / 301 | 938 / 1199 / 2133 / 348 |
| stage max ms fep / chest / demod / ldpc | 48 / 71 / 227 / 18 | 53 / 61 / 46 / 20 | 50 / 65 / 206 / 32 |
| stage p50 / p90 / p99 fep | 75 / 108 / 308 | 75 / 105 / 265 | 75 / 105 / 318 |
| stage p50 / p90 / p99 chest | 80 / 148 / 1008 | 77 / 142 / 908 | 75 / 147 / 973 |
| stage p50 / p90 / p99 demod | 93 / 155 / 1102 | 92 / 157 / 1107 | 95 / 168 / 1148 |
| stage p50 / p90 / p99 ldpc | 115 / 242 / 928 | 117 / 272 / 988 | 115 / 293 / 1033 |
| pdschq queued / decoded | 6894 / 5606 | 6443 / 6120 | 6971 / 6387 |
| pdschq drop_stale (% of queued) / drop_full | 17.1 % +- 11.7 / 0 | 4.1 % +- 3.6 / 0 | 7.2 % +- 7.3 / 0 |
| scanq drop_stale | 683 +- 551 | 118 +- 127 | 213 +- 268 |
| CRC-OK real grants / distinct CRC-OK RNTIs | 1228 +- 544 / 23 +- 7 | 1540 +- 539 / 26 +- 7 | 1734 +- 351 / 29 +- 4 |
| stall events >20 ms per run (clusters) / bodies | 11.5 / 27.5 | 8.7 / 28.5 | 11.8 / 31.3 |

TIM tile3 (arm A only, 6 runs) [M-replay]: real mean **3056 +- 844** us, p50 708 +- 153, p90 3113 +- 1193, p99 19995 (histogram cap, all 6 runs), >5 ms share 6.7 % +- 2.5, >20 ms count 12.5 +- 4.2, max 154 +- 118 ms; retry mean 1419 +- 130 (p99 9927), probe mean 714 +- 107 (p99 4932); per-decode sum p50/p90/p99 377 / 1350 / 9970; stage mean fep/chest/demod/ldpc 149 / 299 / 312 / 240 (sd 1808 / 2640 / 6345 / 527; max 65 / 87 / 415 / 20 ms); pdschq stale 14.0 % +- 5.4 of 4500 queued, scanq stale 249; CRC-OK real 283 +- 78, 6.7 +- 2.0 RNTIs; stalls 13.8 clusters/run. real n is only 473 +- 140 per run so every TIM statistic is noisy.

## Significance, b, A vs B and A vs C (n = 6 vs 6; Welch t-test, Mann-Whitney U, bootstrap 95 % CI of the difference of means, 20000 resamples) [M-replay]
| quantity | A | B | diff B-A, Welch p, MWU p, CI | C | diff C-A, Welch p, MWU p, CI |
|---|---|---|---|---|---|
| real mean | 849 | 830 | -20, p 0.79, 0.49, [-140, +120] | 835 | -15, p 0.85, 0.31, [-130, +135] |
| real p99 | 11797 | 9187 | -2610, p 0.41, 0.24, [-7760, +3098] | 7628 | -4168, p 0.17, 0.13, [-8907, +1192] |
| real p50 | 417 | 420 | +3, p 0.66 | 422 | +5, p 0.45 |
| all-body mean | 617 | 588 | -29, p 0.23, [-67, +13] | 620 | +3, p 0.87, [-27, +32] |
| real mean excl >20 ms | 667 | 626 | -41, p 0.20, [-94, +12] | 630 | -37, p 0.34, [-99, +32] |
| pdschq drop_stale % | 17.1 | 4.1 | -12.9, p 0.042, 0.093, [-21.5, -3.7] | 7.2 | -9.9, p 0.12, 0.18, [-19.4, +0.6] |
| scanq drop_stale | 683 | 118 | -565, p 0.053, 0.15, [-966, -139] | 213 | -470, p 0.10, 0.20, [-904, -15] |
| CRC-OK real count | 1228 | 1540 | +312, p 0.34 | 1734 | +506, p 0.089, 0.065, [+18, +969] |
**Verdict:** neither ISAC_PDSCH_GPU_BATCH=1 nor ISAC_GPU_RT_PARTS=2 changes the real-grant mean or p99 beyond noise (all p > 0.17; the mean CI is about +-130 us, i.e. any effect under ~15 % is undetectable with n = 6; the p99 point estimates are -22 % / -35 % but the CIs include 0 because single 100-400 ms stalls move p99 by 5-10 ms). Secondary signals that favour the flags but are not corrected for the ~15 comparisons made here: B has lower pdschq stale (p 0.042 Welch, 0.093 MWU) and scanq stale (p 0.053), C has more CRC-OK grants (MWU 0.065); they are consistent with the same direction in A having more stale drops, but A's stale rate also varies 0 %-31 % between runs, i.e. it is a property of host/scan-queue timing, not shown to be caused by the flag. Do not claim a win; B and C stay trials. The target **mean < 0.5 ms is not met by any arm** (best single run 705 us; 536-547 us after removing the >5 ms bodies in every arm).

## Where the mean comes from, arm A [M-replay]
Share of the total body time spent in bodies of a given duration (PDTIMCLS share_gt1/5/20 ms, mean of 6 runs; bodies = per-grant sum of the four stages):
| capture, class | < 1 ms | 1-5 ms | 5-20 ms | > 20 ms | (per-run >20 ms range) |
|---|---|---|---|---|---|
| b real | 0.48 | 0.15 | 0.16 | **0.21** | 0.14-0.29 |
| b retry | 0.72 | 0.20 | 0.06 | 0.03 | 0.00-0.10 |
| b probe | 0.57 | 0.08 | 0.08 | **0.26** | 0.20-0.34 |
| tim real | 0.10 | 0.22 | 0.14 | **0.54** | 0.28-0.62 |
| tim retry | 0.16 | 0.65 | 0.12 | 0.07 | 0.00-0.17 |
| tim probe | 0.47 | 0.08 | 0.07 | 0.37 | 0.27-0.48 |
So on b about half of the real-grant mean is bodies under 1 ms (those cost ~0.41 ms p50), **one fifth is the ~12 bodies per run (0.2-0.5 % of grants) that exceed 20 ms and 37 % is in bodies over 5 ms**. On TIM more than half the mean is the >20 ms bodies. The stage-level split by body-duration class is NOT available from the existing counters (PDTIMCLS has no per-stage histograms per duration class); what exists:
* Per stage, arm A, b: mean vs p50 (tail excess = mean - p50): fep 112 vs 75 (33 %), chest 162 vs 80 (51 %), demod 179 vs 93 (48 %), ldpc 163 vs 115 (30 %); stage means sum to 617 us, of which ~55 % lies above the stage medians. TIM: fep 149 vs 25 (83 %), chest 299 vs 87 (71 %), demod 312 vs 90 (71 %), ldpc 240 vs 118 (51 %).
* Stage that held the time of the >20 ms bodies (PDTIMSLOW; all slow bodies of the 6 runs were logged, 15-43 per run, below the 200 cap; sum of stage wall time over those bodies): b: fep 29 %, chest 36 %, demod 33 %, ldpc 1 % (dominant-stage counts fep 65, chest 65, demod 33, ldpc 2); tim: fep 20 %, chest 36 %, demod 41 %, ldpc 3 %. LDPC is almost never the slow stage; the three GPU-launch stages (FEP, chest, demod) are. Wall == CPU time in nearly all PDTIMSLOW lines (as DECTAIL found): the thread was running or spinning in a stream sync, not descheduled.
* Stalls are host/GPU-wide, not per thread: a typical cluster of >20 ms bodies holds 1-5 bodies sharing the same millisecond (28-37 % of clusters have >= 4 bodies, i.e. all 5 consumers stuck at once), in whatever stage each consumer happened to be in. Cluster length (longest body): median 33-41 ms, p90 66-155 ms; > 100 ms: A 10, B 2, C 16 clusters in 6 runs (b), 14 on TIM; max A 429 ms, B 122 ms, C 364 ms, tim 884 ms. No stall reached 1 s. Per run b has 9-12 clusters / 27-31 slow bodies for every arm. Demod sd/max differs by arm (A 2573 us / 227 ms, B 738 us / 46 ms, C 2133 / 206): B's demod stage had no stall > 46 ms in any of 6 runs, A and C had 150-430 ms demod stalls in several; this is the one stage-level difference that looks arm-related (n = 6, not tested formally; the other stages do not show it) [INF: the batch gate keeps at most 3 bodies on the GPU chain so a pause cannot hit all five at once in demod].

## Hand-over for the next agent
1. Baseline to beat (b, t26z, quiet host): real-grant mean 849 +- 86 us (run range 705-1164), p50 417, p99 11.8 ms; all-body mean 617 +- 34; TIM real mean 3056 +- 844 (range 1937-4454). A single-run A/B needs a difference > 15 % of the mean to be visible; use >= 6 runs per arm and the mean excluding > 20 ms bodies (667 +- 52 us, tighter) next to the plain mean.
2. Getting to 0.5 ms means removing the 5 ms+ bodies (they carry 37 % of the real mean on b, ~60+ % on TIM): the cause is ~10 host/GPU-wide pauses of 20-400 ms per 12 s, hitting FEP/chest/demod (not LDPC) of all consumers at once, not the per-body steady-state cost (p50 0.42 ms, stage medians 75 / 80 / 93 / 115 us). A decode-time fix that only speeds the steady-state path cannot reach 0.5 ms.
3. Tooling gap: add per-stage histograms split by body-duration class (or a per-stage share_gt1/5/20) to PDTIMCLS to get the requested stage x duration table directly; not done (no code changes in this task).
4. Host note: 3 stuck D-state processes (`mstflint`, `mlxlink_ext`, PIDs 1762816/1771726/3199390) keep loadavg at 3.0; any "load < N" gate in the harness must subtract them or it will wait forever.

## OTA env delta
None. Both flags unproven; no default changes.
