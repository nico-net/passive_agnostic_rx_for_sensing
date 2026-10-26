#!/bin/bash
S=$(dirname "$0"); LOG=$S/cb.log; BASE=/home/sens/ABS/AgenticBS/logs/configs/gnb_ofh_ota_tdd30_100mhz_liteon.1e3123cc87ac.yml; W=/home/sens/NICOLA/agn-wt/integ-frozen; B=$W/build_frozen
for arm in cbR4a cbD1; do
  Y=$BASE; case $arm in *D1) Y=/home/sens/NICOLA/gnbLogs/armcfg/gnb_D1.yml;; *D2) Y=/home/sens/NICOLA/gnbLogs/armcfg/gnb_D2.yml;; esac
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC" >> $LOG; exit 3; }
  echo "$(date +%T) start $arm" >> $LOG; o=$S/$arm.$(date +%H%M).out
  (cd $W && RXG=36 B=$B GNB_CMD="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c $Y" tests/passive_rx/ota/lab_sens4_run.sh $arm > $o 2>&1)
  echo "$(date +%T) done $arm $(grep -o "/home/sens/NICOLA/captures/${arm}_lab_[0-9_]*" $o | tail -1)" >> $LOG
  grep -qE "ABORT|launcher rc=3" $o && { echo "$(date +%T) ABORT in $arm" >> $LOG; exit 5; }
  sleep 20
done
echo "$(date +%T) cb finished" >> $LOG
