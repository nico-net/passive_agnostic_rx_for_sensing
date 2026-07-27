#!/bin/bash
# FULL FACTORIAL: {2 rx, 3 rx} x {AoA on, AoA off} x {gated, ungated}, repeated until a deadline.
#
# Design notes that matter for interpreting the output:
#   * ONE simultaneous 3-receiver capture per repetition; all 8 arms are then scored OFFLINE from
#     that SAME capture. The detections are therefore byte-identical across arms, so any difference
#     between arms is attributable to the fusion configuration and nothing else.
#   * Repetitions are the point. This harness's fused precision was measured swinging 8-58% across
#     IDENTICAL repetitions (handover 1), so a single run settles nothing; the batch runs until a
#     wall-clock deadline and reports mean +/- SD with per-rep detail.
#   * Only the three factors vary between arms. No other rejection mechanism (rate-RMS, harmonic,
#     speed bound) is enabled anywhere, so the factorial is clean.
#
# Scene: 4 objects, all different and NON-CONSTANT speeds, curved (polynomial + cross-track wobble)
# trajectories, and obj0/obj1 genuinely intersect (min world separation ~10 m).
#
# Fleet: rx1 (100,0) and rx2 (0,200) carry L-shaped 4-element arrays -> unambiguous over 360 deg;
#        rx3 (-50,-50) is single-channel (B210 stand-in) and reports no bearing.
#
# Usage: sudo setsid ./_run_aoa_factorial.sh [hours] [dur_s] [scene] [--resume] > /tmp/ms/fact.log 2>&1
# --resume: keep existing results.csv/azimuth.csv/deadline and append (a watchdog relaunch after an
#           unexpected death uses this so a crash mid-batch does not lose already-completed reps).
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
HOURS=${1:-4}
DUR=${2:-240}
SC=${3:-four}
RESUME=0
[ "${4:-}" = "--resume" ] && RESUME=1
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
ROOT=/tmp/ms/aoa_factorial
DEADLINE_FILE=$ROOT/deadline

GATE="--birth-max-pos-chi2 9 --birth-min-pos-redundancy 1"

if [ "$RESUME" = 1 ] && [ -f "$DEADLINE_FILE" ] && [ -f "$ROOT/results.csv" ]; then
  DEADLINE=$(cat "$DEADLINE_FILE")
  RES=$ROOT/results.csv
  AZ=$ROOT/azimuth.csv
  rep=$(( $(tail -n +2 "$RES" 2>/dev/null | cut -d, -f1 | sort -n | tail -1) ))
  rep=${rep:-0}
  echo "################ RESUMING at rep $((rep+1)), deadline $(date -d @$DEADLINE +%H:%M:%S) ################"
else
  DEADLINE=$(( $(date +%s) + $(python3 -c "print(int($HOURS*3600))") ))
  echo "$DEADLINE" > "$DEADLINE_FILE"
  chmod 666 "$DEADLINE_FILE" 2>/dev/null
  rm -rf "$ROOT"/rep* "$ROOT"/*.jsonl 2>/dev/null
  RES=$ROOT/results.csv
  AZ=$ROOT/azimuth.csv
  echo "rep,nrx,aoa,gated,arm,confirmed,matched,precision_pct,median_err_m,matched_err_m,ids,obj0,obj1,obj2,obj3" > "$RES"
  echo "rep,rx,reports,detections,with_az,matched,mean_err,median_err,rms_err,p90_err,max_err,median_sigma" > "$AZ"
  rep=0
fi
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  rep=$((rep+1))
  OUT=$ROOT/rep$rep; mkdir -p "$OUT"
  d1=$OUT/rx1; d2=$OUT/rx2; d3=$OUT/rx3
  echo ""; echo "################ REP $rep  ($(date +%H:%M:%S), deadline $(date -d @$DEADLINE +%H:%M:%S)) ################"
  for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

  UE_NB_ANT_RX=4 ./_run_mot_variant.sh _scene_${SC}_rx1.conf "$d1" "$DUR" >"$OUT/rx1.log" 2>&1 &
  P1=$!
  ( cd /tmp/ms/rx2_run && UE_NB_ANT_RX=4 ./_run_rx2_sync.sh _scene_${SC}_rx2.conf "$d2" "$DUR" ) >"$OUT/rx2.log" 2>&1 &
  P2=$!
  ( cd /tmp/ms/rx3_run && ./_run_rx3_sync.sh _scene_${SC}_rx3.conf "$d3" "$DUR" ) >"$OUT/rx3.log" 2>&1 &
  P3=$!
  wait $P1 $P2 $P3
  echo "  reports: rx1=$(wc -l < $d1/oaiue_reports.jsonl 2>/dev/null || echo 0) rx2=$(wc -l < $d2/oaiue_reports.jsonl 2>/dev/null || echo 0) rx3=$(wc -l < $d3/oaiue_reports.jsonl 2>/dev/null || echo 0)"

  # Per-receiver STO-tracker internals. With sync_correction ON these are the diagnostic that says
  # WHETHER the tracker actually locked or spent the CPI dead-reckoning -- without them a degraded arm
  # cannot be told apart from a correctly-working one, which is what made the 2026-07-23 finding hard
  # to interpret after the fact. With sync OFF they are all zero and cost nothing.
  for rr in rx1 rx2 rx3; do
    printf "  sync %s: %s\n" "$rr" \
      "$(grep -o 'STO\[n=[0-9]* fly=[0-9]*' "$OUT/$rr/logs/ue.log" 2>/dev/null | \
         awk -F'[= ]' '{n+=$2; f+=$4; c++} END{if(c) printf "locked/CPI %.1f  flywheel/CPI %.1f  (%d CPIs)", n/c, f/c, c; else print "no sync log lines"}')"
  done

  # Per-array-receiver bearing accuracy, so a mirrored/misconfigured array can never again be
  # mistaken for a fusion result.
  for rr in rx1 rx2; do
    python3 score_azimuth.py "$OUT/$rr" --csv "$rep,$rr" >> "$AZ" 2>/dev/null
  done

  m2=$OUT/m2.jsonl; m3=$OUT/m3.jsonl
  python3 merge_receivers_n.py "$m2" 150 \
    "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
    "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" >/dev/null 2>&1
  python3 merge_receivers_n.py "$m3" 150 \
    "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
    "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" \
    "$d3/oaiue_reports.jsonl@$d3/logs/ue.log" >/dev/null 2>&1

  arm() { # nrx aoa gated merged extra
    local nrx=$1 aoa=$2 gated=$3 merged=$4 extra=$5
    local name="${nrx}rx_$([ "$aoa" = 1 ] && echo aoa || echo noaoa)_$([ "$gated" = 1 ] && echo gated || echo open)"
    local t=$OUT/t_$name.jsonl
    timeout 900 $ISAC --quiet --out "$t" $extra replay "$merged" >/dev/null 2>&1
    python3 score_world_tracks.py --csv "$rep,$nrx,$aoa,$gated,$name" "$t" "$d1" 15 >> "$RES" 2>/dev/null
    echo "    $name -> $(tail -1 $RES | cut -d, -f8,9)"
  }
  for nrx in 2 3; do
    mm=$m2; [ "$nrx" = 3 ] && mm=$m3
    arm "$nrx" 0 0 "$mm" "--no-bearing"
    arm "$nrx" 0 1 "$mm" "--no-bearing $GATE"
    arm "$nrx" 1 0 "$mm" ""
    arm "$nrx" 1 1 "$mm" "$GATE"
  done

  pkill -9 -f "nr-.*softmodem" 2>/dev/null; pkill -9 -f iperf3 2>/dev/null; sleep 3
  # Keep only the merged/track outputs; the raw per-receiver captures are large.
  rm -f "$OUT"/*/oaiue_sensing_rvm_*.f32 2>/dev/null
done

echo ""; echo "################ FACTORIAL COMPLETE: $rep reps ################"
python3 report_aoa_factorial.py "$ROOT"
