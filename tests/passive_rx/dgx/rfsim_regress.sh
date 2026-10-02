#!/bin/bash
# Regression gate: N x 150 s 106-PRB fully agnostic baseline. Exit 0 iff all pass.
# GATE_MODE=postconv (default, operator option a): PASS iff, per run, ALL of
#   n_contexts          >= GATE_NCTX_MIN         (2: tda0 + tda2 contexts)
#   postconv_crc        >= GATE_POSTCONV_CRC_MIN (99.5; CRC after the last context converged)
#   ttc_tda0 / ttc_tda2 <= GATE_TTC_MAX_TDA0 (12 s) / GATE_TTC_MAX_TDA2 (47 s)  (a further GATE_TTC_MAX_TDA<n> bounds tda n)
#   crc_floor           >= GATE_CRC_FLOOR        (93.0; overall CRC, search-phase sanity floor)
#   drop_full           <= GATE_DROP_MAX         (1.0 %)
# GATE_MODE=legacy: n_converged >= 1, overall CRC >= GATE_CRC_MIN (98.0), drop_full <= GATE_DROP_MAX (pre-K39 criterion).
# Defaults are calibrated on the DGX idle host (README.txt, "Postconv gate calibration"); override per host, e.g. cloud x86.
# Every criterion is printed with its value and limit on the PASS/FAIL line.
set -u
H=$(cd "$(dirname "$0")" && pwd); N=${1:-2}; OUT=${OUT:-/tmp/rfsim_regress_$(date +%Y%m%d_%H%M%S)}
rc=0; mkdir -p "$OUT"
for i in $(seq 1 "$N"); do
  "$H/rfsim_arm.sh" "$OUT/base_r$i" 150 >"$OUT/base_r$i.arm.log" 2>&1
  python3 "$H/score_rx.py" --json "$OUT/base_r$i"
  python3 "$H/score_rx.py" --gate "$OUT/base_r$i" 2>/dev/null || rc=1
done
echo "evidence: $OUT"; exit $rc
