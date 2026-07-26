#!/bin/bash
# Two-constant-velocity-target MOT sensing-sim run (100 MHz / 273 PRB).
# Companion to _run_100mhz.sh/_run_sine.sh (same 100 MHz carrier + iperf-DL runner), swapped to
# ue.sensing.traffic.moving.mot2.rfsim.conf -- Phase 2 Part 1's first live MOT validation.
#
# Usage: sudo ./_run_mot.sh [out_dir] [duration_s]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

OUT=${1:-/tmp/sensing_mot2}
DUR=${2:-300}

sudo rm -rf "$OUT"; mkdir -p "$OUT/logs"
sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

c="_mot2.conf"
cp ue.sensing.traffic.moving.mot2.rfsim.conf "$c"
sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$OUT/oaiue_sensing\";#" "$c"
sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$OUT/oaiue_reports.jsonl\";#" "$c"

sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
  ./run_sim_traffic_iperf.sh "$DUR" "$OUT/logs" "$c" 3M
rm -f "$c"
