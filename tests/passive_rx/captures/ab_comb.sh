#!/bin/bash
# ONE loop at a time. SENSECOMB 0 vs 1, alternated. Everything else identical.
set -u
cd /home/sens/NICOLA/captures
for rep in 1 2; do
  for c in 0 1; do
    SENSECOMB=$c CONTFO=1 GAINTRIM="0,16.5,3.5,5.6" ARM=sc${c}_r${rep} \
      CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
      DUR=200 TRIES=2 RXG=25 NANT=4 ULPROBE=1 ./run_arm.sh
  done
done
echo "=== SC AB DONE ==="
