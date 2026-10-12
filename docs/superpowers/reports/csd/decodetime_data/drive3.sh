#!/bin/bash
# DECODETIME driver (from quiettiming_data/drive.sh): one replay at a time, quiet-host gate, per-run pre/post record.
# usage: drive.sh <order-file> ; lines "cap arm rep" ; arms: A = t26z, P = t26z + ISAC_PDSCH_GPU_HOST_PIN=1
# effective load = loadavg1 - D-state tasks (3 stuck mstflint/mlxlink_ext hold loadavg at a phantom 3.00 with 0 CPU use)
eff_load(){ awk -v l=$(cut -d' ' -f1 /proc/loadavg) -v d=$(ps -eo stat | grep -c '^D') 'BEGIN{printf "%.2f", l-d}'; }
W=/home/nicola/NICOLA/wt/rr-csd-dtm
B=$W/cmake_targets/ran_build/build_t26
OUT=$W/docs/superpowers/reports/csd/decodetime_data; LOG=$OUT/runs3.txt
export SPEED=1 ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26z.env B TMO=${TMO:-200}
while read -r cap arm rep; do
  [ -z "$cap" ] && continue
  case $cap in
   b) export CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900;;
   tim) export CAP=/home/nicola/NICOLA/ota_runs/raw_pci923_tim_12s_tile3 CAP_SAMPLES=1474560000 CAP_TICK=195322029 CAPC=3731910000 RXC=3759990000 SSB=233 FO=-4800;;
  esac
  case $arm in A) x="";; P) x="ISAC_PDSCH_GPU_HOST_PIN=1";; B) x="ISAC_PDSCH_GPU_HOST_PIN=1 ISAC_PDSCH_GPU_BATCH=1";; C) x="ISAC_PDSCH_GPU_HOST_PIN=1 ISAC_GPU_RT_PARTS=2";; Z) x="ISAC_PDSCH_GPU_HOST_PIN=1 ISAC_PDSCH_GPU_EXACT_CHEST_CHECK=0 ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=0 ISAC_PDSCH_GPU_EXACT_CHECK=0";; esac
  for i in $(seq 1 120); do   # wait up to 30 min for a quiet host
    l=$(eff_load)
    if [ -z "$(pgrep -x nr-uesoftmodem)" ] && awk -v l=$l 'BEGIN{exit !(l<2)}'; then break; fi
    sleep 15
  done
  pre="effload=$(eff_load) load=$(cut -d' ' -f1-3 /proc/loadavg) recv=$(pgrep -cx nr-uesoftmodem) $(nvidia-smi --query-gpu=temperature.gpu,clocks.sm --format=csv,noheader | tr -d ' ')"
  if [ -n "$(pgrep -x nr-uesoftmodem)" ] || ! awk -v l=$l 'BEGIN{exit !(l<2)}'; then echo "$cap $arm $rep NOTQUIET $pre" >> $LOG; echo STOP >> $LOG; exit 1; fi
  name=f${cap}${arm}${rep}
  cd $W
  bash tests/passive_rx/timacq_replay.sh $name ISAC_PDTIM_PCT=1 ISAC_PDTIM_CLASS=1 $x > $OUT/last.out 2>&1 &
  pid=$!; other=0
  while kill -0 $pid 2>/dev/null; do
    sleep 5
    n=$(pgrep -cx nr-uesoftmodem); [ "$n" -gt 1 ] && other=1
    h=$(ps -eo pcpu,comm --sort=-pcpu | awk 'NR>1 && $1>50 && $2!="nr-uesoftmodem" && $2!="passivePdsch*" {print $2}' | head -3 | tr '\n' ',')
    [ -n "$h" ] && echo "  hog:$h" >> $OUT/hogs_$name.txt
  done
  wait $pid; rc=$?
  d=$(cat $W/replays/last)
  echo "$cap $arm $rep dir=$d rc=$rc other_recv=$other PRE $pre POST load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $LOG
done < "$1"
echo DONE >> $LOG
