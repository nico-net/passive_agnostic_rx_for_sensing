# Baseline scorecard and CRC-vs-SNR-margin calibration (Phase 0 Task 2), 2026-10-16

Tools: tests/passive_rx/goal_metrics.py (b13671f919), crc_margin_curve.py (eed26b019d). margin = min used-layer SNR - required_snr_db(mcs, tbl) (Shannon gap + 1.5 dB). Latency in us. PASS = tot p99 <= 500 and crc_q >= 80 % and drops < 1 %. V = Vodafone, T = TIM, never pooled. Arms: X = t26aXE all-on, S = spread off, B = scan pinned to A725 cores, Xold = ab_x*aXE*, E = t26aE ab_q*aE*. Excluded ab_aVX3 (stopped ~60 s); ab_aTS2 anomalous (3.7k grants, no UL), not in medians.
Label for all rows: [MEASURED, DGX OTA 2026-10-16, <cell>, <arm>, n as listed]. Sonnet agent; saved by the orchestrator.

## DL medians per arm (every run FAILs)
| cell | arm (n) | grants | tot p50 | tot p99 | p99.9 | crc_all | n_q | crc_q | drops % |
|---|---|---|---|---|---|---|---|---|---|
| V | X (2) | 41256 | 2032 | 12296 | 27474 | 38.5 | 32619 | 47.8 | 1.8 |
| V | S (2) | 40009 | 1942 | 11954 | 26970 | 40.2 | 33084 | 47.7 | 1.8 |
| V | B (3) | 33920 | 2595 | 9964 | 34038 | 36.3 | 27035 | 47.5 | 3.0 |
| V | Xold (3) | 27274 | 1694 | 11509 | 30057 | 40.9 | 21722 | 50.6 | 1.8 |
| V | E (3) | 48471 | 1923 | 12653 | 49765 | 41.2 | 40043 | 53.2 | 3.3 |
| T | X (2) | 32299 | 1398 | 7668 | 13486 | 59.8 | 27120 | 71.2 | 0.3 |
| T | S (2) | 31705 | 1708 | 10902 | 16160 | 49.7 | 24382 | 62.2 | 0.5 |
| T | B (3) | 25841 | 2124 | 9139 | 17106 | 52.4 | 20422 | 66.2 | 0.7 |
| T | Xold (3) | 32735 | 1424 | 8153 | 13808 | 35.7 | 19734 | 62.6 | 0.4 |
| T | E (3) | 18109 | 1467 | 10457 | 40583 | 59.4 | 14325 | 74.9 | 0.5 |
Per-run DL rows: V X 51.5/44.1 crc_q, S 45.3/50.1, B 52.5/44.0/47.5, Xold 53.3/47.5/50.6, E 53.2/56.4/38.5; T X 75.2/67.1, S 78.9/(50.4 anomalous)/45.5, B 73.7/52.0/66.2, Xold 67.2/62.6/61.4, E 77.7/73.9/74.9. DL p99 6.7-15.4 ms (13-30x over target); only T drops pass.

## UL medians (every run FAILs; CRC genuinely ~0)
| cell | arm | grants | tot p50 | tot p99 | crc_all | drops % |
|---|---|---|---|---|---|---|
| V | X | 92149 | 6075 | 47663 | ~0 | 4.5 |
| V | S | 77499 | 4603 | 50763 | ~0 | 4.1 |
| V | B | 87080 | 4822 | 44391 | ~0 | 5.4 |
| V | Xold | 70221 | 5943 | 55377 | ~0 | 8.9 |
| V | E | 69757 | 15404 | 59540 | ~0 | 17.3 |
| T | X | 84656 | 2882 | 41295 | ~0 | 3.7 |
| T | S | 71195 | 3332 | 12515 | ~0 | 0.0 |
| T | B | 60370 | 3275 | 40002 | ~0 | 0.0 |
| T | Xold | 65740 | 2959 | 23304 | ~0 | 0.1 |
| T | E | 33309 | 2370 | 13228 | ~0 | 0.0 |
UL: 0-9 OK of 20k-112k decodes per run; cause per ULCAUSE-20261016 (unknown per-UE data scrambling id + BWP look-alike lock). UL n_q (up to 47k) are failed decodes at adequate SNR. UL margin curves: 0 % in every bin, every arm.

## DL CRC vs SNR margin (pooled per cell+arm)
| cell | arm | cross80_db | CRC-OK +5 dB | +10 dB | +20 dB |
|---|---|---|---|---|---|
| V | X | None | 33 % | 62 % | 74 % |
| V | S | None | 33 % | 61 % | 74 % |
| V | B | None | 33 % | 61 % | 71 % |
| V | Xold | None | 38 % | 60 % | 76 % |
| V | E | +18 | 48 % | 62 % | 77 % |
| T | X | +10 | 41 % | 92 % | 100 % |
| T | S | +10 | 32 % | 93 % | 100 % |
| T | B | +10 | 53 % | 90 % | 100 % |
| T | Xold | +10 | 62 % | 91 % | 100 % |
| T | E | +9 | 58 % | 93 % | 100 % |
Raw V X: -4:543/2% -2:1294/3% 0:2465/7% +2:3396/7% +4:4579/23% +6:4858/40% +8:4682/50% +9:8022/37% +10:5607/62% +12:2797/70% +14:2285/71% +16:1709/74% +20:2105/74%.
Raw T X: -4:457/0% -2:1104/0% 0:1603/1% +2:1530/4% +4:1911/22% +6:3002/43% +8:4932/68% +9:10984/65% +10:5573/92% +12:1380/81% +14:1057/95% +16:1208/100% +20:6304/100%.

## Step 5 statement: DL CRC loss is NOT explained by SNR
- V: CRC-OK ceiling ~70-77 % even at +20 dB margin (cross80 None in 4/5 arms): a margin-independent loss.
- T: reaches 100 % at >= +17 dB, but 80 % only at +10 dB; 1-10 % at 0..+3 dB: curve shifted ~6-8 dB vs the Shannon + 1.5 dB model.
- IMPL_LOSS_DB = 1.5 is too optimistic, but a larger offset cannot fix the V ceiling; crc_q here is not yet a valid SNR-qualified result until Task 5 explains the shift and the ceiling.
- Both cells have an over-populated +9 dB bin decoding worse than +10 dB (V 8022 at 37 %, T 10984 at 65 %) [INFERRED: one dominant MCS/format; to split in Task 5].
- Caveat: a systematic overestimate of snr_l would shift the curves and is not separable here.
