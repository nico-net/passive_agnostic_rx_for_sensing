#!/bin/bash
# UL rfsim test bed, one arm (2026-10-17): OAI gNB (--phy-test, continuous scheduled UL) + OAI UE (--phy-test, sends PUSCH in every granted UL
# slot) + the passive receiver as a listen-only 3rd rfsim client. Stock rfsimulator never forwards UE UL to a third client, so the gNB (server)
# runs with RFSIM_UL_TAP=1: the 2nd client is a TAP and receives DL + the UL summed at the gNB RX on one timeline, delayed by 10 ms
# (radio/rfsimulator/simulator.cpp tap_send). gNB log/config = validation ground truth only, except the labelled forced-ID arm.
#
# usage: [env] ul_rfsim_arm.sh <outdir> <secs>
#  TBIDS="dmrs:data"   gNB+UE configure scramblingID0 / dataScramblingIdentityPUSCH (ISAC_TB_PUSCH_IDS, nr_radio_config.c). unset = PCI (OAI default)
#  TBVARY=1            gNB varies PRB range + MCS (0..16) per UL grant (ISAC_TB_UL_VARY, gNB_scheduler_phytest.c); unset = fixed 106 PRB MCS 9
#  TBTP=1              transform precoding enabled      TBMCSTAB=256  UL MCS table qam256 (gNB -u 1)
#  ULMCS=9 ULBW=106 ULSLOTS=0x300 ULNL=1 ULTAB=1   gNB phy-test UL MCS (-t), PRBs (-T), slot bitmap (-U), MCS table (-u; 1 = qam256 as the UE cap file configures)
#  FORCE_IDS="dmrs:data"   FORCED-ID ARM: receiver env ISAC_UL_FORCE_IDS (labelled diagnostic, not blind)
#  ULENV (default = the OTA t26gE UL chain flags minus GPU/core pins; ULENV="" = chain off)
#  RXEXTRA / RXENV         extra receiver args / env (space separated NAME=VAL)
set -u
R=$(cd "$(dirname "$0")/../../.." && pwd); B=${BUILD:-$R/cmake_targets/ran_build/build_t26}
OUT=$(realpath -m "$1"); DUR=$2
: "${GNBCONF:=$R/tests/passive_rx/gnb.sa.rfsim.conf}" "${RXCONF:=$R/tests/passive_rx/dgx/ul_rfsim_rx.cfg}"
: "${ULENV=ISAC_UL_PUSCH_CHAIN=1 ISAC_UL_PUSCH_SOLVER=1 ISAC_UL_RETRY=1 ISAC_UL_UCI_SAFE=1 ISAC_UL_DCI_LOG=1 ISAC_UL_LAYOUT_LOCK=1 ISAC_UL_LOCK_SIB1_FIRST=1 ISAC_UL_LOCK_CRC_FIRST=1 ISAC_UL_LOCK_CRC_K=3 ISAC_UL_LOCK_CRC_GUARD=1 ISAC_UL_LOCK_BWP_ALL=1 ISAC_UL_LOCK_MCS_STAT=1 ISAC_UL_PRIO_QUEUE=1 ISAC_UL_HARQ_COMBINE=1 ISAC_UL_SOLVER_FALLTHROUGH=2 ISAC_UL_FIT_READ=1 ISAC_UL_PREFIX_ENT=1 ISAC_UL_STALL_DIAG=1 ISAC_UL_LOCK_SIGN=2 ISAC_UL_LOOKALIKE=1 ISAC_UL_SNAP_POOL=1 ISAC_UL_QUEUE_DEPTH=32 ISAC_UL_SPARE_RESCUE=1}" "${ULMCS:=9}" "${ULBW:=106}" "${ULSLOTS:=0x300}" "${ULNL:=1}" "${RXEXTRA:=--ue-nb-ant-rx 4}" "${RXENV:=}" "${TBIDS:=}" "${TBTP:=}" "${TBMCSTAB:=}" "${FORCE_IDS:=}"
UECAP=$R/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml
CELL="-C 3319680000 -r 106 --ssb 516"
for _ in $(seq 1 30); do pgrep -u "$USER" -x nr-uesoftmodem >/dev/null || pgrep -u "$USER" -x nr-softmodem >/dev/null || break; sleep 1; done
if pgrep -u "$USER" -x nr-uesoftmodem >/dev/null || pgrep -u "$USER" -x nr-softmodem >/dev/null; then echo "another softmodem of this user is running" >&2; exit 2; fi
rm -rf "$OUT"; mkdir -p "$OUT/gnb" "$OUT/ue" "$OUT/rx"; rm -rf /tmp/passive_rx; mkdir -p /tmp/passive_rx
GENV=(RFSIM_UL_TAP=1 ISAC_TB_LOG_WIDTHS=1); [ -n "$TBIDS" ] && GENV+=(ISAC_TB_PUSCH_IDS="$TBIDS"); [ -n "$TBTP" ] && GENV+=(ISAC_TB_PUSCH_TP=1); [ -n "${TBVARY:-}" ] && GENV+=(ISAC_TB_UL_VARY=1)
GARGS=(-m 9 -n 0 -M 106 -l 1 -t "$ULMCS" -T "$ULBW" -U "$ULSLOTS" --uecap_file "$UECAP")
GARGS+=(-u "${ULTAB:-1}")  # phy-test sets the gNB-side table; the UE takes qam256 from the cap file, so -u 0 desynchronises MCS (root cause of the first runs: gNB MCS 9 = QPSK vs UE MCS 9 = 16QAM)
[ -n "$TBMCSTAB" ] && GENV+=(ISAC_TB_PUSCH_MCSTAB=256)
( cd "$OUT/gnb" && touch nrL1_stats.log nrMAC_stats.log nr_stats.log &&
  exec timeout -s INT $((DUR+60)) env "${GENV[@]}" "$B/nr-softmodem" --phy-test --noS1 -D "${DLSLOTS:-127}" -O "$GNBCONF" --rfsim "${GARGS[@]}" ${GNBEXTRA:-} > gnb.log 2>&1 ) &
G=$!
for _ in $(seq 1 40); do [ -s "$OUT/gnb/reconfig.raw" ] && [ -s "$OUT/gnb/rbconfig.raw" ] && break; sleep 1; done
[ -s "$OUT/gnb/rbconfig.raw" ] || { echo "gNB did not dump reconfig.raw/rbconfig.raw" >&2; kill -INT $G; wait $G; exit 3; }
cp "$OUT/gnb/reconfig.raw" "$OUT/gnb/rbconfig.raw" "$OUT/ue/"
( cd "$OUT/ue" && exec timeout -s INT $((DUR+40)) "$B/nr-uesoftmodem" --phy-test --noS1 --rfsim $CELL --numerology 1 --band 78 \
    --reconfig-file reconfig.raw --rbconfig-file rbconfig.raw -O "$R/tests/passive_rx/dgx/ul_rfsim_ue.cfg" --rfsimulator.serveraddr 127.0.0.1 > ue.log 2>&1 ) &
U=$!
sleep "${UE_SETTLE:-25}"
cd "$OUT/rx" && touch nrL1_stats.log nr_stats.log
if [ "${NORX:-0}" = 1 ]; then sleep "$DUR"; else  # NORX=1: gNB+UE only (bed sanity)
RXE=(); [ -n "$FORCE_IDS" ] && RXE+=(ISAC_UL_FORCE_IDS="$FORCE_IDS")
/usr/bin/time -v -o time.txt timeout -s INT "$DUR" env ISAC_UL_LAYOUT_LOG=1 $ULENV "${RXE[@]}" $RXENV "$B/nr-uesoftmodem" --passive-rx --rfsim -O "$RXCONF" $CELL \
  --numerology 1 --band 78 $RXEXTRA 2>&1 |
  gawk -e '@load "time"; BEGIN{t0=gettimeofday()} {printf "%.3f %s\n", gettimeofday()-t0, $0; fflush()}' > rx.log
echo "rx_rc=${PIPESTATUS[0]}" >> time.txt
fi
kill -INT $U 2>/dev/null; sleep 3; kill -INT $G 2>/dev/null; wait $U $G 2>/dev/null
# ground truth summary (validation only)
{ echo "gnb_ulsch_crc_lines: $(grep -c 'ULSCH.*CRC\|ulsch.*crc' "$OUT/gnb/gnb.log")"; } > "$OUT/summary.txt"
