#!/bin/bash
# Live stage 1-2 discovery LOOP (2026-09-24). Waits for the receiver's snapshot dumps, then repeatedly
# re-runs GPU stage 1 + stage 2 on the LATEST N snapshots every POLL seconds, republishing
# /tmp/coresets_discovered.txt so new/changed CORESETs (a new UE, a cell the walk hasn't found yet)
# reach the receiver side -- which was ALREADY built for this: nr_pdcch_blind_monitor_discovered_poll()
# in nr_pdcch_blind_monitor.c polls the file's mtime and reloads/un-pauses the catalog walk on change.
# This loop is the only piece that was missing (see the 2026-09-23 macro-readiness notes: "the hand-off
# protocol IS already designed for periodic re-writes -- the missing piece is a loop around
# discover_live.sh itself"). Previously this script ran the wait+stage1+stage2 sequence exactly ONCE
# per receiver launch and exited.
#
# Runs on cores the receiver does not use (it is pinned to 0-7).
#
# Env overrides (all optional, defaults match the original one-shot behavior's inputs):
#   N          snapshot window size (default 10)
#   POLL       seconds between re-discovery passes (default 30)
#   MAX_ITERS  stop after this many passes; 0 = run forever (default 0; used by the offline test)
#   STAGE1_BIN path to the stage1/2 binary (default ./idsweep_offline_gpu)
#   SNAP_DIR   directory to scan for idsweep_0*.bin snapshots (default /tmp/passive_rx)
#   DISC_OUT   unused by this script directly (the binary itself writes the hand-off file); kept as
#              an env var only so a test can point the STUB binary at a private path without
#              colliding with a real receiver's /tmp/coresets_discovered.txt
N=${N:-10}
POLL=${POLL:-30}
MAX_ITERS=${MAX_ITERS:-0}
STAGE1_BIN=${STAGE1_BIN:-./idsweep_offline_gpu}
SNAP_DIR=${SNAP_DIR:-/tmp/passive_rx}
cd /home/sens/NICOLA/captures || exit 1

iter=0
last_set=""
while :; do
  until [ "$(find "$SNAP_DIR" -maxdepth 1 -name 'idsweep_0*.bin' | wc -l)" -ge "$N" ]; do sleep 2; done
  F=$(find "$SNAP_DIR" -maxdepth 1 -name 'idsweep_0*.bin' | sort | tail -n "$N")
  iter=$((iter + 1))
  if [ "$F" = "$last_set" ]; then
    echo "DISCOVERY pass $iter: no new snapshots since the last pass, skipping re-run"
  else
    last_set="$F"
    t0=$(date +%s.%N)
    OMP_NUM_THREADS=4 taskset -c 8-11 "$STAGE1_BIN" $F > live_stage1.txt 2>&1
    t1=$(date +%s.%N)
    OMP_NUM_THREADS=4 taskset -c 8-11 "$STAGE1_BIN" --stage2 live_stage1.txt $F > live_stage2.txt 2>&1
    t2=$(date +%s.%N)
    echo "DISCOVERY pass $iter: stage1 $(echo "$t1 - $t0" | bc) s, stage2 $(echo "$t2 - $t1" | bc) s"
    grep STAGE1LIST live_stage1.txt
    grep -E "HANDOFF|SUMMARY" live_stage2.txt
  fi
  if [ "$MAX_ITERS" -gt 0 ] && [ "$iter" -ge "$MAX_ITERS" ]; then
    echo "DISCOVERY loop: reached MAX_ITERS=$MAX_ITERS, stopping"
    break
  fi
  sleep "$POLL"
done
