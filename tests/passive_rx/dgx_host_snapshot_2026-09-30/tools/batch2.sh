#!/bin/bash
P=$(cd "$(dirname "$0")" && pwd); T=/home/nicola/NICOLA/passive_agnostic_rx_for_sensing/tests/passive_rx
until [ -f $P/scores.txt ]; do sleep 10; done
for i in 1 2; do GNBCONF=$T/gnb.sa.rfsim.100mhz.conf RXCONF=$T/ue.passive.auto.100mhz.conf CELL="-C 3750000000 -r 273 --ssb 1478" RXEXTRA="" $P/run2.sh p273_r$i 150 -m 9 -n 0 -M 273 -l 1; done
for i in 1 2; do GNBCONF=$T/gnb.sa.rfsim.100mhz.rank4.conf RXCONF=$T/ue.passive.autor4.100mhz.conf CELL="-C 3750000000 -r 273 --ssb 1478" RXEXTRA="--ue-nb-ant-rx 4 --ue-nb-ant-tx 4" $P/run2.sh r4_r$i 150 -m 25 -n 1 -M 273 -l 4; done
$P/score.sh p273_r* r4_r* > $P/scores2.txt; echo BATCH2_DONE
