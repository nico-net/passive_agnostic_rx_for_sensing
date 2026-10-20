# Afternoon on-air A/B/C 2026-10-18 (14:35-15:22 CEST): t26uL vs t26yE (all passing levers) vs t26fE (GPU pass + FP32)

**Setup**
- Binary: csd/int16-int 26b16e3671 build_t26. This is UL-REALDEC + UL-LAT + a2e + GPU path v2 + DL-tail + core plan + UL-stall, merged.
- Unit tests: PDCCH 275/275, UL disc 13/13, core plan 18/18, UL GPU FEP 1/1.
- Runs: 150 s, n=3 per arm per cell, interleaved LYF/FLY/YFL. All 18 runs synced, 0 RF stalls.
- No agent, build or replay ran during the window.
- A read-only thread sampler (tests/passive_rx/sysan_threads.py) ran 90 s per run, starting 40 s in. Output: aft_20261018/coresample_*.json.

**Arms**
- L = t26uL: UL-LAT levers, consumers on 0,1. This is this morning's UL-latency candidate.
- Y = t26yE = t26kE + ISAC_PDCCH_BANK_HANDOFF=1 + ISAC_UL_GPU_FEP_KCHARGE=1 + ISAC_UL_STALE_DROP=1 + ISAC_UL_GPU_LDPC_HOLD=1.
  - t26kE is core plan K5: PUSCH on 8,9 alone; PDSCH on 7,3,4,0,1; unpinned threads in the lane 10-14; the DMRS scan in the lane.
- F = t26fE = L + DL GPU pass v2 (GRAPH, COVER, SPLIT, RETRY, DMRS2) + ISAC_GPU_PASS_FP32=1. This is item 4.

**Label:** [MEASURED, DGX OTA 2026-10-18 afternoon (Sunday), cell, arm, n=3]. Cells are never pooled.
- Load is higher than this morning: TIM DL n_real 31-48k (morning 7-32k), UL n_real 17-105k.

## Scorecard (goal_metrics.py, p99 in us)
| run | DL n_real | DL p99 | DL crc_q | DL drops | UL n_real | UL p99 | UL drops |
|---|---|---|---|---|---|---|---|
| TL1 | 31622 | 3310 | 95.2 | 0.0 | 37315 | 3275 | 0.0 |
| TL2 | 38765 | 3768 | 93.6 | 0.0 | 32672 | 3209 | 0.0 |
| TL3 | 34221 | 3563 | 95.6 | 0.0 | 44860 | 3257 | 0.0 |
| TY1 | 34213 | 3861 | 94.9 | 0.0 | 31461 | 1776 | 0.0 |
| TY2 | 33600 | 4244 | 94.0 | 0.0 | 18924 | 1764 | 0.0 |
| TY3 | 38366 | 3057 | 90.1 | 0.0 | 40211 | 1731 | 0.0 |
| TF1 | 33165 | 4219 | 85.8 | 0.0 | 28276 | 3491 | 0.0 |
| TF2 | 42014 | 4529 | 96.7 | 0.0 | 28445 | 3158 | 0.0 |
| TF3 | 48012 | 2413 | 96.1 | 0.0 | 49171 | 3145 | 0.0 |
| VL1 | 25801 | 4324 | 93.5 | 0.1 | 37320 | 3330 | 0.0 |
| VL2 | 4477 | 4188 | 96.2 | 0.0 | 17451 | 2702 | 0.0 |
| VL3 | 51575 | 7098 | 95.9 | 0.1 | 105208 | 6345 | 0.0 |
| VY1 | 5256 | 4292 | 94.7 | 0.0 | 17749 | 1361 | 0.0 |
| VY2 | 14443 | 6162 | 95.1 | 0.1 | 35327 | 2267 | 0.0 |
| VY3 | 39125 | 5372 | 93.3 | 0.0 | 75767 | 2863 | 0.0 |
| VF1 | 29703 | 3920 | 94.5 | 0.0 | 59535 | 3579 | 0.0 |
| VF2 | 15575 | 5979 | 91.8 | 0.0 | 35223 | 3436 | 0.0 |
| VF3 | 18670 | 7121 | 96.1 | 0.2 | 33283 | 3103 | 0.0 |

UL crc_q = 0 in every run: UL SNR is below the MCS requirement (see UL-REALDEC, DAYTIME-OTA-20261018).

**Medians of p99 (ms)**
| | DL TIM | DL Vodafone | UL TIM | UL Vodafone |
|---|---|---|---|---|
| L | 3.56 | 4.32 | 3.26 | 3.33 |
| Y | 3.86 | 5.37 | **1.76** | **2.27** |
| F | 4.22 | 5.98 | 3.16 | 3.44 |

## Thread placement (sampler) [MEASURED, OTA, n=3 per arm per cell]
- Y does what the core plan intends:
  - core 2 is idle (0 %);
  - PUSCH cores 8,9 carry only UL (3-22 %);
  - the lane 10-14 carries 23-69 % per core (most loaded on TIM: 45-67 %).
- In L the unpinned threads sit on the big cores 8,9 at 35-79 %.
- Total receiver CPU excluding DPDK, in cores-worth:
  - TIM: L 4.9-5.4, Y 4.6-6.4, F 4.5-5.2;
  - Vodafone: L 3.5-6.0, Y 3.2-4.6, F 3.7-4.8.
  - The core count used does not fall yet: Y frees core 2 only (16 vs 17 on air).

## Reading
- **UL latency + drops: met by Y on both cells in all 6 runs** (p99 1.36-2.86 ms, drops 0.0 %), at the highest UL load seen so far (up to 76k UL grants per 150 s).
  - L fails UL latency in 5/6 runs at this load (3.2-6.3 ms), with no queue-full drops.
  - VL3 (105k UL grants) reached 6.3 ms, a milder VL2-like episode.
- **DL CRC + drops: met by every run of every arm** (crc_q 85.8-96.7 %, drops <= 0.2 %).
- **DL latency: not met by any arm** (1/18 runs pass: TF3).
  - Y is worse than L: TIM median 3.86 vs 3.56 ms (+8 %), Vodafone 5.37 vs 4.32 ms. The Vodafone runs are load-confounded, VL3 itself being 7.1 ms.
  - The core plan's replay DL cost (+2-7 %) repeats on air. [INFERRED] The cause is the loaded little-core lane (pdcchJanon, ulBgDisc, DMRS scan at 45-67 % per lane core on TIM).
  - BANK_HANDOFF's b-replay gain is not visible on air (TIM has one bank; not separable inside Y).
- **F (GPU pass + FP32): rejected.** DL p99 is worse on both cells (TIM 4.22, Vodafone 5.98 ms). It matches the replay result that the pass is at best equal to OFF.

## Decision
- **No env meets all constraints. UL CRC is physically unattainable from this position.**
- **Y becomes the UL-latency candidate:** UL p99 < 3 ms and drops < 1 % on both cells, n=3, daytime.
  - Its DL cost comes from the lane, so the next core-plan step is to keep the UL part (PUSCH 8,9 alone) without overloading the lane. For example: lane 2,10-14 (replay: tim DL back to L level), or move the heavy floaters (pdcchJanon, ulBgDisc) to a dedicated little core rather than the shared lane.
- **DL p99 < 3 ms needs a new lever.** The remaining pre-decode cost is polar decoding (29 % of scan CPU) and LLR generation (17 %) (DL-TAIL-20261018). Afternoon load is higher than any replay capture.
