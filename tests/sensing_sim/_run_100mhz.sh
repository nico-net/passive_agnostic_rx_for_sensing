#!/bin/bash
# 100 MHz (273 PRB) sensing-sim probe: verifies RA/traffic still come up on the wider carrier and
# that the sensing grid reports range_res ~3.05 m. Temp helper, 2026-07-23.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

OUT=${1:-/tmp/sensing_100}
DUR=${2:-150}
sudo rm -rf "$OUT"; mkdir -p "$OUT/logs"
sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

c=_100.conf
cp ue.sensing.traffic.moving.eca_walk.rfsim.conf "$c"
sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$OUT/oaiue_sensing\";#" "$c"
sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$OUT/oaiue_reports.jsonl\";#" "$c"

sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
  ./run_sim_traffic_iperf.sh "$DUR" "$OUT/logs" "$c" 3M
rm -f "$c"
