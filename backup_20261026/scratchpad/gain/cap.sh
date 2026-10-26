#!/bin/bash
S=$(dirname "$0"); LOG=$S/cap.log; W=/home/sens/NICOLA/agn-wt/integ-frozen; B=$W/build_frozen; Y=/home/sens/NICOLA/gnbLogs/armcfg/gnb_R2.yml
for arm in capR2d; do
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC" >> $LOG; exit 3; }
  a=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo); [ "$a" -ge 8000 ] || { echo "$(date +%T) ABORT MemAvailable $a" >> $LOG; exit 4; }
  F=/home/sens/NICOLA/captures/replay_${arm}_$(date +%m%d_%H%M).bin
  echo "$(date +%T) start $arm $F" >> $LOG; o=$S/$arm.$(date +%H%M).out
  (cd $W && RXG=36 B=$B RXENV="ISAC_USRP_RING_MS=250 ISAC_PASSIVE_REPLAY_CAPTURE=$F" GNB_CMD="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c $Y" tests/passive_rx/ota/lab_sens4_run.sh $arm > $o 2>&1)
  echo "$(date +%T) done $arm $(grep -o "/home/sens/NICOLA/captures/${arm}_lab_[0-9_]*" $o | tail -1) $(ls -la $F 2>/dev/null | awk '{print $5}')" >> $LOG
  grep -qE "ABORT|launcher rc=3" $o && { echo "$(date +%T) ABORT in $arm" >> $LOG; exit 5; }
  sleep 20
done
echo "$(date +%T) cap finished" >> $LOG
