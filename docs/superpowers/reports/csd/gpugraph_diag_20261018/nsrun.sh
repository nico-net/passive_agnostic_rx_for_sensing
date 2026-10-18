#!/bin/bash
# instrumented (DIAGTMP NVTX) nsys replays: OFF / ON (+ extra arms), n per capture
set -u
W=/home/nicola/NICOLA/wt/rr-csd-int16gpu; S=$W/tests/passive_rx; E=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26gE.env
IDX=${IDX:-/tmp/claude-1001/-home-nicola-NICOLA-passive-agnostic-rx-for-sensing/8b5f4a61-d0d8-4c81-a9a6-30286599ce28/scratchpad/diag/ns.idx}
ON="ISAC_PDSCH_GPU_GRAPH=1 ISAC_GPU_PASS_RETRY_OLD=1 ISAC_GPU_PASS_POLL_SLEEP_US=600 ISAC_PDSCH_GPU_GRAPH_COVER=1 ISAC_PDSCH_GPU_GRAPH_SPLIT=1 ISAC_PDSCH_GPU_GRAPH_RETRY=1 ISAC_PDSCH_GPU_GRAPH_DMRS2=1"
declare -A A=( [OFF]="" [ON]="$ON" [FP32]="$ON ISAC_GPU_PASS_FP32=1" )
WR="/usr/local/cuda/bin/nsys profile -t cuda,nvtx -s none --cuda-graph-trace=node --gpu-metrics-devices=0 --gpu-metrics-frequency=10000 --kill=2 --export=sqlite -d 80 -o nsys_trace"
for i in $(seq 1 ${N:-2}); do for cap in ${CAPS:-b tim}; do for arm in ${ARMS:-OFF ON}; do
  for try in $(seq 1 30); do
    WRAP="$WR" CAPSET=$cap ENVFILE=$E TMO=150 "$S/p99_replay.sh" "dgns_${cap}_${arm}_$i" ${A[$arm]} < /dev/null > /dev/null 2>&1
    d=$(cat "$W/replays/last")
    [ -e "$d/rx_busy" ] || break
    rm -rf "$d"; sleep 60
  done
  echo "$cap $arm $i $d" >> "$IDX"
done; done; done
echo DONE >> "$IDX"
