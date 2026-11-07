# CFR frame-ring drops on single-PCI (twomodes/single1-drop)

## Cause
1. [MEASURED OTA, cell 415, single1-q day runs, offline records] Drops are bursts, not a steady loss. 119 clusters in q1_415N5x3 (477 records with `cfr_frame_id` null); a typical cluster is 20-37 csirs records within 1-2.7 ms, the first at 7 s into the arm. Per-source totals (run.log final ISAC_METRICS, N5): csirs 370, pdsch_data 53, pdsch_dmrs 13, pusch_dmrs 10, prach 31 records without frame (prach is "no frame" by design, not a drop). So the "other sources" are mostly **csirs** (N1 177 of 244; N5 370 of 446; BASE B5 63 of 82) and a few pusch_dmrs/ssb. The csirs records are 52-PRB (nb_rb 52) in BOTH builds, so the 51-PRB wide limit is not the cause.
2. [MEASURED OTA offline] The cell sends CSI-RS in slot bursts: up to 16 frames per slot, 14-20 us apart, often in 2-3 consecutive slots (30-48 frames within ~1.5 ms). Number of 16-frame slots: N5 7562, B5 1440 (cell-side, varies run to run: B3 had 4022 and only 70 drops). The writer (one thread per cell) keeps each batch of frames pinned in the 32-slot ring while it computes their quality; any writer stall of a few ms in front of a burst overflows the ring (`nr_sense_api.c` push_cfr: `frame_count==frame_cap`).
3. [MEASURED replay, DGX, instrumented build, arm d2] The stalls are cold threshold builds: 31 pdsch_data frames took 2.0-4.9 ms each (mean 2.8 ms, n_re only 300-4000, i.e. not size-driven) in the first seconds; a 5.54 ms frame batch (2 pdsch_data frames) was followed within the same ms by 13 ring_full drops (12 csirs, 1 pdsch_data). The cost is the per-thread Laplace-inversion cache `sp_crit` (round 1-3 quality: heterogeneous noise for data-aided / window rows), cold for every (qm or eta, antennas, REs-per-PRB) key; the cache fills slowly on the air because new REs-per-PRB counts keep appearing (late bursts in N5: intervals 12-14). BASE has no such tables, hence 71 vs 252 drops. Writer average load is small (rec_us 15.3 vs 14.1 us/record, about 8 % busy), so this is peak stall, not average throughput. [HYPOTHESIS, not measured on air] Part of the late-run drops could be steady-state batches (16 frames x 0.37 ms = 6 ms at Nl=1 273 PRB); the replay did not show it.
Ruled out [MEASURED]: records offered (csirs nb_rb equal in both builds), ring size relative to average load (writer 8 % busy), replay-vs-OTA is a difference in rate only: the replay reproduced the stall and drops once in 3 un-warmed arms (d2: 13; d1, aq1, aq2: 0).

## Fix (commit f5ba41e43d)
- `nr_sense_quality.c:280` `nr_sense_quality_warm_step(n_ant, &cursor)`: builds one production `sp_crit` key per call, by the same function (cached value bit-identical, test below). Key set: window eta grid (22) x 1..30 rows, QAM 16/64/256 x 1..168 REs (SNR test) and 3..12 (residual test); 1158 keys, whole warm-up 0.2-0.3 s [MEASURED, unit test machine].
- `nr_sense_api.c:258-270` `run_writer`: when the writer has nothing queued (frames 0, records 0), sensing mode only, it runs one warm step instead of waiting (max ~4 ms before it looks at the queue again, 1/8 of the ring time). Own thread only: no new thread, no lock on RX/scan, no flag, traffic mode untouched (the branch requires `nr_isac_mode()==SENSING`), quality values unchanged (same valid PRBs).
- Not done: gcrit (Gamma/F) keys are continuous in (kT, nu) and not warmed; ring size unchanged (32). See open issues.

## Tests
- `tests/nr_sense_quality_test.cc` `SenseQualityDrop.WarmUpBuildsTheProductionKeysBitIdentically` (steps = 1158, one build each, 8 probe keys equal the on-demand values, all hit afterwards, n_ant 0 = no-op) and `SenseQualityDrop.HotShapeCostAfterWarmUp` (160 data-aided records 51..273 PRB, 16/64/256QAM, 4 ant: 0 sp misses, worst < 2 ms; measured mean 163 us, p90 299, worst 398 us).
- ctest -j4 full: 19 failures = the known list (7 Not Run pusch_uci, qam64, qam256, ul_disc, nr_td_sim_test, ssb_rate_match_prod, gpu_pass_cells, dft, ldpc_cuda_pool_cpu, modulation) plus test_nr_gpu_pass (flaky under -j4, passes alone) and static_audit x2 (my first version had a function-`static const` table; fixed by making it a local; `tools/mpci/static_audit.py --strict` rc 0, and ctest -R sense|isac|static_audit 14/14 pass after the fix). The full ctest was not re-run after that last one-word change.

## Replay A/B [MEASURED replay, DGX, raw_pci415_12s_b, 217 PRB, 4 RX, sensing cfg, 1.0x, one 12 s arm each]
Q = rr-single1q build (read-only), D = this branch. Order Q D D Q under both locks.
| arm | cfr_drop | records without CFR | >=51 valid PRB (pdsch_data / pdsch_dmrs / csirs) | >=128 (pdsch_data / pdsch_dmrs) | writer us/record |
|---|---|---|---|---|---|
| aq1 | 0 | 0 | 246 / 180 / 1256 | 101 / 83 | 21.0 |
| ad1 | 0 | 0 | 316 / 233 / 1315 | 120 / 108 | 19.3 |
| ad2 | 0 | 0 | 380 / 261 / 1268 | 148 / 123 | 18.3 |
| aq2 | 0 | 0 | 283 / 224 / 1251 | 113 / 104 | 20.9 |
Un-warmed instrumented arms (same capture): d1 0 drops, d2 13 drops (csirs 12, pdsch_data 1) with 31 slow frames; warmed instrumented arms w1, w2: 0 drops, 16-17 slow frames (the first version of the key set missed N values that are not multiples of 6; widened to 1..168 afterwards, not replayed with instrumentation).
**The replay does not reproduce the OTA drop rate** (0 drops in both Q arms), so it cannot show the A/B difference. The evidence for the cause is the OTA offline analysis (burst structure, per-source split) plus the d2 trace; the fix is verified by the unit tests and by no regression in the replay (records per source and valid-PRB rates are within the arm spread; D is not worse than Q).

## Open issues
1. OTA confirmation is missing: needs one 240 s arm on cell 415 of this build (expect `sense_cfr_drop` close to BASE 71 or lower). Not run: the multi-PCI campaign holds the X410.
2. Drops that are not cold builds (steady 16-frame batches at 273 PRB Nl=1, 6 ms) would remain; the remedy would be releasing ring slots before computing quality (no file sink) or sizing the ring from rate x batch time. Not done: no evidence for it.
3. gcrit (Gamma/F) cache misses are not warmed (cost per miss not measured on air).
4. Warm-up runs once per cell writer (0.2-0.3 s CPU each); with 2+ cells that is per cell.
5. A process violation: one `ninja` of this branch started while a receiver (the campaign's) was listed by `pgrep -x`; I stopped it after about 3 minutes (it only compiled in my own build dir). All later builds ran inside `flock -s` with the pgrep check inside the lock.
