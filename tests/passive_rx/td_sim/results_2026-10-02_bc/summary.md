[SIMULATED, DGX host, nr_td_sim @c826d0e4ae for rows marked "kept"; nr_td_sim @2588a83cc9 (BC6b fixes) for rows marked RERUN] Seed 1, acq 500 x 4 RNTIs, v2 slot model (persist 0.5, dci-miss 0.01, dci-false 1e-3, other-UE occ 0.1, obs-lastset 1, confirmed exclusions, K39 fixed, table-exercise 0.9, twins 2, gate 1, K 1, crc-false 5.96e-8, SA sib1 1). Cold = first two RNTIs of an acquisition, steady = later; seconds; undecidable RNTIs censored. The cold/steady columns of an arm with undecidable>0 describe survivors only. tdd_excl_removed counts hypothesis-grants removed by the TDD exclusion (cumulative, not a rate). Per-RNTI jsonl (66 MB) not committed; rows.json holds every cell.

## Main matrix (operator reduced scope)
| arm | tdd | rx | oracle | acq | cold mean/med s | steady mean/med s | wrong | wrong_pins | undec | crc_acc | geom_pins | sib_blocks | fail_opens | rec grants/RNTIs (never) | inj | cert share | tdd_excl_removed | crc_wrong/bound | geom_bound | certified_wrong | re-run (BC6b) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| prior | none | 4 | 1 | 500 | 159.3/95.8 | 121.61/54.55 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 0 | kept (BC6 @c826d0e4ae) |
| prior | none | 4 | 0 | 500 | 386.6/301.3 | 21.76/16.07 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 77 | kept (BC6 @c826d0e4ae) |
| prior | none | 1 | 1 | 500 | 229.1/138.7 | 174.82/76.41 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 0 | kept (BC6 @c826d0e4ae) |
| prior | none | 1 | 0 | 500 | 554.5/434.7 | 31.09/22.89 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 73 | kept (BC6 @c826d0e4ae) |
| prior | DDDSU | 4 | 1 | 500 | 85.3/63.2 | 42.22/19.23 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 51650 | 0/0 | 0 | 0 | RERUN: sim TDD-exclusion merge (anomaly 1) |
| prior | DDDSU | 4 | 0 | 500 | 257.8/179.4 | 13.25/10.36 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 209512 | 0/0 | 0 | 12 | kept (BC6 @c826d0e4ae) |
| prior | DDDSU | 1 | 1 | 500 | 118.7/90.1 | 61.03/29.01 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 56479 | 0/0 | 0 | 1 | RERUN: sim TDD-exclusion merge (anomaly 1) |
| prior | DDDSU | 1 | 0 | 500 | 372.4/256.9 | 19.23/14.48 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 209512 | 0/0 | 0 | 16 | kept (BC6 @c826d0e4ae) |
| fb2 | none | 4 | 1 | 500 | 159.3/95.8 | 6.52/2.77 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 0 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2 | none | 4 | 0 | 500 | 386.6/301.3 | 1.06/0.92 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 75 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2 | none | 1 | 1 | 500 | 229.1/138.7 | 9.56/3.84 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 0 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2 | none | 1 | 0 | 500 | 554.5/434.7 | 1.52/1.33 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 0 | 0/0 | 0 | 71 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2 | DDDSU | 4 | 1 | 500 | 85.3/63.2 | 2.53/1.01 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 94896 | 0/0 | 0 | 1 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2 | DDDSU | 4 | 0 | 500 | 257.8/179.4 | 0.68/0.53 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 393144 | 0/0 | 0 | 12 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2 | DDDSU | 1 | 1 | 500 | 118.7/90.1 | 3.15/1.38 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 100648 | 0/0 | 0 | 2 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2 | DDDSU | 1 | 0 | 500 | 372.4/256.9 | 0.98/0.72 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | 0 | - | 393144 | 0/0 | 0 | 16 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | none | 4 | 1 | 500 | 100.3/74.9 | 6.68/2.67 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 40108/2.00 (0) | 500 | - | 0 | 0/0 | 0 | 0 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | none | 4 | 0 | 500 | 13.1/9.9 | 1.06/0.92 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 5230/2.00 (0) | 500 | - | 0 | 0/0 | 0 | 3 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | none | 1 | 1 | 500 | 149.8/109.1 | 9.27/3.67 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 59937/2.00 (0) | 500 | - | 0 | 0/0 | 0 | 0 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | none | 1 | 0 | 500 | 18.7/14.2 | 1.52/1.33 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 7471/2.00 (0) | 500 | - | 0 | 0/0 | 0 | 4 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | DDDSU | 4 | 1 | 500 | 62.9/50.0 | 2.31/1.04 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 25169/2.00 (0) | 500 | - | 110553 | 0/0 | 0 | 1 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | DDDSU | 4 | 0 | 500 | 9.4/6.1 | 0.68/0.53 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3762/2.00 (0) | 500 | - | 393144 | 0/0 | 0 | 2 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | DDDSU | 1 | 1 | 500 | 90.1/74.0 | 3.26/1.47 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 36039/2.00 (0) | 500 | - | 117421 | 0/0 | 0 | 2 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_inject | DDDSU | 1 | 0 | 500 | 13.2/8.7 | 0.98/0.72 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 5296/2.00 (0) | 500 | - | 393144 | 0/0 | 0 | 2 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| cp | none | 4 | 1 | 500 | 159.4/95.8 | 121.60/54.55 | 0 | 0 | 0 | 0 | 0 | 934 | 0 | - | 0 | 0.127 | 0 | 0/0 | 0 | 0 | kept (BC6 @c826d0e4ae) |
| cp | none | 4 | 0 | 500 | 358.5/301.1 | 19.59/14.30 | 0 | 0 | 0 | 0 | 0 | 2000 | 0 | - | 0 | 0.379 | 0 | 0/0 | 0 | 70 | kept (BC6 @c826d0e4ae) |
| cp | none | 1 | 1 | 500 | 229.9/138.7 | 174.70/76.41 | 0 | 0 | 0 | 0 | 0 | 937 | 0 | - | 0 | 0.128 | 0 | 0/0 | 0 | 0 | kept (BC6 @c826d0e4ae) |
| cp | none | 1 | 0 | 500 | 520.3/427.8 | 27.87/20.70 | 0 | 0 | 0 | 0 | 0 | 2000 | 0 | - | 0 | 0.375 | 0 | 0/0 | 0 | 69 | kept (BC6 @c826d0e4ae) |
| cp | DDDSU | 4 | 1 | 500 | 85.2/62.8 | 42.18/19.18 | 0 | 0 | 0 | 0 | 0 | 51 | 0 | - | 0 | 0.00788 | 51563 | 0/0 | 0 | 0 | RERUN: sim TDD-exclusion merge (anomaly 1) |
| cp | DDDSU | 4 | 0 | 500 | 246.8/176.2 | 12.14/9.68 | 0 | 0 | 0 | 0 | 0 | 1029 | 0 | - | 0 | 0.117 | 209512 | 0/0 | 0 | 15 | kept (BC6 @c826d0e4ae) |
| cp | DDDSU | 1 | 1 | 500 | 118.3/89.8 | 60.97/28.96 | 0 | 0 | 0 | 0 | 0 | 62 | 0 | - | 0 | 0.00795 | 56430 | 0/0 | 0 | 1 | RERUN: sim TDD-exclusion merge (anomaly 1) |
| cp | DDDSU | 1 | 0 | 500 | 354.2/249.0 | 17.55/13.07 | 0 | 0 | 0 | 0 | 0 | 1046 | 0 | - | 0 | 0.118 | 209512 | 0/0 | 0 | 17 | kept (BC6 @c826d0e4ae) |
| fb2_cp | none | 4 | 1 | 500 | 159.4/95.8 | 6.49/2.80 | 0 | 0 | 0 | 0 | 0 | 997 | 0 | - | 0 | 0.114 | 0 | 0/0 | 0 | 0 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_cp | none | 4 | 0 | 500 | 358.5/301.1 | 1.20/1.07 | 0 | 0 | 0 | 0 | 0 | 1975 | 0 | - | 0 | 0.373 | 0 | 0/0 | 0 | 68 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_cp | none | 1 | 1 | 500 | 229.9/138.7 | 9.54/3.65 | 0 | 0 | 0 | 0 | 0 | 998 | 0 | - | 0 | 0.114 | 0 | 0/0 | 0 | 0 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_cp | none | 1 | 0 | 500 | 520.3/427.8 | 1.72/1.52 | 0 | 0 | 0 | 0 | 0 | 1979 | 0 | - | 0 | 0.37 | 0 | 0/0 | 0 | 67 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_cp | DDDSU | 4 | 1 | 500 | 85.2/62.8 | 2.52/1.02 | 0 | 0 | 0 | 0 | 0 | 228 | 0 | - | 0 | 0.0181 | 95646 | 0/0 | 0 | 1 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_cp | DDDSU | 4 | 0 | 500 | 246.8/176.2 | 0.80/0.60 | 0 | 0 | 0 | 0 | 0 | 956 | 0 | - | 0 | 0.128 | 393144 | 0/0 | 0 | 15 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_cp | DDDSU | 1 | 1 | 500 | 118.3/89.8 | 3.16/1.41 | 0 | 0 | 0 | 0 | 0 | 241 | 0 | - | 0 | 0.0183 | 102406 | 0/0 | 0 | 2 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| fb2_cp | DDDSU | 1 | 0 | 500 | 354.2/249.0 | 1.17/0.82 | 0 | 0 | 0 | 0 | 0 | 957 | 0 | - | 0 | 0.127 | 393144 | 0/0 | 0 | 16 | RERUN: field book k0 fix + sim exclusion merge + pin counting |
| C_stress | none | 4 | 0 | 500 | 408.8/334.8 | 20.11/14.93 | 0 | 0 | 0 | 0 | 0 | 1981 | 0 | - | 0 | 0.235 | 0 | 0/0 | 0 | 79 | kept (BC6 @c826d0e4ae) |
| P_stress | none | 4 | 0 | 500 | 367.0/301.4 | 20.11/14.93 | 0 | 0 | 0 | 0 | 0 | 1988 | 0 | - | 0 | 0.239 | 0 | 0/0 | 0 | 71 | kept (BC6 @c826d0e4ae) |

## Supplementary (NOT in the operator matrix): oracle 0, k0-sibling guard disabled (--sib-pmin 0), acq 500 [RE-RUN at nr_td_sim @2588a83cc9, all 10 cells]
| arm | tdd | rx | oracle | acq | cold mean/med s | steady mean/med s | wrong | wrong_pins | undec | crc_acc | geom_pins | sib_blocks | fail_opens | rec grants/RNTIs (never) | inj | cert share | tdd_excl_removed | crc_wrong/bound | geom_bound | certified_wrong | re-run (BC6b) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cp_nog | none | 4 | 0 | 500 | 35.7/29.1 | 2.59/2.02 | 0 | 0 | 0 | 1603 | 2000 | 0 | 0 | - | 0 | 0.395 | 0 | 0/8.93e-11 | 3.71e-10 | 6 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| cp_nog | none | 1 | 0 | 500 | 49.2/40.5 | 3.80/3.01 | 0 | 0 | 0 | 1567 | 2000 | 0 | 0 | - | 0 | 0.389 | 0 | 0/8.54e-11 | 3.39e-10 | 5 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| cp_nog | DDDSU | 4 | 0 | 500 | 109.7/69.4 | 6.13/4.25 | 0 | 0 | 0 | 560 | 1058 | 0 | 0 | - | 0 | 0.0757 | 209512 | 0/3.91e-11 | 1.87e-10 | 4 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| cp_nog | DDDSU | 1 | 0 | 500 | 157.7/99.5 | 8.60/5.95 | 0 | 0 | 0 | 546 | 1060 | 0 | 0 | - | 0 | 0.0765 | 209512 | 0/4.14e-11 | 1.98e-10 | 6 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| fb2_cp_nog | none | 4 | 0 | 500 | 35.7/29.1 | 0.37/0.36 | 0 | 0 | 0 | 1496 | 1998 | 0 | 0 | - | 0 | 0.385 | 0 | 0/8.93e-11 | 3.64e-10 | 6 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| fb2_cp_nog | none | 1 | 0 | 500 | 49.2/40.5 | 0.53/0.53 | 0 | 0 | 0 | 1450 | 1999 | 0 | 0 | - | 0 | 0.386 | 0 | 0/8.54e-11 | 3.32e-10 | 5 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| fb2_cp_nog | DDDSU | 4 | 0 | 500 | 109.7/69.4 | 0.43/0.36 | 0 | 0 | 0 | 575 | 1048 | 0 | 0 | - | 0 | 0.0918 | 393144 | 0/3.91e-11 | 1.8e-10 | 2 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| fb2_cp_nog | DDDSU | 1 | 0 | 500 | 157.7/99.5 | 0.62/0.56 | 0 | 0 | 0 | 576 | 1059 | 0 | 0 | - | 0 | 0.0925 | 393144 | 0/4.14e-11 | 1.94e-10 | 5 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| C_stress_nog | none | 4 | 0 | 500 | 45.7/31.5 | 2.59/1.98 | 0 | 0 | 0 | 1981 | 0 | 0 | 0 | - | 0 | 0.248 | 0 | 0/0.0644 | 0 | 7 | RERUN: pin counting (anomaly 3) + field book k0 fix |
| P_stress_nog | none | 4 | 0 | 500 | 41.4/30.7 | 2.64/2.02 | 0 | 0 | 0 | 0 | 1988 | 0 | 0 | - | 0 | 0.354 | 0 | 0/0 | 0.0955 | 8 | RERUN: pin counting (anomaly 3) + field book k0 fix |
