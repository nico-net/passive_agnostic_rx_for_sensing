#!/bin/bash
# Keep ONE capture running continuously so the live monitor always has a feed.
#
# WHY. RFSTALL ("PBCH lock lost -- timing runaway") ends a capture with exit 3 and the receiver's
# own message is "restart the run": it is a recoverable condition, not a crash. run_arm.sh stops
# after its TRIES budget, so between captures the monitor correctly reports STALE -- there is
# genuinely no receiver. This loop removes the gap. It does NOT fix the stall; it makes the
# display continuous while the stall is still being chased. Every capture is kept, so the
# post-hoc analysis is unaffected.
#
# stop with:  touch /tmp/keep_live.stop
set -u
cd /home/sens/NICOLA/captures || exit 1
rm -f /tmp/keep_live.stop
n=0
while [ ! -e /tmp/keep_live.stop ]; do
  n=$((n+1))
  echo "=== [$(date +%H:%M:%S)] capture $n ==="
  env ARM="${ARM:-live}" \
      CONF="${CONF:-/home/sens/NICOLA/nrue.passive_rx.ul.q.conf}" \
      DUR="${DUR:-600}" TRIES=1 RXG="${RXG:-25}" NANT="${NANT:-4}" \
      MRC="${MRC:-3}" SENSECOMB="${SENSECOMB:-1}" SYNCONLY="${SYNCONLY:-1}" \
      CONTFO="${CONTFO:-1}" SLOTPOOL="${SLOTPOOL:-512}" \
      GAINTRIM="${GAINTRIM:-4.5,11.6,0,6.1}" ULPROBE="${ULPROBE:-1}" \
      bash ./run_arm.sh 2>&1 | grep -aE 'verdict=|X410|restarting'
  [ -e /tmp/keep_live.stop ] && break
  sleep 3   # let the device settle before re-opening it
done
echo "=== keep_live stopped after $n captures ==="
