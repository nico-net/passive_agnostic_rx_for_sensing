#!/bin/bash
# Single density-restoration test: cfar_per_row=1 with a raised CFAR budget, both receivers in
# parallel, then sim-time align -> fuse -> score. Usage: sudo ./_run_density_single.sh [fa] [dur]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
FA=${1:-16}
DUR=${2:-270}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
OUT=/tmp/ms/dens_single; mkdir -p $OUT
d1=/tmp/ms/dens1_${FA}_rx1; d2=/tmp/ms/dens1_${FA}_rx2

ip netns del oai_isac_gnb2 2>/dev/null; ip netns del oai_isac_ue2 2>/dev/null
ip netns del oai_isac_gnb3 2>/dev/null; ip netns del oai_isac_ue3 2>/dev/null

./_run_mot_variant.sh _dens_rx1_${FA}.conf "$d1" "$DUR" >"$OUT/rx1.log" 2>&1 &
P1=$!
( cd /tmp/ms/rx2_run && ./_run_rx2_sync.sh _dens_rx2_${FA}.conf "$d2" "$DUR" ) >"$OUT/rx2.log" 2>&1 &
P2=$!
wait $P1 $P2

m=$OUT/merged_${FA}.jsonl; t=$OUT/tracks_${FA}.jsonl
python3 merge_receivers_simtime.py "$d1/oaiue_reports.jsonl" "$d2/oaiue_reports.jsonl" "$m" 150 >"$OUT/merge.txt" 2>&1
timeout 600 $ISAC --out "$t" replay "$m" >/dev/null 2>&1

echo "=== cfar_target_fa_per_cpi=$FA, cfar_per_row=1 ==="
echo "--- per-receiver (rx1) ---"
python3 score_run.py "$d1" 2>/dev/null | tail -8
echo "--- fused detection density (the variable under test) ---"
grep PAIRED "$OUT/merge.txt"
python3 -c "
import json
n=d=0
for l in open('$m'):
    j=json.loads(l); n+=1; d+=len(j.get('detections',[]))
print(f'dets/CPI/rx = {d/max(n,1):.1f}   (on@4.0 baseline was 3.8, per_row=0 was 7.5)')
"
echo "--- fused world frame (single run: indicative only, this metric ranged 8-58% across reps) ---"
python3 score_world_tracks.py "$t" "$d1" 15 2>/dev/null
