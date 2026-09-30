#!/bin/bash
# usage: GNBCONF=.. RXCONF=.. CELL="-C .. -r .. --ssb .." RXEXTRA=".." run2.sh <arm> <secs> <gnb extra args...>
R=/home/nicola/NICOLA/passive_agnostic_rx_for_sensing; B=$R/cmake_targets/ran_build/build
ARM=$1; DUR=$2; shift 2
D=$(cd "$(dirname "$0")" && pwd)/$ARM; rm -rf "$D"; mkdir -p "$D/gnb" "$D/rx"; rm -rf /tmp/passive_rx; mkdir -p /tmp/passive_rx
cd "$D/gnb"; touch nrL1_stats.log nrMAC_stats.log nr_stats.log
timeout -s INT $((DUR+25)) $B/nr-softmodem --phy-test --noS1 -D 0xff -O $GNBCONF --rfsim "$@" > gnb.log 2>&1 &
G=$!
sleep 10
cd "$D/rx"; touch nrL1_stats.log nr_stats.log
/usr/bin/time -v -o time.txt timeout -s INT $DUR $B/nr-uesoftmodem --passive-rx --rfsim -O $RXCONF $CELL --numerology 1 --band 78 $RXEXTRA 2>&1 | gawk -e '@load "time"; BEGIN{t0=gettimeofday()} {printf "%.3f %s\n", gettimeofday()-t0, $0; fflush()}' > rx.log
echo rx_rc=${PIPESTATUS[0]} >> time.txt
kill -INT $G 2>/dev/null; wait $G; echo gnb_done
