#!/bin/bash
# 2x2 ablation (sync on/off x range_window hann/chebyshev), 5 repeats/cell, cpi_slots=128, 120s/run.
# Generated for the 2026-07-23 re-validation follow-up. Not a permanent repo script.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

OUT_ROOT=/tmp/sensing_128
mkdir -p "$OUT_ROOT"

DURATION=120
BW=3M
RUNS="1 2 3 4 5"

run_cell() {
  local sync_val="$1" win_val="$2" cell="$3" base_conf="$4"
  for run in $RUNS; do
    local run_dir="$OUT_ROOT/$cell/run$run"
    rm -rf "$run_dir"
    mkdir -p "$run_dir/logs"
    local conf_name="_gen_${cell}_run${run}.rfsim.conf"
    cp "$base_conf" "$conf_name"
    sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$conf_name"
    sed -i "s/sync_correction *= *[0-9]*;/sync_correction = ${sync_val};/" "$conf_name"
    sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"${run_dir}/oaiue_sensing\";#" "$conf_name"
    sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"${run_dir}/oaiue_reports.jsonl\";#" "$conf_name"
    if [ "$win_val" = "chebyshev" ]; then
      if grep -q "range_window" "$conf_name"; then
        sed -i "s/range_window *= *\"[a-z]*\";/range_window = \"chebyshev\";/" "$conf_name"
      else
        sed -i "/out_path/i\\  range_window = \"chebyshev\";" "$conf_name"
      fi
    fi

    sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log

    echo "=== [$(date +%H:%M:%S)] cell=$cell run=$run sync=$sync_val window=$win_val ==="
    sudo ./run_sim_traffic_iperf.sh "$DURATION" "$run_dir/logs" "$conf_name" "$BW"
    status=$?
    if [ $status -ne 0 ]; then
      echo "!!! cell=$cell run=$run FAILED (exit $status)"
    fi
    rm -f "$conf_name"
  done
}

run_cell 1 hann      on_hann   ue.sensing.traffic.moving.eca_walk.rfsim.conf
run_cell 1 chebyshev on_cheb   ue.sensing.traffic.moving.eca_walk.rfsim.conf
run_cell 0 hann      off_hann  ue.sensing.traffic.moving.eca_walk_nosync.rfsim.conf
run_cell 0 chebyshev off_cheb  ue.sensing.traffic.moving.eca_walk_nosync.rfsim.conf

echo "=== ALL 20 RUNS DONE ==="
