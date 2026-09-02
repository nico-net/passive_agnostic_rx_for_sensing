#!/bin/bash
# Continuous digital CFO compensation ON vs OFF, alternated rep-by-rep so cell/machine drift is
# shared. TRIES=1: a mis-lock is the thing under test, so retrying would hide the effect.
set -u
cd /home/sens/NICOLA/captures
for rep in 1 2 3; do
  for cf in 0 1; do
    if [ "$cf" = 1 ]; then export CONTFO=1; else unset CONTFO; fi
    ARM=cfo${cf}_r${rep} CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
      DUR=200 TRIES=1 RXG=25 NANT=4 ULPROBE=1 ./run_arm.sh
  done
done
echo "=== CFO AB DONE ==="
