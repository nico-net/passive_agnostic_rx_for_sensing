[SIMULATED, nr_td_sim] cold = first two RNTIs per acquisition; steady = later RNTIs; seconds = grants / grants-per-s.
Undecidable (capped) RNTIs are censored: excluded from medians/means/p95, counted in the undecidable column.
Medians are quantised (separation is checked every 16 trials): prefer the mean columns.
tbl N = truth mcs_table N as count/median s/mean s/wrong.

| arm | cell | rx | oracle | cold median s | cold p95 s | steady median s | cold mean s | steady p95 s | steady mean s | mean s | mean grants | wrong | undecidable | n_full | n_probe | gated | tbl 0 | tbl 1 | tbl 2 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gate_K3_order | SA | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.09 | 1.8 | 1.06 | 1.07 | 215 | 0 | 0 | 1708112 | 3369852 | 8506 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| gate_K3_order | SA | 4 | 0 | 358.6 | 841.2 | 10.6 | 422.55 | 36.1 | 15.54 | 219.05 | 43809 | 0 | 0 | 348559421 | 697118842 | 1914561 | 2656/115.4/192.4/0 | 2584/233.3/377.3/0 | 2760/64.0/96.5/0 |
| gate_K3_order | SA | 1 | 1 | 1.4 | 2.6 | 1.4 | 1.55 | 2.6 | 1.51 | 1.53 | 306 | 0 | 0 | 1705486 | 3364644 | 743464 | 2656/1.4/1.4/0 | 2584/2.3/2.3/0 | 2760/0.9/0.9/0 |
| gate_K3_order | SA | 1 | 0 | 512.3 | 1203.0 | 15.4 | 605.90 | 51.8 | 22.15 | 314.03 | 62805 | 0 | 0 | 349782158 | 699564316 | 152661838 | 2656/167.4/273.4/0 | 2584/348.6/543.6/0 | 2760/80.6/138.2/0 |
| gate_K3_order | NSA-like | 4 | 1 | 1.0 | 1.8 | 1.0 | 1.08 | 1.7 | 1.06 | 1.07 | 214 | 0 | 0 | 1705751 | 3365280 | 8526 | 2656/1.0/1.0/0 | 2584/1.6/1.6/0 | 2760/0.6/0.7/0 |
| gate_K3_order | NSA-like | 4 | 0 | 358.5 | 841.3 | 10.6 | 421.32 | 36.1 | 15.59 | 218.46 | 43691 | 0 | 0 | 347622207 | 695244414 | 1909337 | 2656/102.1/190.8/0 | 2584/233.2/377.2/0 | 2760/63.9/96.4/0 |
| gate_K3_order | NSA-like | 1 | 1 | 1.4 | 2.6 | 1.4 | 1.55 | 2.6 | 1.51 | 1.53 | 306 | 0 | 0 | 1705454 | 3364437 | 743144 | 2656/1.4/1.4/0 | 2584/2.3/2.3/0 | 2760/0.9/0.9/0 |
| gate_K3_order | NSA-like | 1 | 0 | 512.4 | 1202.7 | 15.4 | 601.60 | 51.7 | 22.17 | 311.89 | 62377 | 0 | 0 | 347400131 | 694800262 | 151617559 | 2656/148.5/272.0/0 | 2584/291.7/538.7/0 | 2760/75.1/137.9/0 |
