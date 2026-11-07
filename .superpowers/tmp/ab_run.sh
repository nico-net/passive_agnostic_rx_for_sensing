#!/bin/bash
W=/home/nicola/NICOLA/wt/rr-single1d; QB=/home/nicola/NICOLA/wt/rr-single1q/cmake_targets/ran_build/build; DB=$W/cmake_targets/ran_build/build
# NOTE: ab_arm.sh sets the sensing cfg from $W (same cfg text in both builds: single1d == single1q cfg)
for a in aq1:$QB ad1:$DB ad2:$DB aq2:$QB; do n=${a%%:*}; b=${a#*:}; $W/.superpowers/tmp/arm_lock.sh $n $b 1.0 sensing; done
echo ABDONE
