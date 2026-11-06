# Single-PCI daytime OTA campaign, 2026-11-06 (FIX twomodes/design 26170ec22c vs BASE rr-sbase b75ae5f8f6)

Label for every number below: **[MEASURED OTA, daytime, DGX GB10 + X410 34B2AF1, 4 RX]**, DEIB Milan NSA cells, 08:30-10:31 local on 2026-11-06, MPCI_20261025.env (as the night campaign), receiver stopped with SIGINT (every valid run RX_EXIT=130), ISAC_RX_BRANCH_FO unset, no receiver code changed, no rebuild, no new env/CLI flag. Cells, modes and arms are never merged. Locks held for the whole campaign: `flock -x /home/nicola/NICOLA/heavy.lock` then `/tmp/td_measure.lock`; `pgrep -x nr-uesoftmodem` was empty before every run (script waits otherwise). Night reference: `SINGLEPCI-OTA-20261105.md` (same cfgs, same tune triples, build f6e91f107e). Load was not measured independently; "daytime" is the wall-clock time, and the DL grant rates below are the only load evidence.

Tooling (copies, originals untouched): `.superpowers/tmp/sp_ota_day.sh` (campaign), `an_sp_day.py` (analyzer; parses both SENSE_SCHED formats), `mk_day.py` (tables), `inv_day.py` (wide-grant coverage and PDSCH admitted-set residency). Campaign dirs `/home/nicola/NICOLA/ota_runs/sp_ota_day_20261106_083020` (415, 923, 403 valid; 508 invalid, see 0) and `sp_ota_day_20261106_100114` (508 rerun). Per run: `run.log`, `an.txt`, `an.json`, `inv.txt`, `cpu_cores.txt`, `score.json`.

## 0. Setup and honest caveats

- Order per cell: T(fix), S(base), S(fix), T(fix), S(fix), S(base). Run names: `sp<PCI><T|S><f|b>[T|S]<n>`, e.g. sp415SbS1 = 415, sensing, base arm, run 1. Each run 240 s. Each arm used its own worktree's cfg (`traffic_ota.dgx.cfg`, `sensing_ota.dgx.cfg`; the sensing cfg is byte-identical in both worktrees) and its own build dir. Base arm only for sensing, as asked. The optional legacy all-sources arm was NOT run.
- Tune triples (unchanged from the script): 415 `3680010000:1171:-4827:217`, 923 `3759990000:233:-4832:217`, 508 `3590010000:193:-4868:162`, 403 `3630000000:18:2178:51`. All valid runs locked the wanted PCI on the first attempt, including **403 (one attempt per mode: T fix, S base, S fix; 403 locked PCI 403 this time, unlike the night when it locked PCI 27)**. 403 reached ACQ CORESET_VERIFIED (not CELL_CONFIGURED; `acq_sib1_decodes` 0 in the metrics line) in all 3 runs.
- **My script bug, first 508 attempt invalid.** In `sp_ota_day.sh` the build-check loop variable `W` overwrote the 508 tune variable `W`, so the 508 cell ran at 09:31-09:46 with tune "/home/nicola/NICOLA/wt/rr-sbase" (12 launches, RX_EXIT=0, sync=0, ~70 s each; the receiver never synced and the radio was not tuned to a cell). These 12 launches are discarded (no data used; dirs `sp508*_1_0931*`-`0943*`, `runs.tsv` MISMATCH). The script was fixed (loop variable renamed, `CELLS=508` filter) and the full 508 sequence was rerun at 10:01-10:31 with the identical order. 508 therefore ran ~30 min after the other cells; no other difference.
- Rates use the span of the run's rs records (about 204-215 s) because sensing records start after sync. Wideband = `valid_cfr_prbs` >= 128. Availability = fraction of 25 ms / 100 ms / 1 s windows containing >= 1 wideband record (score_sensing, min_prb 128).
- Run-to-run variation inside one arm is large (see the two base and two fix runs per cell); one pair of runs per arm cannot separate arm effect from traffic variation except where the effect is an order of magnitude (923 PDSCH DMRS 0 vs 17-22 rec/s).

## 1. Short answer per cell

| cell | traffic | sensing | base vs fix: PDSCH export | base vs fix: wideband availability |
|---|---|---|---|---|
| 415 (3.68 GHz, 217 PRB) | works: CELL_CONFIGURED, 163-240 SOLVED RNTIs, DL CRC 90-91 %, wide-grant (>=128 PRB) DL CRC 86-88 % | works; CSI-RS is 52 PRB only, so wideband only comes from PDSCH | base pdsch_dmrs 2.7 / 9.0 rec/s, pdsch_data 43 / 0; fix pdsch_dmrs 5.6 / 8.3, pdsch_data 15 / 13. Not starved in either arm in this window; fix admits ~2.7x more distinct PDSCH units (33-35 vs 12-13) and keeps one admitted in 96-98 % of decisions vs 48-59 % | both arms ~equally poor: base 1.3 % / 1.1 % (25 ms), fix 2.4 % / 0.5 %; 1 s: base 12.8 / 6.1 %, fix 11.2 / 5.7 % |
| 923 (3.76 GHz, 217 PRB) | works: 137 and 145 SOLVED RNTIs, DL CRC 86.5-91.5 %, wide-grant DL CRC only 63-64 % | works; CSI-RS 217 PRB gives wideband | **starvation reproduced in base, gone in fix**: base pdsch_dmrs 0 / 0 rec/s (PDSCH admitted in 6 and 16 of 53-54 decisions), fix 16.8 / 22.4 rec/s (51 and 49 decisions) | unchanged, set by CSI-RS: 25 ms 0.62-0.67 (base) vs 0.63 / 0.63 (fix); 100 ms and 1 s >= 0.993 everywhere. PDSCH adds 0.96-2.4 wideband rec/s in fix vs 0-0.11 in base |
| 508 (3.59 GHz, 162 PRB) | partial: CELL_CONFIGURED, only fallback DCI 1_0 in T1, 1_1/0_1 DCIs present in T2 but 0 SOLVED; DL CRC 62 % / 47 %; no CSI-RS | works for SSB, pdsch_dmrs (SI-type, 5 PRB median); no CSI-RS, no wideband PDSCH | base 11.2 / 17.4 vs fix 36.0 / 15.3 pdsch_dmrs rec/s; no pdsch_data in any arm; no wide grants exist | none in any arm except SRS 144-160 PRB records in 2 runs (fix S3 44, base S4 36 wb records): availability 0.004-0.005 / 0.9 % / 1.5-2.6 % |
| 403 (3.63 GHz, 51 PRB) | partial: PCI 403 locked, CORESET_VERIFIED only, 48 SOLVED, DL CRC 52.6 %, 0 full-field 1_1 | works (ssb, csirs, pdsch); cell is 51 PRB so wideband (>= 128) is impossible | base pdsch_dmrs 8.1, pdsch_data 16.7 vs fix 23.1, 25.7 rec/s | n/a (0 by construction) |

## 2. Per-cell evidence (tables generated from the run logs)

Notation: `sp<cell><mode><arm><run>`; arm f = fix, b = base. Time columns are local start-end of the receiver run. "other" in the traffic tables is the not-decoded / not-saved counters (`traffic_skip` = DCIs seen but not decoded by policy; `ul_decode_skip` = UL grants not PUSCH-decoded; `e1`/`e3` = layout-search limits `e1_skip_classes`, `e3_skip`).


## Cell 415 (section 2)

### Traffic runs (fix arm)

| run | start-end (local) | CPU cores | DCI rec | RNTIs>=2 DCI (DL+UL) | SOLVED | full-field 1_1 | DL CRC ok/non-probe (%) | DL>=128PRB ok/n (%) | scanq full/stale of queued | pdschq full/stale of queued | other |
|---|---|---|---|---|---|---|---|---|---|---|---|
| sp415TfT1 | 08:30:20-08:34:39 | 6.841 | 201594 | 250 (152+98) | 240 | 13914 of 74484 = 18.7% | 3089/3388 (91.2%) | 669/775 (86.3%) | 52/367 of 336080 (0.12%) | 0/292 of 11336 (2.58%) | traffic_skip 173336, ul_decode_skip 110306, e1 4, e3_skip 130, e3_trunc 31 |
| sp415TfT2 | 08:45:36-08:49:53 | 7.466 | 224042 | 239 (137+102) | 163 | 23068 of 101653 = 22.7% | 4119/4557 (90.4%) | 951/1081 (88.0%) | 56/452 of 336045 (0.15%) | 0/402 of 21355 (1.88%) | traffic_skip 111012, ul_decode_skip 25498, e1 50, e3_skip 114, e3_trunc 385 |

### Sensing runs

| source | run (arm) | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 | drop % (cfr+async) |
|---|---|---|---|---|---|---|
| ssb | sp415SbS1 (base) | 1199.76 | 7/15/20 (21) | 0.000 | 5.7/8.4/15.4 | 0.00 |
| ssb | sp415SfS2 (fix) | 1199.03 | 6/15/20 (21) | 0.000 | 5.7/8.5/15.5 | 0.00 |
| ssb | sp415SfS3 (fix) | 1199.81 | 6/15/20 (21) | 0.000 | 5.7/8.6/13.3 | 0.00 |
| ssb | sp415SbS4 (base) | 1199.69 | 6/15/20 (21) | 0.000 | 5.8/8.4/13.4 | 0.00 |
| csirs | sp415SbS1 (base) | 400.05 | 23/50/52 (53) | 0.000 | 5.1/9.6/21.0 | 0.00 |
| csirs | sp415SfS2 (fix) | 524.38 | 19/50/52 (52) | 0.000 | 4.9/9.3/20.5 | 0.00 |
| csirs | sp415SfS3 (fix) | 541.29 | 28/50/52 (56) | 0.000 | 5.0/10.4/21.5 | 0.02 |
| csirs | sp415SbS4 (base) | 516.85 | 25/49/52 (55) | 0.000 | 4.9/9.9/20.9 | 0.00 |
| pdsch_dmrs | sp415SbS1 (base) | 2.68 | 0/32/217 (217) | 0.692 | 3.9/7.0/14.1 | 0.00 |
| pdsch_dmrs | sp415SfS2 (fix) | 5.59 | 1/32/217 (217) | 1.358 | 4.7/13.2/15.2 | 0.00 |
| pdsch_dmrs | sp415SfS3 (fix) | 8.26 | 1/16/42 (217) | 0.409 | 3.7/14.2/18.2 | 0.00 |
| pdsch_dmrs | sp415SbS4 (base) | 9.04 | 11/22/214 (217) | 1.766 | 7.5/11.7/13.7 | 0.00 |
| pdsch_data | sp415SbS1 (base) | 43.17 | 0/0/48 (217) | 0.341 | 16.8/22.8/26.8 | 0.01 |
| pdsch_data | sp415SfS2 (fix) | 15.47 | 0/16/176 (217) | 2.180 | 10.6/15.8/19.2 | 1.95 |
| pdsch_data | sp415SfS3 (fix) | 12.64 | 0/16/48 (217) | 0.262 | 9.7/15.0/19.0 | 0.04 |
| pdsch_data | sp415SbS4 (base) | 0 | - | - | - | - |
| pusch_dmrs | sp415SbS1 (base) | 0 | - | - | - | - |
| pusch_dmrs | sp415SfS2 (fix) | 29.05 | 0/0/0 (8) | 0.000 | 3.4/5.1/12.8 | 0.00 |
| pusch_dmrs | sp415SfS3 (fix) | 6.41 | 0/0/0 (1) | 0.000 | 3.5/3.5/3.5 | 0.00 |
| pusch_dmrs | sp415SbS4 (base) | 6.67 | 0/0/0 (6) | 0.000 | 3.1/4.2/5.9 | 0.00 |
| pusch_data | sp415SbS1 (base) | 0 | - | - | - | - |
| pusch_data | sp415SfS2 (fix) | 0 | - | - | - | - |
| pusch_data | sp415SfS3 (fix) | 0 | - | - | - | - |
| pusch_data | sp415SbS4 (base) | 0 | - | - | - | - |
| srs | sp415SbS1 (base) | 0.06 | 8/8/8 (8) | 0.000 | 8.4/11.1/15.7 | 0.00 |
| srs | sp415SfS2 (fix) | 0.05 | 3/8/8 (8) | 0.000 | 7.8/17.0/29.5 | 0.00 |
| srs | sp415SfS3 (fix) | 0.10 | 8/8/8 (8) | 0.000 | 13.6/24.5/26.8 | 0.00 |
| srs | sp415SbS4 (base) | 0.07 | 8/8/8 (8) | 0.000 | 18.8/25.1/28.3 | 0.00 |

| run (arm) | time | CPU | avail25ms / 100ms / 1s (wideband) | wb records | gap p95/max s | units enabled series (min-max, mean) | SCHED binding | top | elig mean/max | cold_deferred mean/max | decisions w/ pdsch_dmrs / pdsch_data admitted (of N) | enables/disables |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| sp415SbS1 (base) | 08:35:24-08:39:43 | 7.303 | 0.013 / 0.037 / 0.128 | 218 | 0.009/0.720/38.507 | 0-2, 0.6 | {'pool': 13, 'queue_ul': 35, 'gpu': 6} | n/a | n/a | n/a | 16 / 25 (of 54) | 12/12 |
| sp415SfS2 (fix) | 08:40:29-08:44:50 | 7.186 | 0.024 / 0.043 / 0.112 | 758 | 0.002/0.078/62.449 | 0-6, 3.5 | {'none': 44, 'pool': 3, 'gpu': 3, 'queue_ul': 4} | {'pool': 24, 'queue_dl': 2, 'gpu': 22, 'queue_ul': 3, 'sense_ring': 3} | 33.4/63 | 29.0/58 | 44 / 44 (of 54) | 50/47 |
| sp415SfS3 (fix) | 08:50:38-08:54:57 | 6.984 | 0.005 / 0.013 / 0.057 | 141 | 0.002/2.707/56.155 | 0-7, 4.0 | {'none': 48, 'queue_ul': 2, 'pool': 3} | {'pool': 28, 'queue_ul': 6, 'gpu': 15, 'queue_dl': 3, 'sense_ring': 1} | 41.4/70 | 36.3/66 | 46 / 43 (of 53) | 46/42 |
| sp415SbS4 (base) | 08:55:42-09:00:02 | 6.889 | 0.011 / 0.031 / 0.061 | 377 | 0.001/0.153/1.084 | 0-2, 1.0 | {'pool': 17, 'queue_ul': 37} | n/a | n/a | n/a | 17 / 13 (of 54) | 15/13 |

## Cell 923 (section 2)

### Traffic runs (fix arm)

| run | start-end (local) | CPU cores | DCI rec | RNTIs>=2 DCI (DL+UL) | SOLVED | full-field 1_1 | DL CRC ok/non-probe (%) | DL>=128PRB ok/n (%) | scanq full/stale of queued | pdschq full/stale of queued | other |
|---|---|---|---|---|---|---|---|---|---|---|---|
| sp923TfT1 | 09:00:47-09:05:11 | 7.575 | 198212 | 204 (130+74) | 137 | 13390 of 95510 = 14.0% | 2654/3068 (86.5%) | 252/398 (63.3%) | 46/455 of 336042 (0.15%) | 0/546 of 63364 (0.86%) | traffic_skip 157192, ul_decode_skip 90616, e1 987, e3_skip 18240, e3_trunc 3663 |
| sp923TfT2 | 09:16:16-09:20:38 | 7.140 | 137798 | 234 (155+79) | 145 | 3719 of 72340 = 5.1% | 1871/2045 (91.5%) | 66/103 (64.1%) | 52/391 of 336052 (0.13%) | 0/368 of 62284 (0.59%) | traffic_skip 102933, ul_decode_skip 56223, e1 1021, e3_skip 19305, e3_trunc 4144 |

### Sensing runs

| source | run (arm) | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 | drop % (cfr+async) |
|---|---|---|---|---|---|---|
| ssb | sp923SbS1 (base) | 150.01 | 7/16/19 (21) | 0.000 | 5.7/6.8/8.1 | 0.00 |
| ssb | sp923SfS2 (fix) | 150.01 | 8/18/20 (21) | 0.000 | 6.1/7.3/8.6 | 0.00 |
| ssb | sp923SfS3 (fix) | 150.00 | 7/17/20 (21) | 0.000 | 5.9/7.1/8.4 | 0.00 |
| ssb | sp923SbS4 (base) | 150.01 | 7/17/20 (21) | 0.000 | 5.8/7.0/8.3 | 0.00 |
| csirs | sp923SbS1 (base) | 154.92 | 4/217/217 (217) | 99.312 | 3.8/20.3/21.0 | 0.00 |
| csirs | sp923SfS2 (fix) | 99.57 | 217/217/217 (217) | 99.569 | 20.0/20.9/21.2 | 0.00 |
| csirs | sp923SfS3 (fix) | 99.50 | 217/217/217 (217) | 99.497 | 19.1/20.8/21.1 | 0.00 |
| csirs | sp923SbS4 (base) | 99.57 | 217/217/217 (217) | 99.571 | 19.1/20.8/21.2 | 0.00 |
| pdsch_dmrs | sp923SbS1 (base) | 0 | - | - | - | - |
| pdsch_dmrs | sp923SfS2 (fix) | 16.84 | 13/32/160 (217) | 2.360 | 5.7/19.4/28.8 | 0.00 |
| pdsch_dmrs | sp923SfS3 (fix) | 22.42 | 4/32/80 (217) | 0.956 | 4.8/13.4/21.2 | 0.00 |
| pdsch_dmrs | sp923SbS4 (base) | 0 | - | - | - | - |
| pdsch_data | sp923SbS1 (base) | 5.09 | 0/0/0 (193) | 0.110 | 5.8/7.7/9.3 | 3.59 |
| pdsch_data | sp923SfS2 (fix) | 6.47 | 0/0/16 (217) | 0.266 | 15.3/23.1/30.0 | 0.37 |
| pdsch_data | sp923SfS3 (fix) | 9.19 | 0/0/0 (48) | 0.000 | 6.3/11.8/14.8 | 0.07 |
| pdsch_data | sp923SbS4 (base) | 0.04 | 0/0/0 (0) | 0.000 | -/-/- | 0.00 |
| pusch_dmrs | sp923SbS1 (base) | 27.17 | 0/0/0 (11) | 0.000 | 3.5/4.7/7.1 | 0.00 |
| pusch_dmrs | sp923SfS2 (fix) | 95.12 | 0/0/0 (11) | 0.000 | 3.4/4.5/7.7 | 0.00 |
| pusch_dmrs | sp923SfS3 (fix) | 41.49 | 0/0/0 (10) | 0.000 | 3.4/4.8/7.0 | 0.00 |
| pusch_dmrs | sp923SbS4 (base) | 19.75 | 0/0/0 (11) | 0.000 | 3.4/4.5/7.6 | 0.00 |
| pusch_data | sp923SbS1 (base) | 0 | - | - | - | - |
| pusch_data | sp923SfS2 (fix) | 0 | - | - | - | - |
| pusch_data | sp923SfS3 (fix) | 0 | - | - | - | - |
| pusch_data | sp923SbS4 (base) | 0 | - | - | - | - |
| srs | sp923SbS1 (base) | 0.01 | 0/0/0 (0) | 0.000 | -/-/- | 0.00 |
| srs | sp923SfS2 (fix) | 0 | - | - | - | - |
| srs | sp923SfS3 (fix) | 0.01 | 4/6/8 (8) | 0.000 | 4.6/5.0/5.5 | 0.00 |
| srs | sp923SbS4 (base) | 0.00 | 4/4/4 (4) | 0.000 | 5.9/5.9/5.9 | 0.00 |

| run (arm) | time | CPU | avail25ms / 100ms / 1s (wideband) | wb records | gap p95/max s | units enabled series (min-max, mean) | SCHED binding | top | elig mean/max | cold_deferred mean/max | decisions w/ pdsch_dmrs / pdsch_data admitted (of N) | enables/disables |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| sp923SbS1 (base) | 09:05:56-09:10:20 | 7.049 | 0.621 / 0.993 / 0.995 | 18925 | 0.000/0.039/0.043 | 0-2, 0.7 | {'pool': 23, 'queue_ul': 29, 'sense_ring': 1} | n/a | n/a | n/a | 3 / 3 (of 53) | 9/8 |
| sp923SfS2 (fix) | 09:11:06-09:15:31 | 7.539 | 0.631 / 0.996 / 1.000 | 21525 | 0.000/0.039/0.040 | 0-7, 4.0 | {'none': 49, 'pool': 2, 'gpu': 2} | {'pool': 36, 'queue_ul': 8, 'sense_ring': 6, 'gpu': 2, 'queue_dl': 1} | 35.5/50 | 31.1/46 | 51 / 32 (of 53) | 51/47 |
| sp923SfS3 (fix) | 09:21:23-09:25:49 | 7.778 | 0.627 / 0.995 / 0.995 | 21533 | 0.000/0.039/0.040 | 0-6, 3.8 | {'none': 45, 'pool': 4, 'queue_ul': 5} | {'pool': 41, 'queue_ul': 6, 'queue_dl': 3, 'sense_ring': 4} | 36.4/51 | 30.7/48 | 46 / 18 (of 54) | 46/43 |
| sp923SbS4 (base) | 09:26:34-09:31:00 | 7.644 | 0.671 / 0.996 / 1.000 | 21412 | 0.000/0.039/0.040 | 0-1, 0.6 | {'pool': 14, 'queue_ul': 39, 'sense_ring': 1} | n/a | n/a | n/a | 8 / 9 (of 54) | 14/13 |

## Cell 403 (section 2)

### Traffic runs (fix arm)

| run | start-end (local) | CPU cores | DCI rec | RNTIs>=2 DCI (DL+UL) | SOLVED | full-field 1_1 | DL CRC ok/non-probe (%) | DL>=128PRB ok/n (%) | scanq full/stale of queued | pdschq full/stale of queued | other |
|---|---|---|---|---|---|---|---|---|---|---|---|
| sp403TfT1 | 09:45:54-09:50:08 | 5.082 | 102710 | 65 (41+24) | 48 | 0 of 85087 = 0.0% | 1955/3715 (52.6%) | 0/0 (n/a) | 1376/2179 of 420031 (0.85%) | 0/37 of 44870 (0.08%) | traffic_skip 58175, ul_decode_skip 4107, e1 638, e3_skip 266793, e3_trunc 0 |

### Sensing runs

| source | run (arm) | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 | drop % (cfr+async) |
|---|---|---|---|---|---|---|
| ssb | sp403SbS1 (base) | 150.01 | 10/21/21 (21) | 0.000 | 8.2/9.3/10.5 | 0.00 |
| ssb | sp403SfS2 (fix) | 150.02 | 10/21/21 (21) | 0.000 | 8.3/9.5/10.7 | 0.00 |
| csirs | sp403SbS1 (base) | 97.82 | 25/42/48 (51) | 0.000 | 5.0/7.6/9.5 | 0.00 |
| csirs | sp403SfS2 (fix) | 98.22 | 27/43/49 (51) | 0.000 | 5.0/7.5/9.5 | 0.00 |
| pdsch_dmrs | sp403SbS1 (base) | 8.06 | 0/1/22 (51) | 0.000 | 3.3/4.7/7.1 | 0.00 |
| pdsch_dmrs | sp403SfS2 (fix) | 23.14 | 0/7/44 (51) | 0.000 | 3.7/5.9/9.2 | 0.00 |
| pdsch_data | sp403SbS1 (base) | 16.65 | 0/0/25 (50) | 0.000 | 4.4/5.5/7.2 | 0.82 |
| pdsch_data | sp403SfS2 (fix) | 25.68 | 0/10/47 (51) | 0.000 | 5.2/9.1/11.1 | 2.98 |
| pusch_dmrs | sp403SbS1 (base) | 0.35 | 0/0/0 (0) | 0.000 | -/-/- | 0.00 |
| pusch_dmrs | sp403SfS2 (fix) | 5.02 | 0/0/0 (1) | 0.000 | 3.7/3.7/3.7 | 0.00 |
| pusch_data | sp403SbS1 (base) | 0 | - | - | - | - |
| pusch_data | sp403SfS2 (fix) | 0 | - | - | - | - |
| srs | sp403SbS1 (base) | 0 | - | - | - | - |
| srs | sp403SfS2 (fix) | 0 | - | - | - | - |

| run (arm) | time | CPU | avail25ms / 100ms / 1s (wideband) | wb records | gap p95/max s | units enabled series (min-max, mean) | SCHED binding | top | elig mean/max | cold_deferred mean/max | decisions w/ pdsch_dmrs / pdsch_data admitted (of N) | enables/disables |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| sp403SbS1 (base) | 09:50:53-09:55:07 | 4.875 | 0.000 / 0.000 / 0.000 | 0 | n/a/n/a/n/a | 0-3, 1.5 | {'pool': 40, 'sense_ring': 7, 'da': 6} | n/a | n/a | n/a | 30 / 37 (of 53) | 19/17 |
| sp403SfS2 (fix) | 09:55:53-10:00:08 | 5.478 | 0.000 / 0.000 / 0.000 | 0 | n/a/n/a/n/a | 0-6, 3.5 | {'none': 51, 'pool': 1} | {'pool': 38, 'da': 8, 'sense_ring': 6} | 7.4/16 | 3.7/11 | 42 / 44 (of 52) | 41/36 |

## Cell 508 (section 2)

### Traffic runs (fix arm)

| run | start-end (local) | CPU cores | DCI rec | RNTIs>=2 DCI (DL+UL) | SOLVED | full-field 1_1 | DL CRC ok/non-probe (%) | DL>=128PRB ok/n (%) | scanq full/stale of queued | pdschq full/stale of queued | other |
|---|---|---|---|---|---|---|---|---|---|---|---|
| sp508TfT1 | 10:01:14-10:05:24 | 5.024 | 12437 | 39 (39+0) | 0 | 0 of 0 = 0.0% | 7156/11524 (62.1%) | 0/0 (n/a) | 8/28 of 304044 (0.01%) | 0/0 of 19569 (0.00%) | traffic_skip 0, ul_decode_skip 0, e1 0, e3_skip 0, e3_trunc 0 |
| sp508TfT2 | 10:16:02-10:20:16 | 5.934 | 35321 | 16 (13+3) | 0 | 0 of 23321 = 0.0% | 826/1761 (46.9%) | 0/0 (n/a) | 24/780 of 304031 (0.26%) | 0/616 of 40981 (1.50%) | traffic_skip 0, ul_decode_skip 0, e1 1450, e3_skip 40097, e3_trunc 8177 |

### Sensing runs

| source | run (arm) | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 | drop % (cfr+async) |
|---|---|---|---|---|---|---|
| ssb | sp508SbS1 (base) | 899.95 | 3/6/14 (20) | 0.000 | 4.4/5.4/6.8 | 0.00 |
| ssb | sp508SfS2 (fix) | 899.80 | 3/6/13 (20) | 0.000 | 4.4/5.4/6.9 | 0.00 |
| ssb | sp508SfS3 (fix) | 899.94 | 3/7/14 (20) | 0.000 | 4.4/5.5/6.9 | 0.00 |
| ssb | sp508SbS4 (base) | 900.01 | 3/7/14 (20) | 0.000 | 4.4/5.5/6.9 | 0.00 |
| csirs | sp508SbS1 (base) | 0 | - | - | - | - |
| csirs | sp508SfS2 (fix) | 0 | - | - | - | - |
| csirs | sp508SfS3 (fix) | 0 | - | - | - | - |
| csirs | sp508SbS4 (base) | 0 | - | - | - | - |
| pdsch_dmrs | sp508SbS1 (base) | 11.18 | 0/5/13 (17) | 0.000 | 3.3/4.1/6.8 | 0.00 |
| pdsch_dmrs | sp508SfS2 (fix) | 36.02 | 0/6/15 (16) | 0.000 | 3.4/4.4/8.0 | 0.00 |
| pdsch_dmrs | sp508SfS3 (fix) | 15.25 | 0/6/14 (15) | 0.000 | 3.4/4.3/6.7 | 0.00 |
| pdsch_dmrs | sp508SbS4 (base) | 17.43 | 0/5/11 (15) | 0.000 | 3.3/4.2/5.8 | 0.00 |
| pdsch_data | sp508SbS1 (base) | 0 | - | - | - | - |
| pdsch_data | sp508SfS2 (fix) | 0 | - | - | - | - |
| pdsch_data | sp508SfS3 (fix) | 0 | - | - | - | - |
| pdsch_data | sp508SbS4 (base) | 0 | - | - | - | - |
| pusch_dmrs | sp508SbS1 (base) | 0 | - | - | - | - |
| pusch_dmrs | sp508SfS2 (fix) | 0 | - | - | - | - |
| pusch_dmrs | sp508SfS3 (fix) | 0 | - | - | - | - |
| pusch_dmrs | sp508SbS4 (base) | 0 | - | - | - | - |
| pusch_data | sp508SbS1 (base) | 0 | - | - | - | - |
| pusch_data | sp508SfS2 (fix) | 0 | - | - | - | - |
| pusch_data | sp508SfS3 (fix) | 0 | - | - | - | - |
| pusch_data | sp508SbS4 (base) | 0 | - | - | - | - |
| srs | sp508SbS1 (base) | 0.54 | 1/15/16 (16) | 0.000 | 3.8/6.5/16.0 | 0.00 |
| srs | sp508SfS2 (fix) | 2.16 | 13/16/16 (16) | 0.000 | 5.4/9.7/13.5 | 0.00 |
| srs | sp508SfS3 (fix) | 1.37 | 9/28/156 (160) | 0.227 | 3.9/5.7/13.0 | 0.00 |
| srs | sp508SbS4 (base) | 1.16 | 2/16/144 (160) | 0.182 | 3.7/6.1/11.2 | 0.00 |

| run (arm) | time | CPU | avail25ms / 100ms / 1s (wideband) | wb records | gap p95/max s | units enabled series (min-max, mean) | SCHED binding | top | elig mean/max | cold_deferred mean/max | decisions w/ pdsch_dmrs / pdsch_data admitted (of N) | enables/disables |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| sp508SbS1 (base) | 10:06:09-10:10:19 | 5.520 | 0.000 / 0.000 / 0.000 | 0 | n/a/n/a/n/a | 0-1, 0.4 | {'pool': 12, 'queue_ul': 29, 'sense_ring': 9} | n/a | n/a | n/a | 16 / 0 (of 50) | 7/7 |
| sp508SfS2 (fix) | 10:11:04-10:15:17 | 5.706 | 0.000 / 0.000 / 0.000 | 0 | n/a/n/a/n/a | 0-3, 1.1 | {'none': 49} | {'pool': 26, 'sense_ring': 17, 'queue_ul': 6} | 0.9/3 | 0.0/0 | 30 / 0 (of 49) | 7/7 |
| sp508SfS3 (fix) | 10:21:01-10:25:11 | 5.229 | 0.004 / 0.009 / 0.026 | 44 | 0.040/0.528/0.725 | 0-2, 0.6 | {'none': 50} | {'pool': 30, 'queue_ul': 13, 'sense_ring': 7} | 0.5/2 | 0.0/0 | 11 / 0 (of 50) | 6/6 |
| sp508SbS4 (base) | 10:25:56-10:30:10 | 5.432 | 0.005 / 0.009 / 0.015 | 36 | 0.040/0.236/0.440 | 0-1, 0.6 | {'pool': 9, 'queue_ul': 30, 'sense_ring': 11} | n/a | n/a | n/a | 22 / 0 (of 50) | 7/7 |

## 3. Traffic: other facts, reasons for not decoded / not saved

Per traffic run (counters from ISAC_METRICS, last line). `dci_rec_lost` is 0 and `dci_rec_generated == dci_rec_persisted` in every run; `sense_io_errors` 0, `sense_rec_drop` 0, `sense_cfr_drop` 0, `puschq_drop_epoch` 0, `pdschq_drop_full` 0, `scanq` epoch drops 0, sync losses 0.

| run | PDSCH queue: queued / decoded / crc_ok, failure reasons | PUSCH try / crc_ok | DCI state solved / unsolved |
|---|---|---|---|
| sp415TfT1 | 11336 / 11026 / 3102; queue_expired 286, copy_expired 6, unsupported 18 | 64890 / 2 | 191838 / 7774 |
| sp415TfT2 | 21355 / 20782 / 4669; queue_expired 390, copy_expired 12, unsupported 171 | 21294 / 1 | 127129 / 92899 |
| sp923TfT1 | 63364 / 62158 / 2646; queue_expired 525, copy_expired 21, unsupported 645 | 76461 / 1 | 175127 / 18881 |
| sp923TfT2 | 62284 / 61351 / 1871; queue_expired 358, copy_expired 10, unsupported 565 | 83775 / 6 | 114748 / 19606 |
| sp508TfT1 | 19569 / 11266 / 6998; none | 0 / 0 | 0 / 0 (12148 "fixed", fallback DCI only) |
| sp508TfT2 | 40981 / 39184 / 826; queue_expired 616, unsupported 907 | 658 / 0 | 0 / 26393 unsolved |
| sp403TfT1 | 44870 / 44147 / 1962; queue_expired 37, unsupported 686 | 10510 / 1 | 79027 / 13765 |

Queue drops (all in the traffic tables above): scan queue full 8-1376 and stale 28-2179 per run, worst 403 (full 1376 + stale 2179 of 420031 = 0.85 %), all others <= 0.26 %; PDSCH queue full always 0, stale 0-616 (worst 415 T1 2.58 % of its 11336 queued, 415 T2 1.88 %, 508 T2 1.50 %, 923 0.59-0.86 %). All below the 4 % goal.

Not decoded / not saved, with the reason read from the counter name [CODE-READ, no per-grant trace]:
- DCIs recorded but intentionally not decoded: `traffic_skip` 102933-173336 (415, 923), 58175 (403), 0 (508); UL `ul_decode_skip` 25498-110306 (415, 923), 4107 (403), 0 (508). PUSCH decode: CRC ok in 0-6 of 658-83775 tries in every run, so no UL transport block was recovered in any cell (UL DCIs are saved).
- Layout search limits: `e3_skip` 114-19305 (415, 923), 266793 (403 T1: all 85087 1_1 DCIs have no full field set, 0 full-field records), 40097 (508 T2); `e3_trunc` up to 8177 (508 T2).
- PDSCH decode attempts without a TB: DL non-probe records with CRC fail 299 / 438 (415 T1 / T2), 414 / 174 (923), 1760 (403), 4368 / 935 (508); `unsupported` (not attempted) 18-907; `queue_expired` / `copy_expired` (decode request too old) 37-616 / 0-21. 508 and 403 CRC failures are mostly SI/fallback-class repeats and low-SNR cells [HYPOTHESIS, same as night].
- Decoded PDSCH payload is saved in the decode records; traffic mode exports no CFR by design: `sense_unexported_pdsch_data` equals `pdschq_crc_ok` in every T run.
- Wide grants (>= 128 PRB) DL CRC: 415 86.3 % (669/775) and 88.0 % (951/1081); 923 63.3 % (252/398) and 64.1 % (66/103); 508 and 403 have none. Night values were 96.5 / 67.9 % (415) and 64.3 / 50.6 % (923): 415 and 923 are the same ballpark in the day, 923 wide grants still decode poorly (~63 %).
- 508 T1 saw only fallback DCI 1_0 (no 1_1/0_1, 0 solved, no CSI-RS confirmed); 508 T2 saw 23321 1_1 and 11 k 0_1 DCIs but 0 SOLVED, 26393 unsolved: the dedicated layout was not solved in this window [cause not established].

## 4. PDSCH units in the sensing runs (the main question)

Evidence: the `SENSE_SCHED` line (both formats parsed: BASE `binding= admitted=[...]`, FIX `binding= top= elig= cold_deferred= admitted=[...]`) and `SENSE_UNITS`. **SENSE_UNITS prints only the first 8 of 139-179 units per cell, and those are always the old, never-enabled rows, so its ON/off flags say nothing about the PDSCH units that were actually used; I did not use them for the verdict** (an.txt shows them: pdsch rows are "off" in both arms). The verdict uses the admitted set listed in every SENSE_SCHED decision (one decision per ~4 s, 49-54 per run) plus the exported pdsch_dmrs / pdsch_data record rates. "Admitted" = unit enabled by that decision. A unit "shed" = admitted in decision k and not in k+1; residency = consecutive decisions a unit stays admitted.

| run (arm) | decisions with >= 1 PDSCH unit admitted | first decision with one | distinct PDSCH units admitted | mean / max residency (decisions) | mean PDSCH units per decision | pdsch_dmrs / pdsch_data rec/s | verdict |
|---|---|---|---|---|---|---|---|
| sp415SbS1 (base) | 32 / 54 (59 %) | #3 | 13 | 2.1 / 10 | 0.8 | 2.7 / 43.2 | enabled intermittently, shed often; not starved |
| sp415SfS2 (fix) | 53 / 54 (98 %) | #1 | 35 | 3.9 / 9 | 2.9 | 5.6 / 15.5 | enabled throughout |
| sp415SfS3 (fix) | 51 / 53 (96 %) | #1 | 33 | 4.2 / 15 | 3.3 | 8.3 / 12.6 | enabled throughout |
| sp415SbS4 (base) | 26 / 54 (48 %) | #3 | 12 | 2.5 / 8 | 0.6 | 9.0 / 0 | disabled in more than half the decisions |
| sp923SbS1 (base) | **6 / 53 (11 %)** | #21 | 4 | 1.5 / 2 | 0.1 | **0 / 5.1** | **starved: disabled in 89 % of decisions, no pdsch_dmrs exported at all** |
| sp923SfS2 (fix) | 51 / 53 (96 %) | #1 | 24 | 5.4 / 18 | 2.8 | 16.8 / 6.5 | enabled throughout |
| sp923SfS3 (fix) | 49 / 54 (91 %) | #2 | 17 | 4.5 / 15 | 2.6 | 22.4 / 9.2 | enabled throughout |
| sp923SbS4 (base) | **16 / 54 (30 %)** | #2 | 8 | 2.1 / 5 | 0.3 | **0 / 0.04** | **starved: 70 % of decisions without PDSCH, no pdsch_dmrs exported** |
| sp508SbS1 (base) | 16 / 50 (32 %) | #4 | 2 | 2.3 / 5 | 0.3 | 11.2 / 0 | SI-only cell; disabled in 68 % |
| sp508SfS2 (fix) | 30 / 49 (61 %) | #4 | 1 | 3.0 / 9 | 0.6 | 36.0 / 0 | one unit (broadcast), 61 % |
| sp508SfS3 (fix) | 11 / 50 (22 %) | #4 | 1 | 2.2 / 5 | 0.2 | 15.3 / 0 | one unit, only 22 % of decisions; still 15 rec/s |
| sp508SbS4 (base) | 22 / 50 (44 %) | #3 | 1 | 2.8 / 6 | 0.4 | 17.4 / 0 | one unit, 44 % |
| sp403SbS1 (base) | 45 / 53 (85 %) | #8 | 19 | 3.2 / 13 | 1.6 | 8.1 / 16.7 | not starved |
| sp403SfS2 (fix) | 47 / 52 (90 %) | #5 | 25 | 4.0 / 21 | 3.2 | 23.1 / 25.7 | not starved |

**Answer.** The 923 starvation seen at night in S1 is reproduced in the base arm twice in the day (6 and 16 of 53-54 decisions with a PDSCH unit, 0 pdsch_dmrs records both times) and is absent in the fix arm twice (51 and 49 decisions, 17-22 pdsch_dmrs rec/s, 0.96-2.4 wideband PDSCH rec/s). The 415 S2 starvation of the night did not reproduce in the base arm in the day (pdsch_dmrs 2.7 and 9.0 rec/s), but base still disabled PDSCH in 41-52 % of decisions with 0.6-0.8 units admitted on average, against 2.9-3.3 for the fix [MEASURED]. No PDSCH unit stayed disabled for most of a fix run in 415, 923 or 403. In the fix 508 runs the single admitted unit (SI broadcast) was admitted in 61 % and 22 % of decisions; that cell has one PDSCH source only, other units do not exist there. Fix `binding` is "none" in 44-51 of 52-54 decisions on 415/923/403 and 49-50 of 49-50 on 508; the rare rejections are pool / gpu / queue_ul.

Fix scheduler detail: `top` (the tightest resource) is `pool` in 24-41 decisions per run, `gpu` 15-22 in 415 only. `elig` (eligible candidates) averages 33-41 on 415, 35-36 on 923, 7.4 on 403, 0.5-0.9 on 508; `cold_deferred` averages 29-36 (415), 31 (923), 3.7 (403), 0 (508), i.e. on busy cells most eligible cold candidates still wait for the one cold slot per decision, but warm units are kept (residency 4-5 decisions).

## 5. Investigations

### 5.1 Sensing loss above 5 %

- Export drops (`cfr_drop` + `async_drop` over exported+dropped, per source): no source exceeds 5 %. Largest: base 923 S1 pdsch_data 3.59 % (async_drop), fix 403 S2 pdsch_data 2.98 %, fix 415 S2 pdsch_data 1.95 %, base 403 S1 0.82 %; everything else <= 0.4 %. `sense_rec_drop` 0 in all runs, `sense_cfr_drop` 0 except 27 in fix 415 S3 [MEASURED].
- Wideband availability on 415 is far below 5 % at 25 ms and below 13 % at 1 s in both arms (table above). This is the loss worth explaining, using `inv_day.py` on the grant records of the sensing runs:

| run (arm) | wide DL grants (>= 128 PRB) | grants/s | max gap between wide grants | with any PDSCH rs record | with a >= 128 PRB rs record | distinct wide-grant RNTIs | share of wide grants whose RNTI was ever admitted |
|---|---|---|---|---|---|---|---|
| sp415SbS1 (base) | 8626 | 41.5 | 4.4 s | 6.5 % | 2.2 % | 69 | 54 % |
| sp415SfS2 (fix) | 17005 | 80.9 | 6.4 s | 6.3 % | 3.1 % | 80 | 85 % |
| sp415SfS3 (fix) | 8094 | 39.7 | 3.9 s | 3.3 % | 1.4 % | 81 | 71 % |
| sp415SbS4 (base) | 13526 | 64.9 | 5.6 s | 5.8 % | 2.8 % | 76 | 53 % |
| sp923SbS1 (base) | 2169 | 12.0 | 6.5 s | 10.4 % | 0.6 % | 27 | 74 % |
| sp923SbS4 (base) | 3541 | 17.1 | 9.0 s | 0.0 % | 0.0 % | 36 | 69 % |
| sp923SfS2 (fix) | 5108 | 24.8 | 6.7 s | 18.1 % | 10.0 % | 42 | 88 % |
| sp923SfS3 (fix) | 2567 | 12.3 | 6.5 s | 18.8 % | 8.0 % | 33 | 77 % |

  Reading [MEASURED]: 415 carries 40-81 wide DL grants per second spread over 70-81 RNTIs, with no gap between wide grants above 6.4 s, yet wideband records exist for only 1.4-3.1 % of them and the largest gap between wideband records is 38-62 s in three of four runs (fix S2 62.4 s, fix S3 56.2 s, base S1 38.5 s; base S4 1.1 s). So the 415 loss is not lack of wide traffic. The fix raises the share of wide grants whose RNTI was ever admitted from 53-54 % to 71-85 % and the coverage in 923 from 0-0.6 % to 8-10 %, but in 415 the coverage stays at 1.4-3.1 % in both arms. Scheduler gating therefore explains the 923 base loss and part of the 415 base loss (RNTI never admitted) but not the remaining 415 gap: most wide grants of an admitted RNTI still do not produce a >= 128 PRB record (admitted units cover one RNTI each, ~3 at a time against 70-80 active wide-grant RNTIs; `cold_deferred` ~30 per decision) [HYPOTHESIS: per-RNTI unit capacity and the one-cold-slot-per-decision pacing, not a bug; a cell with a few hundred wide-grant RNTIs per 4 s needs many more simultaneous units than the pool admits]. `top=pool` in 24-28 of 53-54 fix decisions on 415 and `top=gpu` 15-22 point at the pool and GPU budgets as the tightest resources [MEASURED, SENSE_SCHED]; I did not change anything to test this.
- 923: wideband availability 25 ms is 0.62-0.67 in all four runs with the 217 PRB CSI-RS exported at ~100 /s (cfr windows of 25 ms miss it 33-38 % of the time, regardless of arm), identical to the night (0.62 / 0.75). The fix does not change it; cause not investigated here [HYPOTHESIS: CSI-RS burst period/pattern]. 100 ms / 1 s are >= 0.993.
- 508 and 403: no wideband possible from PDSCH or CSI-RS (162 and 51 PRB cell, no CSI-RS on 508; 403 CSI-RS 51 PRB). 508 availability comes only from sparse SRS 144-160 PRB records.
- SRS: 415 SRS records 0.05-0.10 /s, 923 0-0.01 /s, 508 0.5-2.2 /s (SRS in 508 is the only >= 128 PRB source), 403 none. Base 508 S1 0.54, S4 1.16 vs fix 2.16, 1.37 /s.

### 5.2 Scheduler gating

See section 4. The base arm's `binding` is mostly `queue_ul` (29-39 of 50-54 decisions on 415/923/508) or `pool` (9-23), which sheds PDSCH when the SRS and PUSCH units hold the budget; the fix shows `binding=none` in 44-51 decisions per run with the same hardware, so PDSCH units stop being the only thing shed. In 923 the fix also exports more pusch_dmrs (95 / 41 rec/s vs 27 / 20 base): freeing the budget did not take it away from UL.

## 6. CPU (cores used by the receiver process, sidecar, utime+stime over the run)

| cell | T fix | S base | S fix | T fix (2) | S fix (2) | S base (2) |
|---|---|---|---|---|---|---|
| 415 | 6.84 | 7.30 | 7.19 | 7.47 | 6.98 | 6.89 |
| 923 | 7.58 | 7.05 | 7.54 | 7.14 | 7.78 | 7.64 |
| 508 | 5.02 | 5.52 | 5.71 | 5.93 | 5.23 | 5.43 |
| 403 | 5.08 | 4.88 | 5.48 | - | - | - |

Fix vs base CPU: 415 base 7.30 / 6.89, fix 7.19 / 6.98; 923 base 7.05 / 7.64, fix 7.54 / 7.78; 508 base 5.52 / 5.43, fix 5.71 / 5.23; 403 base 4.88, fix 5.48. No consistent CPU penalty for the fix beyond run-to-run spread (<= 0.6 core) [MEASURED].

## 7. Surprises

1. The 415 S2 PDSCH starvation of the night did not reproduce in the base arm during the day (pdsch_dmrs 2.7 / 9.0 rec/s, units admitted in 48-59 % of decisions). The 923 starvation did, twice. The fix removes it in 923 and raises PDSCH admission in 415 from ~50 % to ~97 % of decisions.
2. The fix makes the scheduler admit many more PDSCH units but 415 wideband availability does not improve (1-12 % at 1 s in both arms): coverage of wide grants stays at 1.4-3.1 %. That is a per-RNTI capacity limit, not starvation (section 5.1).
3. The pdsch_data rate in 415 is lower in the fix (12.6-15.5 /s) than in base S1 (43 /s) while pdsch_dmrs is higher; one base run had 0 pdsch_data in 415 S4. Run-to-run variation is of this order, no conclusion drawn.
4. 403 locked PCI 403 this time (at night it locked PCI 27), but only reached CORESET_VERIFIED, with 0 full-field 1_1 records and DL CRC 52.6 %.
5. My own script bug wasted the first 508 pass (section 0); 508 was rerun in full.
6. 923 wide-grant DL CRC is only ~63-64 % in the day too; 415 ~86-88 %. Every cell's UL decode is still 0 (PUSCH CRC ok <= 6).
