#!/bin/bash
# One instrumented capture: DL stage breakdown (BTIM) + UL stage breakdown (UTIM).
# Probes ON deliberately -- this run is for CHARACTERISATION, not scoring (R7).
set -u
OUT=/home/sens/NICOLA/captures/q1_$(date +%H%M%S); mkdir -p "$OUT"
CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf
DUR=${DUR:-95}
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets || exit 1

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
# ISAC_CFO_TRACK_*: the CFO trim loop. It exists and is measured (hard-zero runs ~45 % -> 1/8) but
# is env-gated and was OFF in every run of this session -- four consecutive captures decoded 0.0 %
# on DL PDSCH, SIB1 and PUSCH alike, the documented mis-lock signature. 800 Hz threshold: the
# measured boundary is |err| < 300 Hz decodes, > 2600 Hz gives exactly 0 %.
sudo env ISAC_DISC_NO_RESYNC=1 ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
  ISAC_CFO_TRACK_HZ=800 ISAC_CFO_TRACK_APPLY=1 \
  setsid nohup bash -c "ulimit -c 0; exec timeout $DUR \
  ./ran_build/build/nr-uesoftmodem \
  --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3,use_dpdk=1234 \
  -O $CONF -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 --ue-rxgain 40 \
  --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation \
  --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
  > "$OUT/q1.log" 2>&1 < /dev/null &
sleep 5
while pgrep -x nr-uesoftmodem >/dev/null; do sleep 5; done
A=$(ssh sens4 "stat -c%s /home/sens/NICOLA/gnbLogs/gnb.log")
echo "gnb_log_bracket $B $A" > "$OUT/bracket.txt"
echo "=== DONE === $OUT"

# ---- Run validity. A capture that never decoded SIB1 has no TDD pattern, so nr_ue_slot_select()
# calls every slot DOWNLINK and the UL hook never fires: pusch_book claimed=0 with no defect
# anywhere. That is a VOID run, not a result. Checked here so it cannot be read as a regression.
L=$OUT/q1.log
SIB=$(grep -ac 'SIB1 decoded' $L)
NACK=$(grep -ac 'Got NACK on NR-BCCH' $L)
CPI=$(grep -ac 'SENSING: CPI #' $L)
CLAIM=$(grep -aoE 'claimed=[0-9]+' $L | tail -1)
# DL LDPC health is part of the verdict, not a result to read afterwards: a mis-locked receiver
# decodes 0.0 % on EVERY stream at once, so a run whose downlink is dead cannot say anything about
# the uplink either.
DLOK=$(grep -aoE 'LDPCDIAG ok=[0-9]+' $L | tail -1 | cut -d= -f2)
CFOT=$(grep -ac CFOTRK $L)
if [ "$SIB" -eq 0 ]; then V=VOID_NO_SIB1;
elif [ "${DLOK:-0}" -eq 0 ]; then V=VOID_DL_ZERO;
elif [ "$CPI" -eq 0 ]; then V=VOID_NO_CPI;
else V=VALID; fi
echo "verdict=$V sib1_decoded=$SIB sib1_nack=$NACK dl_ldpc_ok=${DLOK:-0} cpis=$CPI $CLAIM cfotrk_lines=$CFOT" | tee $OUT/verdict.txt
