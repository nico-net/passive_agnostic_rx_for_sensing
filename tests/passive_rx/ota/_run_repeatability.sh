#!/bin/bash
# Repeatability set: 6 x 90 s, identical config, ENERGY probe alternating ON/OFF.
# Tests (a) run-to-run variance in dedicated-CORESET reception, (b) whether the probe's own
# logging load degrades reception (run_probe had it ON and decoded 0; run_ussng had it OFF and
# decoded 4045 -- a confound that must be separated before either number means anything).
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets
OUT=/tmp/repeat_results.txt
: > $OUT
for i in 1 2 3 4 5 6; do
  if [ $((i % 2)) -eq 1 ]; then EP=""; TAG="probe_OFF"; else EP="ISAC_PDCCH_ENERGY=1"; TAG="probe_ON"; fi
  LOG=/tmp/rep_$i.log
  sudo -E env ISAC_DISC_NO_RESYNC=1 ISAC_PDCCH_NO_MISMATCH_GATE=1 $EP \
    setsid nohup bash -c "ulimit -c 0; exec timeout 90 \
    ./ran_build/build/nr-uesoftmodem \
    --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3,use_dpdk=1234 \
    -O /home/sens/NICOLA/nrue.passive_rx.uss_nogate.conf \
    -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 --ue-rxgain 40 \
    --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation \
    --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
    > $LOG 2>&1 < /dev/null &
  sleep 5
  while pgrep -x nr-uesoftmodem >/dev/null; do sleep 5; done
  S=$(grep -a "blind PDCCH monitor summary" $LOG | tail -1)
  OCC=$(echo "$S" | grep -oE "occasions=[0-9]+" | cut -d= -f2)
  ACC=$(echo "$S" | grep -oE "accepts=[0-9]+" | head -1 | cut -d= -f2)
  OK=$(echo "$S"  | grep -oE "crc_ok=[0-9]+" | cut -d= -f2)
  TRY=$(echo "$S" | grep -oE "try=[0-9]+" | cut -d= -f2)
  EF=$(echo "$S"  | grep -oE "efloor=[0-9.]+" | cut -d= -f2)
  DUR=$(python3 -c "print(f'{${OCC:-0}/1600:.0f}')")
  RATE=$(python3 -c "d=${OCC:-1}/1600; print(f'{${OK:-0}/d:.2f}' if d>0 else '0')")
  echo "run$i $TAG occ=${OCC:-?} dur=${DUR}s accepts=${ACC:-?} try=${TRY:-?} crc_ok=${OK:-?} efloor=${EF:-?} rows/s=$RATE" | tee -a $OUT
  sleep 3
done
echo "=== DONE ==="; cat $OUT
