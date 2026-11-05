# Single-PCI OTA campaign, 2026-11-05 night (twomodes/design @f6e91f107e)

Label for every number below: **[OTA LOW LOAD, DGX GB10 + X410 34B2AF1, 4 RX, DEIB Milan NSA cells, build twomodes/design f6e91f107e, 22:53-00:38 local, MPCI_20261025.env]**. This is a functional night run, not a daytime validation. Cells and modes are never merged. Each run: 240 s, one PCI, the receiver stopped with SIGINT (all RX_EXIT=130), ISAC_RX_BRANCH_FO unset. Campaign dir `/home/nicola/NICOLA/ota_runs/sp_ota_20261105_225342` (campaign.log, runs.tsv, per-run `cpu_cores.txt`). Script `.superpowers/tmp/sp_ota.sh` (model: ota_e8.sh), analyzer `.superpowers/tmp/an_sp.py`. Run-dir names are in runs.tsv and in the appendix. Rates use the span of the run's rs records (about 214 s for 415/923, 195 s for 508) because sensing records start after sync.

## 0. Setup and honest caveats

- Tune triples (freq:ssb:fo:rb, from `rr-orchestration/ota/run_ota_20261102.sh`, which locked these cells before): 415 `3680010000:1171:-4827:217`, 923 `3759990000:233:-4832:217`, 508 `3590010000:193:-4868:162`, 403 `3630000000:18:2178:51`. The 508 and 403 triples in the task text ("3570570000/1171/-2400", "3625290000/1171/-2400") were from memory and differ from the logged ones; I used the logged ones. 508 locked PCI 508 on the first try in all 5 runs.
- **403 (Iliad) was not locked.** All 6 launches (2 runs x up to 3 tries) synced PCI 27 (Iliad, barred=1, mcc/mnc 222/50, cell_identity 0x3d0a3cdad), not 403. On 2026-11-02 the same triple locked 403 (run o1S403_1_092526). Results for PCI 27 are given only as a side note and are not 403.
- Modes: traffic = `traffic/traffic_ota.dgx.cfg` (mode=traffic); sensing = `sensing/sensing_ota.dgx.cfg` (mode=sensing); "ALL" = new `sensing/sensing_all_ota.dgx.cfg` (see section 1), 1 run per cell (415, 923, 508), a supplementary arm, not the requested sensing mode.
- Per cell: T1, S1, T2, S2, A1 (A = ALL). 5 runs each for 415/923/508 = 15 runs, plus the 403 attempts (6 launches).
- Night load differs strongly between consecutive runs of one cell (e.g. 415 S1 vs S2). Differences between runs are not a mode effect. No claim of a mode A/B comparison is made.

## 1. Are all sensing resources on? (code-read [CODE-READ], and run evidence)

- The cfg lists all 9 sources, but `mode = "sensing"` cuts two of them by design: `nr_isac_mode_resolve` (openair1/PHY/NR_UE_ISAC/nr_isac_mode.c) allows only `SENSING_SET = ssb, csi_rs, pdsch_dmrs, pdsch_data, pusch_dmrs, srs` plus `prach`. **`pdsch_dmrs_blind` and `pusch_data` are dropped in sensing mode** (`narrowed`). The init warning ("cut sources outside the sensing set") is not visible in run.log (logged before the log sink is up). Run evidence: `sense_prod_count_pusch_data = 0` and no pusch_data record in any S run. `pdsch_dmrs_blind` maps to the same sense source as `pdsch_dmrs` (source_map in nr_isac.cc), so its records are not separable from pdsch_dmrs in any mode.
- Other keys in the sensing group: `sense_min_prb` 128, `sense_min_prb_floor` 32, `sense_cont_target_ms` 25 (all defaults, confirmed by SENSE_FLOOR lines: levels 128/96/64/48/32, target 25 ms), `rec_ring_slots` 1024, `cfr_ring_frames` 32, `cfr_path` unset (CFR frames counted, `sense_cfr_no_sink`, not written), pdsch decode mode 2 and PUSCH decode 1 are the sensing-mode defaults (the cfg sets `pdcch_blind_monitor_pdsch="2:..."`; ul_pusch is unset, so full_auto turns it on). Units are chosen by the S1-S9 scheduler (SENSE_SCHED), not by a cfg key: the scheduler enables/disables units per budget (see section 4), so "unit enable" is not a static setting.
- `sensing_all_ota.dgx.cfg` = `sensing_ota.dgx.cfg` with **one difference: the line `mode = "sensing"` removed** (legacy mode, explicit 9-source list). It is byte-equal (except the comment header) to `traffic/legacy_ota.dgx.cfg`. Consequences: pdsch_dmrs_blind (still not separable) and pusch_data are enabled; but legacy mode has **no quality block** (no `valid_cfr_prbs`, `snr_db_p50`, `cont_floor`), no `dci/decode` records and no S1-S9 scheduler/T_cell. ALL-arm "prbs" below are `nb_rb` (allocated) and "snr" is `snr_db` (a different statistic): not comparable with S-mode numbers.
- Whether the two cut sources are a deliberate decision or an omission is for the owner to decide (spec 2026-11-03 line 199 only specifies sense_min_prb). I changed no code.

## 2. TRAFFIC mode (T1 / T2 per cell)

Common to all traffic runs: sync OK, no sync loss (`acq_sync_losses` 0), SIB1 decoded (`acq_sib1_decodes` 1-2), `dci_rec_lost` 0 and `dci_rec_generated == dci_rec_persisted` in every run (no DCI record lost), `sense_io_errors` 0, `sense_rec_drop` 0, `puschq_drop_epoch` 0, `pdschq_drop_full` 0, `pdschq_drop_epoch` 0.

| item | 415 T1 / T2 | 923 T1 / T2 | 508 T1 / T2 |
|---|---|---|---|
| ACQ state / CSI-RS confirmed (log) | CELL_CONFIGURED; 28 / 40 | CELL_CONFIGURED; 6 / 5 | CELL_CONFIGURED; 0 / 0 |
| CPU cores (sidecar) | 6.52 / 6.56 | 6.89 / 7.28 | 4.98 / 4.85 |
| DCI records | 67300 / 98952 | 128557 / 193559 | 12222 / 11579 |
| by format 1_0 / 1_1 / 0_1 / 0_0 | 3168 / 23938 / 40193 / 1 ; 3218 / 34760 / 60974 / 0 | 3701 / 76704 / 48151 / 1 ; 3552 / 122833 / 67172 / 2 | 12222 / 0 / 0 / 0 ; 11579 / 0 / 0 / 0 |
| DL by class C / SI / RA / TC (UL has no class) | 24537 / 2547 / 3 / 19 ; 35453 / 2497 / 13 / 15 | 76904 / 3458 / 26 / 17 | 1676 / 10498 / 27 / 21 ; 1250 / 10274 / 32 / 23 |
| distinct RNTIs (dir,rnti), any count | 733 / 818 | 317 / 262 | 1674 / 1259 |
| distinct RNTIs with >= 2 DCIs (DL+UL) | 119 (70+49) / 135 (65+70) | 109 (71+38) / 125 (79+46) | 29 (29+0) / 15 (15+0) |
| SOLVED RNTIs (layout_state=solved) | 100 / 96 | 85 / 91 | 0 / 0 |
| DL 1_1 with all of harq_pid, ndi, rv, mcs, k1, dai, tpc, pri non-null | 2835 of 23938 = 11.8 % / 2943 of 34760 = 8.5 % | 23756 of 76704 = 31.0 % / 40205 of 122833 = 32.7 % | n/a (no 1_1) |
| PDSCH decodes, non-probe DL (CRC ok) | 1297 (1208, 93.1 %) / 1912 (1698, 88.8 %) | 2770 (2442, 88.2 %) / 4289 (3638, 84.8 %) | 12217 (7443, 60.9 %) / 11785 (7370, 62.5 %) |
| of those >= 128 PRB (CRC ok) | 286 (276, 96.5 %) / 162 (110, 67.9 %) | 437 (281, 64.3 %) / 724 (366, 50.6 %) | 0 / 0 |
| PUSCH tries (counter), CRC ok | 38131, 3 / 8714, 1 | 90760, 7 / 102600, 5 | 0 / 0 |
| PUSCH decode records (>= 128 PRB) | 1624 (15) / 1481 (12) | 2439 (55) / 3203 (65) | 0 |
| probe decodes (DL+UL, not counted above) | 41199 / 13961 | 123189 / 144681 | 0 |

Drops and unsaved work (counters, per run):

| queue | 415 T1 / T2 | 923 T1 / T2 | 508 T1 / T2 |
|---|---|---|---|
| scanq full / stale (of queued) | 38 / 17 of 336054 (0.02 %) ; 32 / 27 of 336066 (0.02 %) | 37 / 91 of 304050 (0.04 %) ; 34 / 250 of 336055 (0.08 %) | 0 / 11 of 304055 ; 23 / 25 of 304041 |
| pdschq stale (of queued) | 35 of 5655 (0.62 %) ; 10 of 8776 (0.11 %) | 349 of 36569 (0.95 %) ; 730 of 50788 (1.44 %) | 0 ; 0 |
| pdschq unsupported / queue_expired | 33 / 34 ; 31 / 10 | 223 / 331 ; 274 / 705 | 0 / 0 ; 0 / 0 |
| recov dropped full+old+expired / stale | 0 / 4 ; 2 / 19 | 1 / 208 ; 1 / 208 | 0 / 0 |
| sense (sense_rec_drop, sense_cfr_drop) | 0, 0 | 0, 0 | 0, 0 |

Not decoded or not saved (counters; "why" is read from the counter name, not from a per-grant trace [CODE-READ/HYPOTHESIS]):
- DCIs seen but not decoded by policy: `traffic_skip` 44721 / 50305 (415), 97533 / 147735 (923), 0 (508); `ul_decode_skip` (UL grants not PUSCH-decoded) 24793 / 20280 (415), 42932 / 56532 (923).
- Layout search limits: `e3_skip` 56 / 51 (415), 4895 / 8919 (923); `e3_trunc` 44 / 168 (415), 1299 / 2223 (923); `e1_skip_classes` 3 / 22 (415), 262 / 503 (923).
- PDSCH decode attempts that returned no TB: CRC-failed DL (415: 89 + 214, 923: 328 + 651 non-probe, 508: 4774 + 4415). The 508 failures are mostly SI/TC-class repeats of 1_0 (SI 10498 of 12222 DCIs); CRC ok 61-63 % there.
- Decoded PDSCH payload is saved in the decode records; the metric `sense_unexported_pdsch_data` (= crc_ok minus exported pdsch_data CFR) equals pdschq_crc_ok in all traffic runs, i.e. traffic mode exports no CFR by design.
- UL: PUSCH CRC essentially never passes (<= 7 in 146k tries). No UL transport block was recovered on any cell. UL DCIs are saved (0_1 40k-67k per run) but their payload cannot be decoded at this receiver position/SNR [HYPOTHESIS: UE uplink too weak at the X410, consistent with S-mode pusch_dmrs median SNR 3-5 dB where valid].
- 508: only fallback DCI 1_0 was ever seen. No 1_1/0_1 DCI, no dedicated layout solved (0 SOLVED), no CSI-RS confirmed (`csirs_confirmed` 0, `csirs_disc_ambiguous` 5344), and no UL attempts (PUSCH try 0). 1674 / 1259 distinct "C" RNTIs of which only 29 / 15 repeat: mostly one-shot (likely false-alarm) hits. This cell, in this window, shows no decodable UE-specific traffic. Cause not established (low night traffic vs. undiscovered dedicated CORESET; [HYPOTHESIS]).

Key traffic findings: 415 and 923 work end-to-end (sync, SIB1, CORESET, solver: 85-100 SOLVED RNTIs, DL decode CRC 85-93 %), drops <= 1.5 % on every queue, no DCI record lost. Weak points: wide-grant (>= 128 PRB) DL CRC is only 51-68 % on 923 and in 415 T2 (96.5 % in 415 T1), full-field share of 1_1 is 8-33 %, UL decode is 0 on all cells.

## 3. SENSING mode (S1 / S2 per cell)

Per-source table, rates are records/s of exported `rs` records; wb = valid_cfr_prbs >= 128. snr = `snr_db_p50` p10/p50/p90 over records that carry it (n in the appendix). Full histograms are in the appendix.

**415 (S1 / S2)**

| source | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 |
|---|---|---|---|---|
| ssb | 1199 / 1199 | 6/13/20 (21) | 0 | 5.5/7.7/15.6 ; 5.5/7.7/15.6 |
| csi_rs | 634.6 / 514.1 | 22/52/52 (52) ; 21/52/52 | 0 | 4.8/10.5/20.2 ; 4.8/11.3/20.9 |
| pdsch_dmrs (+blind) | 5.49 / 0.005 (1 rec) | 8/42/42 (215) ; 16 | 0.056 / 0 | 3.8/18.4/19.0 ; 15.5 |
| pdsch_data | 11.4 / 0.58 | 0/0/42 (215) ; 0/0/0 | 0.103 / 0 | 10.9/14.7/16.7 ; none |
| pusch_dmrs | 1.32 / 9.66 | 0/0/0 (0) ; 0/0/0 (8) | 0 | none (0 of 283) ; 3.1/4.3/6.0 (15 of 2060) |
| pusch_data | 0 / 0 | - | - | - |
| prach | 0 / 0 exported (10463 / 10240 jobs) | - | - | - |
| srs | 0.019 (4 rec) / 1 rec | 8/8/8 | 0 | 11.3/15.0/17.4 ; 8.4 |

Availability (score_sensing, wideband records): S1 25 ms 0.001, 100 ms 0.003, 1 s 0.014 (34 wideband records in 214 s); S2 all 0.000 (0 wideband). T_cell trace: 128 at start, then 32 for almost the whole run (gap_p95 14-630 ms vs target 25 ms); S2 reached 32 too; cont_floor values on records {128: 13813, 32: 382120} (S1), {128: 65341, 32: 302398} (S2). CPU 6.45 / 6.83 cores. SRS gate_hist S1 [78779, 10225, 296, 16, 1, 0, 0], det [0,0,0,4,0,0,0]; S2 [96182, 12553, 339, 7, 0, 0, 0], det [0,0,0,1,0,0,0].

**923 (S1 / S2)**

| source | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 |
|---|---|---|---|---|
| ssb | 150 / 150 | 9/20/21 (21) | 0 | 7.7/9.2/10.6 ; 7.7/9.2/10.6 |
| csi_rs | 184.2 / 186.9 | 4/217/217 (217) ; 3/217/217 | 99.5 / 99.6 | 3.6/19.6/21.6 ; 3.5/19.4/21.6 |
| pdsch_dmrs (+blind) | **0** / 64.7 | - ; 16/199/217 (217) | 0 / 42.8 | - ; 6.0/13.5/18.3 |
| pdsch_data | **0** / 5.91 | - ; 0/0/16 (217) | 0 / 0.256 | - ; 8.7/14.6/18.3 |
| pusch_dmrs | 73.3 / 50.9 | 0/0/0 (10) ; 0/0/0 (8) | 0 | 3.2/4.5/7.4 (344 of 15739) ; 3.3/4.4/7.8 (236 of 10757) |
| pusch_data | 0 / 0 | - | - | - |
| prach | 0 exported (15286 / 15447 jobs) | - | - | - |
| srs | 0 / 0 (110868 / 122371 jobs) | - | - | - |

Availability: S1 25 ms **0.624**, 100 ms 0.998, 1 s 1.000 (21377 wideband records, all from CSI-RS full-band 217 PRB); S2 25 ms **0.753**, 100 ms 0.996, 1 s 1.000 (30138 wideband). T_cell trace S1: 128, 128, 128, 64, 64, 64, then 32; S2 stays 128/96 (gap_p95 14-60 ms, one 128 -> 96 -> 128 excursion). CPU 7.58 / 7.21 cores. SRS gate_hist S1 [100376, 10309, 182, 1, 0,0,0]; S2 [109977, 12184, 208, 2,0,0,0], no detections. 923 is the only cell that meets the 25 ms / 100 ms wideband target at night, entirely through CSI-RS.

**508 (S1 / S2)**

| source | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 |
|---|---|---|---|---|
| ssb | 900 / 900 | 3/6/15 (20) | 0 | 4.4/5.6/6.9 ; 4.4/5.5/6.9 |
| csi_rs | 0 / 0 (no CSI-RS confirmed) | - | - | - |
| pdsch_dmrs (+blind) | 51.8 / 52.0 | 0/3/19 (24) ; 0/3/19 | 0 | 3.2/4.4/5.2 ; 3.2/4.5/5.3 (7595 / 7626 of ~10100) |
| pdsch_data | 0 / 1.53 | - ; 3/5/7 (10) | 0 | - ; 4.5/5.4/5.9 |
| pusch_dmrs, pusch_data, srs, prach | 0 (srs 109914 / 109923 jobs, prach 37988 / 37998 jobs) | - | - | - |

Availability: all 0.000 (no wideband record at all). T_cell 32 after the first decision (gap_p95 330-1000 ms). CPU 5.23 / 5.28 cores. SRS gate_hist S1 [93112, 16119, 670, 13, 0,0,0], no detections.

**Sensing drops per source** (sense_cfr_drop_X + sense_async_drop_X) / (those + exported): <= 0.04 % for every source in all 6 S runs (csi_rs 9 drops in 415 S1; pdsch_data async 1 of 1422). `sense_rec_drop` 0, `sense_io_errors` 0, `ring_drops` 0. By this definition no source exceeds 5 %.

**But three non-ring losses exceed 5 % and are explained below**:

1. **PDSCH sensing exports gated off by the scheduler (923 S1, 415 S2).** 923 S1: `pdschq_crc_ok` 5008 but exported pdsch_data + pdsch_dmrs = 0 (`sense_unexported_pdsch_data` 5008); `sense_disabled_skips` 156231 ~ the 156085 grant records. SENSE_SCHED shows `binding=queue_ul` (and `pool`), `enabled_now` 0-2, and the admitted list holds `srs` (score ~30720) and `pusch_dmrs` (score 780-2600) while every pdsch unit stays disabled; 146378 PUSCH tries (9 CRC ok) are the UL load. In S2 of the same cell the scheduler admitted pdsch_dmrs units (units_enabled 3-4, `binding=pool`) and exported 64.7 + 5.9 rec/s. Same pattern in 415: S1 5.5 + 11.4 rec/s vs S2 0.005 + 0.58 rec/s (S2 `binding=queue_ul`, units_enabled 1). So whether PDSCH CFR is exported in sensing mode depends on which units the budget scheduler admits; in two of six S runs the PDSCH units were off for most of the run. [OTA LOW LOAD; mechanism from SENSE_SCHED lines and `nr_sense_sched_rt.c` resource names; not root-caused to a specific budget formula.]
2. **PRACH worker queue drops (counter `prach_drops`).** 923: S1 5708 of 20994 occasions (27.2 %), S2 5548 (26.4 %), A1 9385 (46.9 %); 415 A1 823 (7.8 %); 415 S1/S2 0.3 / 2.4 %; 508 0 %. Each job costs 2.1-3.5 ms (`sense_prod_us_prach` / jobs); 923 occasions arrive about every 10 ms while the host is at 7.2-7.6 cores, so the single PRACH worker is starved ([CODE-READ] drop = job ring full or producer busy, nr_prach_passive.c:1040/1088). Exported PRACH is 0 on every run (NSA cells: expected to stay 0 per operator note), so no record is lost, but the PRACH budget is spent on a source that cannot produce on these cells.
3. **SRS: 99.9 % of SRS jobs are skipped before processing.** `srs_pregate_skip` ~ jobs (89300-122371 per run), `srs_budget_skip` 0-62477 (923 S1: 62477), `srs_jobs_dropped` 0 in S runs (68 in 923 A1). Exported SRS records: 4 (415 S1), 1 (415 S2), 0 elsewhere.

## 4. Can SRS and PUSCH be exported/decoded?

- **SRS:** exported only on 415: 4 records (S1) and 1 (S2) in about 214 s each (<= 0.02 /s), valid 8 PRB, unconfirmed (`srs_snr_hist_unconfirmed` [0,0,1,1,2,0,0] bins [no valid][3,6)[6,10)[10,15)[15,20)[20,30)>=30 dB; confirmed all 0), snr_db_p50 8.4-17.4 dB; `srs_gate_det` shows 4 and 1 detections. 923 and 508: 0 SRS records (gate hist without detections). `srs_gate_open_unexplained` 7-17 (415) and 1-13 elsewhere. Result: SRS export works in principle (on 415) but at a negligible rate here; no confirmed track.
- **PUSCH DM-RS:** exported on 415 (1.3 /s and 9.7 /s) and 923 (73.3 /s, 50.9 /s), not on 508 (no UL). It carries a usable CFR only rarely: `valid_cfr_prbs` p90 = 0 in every run (max 8-10 PRB), snr present for 2 % (923) / 0.7 % (415 S2) of records, with median 4.3-4.5 dB. So the UL pilots are detected (the export path works) but the CFR quality is poor.
- **PUSCH data-aided:** none in S mode (`pusch_data` is cut in sensing mode and PUSCH CRC ok is 0-9 per 8k-146k tries). In the ALL arm: 923 A1 exported 4 pusch_data records (snr -8.2/-6.7/-5.4 dB, 0.02 /s); 415 A1 and 508 A1 0. So PUSCH decode and data-aided CFR are effectively not working at this receiver position [OTA LOW LOAD].
- **Wide PDSCH data-aided rate (>= 128 PRB pdsch_data CFR):** S mode: 415 0.10 /s (S1) / 0; 923 0 (S1, gated off) / 0.26 /s (S2); 508 0. ALL arm (no quality gate, `nb_rb` >= 128): 415 A1 2.56 /s, 923 A1 22.8 /s, 508 A1 0 (not comparable with S-mode counts: no gating, different PRB definition, load differs).

## 5. ALL-sources arm (A1), supplementary

415 A1: csi_rs 648 /s, pdsch_data 143 /s (30627 rec, 549 with nb_rb >= 128), pdsch_dmrs 11 /s, pusch_dmrs 11 /s, ssb 1200 /s, pusch_data 0, srs 0. 923 A1: csi_rs 164 /s (99.5 /s with 217 PRB), pdsch_data 366 /s (4573 with nb_rb >= 128), pdsch_dmrs 6.9 /s, pusch_dmrs 582 /s (6863 with nb_rb >= 128), pusch_data 4, srs 0 (`srs_jobs_dropped` 68, `srs_budget_skip` 68544). 508 A1: pdsch_data 38.5 /s, pdsch_dmrs 60.5 /s, rest 0. Sensing drops <= 0.09 % per source. CPU 6.91 / 7.42 / 5.15 cores. Pdschq crc_ok 17622 of 22949 queued in 415 A1 (an unusually busy window). Observation only: legacy mode exports 1-2 orders of magnitude more PDSCH data/pusch DM-RS records than mode=sensing on the same cell within the same hour (e.g. 923 pdsch_data 366 /s vs 0-5.9 /s), because mode=sensing admits only scheduler-approved units; it is not an A/B because load varies.

## 6. 403 attempt (reported as PCI 27, not 403)

6 launches (T1 x3 tries, S1 x3 tries), every one synced PCI 27 (barred=1, SIB1 shows mcc=222 mnc=50 cell_identity 0x3d0a3cdad, ACQ stays CORESET_VERIFIED). Traffic: only DCI 1_0 (6214 DCIs, SI 5474, TC 73, C 667), 14 RNTIs with >= 2 DCIs, 0 solved, PDSCH decodes 4946 (4535 CRC ok = 91.7 %, SI), no UL, drops 0. Sensing: ssb 150 /s (valid 9/17/20 PRB, snr 6.1/7.1/8.3), csi_rs 66.6 /s (50 PRB, snr 4.3/9.0/9.9), no PDSCH/PUSCH/SRS export, no wideband, scheduler units 0. CPU 4.6-4.7 cores. Not a 403 result; 403 itself remains un-locked tonight with this triple.

## 7. Summary of problems found

1. mode=sensing cuts `pdsch_dmrs_blind` and `pusch_data` (by design in `nr_isac_mode.c`); the ALL arm has to be run without `mode` and then has no quality block or scheduler.
2. PDSCH CFR export in sensing mode is intermittent: units gated off by the scheduler in 923 S1 and 415 S2 (loss of all 5008 / ~3000 CRC-ok PDSCH CFR exports in those runs).
3. PRACH worker drops 26-47 % on 923 and 7.8 % on 415 A1 (starved worker, ~3 ms/job), exported PRACH 0 everywhere.
4. SRS and PUSCH are not usable at this site/time: SRS <= 4 records/run; PUSCH CRC ok <= 9 per run; PUSCH DM-RS CFR valid in 1-2 % of records.
5. 508: only fallback DCI 1_0 and CSI-RS undiscovered; dedicated traffic never solved; no wideband sensing records. 403: PCI 27 locked instead.
6. Wide-grant (>= 128 PRB) DL CRC: 51-68 % on 923 and 415 T2.
7. SLOWPASS_BT backtraces (thread passivePdcch1, `nr_cell_heap_get` -> calloc -> mmap) appear once in 415/508/923 runs: a slow-pass diagnostic, not a crash (RX_EXIT=130 everywhere).

Not checked: daytime load, 2-PCI behaviour, a per-grant trace for why individual DCIs were not decoded, root cause of the scheduler's unit choice, cfr_path output (not written by cfg).

## Appendix: raw per-run analyzer output (an_sp.py; lines cut at 420 chars)

### sp415T1
```
RUN /home/nicola/NICOLA/ota_runs/sp415T1_1_225342 mode T PCI synced ['415'] exit RX_EXIT=130 cpu_cores 6.520
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x28 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED', 'CORESET_VERIFIED -> CELL_CONFIGURED']
record types other: {'session': 3, 'config': 819} rs span_s None
DCI records 67300 by format {'1_0': 3168, '1_1': 23938, '0_1': 40193, '0_0': 1}
DCI by (dir,rnti class) {'DL/SI': 2547, 'DL/C': 24537, 'DL/RA': 3, 'UL/UL/none': 40194, 'DL/TC': 19}
distinct RNTI (dir,rnti) 733 >=2 DCIs 119 by dir {'DL': 70, 'UL': 49} SOLVED RNTIs 100 of which >=2 DCIs 100
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 2835 / 23938 = 11.8%
decode records (non-probe) by dir: {'DL': 1297, 'DL_ok': 1208, 'probe': 41199, 'UL': 1624}  >=128PRB: {'DL': 286, 'DL_ok': 276, 'UL': 15}
DCI counters: accepted 00/01/10/11 1 39402 3112 22900 dup 11 rec generated/persisted/lost 65415 65415 0 state cand/fixed/solved/unsolved 61 3113 49257 12984
PDSCH counters: queued 5655 decoded 5587 crc_ok 1159 reasons {'queue_expired': 34, 'future_unready': 0, 'copy_expired': 1, 'unsupported': 33, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 38131 crc_ok 3 | probe_decodes 40639 dl/ul 4126 36513
DROPS scanq full/stale/epoch 38/17/0 of 336054 (0.02%) | pdschq full/stale/epoch/narrow 0/35/0/0 of 5655 (0.62%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 4 submitted/rescued 219 8 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 44721 ul_decode_skip 24793 probe_skip 128 e1_skip_classes 3 e3_skip 56 e3_trunc 44 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 1159 sense_unexported_pusch_data 3 sense_io_errors 0 sense_recon_skip 0
```

### sp415T2
```
RUN /home/nicola/NICOLA/ota_runs/sp415T2_1_230341 mode T PCI synced ['415'] exit RX_EXIT=130 cpu_cores 6.563
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 2 sync_losses 0 log: SIB1 decoded x2 CSIRS_BLIND CONFIRMED x40 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'config': 916} rs span_s None
DCI records 98952 by format {'1_0': 3218, '1_1': 34760, '0_1': 60974}
DCI by (dir,rnti class) {'DL/TC': 15, 'DL/SI': 2497, 'DL/C': 35453, 'DL/RA': 13, 'UL/UL/none': 60974}
distinct RNTI (dir,rnti) 818 >=2 DCIs 135 by dir {'DL': 65, 'UL': 70} SOLVED RNTIs 96 of which >=2 DCIs 94
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 2943 / 34760 = 8.5%
decode records (non-probe) by dir: {'DL': 1912, 'DL_ok': 1698, 'probe': 13961, 'UL': 1481}  >=128PRB: {'DL': 162, 'DL_ok': 110, 'UL': 12}
DCI counters: accepted 00/01/10/11 0 60358 3168 34458 dup 22 rec generated/persisted/lost 97984 97984 0 state cand/fixed/solved/unsolved 56 3168 55719 39041
PDSCH counters: queued 8776 decoded 8735 crc_ok 1695 reasons {'queue_expired': 10, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 31, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 8714 crc_ok 1 | probe_decodes 13713 dl/ul 6459 7254
DROPS scanq full/stale/epoch 32/27/0 of 336066 (0.02%) | pdschq full/stale/epoch/narrow 0/10/0/0 of 8776 (0.11%) | puschq_epoch 0 | recov dropped full/old expired/stale 2 0 0 19 submitted/rescued 371 12 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 50305 ul_decode_skip 20280 probe_skip 87 e1_skip_classes 22 e3_skip 51 e3_trunc 168 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 1695 sense_unexported_pusch_data 1 sense_io_errors 0 sense_recon_skip 0
```

### sp415S1
```
RUN /home/nicola/NICOLA/ota_runs/sp415S1_1_225842 mode S PCI synced ['415'] exit RX_EXIT=130 cpu_cores 6.447
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x41 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 14211} rs span_s 213.8
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 11747 decoded 11667 crc_ok 5290 reasons {'queue_expired': 44, 'future_unready': 0, 'copy_expired': 8, 'unsupported': 28, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 12782 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 25/16/0 of 336096 (0.01%) | pdschq full/stale/epoch/narrow 0/52/0/0 of 11747 (0.44%) | puschq_epoch 0 | recov dropped full/old expired/stale 7 0 0 101 submitted/rescued 2508 291 | sense_rec_drop 0 sense_cfr_drop 9 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 2889 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 213.8s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=135674    634.64/s | prbs n=135674 p10/50/90=22.0/52.0/52.0 max=52 | wb 0 (0.000/s) | snr n=135652 p10/50/90=4.8/10.52/20.19 | hist {0: 17167, 5: 45579, 10: 34486, 15: 22908, 20: 15512} | exp=133249 prod=133258 cfr_drop=9 async_drop=0/0 drop%=0.01 pack=133249
  pdsch_data n=2436       11.39/s | prbs n=2436 p10/50/90=0.0/0.0/42.0 max=215 | wb 22 (0.103/s) | snr n=835 p10/50/90=10.88/14.74/16.65 | hist {0: 33, 5: 25, 10: 544, 15: 219, 20: 12, 25: 2} | exp=2401 prod=1431 cfr_drop=0 async_drop=1/1422 drop%=0.04 pack=2401
  pdsch_dmrs n=1174        5.49/s | prbs n=1174 p10/50/90=8.0/42.0/42.0 max=215 | wb 12 (0.056/s) | snr n=1174 p10/50/90=3.84/18.38/19.0 | hist {0: 463, 5: 58, 10: 25, 15: 628} | exp=1139 prod=2150 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=1139
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=10463 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=283         1.32/s | prbs n=283 p10/50/90=0.0/0.0/0.0 max=0 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=283 prod=283 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=283
  srs        n=4           0.02/s | prbs n=4 p10/50/90=8.0/8.0/8.0 max=8 | wb 0 (0.000/s) | snr n=4 p10/50/90=11.32/15.0/17.35 | hist {5: 1, 10: 1, 15: 2} | exp=4 prod=89321 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=4
  ssb        n=256371   1199.22/s | prbs n=256371 p10/50/90=6.0/13.0/20.0 max=21 | wb 0 (0.000/s) | snr n=256371 p10/50/90=5.47/7.67/15.55 | hist {0: 11276, 5: 169002, 10: 45401, 15: 30546, 20: 39, 25: 107} | exp=251835 prod=251835 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=251835
  record kinds other: grant records 14211 dci 0 decode 0
SRS: gate_hist [78779, 10225, 296, 16, 1, 0, 0] gate_det [0, 0, 0, 4, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 1, 1, 2, 0, 0] jobs_dropped 0 pregate_skip 89300 budget_skip 3763 backoff_skip 0 audits 0 idle_resets 129 gate_open pusch/unexpl 0 17 srs_us 7000385
UL: pusch_try 12782 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 0 10463 29 0
SCHED: units_enabled 4 enables/disables/deferrals/decisions 14 10 1 53 binding pool disabled_skips 51103 violations 1 floor_prb 32 gap_p95_ms 49 gap_max_ms 69487 wb_valid 34 wb_valid_prbs 6298 rec_count/drop 791691 0 gpu_budget_used_pct 15.848
T_cell trace (t_cell:gap_p95_ms) n=54: 128:4000.0 32:85.2 32:630.3 32:99.2 32:26.9 32:141.3 32:393.8 32:199.6 32:48.2 32:46.9 32:132.1 32:388.8 32:14.3 32:49.7
cont_floor values over rs records: {128: 13813, 32: 382120, None: 9}
score_sensing: min_prb=128 span=213.781s malformed=0 non_rs=14214 rs_without_quality=9
score_sensing: pci 415: produced=395933 wideband=34 gap p50/p95/max=0.002/0.865/69.488 s avail25ms=0.001 avail100ms=0.003 avail1s=0.014
score_sensing: AGGREGATE (union of cells): wb=34 gap p50/p95/max=0.002/0.865/69.488 s avail25ms=0.001 avail100ms=0.003 avail1s=0.014 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 1, "violations_max": 1, "disabled_skips_last": 51103, "units_enabled_series": [0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 2, 3, 3, 5, 3, 5, 3, 3, 3, 3, 3, 4]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 10463 exported 0 | pusch_data prod 0 exp 0
```

### sp415S2
```
RUN /home/nicola/NICOLA/ota_runs/sp415S2_1_230840 mode S PCI synced ['415'] exit RX_EXIT=130 cpu_cores 6.834
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x34 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 14198} rs span_s 213.3
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 10197 decoded 9805 crc_ok 3028 reasons {'queue_expired': 296, 'future_unready': 0, 'copy_expired': 5, 'unsupported': 87, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 18547 crc_ok 1 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 26/25/0 of 336089 (0.02%) | pdschq full/stale/epoch/narrow 0/301/0/0 of 10197 (2.95%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 109 submitted/rescued 817 27 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 2905 sense_unexported_pusch_data 1 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 213.3s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=109690    514.13/s | prbs n=109690 p10/50/90=21.0/52.0/52.0 max=52 | wb 0 (0.000/s) | snr n=109678 p10/50/90=4.76/11.34/20.87 | hist {0: 13653, 5: 33027, 10: 27683, 15: 17390, 20: 17925} | exp=108304 prod=108304 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=108304
  pdsch_data n=123         0.58/s | prbs n=123 p10/50/90=0.0/0.0/0.0 max=0 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=123 prod=41 cfr_drop=0 async_drop=0/41 drop%=0.00 pack=123
  pdsch_dmrs n=1           0.00/s | prbs n=1 p10/50/90=16.0/16.0/16.0 max=16 | wb 0 (0.000/s) | snr n=1 p10/50/90=15.5/15.5/15.5 | hist {15: 1} | exp=1 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=1
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=10240 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=2060        9.66/s | prbs n=2060 p10/50/90=0.0/0.0/0.0 max=8 | wb 0 (0.000/s) | snr n=15 p10/50/90=3.14/4.27/5.95 | hist {0: 9, 5: 6} | exp=2060 prod=2060 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=2060
  srs        n=1           0.00/s | prbs n=1 p10/50/90=8.0/8.0/8.0 max=8 | wb 0 (0.000/s) | snr n=1 p10/50/90=8.37/8.37/8.37 | hist {5: 1} | exp=1 prod=109082 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=1
  ssb        n=255864   1199.27/s | prbs n=255864 p10/50/90=6.0/13.0/20.0 max=21 | wb 0 (0.000/s) | snr n=255864 p10/50/90=5.49/7.7/15.57 | hist {0: 10968, 5: 168936, 10: 45118, 15: 30699, 20: 51, 25: 92} | exp=251856 prod=251856 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=251856
  record kinds other: grant records 14198 dci 0 decode 0
SRS: gate_hist [96182, 12553, 339, 7, 0, 0, 0] gate_det [0, 0, 0, 1, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 1, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 109074 budget_skip 2555 backoff_skip 0 audits 0 idle_resets 140 gate_open pusch/unexpl 0 7 srs_us 8548104
UL: pusch_try 18547 pusch_crc_ok 1 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 0 10240 252 0
SCHED: units_enabled 1 enables/disables/deferrals/decisions 16 15 1 53 binding queue_ul disabled_skips 31967 violations 0 floor_prb 32 gap_p95_ms 1003 gap_max_ms 0 wb_valid 0 wb_valid_prbs 0 rec_count/drop 738789 0 gpu_budget_used_pct 16.241
T_cell trace (t_cell:gap_p95_ms) n=54: 128:4000.0 128:2914.3 32:323.5 32:73.7 32:160.9 32:1003.9 32:229.4 32:80.1 32:224.8 32:68.5 32:73.8 32:277.1 32:104.0 32:1003.0
cont_floor values over rs records: {128: 65341, 32: 302398}
score_sensing: min_prb=128 span=213.350s malformed=0 non_rs=14201 rs_without_quality=0
score_sensing: pci 415: produced=367739 wideband=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 0, "violations_max": 0, "disabled_skips_last": 31967, "units_enabled_series": [0, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 2, 3, 1, 2, 1, 2, 2, 2, 1, 1, 1]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 10240 exported 0 | pusch_data prod 0 exp 0
```

### sp415A1
```
RUN /home/nicola/NICOLA/ota_runs/sp415A1_1_231340 mode A PCI synced ['415'] exit RX_EXIT=130 cpu_cores 6.911
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 2 sync_losses 0 log: SIB1 decoded x2 CSIRS_BLIND CONFIRMED x35 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 22350} rs span_s 214.3
DCI records 0 by format {}
DCI by (dir,rnti class) {}
distinct RNTI (dir,rnti) 0 >=2 DCIs 0 by dir {} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 22949 decoded 22874 crc_ok 17622 reasons {'queue_expired': 13, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 56, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 7437 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 57/29/0 of 336045 (0.03%) | pdschq full/stale/epoch/narrow 0/13/0/0 of 22949 (0.06%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 7 submitted/rescued 1474 93 | sense_rec_drop 0 sense_cfr_drop 12 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 0 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip None
SENSE sources: records/s [span 214.3s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=138892    648.18/s | prbs n=138892 p10/50/90=52.0/52.0/52.0 max=52 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=135964 prod=135976 cfr_drop=12 async_drop=0/0 drop%=0.01 pack=135964
  pdsch_data n=30627     142.93/s | prbs n=30627 p10/50/90=16.0/16.0/42.0 max=217 | wb 549 (2.562/s) | snr n=30603 p10/50/90=7.18/12.97/21.04 | hist {-10: 63, -5: 210, 0: 793, 5: 8137, 10: 8760, 15: 9030, 20: 2561, 25: 1005, 30: 44} | exp=29594 prod=15853 cfr_drop=0 async_drop=28/15832 drop%=0.09 pack=29594
  pdsch_dmrs n=2373       11.07/s | prbs n=2373 p10/50/90=42.0/42.0/42.0 max=42 | wb 0 (0.000/s) | snr n=2373 p10/50/90=3.62/5.99/18.7 | hist {-10: 35, -5: 44, 0: 854, 5: 263, 10: 41, 15: 1136} | exp=2356 prod=4712 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=2356
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=9674 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=2369       11.06/s | prbs n=2369 p10/50/90=3.0/47.0/124.0 max=211 | wb 211 (0.985/s) | snr n=2363 p10/50/90=-9.32/-7.23/-4.57 | hist {-30: 1, -25: 4, -20: 15, -15: 123, -10: 1928, -5: 283, 0: 9} | exp=2264 prod=2264 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=2264
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=125986 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=257106   1199.86/s | prbs n=257106 p10/50/90=20.0/20.0/20.0 max=20 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=251982 prod=251982 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=251982
  record kinds other: grant records 22350 dci 0 decode 0
SRS: gate_hist None gate_det None snr_hist_confirmed None unconfirmed None jobs_dropped 0 pregate_skip 125978 budget_skip 2496 backoff_skip 0 audits 0 idle_resets 110 gate_open pusch/unexpl 0 8 srs_us 9841793
UL: pusch_try 7437 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip None PRACH detections/occasions/drops/skipped 0 9674 823 0
SCHED: units_enabled None enables/disables/deferrals/decisions None None None None binding None disabled_skips None violations None floor_prb None gap_p95_ms None gap_max_ms None wb_valid None wb_valid_prbs None rec_count/drop 443722 0 gpu_budget_used_pct 16.869
T_cell trace (t_cell:gap_p95_ms) n=0: 
cont_floor values over rs records: {None: 431367}
score_sensing: min_prb=128 span=0.000s malformed=0 non_rs=22353 rs_without_quality=431367
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=n/a avail100ms=n/a avail1s=n/a rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": null, "violations_max": null, "disabled_skips_last": null, "units_enabled_series": []}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 9674 exported 0 | pusch_data prod 0 exp 0
```

### sp923T1
```
RUN /home/nicola/NICOLA/ota_runs/sp923T1_1_231840 mode T PCI synced ['923'] exit RX_EXIT=130 cpu_cores 6.891
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x6 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED', 'CORESET_VERIFIED -> CELL_CONFIGURED']
record types other: {'session': 3, 'config': 414} rs span_s None
DCI records 128557 by format {'1_0': 3701, '1_1': 76704, '0_1': 48151, '0_0': 1}
DCI by (dir,rnti class) {'DL/SI': 3458, 'DL/C': 76904, 'DL/TC': 17, 'DL/RA': 26, 'UL/UL/none': 48152}
distinct RNTI (dir,rnti) 317 >=2 DCIs 109 by dir {'DL': 71, 'UL': 38} SOLVED RNTIs 85 of which >=2 DCIs 85
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 23756 / 76704 = 31.0%
decode records (non-probe) by dir: {'DL': 2770, 'DL_ok': 2442, 'probe': 123189, 'UL': 2439}  >=128PRB: {'DL': 437, 'DL_ok': 281, 'UL': 55}
DCI counters: accepted 00/01/10/11 1 46506 3610 73396 dup 0 rec generated/persisted/lost 123513 123513 0 state cand/fixed/solved/unsolved 272 3611 114668 4962
PDSCH counters: queued 36569 decoded 35997 crc_ok 2343 reasons {'queue_expired': 331, 'future_unready': 0, 'copy_expired': 18, 'unsupported': 223, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 90760 crc_ok 7 | probe_decodes 119052 dl/ul 30778 88274
DROPS scanq full/stale/epoch 37/91/0 of 304050 (0.04%) | pdschq full/stale/epoch/narrow 0/349/0/0 of 36569 (0.95%) | puschq_epoch 0 | recov dropped full/old expired/stale 1 0 0 208 submitted/rescued 2296 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 97533 ul_decode_skip 42932 probe_skip 1168 e1_skip_classes 262 e3_skip 4895 e3_trunc 1299 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 2343 sense_unexported_pusch_data 7 sense_io_errors 0 sense_recon_skip 0
```

### sp923T2
```
RUN /home/nicola/NICOLA/ota_runs/sp923T2_1_232853 mode T PCI synced ['923'] exit RX_EXIT=130 cpu_cores 7.277
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x5 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'config': 349} rs span_s None
DCI records 193559 by format {'1_0': 3552, '1_1': 122833, '0_1': 67172, '0_0': 2}
DCI by (dir,rnti class) {'DL/SI': 3400, 'DL/C': 122967, 'DL/RA': 9, 'DL/TC': 9, 'UL/UL/none': 67174}
distinct RNTI (dir,rnti) 262 >=2 DCIs 125 by dir {'DL': 79, 'UL': 46} SOLVED RNTIs 91 of which >=2 DCIs 91
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 40205 / 122833 = 32.7%
decode records (non-probe) by dir: {'DL': 4289, 'DL_ok': 3638, 'probe': 144681, 'UL': 3203}  >=128PRB: {'DL': 724, 'DL_ok': 366, 'UL': 65}
DCI counters: accepted 00/01/10/11 2 65732 3478 120866 dup 0 rec generated/persisted/lost 190078 190078 0 state cand/fixed/solved/unsolved 254 3480 172739 13605
PDSCH counters: queued 50788 decoded 49784 crc_ok 3577 reasons {'queue_expired': 705, 'future_unready': 0, 'copy_expired': 25, 'unsupported': 274, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 102600 crc_ok 5 | probe_decodes 141913 dl/ul 42591 99322
DROPS scanq full/stale/epoch 34/250/0 of 336055 (0.08%) | pdschq full/stale/epoch/narrow 0/730/0/0 of 50788 (1.44%) | puschq_epoch 0 | recov dropped full/old expired/stale 1 0 0 208 submitted/rescued 2861 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 147735 ul_decode_skip 56532 probe_skip 1501 e1_skip_classes 503 e3_skip 8919 e3_trunc 2223 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 3577 sense_unexported_pusch_data 5 sense_io_errors 0 sense_recon_skip 0
```

### sp923S1
```
RUN /home/nicola/NICOLA/ota_runs/sp923S1_1_232343 mode S PCI synced ['923'] exit RX_EXIT=130 cpu_cores 7.584
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x6 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 156085} rs span_s 214.9
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 62452 decoded 61065 crc_ok 5008 reasons {'queue_expired': 1157, 'future_unready': 0, 'copy_expired': 39, 'unsupported': 190, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 146378 crc_ok 9 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 40/66/0 of 336051 (0.03%) | pdschq full/stale/epoch/narrow 0/1196/0/0 of 62452 (1.92%) | puschq_epoch 0 | recov dropped full/old expired/stale 7 0 0 1141 submitted/rescued 9106 17 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 5008 sense_unexported_pusch_data 9 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 214.9s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=39573     184.17/s | prbs n=39573 p10/50/90=4.0/217.0/217.0 max=217 | wb 21377 (99.486/s) | snr n=39357 p10/50/90=3.62/19.63/21.64 | hist {0: 17229, 5: 751, 10: 35, 15: 1998, 20: 19344} | exp=38605 prod=38605 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=38605
  pdsch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  prach      n=1           0.00/s | prbs n=1 p10/50/90=0.0/0.0/0.0 max=0 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=15286 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=15739      73.25/s | prbs n=15739 p10/50/90=0.0/0.0/0.0 max=10 | wb 0 (0.000/s) | snr n=344 p10/50/90=3.23/4.5/7.35 | hist {0: 221, 5: 120, 10: 2, 15: 1} | exp=15122 prod=15122 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=15122
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=110868 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=32232     150.00/s | prbs n=32232 p10/50/90=9.0/20.0/21.0 max=21 | wb 0 (0.000/s) | snr n=32232 p10/50/90=7.73/9.21/10.64 | hist {0: 5, 5: 24429, 10: 7798} | exp=31503 prod=31503 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=31503
  record kinds other: grant records 156085 dci 0 decode 0
SRS: gate_hist [100376, 10309, 182, 1, 0, 0, 0] gate_det [0, 0, 0, 0, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 0, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 110867 budget_skip 62477 backoff_skip 0 audits 0 idle_resets 43 gate_open pusch/unexpl 0 1 srs_us 9160289
UL: pusch_try 146378 pusch_crc_ok 9 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 1 15286 5708 0
SCHED: units_enabled 1 enables/disables/deferrals/decisions 9 8 1 53 binding queue_ul disabled_skips 156231 violations 3 floor_prb 32 gap_p95_ms 12 gap_max_ms 0 wb_valid 0 wb_valid_prbs 0 rec_count/drop 322562 0 gpu_budget_used_pct 15.211
T_cell trace (t_cell:gap_p95_ms) n=54: 128:4000.0 128:17.9 128:24.7 64:19.4 64:22.1 64:69.5 32:14.6 32:41.7 32:32.9 32:44.7 32:33.7 32:68.5 32:22.7 32:12.7
cont_floor values over rs records: {128: 17620, 32: 45455, 48: 1079, 64: 23390, None: 1}
score_sensing: min_prb=128 span=214.874s malformed=0 non_rs=156088 rs_without_quality=1
score_sensing: pci 923: produced=87544 wideband=21377 gap p50/p95/max=0.000/0.039/0.040 s avail25ms=0.624 avail100ms=0.998 avail1s=1.000
score_sensing: AGGREGATE (union of cells): wb=21377 gap p50/p95/max=0.000/0.039/0.040 s avail25ms=0.624 avail100ms=0.998 avail1s=1.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 3, "violations_max": 3, "disabled_skips_last": 156231, "units_enabled_series": [0, 1, 2, 2, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 0, 0, 1]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 15286 exported 0 | pusch_data prod 0 exp 0
```

### sp923S2
```
RUN /home/nicola/NICOLA/ota_runs/sp923S2_1_233400 mode S PCI synced ['923'] exit RX_EXIT=130 cpu_cores 7.205
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x4 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 107697} rs span_s 211.3
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 64226 decoded 63445 crc_ok 4305 reasons {'queue_expired': 507, 'future_unready': 0, 'copy_expired': 22, 'unsupported': 252, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 99160 crc_ok 4 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 36/35/0 of 336052 (0.02%) | pdschq full/stale/epoch/narrow 0/529/0/0 of 64226 (0.82%) | puschq_epoch 0 | recov dropped full/old expired/stale 10 0 0 787 submitted/rescued 8856 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 3056 sense_unexported_pusch_data 4 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 211.3s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=39494     186.94/s | prbs n=39494 p10/50/90=3.0/217.0/217.0 max=217 | wb 21040 (99.589/s) | snr n=39147 p10/50/90=3.54/19.37/21.62 | hist {0: 17444, 5: 663, 10: 25, 15: 2004, 20: 19011} | exp=39238 prod=39238 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=39238
  pdsch_data n=1249        5.91/s | prbs n=1249 p10/50/90=0.0/0.0/16.0 max=217 | wb 54 (0.256/s) | snr n=243 p10/50/90=8.66/14.6/18.25 | hist {0: 2, 5: 52, 10: 71, 15: 108, 20: 10} | exp=1249 prod=626 cfr_drop=0 async_drop=0/626 drop%=0.00 pack=1249
  pdsch_dmrs n=13661      64.66/s | prbs n=13661 p10/50/90=16.0/199.0/217.0 max=217 | wb 9044 (42.808/s) | snr n=13323 p10/50/90=5.96/13.52/18.32 | hist {0: 993, 5: 1965, 10: 5446, 15: 4518, 20: 401} | exp=13516 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=13516
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=15447 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=10757      50.92/s | prbs n=10757 p10/50/90=0.0/0.0/0.0 max=8 | wb 0 (0.000/s) | snr n=236 p10/50/90=3.29/4.44/7.78 | hist {0: 136, 5: 93, 10: 7} | exp=10757 prod=10757 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=10757
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=122371 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=31692     150.01/s | prbs n=31692 p10/50/90=9.0/20.0/21.0 max=21 | wb 0 (0.000/s) | snr n=31692 p10/50/90=7.68/9.19/10.63 | hist {0: 10, 5: 24100, 10: 7582} | exp=31503 prod=31503 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=31503
  record kinds other: grant records 107697 dci 0 decode 0
SRS: gate_hist [109977, 12184, 208, 2, 0, 0, 0] gate_det [0, 0, 0, 0, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 0, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 122369 budget_skip 18037 backoff_skip 0 audits 0 idle_resets 63 gate_open pusch/unexpl 0 2 srs_us 9898649
UL: pusch_try 99160 pusch_crc_ok 4 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 0 15447 5548 0
SCHED: units_enabled 3 enables/disables/deferrals/decisions 16 13 1 53 binding pool disabled_skips 88927 violations 24 floor_prb 128 gap_p95_ms 30 gap_max_ms 9157 wb_valid 9015 wb_valid_prbs 1812907 rec_count/drop 299483 0 gpu_budget_used_pct 14.234
T_cell trace (t_cell:gap_p95_ms) n=53: 128:4000.0 128:19.9 128:19.1 96:21.9 96:16.5 128:18.8 128:14.8 128:19.6 128:33.4 128:60.3 128:16.9 128:18.0 128:14.7 128:20.1
cont_floor values over rs records: {128: 86484, 96: 10369}
score_sensing: min_prb=128 span=211.269s malformed=0 non_rs=107700 rs_without_quality=0
score_sensing: pci 923: produced=96853 wideband=30138 gap p50/p95/max=0.000/0.039/0.040 s avail25ms=0.753 avail100ms=0.996 avail1s=1.000
score_sensing: AGGREGATE (union of cells): wb=30138 gap p50/p95/max=0.000/0.039/0.040 s avail25ms=0.753 avail100ms=0.996 avail1s=1.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 24, "violations_max": 24, "disabled_skips_last": 88927, "units_enabled_series": [0, 1, 2, 2, 3, 4, 3, 3, 3, 3, 3, 3, 3, 4, 4, 3, 2, 3, 3, 4, 3, 3]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 15447 exported 0 | pusch_data prod 0 exp 0
```

### sp923A1
```
RUN /home/nicola/NICOLA/ota_runs/sp923A1_1_233908 mode A PCI synced ['923'] exit RX_EXIT=130 cpu_cores 7.422
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x6 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 176641} rs span_s 200.2
DCI records 0 by format {}
DCI by (dir,rnti class) {}
distinct RNTI (dir,rnti) 0 >=2 DCIs 0 by dir {} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 108113 decoded 106935 crc_ok 36903 reasons {'queue_expired': 791, 'future_unready': 0, 'copy_expired': 29, 'unsupported': 358, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 124557 crc_ok 5 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 421/2166/0 of 320047 (0.81%) | pdschq full/stale/epoch/narrow 0/820/0/0 of 108113 (0.76%) | puschq_epoch 0 | recov dropped full/old expired/stale 6 0 0 724 submitted/rescued 13699 19 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 0 sense_unexported_pusch_data 1 sense_io_errors 0 sense_recon_skip None
SENSE sources: records/s [span 200.2s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=32926     164.49/s | prbs n=32926 p10/50/90=24.0/217.0/217.0 max=217 | wb 19924 (99.534/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=32894 prod=32894 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=32894
  pdsch_data n=73221     365.79/s | prbs n=73221 p10/50/90=16.0/16.0/48.0 max=217 | wb 4573 (22.845/s) | snr n=73180 p10/50/90=3.47/11.3/19.37 | hist {-15: 16, -10: 443, -5: 2312, 0: 8075, 5: 19272, 10: 22286, 15: 14773, 20: 5225, 25: 644, 30: 128, 35: 6} | exp=73142 prod=36032 cfr_drop=0 async_drop=655/35991 drop%=0.89 pack=73142
  pdsch_dmrs n=1390        6.94/s | prbs n=1390 p10/50/90=28.0/28.0/28.0 max=28 | wb 0 (0.000/s) | snr n=1390 p10/50/90=7.85/8.51/9.17 | hist {5: 1359, 10: 1, 15: 15, 20: 15} | exp=1387 prod=2774 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=1387
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=10609 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=4           0.02/s | prbs n=4 p10/50/90=2.3/4.5/33.3 max=45 | wb 0 (0.000/s) | snr n=4 p10/50/90=-8.24/-6.65/-5.37 | hist {-10: 4} | exp=4 prod=4 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=4
  pusch_dmrs n=116551    582.25/s | prbs n=116551 p10/50/90=2.0/25.0/120.0 max=216 | wb 6863 (34.285/s) | snr n=116188 p10/50/90=-10.02/-7.48/-4.56 | hist {-30: 31, -25: 155, -20: 676, -15: 10929, -10: 89393, -5: 14834, 0: 170} | exp=116376 prod=116376 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=116376
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=119920 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=30027     150.01/s | prbs n=30027 p10/50/90=20.0/20.0/20.0 max=20 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=30003 prod=30003 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=30003
  record kinds other: grant records 176641 dci 0 decode 0
SRS: gate_hist None gate_det None snr_hist_confirmed None unconfirmed None jobs_dropped 68 pregate_skip 118864 budget_skip 68544 backoff_skip 0 audits 0 idle_resets 238 gate_open pusch/unexpl 0 1056 srs_us 9897892
UL: pusch_try 124557 pusch_crc_ok 5 ul_decode_skip None sense_ul_delay_skip None PRACH detections/occasions/drops/skipped 0 10609 9385 0
SCHED: units_enabled None enables/disables/deferrals/decisions None None None None binding None disabled_skips None violations None floor_prb None gap_p95_ms None gap_max_ms None wb_valid None wb_valid_prbs None rec_count/drop 430209 0 gpu_budget_used_pct 13.669
T_cell trace (t_cell:gap_p95_ms) n=0: 
cont_floor values over rs records: {None: 254119}
score_sensing: min_prb=128 span=0.000s malformed=0 non_rs=176644 rs_without_quality=254119
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=n/a avail100ms=n/a avail1s=n/a rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": null, "violations_max": null, "disabled_skips_last": null, "units_enabled_series": []}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 10609 exported 0 | pusch_data prod 4 exp 4
```

### sp508T1
```
RUN /home/nicola/NICOLA/ota_runs/sp508T1_1_234420 mode T PCI synced ['508'] exit RX_EXIT=130 cpu_cores 4.984
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x0 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED', 'CORESET_VERIFIED -> CELL_CONFIGURED']
record types other: {'session': 3, 'config': 1693} rs span_s None
DCI records 12222 by format {'1_0': 12222}
DCI by (dir,rnti class) {'DL/SI': 10498, 'DL/C': 1676, 'DL/RA': 27, 'DL/TC': 21}
distinct RNTI (dir,rnti) 1674 >=2 DCIs 29 by dir {'DL': 29} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {'DL': 12217, 'DL_ok': 7443}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 0 0 11887 0 dup 1824 rec generated/persisted/lost 11887 11887 0 state cand/fixed/solved/unsolved 0 11887 0 0
PDSCH counters: queued 13103 decoded 11896 crc_ok 7241 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes 0 dl/ul 0 0
DROPS scanq full/stale/epoch 0/11/0 of 304055 (0.00%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 13103 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 0 ul_decode_skip 0 probe_skip 0 e1_skip_classes 0 e3_skip 0 e3_trunc 0 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 7241 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
```

### sp508T2
```
RUN /home/nicola/NICOLA/ota_runs/sp508T2_1_235410 mode T PCI synced ['508'] exit RX_EXIT=130 cpu_cores 4.850
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x0 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> SIB1_DECODED', 'SIB1_DECODED -> CELL_CONFIGURED']
record types other: {'session': 3, 'config': 1271} rs span_s None
DCI records 11579 by format {'1_0': 11579}
DCI by (dir,rnti class) {'DL/SI': 10274, 'DL/C': 1250, 'DL/TC': 23, 'DL/RA': 32}
distinct RNTI (dir,rnti) 1259 >=2 DCIs 15 by dir {'DL': 15} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {'DL': 11785, 'DL_ok': 7370}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 0 0 11498 0 dup 1826 rec generated/persisted/lost 11498 11498 0 state cand/fixed/solved/unsolved 0 11498 0 0
PDSCH counters: queued 12372 decoded 11702 crc_ok 7316 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes 0 dl/ul 0 0
DROPS scanq full/stale/epoch 23/25/0 of 304041 (0.02%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 12372 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 0 ul_decode_skip 0 probe_skip 0 e1_skip_classes 0 e3_skip 0 e3_trunc 0 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 7316 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
```

### sp508S1
```
RUN /home/nicola/NICOLA/ota_runs/sp508S1_1_234915 mode S PCI synced ['508'] exit RX_EXIT=130 cpu_cores 5.230
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x0 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 11956} rs span_s 194.6
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 12749 decoded 11697 crc_ok 7330 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 38/15/0 of 304055 (0.02%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 12749 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 7330 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 194.6s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_dmrs n=10074      51.76/s | prbs n=10074 p10/50/90=0.0/3.0/19.0 max=24 | wb 0 (0.000/s) | snr n=7595 p10/50/90=3.16/4.44/5.2 | hist {0: 6043, 5: 1552} | exp=9815 prod=19630 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=9815
  prach      n=1           0.01/s | prbs n=1 p10/50/90=0.0/0.0/0.0 max=0 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=37988 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=109914 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=175116    899.76/s | prbs n=175116 p10/50/90=3.0/6.0/15.0 max=20 | wb 0 (0.000/s) | snr n=174434 p10/50/90=4.43/5.56/6.92 | hist {0: 48445, 5: 125328, 10: 632, 15: 29} | exp=170958 prod=170958 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=170958
  record kinds other: grant records 11956 dci 0 decode 0
SRS: gate_hist [93112, 16119, 670, 13, 0, 0, 0] gate_det [0, 0, 0, 0, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 0, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 109901 budget_skip 0 backoff_skip 0 audits 0 idle_resets 67 gate_open pusch/unexpl 0 13 srs_us 4642607
UL: pusch_try 0 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 1 37988 0 0
SCHED: units_enabled 2 enables/disables/deferrals/decisions 2 0 1 48 binding queue_ul disabled_skips 11211 violations 0 floor_prb 32 gap_p95_ms 469 gap_max_ms 0 wb_valid 0 wb_valid_prbs 0 rec_count/drop 373247 0 gpu_budget_used_pct 15.972
T_cell trace (t_cell:gap_p95_ms) n=49: 128:4000.0 32:1001.6 32:591.6 32:650.5 32:712.5 32:518.4 32:332.7 32:359.0 32:339.1 32:432.9 32:433.1 32:407.1 32:514.0
cont_floor values over rs records: {128: 9696, 32: 175494, None: 1}
score_sensing: min_prb=128 span=194.625s malformed=0 non_rs=11959 rs_without_quality=1
score_sensing: pci 508: produced=185190 wideband=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 0, "violations_max": 0, "disabled_skips_last": 11211, "units_enabled_series": [0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 37988 exported 0 | pusch_data prod 0 exp 0
```

### sp508S2
```
RUN /home/nicola/NICOLA/ota_runs/sp508S2_1_235905 mode S PCI synced ['508'] exit RX_EXIT=130 cpu_cores 5.284
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x0 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> SIB1_DECODED', 'SIB1_DECODED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 12153} rs span_s 194.6
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 12480 decoded 11885 crc_ok 7376 reasons {'queue_expired': 1, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 32/21/0 of 304055 (0.02%) | pdschq full/stale/epoch/narrow 0/1/0/0 of 12480 (0.01%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 7078 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 194.6s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_data n=298         1.53/s | prbs n=298 p10/50/90=3.0/5.0/7.0 max=10 | wb 0 (0.000/s) | snr n=298 p10/50/90=4.51/5.35/5.85 | hist {0: 70, 5: 228} | exp=298 prod=298 cfr_drop=0 async_drop=0/297 drop%=0.00 pack=298
  pdsch_dmrs n=10119      52.00/s | prbs n=10119 p10/50/90=0.0/3.0/19.0 max=24 | wb 0 (0.000/s) | snr n=7626 p10/50/90=3.15/4.48/5.25 | hist {0: 5891, 5: 1735} | exp=9851 prod=19702 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=9851
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=37998 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=109923 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=175128    899.94/s | prbs n=175128 p10/50/90=3.0/6.0/14.0 max=20 | wb 0 (0.000/s) | snr n=174446 p10/50/90=4.42/5.54/6.89 | hist {0: 49156, 5: 124625, 10: 632, 15: 33} | exp=170988 prod=170988 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=170988
  record kinds other: grant records 12153 dci 0 decode 0
SRS: gate_hist [93408, 15831, 675, 8, 1, 0, 0] gate_det [0, 0, 0, 0, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 0, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 109914 budget_skip 0 backoff_skip 0 audits 0 idle_resets 24 gate_open pusch/unexpl 0 9 srs_us 4606510
UL: pusch_try 0 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 0 37998 0 0
SCHED: units_enabled 2 enables/disables/deferrals/decisions 3 1 1 48 binding queue_ul disabled_skips 11030 violations 0 floor_prb 32 gap_p95_ms 436 gap_max_ms 0 wb_valid 0 wb_valid_prbs 0 rec_count/drop 374162 0 gpu_budget_used_pct 16.029
T_cell trace (t_cell:gap_p95_ms) n=49: 128:2253.4 32:948.9 32:884.1 32:438.3 32:688.1 32:484.5 32:383.9 32:453.3 32:393.5 32:381.9 32:345.4 32:493.3 32:690.6
cont_floor values over rs records: {128: 9708, 32: 175837}
score_sensing: min_prb=128 span=194.599s malformed=0 non_rs=12156 rs_without_quality=0
score_sensing: pci 508: produced=185545 wideband=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 0, "violations_max": 0, "disabled_skips_last": 11030, "units_enabled_series": [0, 1, 2, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 37998 exported 0 | pusch_data prod 0 exp 0
```

### sp508A1
```
RUN /home/nicola/NICOLA/ota_runs/sp508A1_1_000400 mode A PCI synced ['508'] exit RX_EXIT=130 cpu_cores 5.152
ACQ CELL_CONFIGURED pbch_locks 1 sib1_decodes 1 sync_losses 0 log: SIB1 decoded x1 CSIRS_BLIND CONFIRMED x0 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> SIB1_DECODED', 'SIB1_DECODED -> CELL_CONFIGURED']
record types other: {'session': 3, 'grant': 11627} rs span_s 192.1
DCI records 0 by format {}
DCI by (dir,rnti class) {}
distinct RNTI (dir,rnti) 0 >=2 DCIs 0 by dir {} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 12938 decoded 11506 crc_ok 7283 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 16/24/0 of 304048 (0.01%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 12938 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 0 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip None
SENSE sources: records/s [span 192.1s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_data n=7403       38.54/s | prbs n=7403 p10/50/90=15.0/15.0/25.0 max=25 | wb 0 (0.000/s) | snr n=7363 p10/50/90=4.62/6.09/8.6 | hist {-5: 4, 0: 1895, 5: 5464} | exp=7322 prod=7322 cfr_drop=0 async_drop=0/7283 drop%=0.00 pack=7322
  pdsch_dmrs n=11628      60.53/s | prbs n=11628 p10/50/90=15.0/18.0/25.0 max=46 | wb 0 (0.000/s) | snr n=11628 p10/50/90=-3.25/4.57/5.37 | hist {-10: 250, -5: 1245, 0: 6883, 5: 3250} | exp=11507 prod=23014 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=11507
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=37997 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=113991 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=172878    899.90/s | prbs n=172878 p10/50/90=20.0/20.0/20.0 max=20 | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=170988 prod=170988 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=170988
  record kinds other: grant records 11627 dci 0 decode 0
SRS: gate_hist None gate_det None snr_hist_confirmed None unconfirmed None jobs_dropped 0 pregate_skip 113976 budget_skip 0 backoff_skip 0 audits 0 idle_resets 54 gate_open pusch/unexpl 0 15 srs_us 4798616
UL: pusch_try 0 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip None PRACH detections/occasions/drops/skipped 0 37997 0 0
SCHED: units_enabled None enables/disables/deferrals/decisions None None None None binding None disabled_skips None violations None floor_prb None gap_p95_ms None gap_max_ms None wb_valid None wb_valid_prbs None rec_count/drop 201326 0 gpu_budget_used_pct 15.773
T_cell trace (t_cell:gap_p95_ms) n=0: 
cont_floor values over rs records: {None: 191909}
score_sensing: min_prb=128 span=0.000s malformed=0 non_rs=11630 rs_without_quality=191909
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=n/a avail100ms=n/a avail1s=n/a rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": null, "violations_max": null, "disabled_skips_last": null, "units_enabled_series": []}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 37997 exported 0 | pusch_data prod 0 exp 0
```

### sp403T1
```
RUN /home/nicola/NICOLA/ota_runs/sp403T1_1_000855 mode T PCI synced ['27'] exit RX_EXIT=130 cpu_cores 4.675
ACQ CORESET_VERIFIED pbch_locks 1 sib1_decodes 0 sync_losses 0 log: SIB1 decoded x43 CSIRS_BLIND CONFIRMED x3 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED']
record types other: {'session': 3, 'config': 741} rs span_s None
DCI records 6214 by format {'1_0': 6214}
DCI by (dir,rnti class) {'DL/SI': 5474, 'DL/TC': 73, 'DL/C': 667}
distinct RNTI (dir,rnti) 726 >=2 DCIs 14 by dir {'DL': 14} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {'DL': 4946, 'DL_ok': 4535}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 0 0 5959 0 dup 0 rec generated/persisted/lost 5959 5959 0 state cand/fixed/solved/unsolved 0 5959 0 0
PDSCH counters: queued 5186 decoded 4749 crc_ok 4343 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes 0 dl/ul 0 0
DROPS scanq full/stale/epoch 0/2/0 of 400032 (0.00%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 5186 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 1 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 0 ul_decode_skip 0 probe_skip 0 e1_skip_classes 0 e3_skip 0 e3_trunc 0 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 4343 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
```

### sp403T1r2
```
RUN /home/nicola/NICOLA/ota_runs/sp403T1r2_1_001348 mode T PCI synced ['27'] exit RX_EXIT=130 cpu_cores 4.616
ACQ CORESET_VERIFIED pbch_locks 1 sib1_decodes 0 sync_losses 0 log: SIB1 decoded x44 CSIRS_BLIND CONFIRMED x3 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED']
record types other: {'session': 3, 'config': 711} rs span_s None
DCI records 6244 by format {'1_0': 6244}
DCI by (dir,rnti class) {'DL/SI': 5533, 'DL/TC': 80, 'DL/C': 631}
distinct RNTI (dir,rnti) 695 >=2 DCIs 14 by dir {'DL': 14} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {'DL': 5003, 'DL_ok': 4634}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 0 0 6183 0 dup 0 rec generated/persisted/lost 6183 6183 0 state cand/fixed/solved/unsolved 0 6183 0 0
PDSCH counters: queued 5102 decoded 4960 crc_ok 4594 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 3, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes 0 dl/ul 0 0
DROPS scanq full/stale/epoch 44/15/0 of 420035 (0.01%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 5102 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 0 ul_decode_skip 0 probe_skip 0 e1_skip_classes 0 e3_skip 0 e3_trunc 0 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 4594 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
```

### sp403T1r3
```
RUN /home/nicola/NICOLA/ota_runs/sp403T1r3_1_001844 mode T PCI synced ['27'] exit RX_EXIT=130 cpu_cores 4.631
ACQ CORESET_VERIFIED pbch_locks 1 sib1_decodes 0 sync_losses 0 log: SIB1 decoded x44 CSIRS_BLIND CONFIRMED x2 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED']
record types other: {'session': 3, 'config': 641} rs span_s None
DCI records 6222 by format {'1_0': 6222}
DCI by (dir,rnti class) {'DL/SI': 5582, 'DL/TC': 85, 'DL/C': 555}
distinct RNTI (dir,rnti) 629 >=2 DCIs 13 by dir {'DL': 13} SOLVED RNTIs 0 of which >=2 DCIs 0
DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null: 0 / 0 = 0.0%
decode records (non-probe) by dir: {'DL': 4999, 'DL_ok': 4653}  >=128PRB: {}
DCI counters: accepted 00/01/10/11 0 0 6162 0 dup 0 rec generated/persisted/lost 6162 6162 0 state cand/fixed/solved/unsolved 0 6162 0 0
PDSCH counters: queued 4953 decoded 4950 crc_ok 4609 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes 0 dl/ul 0 0
DROPS scanq full/stale/epoch 0/5/0 of 420031 (0.00%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 4953 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 1 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip 0 ul_decode_skip 0 probe_skip 0 e1_skip_classes 0 e3_skip 0 e3_trunc 0 pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 4609 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
```

### sp403S1
```
RUN /home/nicola/NICOLA/ota_runs/sp403S1_1_002340 mode S PCI synced ['27'] exit RX_EXIT=130 cpu_cores 4.670
ACQ CORESET_VERIFIED pbch_locks 1 sib1_decodes 0 sync_losses 0 log: SIB1 decoded x43 CSIRS_BLIND CONFIRMED x6 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED']
record types other: {'session': 3, 'grant': 5128} rs span_s 212.0
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 5084 decoded 5084 crc_ok 4696 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 45/15/0 of 420040 (0.01%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 5084 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 1 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 4696 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 212.0s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=14126      66.62/s | prbs n=14126 p10/50/90=10.0/50.0/50.0 max=50 | wb 0 (0.000/s) | snr n=14114 p10/50/90=4.28/8.98/9.9 | hist {0: 2927, 5: 10085, 10: 1102} | exp=13926 prod=13926 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=13926
  pdsch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=31809     150.02/s | prbs n=31809 p10/50/90=9.0/17.0/20.0 max=21 | wb 0 (0.000/s) | snr n=31809 p10/50/90=6.08/7.12/8.25 | hist {0: 43, 5: 31640, 10: 126} | exp=31503 prod=31503 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=31503
  record kinds other: grant records 5128 dci 0 decode 0
SRS: gate_hist [0, 0, 0, 0, 0, 0, 0] gate_det [0, 0, 0, 0, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 0, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 0 budget_skip 0 backoff_skip 0 audits 0 idle_resets 0 gate_open pusch/unexpl 0 0 srs_us 0
UL: pusch_try 0 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 0 0 0 0
SCHED: units_enabled 0 enables/disables/deferrals/decisions 0 0 1 53 binding pool disabled_skips 0 violations 0 floor_prb 32 gap_p95_ms 4000 gap_max_ms 0 wb_valid 0 wb_valid_prbs 0 rec_count/drop 95945 0 gpu_budget_used_pct 9.657
T_cell trace (t_cell:gap_p95_ms) n=53: 128:4000.0 128:4000.0 32:4000.0 32:3257.6 32:2961.2 32:1792.1 32:4000.0 32:3815.7 32:4000.0 32:4000.0 32:4000.0 32:2790.7 32:4000.0 32:4000.0
cont_floor values over rs records: {128: 4236, 32: 41699}
score_sensing: min_prb=128 span=212.039s malformed=0 non_rs=5131 rs_without_quality=0
score_sensing: pci 27: produced=45935 wideband=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 0, "violations_max": 0, "disabled_skips_last": 0, "units_enabled_series": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 0 exported 0 | pusch_data prod 0 exp 0
```

### sp403S1r2
```
RUN /home/nicola/NICOLA/ota_runs/sp403S1r2_1_002835 mode S PCI synced ['27'] exit RX_EXIT=130 cpu_cores 4.567
ACQ CORESET_VERIFIED pbch_locks 1 sib1_decodes 0 sync_losses 0 log: SIB1 decoded x43 CSIRS_BLIND CONFIRMED x4 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED']
record types other: {'session': 3, 'grant': 5039} rs span_s 211.5
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 5002 decoded 5002 crc_ok 4626 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 0, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 38/15/0 of 420038 (0.01%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 5002 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 4626 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 211.5s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=20542      97.11/s | prbs n=20542 p10/50/90=8.0/49.0/50.0 max=50 | wb 0 (0.000/s) | snr n=20539 p10/50/90=3.9/8.1/9.7 | hist {0: 8531, 5: 11095, 10: 913} | exp=20390 prod=20390 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=20390
  pdsch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=31734     150.01/s | prbs n=31734 p10/50/90=9.0/17.0/20.0 max=21 | wb 0 (0.000/s) | snr n=31734 p10/50/90=6.05/7.09/8.23 | hist {0: 60, 5: 31548, 10: 126} | exp=31503 prod=31503 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=31503
  record kinds other: grant records 5039 dci 0 decode 0
SRS: gate_hist [0, 0, 0, 0, 0, 0, 0] gate_det [0, 0, 0, 0, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 0, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 0 budget_skip 0 backoff_skip 0 audits 0 idle_resets 0 gate_open pusch/unexpl 0 0 srs_us 0
UL: pusch_try 0 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 0 0 0 0
SCHED: units_enabled 0 enables/disables/deferrals/decisions 0 0 1 53 binding pool disabled_skips 0 violations 0 floor_prb 32 gap_p95_ms 2390 gap_max_ms 0 wb_valid 0 wb_valid_prbs 0 rec_count/drop 108791 0 gpu_budget_used_pct 9.168
T_cell trace (t_cell:gap_p95_ms) n=53: 128:4000.0 128:4000.0 32:4000.0 32:4000.0 32:4000.0 32:4000.0 32:4000.0 32:4000.0 32:3338.7 32:4000.0 32:4000.0 32:2899.6 32:3055.3 32:2390.8
cont_floor values over rs records: {128: 4616, 32: 47660}
score_sensing: min_prb=128 span=211.540s malformed=0 non_rs=5042 rs_without_quality=0
score_sensing: pci 27: produced=52276 wideband=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 0, "violations_max": 0, "disabled_skips_last": 0, "units_enabled_series": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 0 exported 0 | pusch_data prod 0 exp 0
```

### sp403S1r3
```
RUN /home/nicola/NICOLA/ota_runs/sp403S1r3_1_003331 mode S PCI synced ['27'] exit RX_EXIT=130 cpu_cores 4.691
ACQ CORESET_VERIFIED pbch_locks 1 sib1_decodes 0 sync_losses 0 log: SIB1 decoded x43 CSIRS_BLIND CONFIRMED x7 coreset-found-lines 1
ACQ transitions: ['SEARCHING -> PBCH_LOCKED', 'PBCH_LOCKED -> CORESET_VERIFIED']
record types other: {'session': 3, 'grant': 4969} rs span_s 210.5
DCI counters: accepted 00/01/10/11 None None None None dup None rec generated/persisted/lost None None None state cand/fixed/solved/unsolved None None None None
PDSCH counters: queued 6029 decoded 4957 crc_ok 4595 reasons {'queue_expired': 0, 'future_unready': 0, 'copy_expired': 0, 'unsupported': 1, 'decode_error': 0, 'gpu_fallback': 0} | PUSCH try 0 crc_ok 0 | probe_decodes None dl/ul None None
DROPS scanq full/stale/epoch 46/14/0 of 420038 (0.01%) | pdschq full/stale/epoch/narrow 0/0/0/0 of 6029 (0.00%) | puschq_epoch 0 | recov dropped full/old expired/stale 0 0 0 0 submitted/rescued 0 0 | sense_rec_drop 0 sense_cfr_drop 0 inline_epoch 0
NOT-DECODED/NOT-SAVED counters: traffic_skip None ul_decode_skip None probe_skip None e1_skip_classes None e3_skip None e3_trunc None pdschq_stale_after_decode 0 td_cb0_budget_skips 0 sense_unexported_pdsch_data 4595 sense_unexported_pusch_data 0 sense_io_errors 0 sense_recon_skip 0
SENSE sources: records/s [span 210.5s] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack
  csirs      n=20888      99.24/s | prbs n=20888 p10/50/90=7.0/47.5/50.0 max=50 | wb 0 (0.000/s) | snr n=20862 p10/50/90=3.94/7.15/9.7 | hist {0: 8741, 5: 11143, 10: 978} | exp=20840 prod=20840 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=20840
  pdsch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pdsch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  prach      n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_data n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  pusch_dmrs n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  srs        n=0           0.00/s | prbs n=0 p10/50/90=None/None/None max=None | wb 0 (0.000/s) | snr n=0 p10/50/90=None/None/None | hist {} | exp=0 prod=0 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=0
  ssb        n=31575     150.02/s | prbs n=31575 p10/50/90=9.0/18.0/20.0 max=21 | wb 0 (0.000/s) | snr n=31575 p10/50/90=6.03/7.09/8.24 | hist {0: 73, 5: 31365, 10: 137} | exp=31503 prod=31503 cfr_drop=0 async_drop=0/0 drop%=0.00 pack=31503
  record kinds other: grant records 4969 dci 0 decode 0
SRS: gate_hist [0, 0, 0, 0, 0, 0, 0] gate_det [0, 0, 0, 0, 0, 0, 0] snr_hist_confirmed [0, 0, 0, 0, 0, 0, 0] unconfirmed [0, 0, 0, 0, 0, 0, 0] jobs_dropped 0 pregate_skip 0 budget_skip 0 backoff_skip 0 audits 0 idle_resets 0 gate_open pusch/unexpl 0 0 srs_us 0
UL: pusch_try 0 pusch_crc_ok 0 ul_decode_skip None sense_ul_delay_skip 0 PRACH detections/occasions/drops/skipped 0 0 0 0
SCHED: units_enabled 0 enables/disables/deferrals/decisions 0 0 1 53 binding pool disabled_skips 0 violations 0 floor_prb 32 gap_p95_ms 4000 gap_max_ms 0 wb_valid 0 wb_valid_prbs 0 rec_count/drop 109646 0 gpu_budget_used_pct 9.279
T_cell trace (t_cell:gap_p95_ms) n=52: 128:4000.0 128:4000.0 32:2987.5 32:3626.8 32:3513.5 32:1382.1 32:4000.0 32:4000.0 32:4000.0 32:4000.0 32:4000.0 32:4000.0 32:4000.0
cont_floor values over rs records: {128: 5797, 32: 46666}
score_sensing: min_prb=128 span=210.478s malformed=0 non_rs=4972 rs_without_quality=0
score_sensing: pci 27: produced=52463 wideband=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000
score_sensing: AGGREGATE (union of cells): wb=0 gap p50/p95/max=n/a/n/a/n/a s avail25ms=0.000 avail100ms=0.000 avail1s=0.000 rescue_fraction=n/a rescue_split=None
score_sensing: STABILITY {"0": {"violations_last": 0, "violations_max": 0, "disabled_skips_last": 0, "units_enabled_series": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}}
  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)
sense_prod/counters: prach prod_count 0 exported 0 | pusch_data prod 0 exp 0
```
