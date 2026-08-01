#!/bin/bash
# A/B of the three tracker architectures against the legacy hit-counting FSM, over several
# independent captures.
#
#   legacy     Tentative/Confirmed/Coasting FSM, greedy per-CPI association+birth  (today's default)
#   existence  recursive Bernoulli/IPDA existence probability (--use-existence-prob)
#   map        sliding-window MAP birth gating by min-cost flow  (--map-window)
#   map+exist  both
#
# One capture is REPLAYED through every arm, so the arms see byte-identical detections and any
# difference is attributable to the tracker rather than to this harness's large run-to-run variance.
# Multiple independent captures are what gives the comparison any statistical standing at all -- the
# project has been bitten before by single-capture conclusions (CLAUDE.md's retracted numbers).
#
# Scored BOTH ways on purpose:
#   * emitted   -- every update the tracker publishes (score_emitted_tracks.py). The consumer's view.
#   * confirmed -- status=="confirmed" only (score_world_tracks.py). The project's historical metric.
# They disagree, because the legacy FSM publishes a large coasting stream that is ~10 % correct, and
# quoting only one of them is how that got overlooked.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

N=${N:-6}                 # captures
DUR=${DUR:-800}           # seconds per capture
OUT=${OUT:-/tmp/ghostkin/ab}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
COMMON=(--quiet --birth-max-pos-chi2 9 --birth-single-pair-bearing)
MAPW=${MAPW:-3}
mkdir -p "$OUT"
CSV="$OUT/results.csv"
echo "capture,arm,metric,emitted,good,precision,median_err,ids" > "$CSV"

arm() { # capture_dir  name  extra-args...
  local cap=$1 name=$2; shift 2
  local t="$OUT/$(basename "$cap")_$name.jsonl"
  timeout 600 $ISAC replay "$cap/oaiue_reports.jsonl" "${COMMON[@]}" "$@" --out "$t" 2>/dev/null
  python3 score_arm_csv.py "$t" "$cap" "$(basename "$cap")" "$name" 15 >> "$CSV"
}

for i in $(seq 1 "$N"); do
  cap="$OUT/cap$i"
  echo "######## capture $i/$N (${DUR}s) ########"
  sudo env UE_NB_ANT_RX=4 ./_run_mot_variant.sh _scene_four_rx1_auto.conf "$cap" "$DUR" \
      > "$OUT/cap$i.log" 2>&1
  [ -s "$cap/oaiue_reports.jsonl" ] || { echo "  capture $i produced no reports, skipping"; continue; }
  echo "  CPIs: $(wc -l < "$cap/oaiue_reports.jsonl")"
  arm "$cap" legacy
  arm "$cap" existence --use-existence-prob
  arm "$cap" map       --map-window "$MAPW"
  arm "$cap" map_exist --map-window "$MAPW" --use-existence-prob
done

echo "############ done -> $CSV ############"
python3 aggregate_tracker_ab.py "$CSV" || cat "$CSV"
