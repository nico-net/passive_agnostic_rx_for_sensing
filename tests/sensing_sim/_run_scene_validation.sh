#!/bin/bash
# Validation batch for the three NEW scenes (crossing / five / manoeuvre) on the CURRENT
# configuration -- sub-slot sampling, cfar_per_row, sub-bin interpolation, 3 receivers.
# Purpose is a correctness check of everything implemented so far, not an A/B.
# Each rep runs all three receivers simultaneously, then fuses 3-pair with the consistency gate.
# Usage: sudo ./_run_scene_validation.sh [reps] [duration_s]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
REPS=${1:-3}
DUR=${2:-270}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
OUT=/tmp/ms/scenes; mkdir -p $OUT

for rep in $(seq 1 "$REPS"); do
  for sc in crossing five manoeuvre; do
    d1=$OUT/${sc}_${rep}_rx1; d2=$OUT/${sc}_${rep}_rx2; d3=$OUT/${sc}_${rep}_rx3
    echo ""
    echo "############ scene=$sc rep=$rep  $(date +%H:%M:%S) ############"
    for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

    ./_run_mot_variant.sh _scene_${sc}_rx1.conf "$d1" "$DUR" >"$OUT/${sc}_${rep}_rx1.log" 2>&1 &
    P1=$!
    ( cd /tmp/ms/rx2_run && ./_run_rx2_sync.sh _scene_${sc}_rx2.conf "$d2" "$DUR" ) >"$OUT/${sc}_${rep}_rx2.log" 2>&1 &
    P2=$!
    ( cd /tmp/ms/rx3_run && ./_run_rx3_sync.sh _scene_${sc}_rx3.conf "$d3" "$DUR" ) >"$OUT/${sc}_${rep}_rx3.log" 2>&1 &
    P3=$!
    wait $P1 $P2 $P3

    echo "--- per-receiver (rx1) ---"
    python3 score_run.py "$d1" 2>/dev/null | sed -n '2,20p'

    m=$OUT/m_${sc}_${rep}.jsonl; t=$OUT/t_${sc}_${rep}.jsonl
    python3 merge_receivers_n.py "$m" 150 \
      "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
      "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" \
      "$d3/oaiue_reports.jsonl@$d3/logs/ue.log" >"$OUT/merge_${sc}_${rep}.txt" 2>&1
    grep -E "fused CPIs" "$OUT/merge_${sc}_${rep}.txt"
    timeout 900 $ISAC --out "$t" --birth-max-rate-rms-mps 4.0 replay "$m" >/dev/null 2>&1
    echo "--- fused world frame (3 pairs + consistency gate) ---"
    python3 score_world_tracks.py "$t" "$d1" 15 2>/dev/null
  done
done
echo ""
echo "############ batch complete $(date +%H:%M:%S) ############"
