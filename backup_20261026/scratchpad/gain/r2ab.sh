#!/bin/bash
S=$(dirname "$0"); LOG=$S/r2ab.log; Y=/home/sens/NICOLA/gnbLogs/armcfg/gnb_D2.yml
for arm in r2dl1 r2in1 r2dl2 r2in2; do
  case $arm in r2dl*) W=/home/sens/NICOLA/agn-wt/hmcs; B=$W/build_hmcs;; *) W=/home/sens/NICOLA/agn-wt/ul-fit; B=$W/build_ulfit;; esac
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC" >> $LOG; exit 3; }
  echo "$(date +%T) start $arm" >> $LOG; o=$S/$arm.$(date +%H%M).out
  (cd $W && RXG=36 B=$B GNB_CMD="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c $Y" tests/passive_rx/ota/lab_sens4_run.sh $arm > $o 2>&1)
  echo "$(date +%T) done $arm $(grep -o "/home/sens/NICOLA/captures/${arm}_lab_[0-9_]*" $o | tail -1)" >> $LOG
  grep -qE "ABORT" $o && { echo "$(date +%T) ABORT in $arm" >> $LOG; exit 5; }
  sleep 20
done
echo "$(date +%T) r2ab finished" >> $LOG
