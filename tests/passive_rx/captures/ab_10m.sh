#!/bin/bash
# 10 Mbps iperf, ZeroMQ bus now compiled in. Working config (CONTFO=1 + gain trim), MRC 0 vs 3
# alternated so cell drift is shared. 3 reps: this rig swings 0-31% run to run.
set -u
cd /home/sens/NICOLA/captures
for rep in 1 2 3; do
  for m in 0 3; do
    MRC=$m CONTFO=1 GAINTRIM="0,16.5,3.5,5.6" ARM=m10_${m}_r${rep} \
      CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
      DUR=200 TRIES=2 RXG=25 NANT=4 ULPROBE=1 ./run_arm.sh
  done
done
echo "=== 10M AB DONE ==="
