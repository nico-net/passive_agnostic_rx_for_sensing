#!/bin/bash
# Regression gate: N x 150 s 106-PRB fully agnostic baseline. Exit 0 iff all pass.
# Thresholds are overridable per host: GATE_CRC_MIN (default 98.0, DGX), GATE_DROP_MAX (default 1.0, DGX),
# e.g. a CPU-limited cloud host may use a re-baselined threshold.
set -u
export GATE_CRC_MIN=${GATE_CRC_MIN:-98.0} GATE_DROP_MAX=${GATE_DROP_MAX:-1.0}
H=$(cd "$(dirname "$0")" && pwd); N=${1:-2}; OUT=${OUT:-/tmp/rfsim_regress_$(date +%Y%m%d_%H%M%S)}
rc=0
for i in $(seq 1 "$N"); do
  "$H/rfsim_arm.sh" "$OUT/base_r$i" 150 >/dev/null 2>&1
  j=$(python3 "$H/score_rx.py" --json "$OUT/base_r$i"); echo "$j"
  python3 - "$j" <<'EOF' || rc=1
import json, os, sys
s = json.loads(sys.argv[1])
ok = (s["n_converged"] >= 1 and (s["crc_pct"] or 0) >= float(os.environ["GATE_CRC_MIN"])
      and s["drop_full_pct"] is not None and s["drop_full_pct"] <= float(os.environ["GATE_DROP_MAX"]))
print(("PASS " if ok else "FAIL ") + s["arm"]); sys.exit(0 if ok else 1)
EOF
done
echo "evidence: $OUT"; exit $rc
