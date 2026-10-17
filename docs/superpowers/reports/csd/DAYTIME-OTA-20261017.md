# Daytime on-air A/B/G: t26aXE vs t26bE vs t26gE, 2026-10-17 08:47-09:43 CEST

**Setup**
- Binary: csd/int16 b5c60c2dc8, build_t26.
- Runner: rr-orchestration/ota/run_day_20261017.sh.
- Runs: 150 s each, n=3 per arm per cell, interleaved rotation XBG/GXB/BGX.
- All 18 runs synced, 0 RF stalls, SIGINT exit.
- TIM: 3759.99 MHz, SSB 233. Vodafone: 3680.01 MHz, SSB 1171.

**Arms**
- X = t26aXE: all-on baseline.
- B = t26bE: night default; X + NC_MIN_CB=4 + RA_TDRA_SWEEP + CUDA_DEVICE_MAX_CONNECTIONS=32 + PDSCH_TDA_FIELD.
- G = t26gE: B + cluster LDPC (CL_MIN_CB=4, NC_MIN_CB=8, LDPC_SM=1) + PDSCH_GATHER_FAST + PDSCH_HOST_FAST + SCAN_READING_HASH + SCAN_SCOREMEMO.

**Load:** daytime, n_real 14k-45k grants per run. This is NOT LOW LOAD.

**Label:** [MEASURED, DGX OTA 2026-10-17 daytime, cell, arm, n=3]. Cells are never pooled.

## DL scorecard (goal_metrics.py, tot latency in us)
| run | n_real | p50 | p99 | p99.9 | crc_q | drops % | recovered |
|---|---|---|---|---|---|---|---|
| TX1 | 40827 | 1250 | 8533 | 18684 | 93.8 | 0.6 | 3 |
| TX2 | 20799 | 1186 | 7501 | 13463 | 93.6 | 0.1 | 1 |
| TX3 | 44519 | 1239 | 4523 | 8120 | 90.3 | 0.0 | 9 |
| TB1 | 34982 | 1265 | 4819 | 8595 | 95.7 | 0.0 | 6 |
| TB2 | 23173 | 1227 | 5208 | 9963 | 94.7 | 0.0 | 4 |
| TB3 | 35847 | 1175 | 5399 | 14120 | 97.6 | 0.1 | 5 |
| TG1 | 35171 | 1050 | 3412 | 7446 | 93.4 | 0.0 | 3 |
| TG2 | 14360 | 1135 | 3584 | 10669 | 96.4 | 0.0 | 1 |
| TG3 | 22357 | 1108 | 6440 | 13215 | 97.2 | 0.0 | 4 |
| VX1 | 33989 | 1422 | 6582 | 31204 | 95.1 | 0.1 | 5071 |
| VX2 | 41932 | 1863 | 11035 | 38781 | 95.1 | 0.8 | 11028 |
| VX3 | 34813 | 1516 | 12474 | 41346 | 94.2 | 0.4 | 6714 |
| VB1 | 33865 | 1546 | 8109 | 17876 | 94.1 | 0.2 | 65 |
| VB2 | 44283 | 1504 | 6554 | 11889 | 95.7 | 0.9 | 23 |
| VB3 | 35604 | 1489 | 8587 | 15197 | 93.9 | 0.1 | 24 |
| VG1 | 29254 | 1138 | 4194 | 8823 | 95.8 | 0.0 | 46 |
| VG2 | 37144 | 1283 | 5109 | 8682 | 94.8 | 0.0 | 33 |
| VG3 | 28326 | 1164 | 4947 | 9248 | 95.9 | 0.0 | 32 |

**Medians per arm (p50 / p99 / p99.9, in us)**

| Cell | X | B | G |
|---|---|---|---|
| TIM | 1239 / 7501 / 13463 | 1227 / 5208 / 9963 | 1108 / 3584 / 10669 |
| Vodafone | 1516 / 11035 / 38781 | 1504 / 8109 / 15197 | 1164 / 4947 / 8823 |

## CRC by slot class (EVMGRANT probe=0, non-SI, all grants, not SNR-qualified)
| arm | TIM slot%10==3 (special) | TIM even-frame slot 8/18 | TIM other | V slot%10==3 | V other |
|---|---|---|---|---|---|
| X | 0 / 0 / 0 % | 0 % | 69 / 61 / 66 % | 70 / 44 / 52 % | 71 / 51 / 62 % |
| B | 57 / 50 / 57 % | 0 % | 70 / 56 / 69 % | 68 / 76 / 72 % | 76 / 84 / 77 % |
| G | 65 / 64 / 51 % | 0 % | 76 / 75 / 68 % | 74 / 71 / 80 % | 78 / 77 / 79 % |

## Reading
- **CRC and drops:** pass in every run of every arm.
  - crc_q: 90.3-97.6 % on TIM, 93.9-95.9 % on Vodafone.
  - drops: <= 0.9 % everywhere, and 0.0 % in all six G runs.
- **Fix A** (PDSCH_TDA_FIELD) confirmed at daytime load: the TIM special slot goes from 0 % (X) to 50-65 % (B, G).
  - Vodafone async rescues: 5-11k per run (X) down to 23-65 (B, G).
  - These rescues drove X's Vodafone p99.9 of 31-41 ms.
- **G vs B latency:** G is lower on both cells.
  - TIM median p99: 5.2 -> 3.6 ms. The ranges overlap: TG3 was 6.4 ms.
  - Vodafone median p99: 8.1 -> 4.9 ms. The ranges are disjoint (B 6.6-8.6 ms, G 4.2-5.1 ms).
  - p50 is 0.1-0.35 ms lower on both cells.
- **TIM even-frame slots 8/18:** still dead (Fix B off).
- **Latency goal:** still FAILS in all arms. Best is G with p50 ~1.1 ms and p99 3.4-6.4 ms, against a target of p99 <= 0.5 ms.

## Decision (freeze rules: adopt only what passes on both cells)
**t26gE becomes the OTA DL default.**
- It replaces t26bE (night, LOW LOAD, provisional).
- It meets CRC and drops on both cells and has the lowest latency on both.
- The latency goal is not met. Next levers:
  - per-RNTI layout cache + known-RNTI fast PDCCH (agent running, branch csd/int16-rcache);
  - moving work out of the Phase-2 lock;
  - TIM even-frame 8/18 (latency-safe Fix B).
