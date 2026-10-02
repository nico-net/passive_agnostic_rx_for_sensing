BC9d (hard TDD / DCI-adjacency exclusions only from CRC-confirmed DCIs), DGX, 2026-10-02. Code: td/convergence-levers 852b9bb86f.
Labels: [MEASURED, DGX rfsim 106 PRB 1 RX, host idle] gate_*; [MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX] rank4_pin49r4;
[SIMULATED, DGX host, nr_td_sim @852b9bb86f] sim_summary.md; [MEASURED, DGX unit test] red_test_nr_dci_history.txt.
gate_base_r1/r2 = tests/passive_rx/dgx/rfsim_regress.sh 2 (GATE_MODE=postconv default): PASS both. Precheck: 1-min load 0.51, no
  softmodem / nr_td_sim (gate_run.log). Receiver stopped by SIGINT (timeout -s INT). *_key_lines.txt = CONVERGED, last
  DCIADJ_CERT / DCIHIST / DCICONF census lines. No SIB1 on the phy-test bed: tdd_known=0, so no exclusion fires; the confirmation
  path itself runs (confirms 56-58k per run, missed_lookup 0, repeat 0).
rank4_pin49r4 = GNBCONF=gnb.sa.rfsim.100mhz.rank4.conf RXCONF=ue.passive.pin49r4.100mhz.conf CELL="-C 3750000000 -r 273 --ssb 1478"
  GNBARGS="-m 25 -n 1 -M 273 -l 4" RXEXTRA="--ue-nb-ant-rx 4 --ue-nb-ant-tx 4", 150 s, precheck load 0.15: CRC 100 % (3007/3007), pinned.
sim_summary.md = tests/passive_rx/td_sim/bc9d_campaign.py --acq 300 (after the rfsim runs); also in baseline_bc0_2026-10-01.txt ## BC9d.
red_test_nr_dci_history.txt = the new tests against the pre-fix adjacency rule / stub confirmation (RED), before GREEN.
