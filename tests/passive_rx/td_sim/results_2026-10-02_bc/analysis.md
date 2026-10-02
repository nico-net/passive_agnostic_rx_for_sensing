# BC6 (reduced) blind-convergence gate: analysis

All numbers `[SIMULATED, DGX host, nr_td_sim @c826d0e4ae]` unless marked BC6b re-run, in which case `[SIMULATED, DGX host, nr_td_sim @2588a83cc9]`, seed 1, acq 500 x 4 RNTIs per cell (2000 RNTIs), cell SA (sib1 1). Full tables: summary.md. Model: v2 slot model, physical k0 trap, persist 0.5, dci-miss 0.01, dci-false 1e-3, other-UE occupancy 0.1, obs-lastset 1, confirmed exclusions, K39 fixed (`--k0-oracle-legacy 0`).
Monte-Carlo resolution: 3/N, i.e. 1.5e-3 per RNTI at N = 2000, and 3/(accepts) for lever events. "0 wrong" proves rate < ~1.5e-3 only; the 1e-6 claim is analytical.

## BC6b re-run (what changed, which rows are replaced)

BC6 was invalidated in part by three defects found in task BC6 investigation (task-BC6-investigation): (1) a field-book hole, the TDRA mask pinned k0 (engine); (2) a simulator artefact, TDD exclusions not merged per row (oracle 1 + DDDSU); (3) a simulator accounting artefact, a pin counted from an exclusion compaction. Fixed at `2588a83cc9` (BC6b). Re-run with that binary, `--acq 500`, same seed and flags:
- all 10 oracle-1 DDDSU cells (prior, cp, fb2, fb2_inject, fb2_cp x 4/1 RX; the brief said 16, the matrix has 10 such rows, all re-run);
- every fb2 / fb2_inject / fb2_cp cell (24, field book changed);
- every supplementary cell (10: cp_nog, fb2_cp_nog, C_stress_nog, P_stress_nog; pin counting, and the field book for fb2_cp_nog).
Rows not re-run are unaffected by construction and are marked "kept" in summary.md: prior and cp at TDD none or oracle 0 (no field book; the exclusion merge only acts with TDD and oracle 1; levers inert under the guard), and the C_stress / P_stress arms. Every row of the re-run set replaced the old row (rows.json, summary.md, supp/). The BC6 numbers for the replaced rows are superseded and must not be quoted (git history keeps them).

## Prominent findings (hard rule)

1. HARD RULE MET: wrong = 0 and wrong_pins = 0 in all 52 cells (42 main + 10 supplementary), including the fb2_inject arm and the guard-off arms. No violation.
2. fb2_inject (forced promotion of a TDRA with the truth's S/L/mapping and another k0): wrong 840-1980 of 2000 before, 0 of 2000 in all 8 cells now. recovery_never = 0 of 500 acquisitions in every cell (was 404-497), mean recovery exactly 2.00 RNTIs, fail_opens 0 (the truth is never dormant, so nothing needs reopening). Criterion "recovery within 2 RNTIs": PASS. Caveat: only the k0-only injection (inject 3) is in the matrix; the S/L/mapping and DM-RS injections are covered by unit tests (TdSim.WrongPromotionRecovers) and were not re-run here. The fb2_inject cold times (oracle 0: 9.9 s median at 4 RX) are an artefact of the injection (the injected value already carries the truth's S/L/mapping and prunes correctly); do not quote them as a speed result.
3. cp_nog DDDSU wrong_pins: 1 and 1 before, 0 and 0 now (anomaly 3 was a bookkeeping artefact: the pin at acq 191 kept the truth's geometry; the later exclusion compaction was counted as a second pin). fb2_cp_nog 0 in all cells. geom_pins in the guard-off cells: 2000 / 2000 / 1058 / 1060 (cp_nog) and 1998 / 1999 / 1048 / 1059 (fb2_cp_nog), against measured wrong 0 and an analytical bound 1.8e-10 to 3.7e-10.
4. Oracle 1 + DDDSU: undecidable 1072 of 2000 before, 0 of 2000 in all 10 cells now. The earlier 1072 (every k0 = 1 truth) was the simulator artefact of anomaly 1, not a property of the oracle model and not runtime-reproducible (runtime merges the exclusion per row). Oracle-1 DDDSU cold median: 63.2 s (4 RX), 90.1 s (1 RX); steady prior 19.2 / 29.0 s versus fb2 1.01 / 1.38 s.
5. Main matrix (guard default): wrong = 0 and wrong_pins = 0 everywhere.

## Levers C and P are inert under the default guard

In cp and fb2_cp (16 cells), crc_accepts = 0 and geom_pins = 0 everywhere. sib_blocks = 934 to 2000 at TDD none (every RNTI blocked at oracle 0). The persistent-traffic k0 sibling test blocks both levers, so there is no speed or risk effect: cp differs from prior only through fewer sibling trials (blind cold median 301.1 vs 301.3 s at 4 RX). Certified share of passes: 0.13 to 0.38 at TDD none, 0.008 to 0.018 (oracle 1, BC6b re-run; BC6 showed 0.002 under the artefact) and 0.08 to 0.13 (oracle 0) at DDDSU. BC9c (count sibling-test trials only on certified grants) is the open lever.
Consequence: the stress arms (oracle 0, crc-false 1e-3) as specified are vacuous: measured 0, bound 0 (no accepts). Supplementary stress arms with the guard off (acq 500, 4 RX, TDD none): C_stress_nog crc_accepts 1981, crc_wrong 0, bound 0.064; P_stress_nog geom_pins 1988, wrong_pins 0, bound 0.096. Measured 0 <= bound.

## Headline: oracle 0 (blind) cold median s, 4 RX

| TDD | prior | fb2 | cp | fb2_cp | cp_nog (supp.) | fb2_cp_nog (supp.) |
|---|---|---|---|---|---|---|
| none | 301.3 | 301.3 | 301.1 | 301.1 | 29.1 | 29.1 |
| DDDSU | 179.4 | 179.4 | 176.2 | 176.2 | 69.4 | 69.4 |

1 RX: none 434.7 / 434.7 / 427.8 / 427.8 (supp. 40.5); DDDSU 256.9 / 256.9 / 249.0 / 249.0 (supp. 99.5).
Steady (later RNTIs) mean/median, oracle 0, 4 RX, none: prior 21.8/16.1; fb2 1.06/0.92 (BC6: 0.32/0.33); cp 19.6/14.3; fb2_cp 1.20/1.07 s. DDDSU: prior 13.3/10.4; fb2 0.68/0.53; fb2_cp 0.80/0.60 s. The steady cost of the BC6b fix (about 0.3 -> 0.9 s) is the price of keeping the k0 layers active; it is still 15-20x faster than prior-only steady.
Targets (cold median <= 30 s at 4 RX, <= 90 s at 1 RX) are missed in the operator matrix for every arm (gap 270 s at 4 RX none). They are met only by the supplementary guard-off arms at TDD none (29.1 s, 40.5 s), not at DDDSU (69.4 s ok at 4 RX, 99.5 s misses at 1 RX).

## Oracle 1 (today's runtime, K39 fixed)

TDD none, 4 RX: cold identical to prior in every arm (159.3 mean / 95.8 median; an arm differs only after RNTI 2); steady prior 121.6/54.55 s versus fb2 6.52/2.77 s (BC6: 0.35; the k0 layers now stay active), 1 RX 174.8/76.4 versus 9.56/3.84. TDD DDDSU (BC6b re-run): undecidable 0 in all 10 cells; 4 RX cold 85.3 mean / 63.2 median, steady prior 42.2/19.2 versus fb2 2.53/1.01 and fb2_cp 2.52/1.02; 1 RX cold 118.7/90.1, steady prior 61.0/29.0 versus fb2 3.15/1.38. The BC6 text "1072 of 2000 undecidable ... a baseline property of the oracle model" is WITHDRAWN: it was the simulator artefact of anomaly 1.

## TDD exclusions

tdd_excl_removed (cumulative hypothesis-grants): oracle 1 DDDSU 5.2e4 to 1.2e5 (BC6: 1.2e8 to 1.8e8, the wipe/re-add storm of anomaly 1, now gone); oracle 0 DDDSU 2.1e5 (prior) / 3.9e5 (fb2). truth_excluded = 0 everywhere (no hard-excluded truth).

## Per-arm criteria summary (non-injected, after BC6b)

| arm | wrong | wrong_pins | undecidable vs prior | oracle-1 time vs prior | recovery (k0-only inject) | cold target |
|---|---|---|---|---|---|---|
| prior | 0 | 0 | baseline (0 everywhere) | baseline | n/a | miss |
| fb2 | 0 | 0 | equal (0) | cold equal, steady 2.8 vs 54.6 s (none), 1.0 vs 19.2 s (DDDSU) at 4 RX | PASS: 0/500 never, 2.00 RNTIs, wrong 0 | miss |
| cp | 0 | 0 | equal (0) | equal (guard blocks, levers nearly inert) | not run | miss |
| fb2_cp | 0 | 0 | equal (0) | cold equal, steady better | PASS (inject arm run for fb2 only; fb2_cp has the same field book) | miss |

Limits: seed 1, 2000 RNTIs per cell, resolution about 3/N = 1.5e-3; the 1e-6 claim stays analytical. Simulator only, DGX host; nothing here is an OTA or runtime measurement.
