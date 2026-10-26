#!/bin/bash
S=$(dirname "$0"); LOG=$S/hmval.log; W=/home/sens/NICOLA/agn-wt/hmcs; A=/home/sens/NICOLA/gnbLogs/armcfg
BASE=/home/sens/ABS/AgenticBS/logs/configs/gnb_ofh_ota_tdd30_100mhz_liteon.1e3123cc87ac.yml
for x in D1 D2 R4; do
  y=$A/gnb_$x.yml; [ $x = R4 ] && y=$BASE
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC" >> $LOG; exit 3; }
  echo "$(date +%T) start hm$x" >> $LOG; o=$S/hm$x.$(date +%H%M).out
  (cd $W && RXG=36 B=$W/build_hmcs GNB_CMD="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c $y" tests/passive_rx/ota/lab_sens4_run.sh hm$x > $o 2>&1)
  echo "$(date +%T) done hm$x $(grep -o "/home/sens/NICOLA/captures/hm${x}_lab_[0-9_]*" $o | tail -1)" >> $LOG
  grep -qE "ABORT|launcher rc=3" $o && { echo "$(date +%T) ABORT in hm$x" >> $LOG; exit 5; }
  sleep 20
done
echo "$(date +%T) hmval finished" >> $LOG
