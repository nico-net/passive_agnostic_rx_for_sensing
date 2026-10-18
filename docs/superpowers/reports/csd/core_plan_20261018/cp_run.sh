#!/bin/bash
# Core-plan replay gate (2026-10-18). usage: cp_run.sh <tag> <idx> <spec...>, spec = cap:arm:i (cap b|tim, arm L|K|K2).
# Every arm = t26uL.env (whole, via p99_replay.sh) + FLAGS_<arm>. One p99_replay.sh (= one heavy.sh job) per run.
set -u
R=/home/nicola/NICOLA/wt/rr-csd-int16gpu; S=$R/tests/passive_rx; E=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26uL.env
export B=$R/cmake_targets/ran_build/build_t26 CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
FLAGS_L=""
FLAGS_K="ISAC_FLOAT_CORES=10-14 ISAC_PUSCH_CORES=8,9 ISAC_UL_SNAP_CORE=8 ISAC_DMRS_SCAN_CORE=-1 ISAC_GPU_CHECK_CORES=10-14"
FLAGS_K2="$FLAGS_K ISAC_PDSCH_CORES=7,8,9,13,14"
tag=$1; IDX=$2; shift 2
for spec in "$@"; do IFS=: read cap arm i <<< "$spec"
  v=FLAGS_$arm
  for try in $(seq 1 30); do
    taskset -c 0-4 nice -n 10 python3 "$S/sysan_threads.py" "$B/" "$R/replays/samp_${tag}_${cap}_${arm}_$i.json" 45 35 > "$R/replays/samp_${tag}_${cap}_${arm}_$i.txt" 2>&1 &
    SP=$!
    CAPSET=$cap ENVFILE=$E TMO=90 "$S/p99_replay.sh" "${tag}_${cap}_${arm}_$i" ${!v} < /dev/null > /dev/null 2>&1
    kill -INT $SP 2>/dev/null; wait $SP 2>/dev/null
    d=$(ls -d $R/replays/replay_${tag}_${cap}_${arm}_${i}_* | tail -1); [ -e "$d/rx_busy" ] || break; rm -rf "$d"; sleep 60
  done; echo "$cap $arm $i $d" >> $IDX
done
echo DONE >> $IDX
