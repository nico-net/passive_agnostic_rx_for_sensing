#!/bin/bash
# rx3 runner for SIMULTANEOUS multi-static capture -- direct analog of _run_rx2_sync.sh (see that
# file's header for the "own directory + own netns pair" rationale), using the THIRD netns set
# (run_sim_traffic_iperf_ns3.sh -> oai_isac_gnb4/ue4, veth-isac-g4/-u3, 192.168.102.0/24) so all
# three receivers can run at once without netns/veth/stats-file collisions.
#
# RECREATED 2026-07-28: this script previously existed ONLY inside the ad-hoc /tmp/ms/rx3_run
# staging directory (never committed), and was lost when /tmp was cleaned. Restored here, in-tree,
# so the 3-receiver captures (_run_aoa_vs_3rx.sh, _run_scene_validation.sh, _run_crossing_check.sh)
# are reproducible from a clean /tmp. Mirrors _run_rx2_sync.sh line-for-line apart from the ns3
# runner and the temp-conf name.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

CONF=$1
OUT=$2
DUR=${3:-200}

sudo rm -rf "$OUT"; mkdir -p "$OUT/logs"
sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

c="_mot2_rx3_sync_run.conf"
cp "$CONF" "$c"
sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$OUT/oaiue_sensing\";#" "$c"
sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$OUT/oaiue_reports.jsonl\";#" "$c"

# BUILD_DIR defaults to THIS SCRIPT'S OWN worktree, resolved relatively. It used to be an absolute
# path, which silently ran rx2/rx3 from a different git worktree (and therefore a different branch)
# than rx1 -- so in a multi-receiver capture the receivers could disagree about which report fields
# exist at all (p_real / p_detect), a silent-garbage failure of the same class as a wrong
# csirs_monitor scramb_id. A relative default cannot drift that way when the file is on two branches.
DEFAULT_BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../cmake_targets/ran_build/build" 2>/dev/null && pwd)"
sudo env BUILD_DIR="${BUILD_DIR:-$DEFAULT_BUILD_DIR}" UE_NB_ANT_RX="${UE_NB_ANT_RX:-1}" \
  GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
  ./run_sim_traffic_iperf_ns3.sh "$DUR" "$OUT/logs" "$c" 3M
rm -f "$c"
