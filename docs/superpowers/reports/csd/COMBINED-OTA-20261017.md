# On-air A/B: combined DL setup t26bE vs t26aXE, 2026-10-17 night (LOW LOAD)

Runs 00:50-01:35 CEST, interleaved, n=3 per arm per cell, 150 s, all 12 synced, SIGINT exit. Binary csd/int16 cc747a25f6 (Fix A + Fix B merged; Fix B NOT enabled). Runner rr-orchestration/ota/run_k_20261017.sh.
X = t26aXE. B = t26bE = X + ISAC_GPU_LDPC_NC_MIN_CB=4 + ISAC_RA_TDRA_SWEEP=1 + CUDA_DEVICE_MAX_CONNECTIONS=32 + ISAC_PDSCH_TDA_FIELD=1.
Fix B (ISAC_CSIRS_FULL_VALID=1) excluded: replay p99 b 4.3 -> 14.9 ms, tim 2.6 -> 12.6 ms (double trial decodes) [MEASURED, DGX replay, n=1].
Label: [MEASURED, DGX OTA 2026-10-17 night LOW LOAD, cell, arm, n=3]. V traffic collapsed during the X runs (VX2/VX3 only 388-390 grants): V arms are not load-matched.

## DL scorecard (goal_metrics.py, tot us)
| run | n_real | p50 | p99 | p99.9 | crc_q | drops % | recovered |
|---|---|---|---|---|---|---|---|
| TX1 | 30784 | 1133 | 3985 | 8036 | 94.0 | 0.1 | 44 |
| TX2 | 29897 | 1180 | 4095 | 8595 | 91.1 | 0.8 | 2 |
| TX3 | 25517 | 1143 | 7417 | 18804 | 80.8 | 0.3 | 63 |
| TB1 | 30917 | 1139 | 5850 | 11570 | 91.4 | 0.1 | 40 |
| TB2 | 23917 | 1110 | 3259 | 6748 | 88.2 | 0.0 | 34 |
| TB3 | 26528 | 1177 | 4283 | 8018 | 84.6 | 0.0 | 73 |
| VX1 | 6257 | 1197 | 9318 | 36533 | 95.5 | 0.3 | 929 |
| VX2 | 388 | 706 | 4181 | 9523 | 96.4 | 0.0 | 0 |
| VX3 | 390 | 654 | 5937 | 17050 | 99.7 | 0.0 | 0 |
| VB1 | 13757 | 1292 | 5290 | 8227 | 95.7 | 0.0 | 16 |
| VB2 | 12052 | 1106 | 3069 | 4397 | 96.1 | 0.0 | 9 |
| VB3 | 4541 | 1980 | 8886 | 18209 | 95.0 | 0.1 | 9 |

## CRC by slot class (EVMGRANT probe=0, non-SI, all grants, not SNR-qualified)
| run | slot%10==3 (special) | even-frame slot 8/18 | other |
|---|---|---|---|
| TX1/2/3 | 0/1507, 0/2441, 0/2152 (0 %) | 0 % | 63 / 70 / 64 % |
| TB1/2/3 | 586/1856 (32 %), 812/2051 (40 %), 618/1676 (37 %) | 0 % | 60 / 65 / 58 % |
| VX1 | 370/525 (70 %) | - | 63 % |
| VB1 | 596/950 (63 %) | - | 77 % |

## Reading
- Fix A works on air: TIM special slot 0 % -> 32-40 % (all grants; these slots have lower SNR); V async rescues 929 (VX1) -> 9-16 per run, V p99.9 36.5 ms (VX1) -> 4.4-18 ms.
- DL CRC (crc_q) passes on both cells in both arms (T 80.8-94 X vs 84.6-91.4 B; V 95-96). Drops < 1 % in every B run.
- Latency still FAILS: p99 3.1-8.9 ms (B), p50 ~1.1-2.0 ms. The end-to-end split (replay, combined env) is PDCCH scan p50 0.52 / p99 4.2 ms + decode body p50 0.5 / p99 2.6 ms; probes and retries add ~1.15 decodes per real grant on the GPU.
- TIM even-frame 8/18 still dead (Fix B off).
- Freeze t26bE as the night default (provisional, LOW LOAD); daytime retest 2026-10-17 morning, n >= 3, interleaved vs t26aXE.
