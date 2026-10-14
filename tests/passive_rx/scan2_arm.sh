#!/bin/bash
# SCAN2 arm driver: one p99_replay.sh run + per-thread CPU sampling of the receiver (utime+stime ticks per thread name,
# last sample before exit) -> <replay dir>/cpu.txt. The replay itself waits for heavy.lock (flock -x) inside heavy.sh.
# usage: CAPSET=b|tim [ENVFILE=..] scan2_arm.sh <name> [ENV=VAL ...]      (SPEED via ISAC_RAW_IQ_SPEED=0.5 as an extra arg)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
name=$1
"$HERE/p99_replay.sh" "$@" > /tmp/claude-1001/scan2_arm_$name.out 2>&1 &
RP=$!
sleep 2
R=$(sed -n 's/^replay dir //p' /tmp/claude-1001/scan2_arm_$name.out | head -1)
[ -n "$R" ] || { echo "no replay dir"; wait $RP; exit 1; }
echo "$R"
last=""; best=0
while kill -0 $RP 2>/dev/null; do
  pid=$(pgrep -f "nr-uesoftmodem -O $R/replay.cfg" | while read p; do [ "$(cat /proc/$p/comm 2>/dev/null)" = nr-uesoftmodem ] && echo $p; done | head -1)
  if [ -n "$pid" ]; then
    s=$(for t in /proc/$pid/task/*; do
          [ -r $t/stat ] || continue
          awk -v c="$(cat $t/comm 2>/dev/null)" '{print c, $14, $15}' $t/stat 2>/dev/null
        done)
    t=$(echo "$s" | awk '{n+=$2+$3} END{print n+0}')
    [ -n "$s" ] && [ "$t" -ge "${best:-0}" ] && { last="$s"; best=$t; }
  fi
  sleep 1
done
wait $RP; rc=$?
echo "$last" > "$R/cpu.txt"
echo "rc=$rc $R"
