#!/bin/bash
# A/B batch: far_harmonic_reject on vs off, N repeats each, everything else identical (current
# best-known config: csi_rs reverted to pdsch_data-only, cpi_quality_gate on, track_harmonic_reject
# on). A single before/after run pair was inconclusive (2026-07-24, PHASE2_MOT_MULTIUE_HANDOVER.md)
# -- this runs a real multi-rep batch instead, same methodology as the earlier track_harmonic_reject
# A/B (which DID give a clean, reliable result).
#
# Usage: sudo ./_run_mot_farharm_ab.sh [repeats] [duration_s] [out_root]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

REPEATS=${1:-4}
DUR=${2:-300}
OUT_ROOT=${3:-/tmp/mot_farharm_ab}

run_one() {
  local reject=$1
  local out=$2
  sudo rm -rf "$out"; mkdir -p "$out/logs"
  sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

  local c
  c=$(mktemp -p . _mot_farharm_ab_XXXX.conf)
  cp ue.sensing.traffic.moving.mot2.rfsim.conf "$c"
  sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
  sed -i "s/far_harmonic_reject *= *[0-9]*;/far_harmonic_reject     = $reject;/" "$c"
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
  echo "reject=$reject out=$out unique_track_ids=$n_ids far_dets=$far_dets total_dets=$total_dets"
}

echo "=== A/B batch: $REPEATS repeats each, ${DUR}s per run ==="
for i in $(seq 1 "$REPEATS"); do
  run_one 0 "$OUT_ROOT/off_$i"
done
for i in $(seq 1 "$REPEATS"); do
  run_one 1 "$OUT_ROOT/on_$i"
done
echo "=== batch done ==="
