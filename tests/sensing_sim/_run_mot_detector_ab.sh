#!/bin/bash
# Lean matched_filter-vs-fft A/B on the minimal-config 100%-coverage MOT baseline.
# Runs both detectors back-to-back (sequential -- fixed netns names) on the identical
# corrected-geometry two-target scene, then scores each with score_run.py.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

DUR=${1:-300}

run_one () {
  local conf=$1 out=$2
  sudo rm -rf "$out"; mkdir -p "$out/logs"
  sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log
  local c="_mot2_ab_run.conf"
  cp "$conf" "$c"
  sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
  sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$out/oaiue_sensing\";#" "$c"
  sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$out/oaiue_reports.jsonl\";#" "$c"
  sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
    ./run_sim_traffic_iperf.sh "$DUR" "$out/logs" "$c" 3M
  rm -f "$c"
}

echo "=== FFT run ==="
run_one _mot2_fft.conf /tmp/sensing_mot2_fft
echo "=== matched_filter run ==="
run_one _mot2_mf.conf /tmp/sensing_mot2_mf

echo "=== SCORE: fft ==="
python3 score_run.py /tmp/sensing_mot2_fft
echo "=== SCORE: matched_filter ==="
python3 score_run.py /tmp/sensing_mot2_mf
