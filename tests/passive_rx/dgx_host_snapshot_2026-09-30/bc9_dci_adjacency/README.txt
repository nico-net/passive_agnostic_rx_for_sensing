BC9 (DCI-adjacency certified k0 evidence + TDD/DCI per-hypothesis exclusion), DGX rfsim, 2026-10-02.
Code: td/convergence-levers after the BC9 commits (gate1 = commit 3273bb25f5; gate2, A/B and rank-4 = the fast-path /
kill-switch commit that adds this directory). Labels: [MEASURED, DGX rfsim 106 PRB 1 RX] for gate*/ab_*,
[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX] for rank4_pin49r4.
gate1_*, gate2_*: tests/passive_rx/dgx/rfsim_regress.sh 2 (default build, K39 fix on). gate1 = before the accept-hook
  fast path (rnti_constrained), gate2 = after.
ab_off_* / ab_on_*: same binary, interleaved 150 s arms (rfsim_arm.sh): off = ISAC_TD_DCI_ADJ=0 (no BC9 runtime path at
  all), on = default.
*.json = score_rx.py --json; *.ctx.json = per-context ttc (Technique D CONVERGED time minus first 1_1 rnti_seen),
  winner trials, final LDPCDIAG (ok, seg_fail, zero_tb), last BC9 census lines.
key_lines.txt = convergence / census / LDPCDIAG lines per arm.
No arm has a SIB1 (phy-test gNB: "MIB indicates no CORESET 0 / SIB1"), so tdd_known=0 and no TDD exclusion or
DCI-adjacency exclusion fired (adj_rows=0) -- see task-BC9-report.md.
