#!/bin/bash
# 4 RX vs 2 RX, sync-only, NIC drop counter bracketed around each arm.
# Hypothesis: RFSTALL/RFTSDISC are host-throughput-bound (rx_out_of_buffer), not sensing-CPU-bound
# (already disproved by SYNCONLY leaving them unchanged). Halving the stream should halve them.
set -u
I=enp129s0f0np0
cd /home/sens/NICOLA/captures
for rep in 1 2; do
  for n in 4 2; do
    A=$(ethtool -S $I | awk '/rx_out_of_buffer/{print $2}')
    SYNCONLY=1 CONTFO=1 GAINTRIM="0,16.5,3.5,5.6" ARM=n${n}_r${rep} NANT=$n \
      CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
      DUR=200 TRIES=1 RXG=25 ULPROBE=1 ./run_arm.sh > /dev/null 2>&1
    B=$(ethtool -S $I | awk '/rx_out_of_buffer/{print $2}')
    D=$(ls -dt /home/sens/NICOLA/captures/n${n}_r${rep}_*/ | head -1)
    L=$D/run.log
    printf "NANT=%s rep%s  nic_oob_delta=%-10s RFSTALL=%-4s RFTSDISC=%-5s drops=%s  %s\n" \
      "$n" "$rep" "$((B-A))" "$(grep -ac RFSTALL $L)" "$(grep -ac RFTSDISC $L)" \
      "$(grep -aoE 'processed=[0-9]+ dropped=[0-9]+' $L | tail -1)" \
      "$(cat $D/verdict.txt 2>/dev/null | grep -oE 'verdict=[A-Z_]+ .*cpis=[0-9]+')"
  done
done
echo "=== NANT AB DONE ==="
