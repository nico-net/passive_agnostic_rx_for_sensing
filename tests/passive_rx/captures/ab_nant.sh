#!/bin/bash
# 1-RX vs 4-RX, alternated rep-by-rep so machine/cell drift is shared between arms.
set -u
cd /home/sens/NICOLA/captures
for rep in 1 2 3; do
  for n in 1 4; do
    ARM=nant${n}_r${rep} CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
      DUR=200 TRIES=3 RXG=25 NANT=$n ULPROBE=1 ./run_arm.sh
  done
done
echo "=== AB DONE ==="
