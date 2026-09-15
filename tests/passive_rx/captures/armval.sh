#!/bin/bash
# usage: ARM=<name> [SFOCORR=1 ...] armval.sh   -- one MPM-checked, settled, survival-checked run_arm.sh run
ARM=${ARM:?}; CAP=/home/sens/NICOLA/captures; MGMT=128.178.122.174; ST=/tmp/${ARM}_status
cd /home/sens/NICOLA/adaptive-rx-UL-DL/tests/passive_rx/captures || exit 1
export REPO=/home/sens/NICOLA/adaptive-rx-UL-DL CONF=$CAP/cons6_ota.conf SCAN=1 AUTOACQ=0 NANT=${NANT:-4}
echo "$ARM start $(date +%H:%M:%S) env: SFOCORR=${SFOCORR:-}" > $ST
stop_clean() {
  local p; p=$(pgrep -x nr-uesoftmodem) || return 0
  sudo -n kill -TERM $p 2>/dev/null
  for i in $(seq 1 20); do sleep 1; pgrep -x nr-uesoftmodem >/dev/null || return 0; done
  sudo -n kill -9 $(pgrep -x nr-uesoftmodem) 2>/dev/null; sleep 2
}
for attempt in 1 2 3 4 5; do
  stop_clean
  # A SIGKILLed session leaves the X410 stream out of sequence; only an MPM restart clears it.
  ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 root@$MGMT "systemctl restart usrp-hwd" 2>/dev/null && echo "a$attempt: usrp-hwd restarted" >> $ST
  sleep 15
  for i in $(seq 1 60); do
    [ "$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 root@$MGMT "systemctl is-active usrp-hwd" 2>/dev/null)" = "active" ] && break
    sleep 5
  done
  echo "a$attempt: settling ${SETTLE:-200}s" >> $ST
  sleep ${SETTLE:-200}
  # Adaptive CFO seed: the previous attempt's CFOTRK "proposed" delta is added to the seed it ran with.
  if [ -n "${FIXFO:-}" ]; then
    INITIALFO=$FIXFO
  elif [ -f /tmp/${ARM}_run.log ] && [ -f "$CAP/.${ARM}_lastfo" ]; then
    last=$(grep -a "CFOTRK" $(ls -dt $CAP/${ARM}_*/ 2>/dev/null | head -1)/run.log 2>/dev/null | tail -1 | grep -oE "proposed=-?[0-9]+" | cut -d= -f2)
    seed=$(cat "$CAP/.${ARM}_lastfo")
    if [ -n "$last" ] && [ "${last#-}" -gt 500 ]; then INITIALFO=$(( seed + last )); echo "a$attempt: CFO seed $seed -> $INITIALFO (proposed $last)" >> $ST; else INITIALFO=$seed; fi
  else
    INITIALFO=${INITIALFO:--15000}
  fi
  echo "$INITIALFO" > "$CAP/.${ARM}_lastfo"
  export INITIALFO
  DUR=${DUR:-1200} TRIES=1 EVMPROBE=1 CSIRSBLIND=1 ARM=$ARM ./run_arm.sh > /tmp/${ARM}_run.log 2>&1 &
  RUNPID=$!
  locked=0
  for w in $(seq 1 24); do
    sleep 10
    d=$(ls -dt $CAP/${ARM}_*/ 2>/dev/null | head -1)
    [ -f "$d/run.log" ] || continue
    grep -aq "Cell Detected" "$d/run.log" 2>/dev/null && { locked=1; break; }
  done
  [ "$locked" != "1" ] && { echo "a$attempt: NO LOCK" >> $ST; kill $RUNPID 2>/dev/null; stop_clean; continue; }
  echo "a$attempt: LOCKED" >> $ST
  d=$(ls -dt $CAP/${ARM}_*/ | head -1)
  s1=$(stat -c %s $d/run.log); sleep 120; s2=$(stat -c %s $d/run.log)
  [ "$s2" -le "$s1" ] && { echo "a$attempt: STALLED after lock" >> $ST; kill $RUNPID 2>/dev/null; stop_clean; continue; }
  # SIB1 decodes within seconds of lock when it decodes at all (8 runs, 2026-09-15: 4/4 misses stayed
  # at 0 for the whole DUR). Waiting out DUR to learn VOID_NO_SIB1 costs 10 min per miss; 120 s is
  # already past the 120 s stall window above. SIB1WAIT=0 disables.
  if [ "${SIB1WAIT:-1}" != "0" ] && ! grep -aq "SIB1 decoded" "$d/run.log"; then
    echo "a$attempt: NO SIB1 120 s after lock -- retrying" >> $ST; kill $RUNPID 2>/dev/null; stop_clean; continue
  fi
  echo "a$attempt: SURVIVING, measuring" >> $ST
  wait $RUNPID
  # VOID_NO_CPI is vacuous with sensing off (no CPI can close), so it must not burn an attempt on
  # a run that decoded fine. Retry only the verdicts that really void the capture.
  if grep -qE "verdict=VOID_(CFO_MISLOCK|NO_SIB1|NO_LOCK|RFSTALL)" /tmp/${ARM}_run.log 2>/dev/null; then
    echo "a$attempt: $(grep -o 'verdict=[A-Z_]*' /tmp/${ARM}_run.log | tail -1) -- retrying" >> $ST; stop_clean; continue
  fi
  echo "DONE $d" >> $ST; exit 0
done
echo "EXHAUSTED" >> $ST
