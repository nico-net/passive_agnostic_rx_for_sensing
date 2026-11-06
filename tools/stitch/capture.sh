#!/bin/bash
# capture.sh: 3 x DUR s single-PCI sensing captures with CFR frames per cell (415 Vodafone, 923 TIM). Derived from .superpowers/tmp/sp_ota_s1.sh (same tune/gain/env, fix build = this worktree).
set -u
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
unset ISAC_RX_BRANCH_FO
O=/home/nicola/NICOLA/wt/rr-orchestration; D=$O/ota; FIXW=/home/nicola/NICOLA/wt/rr-single1
ENVF=$D/MPCI_20261025.env; DUR=${DUR:-120}; SETTLE=${SETTLE:-45}
V=3680010000:1171:-4827:217; T=3759990000:233:-4832:217
B=$FIXW/cmake_targets/ran_build/build; CFG=$FIXW/tests/passive_rx/sensing/sensing_stitch.dgx.cfg
OUT=/home/nicola/NICOLA/ota_runs; say() { echo "$(date +%H:%M:%S) $*"; }
[ -e $O/X410_UNAVAILABLE ] && { say "X410_UNAVAILABLE"; exit 1; }
exec 9>/home/nicola/NICOLA/heavy.lock; flock -x -w 3600 9 || exit 1; export HEAVY_HELD=1; exec 8>/tmp/td_measure.lock; flock -x -w 3600 8 || exit 1
for c in "415 $V" "923 $T"; do set -- $c; p=$1; IFS=: read -r f ssb fo rb <<< "$2"
  for n in 1 2 3; do tag=stch_${p}_$n
    pgrep -x nr-uesoftmodem >/dev/null && { say "receiver running, waiting"; while pgrep -x nr-uesoftmodem >/dev/null; do sleep 30; done; }
    rm -f $OUT/sense_records_last*.jsonl $OUT/sense_cfr_last*.bin; prev=$(cat $D/lastrun 2>/dev/null)
    BDIR=$B FREQ=$f SSBOFF=$ssb IFO=$fo RB=$rb DUR=$DUR CFG=$CFG $D/starttest_auto.sh $tag 1 65536 $(head -1 $ENVF) ISAC_SIB1_CACHE=0
    r=$(cat $D/lastrun); [ "$r" = "$prev" ] && { say "$tag NOT STARTED"; continue; }
    mv $OUT/sense_records_last*.jsonl $OUT/sense_cfr_last*.bin $r/ 2>/dev/null
    say "$tag -> $r $(grep -o 'Initial sync successful, PCI: [0-9]*' $r/run.log | sort -u | tr '\n' ' ') $(cat $r/rx_exit.txt)"; sleep $SETTLE
  done
done
