#!/bin/bash
# Catch-rate characterisation for the DEDICATED CORESET (2026-08-23).
#
# WHY THIS EXISTS: rows/s is meaningless on its own -- the offered load on this cell was measured at
# 170/s, 543/s and 738/s at different points on the same day, so a rows/s trend can be the receiver
# improving OR iperf ramping. The predecessor script (_run_repeatability.sh) did NOT separate them
# and its monotonic 0 -> 61 rows/s trend is therefore uninterpretable. This one brackets the gNB log
# around EVERY run and reports CATCH RATE = real rows / offered grants, which is load-invariant.
#
# Ground truth is taken by BYTE RANGE, never by clock arithmetic: sens4 runs ~21 min behind sens6
# (rule R2), so timestamps cannot be used to align the two machines.
#
# Logs go to a PERSISTENT directory. /tmp on sens6 is cleaned and has already destroyed a full set
# of capture logs once.
set -u
GNB=sens4
GNB_LOG=/home/sens/NICOLA/gnbLogs/gnb.log
OUTDIR=${OUTDIR:-/home/sens/NICOLA/captures/catchrate_$(date +%Y%m%d_%H%M%S)}
CONF=${CONF:-/home/sens/NICOLA/nrue.passive_rx.uss_nogate.conf}
RUNS=${RUNS:-6}
DUR=${DUR:-90}
RXGAIN=${RXGAIN:-40}
mkdir -p "$OUTDIR"
RESULTS=$OUTDIR/results.txt
: > "$RESULTS"
echo "outdir=$OUTDIR conf=$CONF runs=$RUNS dur=${DUR}s rxgain=$RXGAIN" | tee -a "$RESULTS"

cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets || exit 1

for i in $(seq 1 "$RUNS"); do
  LOG=$OUTDIR/run$i.log
  # --- bracket the gNB log around THIS run only ---
  B=$(ssh $GNB "stat -c%s $GNB_LOG")
  sudo -E env ISAC_DISC_NO_RESYNC=1 ISAC_PDCCH_NO_MISMATCH_GATE=1 ISAC_PDCCH_DCIGT=1 \
    setsid nohup bash -c "ulimit -c 0; exec timeout $DUR \
    ./ran_build/build/nr-uesoftmodem \
    --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3,use_dpdk=1234 \
    -O $CONF \
    -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 --ue-rxgain $RXGAIN \
    --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation \
    --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
    > "$LOG" 2>&1 < /dev/null &
  sleep 5
  while pgrep -x nr-uesoftmodem >/dev/null; do sleep 5; done
  A=$(ssh $GNB "stat -c%s $GNB_LOG")
  N=$((A-B))

  # Offered grants and the live C-RNTI, from the gNB's own scheduler decisions. Streamed and
  # grepped on sens4 -- the window is multi-GB at debug level, never transfer it.
  read -r OFFERED CRNTI <<<"$(ssh $GNB "tail -c +$((B+1)) $GNB_LOG | head -c $N | \
      grep -aoE 'DL PDCCH: rnti=0x[0-9a-f]+ type=c-rnti' | grep -oE '0x[0-9a-f]+' | \
      sort | uniq -c | sort -rn | head -1 | awk '{print \$1, \$2}'")"
  # Per-CCE offered histogram for the frequency-dependence test (see score_cce.py)
  ssh $GNB "tail -c +$((B+1)) $GNB_LOG | head -c $N | \
      grep -aoE 'DL PDCCH: rnti=${CRNTI:-0x0} type=c-rnti .*cce=[0-9]+ al=2' | \
      grep -oE 'cce=[0-9]+' | sort | uniq -c" > "$OUTDIR/run${i}_gnb_cce.txt"

  S=$(grep -a "blind PDCCH monitor summary" "$LOG" | tail -1)
  OCC=$(echo "$S" | grep -oE "occasions=[0-9]+" | cut -d= -f2)
  OK=$(echo  "$S" | grep -oE "crc_ok=[0-9]+"    | cut -d= -f2)
  EF=$(echo  "$S" | grep -oE "efloor=[0-9.]+"   | cut -d= -f2)
  # Real accepts = those carrying the live C-RNTI. Raw `accepts` is NOT this: with the mismatch
  # gate off it is dominated by one-off false decodes (measured: 9948 distinct RNTIs in 11178
  # accepts). crc_ok is self-validating -- a false accept cannot pass a PDSCH CRC.
  REAL=$(grep -a DCIGT "$LOG" | grep -c "rnti=${CRNTI:-none}")
  D=$(python3 -c "print(f'{${OCC:-0}/1600:.0f}')")
  python3 - "$i" "$D" "${OCC:-0}" "${OFFERED:-0}" "${OK:-0}" "$REAL" "${EF:-0}" "${CRNTI:-?}" <<'PY' | tee -a "$RESULTS"
import sys
i,d,occ,off,ok,real,ef,crnti = sys.argv[1:]
d=float(d) or 1; off=int(off); ok=int(ok); real=int(real)
print(f"run{i} crnti={crnti} dur={d:.0f}s occ={occ} offered={off} ({off/d:.0f}/s) "
      f"real_accepts={real} crc_ok={ok} rows/s={ok/d:.1f} "
      f"CATCH={100*ok/off if off else 0:.2f}% efloor={ef}")
PY
  sleep 3
done
echo "=== DONE ==="; cat "$RESULTS"
