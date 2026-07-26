#!/bin/bash
# Two-receiver FUSION A/B: cfar_per_row on vs off, N repetitions.
# Each rep runs rx1 and rx2 SIMULTANEOUSLY (distinct netns sets + distinct working dirs), then
# sim-time-aligns their reports, fuses through isac-track, and scores in the world frame.
# Usage: sudo ./_run_fusion_ab.sh [reps] [duration_s]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
REPS=${1:-4}
DUR=${2:-270}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
RX2DIR=/tmp/ms/rx2_run
OUT=/tmp/ms/fusion_ab
mkdir -p $OUT

for rep in $(seq 1 "$REPS"); do
  for cfg in on off; do
    d1=/tmp/ms/ab_${cfg}_${rep}_rx1
    d2=/tmp/ms/ab_${cfg}_${rep}_rx2
    echo "=== rep $rep  cfar_per_row=$cfg  ($(date +%H:%M:%S)) ==="
    ip netns del oai_isac_gnb2 2>/dev/null; ip netns del oai_isac_ue2 2>/dev/null
    ip netns del oai_isac_gnb3 2>/dev/null; ip netns del oai_isac_ue3 2>/dev/null

    ./_run_mot_variant.sh _ab_rx1_${cfg}.conf "$d1" "$DUR" >"$OUT/rx1_${cfg}_${rep}.log" 2>&1 &
    P1=$!
    ( cd $RX2DIR && ./_run_rx2_sync.sh _ab_rx2_${cfg}.conf "$d2" "$DUR" ) >"$OUT/rx2_${cfg}_${rep}.log" 2>&1 &
    P2=$!
    wait $P1 $P2

    m=$OUT/merged_${cfg}_${rep}.jsonl
    t=$OUT/tracks_${cfg}_${rep}.jsonl
    python3 merge_receivers_simtime.py "$d1/oaiue_reports.jsonl" "$d2/oaiue_reports.jsonl" "$m" 150 \
        >"$OUT/merge_${cfg}_${rep}.txt" 2>&1
    timeout 600 $ISAC --out "$t" replay "$m" >/dev/null 2>&1
    echo -n "REP $rep $cfg | per-rx1: "
    python3 score_run.py "$d1" 2>/dev/null | grep -E "^RAW" | head -1
    echo -n "REP $rep $cfg | WORLD:   "
    python3 score_world_tracks.py "$t" "$d1" 15 2>/dev/null | head -1
  done
done
echo "=== batch complete $(date +%H:%M:%S) ==="
