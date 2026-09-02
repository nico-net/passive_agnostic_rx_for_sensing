#!/bin/bash
# CONTROL: current binary, ALL of my flags unset. If DL comes back near run-c 67%, one of the flags
# is the cost; if it sits near the sweep 16%, the change is traffic/hardware, not mine.
set -u
cd /home/sens/NICOLA/captures
for rep in 1 2 3; do
  ARM=ctl_r${rep} CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
    DUR=200 TRIES=2 RXG=25 NANT=4 ULPROBE=1 ./run_arm.sh
done
echo "=== CTL DONE ==="
