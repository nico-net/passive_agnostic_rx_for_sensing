#!/bin/bash
# SYSAN: CPU profile of our running receiver: waits for it (exe under this worktree), skips DELAY s, records SECS s with perf.
# usage: sysan_perf.sh OUT.data [DELAY=10] [SECS=15]
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
for i in $(seq 1 7200); do
  for p in $(pgrep -x nr-uesoftmodem); do sudo -n readlink /proc/$p/exe | grep -q "^$ROOT/" && PID=$p; done
  [ -n "$PID" ] && break; sleep 0.5
done
[ -z "$PID" ] && { echo no receiver; exit 1; }
sleep ${2:-10}
sudo -n perf record -e cpu-clock -F ${PF:-997} ${PERF_EXTRA:--a} -o "$1" -- sleep ${3:-15} 2>&1 | tail -2
sudo -n chown $(id -u) "$1"
