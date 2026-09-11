#!/usr/bin/env bash
# Bounded passive manual OTA test. Never resets the radio or changes the NIC/gNB.
set -euo pipefail
umask 022
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=$REPO/cmake_targets/ran_build/build
DURATION=${DURATION:-120}
PORT=${MONITOR_PORT:-8083}
[[ $EUID == 0 ]] || { echo 'Run with sudo -n env DURATION=120 bash tests/passive_rx/run_manual_x410.sh'; exit 3; }
[[ $DURATION =~ ^[0-9]+$ && $PORT =~ ^[0-9]+$ ]] || exit 3
((DURATION >= 30 && DURATION <= 3600 && PORT >= 1024 && PORT <= 65535)) || exit 3
test -x "$BUILD/nr-uesoftmodem"
grep -qx 'ENABLE_ISAC_SENSING:BOOL=ON' "$BUILD/CMakeCache.txt"
test "$(readlink "$BUILD/liboai_device.so")" = liboai_usrpdevif.so
if find "$REPO/openair1" "$REPO/openair2" "$REPO/executables" "$REPO/radio" -path '*/tests/*' -prune -o \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \) -newer "$BUILD/nr-uesoftmodem" -print -quit | grep -q .; then
  echo 'BLOCKED: rebuild the receiver before testing changed source'; exit 3
fi
# Read-only opening avoids Linux protected_regular's root/O_CREAT trap in /tmp.
if [[ ! -e /tmp/adaptive-rx-UL-DL.radio.lock ]]; then
  (umask 002; : > /tmp/adaptive-rx-UL-DL.radio.lock)
fi
exec 9</tmp/adaptive-rx-UL-DL.radio.lock
flock -n 9 || { echo 'BLOCKED: radio lock held'; exit 3; }
if pgrep -x nr-uesoftmodem >/dev/null || pgrep -x nr-softmodem >/dev/null ||
   pgrep -f '(^|/)(uhd_usrp_probe|benchmark_rate|rx_samples_to_file|rx_multi_samples)( |$)' >/dev/null; then
  echo 'BLOCKED: another modem or radio utility is running'; exit 3
fi
test "$(cat /sys/class/net/enp129s0f0np0/mtu)" = 9000
ip -4 addr show enp129s0f0np0 | grep -q '192.168.20.1/24'
python3 - "$PORT" <<'PY'
import socket, sys
with socket.socket() as sock:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', int(sys.argv[1])))
PY
OUT=$(mktemp -d /home/sens/NICOLA/captures/sensing_manual_fixed.XXXXXX)
chmod 755 "$OUT"
sed "s|@OUT@|$OUT|g" "$REPO/tests/passive_rx/adaptive_manual_dlul.conf" > "$OUT/receiver.conf"
sha256sum "$BUILD/nr-uesoftmodem" "$BUILD/liboai_usrpdevif.so" "$OUT/receiver.conf" > "$OUT/checksums.txt"
read_counter() {
  local value
  IFS= read -r value < /sys/class/net/enp129s0f0np0/statistics/rx_missed_errors || return 1
  [[ $value =~ ^[0-9]+$ ]] || return 1
  printf '%s\n' "$value"
}
before=$(read_counter)
printf '%s\n' "$before" > "$OUT/nic_missed_before.txt"
printf 'OUTPUT=%s\nMONITOR=http://localhost:%s/\n' "$OUT" "$PORT"
date -u +%FT%TZ > "$OUT/start.txt"
# The dashboard must not inherit the radio lock: it remains available after the test.
nohup python3 "$REPO/tests/passive_rx/monitor/monitor.py" --connect tcp://127.0.0.1:5556 \
  --log "$OUT/run.log" --port "$PORT" --bind 127.0.0.1 > "$OUT/dashboard.log" 2>&1 < /dev/null 9<&- &
printf '%s\n' "$!" > "$OUT/dashboard.pid"
PID=
rc=0
cleanup_status=CLEAN
stop_receiver() {
  [[ -n $PID ]] || return 0
  if kill -0 "$PID" 2>/dev/null; then
    kill -INT "$PID" 2>/dev/null || true
    sleep 3
    kill -INT "$PID" 2>/dev/null || true
    for ((n=0;n<12;n++)); do
      kill -0 "$PID" 2>/dev/null || break
      sleep 1
    done
    if kill -0 "$PID" 2>/dev/null; then
      cleanup_status=FORCED_KILL
      kill -KILL "$PID" 2>/dev/null || true
    fi
  fi
  rc=0
  wait "$PID" || rc=$?
  PID=
}
trap stop_receiver EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cd "$BUILD"
replay_env=()
[[ ${REPLAY:-} == 1 ]] && replay_env=(ISAC_PASSIVE_REPLAY_CAPTURE="$OUT/replay.bin" ISAC_PASSIVE_REPLAY_FAILURES=1)
env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  ISAC_RX_MRC_MODE=2 ISAC_UL_RX_BRANCH=-1 \
  ISAC_DMRS_FO_APPLY=0 ISAC_SFO_CORRECT=0 ISAC_RX_BRANCH_FO=0 ISAC_RX_GAIN_TRIM=0,0,0,0 \
  ISAC_DISC_NO_RESYNC=0 ISAC_RF_STALL_MAX_REINIT=0 ISAC_CFO_TRACK_HZ=1 ISAC_CFO_TRACK_PERIOD=20 \
  ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
  ISAC_UL_TA_SWEEP=0:0:0 ISAC_SENSE_COMB=0 ISAC_TSYNC_RESET=0 \
  "${replay_env[@]}" \
  LD_LIBRARY_PATH="$BUILD:/usr/local/lib" \
  taskset -c 0-7 "$BUILD/nr-uesoftmodem" \
  --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174 \
  -O "$OUT/receiver.conf" -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
  --ue-rxgain 40 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx \
  --ue-fo-compensation --cont-fo-comp 1 --freq-sync-P 0.05 --freq-sync-I 0.001 \
  --initial-fo -16480 --thread-pool 0,1,6,7 --time-sync-I 0.01 \
  --ntn-initial-time-drift -4.25 -A 90 > "$OUT/run.log" 2>&1 9<&- &
PID=$!
printf '%s\n' "$PID" > "$OUT/receiver.pid"
reason=DURATION_COMPLETE
started=$SECONDS
while ((SECONDS - started < DURATION)); do
  sleep 2
  if grep -qaE 'RXDISCONT|RFSTALL|Assertion.*failed' "$OUT/run.log"; then reason=VOID_RF_OR_ASSERT; break; fi
  if ! current=$(read_counter); then reason=VOID_NIC_COUNTER_READ; break; fi
  if [[ $current != "$before" ]]; then reason=VOID_NIC_MISSED; break; fi
  if ! kill -0 "$PID" 2>/dev/null; then reason=VOID_EARLY_EXIT; break; fi
done
printf '%s\n' "$reason" > "$OUT/stop_reason.txt"
stop_receiver
printf '%s\n' "$cleanup_status" > "$OUT/cleanup_status.txt"
printf '%s\n' "$rc" > "$OUT/process_exit.txt"
after=$(read_counter) || { after=UNREADABLE; reason=VOID_NIC_COUNTER_READ; }
printf '%s\n' "$after" > "$OUT/nic_missed_after.txt"
if [[ $after != "$before" && $reason == DURATION_COMPLETE ]]; then reason=VOID_NIC_MISSED; fi
if [[ $cleanup_status != CLEAN && $reason == DURATION_COMPLETE ]]; then reason=VOID_SHUTDOWN_TIMEOUT; fi
if [[ $reason == DURATION_COMPLETE && $rc != 0 && $rc != 130 ]]; then reason=VOID_ABNORMAL_EXIT; fi
if [[ $reason == DURATION_COMPLETE ]] && grep -qaE 'RXDISCONT|RFSTALL|Assertion.*failed' "$OUT/run.log"; then reason=VOID_RF_OR_ASSERT; fi
if [[ $reason == DURATION_COMPLETE ]]; then reason=RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE; fi
printf '%s\n' "$reason" > "$OUT/validity.txt"
date -u +%FT%TZ > "$OUT/end.txt"
printf 'verdict=%s cleanup=%s exit=%s OUTPUT=%s\n' "$reason" "$cleanup_status" "$rc" "$OUT"
