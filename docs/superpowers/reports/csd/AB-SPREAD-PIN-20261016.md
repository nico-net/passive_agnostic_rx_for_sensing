# Interleaved A/B 2026-10-16 15:20-16:10: slot spread on/off (A1) and pinned PDCCH scan (B1)

Arms (binary csd/int16 7f34aff2a3; random order within each round): X = t26aXE (all-on); S = X with ISAC_PDSCH_SLOT_SPREAD=0; B = X with 3 scan threads pinned to cores 12-14 (cfg sd_t26ul3p), NID GPU worker 10, USS tracker 11, joint-anon search 6, no PAR_BANKS.
Stopped by the user at 16:10 (X410 to be unplugged): aVX3 SIGINTed after ~60 s -> excluded; the MPS multi-PCI runs did NOT run. aTS2 decoded only 3.7k DL grants and logged no UL (anomalous run, reported, not used for conclusions).
Labels [MEASURED, DGX OTA 2026-10-16, <cell>, <arm>, n=runs]. crc_q = SNR-qualified CRC (goal_metrics.py, margin 2 dB, Phase 0 Task 1 code).

## DL per run
| run | grants | crc_all % | crc_q % | drops % | tot p50 | tot p99 | a2e p50 / p99 | pre p99 | qwait p99 |
|---|---|---|---|---|---|---|---|---|---|
| VX1 | 41051 | 40.3 | 51.5 | 1.6 | 2039 | 11248 | 880 / 4521 | 2214 | 5921 |
| VX2 | 41462 | 36.6 | 44.1 | 2.0 | 2031 | 13345 | 734 / 3371 | 2334 | 7245 |
| VS1 | 43811 | 37.5 | 45.3 | 1.6 | 2183 | 13283 | 947 / 5005 | 2999 | 6063 |
| VS2 | 36207 | 43.0 | 50.1 | 2.0 | 1706 | 10625 | 772 / 2998 | 2028 | 7335 |
| VB1 | 38444 | 46.6 | 52.5 | 2.9 | 2163 | 9964 | 1118 / 5389 | 676 | 5777 |
| VB2 | 33920 | 35.6 | 44.0 | 3.0 | 2597 | 9606 | 1583 / 7031 | 413 | 4216 |
| VB3 | 31484 | 36.3 | 47.5 | 3.5 | 2723 | 10222 | 1782 / 6512 | 223 | 4664 |
| TX1 | 33431 | 64.0 | 75.2 | 0.6 | 1467 | 8656 | 706 / 3797 | 1490 | 2844 |
| TX2 | 31167 | 55.6 | 67.1 | 0.1 | 1328 | 6678 | 628 / 2815 | 1160 | 1007 |
| TS1 | 26170 | 66.8 | 78.9 | 0.2 | 1393 | 7782 | 650 / 3312 | 1368 | 1647 |
| TS2* | 3671 | 38.5 | 50.4 | 0.1 | 2000 | 6035 | 730 / 2534 | 1368 | 619 |
| TS3 | 37239 | 32.7 | 45.5 | 0.7 | 2023 | 14010 | 651 / 2701 | 1645 | 7464 |
| TB1 | 37233 | 59.8 | 73.7 | 0.7 | 2141 | 8831 | 1331 / 6095 | 257 | 3312 |
| TB2 | 12190 | 39.8 | 52.0 | 0.5 | 1983 | 10094 | 1192 / 4935 | 156 | 3984 |
| TB3 | 25841 | 52.4 | 66.2 | 0.8 | 2125 | 9138 | 1268 / 6037 | 441 | 4029 |
UL: CRC ~0 in every run (0-9 OK of 20k-112k decodes).

## A1 verdict: slot spread is NOT the cause of the TIM CRC drop
- crc_q with spread on (X) 75.2/67.1 vs off (S) 78.9/45.5 (T), 51.5/44.1 vs 45.3/50.1 (V): the arms overlap; the run-to-run spread (T 45-79 %) is larger than any arm effect. The 59 -> 36 % drop seen earlier is traffic/time variation.
- p99: no consistent difference (V X 11.2/13.3 vs S 13.3/10.6 ms; T X 8.7/6.7 vs S 7.8/14.0 ms).
- Keep spread ON (it does not hurt; its `pre` gain is small).

## B1 verdict: pinning the scan to cores 12-14 made the PDCCH SLOWER — those are efficiency cores
- GB10 core capacities (`/sys/devices/system/cpu/cpu*/cpu_capacity`): cores 0-4 and 10-14 = Cortex-A725 (718-731), cores 5-9 and 15-19 = Cortex-X925 (997-1024). The default scan cores 6,7 are X925; B moved the scan to A725 cores.
- a2e p50 V 0.73-0.88 -> 1.12-1.78 ms, T 0.63-0.71 -> 1.19-1.33 ms; a2e is now the top p99 component in T (43-49 %). Drops V 1.6-2.0 -> 2.9-3.5 %.
- Side effect: `pre` p99 fell to 0.16-0.68 ms (core 7 no longer shared by scan thread 1 and PDSCH consumer 0).
- Also: today PDSCH consumers 3,4 and both UL decode threads (0,1) run on A725 cores; the DPDK cores 18,19 and the UHD control core 15 hold X925 cores.

## Consequences for the plan
- Core placement must use the big/little split: latency-critical threads (RT read, PDCCH scan, PDSCH/PUSCH decode) on X925 cores 5-9, 15-17; helpers on A725 cores. Goes into Phase 0 Task 3 (inventory) and G3.
- DL crc_q is 44-53 % (V) and 45-79 % (T): far from 80 %; DL CRC root cause (Phase 0 Task 5) is a G1 blocker.
- MPS multi-PCI still untested.
