#!/bin/bash
# Sinusoidal-trajectory sensing-sim run (100 MHz / 273 PRB), CV or CA tracker model.
# Companion to _run_100mhz.sh (same 100 MHz carrier + iperf-DL runner), swapped to
# ue.sensing.traffic.moving.eca_walk_sine.rfsim.conf and parameterized on track_model so the same
# script drives both halves of the CV-vs-CA A/B comparison. 2026-07-24, follow-up to the
# cpi_period_slots dt/velocity-axis fix in sensing_engine.cc -- see CLAUDE.md.
#
# Usage: sudo ./_run_sine.sh <cv|ca> [out_dir] [duration_s]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

MODEL=${1:-cv}
OUT=${2:-/tmp/sensing_sine_$MODEL}
DUR=${3:-300}

if [ "$MODEL" != "cv" ] && [ "$MODEL" != "ca" ]; then
  echo "ERROR: track model must be cv or ca (got '$MODEL')" >&2
  exit 1
fi

sudo rm -rf "$OUT"; mkdir -p "$OUT/logs"
sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

c="_sine_${MODEL}.conf"
cp ue.sensing.traffic.moving.eca_walk_sine.rfsim.conf "$c"
sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
sed -i "s/track_model *= *\"[a-z]*\";/track_model      = \"$MODEL\";/" "$c"
sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$OUT/oaiue_sensing\";#" "$c"
sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$OUT/oaiue_reports.jsonl\";#" "$c"

sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
  ./run_sim_traffic_iperf.sh "$DUR" "$OUT/logs" "$c" 3M
rm -f "$c"
