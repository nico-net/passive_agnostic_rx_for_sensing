#!/bin/bash
# P99 replay driver: interleaved arms, one replay at a time (p99_replay.sh takes heavy.lock), quiet-host record per run.
S=/home/nicola/NICOLA/wt/rr-csd-p99/tests/passive_rx/p99_replay.sh
LOG=/tmp/claude-1001/-home-nicola-NICOLA-passive-agnostic-rx-for-sensing/8b5f4a61-d0d8-4c81-a9a6-30286599ce28/scratchpad/p99/runs.txt
Y="ISAC_PDSCH_RETRY_YIELD=2"; F="ISAC_SCANQ_FRESH=6"
C="ISAC_PDSCH_GPU_EXACT_CHEST_CHECK=1024 ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=1024 ISAC_CHEST_GPU_DELAY_CHECK=1024"
run() { # cap name extra...
  local cap=$1 name=$2; shift 2
  local rc=99 pre
  while [ $rc -eq 99 ]; do
    while pgrep -x nr-uesoftmodem >/dev/null; do sleep 10; done
    pre="$(date +%T) load=$(cut -d' ' -f1 /proc/loadavg) hogs=$(ps -eo pcpu=,comm= --sort=-pcpu | awk '$1>50{printf "%s:%s,",$2,$1}')"
    CAPSET=$cap TMO=${TMO:-90} $S $name "$@" > /dev/null 2>&1; rc=$?
    [ $rc -eq 99 ] && rm -rf "$(cat /home/nicola/NICOLA/wt/rr-csd-p99/replays/last)" && sleep 20
  done
  echo "$name cap=$cap rc=$rc pre[$pre] post=$(date +%T) dir=$(cat /home/nicola/NICOLA/wt/rr-csd-p99/replays/last) extra=$*" >> $LOG
}
PP="ISAC_GPU_POLL_SLEEP_US=200"
for r in ${ROUNDS:-1 2 3}; do
  run b bA$r; run b bP$r $PP; run b bPY$r $PP $Y; run b bPF$r $PP $F; run b bPYF$r $PP $Y $F
  run tim tA$r; run tim tP$r $PP; run tim tPYF$r $PP $Y $F
done
