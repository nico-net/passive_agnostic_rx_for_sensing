#!/bin/bash
# One rfsim phy-test arm on the DGX: gNB (plain HEAD nr-softmodem --phy-test) + passive receiver.
# usage: GNBCONF=.. RXCONF=.. CELL="-C .. -r .. --ssb .." [RXEXTRA=..] [GNBARGS="-m 9 -n 0 -M 106 -l 1"] rfsim_arm.sh <outdir> <secs>
set -u
R=$(cd "$(dirname "$0")/../../.." && pwd); B=${BUILD:-$R/cmake_targets/ran_build/build}
OUT=$(realpath -m "$1"); DUR=$2
: "${GNBCONF:=$R/tests/passive_rx/gnb.sa.rfsim.conf}" "${RXCONF:=$R/tests/passive_rx/ue.passive.bwp.agn.conf}"
: "${CELL:=-C 3319680000 -r 106 --ssb 516}" "${GNBARGS:=-m 9 -n 0 -M 106 -l 1}" "${RXEXTRA:=}"
if pgrep -x nr-uesoftmodem >/dev/null || pgrep -x nr-softmodem >/dev/null; then echo "another softmodem is running" >&2; exit 2; fi
rm -rf "$OUT"; mkdir -p "$OUT/gnb" "$OUT/rx"; rm -rf /tmp/passive_rx; mkdir -p /tmp/passive_rx
( cd "$OUT/gnb" && touch nrL1_stats.log nrMAC_stats.log nr_stats.log &&
  exec timeout -s INT $((DUR+25)) "$B/nr-softmodem" --phy-test --noS1 -D 0xff -O "$GNBCONF" --rfsim $GNBARGS > gnb.log 2>&1 ) &
G=$!
sleep 10
cd "$OUT/rx" && touch nrL1_stats.log nr_stats.log
/usr/bin/time -v -o time.txt timeout -s INT "$DUR" "$B/nr-uesoftmodem" --passive-rx --rfsim -O "$RXCONF" $CELL \
  --numerology 1 --band 78 $RXEXTRA 2>&1 |
  gawk -e '@load "time"; BEGIN{t0=gettimeofday()} {printf "%.3f %s\n", gettimeofday()-t0, $0; fflush()}' > rx.log
echo "rx_rc=${PIPESTATUS[0]}" >> time.txt
kill -INT $G 2>/dev/null; wait $G
