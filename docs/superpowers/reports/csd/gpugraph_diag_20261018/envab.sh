#!/bin/bash
# env-only lever A/B (diagnosis 2026-10-18): arms interleaved, n per capture; each replay is its own heavy call via p99_replay.sh
set -u
W=/home/nicola/NICOLA/wt/rr-csd-int16gpu; S=$W/tests/passive_rx; E=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26gE.env
IDX=${IDX:-/tmp/claude-1001/-home-nicola-NICOLA-passive-agnostic-rx-for-sensing/8b5f4a61-d0d8-4c81-a9a6-30286599ce28/scratchpad/diag/envab.idx}
ON="ISAC_PDSCH_GPU_GRAPH=1 ISAC_GPU_PASS_RETRY_OLD=1 ISAC_GPU_PASS_POLL_SLEEP_US=600 ISAC_PDSCH_GPU_GRAPH_COVER=1 ISAC_PDSCH_GPU_GRAPH_SPLIT=1 ISAC_PDSCH_GPU_GRAPH_RETRY=1 ISAC_PDSCH_GPU_GRAPH_DMRS2=1"
declare -A A=( [OFF]="" [ON]="$ON" [SPIN]="$ON ISAC_GPU_PASS_POLL_SLEEP_US=100000" [FP32]="$ON ISAC_GPU_PASS_FP32=1" [PRIO]="$ON ISAC_PDSCH_GPU_PRIO=1" [PR3]="$ON ISAC_PDSCH_GPU_GRAPH_PROBE_RESERVE=3" )
for i in $(seq 1 ${N:-3}); do for cap in ${CAPS:-b tim}; do for arm in ${ARMS:-OFF ON SPIN FP32 PRIO PR3}; do
  for try in $(seq 1 30); do
    CAPSET=$cap ENVFILE=$E TMO=90 "$S/p99_replay.sh" "dg${TAG:-}_${cap}_${arm}_$i" ${A[$arm]} < /dev/null > /dev/null 2>&1
    d=$(cat "$W/replays/last")
    [ -e "$d/rx_busy" ] || break
    rm -rf "$d"; sleep 60
  done
  echo "$cap $arm $i $d" >> "$IDX"
done; done; done
echo DONE >> "$IDX"
