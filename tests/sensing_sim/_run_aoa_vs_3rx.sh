#!/bin/bash
# "Do two ARRAY receivers replace three range-only receivers?" -- PHASE3_AOA_MULTISTATIC_HANDOVER 8.11.
#
# ONE simultaneous 3-receiver capture, then every fusion configuration is scored OFFLINE from that
# SAME capture. That is the whole point of the design: the detections are byte-identical across arms,
# so any difference is attributable to the fusion, not to run-to-run variance -- which this harness has
# in abundance (8-58% fused precision across identical repetitions, see the handover 1).
#
#   rx1 (100,0)    4-element lambda/2 ULA, boresight 90  -> X410 stand-in, reports AoA
#   rx2 (0,200)    4-element lambda/2 ULA, boresight 0   -> X410 stand-in, reports AoA
#   rx3 (-50,-50)  single antenna, no array              -> B210 stand-in, range/Doppler only
#
# Usage: sudo setsid nohup ./_run_aoa_vs_3rx.sh [dur] > /tmp/ms/aoa3.log 2>&1 &
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
DUR=${1:-300}
SC=${2:-crossing}
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
REP=${3:-1}
OUT=/tmp/ms/aoa_vs_3rx/rep$REP; mkdir -p "$OUT"
RES=/tmp/ms/aoa_vs_3rx/results.csv

d1=$OUT/rx1; d2=$OUT/rx2; d3=$OUT/rx3
for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

echo "############ capture: $SC, ${DUR}s, 3 receivers ($(date +%H:%M:%S)) ############"
# rx1/rx2 carry arrays -> 4 coherent rx chains. rx3 is the single-channel B210 stand-in and MUST stay
# at 1 antenna: a heterogeneous fleet is the case the per-detection measurement dimension exists for.
UE_NB_ANT_RX=4 ./_run_mot_variant.sh _scene_${SC}_rx1.conf "$d1" "$DUR" >"$OUT/rx1.log" 2>&1 &
P1=$!
( cd /tmp/ms/rx2_run && UE_NB_ANT_RX=4 ./_run_rx2_sync.sh _scene_${SC}_rx2.conf "$d2" "$DUR" ) >"$OUT/rx2.log" 2>&1 &
P2=$!
( cd /tmp/ms/rx3_run && ./_run_rx3_sync.sh _scene_${SC}_rx3.conf "$d3" "$DUR" ) >"$OUT/rx3.log" 2>&1 &
P3=$!
wait $P1 $P2 $P3
echo "############ capture done ($(date +%H:%M:%S)) ############"

for d in $d1 $d2 $d3; do
  echo "  $(basename $d): $(wc -l < $d/oaiue_reports.jsonl 2>/dev/null || echo 0) reports"
done
echo "--- per-receiver bearing accuracy (the array receivers only) ---"
python3 score_azimuth.py "$d1" 2>&1 | sed -n '2,6p'
python3 score_azimuth.py "$d2" 2>&1 | sed -n '2,6p'

# ---------------------------------------------------------------- fusion arms, same capture
m2=$OUT/m2.jsonl; m3=$OUT/m3.jsonl
python3 merge_receivers_n.py "$m2" 150 \
  "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
  "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" 2>&1 | grep -E "fused CPIs"
python3 merge_receivers_n.py "$m3" 150 \
  "$d1/oaiue_reports.jsonl@$d1/logs/ue.log" \
  "$d2/oaiue_reports.jsonl@$d2/logs/ue.log" \
  "$d3/oaiue_reports.jsonl@$d3/logs/ue.log" 2>&1 | grep -E "fused CPIs"

# GATES: chi2 9 ~ 3 sigma; min-redundancy 1 = "only birth what can be verified".
# Emits both a human line and a machine-readable CSV row, so repetitions can be aggregated: this
# harness's fused precision was measured swinging 8-58% across IDENTICAL repetitions (handover 1),
# so a single run cannot settle anything.
arm() { # name  merged-file  extra-isac-args
  local t=$OUT/t_$1.jsonl
  timeout 600 $ISAC --quiet --out "$t" $3 replay "$2" >/dev/null 2>&1
  echo ""; echo "=== ARM: $1 (rep $REP) ==="
  local out
  out=$(python3 score_world_tracks.py "$t" "$d1" 15 2>/dev/null)
  echo "$out"
  # "confirmed updates: N  within 15.0m: M (P% precision)  distinct ids: I"
  # "median world err: E m (matched X m)  per-target: obj0=A  obj1=B"
  local n m prec err o0 o1
  n=$(sed -n 's/.*confirmed updates: \([0-9]*\).*/\1/p' <<<"$out")
  m=$(sed -n 's/.*within [0-9.]*m: \([0-9]*\).*/\1/p' <<<"$out")
  prec=$(sed -n 's/.*(\([0-9]*\)% precision).*/\1/p' <<<"$out")
  err=$(sed -n 's/.*median world err: \([0-9.]*\) m.*/\1/p' <<<"$out")
  o0=$(sed -n 's/.*obj0=\([0-9]*\).*/\1/p' <<<"$out")
  o1=$(sed -n 's/.*obj1=\([0-9]*\).*/\1/p' <<<"$out")
  echo "$REP,$1,${n:-0},${m:-0},${prec:-0},${err:-nan},${o0:-0},${o1:-0}" >> "$RES"
}

arm "3rx_norange_aoa"  "$m3" "--no-bearing --birth-max-rate-rms-mps 4.0"
arm "2rx_no_aoa"       "$m2" "--no-bearing"
arm "2rx_aoa_nogate"   "$m2" ""
arm "2rx_aoa_gated"    "$m2" "--birth-max-pos-chi2 9 --birth-min-pos-redundancy 1"
arm "3rx_aoa_gated"    "$m3" "--birth-max-pos-chi2 9 --birth-min-pos-redundancy 1 --birth-max-rate-rms-mps 4.0"

echo ""; echo "############ done $(date +%H:%M:%S) ############"
