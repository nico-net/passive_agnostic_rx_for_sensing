#!/bin/bash
# discovery-fix validation: 6 runs, gain 36, disc-dedup binary
S=$(dirname "$0"); cd /home/sens/NICOLA/agn-wt/disc-dedup || exit 1
for i in 15 16 17 18; do
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC address gone" >> $S/disc.log; exit 3; }
  a=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo); [ "$a" -ge 7000 ] || { echo "$(date +%T) ABORT MemAvailable $a" >> $S/disc.log; exit 4; }
  echo "$(date +%T) start dd$i" >> $S/disc.log
  o=$S/dd$i.$(date +%H%M).out
  RXG=36 B=/home/sens/NICOLA/agn-wt/disc-dedup/build_dd tests/passive_rx/ota/lab_sens4_run.sh dd$i > $o 2>&1
  echo "$(date +%T) done dd$i $(grep -o '/home/sens/NICOLA/captures/dd[0-9]*_lab_[0-9_]*' $o | tail -1)" >> $S/disc.log
  grep -qE "ABORT|launcher rc=3" $o && { echo "$(date +%T) ABORT in dd$i" >> $S/disc.log; exit 5; }
  sleep 20
done
echo "$(date +%T) disc finished" >> $S/disc.log
