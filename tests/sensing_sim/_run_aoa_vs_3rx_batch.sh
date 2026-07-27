#!/bin/bash
# N repetitions of the "2 array receivers vs 3 receivers" comparison, aggregated.
# Reps matter: this harness's fused precision was measured swinging 8-58% across IDENTICAL
# repetitions (handover 1), so a single run settles nothing.
# Usage: sudo setsid ./_run_aoa_vs_3rx_batch.sh [reps] [dur] > /tmp/ms/aoa3b.log 2>&1
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
REPS=${1:-4}
DUR=${2:-240}
rm -rf /tmp/ms/aoa_vs_3rx; mkdir -p /tmp/ms/aoa_vs_3rx
echo "rep,arm,confirmed,matched,precision_pct,median_err_m,obj0,obj1" > /tmp/ms/aoa_vs_3rx/results.csv
for r in $(seq 1 "$REPS"); do
  echo ""; echo "################ REP $r/$REPS ################"
  ./_run_aoa_vs_3rx.sh "$DUR" crossing "$r"
  pkill -9 -f "nr-.*softmodem" 2>/dev/null; pkill -9 -f iperf3 2>/dev/null; sleep 3
  for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done
done
echo ""; echo "################ AGGREGATE ################"
python3 aggregate_aoa_vs_3rx.py /tmp/ms/aoa_vs_3rx/results.csv
