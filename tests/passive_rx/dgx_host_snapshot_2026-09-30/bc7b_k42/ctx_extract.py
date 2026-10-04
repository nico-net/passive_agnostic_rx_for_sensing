#!/usr/bin/env python3
"""BC7b per-context extraction from an rfsim_arm.sh arm directory (rx/rx.log), same definitions as the BC9 *.ctx.json:
ttc per tda = first 'Technique D CONVERGED ... tda=N' time minus the first 1_1 rnti_seen time; winner trials of tda 0 from the
SWEEP CONVERGED line; final LDPCDIAG ok / seg_fail / zero_tb. K42 diagnostics: ORACLE_RESTORE count and the last SWEEPSTAT
stale / reindexed counters."""
import json, re, sys
arm = sys.argv[1]
t_first = None; ttc = {}; trials = {}; ldpc = None; restores = 0; stat = None
for line in open(arm + "/rx/rx.log", errors="replace"):
    m = re.match(r"([0-9.]+) ", line)
    if not m:
        continue
    t = float(m.group(1))
    if t_first is None and "rnti_seen" in line and "fmt=1_1" in line:
        t_first = t
    m2 = re.search(r"Technique D CONVERGED rnti=\S+ tda=(\d+)", line)
    if m2 and m2.group(1) not in ttc and t_first is not None:
        ttc[m2.group(1)] = round(t - t_first, 3)
    m3 = re.search(r"SWEEP: rnti=\S+ CONVERGED tda=(\d+) .*\((\d+)/\d+ trials on the winner", line)
    if m3 and m3.group(1) not in trials:
        trials[m3.group(1)] = m3.group(2)
    m4 = re.search(r"LDPCDIAG ok=(\d+) seg_fail=(\d+) tb_fail=\d+ zero_tb=(\d+)", line)
    if m4:
        ldpc = list(m4.groups())
    if "ORACLE_RESTORE" in line:
        restores += 1
    m5 = re.search(r"SWEEPSTAT scored=(\d+) stale=(\d+) created=(\d+) reindexed=(\d+)", line)
    if m5:
        stat = dict(scored=int(m5.group(1)), stale=int(m5.group(2)), created=int(m5.group(3)), reindexed=int(m5.group(4)))
print(json.dumps(dict(arm=arm, first_1_1_s=t_first, ttc_per_tda=ttc, winner_trials=trials, ldpc_ok_segfail_zero_tb=ldpc,
                      oracle_restore_lines=restores, sweepstat_last=stat)))
