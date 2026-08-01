#!/bin/bash
# Does the tracker work survive the addition of GEOMETRY?
#
# Everything in GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md §7.16-§7.20 was measured on ONE receiver,
# where birth redundancy is exactly 0 (`pairs + bearings - 2` = 0), so `birth_max_pos_chi2` is
# structurally inert and MAP's ghost-birth suppression had no competition. The oracle budget (§7.20)
# then showed detection-stage work is exhausted and the residual error is birth/association geometry.
#
# That raises the question this script answers: with 2 or 3 receivers the geometry itself rejects bad
# births, so does `map + existence` still buy anything, or is it a single-receiver-only mitigation?
# Both answers are useful and the difference matters before recommending a default.
#
# Design: ONE simultaneous 3-receiver capture, every arm scored OFFLINE from it, so detections are
# byte-identical across arms and differences are attributable to the fusion/tracker rather than to
# this harness's large run-to-run variance (8-58% fused precision across identical repetitions).
#
#   rx1 (100,0)    4-element lambda/2 ULA -> reports AoA
#   rx2 (0,200)    4-element lambda/2 ULA -> reports AoA
#   rx3 (-50,-50)  single antenna         -> range/Doppler only (B210 stand-in)
#
# Usage: sudo setsid nohup ./_run_geom_vs_tracker.sh [dur] [scene] > /tmp/ms/geom.log 2>&1 &
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
DUR=${1:-600}
SC=${2:-crossing}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
OUT=${OUT:-/tmp/ms/geom_vs_tracker}; mkdir -p "$OUT"
CSV="$OUT/results.csv"
echo "capture,arm,metric,emitted,good,precision,median_err,ids" > "$CSV"

d1=$OUT/rx1; d2=$OUT/rx2; d3=$OUT/rx3
for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

echo "############ capture: $SC, ${DUR}s, 3 receivers ($(date +%H:%M:%S)) ############"
UE_NB_ANT_RX=4 ./_run_mot_variant.sh _scene_${SC}_rx1_auto.conf "$d1" "$DUR" >"$OUT/rx1.log" 2>&1 &
P1=$!
( cd /tmp/ms/rx2_run && UE_NB_ANT_RX=4 ./_run_rx2_sync.sh _scene_${SC}_rx2_auto.conf "$d2" "$DUR" ) >"$OUT/rx2.log" 2>&1 &
P2=$!
( cd /tmp/ms/rx3_run && ./_run_rx3_sync.sh _scene_${SC}_rx3_auto.conf "$d3" "$DUR" ) >"$OUT/rx3.log" 2>&1 &
P3=$!
wait $P1 $P2 $P3
echo "############ capture done ($(date +%H:%M:%S)) ############"
for d in $d1 $d2 $d3; do
  echo "  $(basename $d): $(wc -l < "$d/oaiue_reports.jsonl" 2>/dev/null || echo 0) reports"
done

# ---------------------------------------------------------------- receiver streams
m1=$OUT/m1.jsonl; m2=$OUT/m2.jsonl; m3=$OUT/m3.jsonl
cp "$d1/oaiue_reports.jsonl" "$m1"
python3 merge_receivers_n.py "$m2" 150 \
  "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
  "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" 2>&1 | grep -E "fused CPIs" || true
python3 merge_receivers_n.py "$m3" 150 \
  "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
  "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" \
  "$d3/oaiue_reports.jsonl@$d3/logs/ue.log" 2>&1 | grep -E "fused CPIs" || true

# ---------------------------------------------------------------- arms
# 1 receiver needs --birth-single-pair-bearing (2 constraints, 2 unknowns) and CANNOT use
# --birth-min-pos-redundancy, which would refuse every birth there. 2-3 receivers have real
# redundancy, so the geometric gates are the ones under test.
arm() { # nrx  name  merged  extra-args...
  local nrx=$1 name=$2 m=$3; shift 3
  local t="$OUT/t_${nrx}rx_$name.jsonl"
  [ -s "$m" ] || { echo "  ${nrx}rx/$name: no input"; return; }
  timeout 900 $ISAC replay "$m" --quiet "$@" --out "$t" 2>/dev/null
  python3 score_arm_csv.py "$t" "$d1" "${nrx}rx" "$name" 15 >> "$CSV"
}

G1=(--birth-max-pos-chi2 9 --birth-single-pair-bearing)
GN=(--birth-max-pos-chi2 9 --birth-min-pos-redundancy 1)
MAPEX=(--map-window 3 --use-existence-prob)

arm 1 legacy    "$m1" "${G1[@]}"
arm 1 map_exist "$m1" "${G1[@]}" "${MAPEX[@]}"
arm 2 legacy    "$m2" "${GN[@]}"
arm 2 map_exist "$m2" "${GN[@]}" "${MAPEX[@]}"
arm 3 legacy    "$m3" "${GN[@]}"
arm 3 map_exist "$m3" "${GN[@]}" "${MAPEX[@]}"

echo ""; echo "############ done $(date +%H:%M:%S) -> $CSV ############"
column -s, -t < "$CSV"
