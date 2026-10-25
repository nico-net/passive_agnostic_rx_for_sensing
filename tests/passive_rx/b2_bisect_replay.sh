#!/bin/bash
# usage: S=<dir with var/{main,b2,nodisc,fix}> b2_bisect_replay.sh REPS arm...  arms: main b2 nodisc fix (rot cap phase n0 need the temporary B2_OFF toggles, commit-less, see docs/superpowers/reports/csd/B2-FIX-20261024.md)
S=${S:?scratch dir with var/<arm>/ build dirs}
W=/home/nicola/NICOLA/wt/rr-b2fix
reps=$1; shift
for r in $(seq 1 $reps); do for a in "$@"; do
  while [ ! -e /home/nicola/NICOLA/wt/rr-orchestration/ota/blind_capture_done ]; do sleep 60; done
  case $a in fix) B=$S/var/fix; off="";; main) B=$S/var/main; off="";; b2) B=$S/var/b2; off="";; nodisc) B=$S/var/nodisc; off="";; *) B=$S/var/b2; off=$a;; esac
  export B2_OFF=$off
  echo "$(date +%T) arm=$a rep=$r" >> $S/bisect.log
  CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900 RB=217 SPEED=0.25 DUR=150 B=$B \
    $W/tests/passive_rx/wl_replay.sh bs_${a}_$r < /dev/null > /dev/null 2>&1
  echo "$a $r $(cat $W/replays/last)" >> $S/bisect.runs
done; done
echo DONE >> $S/bisect.log
