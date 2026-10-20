# Daytime on-air A/B/C: t26gE vs t26uL vs t26uC, 2026-10-18 08:23-09:18 CEST

**Setup**
- Binary: csd/int16-ulrfsim d45b8db2fe build_t26. It contains the csd/int16 b5c60c2dc8 DL code, so one binary serves all arms.
- Runner: ota/run_day_20261018.sh. Runs are 150 s, n=3 per arm per cell, interleaved GLC/CGL/LCG.
- All 18 runs synced, 0 RF stalls. No agent, build or replay ran during the window.
- TIM: 3759.99 MHz, SSB 233. Vodafone: 3680.01 MHz, SSB 1171.

**Arms**
- G = t26gE: the DL default (UL chain as in 10-17).
- L = t26uL: t26gE + UL-REALDEC flags (TA_TRACK, BGDISC, NID_BG, ...) + UL-LAT levers (DELAY_FAST, DELAY_ONCE, RETRY_SNR_MARGIN=3, LDPC_SNR_MARGIN=6).
- C = t26uC: L with UL consumers on big cores 5,6 instead of 0,1. Cores 17-19 are DPDK, 16 the ring, 15 uhd_ctrl.

**Label:** [MEASURED, DGX OTA 2026-10-18 daytime (Sunday morning), cell, arm, n=3]. Cells are never pooled.

## Scorecard (goal_metrics.py, margin 2 dB, p99 in us)
| run | DL n_real | DL p99 | DL crc_q | DL drops | UL n_real | UL p99 | UL crc_q (n_q) | UL drops |
|---|---|---|---|---|---|---|---|---|
| TG1 | 7087 | 3173 | 96.4 | 0.0 | 0 | - | - | - |
| TG2 | 16709 | 5792 | 95.3 | 0.0 | 0 | - | - | - |
| TG3 | 32202 | 4240 | 93.8 | 0.1 | 6233 | 33797 | 0 | 0.2 |
| TL1 | 9669 | 4447 | 96.8 | 0.0 | 4013 | 2714 | - | 0.0 |
| TL2 | 23772 | 2955 | 92.9 | 0.1 | 17486 | 2443 | 0 | 0.0 |
| TL3 | 12804 | 3213 | 94.9 | 0.0 | 9558 | 2665 | - | 0.0 |
| TC1 | 6788 | 2544 | 97.7 | 0.0 | 2744 | 1851 | - | 0.0 |
| TC2 | 18294 | 3991 | 94.3 | 0.0 | 14765 | 2128 | 0 (2) | 0.0 |
| TC3 | 21196 | 5940 | 96.4 | 0.1 | 20339 | 1895 | 0 | 0.0 |
| VG1 | 3132 | 3456 | 96.9 | 0.0 | 15450 | 14049 | - | 0.0 |
| VG2 | 4382 | 2901 | 97.5 | 0.0 | 0 | - | - | - |
| VG3 | 1121 | 1756 | 99.9 | 0.0 | 0 | - | - | - |
| VL1 | 8619 | 4160 | 93.4 | 0.0 | 13141 | 2760 | 0 | 0.0 |
| VL2 | 5082 | 14032 | 97.1 | 0.7 | 8395 | 45649 | 12.0 (25) | 0.1 |
| VL3 | 16030 | 3869 | 95.4 | 0.0 | 37760 | 2772 | 0 | 0.0 |
| VC1 | 4169 | 9318 | 96.9 | 0.2 | 8730 | 1868 | - | 0.0 |
| VC2 | 5395 | 4269 | 85.5 | 0.0 | 6018 | 2795 | 0 (3) | 0.0 |
| VC3 | 5743 | 8833 | 95.9 | 0.0 | 15464 | 2482 | - | 0.0 |

**Arm medians of p99 (us)**
| | DL TIM | DL Vodafone | UL TIM | UL Vodafone |
|---|---|---|---|---|
| G | 4240 | 2901 | (1 of 3 runs decoded UL: 33797) | (1 of 3: 14049) |
| L | 3213 | 4160 | 2665 | 2772 (VL2 45649 outlier) |
| C | 3991 | 8833 | 1895 | 2482 |

## Reading
- **UL latency + drops: met by C on both cells.**
  - All 6 C runs: p99 1.85-2.80 ms, drops 0.0 %.
  - L: 5 of 6 runs are under 3 ms. VL2 had a 45.6 ms UL tail together with a 14.0 ms DL tail (a whole-receiver stall, cause not analysed).
  - G mostly does not lock the UL chain in 150 s (4 of 6 runs have 0 UL grants). Where it does, UL p99 is 14-34 ms.
- **UL CRC: not met.**
  - 99.9 % of UL grants are below the SNR needed for their MCS (e.g. VC2: 6015 low-SNR vs 3 qualified).
  - First genuine on-air UL CRC passes: 4 in VL2, 3 of 25 qualified grants. n is too small to score.
  - This confirms UL-REALDEC: from this antenna position the UEs' uplink is below the decodable SNR.
- **DL CRC + drops: met in every run of every arm.** crc_q 85.5-99.9 %, drops <= 0.7 %.
- **DL latency (p99 < 3 ms): NOT met in daytime.**
  - 4 of 18 runs pass: TC1, TL2, VG2, VG3.
  - Medians are 2.9-4.2 ms for G and L.
  - C has worse Vodafone DL (median 8.8 ms vs G 2.9 ms). The loads differ (G Vodafone n_real 1.1-4.4k vs C 4.2-5.7k), but L at similar or higher load gives 3.9-4.2 ms. So moving the UL consumers to cores 5,6 most likely costs the DL tail [INFERRED: unpinned DL threads use 5,6].
- The UL-LAT replay A/B predicted UL p99 3.0-3.6 ms. On air it is lower (UL slots carry less load than the replay captures).

## Decision
- **No single env meets all three constraints on both cells in daytime.**
- **UL-latency candidate: t26uL.** It meets UL latency on 5/6 runs and has DL tails close to G. It is not adopted yet: one VL2 stall, n=3.
- **t26uC is not adopted.** It meets UL latency everywhere but worsens the Vodafone DL tail.
- **t26gE stays the DL default.**
- **Next:**
  1. Pin or measure what runs on cores 5,6 on air, to keep C's UL gain without the DL cost.
  2. Analyse the VL2 stall.
  3. DL tail: a2e parts 2-4 from DL-A2E-20261018 (Phase-1 pass, the solver reading pass, bank-1 waiting for bank 0).
