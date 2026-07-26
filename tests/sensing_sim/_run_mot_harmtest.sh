#!/bin/bash
# A/B batch: track_harmonic_reject on vs off, N repeats each, everything else identical.
# Motivation (2026-07-24): a single on/off comparison gave a counterintuitive result (60 vs 39
# unique tracks) that contradicts the fact that track_harmonic_reject is a pure post-filter on the
# reported track list and cannot increase the number of distinct ids ever created -- almost
# certainly run-to-run scheduling/timing noise in this live netns/iperf harness (repeated baseline
# runs earlier swung 78-90 with NO config change at all). This runs a real batch instead of trusting
# two single noisy runs.
#
# Usage: sudo ./_run_mot_harmtest.sh [repeats] [duration_s] [out_root]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

REPEATS=${1:-3}
DUR=${2:-300}
OUT_ROOT=${3:-/tmp/mot_harmtest}

run_one() {
  local reject=$1
  local out=$2
  sudo rm -rf "$out"; mkdir -p "$out/logs"
  sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

  local c
  c=$(mktemp -p . _mot_harmtest_XXXX.conf)
  cp ue.sensing.traffic.moving.mot2.rfsim.conf "$c"
  sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
  sed -i "s/track_harmonic_reject *= *[0-9]*;/track_harmonic_reject     = $reject;/" "$c"
  sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"$out/oaiue_sensing\";#" "$c"
  sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"$out/oaiue_reports.jsonl\";#" "$c"

  sudo env GNB_CONF=gnb.sensing.100mhz.rfsim.conf UE_PRB=273 UE_CFREQ=3750000000 \
    ./run_sim_traffic_iperf.sh "$DUR" "$out/logs" "$c" 3M > "$out/runner.log" 2>&1
  rm -f "$c"

  local n_ids
  n_ids=$(grep -oP "track_id=\K[0-9]+" "$out/logs/ue.log" 2>/dev/null | sort -u | wc -l)
  echo "reject=$reject out=$out unique_track_ids=$n_ids"
}

echo "=== A/B batch: $REPEATS repeats each, ${DUR}s per run ==="
for i in $(seq 1 "$REPEATS"); do
  run_one 0 "$OUT_ROOT/off_$i"
done
for i in $(seq 1 "$REPEATS"); do
  run_one 1 "$OUT_ROOT/on_$i"
done
echo "=== batch done ==="
