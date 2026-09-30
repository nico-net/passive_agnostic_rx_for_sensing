#!/bin/bash
# Swisscom n78 (PCI 382, 3649.98 MHz, 273 PRB) passive capture on sens6/X410, adaptive-rx-UL-DL tree.
# Minimal: preflight NIC tuning (same values run_arm.sh asserts), SIGTERM-only shutdown (timeout),
# >=60 s gap enforced before launch, then the counters that matter for the dedicated-decode chain.
# ARM=<name> CONF=<path> DUR=<s> [RXG=50 NANT=1 XENV="K=V ..."]
set -u
ARM=${ARM:?set ARM}; CONF=${CONF:?set CONF}; DUR=${DUR:-300}
RXG=${RXG:-50}; NANT=${NANT:-1}; CARRIER=${CARRIER:-3649980000}; SSB=${SSB:-204}
REPO=/home/sens/NICOLA/adaptive-rx-UL-DL
BIN=$REPO/cmake_targets/ran_build/build/nr-uesoftmodem
DATA=192.168.20.2; MGMT=192.168.1.140; NIC=enp2s0f1np1
BASE=/home/sens/NICOLA/captures

[ -x "$BIN" ] || { echo "no binary $BIN"; exit 5; }
newer=$(find "$REPO/openair1" "$REPO/openair2" "$REPO/executables" "$REPO/radio" \
          \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' \) -newer "$BIN" -print -quit 2>/dev/null)
[ -z "$newer" ] || { echo "ABORT: source newer than binary: $newer"; exit 4; }
if pgrep -x nr-uesoftmodem >/dev/null; then echo "ABORT: a receiver is already running (not killing it)"; exit 6; fi
# >= 60 s since the previous receiver exited (user rule). Newest run.log mtime is the last exit.
last=$(ls -t "$BASE"/*/run.log 2>/dev/null | head -1)
if [ -n "$last" ]; then
  gap=$(( $(date +%s) - $(date -r "$last" +%s) ))
  [ "$gap" -ge 60 ] || { echo "  waiting $((60 - gap)) s (60 s gap rule)"; sleep $((60 - gap)); }
fi
# NIC preflight (run_arm.sh's values)
[ "$(cat /sys/class/net/$NIC/mtu)" = 9000 ] || { sudo ip link set "$NIC" mtu 9000; NIC_CHANGED=1; }
ethtool -g "$NIC" | awk '/Current hardware/,0' | grep -qE '^RX:[[:space:]]+8192' || { sudo ethtool -G "$NIC" rx 8192 tx 8192; NIC_CHANGED=1; }
[ "$(sysctl -n net.core.netdev_max_backlog)" = 250000 ] || sudo sysctl -q -w net.core.netdev_max_backlog=250000 net.core.rmem_default=62500000 net.core.rmem_max=250000000 net.core.wmem_max=250000000
n=0; for i in $(ls /sys/class/net/$NIC/device/msi_irqs); do
  [ -e /proc/irq/$i/smp_affinity_list ] && { echo "$(( 8 + n % 6 ))" | sudo tee /proc/irq/$i/smp_affinity_list >/dev/null; n=$((n+1)); }
done
# any NIC change above flaps the link -- let it settle before the stream starts (RFSTALL at RX_START otherwise)
[ "$(cat /sys/class/net/$NIC/mtu)" = 9000 ] && [ -z "${NIC_CHANGED:-}" ] || sleep 30
ping -c1 -W2 "$DATA" >/dev/null || { echo "ABORT: X410 $DATA unreachable"; exit 3; }
timeout 45 uhd_usrp_probe --args "type=x4xx,addr=$DATA,mgmt_addr=$MGMT" 2>&1 | grep -qE "X410|Device: X400" || { echo "ABORT: X410 probe failed"; exit 3; }
sleep 2

OUT=$BASE/${ARM}_$(date +%H%M%S); mkdir -p "$OUT"
MISS0=$(cat /sys/class/net/$NIC/statistics/rx_missed_errors)
echo "  launching $ARM -> $OUT (DUR=$DUR NANT=$NANT RXG=$RXG conf=$(basename $CONF))"
# timeout sends SIGINT at DUR (OAI's Ctrl-C path; a stream-stalled receiver ignored SIGTERM 2026-09-21); no SIGKILL anywhere.
sudo env ISAC_DISC_NO_RESYNC=1 ISAC_UE_RT_CORE=2 ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
  ISAC_CFO_TRACK_HZ=800 ISAC_CFO_TRACK_PERIOD=20 ISAC_TSYNC_RESET=0 ISAC_AUTO_ACQUIRE=0 ${XENV:-} \
  setsid nohup bash -c "ulimit -c 0; exec timeout -s INT $DUR taskset -c 0-7 $BIN \
    --usrp-args type=x4xx,addr=$DATA,mgmt_addr=$MGMT -O $CONF -r 273 --numerology 1 --band 78 \
    -C $CARRIER --ssb $SSB --ue-rxgain $RXG --ue-nb-ant-rx $NANT --ue-nb-ant-tx $NANT --passive-rx \
    --ue-fo-compensation --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
  > "$OUT/run.log" 2>&1 < /dev/null &
sleep 5
while pgrep -x nr-uesoftmodem >/dev/null; do sleep 5; done
L=$OUT/run.log
{
echo "arm=$ARM dur=$DUR nant=$NANT rxg=$RXG conf=$(basename $CONF) nic_miss=$(( $(cat /sys/class/net/$NIC/statistics/rx_missed_errors) - MISS0 ))"
echo "sib1_decoded=$(grep -ac 'SIB1 decoded' $L) rfstall=$(grep -ac RFSTALL $L) cfotrk_stable=$(grep -ac 'CFOTRK .*stable=yes' $L)"
grep -a 'blind PDCCH monitor summary' $L | tail -1 | grep -oE 'accepts=[0-9]+ dci10\[[^]]*\]|held\[[^]]*\]|scanq\[[^]]*\]'
grep -a 'BTIM fep' $L | tail -1 | grep -oE 'TOTAL\[[^]]*\]|dlsweep\[[^]]*\]|ulsweep\[[^]]*\]'
grep -a 'BTIM occ_total' $L | tail -1 | grep -oE 'over_slot.*'
grep -a 'LDPCDIAG' $L | tail -1 | grep -oE 'ok=[0-9]+ seg_fail=[0-9]+'
grep -a 'PDSCHQ per-rnti' $L | tail -1 | sed 's/.*per-rnti/per-rnti/'
echo "non-SI TBRESULT:"; grep -a TBRESULT $L | grep -av 'rnti=0xffff' | sed 's/.*rnti=\(0x[0-9a-f]*\).*prb=\([0-9+]*\).*refpt=\([01]\).*sym=\([0-9+]*\).*status=\(\w*\)/  \1 prb=\2 refpt=\3 sym=\4 \5/' | sort | uniq -c | sort -rn | head -8
grep -a 'RRCHARVEST census' $L | tail -1 | sed 's/.*RRCHARVEST/RRCHARVEST/'
grep -a 'RRCSETUP HARVEST' $L | head -5 | cut -c1-300
grep -a 'RA-RNTI VERIFIED\|TC-RNTI\|trusted' $L | tail -3 | cut -c1-200
grep -a 'ACQ state' $L | tail -1 | cut -c1-200
} | tee "$OUT/summary.txt"
echo "=== $ARM DONE -> $OUT"
