#!/bin/bash
# one air-like replay (capture b) with perf sched recorded for 20 s from its first PDTIM line; usage: perfrun.sh <unique name> [ENV=VAL ...]
P=/tmp/claude-1001/-home-nicola-NICOLA-passive-agnostic-rx-for-sensing/8b5f4a61-d0d8-4c81-a9a6-30286599ce28/scratchpad/p99
cd $P; name=$1
( ./one.sh b "$@" ) &
DRV=$!
R=""
until [ -n "$R" ] && grep -q 'PDTIM calls=' $R/run.log 2>/dev/null; do
  kill -0 $DRV 2>/dev/null || { echo "driver ended before PDTIM"; exit 1; }
  R=$(ls -d /home/nicola/NICOLA/wt/rr-csd-p99/replays/replay_${name}_* 2>/dev/null | tail -1); sleep 0.5
done
sudo -n perf sched record -a -o $P/perf.$name.data -- sleep 20 > $P/perf.$name.log 2>&1
wait $DRV
echo "$R"
