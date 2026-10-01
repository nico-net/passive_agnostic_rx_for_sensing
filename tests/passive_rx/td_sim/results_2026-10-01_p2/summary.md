# P2 failure-only probe evidence: simulator gate

Label: `SIMULATED, DGX host, nr_td_sim @02524c44ff`. Arms paired per RNTI (same seed, channel draws, truth, engine RNG seed; only `--p2` differs). Matrix: `tests/passive_rx/td_sim/gate_p2.json`; runner `gate_p2.py`. Undecidable = hit the 3600 s grant cap (the simulator does not model probation withdrawal). Paired-decided = RNTIs decided in BOTH arms; time/trial comparisons are on that set (uncensored).

## Per (cell, rx, oracle)

| cell | rx | oracle | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | truth_elim P2 | same winner | differ (both decided) | P1 dec only | P2 dec only | paired-decided | median grants P1/P2 | mean s P1/P2 | median truth full-TB P1/P2 | n_full/RNTI P1/P2 | admitted probe FAILs P2 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| NSA-like | 4 | 1 | 74880 | 0 | 0 | 0 | 0 | 0 | 74880 | 0 | 0 | 0 | 74880 | 176 / 93 | 1.21 / 0.82 | 97 / 69 | 236 / 160 | 11426783 |
| NSA-like | 4 | 0 | 5760 | 0 | 234 | 313 | 369 | 0 | 5294 | 180 | 171 | 115 | 5276 | 62216 / 32284 | 491.30 / 249.10 | 143 / 102 | 96037 / 48667 | 527077239 |
| NSA-like | 1 | 1 | 74880 | 0 | 0 | 0 | 0 | 0 | 74880 | 0 | 0 | 0 | 74880 | 251 / 135 | 1.72 / 1.17 | 97 / 69 | 236 / 159 | 11424881 |
| NSA-like | 1 | 0 | 5760 | 0 | 212 | 505 | 393 | 0 | 5378 | 110 | 80 | 192 | 5175 | 75772 / 41898 | 626.71 / 336.45 | 128 / 100 | 85915 / 46050 | 476681600 |
| SA | 4 | 1 | 74880 | 0 | 0 | 0 | 0 | 0 | 74880 | 0 | 0 | 0 | 74880 | 175 / 93 | 1.20 / 0.81 | 97 / 70 | 233 / 158 | 11291741 |
| SA | 4 | 0 | 5760 | 0 | 236 | 314 | 369 | 0 | 5282 | 185 | 174 | 119 | 5272 | 59638 / 30940 | 497.51 / 241.88 | 143 / 101 | 97229 / 47251 | 515408064 |
| SA | 1 | 1 | 74880 | 0 | 0 | 0 | 0 | 0 | 74880 | 0 | 0 | 0 | 74880 | 251 / 134 | 1.72 / 1.16 | 97 / 70 | 234 / 158 | 11356449 |
| SA | 1 | 0 | 5760 | 0 | 250 | 433 | 368 | 0 | 5319 | 166 | 105 | 170 | 5222 | 84788 / 43920 | 638.79 / 342.16 | 142 / 101 | 87533 / 46783 | 464875948 |

## Per-criterion verdict per (cell, rx, oracle) (spec 5.4; PASS/FAIL)

Criteria: wrong = 0 in both arms; truth_eliminated_by_probe = 0; same winner for every paired RNTI (differ = 0 and no one-sided decision); true hypothesis not slower (median truth full-TB decodes and median grants-to-convergence P2 <= P1); full-TB decodes and convergence time reduced (mean, paired-decided); undecidable not increased.

| cell | rx | oracle | wrong=0 | truth_elim=0 | same winner | truth not slower | full-TB+time reduced | undecidable not up |
|---|---|---|---|---|---|---|---|---|
| NSA-like | 4 | 1 | PASS | PASS | PASS | PASS | PASS | PASS |
| NSA-like | 4 | 0 | FAIL | PASS | FAIL | PASS | PASS | FAIL |
| NSA-like | 1 | 1 | PASS | PASS | PASS | PASS | PASS | PASS |
| NSA-like | 1 | 0 | FAIL | PASS | FAIL | PASS | PASS | PASS |
| SA | 4 | 1 | PASS | PASS | PASS | PASS | PASS | PASS |
| SA | 4 | 0 | FAIL | PASS | FAIL | PASS | PASS | FAIL |
| SA | 1 | 1 | PASS | PASS | PASS | PASS | PASS | PASS |
| SA | 1 | 0 | FAIL | PASS | FAIL | PASS | PASS | PASS |

**oracle 1 overall: PASS**

**oracle 0 overall: FAIL**

## Wrong winners and undecidables by dimension (summed over cells and rx)


| oracle | twins | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | mean s P1/P2 (paired-decided) |
|---|---|---|---|---|---|---|---|
| 1 | 0 | 149760 | 0 | 0 | 0 | 0 | 1.46 / 0.99 |
| 1 | 2 | 149760 | 0 | 0 | 0 | 0 | 1.46 / 0.99 |
| 0 | 0 | 11520 | 0 | 0 | 0 | 0 | 349.24 / 229.23 |
| 0 | 2 | 11520 | 0 | 932 | 1565 | 1499 | 824.48 / 368.87 |

| oracle | table_exercise | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | mean s P1/P2 (paired-decided) |
|---|---|---|---|---|---|---|---|
| 1 | 0.5 | 149760 | 0 | 0 | 0 | 0 | 1.46 / 0.99 |
| 1 | 0.9 | 149760 | 0 | 0 | 0 | 0 | 1.46 / 0.99 |
| 0 | 0.5 | 11520 | 0 | 920 | 1565 | 1438 | 704.17 / 332.57 |
| 0 | 0.9 | 11520 | 0 | 12 | 0 | 61 | 446.30 / 258.53 |

| oracle | K | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | mean s P1/P2 (paired-decided) |
|---|---|---|---|---|---|---|---|
| 1 | 2 | 99840 | 0 | 0 | 0 | 0 | 1.46 / 1.09 |
| 1 | 3 | 99840 | 0 | 0 | 0 | 0 | 1.46 / 0.94 |
| 1 | 4 | 99840 | 0 | 0 | 0 | 0 | 1.46 / 0.94 |
| 0 | 2 | 7680 | 0 | 414 | 535 | 749 | 531.28 / 357.68 |
| 0 | 3 | 7680 | 0 | 518 | 510 | 474 | 571.90 / 288.70 |
| 0 | 4 | 7680 | 0 | 0 | 520 | 276 | 584.73 / 232.77 |

| oracle | p_true_snr_mu | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | mean s P1/P2 (paired-decided) |
|---|---|---|---|---|---|---|---|
| 1 | 8 | 99840 | 0 | 0 | 0 | 0 | 2.18 / 1.60 |
| 1 | 15 | 99840 | 0 | 0 | 0 | 0 | 1.33 / 0.85 |
| 1 | 25 | 99840 | 0 | 0 | 0 | 0 | 0.87 / 0.51 |
| 0 | 8 | 7680 | 0 | 449 | 1071 | 798 | 775.44 / 427.89 |
| 0 | 15 | 7680 | 0 | 405 | 494 | 548 | 580.39 / 298.49 |
| 0 | 25 | 7680 | 0 | 78 | 0 | 153 | 365.17 / 169.76 |

| oracle | catalog_tda | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | mean s P1/P2 (paired-decided) |
|---|---|---|---|---|---|---|---|
| 1 | 4 | 149760 | 0 | 0 | 0 | 0 | 1.46 / 0.99 |
| 1 | 16 | 149760 | 0 | 0 | 0 | 0 | 1.46 / 0.99 |
| 0 | 4 | 11520 | 0 | 482 | 782 | 767 | 567.61 / 293.57 |
| 0 | 16 | 11520 | 0 | 450 | 783 | 732 | 558.58 / 290.56 |

## Run record

- Simulator `nr_td_sim` built at commit 02524c44ff (branch td/convergence-levers) on the DGX host; runner
  `python3 tests/passive_rx/td_sim/gate_p2.py run --sim nr_td_sim --matrix tests/passive_rx/td_sim/gate_p2.json --raw RAW -j 8`,
  then `summarize`. Wall time [MEASURED, DGX host]: oracle 1 half 7 s, oracle 0 half 2396 s (8 processes).
- Acquisitions (4 RNTIs each) per (cell, rx): oracle 1 = 260 x 72 configs = 18 720; oracle 0 = 20 x 72 configs = 1 440;
  total 20 160 per (cell, rx), 80 640 overall, per arm. Oracle 0 was cut to 20 acquisitions per config for compute (~40 min
  for the blind half); the oracle 0 failure is already decisive at this size (932 wrong P2 winners vs 0 in P1).
- Twins: the brief asked {0, 3}; the simulator caps physical twins at 2 (all other-table entries), so the arms are
  twins 2 (physical) and twins 0 (UNPHYSICAL stress arm: other-table entries always fail).
- Raw per-RNTI output (196 MB) is not committed; `per_config.jsonl` holds one aggregate line per (config, arm).
- `truth_eliminated_by_probe` is measured on the engine's counters (truth KL failures added by feed_k on grants where the
  truth's full decode passes); 0 everywhere.

## Why P2 produces wrong winners in the blind arm [SIMULATED, DGX host, nr_td_sim @02524c44ff]

Every wrong P2 winner is a physical twin of the truth (same S, L, k0, DM-RS; other mcs_table): (truth, winner) table pairs
(0,1) 471, (1,2) 198, (0,2) 136, (2,1) 114, (1,0) 13. All wrong winners occur with twins 2 (twins 0: 0 wrong), 920/932 at
table_exercise 0.5, none at K 4. In the 932 wrong RNTIs the median share of the truth's KL trials that are admitted probe
FAILs is 0.92; in the 9 089 correct twins-2 oracle-0 RNTIs it is 0.06.
Mechanism: P2 counts probe FAILs and discards probe PASSes, so the engine's rate ok/trials for any hypothesis that sometimes
passes is deflated in proportion to how often it is probed. Probe allocation depends on the search state (the probes are the
next entries at the round-robin cursor; during the "hot" exploit the main decode does not advance the cursor, so the same
hypothesis is probed on consecutive grants: nr_pdsch_config_sweep.c:564-573 (hot exploit returns without moving the cursor), 669-680 (probes read from the cursor)). When the truth is probed far more
than its twin, the twin's undeflated rate leads and passes the separation test or the 300-trial fallback. The probe
outcome is correct on every grant (truth_eliminated_by_probe = 0); the bias is in the estimator, which no longer sees a
Bernoulli sample. With oracle 1 the Qm oracle removes the twins after two sightings, so no twin is left to win.
