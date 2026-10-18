#!/bin/bash
# usage: a2e_run.sh <tag> <idx> <spec...>  where spec = cap:arm:i ; arm ON adds $ONFLAGS. All arms add ISAC_A2E_TRACE=1.
set -u
R=/home/nicola/NICOLA/wt/rr-csd-int16kf; S=$R/tests/passive_rx; E=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26gE.env
export B=$R/cmake_targets/ran_build/build_t26 CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
tag=$1; IDX=$2; shift 2
for spec in "$@"; do IFS=: read cap arm i <<< "$spec"
  extra="ISAC_A2E_TRACE=1"; [ "$arm" = ON ] && extra="$extra ${ONFLAGS:-}"; v=FLAGS_$arm; [ -n "${!v:-}" ] && extra="$extra ${!v}"
  for try in $(seq 1 30); do
    CAPSET=$cap ENVFILE=$E TMO=90 "$S/p99_replay.sh" "${tag}_${cap}_${arm}_$i" $extra < /dev/null > /dev/null 2>&1
    d=$(ls -d $R/replays/replay_${tag}_${cap}_${arm}_${i}_* | tail -1); [ -e "$d/rx_busy" ] || break; rm -rf "$d"; sleep 60
  done; echo "$cap $arm $i $d" >> $IDX
done
echo DONE >> $IDX
