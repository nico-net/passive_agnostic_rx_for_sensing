#!/bin/bash
# Single THREE-receiver multi-static test: rx1(100,0) rx2(0,200) rx3(-50,-50), all simultaneous.
# Fuses 2-pair and 3-pair versions of the SAME capture so the only difference is the extra pair.
# Usage: sudo ./_run_3rx_single.sh [fa] [dur]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
FA=${1:-16}; DUR=${2:-270}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
OUT=/tmp/ms/three; mkdir -p $OUT
d1=$OUT/rx1; d2=$OUT/rx2; d3=$OUT/rx3

for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

./_run_mot_variant.sh _dens_rx1_${FA}.conf "$d1" "$DUR" >"$OUT/rx1.log" 2>&1 &
P1=$!
( cd /tmp/ms/rx2_run && ./_run_rx2_sync.sh _dens_rx2_${FA}.conf "$d2" "$DUR" ) >"$OUT/rx2.log" 2>&1 &
P2=$!
( cd /tmp/ms/rx3_run && ./_run_rx3_sync.sh _dens_rx3_${FA}.conf "$d3" "$DUR" ) >"$OUT/rx3.log" 2>&1 &
P3=$!
wait $P1 $P2 $P3

echo "=== per-receiver CPIs ==="
for d in $d1 $d2 $d3; do echo -n "$(basename $d): "; grep -c "SENSING: CPI #" $d/logs/ue.log; done

run () { # $1=label $2=merged $3=tracks  $4..=specs
  local lab=$1 m=$2 t=$3; shift 3
  python3 merge_receivers_n.py "$m" 150 "$@" >"$OUT/merge_$lab.txt" 2>&1
  timeout 900 $ISAC --out "$t" replay "$m" >/dev/null 2>&1
  echo "--- $lab ---"; grep -E "fused CPIs" "$OUT/merge_$lab.txt"
  python3 score_world_tracks.py "$t" "$d1" 15 2>/dev/null
}
echo "=== FUSION ==="
run 2pair $OUT/m2.jsonl $OUT/t2.jsonl "$d1/oaiue_reports.jsonl:100,0" "$d2/oaiue_reports.jsonl:0,200"
run 3pair $OUT/m3.jsonl $OUT/t3.jsonl "$d1/oaiue_reports.jsonl:100,0" "$d2/oaiue_reports.jsonl:0,200" "$d3/oaiue_reports.jsonl:-50,-50"
echo "=== 3-pair + over-determined consistency gate (only possible with >=3 pairs) ==="
timeout 900 $ISAC --out $OUT/t3g.jsonl --birth-max-rate-rms-mps 4.0 replay $OUT/m3.jsonl >/dev/null 2>&1
python3 score_world_tracks.py $OUT/t3g.jsonl "$d1" 15 2>/dev/null
