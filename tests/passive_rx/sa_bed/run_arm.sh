#!/usr/bin/env bash
# R13 sens6 live SA rfsim bed. Run through campaign.py; cwd is the run directory.
set -Eeuo pipefail
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$DIR/../../.." && pwd)
B=${BUILD:-$REPO/cmake_targets/ran_build/build}
SCENARIO=${1:-}; SIB=${2:-}; RECONF=${3:-}
case "$SCENARIO" in stable|bwp_switch|dedicated_change|cell_restart) ;; *) echo "usage: $0 {stable|bwp_switch|dedicated_change|cell_restart} {sa|sib1less} {on|off}" >&2; exit 2;; esac
case "$SIB" in sa|sib1less) ;; *) echo "arm must be sa or sib1less" >&2; exit 2;; esac
case "$RECONF" in on|off) ;; *) echo "flag must be on or off" >&2; exit 2;; esac
case ${R13_GPU:-0} in 0|1) ;; *) echo "R13_GPU must be 0 or 1" >&2; exit 2;; esac
[[ $(hostname -s) == sens6 ]] || { echo "R13 live bed is for sens6 only" >&2; exit 2; }
[[ ${ISAC_RX_BRANCH_FO:-} == '' ]] || { echo "ISAC_RX_BRANCH_FO must be unset" >&2; exit 2; }
for x in "$B/nr-softmodem" "$B/nr-uesoftmodem" "$B/librfsimulator.so" "$DIR/gnb_baseline.cfg" "$DIR/ue_active.cfg" "$DIR/ue_passive.cfg"; do
  [[ -e $x ]] || { echo "missing $x; build/configure first" >&2; exit 2; }
done
if [[ ${R13_GPU:-0} == 1 ]]; then
  for x in "$B/libldpc_cuda.so" "$B/libtd_cb0_gpu.so"; do [[ -e $x ]] || { echo "R13_GPU=1 but $x missing" >&2; exit 2; }; done
fi
for c in ss flock sudo python3 pgrep setsid timeout; do command -v "$c" >/dev/null || { echo "missing $c" >&2; exit 2; }; done
if [[ ${R13_GPU:-0} == 1 ]]; then
  command -v nvidia-smi >/dev/null && nvidia-smi -L >/dev/null || { echo "R13_GPU=1 but nvidia-smi cannot see a GPU" >&2; exit 2; }
fi
sudo -n true || { echo "passwordless sudo required for active UE TUN" >&2; exit 2; }
[[ ! -e /home/nicola/NICOLA/wt/rr-orchestration/BUSY ]] || { echo "shared host BUSY; wait for quiet slot" >&2; exit 2; }
exec 9>/tmp/td_measure.lock
flock -x -n 9 || { echo "/tmp/td_measure.lock held; another bed/build is active" >&2; exit 2; }
if pgrep -x nr-softmodem >/dev/null || pgrep -x nr-uesoftmodem >/dev/null; then echo "another softmodem is running" >&2; exit 2; fi
ss -H -ln -A sctp | grep -Eq '(:38412)[[:space:]]' || { echo "Open5GS AMF SCTP :38412 absent; check open5gs-amfd and amf.cfg template" >&2; exit 2; }
ss -H -lun 'sport = :2152' | grep -q . || { echo "Open5GS UPF UDP :2152 absent" >&2; exit 2; }
! ss -H -lun 'sport = :5201' | grep -q . || { echo "UDP :5201 busy (traffic receiver)" >&2; exit 2; }
for port in 4043 9090; do
  ! ss -H -ltn "sport = :$port" | grep -q . || { echo "TCP :$port busy" >&2; exit 2; }
done
[[ -d /sys/class/net/ogstun ]] || { echo "ogstun missing; configure Open5GS UPF/TUN" >&2; exit 2; }
AT=${R13_APPLY_AT_S:-45}; DUR=${R13_DURATION_S:-180}; [[ $AT =~ ^[0-9]+$ && $DUR =~ ^[0-9]+$ && $AT -ge 15 && $DUR -gt $((AT+35)) ]] || { echo "bad R13_APPLY_AT_S/R13_DURATION_S" >&2; exit 2; }
OUT=$PWD
[[ ! -e $OUT/events.jsonl ]] || { echo "run directory already contains events.jsonl" >&2; exit 2; }
mkdir -p gnb ue rx traffic
declare -A ACTIVE=()
event() { python3 - "$OUT/events.jsonl" "$1" "$SCENARIO" "$SIB" "$RECONF" <<'PY'
import json,os,socket,sys,time
with open(sys.argv[1], 'a') as f: f.write(json.dumps(dict(t=time.time(),event=sys.argv[2],scenario=sys.argv[3],sib=sys.argv[4],reconf=sys.argv[5],gpu=int(os.environ.get('R13_GPU','0')),host=socket.gethostname().split('.')[0]))+'\n')
PY
}
stop_pid() {
  local pid=${1:-} root=${2:-0}
  [[ -n $pid ]] || return 0
  if [[ $root == 1 ]]; then sudo -n kill -INT -- "-$pid" 2>/dev/null || true
  else kill -INT -- "-$pid" 2>/dev/null || true; fi
  for _ in {1..15}; do
    if ! kill -0 "$pid" 2>/dev/null; then unset "ACTIVE[$pid]"; return 0; fi
    sleep 1
  done
  echo "process group $pid did not stop after SIGINT; operator must inspect it" >&2
  return 1
}
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  event stop || true
  for pid in "${RX:-}" "${TX:-}" "${TR:-}"; do stop_pid "$pid" || rc=1; done
  stop_pid "${UE:-}" 1 || rc=1
  stop_pid "${GNB:-}" || rc=1
  for pid in "${!ACTIVE[@]}"; do stop_pid "$pid" || rc=1; done
  sleep 1  # let timestamping pipe readers drain before campaign.py scores the run
  exit "$rc"
}
trap cleanup EXIT INT TERM
start_gnb() {
  local cfg=$1 log=$2
  ( cd gnb; touch nrL1_stats.log nrMAC_stats.log nr_stats.log
    exec setsid "$B/nr-softmodem" -O "$DIR/$cfg" --rfsim --telnetsrv --telnetsrv.listenport 9090 --telnetsrv.shrmod ci \
      > >(python3 "$DIR/stamp.py" "$OUT/$log") 2>&1 ) &
  GNB=$!; ACTIVE[$GNB]=1
  wait_pattern "$OUT/$log" 'Received NGSetupResponse' 40 "gNB NG setup"
  for _ in {1..40}; do ss -H -ltn 'sport = :4043' | grep -q . && return 0; sleep 1; done
  echo "rfsim :4043 did not listen; see $log" >&2; return 1
}
wait_pattern() {
  local file=$1 pattern=$2 limit=$3 label=$4
  for ((i=0;i<limit;i++)); do grep -q "$pattern" "$file" 2>/dev/null && return 0; sleep 1; done
  echo "$label timed out after ${limit}s; see $file" >&2; return 1
}
start_ue() {
  local log=$1
  ( cd ue; touch nrL1_UE_stats-0.log
    exec setsid sudo -n "$B/nr-uesoftmodem" -O "$DIR/ue_active.cfg" --rfsim -C 3319680000 -r 106 --numerology 1 --band 78 --ssb 516 \
      > >(python3 "$DIR/stamp.py" "$OUT/$log") 2>&1 ) &
  UE=$!; ACTIVE[$UE]=1
  wait_pattern "$OUT/$log" 'RA procedure succeeded' 150 'UE random access'
  wait_pattern "$OUT/$log" 'PDU Session Establishment Accept' 150 'UE PDU session'
  UEIP=$(sed -n 's/.*UE IPv4: \([0-9.]*\).*/\1/p' "$OUT/$log" | tail -1)
  [[ -n $UEIP ]] || { echo "UE attached without IPv4 address in $log" >&2; return 1; }
}
start_traffic() {
  local phase=${1:-before}
  ( cd traffic; exec setsid python3 "$REPO/tests/passive_rx/udp_dl.py" recv --bind "$UEIP" --port 5201 --dur "$((DUR+180))" >"$OUT/traffic/${phase}_recv.log" 2>&1 ) &
  TR=$!; ACTIVE[$TR]=1; sleep 2
  kill -0 "$TR" 2>/dev/null || { echo "UDP receiver failed; see traffic/${phase}_recv.log" >&2; return 1; }
  ( cd traffic; exec setsid python3 "$REPO/tests/passive_rx/udp_dl.py" send --dst "$UEIP" --port 5201 --rate "${R13_TRAFFIC_RATE:-6M}" --dur "$((DUR+180))" >"$OUT/traffic/${phase}_send.log" 2>&1 ) &
  TX=$!; ACTIVE[$TX]=1
}
stop_traffic() { stop_pid "$TX"; stop_pid "$TR"; }
ci() {
  exec 3<>/dev/tcp/127.0.0.1/9090
  printf 'ci %s\n' "$1" >&3
  timeout 3 cat <&3 || true
  exec 3<&-
}
RX_ENV=(env "ISAC_RECONF=$([[ $RECONF == on ]] && echo 1 || echo 0)"
        "ISAC_BWP_TRACK=1"
        "ISAC_TD_IGNORE_SIB1=$([[ $SIB == sib1less ]] && echo 1 || echo 0)"
        "ISAC_METRICS_PATH=$OUT/metrics.jsonl" "ISAC_UECTX_PATH=$OUT/uectx.jsonl")
if [[ -n ${ISAC_OBS_PATH:-} ]]; then RX_ENV+=("ISAC_OBS_PATH=$ISAC_OBS_PATH"); fi
unset ISAC_RECONF ISAC_BWP_TRACK ISAC_TD_IGNORE_SIB1 ISAC_METRICS_PATH ISAC_UECTX_PATH ISAC_OBS_PATH
unset ISAC_RX_BRANCH_FO
export LD_LIBRARY_PATH="$B${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
RX_ARGS=()
[[ ${R13_GPU:-0} == 1 ]] && RX_ARGS=(--loader.ldpc.shlibversion _cuda)
event start
start_gnb gnb_baseline.cfg gnb/before.log
start_ue ue/before.log
start_traffic before
( cd rx; touch nrL1_UE_stats-0.log
  exec setsid "${RX_ENV[@]}" "$B/nr-uesoftmodem" --passive-rx --rfsim -O "$DIR/ue_passive.cfg" -C 3319680000 -r 106 --numerology 1 --band 78 --ssb 516 \
    --ue-nb-ant-rx "${R13_RX_ANT:-4}" "${RX_ARGS[@]}" > >(python3 "$DIR/stamp.py" "$OUT/rx/rx.log") 2>&1 ) &
RX=$!; ACTIVE[$RX]=1
wait_pattern "$OUT/rx/rx.log" 'SIB1 decoded\|DCI 1_1 length locked\|CORESET VERIFIED' 150 'passive acquisition'
if [[ $SIB == sa ]]; then wait_pattern "$OUT/rx/rx.log" 'SIB1 decoded' 150 'SA SIB1 decode'; fi
wait_pattern "$OUT/rx/rx.log" 'DCI 1_1 length locked' 180 'baseline DCI 1_1 lock'
wait_pattern "$OUT/rx/rx.log" 'Technique D CONVERGED' 180 'baseline Technique D convergence'
event ready
sleep "$AT"
kill -0 "$RX" || { echo "passive receiver exited before scenario" >&2; exit 1; }
case "$SCENARIO" in
  stable) event apply ;;
  bwp_switch)
    ci get_current_bwp >"$OUT/telnet.log" 2>&1
    event apply
    ci 'trigger_bwp_switch 2' >>"$OUT/telnet.log" 2>&1
    grep -q 'triggered BWP switch' "$OUT/telnet.log" || { echo "BWP trigger not acknowledged; see telnet.log" >&2; exit 1; }
    ;;
  dedicated_change|cell_restart)
    event apply
    stop_traffic
    stop_pid "$UE" 1
    stop_pid "$GNB"
    if [[ $SCENARIO == dedicated_change ]]; then start_gnb gnb_dedicated.cfg gnb/after.log
    else start_gnb gnb_cell.cfg gnb/after.log; fi
    start_ue ue/after.log
    start_traffic after
    ;;
esac
event applied
sleep "$((DUR-AT))"
kill -0 "$RX" 2>/dev/null || { echo "passive receiver exited; within-stream recovery unscorable" >&2; exit 1; }
event end
