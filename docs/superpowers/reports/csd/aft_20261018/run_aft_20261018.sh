#!/bin/bash
# Afternoon on-air 2026-10-18: L = t26uL (baseline) vs Y = t26yE (L + core plan K5 + BANK_HANDOFF + UL-stall KCHARGE/STALE_DROP/LDPC_HOLD)
# vs F = t26fE (L + DL GPU pass v2 with FP32 demod). One binary: csd/int16-int 26b16e3671 build_t26. 150 s, n=3 per arm per cell, interleaved.
# A read-only thread sampler (sysan_threads.py, /proc only) records thread->core placement in each run.
set -u
O=/home/nicola/NICOLA/wt/rr-orchestration; cd $O
SAMP=/home/nicola/NICOLA/wt/rr-csd-int16ulr/tests/passive_rx/sysan_threads.py
( for t in $(cat ota/specs_aft_20261018.txt); do tag=${t%%:*}
    taskset -c 0-4 nice -n 10 python3 $SAMP /home/nicola/ /home/nicola/NICOLA/ota_runs/coresample_$tag.json 90 40 >/dev/null 2>&1
    while pgrep -x nr-uesoftmodem >/dev/null; do sleep 2; done
  done ) &
SP=$!
SPECS="$(cat ota/specs_aft_20261018.txt)" BDIR=/home/nicola/NICOLA/wt/rr-csd-int16ulr/cmake_targets/ran_build/build_t26 ota/ota_window_gen.sh
kill $SP 2>/dev/null
