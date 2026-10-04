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

## Wrong P2 winners: identity and truth admitted-fail share (oracle 0, decided RNTIs; correlational)

| (truth table, winner table) | wrong P2 RNTIs |
|---|---|
| (0, 1) | 471 |
| (0, 2) | 136 |
| (1, 0) | 13 |
| (1, 2) | 198 |
| (2, 1) | 114 |

Median share of the truth's KL trials that are admitted probe FAILs (twins 2): wrong RNTIs 0.92 (n 932), correct RNTIs 0.06 (n 9089).
