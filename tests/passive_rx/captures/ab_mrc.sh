#!/bin/bash
# Receive-branch combining sweep. Modes: 0 = branch 0 only (current default, and the reason the DL
# died when reseating the antennas made branch 0 the WEAK one), 1 = best single branch,
# 3 = MRC over live branches. Re-testable only now that the branches are balanced.
set -u
cd /home/sens/NICOLA/captures
for rep in 1 2; do
  for m in 0 1 3; do
    MRC=$m CONTFO=1 GAINTRIM="0,16.5,3.5,5.6" ARM=mrc${m}_r${rep} \
      CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
      DUR=200 TRIES=2 RXG=25 NANT=4 ULPROBE=1 ./run_arm.sh
  done
done
echo "=== MRC AB DONE ==="
