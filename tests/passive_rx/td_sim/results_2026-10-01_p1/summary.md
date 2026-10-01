# P1 ablation, Technique D levers  [SIMULATED, DGX host, nr_td_sim @00dd79eed4, frozen binary]

Matrix: ablation_p1.json = 6 arms x cells SA (sib1 1) / NSA-like (sib1 0) x rx 4/1 x oracle 1/0 x 2000 acquisitions x 4 RNTIs, seed 1, twins 2, grant cap 3600 s/RNTI.
Wall time: ~25 min for the 48 runs at 8 parallel (pilot --acq 50, oracle 0, 1 RX: 11-14 s => ~8-10 min per 2000-acq run; oracle 1 ~0.01 s per 50 acq).
Raw per-RNTI results.jsonl (110 MB, not committed) kept outside the repo. Confirmation runs (arm gate_K3_order seed 1; baseline / +gate / gate_K3_order seed 2): summary_confirm_seed{1,2}.md, confirm_seed{1,2}.json.
oracle 1 = today's runtime (DM-RS/Qm oracles); oracle 0 = blind / oracle failure. cold = RNTI 1-2, steady = RNTI 3-4. Seconds = grants / 200 per s (assumption).

## Findings (all [SIMULATED])
1. wrong = 0 and undecidable = 0 in every row of every run (ablation + both confirmations; 0 and 0 summed over all rows). No undecidable increase over baseline.
2. +gate is the only lever with an effect: 1 RX oracle 0 cold mean 756.8 -> 603.1 s (-20 %), cold median 716.6 -> 512.6 s, steady median 25.6 -> 15.4 s; oracle 1 1 RX 1.77 -> 1.55 s cold (-12 %); 4 RX unchanged (422.4 -> 422.2 s) because rank-2 grants are decodable there. Cold p95 at 1 RX oracle 0 rises 1125 -> 1203 s (censoring-free; the tail is slightly longer even though mean/median drop).
3. +topK (K 3, w_probe 1) and +order (w_sib1/w_default/w_obs 1) give no gain in this model: cold mean 4 RX oracle 0 424.4 / 426.7 vs 422.4 s baseline (+0.5 / +1 %, inside the ~1 % seed noise: baseline seed 1 vs 2 cold mean 422.4 vs 426.9 at 4 RX, 756.8 vs 758.1 at 1 RX). +topK spends ~700 M probes (4 RX oracle 0) for nothing. SA and NSA-like are identical for arms without w_sib1 because the simulator's SIB1 only feeds the ordering term.
4. +fieldbook is WORSE: the simulator forces the runtime prior off with the field book (plan R2), the field book here only reorders, so steady RNTIs lose the prior: steady median at 4 RX oracle 0 356.6 s vs 10.6 s baseline (steady mean 425.8 vs 15.6 s), n_full 2x; 1 RX 716.6 vs 25.6 s. all_P1 inherits this (steady 358.6 s). The fieldbook lever must not replace the pruning prior until it also prunes (not modelled). Against the baseline with the prior on, the fieldbook arm is a regression.
5. Known simulator gap: probation withdrawal is not modelled, so a lever that produced a wrong cell prior would show as undecidable RNTIs; none did (undecidable 0 everywhere). A real wrong-prior risk is therefore NOT exercised here.

## Targets (spec 1: cold <= 30 s median, steady <= 2 s at 4 RX; 1 RX <= 3x = 90 s / 6 s) [SIMULATED]
| oracle | config | 4 RX cold / steady median | 1 RX cold / steady median | verdict |
|---|---|---|---|---|
| 1 (today's runtime) | baseline and chosen | 1.0 / 1.0 s | 1.6 / 1.6 s (chosen 1.4 / 1.4) | PASS (already met by baseline) |
| 0 (blind) | baseline | 356.6 / 10.6 s | 716.6 / 25.6 s | FAIL cold (12x / 8x over), FAIL steady (5x) |
| 0 (blind) | all_P1 | 358.6 / 358.6 s | 512.3 / 512.4 s | FAIL (steady regressed) |
| 0 (blind) | chosen (gate 1 only) | 358.6 / 10.6 s | 512.6 / 15.4 s | FAIL cold (12x / 5.7x), FAIL steady |
Conclusion: P1 levers as modelled do not bring the blind arm anywhere near the targets; at oracle 1 the targets hold without any lever. The cold blind time is dominated by exhaustive per-hypothesis failure accumulation which none of the P1 levers (ordering, probes) shortens in this model; needs P2 (failure-only probe evidence, Task 7) or stronger pruning.

## Chosen P1 defaults
ISAC_TD_GATE=1 (margin 6 dB), ISAC_TD_K=1, ISAC_TD_W_SIB1/DEFAULT/OBS/FIELD/PROBE=0, ISAC_TD_FIELDBOOK=0 (today's g_prior pruning kept), ISAC_TD_P2=0.
Justification: minimises cold time at oracle 0 (ties with all other no-fieldbook arms at 4 RX, -20 % mean at 1 RX), 0 wrong, 0 undecidable, oracle 1 not slower (4 RX 1.09 vs 1.08 s, 1 RX faster). Noise judged by seed 2 on baseline: cold mean differs 1.1 % (4 RX oracle 0), 0.2 % (1 RX oracle 0), 0 % oracle 1; the gate effect (-20 % at 1 RX, reproduced seed 2: 758.1 -> 602.6 s) is 20x the noise; K3+order on top of the gate (arm gate_K3_order, seeds 1 and 2: 4 RX oracle 0 cold mean 421.3-425.9 s vs gate 422.2-423.7 s; 1 RX 601.6-608.1 vs 602.6-603.1 s) is within noise, so K=3/weights stay neutral (they cost ~700 M probes). Spec 8's "K start 3" is not supported by the simulation.

## Full ablation table (seed 1, 2000 acq x 4 RNTIs)
[SIMULATED, nr_td_sim] cold = first two RNTIs per acquisition; steady = later RNTIs; seconds = grants / grants-per-s.
Undecidable (capped) RNTIs are censored: excluded from medians/means/p95, counted in the undecidable column.
Medians are quantised (separation is checked every 16 trials): prefer the mean columns.
tbl N = truth mcs_table N as count/median s/mean s/wrong.


| arm | cell | rx | oracle | cold median s | cold p95 s | steady median s | cold mean s | steady p95 s | steady mean s | mean s | mean grants | wrong | undecidable | n_full | n_probe | gated | tbl 0 | tbl 1 | tbl 2 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline | SA | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.06 | 1.07 | 215 | 0 | 0 | 1717916 | 0 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| baseline | SA | 4 | 0 | 356.6 | 836.6 | 10.6 | 422.38 | 35.8 | 15.58 | 218.98 | 43796 | 0 | 0 | 350370521 | 0 | 0 | 2656/102.0/191.0/0 | 2584/204.8/378.0/0 | 2760/63.6/97.1/0 |
| baseline | SA | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.77 | 2.8 | 1.72 | 1.74 | 349 | 0 | 0 | 2791634 | 0 | 0 | 2656/1.6/1.6/0 | 2584/2.5/2.5/0 | 2760/1.2/1.2/0 |
| baseline | SA | 1 | 0 | 716.6 | 1125.3 | 25.6 | 756.84 | 58.1 | 28.05 | 392.45 | 78489 | 0 | 0 | 627912024 | 0 | 0 | 2656/201.3/369.2/0 | 2584/398.5/568.2/0 | 2760/150.2/250.3/0 |
| baseline | NSA-like | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.06 | 1.07 | 215 | 0 | 0 | 1717916 | 0 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| baseline | NSA-like | 4 | 0 | 356.6 | 836.6 | 10.6 | 422.38 | 35.8 | 15.58 | 218.98 | 43796 | 0 | 0 | 350370521 | 0 | 0 | 2656/102.0/191.0/0 | 2584/204.8/378.0/0 | 2760/63.6/97.1/0 |
| baseline | NSA-like | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.77 | 2.8 | 1.72 | 1.74 | 349 | 0 | 0 | 2791634 | 0 | 0 | 2656/1.6/1.6/0 | 2584/2.5/2.5/0 | 2760/1.2/1.2/0 |
| baseline | NSA-like | 1 | 0 | 716.6 | 1125.3 | 25.6 | 756.84 | 58.1 | 28.05 | 392.45 | 78489 | 0 | 0 | 627912024 | 0 | 0 | 2656/201.3/369.2/0 | 2584/398.5/568.2/0 | 2760/150.2/250.3/0 |
| +gate | SA | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.06 | 1.07 | 214 | 0 | 0 | 1704843 | 0 | 8477 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +gate | SA | 4 | 0 | 358.6 | 841.3 | 10.6 | 422.21 | 36.1 | 15.54 | 218.87 | 43775 | 0 | 0 | 348286017 | 0 | 1913165 | 2656/102.2/191.2/0 | 2584/232.6/378.5/0 | 2760/52.6/96.1/0 |
| +gate | SA | 1 | 1 | 1.4 | 2.6 | 1.4 | 1.55 | 2.6 | 1.51 | 1.53 | 306 | 0 | 0 | 1704112 | 0 | 742753 | 2656/1.4/1.4/0 | 2584/2.3/2.3/0 | 2760/0.9/0.9/0 |
| +gate | SA | 1 | 0 | 512.6 | 1203.4 | 15.4 | 603.12 | 52.0 | 22.28 | 312.70 | 62541 | 0 | 0 | 348309087 | 0 | 152017361 | 2656/148.2/274.0/0 | 2584/279.8/540.7/0 | 2760/74.3/136.5/0 |
| +gate | NSA-like | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.06 | 1.07 | 214 | 0 | 0 | 1704843 | 0 | 8477 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +gate | NSA-like | 4 | 0 | 358.6 | 841.3 | 10.6 | 422.21 | 36.1 | 15.54 | 218.87 | 43775 | 0 | 0 | 348286017 | 0 | 1913165 | 2656/102.2/191.2/0 | 2584/232.6/378.5/0 | 2760/52.6/96.1/0 |
| +gate | NSA-like | 1 | 1 | 1.4 | 2.6 | 1.4 | 1.55 | 2.6 | 1.51 | 1.53 | 306 | 0 | 0 | 1704112 | 0 | 742753 | 2656/1.4/1.4/0 | 2584/2.3/2.3/0 | 2760/0.9/0.9/0 |
| +gate | NSA-like | 1 | 0 | 512.6 | 1203.4 | 15.4 | 603.12 | 52.0 | 22.28 | 312.70 | 62541 | 0 | 0 | 348309087 | 0 | 152017361 | 2656/148.2/274.0/0 | 2584/279.8/540.7/0 | 2760/74.3/136.5/0 |
| +topK | SA | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.09 | 1.8 | 1.06 | 1.08 | 215 | 0 | 0 | 1721861 | 3397489 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +topK | SA | 4 | 0 | 356.6 | 836.6 | 10.6 | 424.42 | 35.8 | 15.56 | 219.99 | 43997 | 0 | 0 | 351979449 | 703958898 | 0 | 2656/118.5/192.0/0 | 2584/232.2/379.9/0 | 2760/63.6/97.3/0 |
| +topK | SA | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.76 | 2.8 | 1.73 | 1.75 | 349 | 0 | 0 | 2792312 | 5518725 | 0 | 2656/1.6/1.6/0 | 2584/2.5/2.5/0 | 2760/1.2/1.2/0 |
| +topK | SA | 1 | 0 | 716.6 | 1125.3 | 25.6 | 754.30 | 58.1 | 28.02 | 391.16 | 78232 | 0 | 0 | 625857566 | 1251715132 | 0 | 2656/232.0/368.7/0 | 2584/397.9/568.2/0 | 2760/151.1/247.0/0 |
| +topK | NSA-like | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.09 | 1.8 | 1.06 | 1.08 | 215 | 0 | 0 | 1721861 | 3397489 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +topK | NSA-like | 4 | 0 | 356.6 | 836.6 | 10.6 | 424.42 | 35.8 | 15.56 | 219.99 | 43997 | 0 | 0 | 351979449 | 703958898 | 0 | 2656/118.5/192.0/0 | 2584/232.2/379.9/0 | 2760/63.6/97.3/0 |
| +topK | NSA-like | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.76 | 2.8 | 1.73 | 1.75 | 349 | 0 | 0 | 2792312 | 5518725 | 0 | 2656/1.6/1.6/0 | 2584/2.5/2.5/0 | 2760/1.2/1.2/0 |
| +topK | NSA-like | 1 | 0 | 716.6 | 1125.3 | 25.6 | 754.30 | 58.1 | 28.02 | 391.16 | 78232 | 0 | 0 | 625857566 | 1251715132 | 0 | 2656/232.0/368.7/0 | 2584/397.9/568.2/0 | 2760/151.1/247.0/0 |
| +order | SA | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.09 | 1.8 | 1.06 | 1.08 | 215 | 0 | 0 | 1720943 | 0 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +order | SA | 4 | 0 | 356.6 | 840.3 | 10.6 | 426.73 | 35.8 | 15.57 | 221.15 | 44230 | 0 | 0 | 353839667 | 0 | 0 | 2656/123.7/192.1/0 | 2584/232.0/382.1/0 | 2760/60.1/98.4/0 |
| +order | SA | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.76 | 2.8 | 1.73 | 1.75 | 349 | 0 | 0 | 2792578 | 0 | 0 | 2656/1.6/1.6/0 | 2584/2.5/2.5/0 | 2760/1.2/1.2/0 |
| +order | SA | 1 | 0 | 716.6 | 1125.3 | 25.6 | 759.20 | 58.1 | 28.01 | 393.60 | 78720 | 0 | 0 | 629762004 | 0 | 0 | 2656/232.0/371.7/0 | 2584/426.7/569.8/0 | 2760/134.3/249.8/0 |
| +order | NSA-like | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.06 | 1.07 | 215 | 0 | 0 | 1717844 | 0 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +order | NSA-like | 4 | 0 | 356.6 | 836.6 | 10.6 | 426.68 | 36.5 | 15.59 | 221.14 | 44227 | 0 | 0 | 353816719 | 0 | 0 | 2656/124.1/193.2/0 | 2584/233.9/380.4/0 | 2760/66.0/98.9/0 |
| +order | NSA-like | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.76 | 2.8 | 1.73 | 1.74 | 349 | 0 | 0 | 2791227 | 0 | 0 | 2656/1.6/1.6/0 | 2584/2.5/2.5/0 | 2760/1.2/1.2/0 |
| +order | NSA-like | 1 | 0 | 716.6 | 1125.3 | 25.6 | 761.30 | 58.1 | 28.00 | 394.65 | 78931 | 0 | 0 | 631444172 | 0 | 0 | 2656/232.1/372.1/0 | 2584/365.5/570.0/0 | 2760/154.9/252.2/0 |
| +fieldbook | SA | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.09 | 1.09 | 217 | 0 | 0 | 1738325 | 0 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +fieldbook | SA | 4 | 0 | 356.6 | 836.6 | 356.6 | 422.38 | 836.6 | 425.78 | 424.08 | 84816 | 0 | 0 | 678527298 | 0 | 0 | 2656/356.6/370.2/0 | 2584/716.6/733.5/0 | 2760/176.6/186.3/0 |
| +fieldbook | SA | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.77 | 2.9 | 1.77 | 1.77 | 353 | 0 | 0 | 2824204 | 0 | 0 | 2656/1.6/1.6/0 | 2584/2.6/2.5/0 | 2760/1.2/1.2/0 |
| +fieldbook | SA | 1 | 0 | 716.6 | 1125.3 | 716.6 | 756.84 | 1125.3 | 759.76 | 758.30 | 151660 | 0 | 0 | 1213278634 | 0 | 0 | 2656/716.6/715.7/0 | 2584/1125.3/1095.6/0 | 2760/476.6/483.5/0 |
| +fieldbook | NSA-like | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.09 | 1.09 | 217 | 0 | 0 | 1738325 | 0 | 0 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| +fieldbook | NSA-like | 4 | 0 | 356.6 | 836.6 | 356.6 | 422.38 | 836.6 | 425.78 | 424.08 | 84816 | 0 | 0 | 678527298 | 0 | 0 | 2656/356.6/370.2/0 | 2584/716.6/733.5/0 | 2760/176.6/186.3/0 |
| +fieldbook | NSA-like | 1 | 1 | 1.6 | 2.9 | 1.6 | 1.77 | 2.9 | 1.77 | 1.77 | 353 | 0 | 0 | 2824204 | 0 | 0 | 2656/1.6/1.6/0 | 2584/2.6/2.5/0 | 2760/1.2/1.2/0 |
| +fieldbook | NSA-like | 1 | 0 | 716.6 | 1125.3 | 716.6 | 756.84 | 1125.3 | 759.76 | 758.30 | 151660 | 0 | 0 | 1213278634 | 0 | 0 | 2656/716.6/715.7/0 | 2584/1125.3/1095.6/0 | 2760/476.6/483.5/0 |
| all_P1 | SA | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.09 | 1.8 | 1.09 | 1.09 | 217 | 0 | 0 | 1728874 | 3411322 | 8615 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| all_P1 | SA | 4 | 0 | 358.6 | 841.2 | 358.6 | 422.55 | 841.2 | 422.36 | 422.46 | 84491 | 0 | 0 | 672234920 | 1344469840 | 3694656 | 2656/358.6/371.7/0 | 2584/720.5/728.1/0 | 2760/177.6/185.1/0 |
| all_P1 | SA | 1 | 1 | 1.4 | 2.6 | 1.4 | 1.55 | 2.6 | 1.54 | 1.55 | 309 | 0 | 0 | 1722177 | 3398126 | 750656 | 2656/1.4/1.4/0 | 2584/2.3/2.3/0 | 2760/0.9/0.9/0 |
| all_P1 | SA | 1 | 0 | 512.3 | 1203.0 | 512.4 | 605.90 | 1203.3 | 604.42 | 605.16 | 121032 | 0 | 0 | 674058347 | 1348116694 | 294195719 | 2656/512.6/528.5/0 | 2584/1030.0/1047.8/0 | 2760/253.8/264.5/0 |
| all_P1 | NSA-like | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.8 | 1.09 | 1.08 | 217 | 0 | 0 | 1726128 | 3405908 | 8613 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| all_P1 | NSA-like | 4 | 0 | 358.5 | 841.3 | 358.6 | 421.32 | 841.2 | 421.87 | 421.59 | 84319 | 0 | 0 | 670862921 | 1341725842 | 3687222 | 2656/358.6/369.6/0 | 2584/720.5/727.7/0 | 2760/177.6/185.0/0 |
| all_P1 | NSA-like | 1 | 1 | 1.4 | 2.6 | 1.4 | 1.55 | 2.6 | 1.54 | 1.55 | 309 | 0 | 0 | 1722056 | 3397789 | 750477 | 2656/1.4/1.4/0 | 2584/2.3/2.3/0 | 2760/0.9/0.9/0 |
| all_P1 | NSA-like | 1 | 0 | 512.4 | 1202.7 | 512.4 | 601.60 | 1203.2 | 604.70 | 603.15 | 120630 | 0 | 0 | 671822785 | 1343645570 | 293221118 | 2656/512.6/526.8/0 | 2584/1030.0/1044.0/0 | 2760/253.8/263.8/0 |
