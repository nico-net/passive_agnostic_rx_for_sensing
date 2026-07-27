#!/bin/bash
# Restart the AoA factorial batch if it dies before its wall-clock deadline. The first launch of
# this batch died silently ~5 minutes in when its parent session tore down, despite setsid --
# this watchdog is the safety net for that happening again over a 4-hour unattended window.
#
# Usage: ./_watchdog_factorial.sh <expected_deadline_epoch>
#
# The argument is NOT optional, and it is the fix for a trap that bit twice: a previous batch's
# deadline file can still be in the FUTURE when that batch is already dead. A watchdog started with
# no notion of which batch it owns then cheerfully relaunches the OLD one with --resume, which either
# competes with a newly started batch for the machine or silently appends new reps to stale results.
# Pinning the expected deadline makes the watchdog exit rather than adopt a batch it did not launch
# for. Read the deadline AFTER the batch has written it, and pass it here.
set -u
DEADLINE_FILE=/tmp/ms/aoa_factorial/deadline
LOG=/tmp/ms/fact.log
SS=/home/sens/NICOLA/openairinterface5g/tests/sensing_sim
EXPECT=${1:-}
if [ -z "$EXPECT" ]; then
  echo "usage: $0 <expected_deadline_epoch>" >&2
  exit 2
fi

while true; do
  if [ ! -f "$DEADLINE_FILE" ]; then
    sleep 30
    continue
  fi
  now=$(date +%s)
  deadline=$(cat "$DEADLINE_FILE" 2>/dev/null || echo 0)
  if [ "$deadline" != "$EXPECT" ]; then
    echo "$(date): deadline is $deadline, expected $EXPECT -- a different batch owns this root, exiting" >> /tmp/ms/watchdog.log
    exit 0
  fi
  if [ "$now" -ge "$deadline" ]; then
    echo "$(date): deadline reached, watchdog exiting" >> /tmp/ms/watchdog.log
    exit 0
  fi
  if ! pgrep -f "_run_aoa_factorial.sh" >/dev/null 2>&1; then
    echo "$(date): factorial batch is DOWN, relaunching for the remaining $((deadline-now))s" >> /tmp/ms/watchdog.log
    sudo pkill -9 -f "nr-.*softmodem" 2>/dev/null
    sudo pkill -9 -f iperf3 2>/dev/null
    sleep 3
    for n in 2 3 4; do sudo ip netns del oai_isac_gnb$n 2>/dev/null; sudo ip netns del oai_isac_ue$n 2>/dev/null; done
    remaining_h=$(python3 -c "print(max(0.05,($deadline-$now)/3600.0))")
    nohup setsid sudo -n "$SS/_run_aoa_factorial.sh" "$remaining_h" 240 four --resume >> "$LOG" 2>&1 < /dev/null &
    disown -a 2>/dev/null
  fi
  sleep 30
done
