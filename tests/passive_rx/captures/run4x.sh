#!/bin/bash
# 4 sensing-enabled captures. usrp-hwd is restarted before each one: the X410 degrades with use and
# a restart is what bought an 87 s run where the previous attempt stalled at 7 s.
set -u
OUT=/home/sens/NICOLA/captures/rows_$(date +%H%M%S); mkdir -p "$OUT"
CONF=/home/sens/NICOLA/nrue.passive_rx.rank2sense.conf
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets || exit 1
for i in 1 2 3 4; do
  sudo pkill -9 -x nr-uesoftmodem 2>/dev/null
  sleep 2
  ssh -o BatchMode=yes -o StrictHostKeyChecking=no root@128.178.122.3 "systemctl restart usrp-hwd" >/dev/null 2>&1
  sudo rm -rf /var/run/dpdk/* /dev/hugepages/* 2>/dev/null
  sleep 40
  for a in 1 2 3 4; do
    timeout 45 uhd_usrp_probe --args "type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3" 2>&1 \
      | grep -qE "X410|Device: X400" && break
    sleep 12
  done
  sudo rm -rf /var/run/dpdk/* /dev/hugepages/* 2>/dev/null; sleep 2
  B=$(ssh sens4 "stat -c%s /home/sens/NICOLA/gnbLogs/gnb.log")
  sudo env ISAC_DISC_NO_RESYNC=1 setsid nohup bash -c "ulimit -c 0; exec timeout 95 \
    ./ran_build/build/nr-uesoftmodem \
    --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3,use_dpdk=1234 \
    -O $CONF -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 --ue-rxgain 40 \
    --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation \
    --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
    > "$OUT/run$i.log" 2>&1 < /dev/null &
  sleep 5
  while pgrep -x nr-uesoftmodem >/dev/null; do sleep 5; done
  A=$(ssh sens4 "stat -c%s /home/sens/NICOLA/gnbLogs/gnb.log")
  L=$OUT/run$i.log
  OCC=$(grep -a "blind PDCCH monitor summary" "$L" | tail -1 | grep -oE "occasions=[0-9]+" | cut -d= -f2)
  DEC=$(grep -a "blind PDCCH monitor summary" "$L" | tail -1 | grep -oE "pdsch_decode\[[^]]*\]")
  ST=$(grep -ac "RFSTALL RF\|RFSTALL PBCH" "$L")
  echo "run$i occ=${OCC:-0} dur=$(python3 -c "print(f'{${OCC:-0}/1600:.0f}')")s stalls=$ST $DEC" | tee -a "$OUT/results.txt"
  echo "   occ_line: $(grep -aoE 'occ\[[^]]*\]' "$L" | tail -1)" | tee -a "$OUT/results.txt"
  echo "   cpi:      $(grep -aoE 'range\[[^]]*\] vel\[[^]]*\]' "$L" | tail -1)" | tee -a "$OUT/results.txt"
done
echo "=== DONE === $OUT" | tee -a "$OUT/results.txt"
