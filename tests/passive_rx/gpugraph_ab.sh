#!/bin/bash
# GPUGRAPH replay A/B (plan 2026-10-17-dl-gpu-datapath): OFF = t26gE, PREV, ON; n=3 interleaved per capture; captures never pooled.
# usage: gpugraph_ab.sh <tag> "<PREV flags>" "<ON flags>"   (PREV "" = no PREV arm). Each replay takes heavy.lock via p99_replay.sh.
# A replay refused because another receiver was up (rx_busy) is retried after 60 s.
set -u
S=$(cd "$(dirname "$0")" && pwd); E=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26gE.env
tag=$1; prev=$2; on=$3
IDX="$S/../../replays/gg_${tag}.idx"
for i in 1 2 3; do for cap in b tim; do for arm in OFF PREV ON; do
  case $arm in OFF) X="";; PREV) X="$prev";; ON) X="$on";; esac
  [ "$arm" = PREV ] && [ -z "$prev" ] && continue
  for try in $(seq 1 30); do
    CAPSET=$cap ENVFILE=$E TMO=90 "$S/p99_replay.sh" "gg_${tag}_${cap}_${arm}_$i" $X < /dev/null
    d=$(cat "$S/../../replays/last")
    [ -e "$d/rx_busy" ] || break
    rm -rf "$d"; sleep 60
  done
  echo "$cap $arm $i $d" >> "$IDX"
done; done; done
while read cap arm i d; do
  echo "== $cap $arm $i $d"
  python3 "$S/goal_metrics.py" "$d/run.log" | head -1
  python3 "$S/gpugraph_tail.py" "$d/run.log"
  grep -a "GPUGRAPH " "$d/run.log" | tail -1
  grep -a -c "GPUPASS MISMATCH\|GPUGRAPH MISMATCH\|GPUPASS DISABLED\|GPUGRAPH DISABLED" "$d/run.log"
done < "$IDX"
