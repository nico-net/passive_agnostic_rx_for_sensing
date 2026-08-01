#!/bin/bash
# Measured ERROR BUDGET: convert architectural hypotheses into quantitative upper bounds.
#
# For each capture, replay three streams through the SAME tracker:
#   baseline            what the receiver actually emitted
#   oracle_no_harmonic  ground truth used to delete ONLY identified k-harmonics and mirrors.
#                       This is the exact ceiling of "preserve harmonic-family structure and
#                       propagate it into the tracker" -- a perfect version of that architecture
#                       cannot do better than deleting every harmonic, which is what this does.
#   oracle_perfect      ground truth used to delete EVERY false detection. A far stronger model than
#                       any real detector could be; the ceiling of detection-stage work in general.
#
# The gap that REMAINS under oracle_perfect is, by construction, not attributable to detection
# quality at all -- it is birth/association geometry. That is the number this script exists to
# produce, and it is why the oracles are worth running instead of argued about.
#
# Usage: _run_oracle_budget.sh [capture_dir ...]   (default: /tmp/ghostkin/ab/cap*)
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
COMMON=(--quiet --birth-max-pos-chi2 9 --birth-single-pair-bearing)
OUT=${OUT:-/tmp/ghostkin/oracle}
mkdir -p "$OUT"
CSV="$OUT/budget.csv"
echo "capture,arm,metric,emitted,good,precision,median_err,ids" > "$CSV"

CAPS=("$@")
[ ${#CAPS[@]} -eq 0 ] && CAPS=(/tmp/ghostkin/ab/cap*/)

for cap in "${CAPS[@]}"; do
  cap="${cap%/}"
  [ -s "$cap/oaiue_reports.jsonl" ] || continue
  name=$(basename "$cap")
  echo "#### $name ($(wc -l < "$cap/oaiue_reports.jsonl") CPIs)"
  python3 oracle_family_filter.py "$cap" --out "$OUT/${name}_ro.jsonl"   --keep real+other 2>&1 | sed 's/^/    /'
  python3 oracle_family_filter.py "$cap" --out "$OUT/${name}_real.jsonl" --keep real       2>&1 | sed 's/^/    /'
  for v in "baseline:$cap/oaiue_reports.jsonl" \
           "oracle_no_harmonic:$OUT/${name}_ro.jsonl" \
           "oracle_perfect:$OUT/${name}_real.jsonl"; do
    n=${v%%:*}; f=${v#*:}
    [ -s "$f" ] || continue
    timeout 600 $ISAC replay "$f" "${COMMON[@]}" --out "$OUT/${name}_$n.jsonl" 2>/dev/null
    python3 score_arm_csv.py "$OUT/${name}_$n.jsonl" "$cap" "$name" "$n" 15 >> "$CSV"
  done
done

echo "############ budget -> $CSV ############"
python3 aggregate_tracker_ab.py "$CSV"
