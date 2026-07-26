#!/bin/bash
# Live run of the FIXED transversal crossing scene + the 5-target scene, 3 receivers each.
# Detached-safe: launch with `setsid` so a session teardown cannot kill it mid-batch.
# Usage: sudo setsid ./_run_crossing_check.sh [dur] > /tmp/ms/xcheck.log 2>&1 &
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
DUR=${1:-270}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
OUT=/tmp/ms/xcheck; mkdir -p $OUT

for sc in crossing five; do
  d1=$OUT/${sc}_rx1; d2=$OUT/${sc}_rx2; d3=$OUT/${sc}_rx3
  echo ""; echo "############ $sc  $(date +%H:%M:%S) ############"
  for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

  ./_run_mot_variant.sh _scene_${sc}_rx1.conf "$d1" "$DUR" >"$OUT/${sc}_rx1.log" 2>&1 &
  P1=$!
  ( cd /tmp/ms/rx2_run && ./_run_rx2_sync.sh _scene_${sc}_rx2.conf "$d2" "$DUR" ) >"$OUT/${sc}_rx2.log" 2>&1 &
  P2=$!
  ( cd /tmp/ms/rx3_run && ./_run_rx3_sync.sh _scene_${sc}_rx3.conf "$d3" "$DUR" ) >"$OUT/${sc}_rx3.log" 2>&1 &
  P3=$!
  wait $P1 $P2 $P3

  echo "--- per-receiver rx1 ---"
  python3 score_run.py "$d1" 2>/dev/null | sed -n '2,18p'
  echo "--- identity continuity (rx1) ---"
  python3 check_identity.py "$d1" 2>/dev/null
  m=$OUT/m_${sc}.jsonl; t=$OUT/t_${sc}.jsonl
  python3 merge_receivers_n.py "$m" 150 \
    "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
    "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" \
    "$d3/oaiue_reports.jsonl@$d3/logs/ue.log" 2>&1 | grep -E "fused CPIs"
  timeout 900 $ISAC --out "$t" --birth-max-rate-rms-mps 4.0 replay "$m" >/dev/null 2>&1
  echo "--- fused world (3 pairs + gate) ---"
  python3 score_world_tracks.py "$t" "$d1" 15 2>/dev/null
done
echo ""; echo "############ done $(date +%H:%M:%S) ############"
