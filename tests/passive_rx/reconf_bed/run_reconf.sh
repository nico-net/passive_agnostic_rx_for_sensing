#!/bin/bash
# Live-reconfiguration bed (DGX rfsim, phy-test): ONE passive receiver process stays up while the gNB
# server is stopped (SIGINT) and restarted with a changed cell. See README.md. Never run without the lock:
#   flock -x /tmp/td_measure.lock tests/passive_rx/reconf_bed/run_reconf.sh <out_dir> <S0|S1|S2|S2c|S3> [dur_s]
# env: WARM (s of gNB A before the change, default 90), GAP (s between gNB A exit and gNB B start, default 3),
#      PENV (env file for the receiver, default the OTA env MPCI_20261025.env), PCFG (receiver cfg),
#      DMASK (gNB -D mask, default 0x7f), RXEXTRA (extra receiver args), BUILD (build dir),
#      ALLOW_NOLOCK=1 (skip the caller-holds-lock check).
set -u
OUT=${1:?usage: run_reconf.sh <out_dir> <S0|S1|S2|S2c|S3> [dur]}; SC=${2:?scenario}; DUR=${3:-240}
HERE=$(cd "$(dirname "$0")" && pwd); R=$(cd "$HERE/../../.." && pwd); B=${BUILD:-$R/cmake_targets/ran_build/build}
WARM=${WARM:-90}; GAP=${GAP:-3}; DMASK=${DMASK:-0x7f}
PENV=${PENV:-/home/nicola/NICOLA/wt/rr-orchestration/ota/MPCI_20261025.env}
PCFG=${PCFG:-$R/tests/passive_rx/dgx/ue.passive.auto.100mhz.dgx.cfg}
BASE=$R/tests/passive_rx/gnb.sa.rfsim.100mhz.rank4.conf
UECAP=$R/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports4.xml
# gNB A (before) is the same for every scenario; gNB B (after) differs in config file and/or -m/-l.
MA=9; LA=4; CB=$BASE; MB=9; LB=4; EXPECT_PCI=0
case $SC in
  S0)  ;;                                                   # identical restart
  S1)  CB=$HERE/gnb_pci1.cfg; EXPECT_PCI=1 ;;               # PCI 0 -> 1
  S2)  CB=$HERE/gnb_tdd_6d3u.cfg ;;                         # TDD 7D+6s/2U+4s -> 6D+6s/3U+4s, same PCI
  S2c) CB=$HERE/gnb_ss0_1.cfg ;;                            # MIB searchSpaceZero 0 -> 1, same PCI
  S3)  MB=16; LB=2 ;;                                       # MCS 9 -> 16, rank 4 -> 2 (scheduler only)
  *) echo "unknown scenario $SC" >&2; exit 2 ;;
esac
[ -x "$B/nr-uesoftmodem" ] || { echo "no $B/nr-uesoftmodem" >&2; exit 2; }
if pgrep -x nr-uesoftmodem >/dev/null || pgrep -x nr-softmodem >/dev/null; then echo "another softmodem is running (OTA?)" >&2; exit 2; fi
if [ "${ALLOW_NOLOCK:-0}" != 1 ]; then
  # we are a child of the caller's flock: a fresh non-blocking attempt must FAIL if the caller holds it
  if flock -n -x /tmp/td_measure.lock true 2>/dev/null; then echo "caller does not hold /tmp/td_measure.lock (use flock -x ... run_reconf.sh)" >&2; exit 2; fi
fi
export LD_LIBRARY_PATH=$B CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps
rm -rf "$OUT"; mkdir -p "$OUT/gnbA" "$OUT/gnbB" "$OUT/pas"; OUT=$(cd "$OUT" && pwd)
EV=$OUT/events.txt; : > "$EV"
ev(){ printf '%.6f %s\n' "$(date +%s.%N)" "$*" >> "$EV"; }
ev "scenario $SC expect_pci=$EXPECT_PCI expect_nl=$LB expect_mcs=$MB before_nl=$LA before_mcs=$MA warm=$WARM gap=$GAP dur=$DUR"
rm -rf /tmp/passive_rx; mkdir -p /tmp/passive_rx

start_gnb(){ # <dir> <conf> <mcs> <layers>
  ( cd "$1" && touch nrL1_stats.log nrMAC_stats.log nr_stats.log &&
    exec timeout -s INT $((DUR+90)) "$B/nr-softmodem" --phy-test --noS1 -D "$DMASK" -O "$2" --rfsim \
      -m "$3" -n 1 -M 273 -l "$4" --uecap_file "$UECAP" > gnb.log 2>&1 ) &
  GPID=$!
}
wait_ready(){ for _ in $(seq 90); do [ -s "$1/reconfig.raw" ] && return 0; sleep 1; done; return 1; }

ev gnbA_start; start_gnb "$OUT/gnbA" "$BASE" $MA $LA; GA=$GPID
wait_ready "$OUT/gnbA" || { ev gnbA_not_ready; kill -INT $GA 2>/dev/null; wait; echo "gNB A never produced reconfig.raw" >&2; exit 3; }
ev gnbA_ready
cd "$OUT/pas" && touch nrL1_UE_stats-0.log nr_stats.log
# shellcheck disable=SC2046
env ${PENV:+$(cat "$PENV")} ISAC_METRICS_PATH=$OUT/metrics.jsonl timeout -s INT $((DUR+60)) "$B/nr-uesoftmodem" --passive-rx --rfsim -O "$PCFG" \
  -C 3750000000 -r 273 --numerology 1 --band 78 --ssb 1478 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 ${RXEXTRA:-} \
  > >(python3 -I "$HERE/stamp.py" "$OUT/run.log") 2>&1 &
P=$!
ev rx_start
sleep "$WARM"

# --- the change: SIGINT only (never SIGKILL); wait for the server socket to disappear ---
ev gnbA_stop_sent; kill -INT $GA 2>/dev/null
for _ in $(seq 90); do kill -0 $GA 2>/dev/null || break; sleep 1; done
kill -0 $GA 2>/dev/null && { ev gnbA_hung; echo "gNB A did not exit on SIGINT in 90 s; aborting (no SIGKILL)" >&2; kill -INT $P 2>/dev/null; wait; exit 4; }
ev gnbA_exited
sleep "$GAP"
ev gnbB_start; start_gnb "$OUT/gnbB" "$CB" $MB $LB; GB=$GPID
wait_ready "$OUT/gnbB" && ev gnbB_ready || ev gnbB_not_ready
ELAPSED=$(( $(date +%s) - $(awk '/ rx_start/{printf "%d",$1}' "$EV") ))
REM=$(( DUR - ELAPSED )); [ $REM -lt 30 ] && REM=30
sleep "$REM"

kill -INT $P 2>/dev/null; ev rx_stop_sent
for _ in $(seq 60); do kill -0 $P 2>/dev/null || break; sleep 1; done
kill -INT $GB 2>/dev/null
wait
sleep 3; ev end
echo "done $OUT; score with: python3 -I $HERE/score_reconf.py $OUT"
