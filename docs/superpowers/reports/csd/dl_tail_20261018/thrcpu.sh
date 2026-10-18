#!/bin/bash
# samples, for each of our receiver processes, the CPU seconds of the scan consumer threads (passivePdcch*) just before it exits
out=$1
while true; do
  p=$(pgrep -f "^/home/nicola/NICOLA/wt/rr-csd-int16kf/cmake_targets/ran_build/build_t26/nr-uesoftmodem" | head -1)
  if [ -n "$p" ]; then
    cmd=$(tr '\0' ' ' < /proc/$p/environ 2>/dev/null | grep -o "ISAC_PDCCH_BANK_HANDOFF=1\|ISAC_DCI11_READ_MEMO=1" | tr '\n' ' ')
    last=""
    while [ -d /proc/$p ]; do
      cur=$(for t in /proc/$p/task/*; do n=$(cat $t/comm 2>/dev/null); case $n in passivePdcch*|pdcchPar*) awk -v n=$n '{printf "%s=%.2f ", n, ($14+$15)/100}' $t/stat 2>/dev/null;; esac; done)
      [ -n "$cur" ] && last=$cur
      sleep 1
    done
    echo "$(date +%H:%M:%S) pid=$p flags=[$cmd] $last" >> $out
  fi
  sleep 1
done
