#!/bin/bash
# integration series: UL A/B (base disc-dedup-2 vs integ), then DL rank/MCS arms on integ; gain 36
S=$(dirname "$0"); LOG=$S/integ2.log
GB="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c"
run() { # arm worktree builddir gnb_yml
  local arm=$1 wt=$2 b=$3 yml=$4
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC address gone" >> $LOG; exit 3; }
  a=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo); [ "$a" -ge 7000 ] || { echo "$(date +%T) ABORT MemAvailable $a" >> $LOG; exit 4; }
  echo "$(date +%T) start $arm" >> $LOG; o=$S/$arm.$(date +%H%M).out
  (cd $wt && RXG=36 B=$b GNB_CMD="$GB $yml" tests/passive_rx/ota/lab_sens4_run.sh $arm > $o 2>&1)
  echo "$(date +%T) done $arm $(grep -o "/home/sens/NICOLA/captures/${arm}_lab_[0-9_]*" $o | tail -1)" >> $LOG
  grep -qE "ABORT|launcher rc=3" $o && { echo "$(date +%T) ABORT in $arm" >> $LOG; exit 5; }
  sleep 20; }
BASE=/home/sens/ABS/AgenticBS/logs/configs/gnb_ofh_ota_tdd30_100mhz_liteon.1e3123cc87ac.yml; A=/home/sens/NICOLA/gnbLogs/armcfg
D2W=/home/sens/NICOLA/agn-wt/disc-dedup2; IW=/home/sens/NICOLA/agn-wt/integ1025

run ulintb1  $IW  $IW/build_int  $BASE

run ulintb2  $IW  $IW/build_int  $BASE
for x in D1 D1M D2 D2M; do run arm$x $IW $IW/build_int $A/gnb_$x.yml; done
echo "$(date +%T) integ2 finished" >> $LOG
