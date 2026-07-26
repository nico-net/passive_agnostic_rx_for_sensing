#!/bin/bash
# Flicker-gate on/off A/B on the minimal-config MOT baseline. Sequential, scored via score_run.py.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
DUR=${1:-300}
run_one () {
  local conf=$1 out=$2
  sudo rm -rf "$out"; mkdir -p "$out/logs"
  sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log
  local c="_mot2_fab_run.conf"; cp "$conf" "$c"
  sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
  sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$out/oaiue_sensing\";#" "$c"
  sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$out/oaiue_reports.jsonl\";#" "$c"
  sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
    ./run_sim_traffic_iperf.sh "$DUR" "$out/logs" "$c" 3M
  rm -f "$c"
}
echo "=== FLICKER OFF ==="; run_one _mot2_flick_off.conf /tmp/sensing_mot2_flick_off
echo "=== FLICKER ON ===";  run_one _mot2_flick_on.conf  /tmp/sensing_mot2_flick_on
echo "=== SCORE off ==="; python3 score_run.py /tmp/sensing_mot2_flick_off
echo "=== SCORE on  ==="; python3 score_run.py /tmp/sensing_mot2_flick_on
