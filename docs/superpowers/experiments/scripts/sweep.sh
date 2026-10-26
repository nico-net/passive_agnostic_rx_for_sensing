#!/bin/bash
# gain sweep on the unchanged lab gNB: one lab_sens4_run.sh per gain, alternated
S=$(dirname "$0"); cd /home/sens/NICOLA/agn-wt/s6-ota || exit 1
for g in 49 42 36 30 30 36 42 49; do
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC address gone" >> $S/sweep.log; exit 3; }
  a=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
  [ "$a" -ge 7000 ] || { echo "$(date +%T) ABORT MemAvailable ${a} MB" >> $S/sweep.log; exit 4; }
  echo "$(date +%T) start g$g" >> $S/sweep.log
  RXG=$g tests/passive_rx/ota/lab_sens4_run.sh g$g > $S/g$g.$(date +%H%M).out 2>&1
  echo "$(date +%T) done g$g rc=$? $(grep -o '/home/sens/NICOLA/captures/g[0-9]*_lab_[0-9_]*' $S/g$g.*.out | tail -1)" >> $S/sweep.log
  grep -q ABORT $S/g$g.*.out && { echo "$(date +%T) ABORT in g$g, stopping" >> $S/sweep.log; exit 5; }
  sleep 20
done
echo "$(date +%T) sweep finished" >> $S/sweep.log
