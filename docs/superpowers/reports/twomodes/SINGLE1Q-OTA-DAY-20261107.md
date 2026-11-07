# twomodes/single1-q vs twomodes/single1: day OTA A/B, sensing mode, cells 415 and 923, 2026-11-07

Label for every number: [MEASURED OTA, DGX GB10 + X410 34B2AF1, 4 RX], daytime 2026-11-07 (campaign dir /home/nicola/NICOLA/ota_runs/q1camp_20261107_141431, 14:14-16:23 CET), sensing mode, 240 s per arm, `tests/passive_rx/sensing/sensing_ota.dgx.cfg` (identical in both worktrees, diffed; `sense_min_prb` not set, so each build uses its own default), env `MPCI_20261025.env` + ISAC_SIB1_CACHE=0 (the runner's env, as SINGLE1-OTA-20261106), tune 3680010000:1171:-4827:217 (415) and 3759990000:233:-4832:217 (923), locks heavy.lock + td_measure.lock as the runner. BASE = rr-single1 `0b72a1ecea` (wide limit 128). NEW = rr-single1q `4ff0b59f80` (wide limit 51, adaptive per-PRB tests). Neither was rebuilt. ISAC_RX_BRANCH_FO unset; every receiver stopped by SIGINT (all RX_EXIT=130).

The 2026-11-06 evening numbers (SINGLE1-OTA-20261106.md, BASE rr-twomodes `f6c6470ae7`, NEW rr-single1 `db9845f48c`, evening load) are a different campaign with different builds and load; they are quoted only in the side-by-side section, labelled, never merged.

## Campaign course and deviations (read first)

- Planned order per cell B N N B N B B N B N. Executed as planned for both cells, except the cell order and the 415 start (below). Tags in runs.tsv: 923 `q1_923*`; 415 `q1_415*x3` (third round).
- **Cell 415 was not on air at the 415 tune in the first part of the afternoon.** 13:59-14:14 (first attempt, 3 arms, `sp_ota_q1_first_attempt.sh`, campaign q1camp_20261107_135922) and again 15:06 and 15:11 the receiver synced PCI 442 (SSB candidates 442 / 994 / 211 with 442 at SSS ~2.6e5; 415 absent, while in the 2026-11-07 10:08 run and the evening run 415 had SSS ~1.1e6). These 5 arms are MISMATCH and are excluded (a different cell; PCI 442 data is not merged and not analysed). The campaign script was restarted with cell 923 first and a gate: a cell whose first arm does not reach the target PCI is retried in a later round. 415 returned at 15:26 (round 3) and its 10 arms ran 15:26-16:16. So 923 ran 14:14-15:05 and 415 ran 15:26-16:16 (different time of day: 923 and 415 are never merged anyway).
- The "rerun failed arms once at the end" rule did not trigger: no arm failed after a cell had started producing good arms; the 415 PCI-mismatch probes are the failed first arms and the retry is the round-3 arm B1 (each order slot of the B N N B N B B N B N sequence has exactly one valid arm).
- Traffic sanity arms (item 7) ran on 415, 120 s each, after the sensing arms.

## Integrity checks (every NEW arm)

- "SENSE_SCHED thread not started": 0 in all 20 arms (and 0 in all 20 BASE arms). pthread_create failures: 0 everywhere.
- Records without quality (valid_cfr_prbs absent): **non-zero in every NEW arm of 415 and in 1 of 5 NEW arms of 923 (and in BASE arms too, 3-11 pdsch_data per arm)**. Literal reading of the instruction makes these arms invalid. In every arm the count per source is exactly equal to that source's `sense_cfr_drop_<src>` (CFR ring full: the record is exported without the CFR, hence without quality), see "integrity per arm" tables. So this is the known CFR-drop path, not the quality code not running. NEW 415 has more of them than BASE 415 (sense_cfr_drop 252 (142-446) vs 71 (56-82), outside the BASE spread, "above"); 923 has 0.2 vs 9.2. They are reported here at once as the operator asked; arms are NOT excluded from the tables (the literal rule would leave 0 valid NEW arms in 415), the decision is the operator's.
- No sync loss (acq_sync_losses 0) in any arm.

## Definitions

Rates are per second of the rs-record span. Wide grant = EVMGRANT (probe=0, de-duplicated per rnti+frame.slot, best crc/cb_ok kept) with allocation >= T PRB, T = 51 or 128. Proven = TB CRC-ok or partial-TB proven (crc=0, cb_total>1, cb_ok>=1). A record belongs to a grant if (rnti, abs_slot mod 20480) match. "Yield" = fraction of proven wide grants that have at least one pdsch_dmrs/pdsch_data record with valid_cfr_prbs >= 51 (resp. >= 128), the same raw field on BASE and NEW. Layers: pdsch_dmrs split by `est_layers`, pdsch_data by `n_layers`; `Lna` = key absent. inval = the records' `inval` field (PRBs removed per rule), shown per 1000 records with nb_rb >= T; BASE prints inval only for nb_rb >= 128, NEW for >= 51, so use the nb128 rows to compare builds. **SENSE_WBQ wide/short counts are NOT comparable between builds: the limit is 128 on BASE and 51 on NEW, so "wide" and "short" count different record populations.** n = 5 arms per build and cell; mean (min-max). Offered load swings between arms, so per-grant yields are the load-independent numbers.

Cell tables below are produced by `.superpowers/tmp/tab_q1.py` (items 1-4, from `m_q1.py`); the SINGLE1-comparable tables by `tab2_q1.py` and `sum_q1.py` (same metrics as the 2026-11-06 report).

## Items 1-4 per cell (m_q1.py / tab_q1.py)
Excluded / flagged arms: [('q1_415B1', '/home/nicola/NICOLA/ota_runs/q1_415B1_1_150612', 'MISMATCH', ''), ('q1_415B1x2', '/home/nicola/NICOLA/ota_runs/q1_415B1x2_1_151121', 'MISMATCH', '')]

## Cell 415 (Vodafone-labelled in earlier reports; PCI 415, synced PCI verified = 415 in every arm)

### 1. headline: records/s with valid PRBs >= 51 / >= 128 (rate = count / rs-record span)
| source/layers | BASE | NEW |
|---|---|---|
| pdsch_data/L2 v>=51 /s | 15.03 (4.46-23.91) | 21.30 (9.01-43.32) |
| pdsch_data/L2 v>=128 /s | 7.24 (2.38-14.82) | 9.49 (5.00-14.12) |
| pdsch_data/L3 v>=51 /s | 0.00 (0.00-0.00) | 2.97 (1.84-4.33) |
| pdsch_data/L3 v>=128 /s | 0.00 (0.00-0.00) | 2.06 (1.16-3.61) |
| pdsch_data/L4 v>=51 /s | 0.00 (0.00-0.00) | 0.92 (0.09-3.19) |
| pdsch_data/L4 v>=128 /s | 0.00 (0.00-0.00) | 0.51 (0.00-2.16) |
| pdsch_data/Lall v>=51 /s | 16.95 (4.65-27.66) | 27.45 (13.49-49.63) |
| pdsch_data/Lall v>=128 /s | 8.18 (2.46-17.24) | 13.16 (7.49-21.19) |
| pdsch_data/Lna v>=51 /s | 1.92 (0.19-3.75) | 2.26 (0.34-6.55) |
| pdsch_data/Lna v>=128 /s | 0.94 (0.08-2.43) | 1.11 (0.13-3.67) |
| pdsch_dmrs/L1 v>=51 /s | 2.04 (0.32-4.91) | 4.23 (0.97-10.38) |
| pdsch_dmrs/L1 v>=128 /s | 1.43 (0.23-3.62) | 2.99 (0.73-7.70) |
| pdsch_dmrs/L2 v>=51 /s | 10.28 (3.61-16.87) | 13.87 (7.49-17.41) |
| pdsch_dmrs/L2 v>=128 /s | 6.61 (2.34-11.78) | 9.13 (5.01-12.06) |
| pdsch_dmrs/L3 v>=51 /s | 3.77 (0.99-5.53) | 2.92 (1.17-5.10) |
| pdsch_dmrs/L3 v>=128 /s | 2.60 (0.61-3.96) | 1.97 (0.90-3.60) |
| pdsch_dmrs/L4 v>=51 /s | 0.67 (0.03-1.43) | 0.60 (0.14-2.28) |
| pdsch_dmrs/L4 v>=128 /s | 0.44 (0.00-1.02) | 0.40 (0.04-1.62) |
| pdsch_dmrs/Lall v>=51 /s | 17.19 (5.46-26.82) | 21.89 (11.94-31.06) |
| pdsch_dmrs/Lall v>=128 /s | 11.30 (3.44-18.83) | 14.63 (7.91-22.42) |
| pdsch_dmrs/Lna v>=51 /s | 0.43 (0.11-0.91) | 0.26 (0.13-0.39) |
| pdsch_dmrs/Lna v>=128 /s | 0.22 (0.07-0.42) | 0.14 (0.07-0.19) |

### 1b. proven wide grants (CRC-ok or partial-TB proven) with a pdsch record of valid >= 51 / >= 128
| allocation / record threshold | BASE | NEW |
|---|---|---|
| alloc51 grants proven (n/arm) | 5243 (1393-8380) | 6322 (3476-9129) |
| alloc51: any pdsch record | 100.0 (100.0-100.0) | 100.0 (99.9-100.0) |
| alloc51: record valid>=51 % | 74.0 (71.1-78.1) | 81.8 (76.7-85.5) |
| alloc51: record valid>=128 % | 44.4 (33.4-54.8) | 51.3 (37.8-62.2) |
| alloc128 grants proven (n/arm) | 3439 (872-6229) | 4196 (2494-7104) |
| alloc128: any pdsch record | 100.0 (100.0-100.0) | 100.0 (100.0-100.0) |
| alloc128: record valid>=51 % | 85.8 (82.8-88.4) | 90.9 (89.5-92.0) |
| alloc128: record valid>=128 % | 68.5 (63.2-73.7) | 75.8 (70.8-79.6) |

### 2. per-rule PRBs removed in wide records (records' inval; per 1000 records with nb_rb>=T) and SENSE_WBQ last line
| item | BASE | NEW |
|---|---|---|
| inval pdsch_dmrs/nb128 records n/arm | 2487 (769-4043) | 3172 (1738-4772) |
| inval pdsch_dmrs/nb128 records with any removal % | 28.4 (14.9-37.6) | 91.1 (88.1-94.0) |
| inval pdsch_dmrs/nb128 PRBs removed by absent /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by da_partial /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by few_re /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by nosig /1000 rec | 0 (0-1) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by recon /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by resid /1000 rec | 0 (0-0) | 5476 (3630-6488) |
| inval pdsch_dmrs/nb128 PRBs removed by snr /1000 rec | 7306 (3797-10625) | 1725 (388-2800) |
| inval pdsch_data/nb128 records n/arm | 3074 (630-5906) | 3008 (1703-4901) |
| inval pdsch_data/nb128 records with any removal % | 64.1 (49.2-73.0) | 20.1 (10.7-35.2) |
| inval pdsch_data/nb128 PRBs removed by absent /1000 rec | 216 (0-321) | 137 (17-372) |
| inval pdsch_data/nb128 PRBs removed by da_partial /1000 rec | 2212 (637-3802) | 1368 (158-2691) |
| inval pdsch_data/nb128 PRBs removed by few_re /1000 rec | 67760 (19929-89057) | 0 (0-1) |
| inval pdsch_data/nb128 PRBs removed by nosig /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_data/nb128 PRBs removed by recon /1000 rec | 46 (8-105) | 99 (49-145) |
| inval pdsch_data/nb128 PRBs removed by resid /1000 rec | 229 (24-408) | 4646 (1158-11598) |
| inval pdsch_data/nb128 PRBs removed by snr /1000 rec | 2572 (1908-3192) | 2689 (1945-3286) |
| inval pdsch_dmrs/nb51 records n/arm | 3751 (1211-5746) | 4693 (2598-6553) |
| inval pdsch_dmrs/nb51 records with any removal % | 18.3 (10.5-23.9) | 83.9 (80.7-85.8) |
| inval pdsch_dmrs/nb51 PRBs removed by absent /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by da_partial /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by few_re /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by nosig /1000 rec | 0 (0-1) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by recon /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by resid /1000 rec | 0 (0-0) | 4401 (2982-5235) |
| inval pdsch_dmrs/nb51 PRBs removed by snr /1000 rec | 4747 (2672-6667) | 1519 (352-2385) |
| inval pdsch_data/nb51 records n/arm | 5446 (1239-8821) | 5931 (2964-10589) |
| inval pdsch_data/nb51 records with any removal % | 34.7 (25.0-42.8) | 16.3 (8.5-32.1) |
| inval pdsch_data/nb51 PRBs removed by absent /1000 rec | 119 (0-210) | 123 (32-266) |
| inval pdsch_data/nb51 PRBs removed by da_partial /1000 rec | 1131 (426-1933) | 662 (124-989) |
| inval pdsch_data/nb51 PRBs removed by few_re /1000 rec | 36841 (10133-51161) | 0 (0-0) |
| inval pdsch_data/nb51 PRBs removed by nosig /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_data/nb51 PRBs removed by recon /1000 rec | 27 (4-70) | 70 (34-123) |
| inval pdsch_data/nb51 PRBs removed by resid /1000 rec | 129 (12-273) | 3313 (1000-9138) |
| inval pdsch_data/nb51 PRBs removed by snr /1000 rec | 1412 (820-1797) | 1727 (805-2267) |
| WBQ wide (limit: BASE 128 / NEW 51, NOT comparable) | 5503 (1194-9905) | 10592 (5551-16725) |
| WBQ short (limit: BASE 128 / NEW 51, NOT comparable) | 1401 (106-2290) | 144 (61-243) |
| WBQ prbs absent (hit/first) | 777 (0-1643) | 202 (0-462) |
| WBQ prbs da_partial (hit/first) | 4376 (1197-6282) | 186 (46-334) |
| WBQ prbs few_re (hit/first) | 226382 (10726-361911) | 0 (0-0) |
| WBQ prbs nosig (hit/first) | 0 (0-2) | 0 (0-0) |
| WBQ prbs recon (hit/first) | 44 (0-157) | 14 (0-28) |
| WBQ prbs resid (hit/first) | 443 (0-1372) | 6249 (2309-14479) |
| WBQ prbs snr (hit/first) | 12605 (3624-19021) | 8802 (4839-14468) |
| WBQ prbs sym (hit/first) | 0 (0-0) | 0 (0-0) |
| WBQ prbs trunc (hit/first) | 0 (0-0) | 0 (0-0) |

### 3. median snr_db_p10 (dB), records with valid >= 51 (v51) / >= 128 (v128)
| source | BASE | NEW |
|---|---|---|
| pdsch_dmrs/Lall v51 | 11.10 (9.32-13.86) | 17.56 (14.93-20.67) |
| pdsch_dmrs/Lall v128 | 11.57 (9.76-13.90) | 18.36 (15.63-22.34) |
| pdsch_data/Lall v51 | 11.42 (10.46-12.52) | 13.61 (11.98-15.62) |
| pdsch_data/Lall v128 | 11.40 (10.52-12.84) | 13.72 (12.09-16.33) |

### 4. decode, drops, CPU
| item | BASE | NEW |
|---|---|---|
| wide(>=128) grants decoded/s | 21.54 (7.13-34.39) | 25.97 (14.84-37.37) |
| wide(>=128) CRC-ok/s | 14.58 (3.32-27.51) | 18.32 (10.52-32.85) |
| wide(>=128) partial-TB proven/s | 1.56 (0.71-1.92) | 1.46 (0.89-1.92) |
| wide(>=51) grants decoded/s | 33.85 (11.65-47.70) | 40.40 (21.90-66.46) |
| wide(>=51) CRC-ok/s | 22.18 (5.32-36.82) | 27.20 (14.45-41.83) |
| wide(>=51) partial-TB proven/s | 2.40 (1.12-3.02) | 2.59 (1.35-5.49) |
| sense_rec_drop (record ring/writer) count | 0 (0-0) | 0 (0-0) |
| sense_io_errors | 0 (0-0) | 0 (0-0) |
| CFR ring full drops pdsch_dmrs (count) | 1 (0-3) | 13 (4-27) |
| DA async drop pdsch_dmrs % of queued | 0.00 (0.00-0.00) | 0.00 (0.00-0.00) |
| CFR ring full drops pdsch_data (count) | 7 (3-11) | 34 (14-53) |
| DA async drop pdsch_data % of queued | 1.12 (0.12-3.90) | 1.38 (0.15-3.61) |
| sense_cfr_drop total (count) | 71 (56-82) | 252 (142-446) |
| scanq drop % (full/stale/epoch) | 0.14 (0.02-0.41) | 0.09 (0.03-0.23) |
| scanq full % | 0.023 (0.000-0.064) | 0.008 (0.000-0.015) |
| scanq stale % | 0.119 (0.006-0.348) | 0.086 (0.018-0.223) |
| pdschq full % | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) |
| pdschq stale % | 0.459 (0.076-1.031) | 0.599 (0.147-1.148) |
| CPU cores | 7.27 (7.13-7.47) | 7.31 (7.19-7.53) |

### integrity per arm
| arm | sched_fail | pthread | noqual pdsch_dmrs / cfr_drop | noqual pdsch_data / cfr_drop | noqual all sources | sync_losses |
|---|---|---|---|---|---|---|
| q1_415B1x3 | 0 | 0 | 0 / 0 | 3 / 3 | {'ssb': 0, 'pdsch_data': 3, 'csirs': 53, 'prach': 56, 'pdsch_dmrs': 0, 'pusch_dmrs': 0, 'srs': 0} | 0 |
| q1_415B2x3 | 0 | 0 | 1 / 1 | 11 / 11 | {'ssb': 0, 'csirs': 63, 'pdsch_data': 11, 'srs': 0, 'pdsch_dmrs': 1, 'pusch_dmrs': 0, 'prach': 3} | 0 |
| q1_415B3x3 | 0 | 0 | 0 / 0 | 5 / 5 | {'ssb': 0, 'pdsch_data': 5, 'csirs': 64, 'pdsch_dmrs': 0, 'srs': 0, 'pusch_dmrs': 1, 'prach': 11} | 0 |
| q1_415B4x3 | 0 | 0 | 3 / 3 | 4 / 4 | {'ssb': 0, 'csirs': 64, 'pdsch_data': 4, 'srs': 0, 'pdsch_dmrs': 3, 'pusch_dmrs': 0, 'prach': 2} | 0 |
| q1_415B5x3 | 0 | 0 | 3 / 3 | 10 / 10 | {'ssb': 0, 'prach': 55, 'csirs': 63, 'srs': 0, 'pdsch_data': 10, 'pdsch_dmrs': 3, 'pusch_dmrs': 6} | 0 |
| q1_415N1x3 | 0 | 0 | 12 / 12 | 36 / 36 | {'ssb': 11, 'csirs': 177, 'pdsch_dmrs': 12, 'pdsch_data': 36, 'pusch_dmrs': 8, 'srs': 0, 'prach': 2} | 0 |
| q1_415N2x3 | 0 | 0 | 7 / 7 | 39 / 39 | {'ssb': 12, 'pdsch_data': 39, 'csirs': 142, 'pdsch_dmrs': 7, 'pusch_dmrs': 2, 'prach': 30, 'srs': 0} | 0 |
| q1_415N3x3 | 0 | 0 | 4 / 4 | 14 / 14 | {'ssb': 3, 'pdsch_data': 14, 'csirs': 121, 'prach': 1, 'srs': 0, 'pdsch_dmrs': 4, 'pusch_dmrs': 0} | 0 |
| q1_415N4x3 | 0 | 0 | 27 / 27 | 28 / 28 | {'ssb': 20, 'csirs': 152, 'pdsch_data': 28, 'pdsch_dmrs': 27, 'pusch_dmrs': 0, 'prach': 4} | 0 |
| q1_415N5x3 | 0 | 0 | 13 / 13 | 53 / 53 | {'ssb': 0, 'csirs': 370, 'pdsch_data': 53, 'pdsch_dmrs': 13, 'pusch_dmrs': 10, 'srs': 0, 'prach': 31} | 0 |

## Cell 923 (PCI 923)

### 1. headline: records/s with valid PRBs >= 51 / >= 128 (rate = count / rs-record span)
| source/layers | BASE | NEW |
|---|---|---|
| pdsch_data/L2 v>=51 /s | 4.69 (0.45-10.71) | 12.18 (1.50-31.79) |
| pdsch_data/L2 v>=128 /s | 2.96 (0.09-8.65) | 8.13 (0.49-20.42) |
| pdsch_data/L3 v>=51 /s | 0.00 (0.00-0.00) | 1.75 (0.88-3.69) |
| pdsch_data/L3 v>=128 /s | 0.00 (0.00-0.00) | 1.13 (0.61-2.73) |
| pdsch_data/L4 v>=51 /s | 0.00 (0.00-0.00) | 0.22 (0.04-0.45) |
| pdsch_data/L4 v>=128 /s | 0.00 (0.00-0.00) | 0.17 (0.02-0.37) |
| pdsch_data/Lall v>=51 /s | 6.08 (1.79-13.45) | 15.83 (4.38-36.54) |
| pdsch_data/Lall v>=128 /s | 3.59 (0.86-9.63) | 10.26 (2.11-23.63) |
| pdsch_data/Lna v>=51 /s | 1.39 (0.25-2.75) | 1.70 (1.00-2.90) |
| pdsch_data/Lna v>=128 /s | 0.62 (0.12-0.99) | 0.83 (0.30-1.82) |
| pdsch_dmrs/L1 v>=51 /s | 1.75 (0.38-3.12) | 2.40 (1.64-3.88) |
| pdsch_dmrs/L1 v>=128 /s | 0.78 (0.16-1.47) | 1.32 (0.77-2.51) |
| pdsch_dmrs/L2 v>=51 /s | 10.44 (4.40-18.21) | 12.17 (3.64-25.27) |
| pdsch_dmrs/L2 v>=128 /s | 6.47 (2.22-12.76) | 7.41 (2.40-15.03) |
| pdsch_dmrs/L3 v>=51 /s | 4.14 (1.56-10.08) | 11.45 (2.42-40.79) |
| pdsch_dmrs/L3 v>=128 /s | 2.25 (0.91-6.09) | 5.20 (1.19-16.28) |
| pdsch_dmrs/L4 v>=51 /s | 1.33 (0.34-3.11) | 0.88 (0.30-1.77) |
| pdsch_dmrs/L4 v>=128 /s | 0.83 (0.11-2.09) | 0.58 (0.12-1.25) |
| pdsch_dmrs/Lall v>=51 /s | 18.48 (9.46-33.01) | 27.94 (10.84-52.85) |
| pdsch_dmrs/Lall v>=128 /s | 10.70 (5.06-20.88) | 14.93 (6.22-21.50) |
| pdsch_dmrs/Lna v>=51 /s | 0.80 (0.40-1.17) | 1.04 (0.36-1.93) |
| pdsch_dmrs/Lna v>=128 /s | 0.38 (0.14-0.57) | 0.42 (0.13-0.59) |

### 1b. proven wide grants (CRC-ok or partial-TB proven) with a pdsch record of valid >= 51 / >= 128
| allocation / record threshold | BASE | NEW |
|---|---|---|
| alloc51 grants proven (n/arm) | 1987 (390-5002) | 1976 (702-4507) |
| alloc51: any pdsch record | 100.0 (100.0-100.0) | 100.0 (100.0-100.0) |
| alloc51: record valid>=51 % | 93.8 (91.5-97.2) | 98.5 (97.9-99.3) |
| alloc51: record valid>=128 % | 57.7 (46.0-73.3) | 65.1 (50.6-77.5) |
| alloc128 grants proven (n/arm) | 1476 (205-4167) | 1423 (430-3128) |
| alloc128: any pdsch record | 100.0 (100.0-100.0) | 100.0 (100.0-100.0) |
| alloc128: record valid>=51 % | 95.3 (92.1-99.5) | 98.8 (98.3-99.4) |
| alloc128: record valid>=128 % | 87.2 (78.6-95.6) | 91.8 (82.1-96.6) |

### 2. per-rule PRBs removed in wide records (records' inval; per 1000 records with nb_rb>=T) and SENSE_WBQ last line
| item | BASE | NEW |
|---|---|---|
| inval pdsch_dmrs/nb128 records n/arm | 2868 (1622-5616) | 3622 (1470-5690) |
| inval pdsch_dmrs/nb128 records with any removal % | 51.1 (28.4-74.2) | 98.1 (97.0-99.0) |
| inval pdsch_dmrs/nb128 PRBs removed by absent /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by da_partial /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by few_re /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by nosig /1000 rec | 0 (0-1) | 1 (0-3) |
| inval pdsch_dmrs/nb128 PRBs removed by recon /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb128 PRBs removed by resid /1000 rec | 0 (0-0) | 9852 (7673-11210) |
| inval pdsch_dmrs/nb128 PRBs removed by snr /1000 rec | 36558 (11771-59499) | 11354 (3846-21766) |
| inval pdsch_data/nb128 records n/arm | 2389 (238-6878) | 2561 (581-5383) |
| inval pdsch_data/nb128 records with any removal % | 76.3 (39.9-88.0) | 52.4 (25.0-67.9) |
| inval pdsch_data/nb128 PRBs removed by absent /1000 rec | 7034 (244-13171) | 12741 (2460-31265) |
| inval pdsch_data/nb128 PRBs removed by da_partial /1000 rec | 8983 (5868-11862) | 5060 (3219-7715) |
| inval pdsch_data/nb128 PRBs removed by few_re /1000 rec | 92938 (28782-124982) | 2 (0-7) |
| inval pdsch_data/nb128 PRBs removed by nosig /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_data/nb128 PRBs removed by recon /1000 rec | 598 (51-1451) | 941 (89-3177) |
| inval pdsch_data/nb128 PRBs removed by resid /1000 rec | 954 (261-2519) | 1929 (268-5725) |
| inval pdsch_data/nb128 PRBs removed by snr /1000 rec | 1522 (408-2063) | 12539 (3661-23882) |
| inval pdsch_dmrs/nb51 records n/arm | 4533 (2389-7960) | 6254 (2372-12332) |
| inval pdsch_dmrs/nb51 records with any removal % | 33.2 (14.3-50.4) | 89.6 (84.3-91.9) |
| inval pdsch_dmrs/nb51 PRBs removed by absent /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by da_partial /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by few_re /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by nosig /1000 rec | 0 (0-1) | 1 (0-2) |
| inval pdsch_dmrs/nb51 PRBs removed by recon /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_dmrs/nb51 PRBs removed by resid /1000 rec | 0 (0-0) | 7115 (5382-8121) |
| inval pdsch_dmrs/nb51 PRBs removed by snr /1000 rec | 23669 (7851-40396) | 8293 (3232-16040) |
| inval pdsch_data/nb51 records n/arm | 3288 (489-8259) | 3554 (1012-7981) |
| inval pdsch_data/nb51 records with any removal % | 50.4 (19.4-73.3) | 47.1 (22.7-65.3) |
| inval pdsch_data/nb51 PRBs removed by absent /1000 rec | 5082 (119-10048) | 10003 (2044-22100) |
| inval pdsch_data/nb51 PRBs removed by da_partial /1000 rec | 5702 (3280-7843) | 3902 (2229-6358) |
| inval pdsch_data/nb51 PRBs removed by few_re /1000 rec | 62842 (14008-104084) | 1 (0-4) |
| inval pdsch_data/nb51 PRBs removed by nosig /1000 rec | 0 (0-0) | 0 (0-0) |
| inval pdsch_data/nb51 PRBs removed by recon /1000 rec | 334 (39-858) | 830 (144-2432) |
| inval pdsch_data/nb51 PRBs removed by resid /1000 rec | 583 (143-1489) | 1564 (358-4238) |
| inval pdsch_data/nb51 PRBs removed by snr /1000 rec | 1004 (198-1325) | 9434 (2719-19522) |
| WBQ wide (limit: BASE 128 / NEW 51, NOT comparable) | 5257 (1860-12494) | 9746 (4144-15444) |
| WBQ short (limit: BASE 128 / NEW 51, NOT comparable) | 2208 (588-6026) | 465 (260-1084) |
| WBQ prbs absent (hit/first) | 24148 (58-77267) | 10965 (2563-23990) |
| WBQ prbs da_partial (hit/first) | 17727 (1604-37754) | 2944 (1086-7218) |
| WBQ prbs few_re (hit/first) | 275612 (6850-857961) | 0 (0-0) |
| WBQ prbs nosig (hit/first) | 1 (0-7) | 3 (0-14) |
| WBQ prbs recon (hit/first) | 219 (18-535) | 72 (3-281) |
| WBQ prbs resid (hit/first) | 641 (70-2217) | 1907 (620-5508) |
| WBQ prbs snr (hit/first) | 80115 (21599-159303) | 27852 (15712-42443) |
| WBQ prbs sym (hit/first) | 0 (0-0) | 0 (0-0) |
| WBQ prbs trunc (hit/first) | 0 (0-0) | 0 (0-0) |

### 3. median snr_db_p10 (dB), records with valid >= 51 (v51) / >= 128 (v128)
| source | BASE | NEW |
|---|---|---|
| pdsch_dmrs/Lall v51 | 8.44 (6.21-10.74) | 11.39 (7.42-14.26) |
| pdsch_dmrs/Lall v128 | 8.49 (6.06-10.57) | 11.18 (7.32-14.39) |
| pdsch_data/Lall v51 | 11.96 (9.89-15.47) | 11.33 (8.34-13.01) |
| pdsch_data/Lall v128 | 12.05 (9.31-17.92) | 10.74 (7.93-13.02) |

### 4. decode, drops, CPU
| item | BASE | NEW |
|---|---|---|
| wide(>=128) grants decoded/s | 15.96 (8.35-31.27) | 17.87 (8.01-25.62) |
| wide(>=128) CRC-ok/s | 4.74 (0.81-14.49) | 4.99 (1.55-11.65) |
| wide(>=128) partial-TB proven/s | 2.19 (0.15-5.16) | 1.69 (0.42-3.87) |
| wide(>=51) grants decoded/s | 23.11 (11.43-41.05) | 26.72 (12.29-39.42) |
| wide(>=51) CRC-ok/s | 6.70 (1.56-17.88) | 7.18 (2.65-17.02) |
| wide(>=51) partial-TB proven/s | 2.62 (0.26-5.71) | 2.10 (0.55-4.37) |
| sense_rec_drop (record ring/writer) count | 0 (0-0) | 0 (0-0) |
| sense_io_errors | 0 (0-0) | 0 (0-0) |
| CFR ring full drops pdsch_dmrs (count) | 2 (0-8) | 0 (0-1) |
| DA async drop pdsch_dmrs % of queued | 0.00 (0.00-0.00) | 0.00 (0.00-0.00) |
| CFR ring full drops pdsch_data (count) | 7 (0-34) | 0 (0-0) |
| DA async drop pdsch_data % of queued | 1.57 (0.24-4.49) | 0.78 (0.34-1.32) |
| sense_cfr_drop total (count) | 9 (0-46) | 0 (0-1) |
| scanq drop % (full/stale/epoch) | 0.69 (0.02-2.20) | 0.74 (0.02-3.47) |
| scanq full % | 0.207 (0.010-0.664) | 0.212 (0.001-1.034) |
| scanq stale % | 0.486 (0.008-1.539) | 0.526 (0.012-2.437) |
| pdschq full % | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) |
| pdschq stale % | 0.352 (0.128-0.557) | 0.480 (0.156-0.729) |
| CPU cores | 7.57 (7.41-7.80) | 7.41 (7.09-7.72) |

### integrity per arm
| arm | sched_fail | pthread | noqual pdsch_dmrs / cfr_drop | noqual pdsch_data / cfr_drop | noqual all sources | sync_losses |
|---|---|---|---|---|---|---|
| q1_923B1 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'pdsch_data': 0, 'csirs': 0, 'pdsch_dmrs': 0, 'pusch_dmrs': 0, 'prach': 45, 'srs': 0} | 0 |
| q1_923B2 | 0 | 0 | 8 / 8 | 34 / 34 | {'ssb': 0, 'csirs': 4, 'pdsch_data': 34, 'srs': 0, 'pdsch_dmrs': 8, 'pusch_dmrs': 0, 'prach': 6} | 0 |
| q1_923B3 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'csirs': 0, 'pdsch_data': 0, 'prach': 3, 'srs': 0, 'pdsch_dmrs': 0, 'pusch_dmrs': 0} | 0 |
| q1_923B4 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'csirs': 0, 'pdsch_data': 0, 'pdsch_dmrs': 0, 'pusch_dmrs': 0, 'prach': 13} | 0 |
| q1_923B5 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'csirs': 0, 'pdsch_data': 0, 'pdsch_dmrs': 0, 'pusch_dmrs': 0, 'prach': 10, 'srs': 0} | 0 |
| q1_923N1 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'csirs': 0, 'pdsch_data': 0, 'pdsch_dmrs': 0, 'pusch_dmrs': 0, 'prach': 31} | 0 |
| q1_923N2 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'csirs': 0, 'pdsch_data': 0, 'prach': 17, 'pdsch_dmrs': 0, 'pusch_dmrs': 0, 'srs': 0} | 0 |
| q1_923N3 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'csirs': 0, 'pdsch_data': 0, 'pdsch_dmrs': 0, 'pusch_dmrs': 0, 'prach': 6, 'srs': 0} | 0 |
| q1_923N4 | 0 | 0 | 0 / 0 | 0 / 0 | {'ssb': 0, 'csirs': 0, 'pdsch_data': 0, 'pdsch_dmrs': 0, 'prach': 5, 'pusch_dmrs': 0} | 0 |
| q1_923N5 | 0 | 0 | 1 / 1 | 0 / 0 | {'ssb': 0, 'pdsch_data': 0, 'csirs': 0, 'pdsch_dmrs': 1, 'pusch_dmrs': 0, 'prach': 8} | 0 |

## Item 5: SINGLE1-OTA-20261106 item set, this campaign (tab2_q1.py)

#### cell 415
base ['q1_415B1x3', 'q1_415B2x3', 'q1_415B3x3', 'q1_415B4x3', 'q1_415B5x3']
fix ['q1_415N1x3', 'q1_415N2x3', 'q1_415N3x3', 'q1_415N4x3', 'q1_415N5x3']
| metric | BASE | NEW |
|---|---|---|
| wide dec/s | 21.54 (7.13-34.39) | 25.97 (14.84-37.37) |
| wide TB crc-ok/s | 14.58 (3.32-27.51) | 18.32 (10.52-32.85) |
| wide partial-TB proven/s | 1.56 (0.71-1.92) | 1.46 (0.89-1.92) |
| wide proven/s | 16.13 (4.03-29.39) | 19.77 (11.55-33.74) |
| dmrs rec/s | 182.22 (111.36-242.25) | 198.32 (141.51-321.40) |
| dmrs nb>=128 /s | 11.66 (3.56-19.08) | 14.95 (8.05-22.67) |
| dmrs valid>=128 /s | 11.30 (3.44-18.83) | 14.63 (7.91-22.42) |
| proven wide w/ dmrs-or-data rec | 1.000 (1.000-1.000) | 1.000 (1.000-1.000) |
| proven wide w/ >=128 valid | 0.685 (0.632-0.737) | 0.758 (0.708-0.796) |
| data rec/s | 307.92 (191.13-454.73) | 305.17 (199.04-383.09) |
| data whole/s | 302.51 (189.13-447.28) | 297.81 (196.67-373.41) |
| data partial/s | 5.42 (1.99-8.52) | 7.37 (2.37-23.57) |
| data valid>=128 /s | 8.18 (2.46-17.24) | 13.16 (7.49-21.19) |
| avail PDSCH 25ms | 0.090 (0.034-0.133) | 0.105 (0.066-0.165) |
| avail PDSCH 100ms | 0.191 (0.086-0.254) | 0.210 (0.140-0.328) |
| avail PDSCH 1s | 0.598 (0.421-0.678) | 0.612 (0.476-0.778) |
| avail ALL 25ms | 0.090 (0.034-0.133) | 0.105 (0.066-0.165) |
| avail ALL 100ms | 0.192 (0.086-0.254) | 0.210 (0.140-0.328) |
| avail ALL 1s | 0.598 (0.421-0.678) | 0.612 (0.476-0.778) |
| lat all p50 us | 770.50 (692.00-833.00) | 798.80 (746.00-897.00) |
| lat all p99 us | 6800.67 (5447.38-9468.66) | 6041.97 (4744.00-7745.90) |
| lat wide p50 us | 1939.80 (1475.00-2397.50) | 1836.00 (1356.00-2346.50) |
| lat wide p99 us | 20549.22 (8776.94-33736.60) | 12459.53 (8824.38-22452.01) |
| cpu cores | 7.27 (7.13-7.47) | 7.31 (7.19-7.53) |

#### cell 923
base ['q1_923B1', 'q1_923B2', 'q1_923B3', 'q1_923B4', 'q1_923B5']
fix ['q1_923N1', 'q1_923N2', 'q1_923N3', 'q1_923N4', 'q1_923N5']
| metric | BASE | NEW |
|---|---|---|
| wide dec/s | 15.96 (8.35-31.27) | 17.87 (8.01-25.62) |
| wide TB crc-ok/s | 4.74 (0.81-14.49) | 4.99 (1.55-11.65) |
| wide partial-TB proven/s | 2.19 (0.15-5.16) | 1.69 (0.42-3.87) |
| wide proven/s | 6.93 (0.95-19.65) | 6.68 (2.01-14.56) |
| dmrs rec/s | 255.01 (221.14-311.98) | 215.01 (91.33-388.58) |
| dmrs nb>=128 /s | 13.44 (7.56-26.49) | 17.01 (6.96-26.56) |
| dmrs valid>=128 /s | 10.70 (5.06-20.88) | 14.93 (6.22-21.50) |
| proven wide w/ dmrs-or-data rec | 1.000 (1.000-1.000) | 1.000 (1.000-1.000) |
| proven wide w/ >=128 valid | 0.872 (0.786-0.956) | 0.918 (0.821-0.966) |
| data rec/s | 453.99 (372.97-600.70) | 331.57 (145.39-402.21) |
| data whole/s | 447.70 (361.95-591.44) | 326.19 (143.20-398.97) |
| data partial/s | 6.29 (2.07-11.01) | 5.38 (1.76-10.08) |
| data valid>=128 /s | 3.59 (0.86-9.63) | 10.26 (2.11-23.63) |
| avail PDSCH 25ms | 0.077 (0.052-0.110) | 0.113 (0.046-0.205) |
| avail PDSCH 100ms | 0.164 (0.110-0.223) | 0.225 (0.108-0.378) |
| avail PDSCH 1s | 0.570 (0.425-0.651) | 0.655 (0.460-0.846) |
| avail ALL 25ms | 0.664 (0.642-0.697) | 0.673 (0.642-0.725) |
| avail ALL 100ms | 0.995 (0.994-0.996) | 0.996 (0.993-0.998) |
| avail ALL 1s | 0.996 (0.995-1.000) | 0.998 (0.995-1.000) |
| lat all p50 us | 719.40 (678.00-751.00) | 757.20 (724.00-775.00) |
| lat all p99 us | 3936.82 (3551.00-4578.39) | 4486.80 (3580.88-5625.58) |
| lat wide p50 us | 2032.90 (1802.00-2285.00) | 1891.10 (1657.50-2105.00) |
| lat wide p99 us | 11208.83 (8383.43-16707.64) | 10139.60 (7885.92-14330.54) |
| cpu cores | 7.57 (7.41-7.80) | 7.41 (7.09-7.72) |


## Item 5 (continued): per-wide-grant rates and drops with NEW-outside-BASE-range test (sum_q1.py)

## 415
| metric | BASE | NEW | NEW mean range vs BASE min-max (all 5 NEW runs) |
|---|---|---|---|
| wide dec/s | 21.536 (7.130-34.388) | 25.966 (14.842-37.368) | inside |
| wide proven/s | 16.134 (4.032-29.395) | 19.774 (11.553-33.744) | inside |
| dmrs v128 per wide grant | 0.514 (0.451-0.551) | 0.561 (0.522-0.600) | inside |
| data v128 per wide grant | 0.361 (0.294-0.501) | 0.504 (0.406-0.567) | inside |
| dmrs v128+data v128 per wide grant | 0.875 (0.745-1.049) | 1.064 (0.986-1.167) | inside |
| scanq drop % (full+stale+epoch) | 0.142 (0.016-0.412) | 0.094 (0.032-0.235) | inside |
| scanq full % | 0.023 (0.000-0.064) | 0.008 (0.000-0.015) | inside |
| scanq stale % | 0.119 (0.006-0.348) | 0.086 (0.018-0.223) | inside |
| pdschq full % | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) | inside |
| pdschq stale % | 0.459 (0.076-1.031) | 0.599 (0.147-1.148) | inside |
| sense ring rec drop /1e6 recs | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) | inside |
| sense_cfr_drop (count) | 70.800 (56.000-82.000) | 252.200 (142.000-446.000) | above |
| DA async drop pdsch_data % of queued | 1.119 (0.124-3.898) | 1.382 (0.149-3.614) | inside |
| DA async drop pdsch_dmrs (count) | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) | inside |
| records total (sense_rec_count) | 5077.841 (4475.911-5669.143) | 5214.569 (4604.399-5830.181) | inside |
| lat wide p50 ms | 1.940 (1.475-2.397) | 1.836 (1.356-2.346) | inside |
| lat wide p99 ms | 20.549 (8.777-33.737) | 12.460 (8.824-22.452) | inside |
| lat all p50 ms | 0.770 (0.692-0.833) | 0.799 (0.746-0.897) | inside |
| lat all p99 ms | 6.801 (5.447-9.469) | 6.042 (4.744-7.746) | inside |
base cfr_est all dmrs recs (5 runs summed): {'decoder_seg': 78486, 'decoder': 115287, 'own': 678} | v128: {'decoder': 9027, 'decoder_seg': 2788, 'own': 238} | partial_tb dmrs recs 4245 of which v128 787
base funnel proven wide (summed, 5 runs): {'v128': 11941, 'lt128': 5255} tot 17196
fix cfr_est all dmrs recs (5 runs summed): {'decoder': 110038, 'decoder_seg': 100247, 'own': 466} | v128: {'decoder': 11920, 'decoder_seg': 3460, 'own': 145} | partial_tb dmrs recs 5676 of which v128 719
fix funnel proven wide (summed, 5 runs): {'v128': 16074, 'lt128': 4908} tot 20982
pdsch_data partial/whole per s: NEW 7.368 (2.365-23.565) 297.806 (196.672-373.406)

## 923
| metric | BASE | NEW | NEW mean range vs BASE min-max (all 5 NEW runs) |
|---|---|---|---|
| wide dec/s | 15.962 (8.348-31.266) | 17.868 (8.011-25.623) | inside |
| wide proven/s | 6.930 (0.955-19.654) | 6.684 (2.007-14.562) | inside |
| dmrs v128 per wide grant | 0.664 (0.607-0.779) | 0.821 (0.728-1.035) | inside |
| data v128 per wide grant | 0.193 (0.103-0.308) | 0.533 (0.102-0.922) | inside |
| dmrs v128+data v128 per wide grant | 0.857 (0.710-1.012) | 1.354 (1.137-1.673) | above |
| scanq drop % (full+stale+epoch) | 0.693 (0.021-2.203) | 0.739 (0.021-3.471) | inside |
| scanq full % | 0.207 (0.010-0.664) | 0.212 (0.001-1.034) | inside |
| scanq stale % | 0.486 (0.008-1.539) | 0.526 (0.012-2.437) | inside |
| pdschq full % | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) | inside |
| pdschq stale % | 0.352 (0.128-0.557) | 0.480 (0.156-0.729) | inside |
| sense ring rec drop /1e6 recs | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) | inside |
| sense_cfr_drop (count) | 9.200 (0.000-46.000) | 0.200 (0.000-1.000) | inside |
| DA async drop pdsch_data % of queued | 1.574 (0.235-4.489) | 0.783 (0.344-1.318) | inside |
| DA async drop pdsch_dmrs (count) | 0.000 (0.000-0.000) | 0.000 (0.000-0.000) | inside |
| records total (sense_rec_count) | 3467.248 (2871.702-4077.966) | 2816.808 (1700.201-3704.266) | inside |
| lat wide p50 ms | 2.033 (1.802-2.285) | 1.891 (1.657-2.105) | inside |
| lat wide p99 ms | 11.209 (8.383-16.708) | 10.140 (7.886-14.331) | inside |
| lat all p50 ms | 0.719 (0.678-0.751) | 0.757 (0.724-0.775) | inside |
| lat all p99 ms | 3.937 (3.551-4.578) | 4.487 (3.581-5.626) | inside |
base cfr_est all dmrs recs (5 runs summed): {'decoder_seg': 32888, 'own': 3163, 'decoder': 236766, 'decoder_band': 10} | v128: {'own': 402, 'decoder_seg': 1350, 'decoder': 9672, 'decoder_band': 1} | partial_tb dmrs recs 3943 of which v128 1907
base funnel proven wide (summed, 5 runs): {'v128': 6108, 'lt128': 1274} tot 7382
fix cfr_est all dmrs recs (5 runs summed): {'decoder_seg': 36899, 'own': 3746, 'decoder': 188349, 'decoder_band': 24} | v128: {'decoder': 13300, 'decoder_seg': 2150, 'own': 449} | partial_tb dmrs recs 3022 of which v128 1473
fix funnel proven wide (summed, 5 runs): {'v128': 6747, 'lt128': 369} tot 7116
pdsch_data partial/whole per s: NEW 5.376 (1.756-10.079) 326.192 (143.201-398.966)

## Item 6: scanq stale counter (reported, not investigated)

scanq stale %: 415 BASE 0.119 (0.006-0.348), NEW 0.086 (0.018-0.223); 923 BASE 0.486 (0.008-1.539), NEW 0.526 (0.012-2.437). scanq full %: 415 0.023 / 0.008; 923 0.207 / 0.212 (BASE / NEW). No NEW-vs-BASE difference outside the BASE spread in either cell.

## Item 7: traffic-mode sanity (415, 120 s each, traffic_ota.dgx.cfg, PCI 415 verified, RX_EXIT=130)

- ISAC_METRICS key set: BASE 336 keys, NEW 336 keys, **identical** (set difference empty both ways; last ISAC_METRICS line of each run). Both contain the `sense_*` counters (zero activity), as before.
- No SENSE lines: 0 occurrences of `SENSE_*` tokens (SENSE_SCHED, SENSE_FLOOR, SENSE_WBG, SENSE_WBQ, SENSE_UNITS) in either run.log. The generic `SENSING:` log prefix still appears (it is the passive-RX prefix, e.g. PRG latched, USS_HASH, PDSCH_SOLVER). The records file holds only `dci`, `decode`, `config`, `session` records (BASE 64049 dci / 10816 decode; NEW 74206 / 16676), no `rs` and no `grant` records.
- Process CPU 6.31 (BASE) vs 6.37 cores (NEW); acq_sync_losses 0 in both; scanq stale/full counts BASE 35/40, NEW 24/37 of 144k queued. n = 1 per build.

## Side-by-side with SINGLE1-OTA-20261106 (evening, different builds and load; NOT merged)

| item (label) | 415 BASE | 415 NEW | 923 BASE | 923 NEW |
|---|---|---|---|---|
| wide(>=128) decoded/s, 2026-11-06 evening (twomodes f6c6470ae7 -> single1 db9845f48c) | 97.7 | 72.5 | 13.9 | 12.7 |
| wide(>=128) decoded/s, 2026-11-07 day (single1 0b72a1ecea -> single1-q 4ff0b59f80) | 21.5 | 26.0 | 16.0 | 17.9 |
| proven wide grants with >=128 valid record, 11-06 | 35.5 % | 73.2 % | 32.8 % | 86.4 % |
| proven wide grants with >=128 valid record, 11-07 (alloc128) | 68.5 % | 75.8 % | 87.2 % | 91.8 % |
| pdsch_dmrs valid>=128 /s, 11-06 | 35.2 | 62.9 | 3.01 | 10.3 |
| pdsch_dmrs valid>=128 /s, 11-07 | 11.3 | 14.6 | 10.7 | 14.9 |
| pdsch_data valid>=128 /s, 11-06 | 2.72 | 29.4 | 0.50 | 5.04 |
| pdsch_data valid>=128 /s, 11-07 | 8.18 | 13.2 | 3.59 | 10.3 |
| CPU cores, 11-06 | 7.27 | 7.45 | 7.39 | 7.47 |
| CPU cores, 11-07 | 7.27 | 7.31 | 7.57 | 7.41 |

The 11-07 BASE already contains the 11-06 NEW changes (single1), so the 11-07 BASE -> NEW pair measures only the prbq round (limit 51 + adaptive tests), a smaller step than the 11-06 pair. Offered 415 load was 3-4 times lower today than in the 11-06 evening.

## Reading (counters; inference from them)

- Records with valid >= 51 and >= 128 per second are higher on NEW in both cells for pdsch_data (415: >=51 16.95 -> 27.45, >=128 8.18 -> 13.16; 923: 6.08 -> 15.83, 3.59 -> 10.26) and for pdsch_dmrs in 923 and 415 (>=128: 11.3 -> 14.6, 10.7 -> 14.9), but the arm ranges overlap for pdsch_dmrs (offered-load dependence); the multi-layer pdsch_data rows (L3, L4) exist only on NEW (BASE 0 records with >= 51 valid PRBs).
- Per proven wide grant (load independent): record with >= 51 valid PRBs for alloc>=51 grants 74.0 -> 81.8 % (415) and 93.8 -> 98.5 % (923); >= 128 valid for alloc>=128 grants 68.5 -> 75.8 % (415), 87.2 -> 91.8 % (923); ranges overlap in 415 for the >= 128 case.
- Rule removal (nb128 records): the `few_re` rule (BASE: 67760 PRB/1000 pdsch_data records in 415, 92938 in 923) is gone on NEW (0-2), as designed; `snr` removal in pdsch_dmrs fell (415: 7306 -> 1725; 923: 36558 -> 11354). New on NEW: `resid` (adaptive null test) is now the top pdsch_dmrs rule in 415 (5476/1000 rec) and, with `snr`, in 923 (9852 vs 11354); for pdsch_data the top NEW rule is `resid` in 415 (4646 vs snr 2689) and `absent` (12741) ~ `snr` (12539) in 923. 91 % (415) and 98 % (923) of NEW pdsch_dmrs wide records carry some removal (BASE 28 % and 51 %), mostly a few PRBs each.
- Median snr_db_p10 of the valid PRBs rose on NEW for pdsch_dmrs (415: 11.6 -> 18.4 dB; 923: 8.5 -> 11.2 dB at v>=128), consistent with the changed SNR reference (chest nvar/rho) and not by itself a quality gain; pdsch_data 415 11.4 -> 13.7, 923 12.1 -> 10.7 dB.
- Decode: wide decoded/CRC-ok/partial rates are inside the BASE spread (load-dominated). CPU 7.3-7.6 cores both builds. sense_rec_drop 0 everywhere. Drops that moved: sense_cfr_drop in 415 (above BASE spread; pdsch_data CFR ring-full 7 -> 34, pdsch_dmrs 1 -> 13 per arm); async DA drop on pdsch_data 1.1 -> 1.4 % (415), 1.6 -> 0.8 % (923), inside.

## Tooling (.superpowers/tmp/, suffix _q1)

sp_ota_q1.sh (campaign: 923 first, gate/retry rounds, traffic arms, end reruns), sp_ota_q1_first_attempt.sh (+ .out; the 415-first version that synced PCI 442), m_q1.py (m_s1 + thresholds 51/128, per-layer, yield, inval, WBQ, integrity), tab_q1.py, tab2_q1.py, sum_q1.py, outputs q1_tab_all.txt, q1_tab2.txt, q1_sum.txt, q1_tab_923.txt (partial, superseded). The shared run dirs under ota_runs/ of the 2026-11-06 campaign were not touched except that m_q1.py was run once on s1_415N1_1_204330 as a smoke test and rewrote its m.json as a strict superset of the previous keys.
