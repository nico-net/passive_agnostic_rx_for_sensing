#!/bin/bash
# Single-run helper for the "add mechanisms back one at a time" MOT precision investigation.
# Usage: sudo ./_run_mot_variant.sh <conf_file> <out_dir> [duration_s]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

CONF=$1
OUT=$2
DUR=${3:-200}

sudo rm -rf "$OUT"; mkdir -p "$OUT/logs"
sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

c="_mot2_variant_run.conf"
cp "$CONF" "$c"
sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$OUT/oaiue_sensing\";#" "$c"
sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$OUT/oaiue_reports.jsonl\";#" "$c"

sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 UE_NB_ANT_RX="${UE_NB_ANT_RX:-1}" \
  ./run_sim_traffic_iperf.sh "$DUR" "$OUT/logs" "$c" 3M
rm -f "$c"

echo "=== SCORE: $OUT ==="
python3 score_run.py "$OUT"
