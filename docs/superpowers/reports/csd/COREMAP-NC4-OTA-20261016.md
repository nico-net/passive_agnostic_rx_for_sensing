# On-air A/B: Phase 1 T4 core map (C) and T5 NC_MIN_CB=4 (N) vs all-on (X), 2026-10-16 evening

Runs 21:15-22:20 CEST (Friday EVENING load; not pooled with afternoon runs). Interleaved rotation (X C N / N X C / C N X), n=3 per arm per cell, 150 s, all 18 synced, SIGINT exit. Binary csd/int16 build_t26 (a275844782 tree).
X = t26aXE; C = t26aXCE (ISAC_PDSCH_CORES=8,9,3,4 [core 17 is DPDK-reserved by nr_core_plan], USS 11, JOINT_ANON_CORE 12, scan 6,7 alone); N = t26aXE + ISAC_GPU_LDPC_NC_MIN_CB=4 (was 8).
Runner rr-orchestration/ota/run_cn_20261016.sh. Label: [MEASURED, DGX OTA 2026-10-16 evening, cell, arm, n=3]. Tool goal_metrics.py.
Before the A/B, two nsys on-air traces (stall plan Task 4): ota_runs/ab_nV_1_210903, ab_nT_1_*; analysis pending.

## DL per run (tot us; crc_q %; drops %; recovered)
| arm | T p99 | T p99.9 | T crc_q | T drops | V p99 | V p99.9 | V crc_q | V drops | V rec |
|---|---|---|---|---|---|---|---|---|---|
| X | 5087 / 4893 / 6650 | 9267 / 14451 / 13467 | 93.0 / 93.2 / 95.2 | 0.1 / 0.1 / 0.4 | 13866 / 15800 / 17492 | 45063 / 41202 / 56471 | 93.8 / 89.4 / 90.0 | 0.4 / 1.7 / 4.2 | 5621 / 2694 / 4159 |
| C | 6434 / 3362 / 7752 | 12704 / 6365 / 16509 | 94.7 / 95.5 / 95.1 | 0.4 / 0.6 / 1.3 | 11011 / 15713 / 12741 | 47015 / 47683 / 37181 | 93.8 / 90.7 / 94.1 | 0.9 / 0.7 / 0.8 | 4799 / 1432 / 4644 |
| N | 4116 / 5381 / 6557 | 8036 / 11382 / 13589 | 93.4 / 93.6 / 94.1 | 0.1 / 0.1 / 0.1 | 14162 / 13872 / 14538 | 42404 / 34391 / 50035 | 94.0 / 93.9 / 93.6 | 0.5 / 1.4 / 0.4 | 3126 / 2437 / 4953 |
Medians: T p99 X 5087, C 6434, N 5381; V p99 X 15800, C 12741, N 14162. V drops med X 1.7, C 0.8, N 0.5; T drops med X 0.1, C 0.6, N 0.1.
cVC2 had only 8.6k DL grants (light traffic window); cVX2/cVN2 also low (13-14k).
UL: crc ~0 in all 18 runs (ULCAUSE unchanged).

## GPU LDPC fallbacks (final stats line per run, fallback[err]; reasons not yet logged)
T: X 1546 / 743 / 252; C 1413 / 497 / 559; N 1551 / 797 / 1027. V: X 10 / 1189 / 39; C 8 / 13 / 181; N 933 / 834 / 156.
Spread within an arm is 6-100x: the counter cannot rank arms until T5's reason codes exist.

## Reading
- N (NC_MIN_CB=4): best V drops (all < 1.5 %) and V p99 tighter than X (13.9-14.5 vs 13.9-17.5 ms); T no worse; crc_q unchanged (V 93.6-94.0, the tightest of the three). Weak positive, inside noise for T. Candidate for the default, confirm in daytime.
- C (core map): mixed. V p99/drops better than X, T drops worse (0.4-1.3 vs 0.1-0.4) and T p99 spread wider (3.4-7.8 ms). Not adopted; T4 acceptance (no cell worse) not met at n=3.
- No arm moves latency toward 0.5 ms: tails stay 3-17 ms p99 and 6-56 ms p99.9; V rescues (1.4-5.6k/run) dominate V p99.9 -> T7 (TDRA predictor) is the lever, not placement.
- T5 code part (reason codes) is still required to interpret fallbacks.
