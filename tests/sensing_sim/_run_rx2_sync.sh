#!/bin/bash
# rx2 runner for SIMULTANEOUS multi-static capture: runs from its own directory (symlinked configs)
# so relative-path stats files don't collide with the rx1 instance running from tests/sensing_sim/,
# and uses the ns2 netns pair (oai_isac_gnb3/ue3) so netns/veth names don't collide either.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

CONF=$1
OUT=$2
DUR=${3:-200}

sudo rm -rf "$OUT"; mkdir -p "$OUT/logs"
sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

c="_mot2_rx2_sync_run.conf"
cp "$CONF" "$c"
sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$OUT/oaiue_sensing\";#" "$c"
sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$OUT/oaiue_reports.jsonl\";#" "$c"

sudo env BUILD_DIR=/home/sens/NICOLA/openairinterface5g/cmake_targets/ran_build/build \
  GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
  ./run_sim_traffic_iperf_ns2.sh "$DUR" "$OUT/logs" "$c" 3M
rm -f "$c"
