#!/bin/bash
S=$(dirname "$0"); LOG=$S/dlval.log; W=/home/sens/NICOLA/agn-wt/s6-ota
for x in D1 D2; do
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC" >> $LOG; exit 3; }
  echo "$(date +%T) start val$x" >> $LOG; o=$S/val$x.$(date +%H%M).out
  (cd $W && RXG=36 B=$W/build_ota GNB_CMD="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c /home/sens/NICOLA/gnbLogs/armcfg/gnb_$x.yml" tests/passive_rx/ota/lab_sens4_run.sh val$x > $o 2>&1)
  echo "$(date +%T) done val$x $(grep -o "/home/sens/NICOLA/captures/val${x}_lab_[0-9_]*" $o | tail -1)" >> $LOG
  grep -qE "ABORT|launcher rc=3" $o && { echo "$(date +%T) ABORT in val$x" >> $LOG; exit 5; }
  sleep 20
done
echo "$(date +%T) dlval finished" >> $LOG
