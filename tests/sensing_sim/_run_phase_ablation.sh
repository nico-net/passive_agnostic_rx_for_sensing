#!/bin/bash
# Per-phase sync ablation: which of Phase 1 (STO) / 2 (CFO) / 3 (SFO) / 4 (LOS loop) costs detection?
# 2 repeats x 6 configs, cpi_slots=128, 120s each. Generated 2026-07-23; not a permanent repo script.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

OUT_ROOT=/tmp/sensing_phase
DURATION=120
BW=3M
BASE=ue.sensing.traffic.moving.eca_walk.rfsim.conf

# name         sto cfo sfo los
CONFIGS="
all_off:0:0:0:0
all_on:1:1:1:1
sto_only:1:0:0:0
cfo_only:0:1:0:0
sfo_only:0:0:1:0
los_only:0:0:0:1
"

for spec in $CONFIGS; do
  name=$(echo "$spec" | cut -d: -f1)
  sto=$(echo "$spec" | cut -d: -f2); cfo=$(echo "$spec" | cut -d: -f3)
  sfo=$(echo "$spec" | cut -d: -f4); los=$(echo "$spec" | cut -d: -f5)
  for run in 1 2; do
    d="$OUT_ROOT/$name/run$run"
    rm -rf "$d"; mkdir -p "$d/logs"
    c="_gen_ph_${name}_${run}.conf"
    cp "$BASE" "$c"
    sed -i "s/cpi_slots *= *[0-9]*;/cpi_slots       = 128;/" "$c"
    sed -i "s/sync_correction *= *[0-9]*;/sync_correction = 1;/" "$c"
    sed -i "s#out_path *= *\"[^\"]*\";#out_path    = \"${d}/oaiue_sensing\";#" "$c"
    sed -i "s#report_path *= *\"[^\"]*\";#report_path = \"${d}/oaiue_reports.jsonl\";#" "$c"
    sed -i "/out_path/i\\  sync_sto = ${sto};\n  sync_cfo = ${cfo};\n  sync_sfo = ${sfo};\n  sync_los = ${los};" "$c"
    sudo rm -f reconfig.raw rbconfig.raw nrL1_stats.log nrMAC_stats.log nrL1_UE_stats-0.log nrRRC_stats.log
    echo "=== [$(date +%H:%M:%S)] $name run=$run (sto=$sto cfo=$cfo sfo=$sfo los=$los) ==="
    sudo ./run_sim_traffic_iperf.sh "$DURATION" "$d/logs" "$c" "$BW" >/dev/null 2>&1 || echo "!!! $name/$run FAILED"
    rm -f "$c"
  done
done
echo "=== PHASE ABLATION DONE ==="
