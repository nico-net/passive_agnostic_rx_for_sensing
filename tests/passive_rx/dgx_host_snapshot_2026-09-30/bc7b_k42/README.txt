BC7b (K42: monotone observed last-symbol sets, no evidence wipe on restore round-trips), DGX rfsim, 2026-10-02.
Code: td/convergence-levers, engine commit 7b5c51b115 (fix) on top of 8af53c3a40 (pre). Receiver binary = default build, K39 fix on,
BC9 default (no SIB1 in the phy-test beds, so TDD / DCI-adjacency exclusions are inert).
Labels: [MEASURED, DGX rfsim 106 PRB 1 RX, host idle] for gate_* / ab_*; [MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX] for rank4_pin49r4.
Every arm: 150 s (rfsim_arm.sh), receiver stopped by SIGINT (timeout -s INT), precheck 1-min load < 2 and no nr_td_sim / softmodem
(gate_run.log, ab_run.log), except rank-4: precheck load 2.80 (started right after gate r5; noted in gate_run.log).

gate_r1, gate_r2  = tests/passive_rx/dgx/rfsim_regress.sh 2 (fix binary): FAIL both (CRC 94.63 / 95.88 < 98).
gate_r3..r5       = three more rfsim_arm.sh arms of the same binary (n = 5 for the comparison with the BC9 idle A/B).
ab_pre_r1..r4 / ab_fix_r1..r4 = interleaved A/B (pre, fix, pre, fix, ...): pre = nr-uesoftmodem built from 8af53c3a40's
                    nr_pdsch_config_sweep.c (the only runtime file BC7b changes), run from a symlink farm of the same build dir
                    (BUILD=/tmp/bc7b_k42/build_pre); fix = HEAD build.
rank4_pin49r4     = GNBCONF=gnb.sa.rfsim.100mhz.rank4.conf RXCONF=ue.passive.pin49r4.100mhz.conf CELL="-C 3750000000 -r 273 --ssb 1478"
                    GNBARGS="-m 25 -n 1 -M 273 -l 4" RXEXTRA="--ue-nb-ant-rx 4 --ue-nb-ant-tx 4", 150 s.
*.json = score_rx.py --json; *.ctx.json = ctx_extract.py (this directory; same definitions as the BC9 *.ctx.json: ttc per tda =
first "Technique D CONVERGED tda=N" minus first 1_1 rnti_seen; winner trials; final LDPCDIAG ok/seg_fail/zero_tb) plus K42
diagnostics: ORACLE_RESTORE line count and the last SWEEPSTAT stale / reindexed counters.
ab_stats.txt = exact Mann-Whitney / Fisher tests; tda0_catalogue_mode.txt = final tda0 context size (12 or 24 hypotheses) of every
BC9 idle arm (pre-fix code) and every BC7b arm; key_lines.txt = oracle / restore / convergence / LDPCDIAG / SWEEPSTAT lines of gate_*.
