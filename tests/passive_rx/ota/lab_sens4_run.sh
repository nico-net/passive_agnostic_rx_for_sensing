#!/bin/bash
# One lab-cell OTA run, orchestrated from sens6: sens4 gNB + phone traffic start with the run, the passive receiver
# (sens6, X410) captures, then everything stops in reverse order with SIGINT and the logs land in one run directory.
#
# usage: lab_sens4_run.sh [--dry-run] ARM        settings: lab_sens4_run.cfg next to this script (CFG=<file> to override);
#                                                any cfg key can be overridden from the environment (e.g. DUR=600).
# Steps: 1 read-only checks (testbed lock idle in "stop_all", no gNB, phones on adb, receiver preflight)
#        2 take the sens4 testbed lock (tmux s6lab_lock, note "lab_sens4_run ... frees~=") so the testbed controller does not act
#        3 gNB in tmux s6lab_gnb (stdout tee'd to a timestamped log), wait for the AMF connection
#        4 phones: PLMN + 10.45.x address, else one airplane-mode toggle; iperf3 servers on sens4 (looped one-shot, 10.42.0.1:5204/5205)
#        5 traffic loops (DL -R / UL phases, ping fallback), then the receiver through run_ota_s6.sh
#        6 teardown, reverse order, SIGINT only: receiver (launcher), phone iperf3, iperf servers, gNB (tmux C-c, exit confirmed),
#          airplane mode restored, lock released; logs collected to OUTBASE/<ARM>_lab_<date>/
# --dry-run: runs the read-only checks and prints every command that would change something; changes nothing.
# Never SIGKILL: the gNB drives an OFH radio and the receiver an X410 (claim hygiene).
set -u
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$H/../../.." && pwd)
DRY=0; [ "${1:-}" = "--dry-run" ] && { DRY=1; shift; }
ARM=${1:-}; [ -n "$ARM" ] || { sed -n 2,16p "$0"; exit 2; }
CFG=${CFG:-$H/lab_sens4_run.cfg}
KEYS="GNB_HOST GNB_CMD GNB_OVERRIDES GNB_PCI GNB_LOGDIR GNB_READY_RE GNB_READY_S TESTBED_LOCK TMUX_PREFIX IPERF_BIND PHONES PLMN UE_IP_RE ATTACH_S PHASE_S PHASE_BW RX_LAUNCHER RX_ENVFILE RX_START_DELAY_S DUR OUTBASE"
for k in $KEYS; do eval "_o_$k=\${$k-__unset__}"; done
# shellcheck disable=SC1090
. "$CFG"
for k in $KEYS; do eval "[ \"\$_o_$k\" != __unset__ ] && $k=\$_o_$k"; done
TS=$(date +%Y%m%d_%H%M%S); RUN=$OUTBASE/${ARM}_lab_$TS; GNBLOG=$GNB_LOGDIR/gnb_s6lab_$TS.stdout.log; GNBSCHED=$GNB_LOGDIR/gnb_s6lab_$TS.log; GNBPCAP=$GNB_LOGDIR/gnb_s6lab_$TS.mac.pcap
GNB_FULL="$GNB_CMD $(echo "${GNB_OVERRIDES:-}" | sed "s#@LOG@#$GNBSCHED#; s#@PCAP@#$GNBPCAP#")"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8"
log() { echo "$(date +%T) $*"; }
r()   { $SSH "$1" "$2"; }                                   # read-only remote command
m()   { if [ $DRY = 1 ]; then echo "  DRY $1: $2"; else $SSH "$1" "$2"; fi; }   # mutating remote command
die() { log "ABORT: $*"; exit 3; }

# ---------------------------------------------------------------- 1. read-only checks
log "== checks (read-only)"
st=$(r "$GNB_HOST" "python3 -c 'import fcntl,sys
h=open(\"$TESTBED_LOCK\",\"a+\")
try: fcntl.flock(h,fcntl.LOCK_SH|fcntl.LOCK_NB); held=0
except OSError: held=1
h.seek(0); print(held, h.read().strip())'") || die "cannot read $TESTBED_LOCK on $GNB_HOST"
held=${st%% *}; note=${st#* }
[ "$held" = 0 ] || die "testbed lock held by a writer: $note"
case "$note" in stop_all*) log "ok testbed lock idle: $note";; *) die "testbed lock not in the idle stop_all state: '$note'";; esac
g=$(r "$GNB_HOST" "pgrep -a -x gnb; tmux ls 2>/dev/null | grep -E '^${TMUX_PREFIX}_' || true")
[ -z "$g" ] && log "ok no gNB / no $TMUX_PREFIX tmux on $GNB_HOST" || die "already running on $GNB_HOST: $g"
r "$GNB_HOST" "sudo -n true" || die "no passwordless sudo on $GNB_HOST (the gNB command uses sudo)"
r "$GNB_HOST" "ip -4 -br addr | grep -q ' $IPERF_BIND/'" && log "ok $IPERF_BIND on $GNB_HOST" || die "$IPERF_BIND not on $GNB_HOST"
declare -A AIR0
for p in $PHONES; do IFS=: read -r name host adb ser ip3 port <<< "$p"
  st=$(r "$host" "$adb devices | grep -w '$ser' | awk '{print \$2}'")
  [ "$st" = device ] || die "phone $name ($ser) not on adb at $host (state '$st')"
  AIR0[$name]=$(r "$host" "$adb -s $ser shell settings get global airplane_mode_on" | tr -d '\r')
  r "$host" "$adb -s $ser shell 'command -v $ip3 >/dev/null || ls $ip3 >/dev/null'" >/dev/null 2>&1 || die "phone $name: $ip3 missing"
  log "ok phone $name $ser on $host (airplane=${AIR0[$name]}, operator=$(r "$host" "$adb -s $ser shell getprop gsm.operator.numeric" | tr -d '\r'))"
done
RXL=$ROOT/$RX_LAUNCHER
ENVFILE=$ROOT/$RX_ENVFILE DUR=$DUR "$RXL" check || { [ $DRY = 1 ] && log "WARN receiver preflight not clean (see above)" || die "receiver preflight not clean (fix it, see the sudo commands above)"; }

# ---------------------------------------------------------------- ground truth (lab, validation only; never receiver input)
ground_truth() {
  local X; X=$(ls -d "$RUN"/*/export 2>/dev/null | head -1); local G=$RUN/gnb/$(basename "$GNBSCHED") M="systemd-run --user --scope --quiet -p MemoryMax=3G -p MemorySwapMax=1G choom -n 1000 --"
  [ -n "$X" ] && [ -s "$G" ] || { log "ground truth skipped (export '$X', gNB log '$G')"; return; }
  $M python3 -I "$ROOT/tools/mobisys/ocudu_gt.py" --log "$G" --pci "$GNB_PCI" --out "$RUN/gnb/sched.csv" > "$RUN/gnb/ocudu_gt.log" 2>&1 || log "WARN ocudu_gt failed"
  $M python3 -I "$ROOT/tools/mobisys/groundtruth.py" --rx "$X" --gnb-csv "$RUN/gnb/sched.csv" --out "$X/groundtruth" 2>&1 | tail -1
  $M python3 -I "$ROOT/tools/mobisys/ul_groundtruth.py" --rx "$X" --gnb-csv "$RUN/gnb/sched.csv" --rx-log "$(dirname "$X")/run.log" --out "$X/groundtruth" 2>&1 | tail -1
  $M python3 -I "$ROOT/tools/mobisys/plots.py" --lab "$X" --out "$RUN/plots" > "$RUN/plots.log" 2>&1 || log "WARN plots failed"
}

# ---------------------------------------------------------------- teardown (reverse order, SIGINT only)
TRAFFIC_PIDS=(); RXPID=""; TOGGLED=""
teardown() {
  trap - INT TERM
  log "== teardown"
  [ -n "$RXPID" ] && kill -0 "$RXPID" 2>/dev/null && { log "waiting for the receiver launcher (it stops the receiver with SIGINT)"; wait "$RXPID"; }
  for t in "${TRAFFIC_PIDS[@]}"; do kill -INT "$t" 2>/dev/null; done
  for p in $PHONES; do IFS=: read -r name host adb ser ip3 port <<< "$p"; m "$host" "$adb -s $ser shell 'pkill -INT iperf3; pkill -INT ping' 2>/dev/null || true"; done
  m "$GNB_HOST" "tmux send-keys -t ${TMUX_PREFIX}_iperf C-c 2>/dev/null; sleep 1; tmux kill-session -t ${TMUX_PREFIX}_iperf 2>/dev/null; true"
  m "$GNB_HOST" "tmux send-keys -t ${TMUX_PREFIX}_gnb C-c"
  if [ $DRY = 0 ]; then
    for _ in $(seq 1 30); do r "$GNB_HOST" "pgrep -x gnb >/dev/null" || break; sleep 2; done
    if r "$GNB_HOST" "pgrep -x gnb >/dev/null"; then m "$GNB_HOST" "tmux send-keys -t ${TMUX_PREFIX}_gnb C-c"; sleep 30
      r "$GNB_HOST" "pgrep -x gnb >/dev/null" && log "WARNING: gNB still alive after 2 x C-c -- left to the operator (tmux ${TMUX_PREFIX}_gnb)" || log "gNB exited"
    else log "gNB exited"; m "$GNB_HOST" "tmux kill-session -t ${TMUX_PREFIX}_gnb 2>/dev/null; true"; fi
  fi
  for p in $PHONES; do IFS=: read -r name host adb ser ip3 port <<< "$p"
    if [ -n "${AIR0[$name]:-}" ]; then m "$host" "$adb -s $ser shell cmd connectivity airplane-mode $([ "${AIR0[$name]}" = 1 ] && echo enable || echo disable)"; fi; done
  m "$GNB_HOST" "tmux send-keys -t ${TMUX_PREFIX}_lock C-c 2>/dev/null; sleep 1; tmux kill-session -t ${TMUX_PREFIX}_lock 2>/dev/null; printf '%s\\n' '$note' > $TESTBED_LOCK"   # the idle note found at the start
  if [ $DRY = 0 ]; then mkdir -p "$RUN/gnb"; for f in $GNBLOG $GNBSCHED; do scp -q -o BatchMode=yes "$GNB_HOST:$f" "$RUN/gnb/" 2>/dev/null || log "WARN not copied: $f"; done
    log "gNB MAC pcap left on $GNB_HOST: $GNBPCAP ($(r "$GNB_HOST" "du -h $GNBPCAP 2>/dev/null | cut -f1"))"
    cp "$CFG" "$RUN/"; ground_truth; log "run directory $RUN"; fi
}

# ---------------------------------------------------------------- 2. testbed lock
TOTAL=$(( DUR + RX_START_DELAY_S + 30 ))
FREES=$(date -u -d "+$(( TOTAL + GNB_READY_S + 300 )) sec" +%Y-%m-%dT%H:%M:%S+00:00)
[ $DRY = 0 ] && mkdir -p "$RUN" && trap 'teardown; exit 130' INT TERM
m "$GNB_HOST" "cat > /tmp/${TMUX_PREFIX}_lock.sh <<'EOS'
exec 9>>\"\$1\"; flock -x -n 9 || exit 1
printf 'lab_sens4_run pid=%s since=%s frees~=%s\\n' \$\$ \"\$(date -u +%Y-%m-%dT%H:%M:%S+00:00)\" \"\$2\" > \"\$1\"
exec sleep infinity
EOS
tmux new-session -d -s ${TMUX_PREFIX}_lock bash /tmp/${TMUX_PREFIX}_lock.sh $TESTBED_LOCK $FREES"
[ $DRY = 0 ] && { sleep 2; r "$GNB_HOST" "tmux has-session -t ${TMUX_PREFIX}_lock 2>/dev/null && grep -q lab_sens4_run $TESTBED_LOCK" || { teardown; die "could not take the testbed lock"; }; log "testbed lock taken until ~$FREES"; }

# ---------------------------------------------------------------- 3. gNB
m "$GNB_HOST" "mkdir -p $GNB_LOGDIR && tmux new-session -d -s ${TMUX_PREFIX}_gnb \"$GNB_FULL 2>&1 | tee -i $GNBLOG\""
if [ $DRY = 0 ]; then
  ok=0; for _ in $(seq 1 $(( GNB_READY_S / 3 ))); do r "$GNB_HOST" "grep -qsE '$GNB_READY_RE' $GNBLOG $GNBSCHED" && { ok=1; break; }; sleep 3; done
  [ $ok = 1 ] || { log "gNB not ready in $GNB_READY_S s: $(r "$GNB_HOST" "tail -3 $GNBLOG")"; teardown; die "gNB not ready"; }
  log "gNB ready: $(r "$GNB_HOST" "grep -hs -m1 -E '$GNB_READY_RE' $GNBLOG $GNBSCHED | head -1")"
fi

# ---------------------------------------------------------------- 4. phones + iperf servers
for p in $PHONES; do IFS=: read -r name host adb ser ip3 port <<< "$p"
  att() { r "$host" "$adb -s $ser shell 'getprop gsm.operator.numeric; ip -4 addr'" | tr -d '\r' | grep -q "$PLMN" && r "$host" "$adb -s $ser shell ip -4 addr" | grep -qE "inet $UE_IP_RE"; }
  if [ $DRY = 1 ]; then echo "  DRY $host: (if not on $PLMN with a $UE_IP_RE address) $adb -s $ser shell cmd connectivity airplane-mode enable; sleep 3; ... disable; wait ${ATTACH_S}s"; continue; fi
  if ! att; then log "phone $name not attached to $PLMN: airplane toggle"; TOGGLED="$TOGGLED $name"
    m "$host" "$adb -s $ser shell cmd connectivity airplane-mode enable; sleep 3; $adb -s $ser shell cmd connectivity airplane-mode disable"
    for _ in $(seq 1 $(( ATTACH_S / 5 ))); do att && break; sleep 5; done; fi
  att && log "phone $name attached: $(r "$host" "$adb -s $ser shell ip -4 addr" | tr -d '\r' | grep -oE "inet $UE_IP_RE[0-9.]+" | head -1)" || log "WARN phone $name not attached to $PLMN (traffic falls back to ping, which will fail too)"
done
SRV="tmux new-session -d -s ${TMUX_PREFIX}_iperf"; first=1
for p in $PHONES; do IFS=: read -r name host adb ser ip3 port <<< "$p"
  [ "$port" = 5201 ] && die "port 5201 is reserved for another server"
  c="while :; do iperf3 -s -1 -B $IPERF_BIND -p $port; done"
  if [ $first = 1 ]; then SRV="$SRV \"$c\""; first=0; else SRV="$SRV \\; new-window \"$c\""; fi
done
m "$GNB_HOST" "$SRV"

# ---------------------------------------------------------------- 5. traffic, then the receiver
traffic() { # name host adb ser ip3 port
  local name=$1 host=$2 adb=$3 ser=$4 ip3=$5 port=$6 end=$(( $(date +%s) + TOTAL )) dir=-R k=0 f
  while [ "$(date +%s)" -lt "$end" ]; do k=$((k+1)); f=$RUN/traffic_${name}.log
    echo "$(date +%T) phase $k ${dir:-UL}${dir:+DL}" >> "$f"
    $SSH "$host" "$adb -s $ser shell $ip3 -c $IPERF_BIND -p $port -t $PHASE_S -i 5 $dir $PHASE_BW" >> "$f" 2>&1 \
      || { echo "$(date +%T) iperf3 failed -> ping" >> "$f"; $SSH "$host" "$adb -s $ser shell ping -c $PHASE_S -i 1 $IPERF_BIND" >> "$f" 2>&1; }
    [ -n "$dir" ] && dir= || dir=-R
  done; }
if [ $DRY = 1 ]; then
  for p in $PHONES; do IFS=: read -r name host adb ser ip3 port <<< "$p"; echo "  DRY $host: loop ${TOTAL}s: $adb -s $ser shell $ip3 -c $IPERF_BIND -p $port -t $PHASE_S -i 5 [-R | UL] $PHASE_BW   (ping $IPERF_BIND on failure)"; done
  echo "  DRY sens6: sleep $RX_START_DELAY_S; ENVFILE=$ROOT/$RX_ENVFILE DUR=$DUR $RXL run $ARM"
  ENVFILE=$ROOT/$RX_ENVFILE DUR=$DUR "$RXL" dry-run "$ARM" | sed -n '/== launch/,$p'
  teardown; log "dry run done (nothing was changed)"; exit 0
fi
for p in $PHONES; do IFS=: read -r name host adb ser ip3 port <<< "$p"; traffic "$name" "$host" "$adb" "$ser" "$ip3" "$port" & TRAFFIC_PIDS+=($!); done
sleep "$RX_START_DELAY_S"
( OUTBASE=$RUN ENVFILE=$ROOT/$RX_ENVFILE DUR=$DUR "$RXL" run "$ARM" > "$RUN/receiver_launcher.log" 2>&1 ) & RXPID=$!
log "receiver started (launcher pid $RXPID, log $RUN/receiver_launcher.log)"
wait "$RXPID"; rc=$?; RXPID=""
log "receiver launcher rc=$rc"
teardown
exit $rc
