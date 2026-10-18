#!/bin/bash
# Equivalence replay (2026-10-18 cleanup). usage: eq_run.sh <tag> <idx> <rounds> [caps]   (caps default "b tim")
# per round and capture: OLD (build of csd/int16-int + t26yE.env, p99_replay.sh) then NEW (this worktree's build, no ISAC env, p99_replay_clean.sh).
# Both arms add ISAC_A2E_TRACE=1 (latency trace, diagnostic only). A run that overlaps another agent's `burst` GPU load is discarded and redone.
set -u
R=/home/nicola/NICOLA/wt/rr-csd-int16kf; S=$R/tests/passive_rx
OLDB=/home/nicola/NICOLA/wt/rr-csd-int16ulr/cmake_targets/ran_build/build_t26; NEWB=$R/cmake_targets/ran_build/build_t26
E=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26yE.env
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
tag=$1; IDX=$2; rounds=$3; caps=${4:-"b tim"}
one() { # cap arm i
  cap=$1; arm=$2; i=$3
  for try in $(seq 1 30); do
    flag=$(mktemp); ( while [ -e "$flag" ]; do pgrep -f "nr-uesoftmodem" > /dev/null && pgrep -x burst > /dev/null && echo burst >> "$flag.seen"; sleep 2; done ) &
    if [ $arm = OLD ]; then B=$OLDB CAPSET=$cap ENVFILE=$E TMO=90 "$S/p99_replay.sh" "${tag}_${cap}_${arm}_$i" ISAC_A2E_TRACE=1 < /dev/null > /dev/null 2>&1
    else B=$NEWB CAPSET=$cap TMO=90 "$S/p99_replay_clean.sh" "${tag}_${cap}_${arm}_$i" ISAC_A2E_TRACE=1 < /dev/null > /dev/null 2>&1; fi
    rm -f "$flag"; wait
    d=$(ls -d $R/replays/replay_${tag}_${cap}_${arm}_${i}_* | tail -1)
    if [ -e "$flag.seen" ]; then rm -f "$flag.seen"; echo "$cap $arm $i $d BURST-DISCARDED" >> $IDX.log; rm -rf "$d"; continue; fi
    [ -e "$d/rx_busy" ] || break; rm -rf "$d"; sleep 60
  done; echo "$cap $arm $i $d" >> $IDX
}
for i in $(seq 1 $rounds); do for cap in $caps; do one $cap OLD $i; one $cap NEW $i; done; done
echo DONE >> $IDX
