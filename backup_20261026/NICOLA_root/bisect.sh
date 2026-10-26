#!/bin/bash
# usage: bisect.sh <commit>  -- build that commit in the coh-gpu-detect worktree and score the sim chain
set -e
W=/home/sens/NICOLA/mrx-wt/coh-gpu-detect; C=$1; O=/tmp/bisect_$C
cd $W && git stash -u -q 2>/dev/null || true; git checkout -q --detach $C
cd cmake_targets/ran_build/build_sense && nice -n 19 ninja -j4 isac_replay 2>&1 | grep -E " error|FAILED" | head -3 || true
mkdir -p $O && rm -f $O/coh*; sed "s#/tmp/isac_coh_sim/#$O/#" /tmp/isac_coh_sim/coherent.conf > $O/coherent.conf
nice timeout 1200 ./isac_replay -O $O/coherent.conf --rows /tmp/isac_coh_sim/rows.bin > $O/replay.txt 2>&1 || true
python3 /home/sens/NICOLA/multirx-clean-adaptive/.superpowers/sdd/2026-09-24-coherent-fuser-tracker/simscore.py $O
