#!/bin/bash
# UCI-on-PUSCH A/B at gain 42, alternated, after the gain sweep
S=$(dirname "$0"); cd /home/sens/NICOLA/agn-wt/s6-ota || exit 1
until grep -qE "sweep finished|ABORT" $S/sweep.log; do sleep 15; done
grep -q ABORT $S/sweep.log && { echo "$(date +%T) sweep aborted, A/B not started" >> $S/ab.log; exit 2; }
for arm in uci off uci off; do
  ip -4 -br addr show enp2s0f1np1 | grep -q 192.168.20.65 || { echo "$(date +%T) ABORT NIC address gone" >> $S/ab.log; exit 3; }
  a=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo); [ "$a" -ge 7000 ] || { echo "$(date +%T) ABORT MemAvailable $a" >> $S/ab.log; exit 4; }
  env=tests/passive_rx/sens6_ota/ota_lab273.s6.env; [ $arm = uci ] && env=tests/passive_rx/sens6_ota/ota_lab273.uci.s6.env
  echo "$(date +%T) start $arm" >> $S/ab.log
  o=$S/ab_$arm.$(date +%H%M).out
  RXG=42 RX_ENVFILE=$env tests/passive_rx/ota/lab_sens4_run.sh ab$arm > $o 2>&1
  echo "$(date +%T) done $arm $(grep -o '/home/sens/NICOLA/captures/ab[a-z]*_lab_[0-9_]*' $o | tail -1)" >> $S/ab.log
  grep -q ABORT $o && { echo "$(date +%T) ABORT in $arm" >> $S/ab.log; exit 5; }
  sleep 20
done
echo "$(date +%T) ab finished" >> $S/ab.log
