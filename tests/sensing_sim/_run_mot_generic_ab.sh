#!/bin/bash
# Generic single-parameter A/B batch: toggles one config key between two values, N repeats each,
# everything else held at the current mot2.conf baseline. Reports unique track ids and far-ghost
# detection fraction per run so a proper (not single-run) comparison can be made.
#
# Usage: sudo ./_run_mot_generic_ab.sh <param_name> <off_value> <on_value> [repeats] [duration_s] [out_root]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

PARAM=${1:?param name required}
OFF_VAL=${2:?off value required}
ON_VAL=${3:?on value required}
REPEATS=${4:-3}
DUR=${5:-300}
OUT_ROOT=${6:-/tmp/mot_ab_${PARAM}}

run_one() {
  local val=$1
  local out=$2
  sudo rm -rf "$out"; mkdir -p "$out/logs"
  sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

  local c
  c=$(mktemp -p . _mot_ab_XXXX.conf)
  cp ue.sensing.traffic.moving.mot2.rfsim.conf "$c"
  sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
  sed -i "s/${PARAM} *= *[0-9.]*;/${PARAM}     = ${val};/" "$c"
  sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$out/oaiue_sensing\";#" "$c"
  sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$out/oaiue_reports.jsonl\";#" "$c"

  sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
    ./run_sim_traffic_iperf.sh "$DUR" "$out/logs" "$c" 3M > "$out/runner.log" 2>&1
  rm -f "$c"

  local n_ids far_dets total_dets
  n_ids=$(grep -oP "track_id=\K[0-9]+" "$out/logs/ue.log" 2>/dev/null | sort -u | wc -l)
  if [ -f "$out/oaiue_sensing_detections.csv" ]; then
    total_dets=$(wc -l < "$out/oaiue_sensing_detections.csv")
    far_dets=$(awk -F, '$4>500' "$out/oaiue_sensing_detections.csv" | wc -l)
  else
    total_dets=0; far_dets=0
  fi
  echo "RESULT param=${PARAM} val=${val} out=$out unique_track_ids=$n_ids far_dets=$far_dets total_dets=$total_dets"
}

echo "=== A/B batch: ${PARAM} = ${OFF_VAL} vs ${ON_VAL}, $REPEATS repeats each, ${DUR}s per run ==="
for i in $(seq 1 "$REPEATS"); do
  run_one "$OFF_VAL" "$OUT_ROOT/off_$i"
done
for i in $(seq 1 "$REPEATS"); do
  run_one "$ON_VAL" "$OUT_ROOT/on_$i"
done
echo "=== ${PARAM} batch done ==="
