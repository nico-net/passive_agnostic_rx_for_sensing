# BC6 (reduced) blind-convergence gate: analysis

All numbers `[SIMULATED, DGX host, nr_td_sim @c826d0e4ae]`, seed 1, acq 500 x 4 RNTIs per cell (2000 RNTIs), cell SA (sib1 1). Full tables: summary.md. Model: v2 slot model, physical k0 trap, persist 0.5, dci-miss 0.01, dci-false 1e-3, other-UE occupancy 0.1, obs-lastset 1, confirmed exclusions, K39 fixed (`--k0-oracle-legacy 0`).
Monte-Carlo resolution: 3/N, i.e. 1.5e-3 per RNTI at N = 2000, and 3/(accepts) for lever events. "0 wrong" proves rate < ~1.5e-3 only; the 1e-6 claim is analytical.

## Prominent findings (hard rule)

1. fb2_inject: wrong > 0 in all 8 cells (840 to 1980 of 2000 RNTIs; wrong_pins 0). This is the injected fault (forced promotion of a TDRA differing only in k0). The guard did not recover it within the acquisition: recovery_never = 404 to 497 of 500 acquisitions. Mean recovery over the few that did recover: 2.3 to 3.1 RNTIs. Fails the "recovery within 2 RNTIs" criterion. No non-injected arm has a wrong winner.
2. Supplementary guard-off arm cp_nog, TDD DDDSU: wrong_pins = 1 at 4 RX and 1 at 1 RX (seed 1; at 4 RX it is acquisition index 191, RNTI rank 2, truth k0 = 0, geom_pins 2, k0_trap_passes 6; the final winner was still correct, wrong = 0). Measured 1 versus analytical bound 1.9e-10: exceeds the bound. This arm disables the k0-sibling guard (`--sib-pmin 0`), outside the operator matrix. fb2_cp_nog (same lever settings plus fb2) had wrong_pins 0 in all cells. I did not analyse that arm further.
3. Main matrix (guard default): wrong = 0 and wrong_pins = 0 in all 32 non-injected cells and both stress arms.

## Levers C and P are inert under the default guard

In cp and fb2_cp (16 cells), crc_accepts = 0 and geom_pins = 0 everywhere. sib_blocks = 934 to 2000 at TDD none (every RNTI blocked at oracle 0). The persistent-traffic k0 sibling test blocks both levers, so there is no speed or risk effect: cp differs from prior only through fewer sibling trials (blind cold median 301.1 vs 301.3 s at 4 RX). Certified share of passes: 0.13 to 0.38 at TDD none, about 0.002 (oracle 1) and 0.08 to 0.12 (oracle 0) at DDDSU. BC9c (count sibling-test trials only on certified grants) is the open lever.
Consequence: the stress arms (oracle 0, crc-false 1e-3) as specified are vacuous: measured 0, bound 0 (no accepts). Supplementary stress arms with the guard off (acq 500, 4 RX, TDD none): C_stress_nog crc_accepts 1981, crc_wrong 0, bound 0.064; P_stress_nog geom_pins 1988, wrong_pins 0, bound 0.096. Measured 0 <= bound.

## Headline: oracle 0 (blind) cold median s, 4 RX

| TDD | prior | fb2 | cp | fb2_cp | cp_nog (supp.) | fb2_cp_nog (supp.) |
|---|---|---|---|---|---|---|
| none | 301.3 | 301.3 | 301.1 | 301.1 | 29.1 | 29.1 |
| DDDSU | 179.4 | 179.4 | 176.2 | 176.2 | 69.4 | 69.4 |

1 RX: none 434.7 / 434.7 / 427.8 / 427.8 (supp. 40.5); DDDSU 256.9 / 256.9 / 249.0 / 249.0 (supp. 99.5).
Steady (later RNTIs) mean/median, oracle 0, 4 RX, none: prior 21.8/16.1; fb2 0.32/0.33; cp 19.6/14.3; fb2_cp 0.51/0.44 s. DDDSU: prior 13.3/10.4; fb2 0.34/0.33; fb2_cp 0.45/0.34 s.
Targets (cold median <= 30 s at 4 RX, <= 90 s at 1 RX) are missed in the operator matrix for every arm (gap 270 s at 4 RX none). They are met only by the supplementary guard-off arms at TDD none (29.1 s, 40.5 s), not at DDDSU (69.4 s ok at 4 RX, 99.5 s misses at 1 RX).

## Oracle 1 (today's runtime, K39 fixed)

Cold identical to prior in every arm (a different arm differs only after RNTI 2): 4 RX none 159.3 mean / 95.8 median; fb2 steady 0.35 vs 121.6 s. At TDD DDDSU 1072 of 2000 RNTIs are undecidable in prior and all other arms (identical, so a baseline property of the oracle model under TDD, not caused by the candidates; cold/steady there describe survivors only, about 1.1 s).

## TDD exclusions

tdd_excl_removed (cumulative hypothesis-grants): oracle 1 DDDSU 1.2e8 to 1.8e8; oracle 0 DDDSU 2.1e5 (prior) / 3.9e5 (fb2). truth_excluded = 0 everywhere (no hard-excluded truth).

## Per-arm criteria summary (non-injected)

| arm | wrong | wrong_pins | undecidable vs prior | oracle-1 time vs prior | recovery | cold target |
|---|---|---|---|---|---|---|
| prior | 0 | 0 | baseline | baseline | n/a | miss |
| fb2 | 0 | 0 | equal | cold equal, steady far better | FAIL (inject) | miss |
| cp | 0 | 0 | equal | equal (guard blocks, no lever events) | not run | miss |
| fb2_cp | 0 | 0 | equal | cold equal, steady better | not run (not in matrix) | miss |
