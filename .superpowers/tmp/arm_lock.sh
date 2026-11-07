#!/bin/bash
# arm_lock.sh <name> <build> <speed>: one replay arm under the heavy lock then td_measure lock (as sp_ota_q1.sh)
exec 9>/home/nicola/NICOLA/heavy.lock; flock -x -w 7200 9 || exit 1; export HEAVY_HELD=1
exec 8>/tmp/td_measure.lock; flock -x -w 7200 8 || exit 1
pgrep -x nr-uesoftmodem >/dev/null && { echo "receiver running"; exit 2; }
exec /home/nicola/NICOLA/wt/rr-single1d/.superpowers/tmp/ab_arm.sh "$@"
