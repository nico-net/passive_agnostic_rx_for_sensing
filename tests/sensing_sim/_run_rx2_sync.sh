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

# BUILD_DIR is OVERRIDABLE and defaults to the worktree this script was copied from. It used to be
# hardcoded to /home/sens/NICOLA/openairinterface5g, which silently ran rx2/rx3 from a DIFFERENT
# git worktree (and therefore a different branch) than rx1 -- so in a multi-receiver capture the
# receivers could disagree about which report fields exist at all (p_real / p_detect), which is a
# silent-garbage failure of the same class as a wrong csirs_monitor scramb_id.
sudo env BUILD_DIR="${BUILD_DIR:-/home/sens/NICOLA/openairinterface5g-active-ue/cmake_targets/ran_build/build}" UE_NB_ANT_RX="${UE_NB_ANT_RX:-1}" \
  GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
  ./run_sim_traffic_iperf_ns2.sh "$DUR" "$OUT/logs" "$c" 3M
rm -f "$c"
