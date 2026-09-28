#!/usr/bin/env bash
# OCUDU gNB + srsUE (attached) + OAI --passive-rx (fully agnostic) over ZMQ, one host, ideal channel.
#
#   gNB(:2000 tx, ->:2101 rx) <-> ocudu/ocudu_zmq_broker.py <-> srsUE(:2001 tx, ->:2100 rx)
#                                        \--> passive OAI UE (->:2200 rx, :2201 tx sink), sees DL+UL
#
# Usage:  run_ocudu_passive.sh [DUR_S=300] [RUN_DIR=/tmp/ocudu_passive/<ts>]
#         run_ocudu_passive.sh stop            # tear down a previous run (pids in /tmp/ocudu_passive/pids)
# Env:    PX=1          start the passive receiver (0 = gNB+srsUE only)
#         PX_FIRST=0    1 = start the passive first and start srsUE only once the passive has decoded SIB1
#                       (so PRACH/RAR/Msg4 are on air while the passive is already listening)
#         GNB_ENV=""    env for the gNB, e.g. "ISAC_OCUDU_TEST_DL_RA_TYPE0=1" (needs GNB_BIN=ocudu-test build)
#         SRSUE_ENV=""  env for srsUE, e.g. "SRSUE_ADVERTISE_256QAM=1".  PX_ENV="" env for the passive, e.g. "ISAC_PUSCH_DIAG=1"
#         PX_BIN=""     alternate passive receiver binary (for fixed/baseline A/B without modifying either build tree)
#         NO_UE=0       1 = no srsUE; the broker feeds the gNB zero UL (passive SSB/SIB1 bring-up only)
#         DL_RATE=2M    host->UE UDP rate (udp_dl.py); 0 disables.  UL_PING=0.2  UE->gateway ping interval, 0 disables
#         GNB_EXTRA=""  extra OCUDU CLI args (test-matrix knobs, e.g. "cell_cfg --pdsch.mcs_table qam64")
#         SRSUE_EXTRA="" extra srsUE args (e.g. "--rf.tx_gain=0").  PX_EXTRA="" extra nr-uesoftmodem args.   BROKER_EXTRA=""  extra broker args
# Never edit this file while it runs: bash reads scripts incrementally (a mid-run edit killed m2).
# Rules baked in: gNB first, wait for AMF, then UE; gNB and srsUE are only ever stopped TOGETHER
# (a survivor of a one-sided kill wedges on its lockstep ZMQ receive). The passive can be stopped or
# restarted alone -- the broker drops its lane and keeps the gNB<->UE loop running.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OC="$HERE/ocudu"
BUILD="$HERE/../../cmake_targets/ran_build/build"
PX_BIN="${PX_BIN:-$BUILD/nr-uesoftmodem}"
GNB_BIN="${GNB_BIN:-/home/sens/NICOLA/repos/ocudu/build/apps/gnb/gnb}"   # isac-test-knobs: /home/sens/NICOLA/repos/ocudu-test/build/apps/gnb/gnb
SRSUE_BIN=/home/sens/NICOLA/repos/srs-ue/build/srsue/src/srsue
BASE=/tmp/ocudu_passive
PIDS=$BASE/pids
NS=ue1
OWNED_HELPER="$HERE/ocudu_owned_process.py"
DEFER_SIGNALS=0
DEFERRED_SIGNAL=""
STOPPING=0

identity_is_registered() {
  local wanted_pid="$1" wanted_start="$2" i name pid start
  for ((i=0; i<${OWNED_COUNT:-0}; i++)); do
    name="OWNED_PID_$i"; pid="${!name:-}"
    name="OWNED_START_$i"; start="${!name:-}"
    [ "$pid" = "$wanted_pid" ] && [ "$start" = "$wanted_start" ] && return 0
  done
  return 1
}

check_recovery_identity() {
  local pid="$1" start="$2"
  [[ "$pid" =~ ^[0-9]+$ && "$start" =~ ^[0-9]+$ ]] || return 2
  if read_proc_identity "$pid" 2>/dev/null; then
    [ "$PROC_START" = "$start" ] && return 0
    echo "REFUSING recovery: PID $pid start-time mismatch; preserve $PIDS" >&2
    return 1
  fi
  [ ! -e "/proc/$pid" ]
}

recover_unregistered_claim_identity() {
  local pid="$1" start="$2" attempt supervisor_index supervisor supervisor_start exited=0
  shift 2
  [[ "$pid" =~ ^[0-9]+$ && "$start" =~ ^[0-9]+$ && $(( $# % 2 )) = 0 ]] || return 2
  identity_is_registered "$pid" "$start" && return 0
  check_recovery_identity "$pid" "$start" || return 1
  local -a supervisors=("$@")
  for ((attempt=0; attempt<${#supervisors[@]}; attempt+=2)); do
    check_recovery_identity "${supervisors[attempt]}" "${supervisors[attempt+1]}" || return 1
  done
  if [ -e "/proc/$pid" ]; then
    echo "terminating unregistered claimed launch identity $pid for pending-launch recovery" >&2
    signal_recorded "$pid" "$start" SIGKILL strict || return 1
    # Never resume sudo while its unregistered child could still execute.
    # A zombie is permission to let its exact supervisors reap, not completion.
    for ((attempt=0; attempt<1000; attempt++)); do
      check_recovery_identity "$pid" "$start" || return 1
      if [ ! -e "/proc/$pid" ] || [[ "$PROC_STATE" = Z || "$PROC_STATE" = X || "$PROC_STATE" = x ]]; then
        exited=1
        break
      fi
      sleep 0.01
    done
    [ "$exited" = 1 ] || return 1
    for attempt in 1 2 3 4 5; do
      # Recheck because sudo can process the earlier stop notification late.
      for ((supervisor_index=0; supervisor_index<${#supervisors[@]}; supervisor_index+=2)); do
        supervisor="${supervisors[supervisor_index]}"; supervisor_start="${supervisors[supervisor_index+1]}"
        [ "$supervisor" = "$pid" ] && continue
        check_recovery_identity "$supervisor" "$supervisor_start" || return 1
        if [ -e "/proc/$supervisor" ] && [[ "$PROC_STATE" = T || "$PROC_STATE" = t ]]; then
          signal_recorded "$supervisor" "$supervisor_start" SIGCONT strict || return 1
        fi
      done
      wait "$pid" 2>/dev/null || true
      check_recovery_identity "$pid" "$start" || return 1
      [ ! -e "/proc/$pid" ] && return 0
      sleep 1
    done
    return 1
  fi
  return 0
}

recover_pending_launch_claims() {
  [ "${LAUNCH_PENDING:-0}" = 1 ] || return 0
  local monitor_claim="${PENDING_MONITOR_CLAIM:-}" target_claim="${PENDING_TARGET_CLAIM:-}"
  local pid start parent parent_start wrapper_pid="${PENDING_WRAPPER_PID:-0}"
  local wrapper_start="${PENDING_WRAPPER_START:-0}"
  local monitor_pid="" monitor_start=""
  local -a supervisors=()
  local evidence=0
  [ -n "$monitor_claim" ] && [ -n "$target_claim" ] || {
    echo "REFUSING cleanup: pending launch lacks durable claim paths; preserve $PIDS" >&2
    return 1
  }
  if [ "$wrapper_pid" != 0 ] || [ "$wrapper_start" != 0 ]; then
    evidence=1
    recover_unregistered_claim_identity "$wrapper_pid" "$wrapper_start" || return 1
  fi
  if [ -s "$monitor_claim" ]; then
    evidence=1
    read -r pid start < "$monitor_claim" || return 1
    monitor_pid="$pid"; monitor_start="$start"
    recover_unregistered_claim_identity "$pid" "$start" || return 1
  fi
  if [ -s "$target_claim" ]; then
    evidence=1
    read -r pid start parent parent_start < "$target_claim" || return 1
    if [ -n "${parent:-}" ] || [ -n "${parent_start:-}" ]; then
      supervisors+=("$parent" "$parent_start")
    fi
    [ -n "$monitor_pid" ] && supervisors+=("$monitor_pid" "$monitor_start")
    if [ "$wrapper_pid" != 0 ] || [ "$wrapper_start" != 0 ]; then
      supervisors+=("$wrapper_pid" "$wrapper_start")
    fi
    recover_unregistered_claim_identity "$pid" "$start" "${supervisors[@]}" || return 1
    if [ -n "${parent:-}" ] || [ -n "${parent_start:-}" ]; then
      recover_unregistered_claim_identity "$parent" "$parent_start" "${supervisors[@]}" || return 1
    fi
  fi
  if [ "$evidence" != 1 ]; then
    echo "REFUSING cleanup: pending launch has no durable wrapper/monitor/target identity; preserve $PIDS" >&2
    return 1
  fi
}

stop_all() {
  [ "$STOPPING" = 1 ] && return 0
  STOPPING=1
  [ -f "$PIDS" ] || { echo "no $PIDS"; STOPPING=0; return 0; }
  local stop_lock_fd
  exec {stop_lock_fd}>> "$PIDS.lock" || { STOPPING=0; return 2; }
  flock -x "$stop_lock_fd" || { exec {stop_lock_fd}>&-; STOPPING=0; return 2; }
  [ -f "$PIDS" ] || { finish_stop 0; return 0; }
  # shellcheck disable=SC1090
  . "$PIDS" || { echo "REFUSING cleanup: cannot read $PIDS" >&2; finish_stop 2; return 2; }
  echo "--- stopping (traffic, passive, then gNB+srsUE together, then broker) ---"
  local i name pid start role group rc=0 alive=1 attempt had_nsadd=0
  local count="${OWNED_COUNT:-}"
  if [[ ! "$count" =~ ^[0-9]+$ ]]; then
    echo "REFUSING cleanup: $PIDS has no valid owned-process registry; preserve it" >&2
    finish_stop 2
    return 2
  fi
  for ((i=0; i<count; i++)); do
    name="OWNED_PID_$i"; pid="${!name:-}"
    name="OWNED_START_$i"; start="${!name:-}"
    name="OWNED_ROLE_$i"; role="${!name:-}"
    case "$role" in PX|GNB|UE|BROKER|TRAFFIC|NSADD) ;; *)
      echo "REFUSING cleanup: incomplete or unknown identity role at $i; preserve $PIDS" >&2
      finish_stop 2
      return 2
    esac
    [ "$role" = NSADD ] && had_nsadd=1
    if [[ ! "$pid" =~ ^[0-9]+$ || ! "$start" =~ ^[0-9]+$ ]]; then
      echo "REFUSING cleanup: invalid PID identity $i; preserve $PIDS" >&2
      finish_stop 2
      return 2
    fi
  done
  recover_pending_launch_claims || { finish_stop 1; return 1; }
  STOPPING=1
  for group in NSADD TRAFFIC PX RADIO BROKER; do
    for ((i=0; i<count; i++)); do
      name="OWNED_ROLE_$i"; role="${!name}"
      name="OWNED_PID_$i"; pid="${!name}"
      name="OWNED_START_$i"; start="${!name}"
      case "$group:$role" in
        TRAFFIC:TRAFFIC) signal_recorded "$pid" "$start" SIGTERM || rc=1 ;;
        PX:PX) signal_recorded "$pid" "$start" SIGINT || rc=1 ;;
        RADIO:GNB|RADIO:UE) signal_recorded "$pid" "$start" SIGINT || rc=1 ;;
        BROKER:BROKER) signal_recorded "$pid" "$start" SIGTERM || rc=1 ;;
        NSADD:NSADD)
          if [ "${NSADD_DONE:-0}" != 1 ]; then signal_recorded "$pid" "$start" SIGTERM || rc=1; fi
          ;;
        *) continue ;;
      esac
    done
  done
  sleep 4
  for ((i=0; i<count; i++)); do
    name="OWNED_ROLE_$i"; role="${!name}"
    [ "$role" = NSADD ] && [ "${NSADD_DONE:-0}" = 1 ] && continue
    name="OWNED_PID_$i"; pid="${!name}"
    name="OWNED_START_$i"; start="${!name}"
    signal_recorded "$pid" "$start" SIGKILL || rc=1
  done
  for attempt in 1 2 3 4 5; do
    alive=0
    for ((i=0; i<count; i++)); do
      name="OWNED_ROLE_$i"
      if [ "${!name}" = NSADD ] && [ "${NSADD_DONE:-0}" = 1 ]; then continue; fi
      name="OWNED_PID_$i"; pid="${!name}"
      name="OWNED_START_$i"; start="${!name}"
      recorded_alive "$pid" "$start" && alive=1
    done
    [ "$alive" -eq 0 ] && break
    sleep 1
  done
  if [ "$alive" -ne 0 ]; then
    echo "REFUSING cleanup: one or more recorded processes remain; preserve $PIDS for manual recovery" >&2
    finish_stop 1
    return 1
  fi
  if [ "${LAUNCH_PENDING:-0}" = 1 ]; then
    if ! python3 "$OWNED_HELPER" registry-append --locked "$PIDS" 'LAUNCH_PENDING=0'; then
      echo "REFUSING cleanup: could not durably clear recovered launch state; preserve $PIDS" >&2
      finish_stop 1
      return 1
    fi
    LAUNCH_PENDING=0
  fi
  if [ "$had_nsadd" = 1 ] && [ "${NSADD_DONE:-0}" != 1 ]; then
    if ! python3 "$OWNED_HELPER" registry-append --locked "$PIDS" 'NSADD_DONE=1'; then
      echo "REFUSING cleanup: could not durably record namespace-add completion; preserve $PIDS" >&2
      finish_stop 1
      return 1
    fi
    NSADD_DONE=1
  fi
  if [ "$had_nsadd" = 1 ] && [ "${NS_CREATED:-0}" != 1 ] \
     && sudo -n ip netns list | grep -qw "$NS"; then
    echo "REFUSING cleanup: $NS exists but its successful add was not recorded; preserve $PIDS for inspection" >&2
    finish_stop 1
    return 1
  fi
  if [ "${NS_CREATED:-0}" = 1 ]; then
    local namespaces
    if ! namespaces=$(sudo -n ip netns list); then
      echo "REFUSING cleanup: could not inspect namespace state; preserve $PIDS" >&2
      finish_stop 1
      return 1
    fi
    if grep -qw "$NS" <<< "$namespaces" && ! sudo -n ip netns del "$NS"; then
      echo "REFUSING cleanup: could not delete invocation-owned namespace $NS; preserve $PIDS for manual recovery" >&2
      finish_stop 1
      return 1
    fi
  fi
  rm -f "$PIDS" || { echo "REFUSING cleanup: could not remove $PIDS" >&2; finish_stop 1; return 1; }
  finish_stop "$rc"
  return "$rc"
}

finish_stop() {
  local status="$1"
  if [[ "${stop_lock_fd:-}" =~ ^[0-9]+$ ]]; then
    flock -u "$stop_lock_fd" || true
    exec {stop_lock_fd}>&-
  fi
  STOPPING=0
  return "$status"
}

read_proc_identity() {
  local pid="$1" stat
  local -a fields=()
  PROC_STATE=""; PROC_START=""
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  stat=$(< "/proc/$pid/stat") || return 1
  # comm (field 2) may contain spaces or ')'; fields below begin at field 3.
  read -r -a fields <<< "${stat##*) }"
  [[ "${fields[0]:-}" =~ ^[A-Za-z]$ && "${fields[19]:-}" =~ ^[0-9]+$ ]] || return 1
  PROC_STATE="${fields[0]}"; PROC_START="${fields[19]}"
}

proc_starttime() {
  read_proc_identity "$1" || return 1
  printf '%s\n' "$PROC_START"
}

proc_has_state() {
  local pid="$1" expected="$2" wanted="$3"
  read_proc_identity "$pid" 2>/dev/null || return 1
  [ "$PROC_START" = "$expected" ] || return 1
  case "$wanted:$PROC_STATE" in
    stopped:T|stopped:t) return 0 ;;
    active:Z|active:X|active:x) return 1 ;;
    active:*) return 0 ;;
  esac
  return 1
}

signal_recorded() {
  local pid="$1" expected="$2" sig="$3" strict="${4:-}"
  sudo -n python3 "$OWNED_HELPER" signal "$pid" "$expected" "$sig" >/dev/null 2>&1 && return 0
  if [ "$strict" = strict ]; then
    echo "pidfd signal failed during recovery for owned PID $pid ($sig); preserve $PIDS" >&2
    return 1
  fi
  recorded_alive "$pid" "$expected" && { echo "pidfd signal failed for owned PID $pid ($sig)" >&2; return 1; }
  return 0
}

recorded_alive() {
  local pid="$1" expected="$2" current
  [[ "$pid" =~ ^[0-9]+$ && "$expected" =~ ^[0-9]+$ ]] || return 1
  current=$(proc_starttime "$pid" 2>/dev/null) || return 1
  [ "$current" = "$expected" ]
}

record_identity() {
  local role="$1" pid="$2" start="$3" count="${OWNED_COUNT:-0}" i name prior_pid prior_start text
  [[ "$pid" =~ ^[0-9]+$ && "$start" =~ ^[0-9]+$ && "$count" =~ ^[0-9]+$ ]] || return 1
  for ((i=0; i<count; i++)); do
    name="OWNED_PID_$i"; prior_pid="${!name:-}"
    name="OWNED_START_$i"; prior_start="${!name:-}"
    if [ "$pid" = "$prior_pid" ] && [ "$start" = "$prior_start" ]; then return 0; fi
  done
  text=$(printf 'OWNED_PID_%d=%q\nOWNED_START_%d=%q\nOWNED_ROLE_%d=%q\nOWNED_COUNT=%d' \
    "$count" "$pid" "$count" "$start" "$count" "$role" "$((count + 1))") || return 1
  python3 "$OWNED_HELPER" registry-append "$PIDS" "$text" || return 1
  printf -v "OWNED_PID_$count" '%s' "$pid"
  printf -v "OWNED_START_$count" '%s' "$start"
  printf -v "OWNED_ROLE_$count" '%s' "$role"
  OWNED_COUNT=$((count + 1))
}

set_launch_pending() {
  local pending="$1" monitor_claim="${2:-${PENDING_MONITOR_CLAIM:-}}"
  local target_claim="${3:-${PENDING_TARGET_CLAIM:-}}" text
  if [ "$pending" = 1 ]; then
    text=$(printf 'PENDING_MONITOR_CLAIM=%q\nPENDING_TARGET_CLAIM=%q\nPENDING_WRAPPER_PID=0\nPENDING_WRAPPER_START=0\nLAUNCH_PENDING=1' \
      "$monitor_claim" "$target_claim") || return 1
  else
    text='LAUNCH_PENDING=0'
  fi
  python3 "$OWNED_HELPER" registry-append "$PIDS" "$text" || return 1
  LAUNCH_PENDING="$pending"
  PENDING_MONITOR_CLAIM="$monitor_claim"
  PENDING_TARGET_CLAIM="$target_claim"
}

record_pending_wrapper() {
  local pid="$1" start="$2" text
  [[ "$pid" =~ ^[0-9]+$ && "$start" =~ ^[0-9]+$ ]] || return 1
  text=$(printf 'PENDING_WRAPPER_PID=%q\nPENDING_WRAPPER_START=%q' "$pid" "$start") || return 1
  python3 "$OWNED_HELPER" registry-append "$PIDS" "$text" || return 1
  PENDING_WRAPPER_PID="$pid"
  PENDING_WRAPPER_START="$start"
}

terminate_launch_wrapper() {
  local pid="$1" start="$2" attempt
  if signal_recorded "$pid" "$start" SIGKILL; then wait "$pid" 2>/dev/null || true; fi
  for attempt in 1 2 3 4 5; do
    if ! recorded_alive "$pid" "$start"; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep 1
  done
  return 1
}

abandon_unrecorded_wrapper() {
  local pid="$1" start="$2" monitor_claim="$3" target_claim="$4"
  if ! terminate_launch_wrapper "$pid" "$start"; then return 1; fi
  if [ ! -s "$monitor_claim" ] && [ ! -s "$target_claim" ]; then
    set_launch_pending 0 || return 1
  fi
  return 0
}

handle_signal() {
  if [ "$DEFER_SIGNALS" = 1 ]; then
    [ -n "$DEFERRED_SIGNAL" ] || DEFERRED_SIGNAL="$1"
    return 0
  fi
  [ "$STOPPING" = 1 ] && return 0
  stop_all || true
  exit 130
}

honor_deferred_signal() {
  DEFER_SIGNALS=0
  if [ -n "$DEFERRED_SIGNAL" ]; then
    echo "honoring deferred $DEFERRED_SIGNAL after owned process identities were recorded" >&2
    stop_all || true
    exit 130
  fi
}

launch_owned() {
  local role="$1" mode="$2" logfile="$3" cwd="$4" claim_base="$5"
  shift 5
  CLAIM_SEQUENCE=$((CLAIM_SEQUENCE + 1))
  claim_base="$claim_base.${BASHPID}.${CLAIM_SEQUENCE}"
  local monitor_claim="$claim_base.monitor" target_claim="$claim_base.target"
  local launch_pid launch_start rc tries target_pid target_start parent_pid parent_start stopped=0
  local -a command=() supervisor_command=()
  DEFER_SIGNALS=1
  set_launch_pending 1 "$monitor_claim" "$target_claim" || { echo "could not durably mark launch pending: $role" >&2; return 1; }
  if [ "$mode" = root ]; then
    command=(sudo -n python3 "$OWNED_HELPER" exec --track-parent --stop-after-claim --monitor-claim "$monitor_claim" --claim "$target_claim" -- "$@")
  elif [ "$mode" = root-ns ]; then
    local netns="$1"
    shift
    command=(sudo -n ip netns exec "$netns" python3 "$OWNED_HELPER" exec \
      --track-parent --stop-after-claim --monitor-claim "$monitor_claim" --claim "$target_claim" -- "$@")
  else
    command=("$@")
  fi
  supervisor_command=(python3 "$OWNED_HELPER" supervise --stop-before-claim --claim "$monitor_claim")
  [ -n "$cwd" ] && supervisor_command+=(--cwd "$cwd")
  supervisor_command+=(-- "${command[@]}")
  "${supervisor_command[@]}" > "$logfile" 2>&1 &
  launch_pid=$!
  LAST_LAUNCH_PID="$launch_pid"
  launch_start=$(proc_starttime "$launch_pid" 2>/dev/null) || launch_start=""
  for ((tries=0; tries<1000; tries++)); do
    if proc_has_state "$launch_pid" "$launch_start" stopped; then
      if [ -n "$launch_start" ] && proc_has_state "$launch_pid" "$launch_start" stopped; then
        if ! record_pending_wrapper "$launch_pid" "$launch_start"; then
          echo "could not durably record pending wrapper identity: $role" >&2
          abandon_unrecorded_wrapper "$launch_pid" "$launch_start" "$monitor_claim" "$target_claim" \
            || echo "REFUSING cleanup: pending wrapper $launch_pid may remain; preserve $PIDS" >&2
          return 1
        fi
        if ! record_identity "$role" "$launch_pid" "$launch_start"; then
          echo "could not record wrapper role identity: $role" >&2
          terminate_launch_wrapper "$launch_pid" "$launch_start" \
            || echo "pending wrapper $launch_pid remains for stop-time exact recovery" >&2
          return 1
        fi
        local claimed_monitor claimed_start
        if ! read -r claimed_monitor claimed_start < "$monitor_claim" \
           || [ "$claimed_monitor" != "$launch_pid" ] || [ "$claimed_start" != "$launch_start" ]; then
          echo "launch claim does not match stopped wrapper identity: $role" >&2
          terminate_launch_wrapper "$launch_pid" "$launch_start" || return 1
          return 1
        fi
        if ! signal_recorded "$launch_pid" "$launch_start" SIGCONT; then
          echo "could not resume durably claimed launch wrapper $launch_pid" >&2
          return 1
        fi
        stopped=1
        break
      fi
    fi
    if ! proc_has_state "$launch_pid" "$launch_start" active; then
      wait "$launch_pid"; rc=$?
      echo "launch wrapper exited before ownership handshake (status $rc): $role" >&2
      if [ ! -s "$target_claim" ]; then set_launch_pending 0 || true; fi
      return 1
    fi
    sleep 0.01
  done
  if [ "$stopped" != 1 ]; then
    while proc_has_state "$launch_pid" "$launch_start" active; do
      if [ -n "$launch_start" ] && proc_has_state "$launch_pid" "$launch_start" active; then
        record_pending_wrapper "$launch_pid" "$launch_start" || {
          echo "could not durably record pending wrapper identity after handshake timeout: $role" >&2
          abandon_unrecorded_wrapper "$launch_pid" "$launch_start" "$monitor_claim" "$target_claim" \
            || echo "REFUSING cleanup: pending wrapper $launch_pid may remain; preserve $PIDS" >&2
          return 1
        }
        if ! record_identity "$role" "$launch_pid" "$launch_start"; then
          terminate_launch_wrapper "$launch_pid" "$launch_start" || return 1
          return 1
        fi
        terminate_launch_wrapper "$launch_pid" "$launch_start" || return 1
        if [ ! -s "$target_claim" ]; then set_launch_pending 0 || return 1; fi
        break
      fi
      sleep 0.05
    done
    if ! proc_has_state "$launch_pid" "$launch_start" active; then
      wait "$launch_pid" 2>/dev/null || true
      if [ ! -s "$target_claim" ]; then set_launch_pending 0 || true; fi
    fi
    echo "timeout waiting for stopped ownership handshake: $role" >&2
    return 1
  fi
  for ((tries=0; tries<1000; tries++)); do
    [ -s "$monitor_claim" ] && break
    if ! proc_has_state "$launch_pid" "$launch_start" active; then
      wait "$launch_pid"; rc=$?
      echo "launch wrapper exited before its durable claim (status $rc): $role" >&2
      if [ ! -s "$target_claim" ]; then set_launch_pending 0 || true; fi
      return 1
    fi
    sleep 0.01
  done
  if [ ! -s "$monitor_claim" ]; then
    echo "timeout waiting for launch identity claim: $role" >&2
    terminate_launch_wrapper "$launch_pid" "$launch_start" || return 1
    return 1
  fi
  read -r claimed_monitor claimed_start < "$monitor_claim" || return 1
  if [ "$claimed_monitor" != "$launch_pid" ] || [ "$claimed_start" != "$launch_start" ]; then
    echo "launch claim does not match stopped wrapper identity: $role" >&2
    terminate_launch_wrapper "$launch_pid" "$launch_start" || return 1
    return 1
  fi
  if [ "$mode" != local ]; then
    for ((tries=0; tries<1000; tries++)); do
      [ -s "$target_claim" ] && break
      if ! proc_has_state "$launch_pid" "$launch_start" active; then
        wait "$LAST_LAUNCH_PID"; rc=$?
        echo "privileged command exited before its target claim (status $rc): $role" >&2
        return 1
      fi
      sleep 0.01
    done
    if [ ! -s "$target_claim" ]; then
      echo "timeout waiting for privileged target claim: $role" >&2
      # LAUNCH_PENDING remains set so stop_all preserves recovery state.
      return 1
    fi
    read -r target_pid target_start parent_pid parent_start < "$target_claim" || return 1
    record_identity "$role" "$target_pid" "$target_start" || return 1
    if [[ "${parent_pid:-}" =~ ^[0-9]+$ && "${parent_start:-}" =~ ^[0-9]+$ ]]; then
      record_identity "$role" "$parent_pid" "$parent_start" || return 1
    elif [ "$target_pid" != "$launch_pid" ]; then
      echo "privileged target has no valid parent identity: $role" >&2
      return 1
    fi
    # The target writes its claim before SIGSTOP. sudo then propagates that
    # stop to the outer wrapper. Observe both stops before continuing either:
    # otherwise an early SIGCONT can be lost or sudo can stop after our resume.
    local privileged_stopped=0
    for ((tries=0; tries<1000; tries++)); do
      if ! proc_has_state "$target_pid" "$target_start" active \
         || ! proc_has_state "$launch_pid" "$launch_start" active; then
        echo "privileged launch identity exited before stop propagation: $role" >&2
        return 1
      fi
      if [ -n "${parent_pid:-}" ] && ! proc_has_state "$parent_pid" "$parent_start" active; then
        echo "privileged parent identity exited before stop propagation: $role" >&2
        return 1
      fi
      if proc_has_state "$target_pid" "$target_start" stopped \
         && proc_has_state "$launch_pid" "$launch_start" stopped; then
        privileged_stopped=1
        break
      fi
      sleep 0.01
    done
    if [ "$privileged_stopped" != 1 ]; then
      echo "timeout waiting for privileged stop propagation: $role" >&2
      return 1
    fi
    # Resume from child to outer wrapper, so each supervisor can reap its child.
    # All identities are durable; any failure leaves LAUNCH_PENDING for recovery.
    signal_recorded "$target_pid" "$target_start" SIGCONT || return 1
    if [ -n "${parent_pid:-}" ] && [ "$parent_pid" != "$target_pid" ]; then
      signal_recorded "$parent_pid" "$parent_start" SIGCONT || return 1
    fi
    if [ "$launch_pid" != "$target_pid" ] && [ "$launch_pid" != "${parent_pid:-}" ]; then
      signal_recorded "$launch_pid" "$launch_start" SIGCONT || return 1
    fi
    printf -v "${role}_PID" '%s' "$target_pid"
    printf -v "${role}_PID_START" '%s' "$target_start"
  else
    printf -v "${role}_PID" '%s' "$launch_pid"
    printf -v "${role}_PID_START" '%s' "$launch_start"
  fi
  set_launch_pending 0 || return 1
}
if [ "${1:-}" = stop ]; then stop_all; exit $?; fi

DUR="${1:-300}"
RUN="${2:-$BASE/$(date +%Y%m%d_%H%M%S)}"
PX="${PX:-1}"; PX_FIRST="${PX_FIRST:-0}"; NO_UE="${NO_UE:-0}"
DL_RATE="${DL_RATE:-2M}"; UL_PING="${UL_PING:-0.2}"
CLAIM_SEQUENCE=0
mkdir -p "$BASE"

# ---- guards: never share the box with another radio campaign -------------------------------
if [ -e "$PIDS" ]; then
  echo "REFUSING: owned harness state already exists at $PIDS; inspect it and stop that invocation first"
  exit 2
fi
if pgrep -x nr-softmodem >/dev/null || pgrep -x gnb >/dev/null \
   || pgrep -x srsue >/dev/null || pgrep -x nr-uesoftmodem >/dev/null; then
  echo "REFUSING: another radio process/campaign is running:"
  for n in nr-softmodem gnb srsue nr-uesoftmodem; do pgrep -a -x "$n" || true; done
  exit 2
fi
if ss -ltn | grep -Eq ':(2000|2001|2100|2101|2200|2201)\b'; then echo "REFUSING: ZMQ ports busy"; ss -ltnp | grep -E ':(2000|2001|2100|2101|2200|2201)\b'; exit 2; fi
if [ "$NO_UE" != 1 ] && sudo -n ip netns list | grep -qw "$NS"; then
  echo "REFUSING: network namespace $NS already exists; this invocation will not adopt or clean it" >&2
  exit 2
fi
[ -f "$BUILD/liboai_zmqdevif.so" ] || { echo "missing $BUILD/liboai_zmqdevif.so: cmake -DOAI_ZMQ=ON . && ninja oai_zmqdevif"; exit 2; }

umask 077
if ! (set -o noclobber; : > "$PIDS") 2>/dev/null; then
  echo "REFUSING: another invocation claimed $PIDS during startup" >&2
  exit 2
fi
if ! python3 "$OWNED_HELPER" registry-append "$PIDS" $'OWNED_COUNT=0\nLAUNCH_PENDING=0'; then
  echo "could not durably initialize owned-process registry at $PIDS" >&2
  rm -f "$PIDS"
  exit 1
fi
OWNED_COUNT=0; LAUNCH_PENDING=0
trap 'handle_signal INT' INT
trap 'handle_signal TERM' TERM
trap 'handle_signal HUP' HUP
mkdir -p "$RUN"; chmod 777 "$BASE" "$RUN" 2>/dev/null
echo "run dir: $RUN"
sed "s#/tmp/ocudu_passive/gnb.log#$RUN/gnb.log#" "$OC/gnb.ocudu.zmq.yaml" > "$RUN/gnb.yaml"
mkdir -p "$RUN/claims" && chmod 700 "$RUN/claims" || { stop_all || true; exit 1; }

wait_for() { # file pattern timeout label [pid]
  local i=0
  while [ "$i" -lt "$3" ]; do
    grep -q "$2" "$1" 2>/dev/null && { echo "  [ok] $4 (${i}s)"; return 0; }
    [ -n "${5:-}" ] && [ ! -d "/proc/$5" ] && { echo "  [DIED] $4 -- see $1" >&2; return 1; }
    sleep 1; i=$((i+1))
  done
  echo "  [TIMEOUT] $4 (${3}s) -- see $1" >&2; return 1
}

start_px() {
  echo "--- passive OAI UE (--passive-rx, fully agnostic) ---"
  launch_owned PX local "$RUN/passive.log" "$RUN" "$RUN/claims/PX" env ${PX_ENV:-} "$PX_BIN" \
    -O "$OC/ue.passive.ocudu.conf" --passive-rx -E -r 51 --numerology 1 --band 78 -C 3414990000 --ssb 33 \
    --device.name oai_zmqdevif --zmq.[0].tx_channels tcp://127.0.0.1:2201 \
    --zmq.[0].rx_channels tcp://127.0.0.1:2200 ${PX_EXTRA:-} || { honor_deferred_signal; stop_all || true; exit 1; }
  honor_deferred_signal
}

# ---- 1. broker ---------------------------------------------------------------------------------
BARGS=(); [ "$NO_UE" = 1 ] && BARGS+=(--no-ue)
launch_owned BROKER local "$RUN/broker.log" "" "$RUN/claims/BROKER" \
  python3 -u "$OC/ocudu_zmq_broker.py" "${BARGS[@]}" ${BROKER_EXTRA:-} \
  || { honor_deferred_signal; stop_all || true; exit 1; }
honor_deferred_signal
wait_for "$RUN/broker.log" "broker\] up" 10 "broker" $BROKER_PID || { stop_all; exit 1; }

# ---- 2. gNB ------------------------------------------------------------------------------------
echo "--- OCUDU gNB ---"
launch_owned GNB root "$RUN/gnb_stdout.log" "" "$RUN/claims/GNB" \
  env ${GNB_ENV:-} "$GNB_BIN" -c "$RUN/gnb.yaml" pcap --f1ap_enable true \
  --f1ap_filename "$RUN/f1ap.pcap" ${GNB_EXTRA:-} || { honor_deferred_signal; stop_all || true; exit 1; }
honor_deferred_signal
wait_for "$RUN/gnb_stdout.log" "Connection to AMF on .* completed" 30 "gNB N2 up" $GNB_PID || { stop_all; exit 1; }

if [ "$PX" = 1 ] && [ "$PX_FIRST" = 1 ]; then
  start_px
  wait_for "$RUN/passive.log" "SIB1 decoded" 300 "passive SSB/MIB/SIB1 (before attach)" "$PX_PID" || { stop_all; exit 1; }
fi

# ---- 3. srsUE ----------------------------------------------------------------------------------
if [ "$NO_UE" != 1 ]; then
  echo "--- srsUE (attach) ---"
  launch_owned NSADD root "$RUN/netns_add.log" "" "$RUN/claims/NSADD" ip netns add "$NS" \
    || { honor_deferred_signal; stop_all || true; exit 1; }
  while :; do
    wait "$LAST_LAUNCH_PID"; NSADD_STATUS=$?
    [ -n "$DEFERRED_SIGNAL" ] && [ "$NSADD_STATUS" -gt 128 ] && continue
    break
  done
  if [ "$NSADD_STATUS" -ne 0 ]; then
    echo "network namespace creation failed with status $NSADD_STATUS" >&2
    python3 "$OWNED_HELPER" registry-append "$PIDS" 'NSADD_DONE=1' || { stop_all || true; exit 1; }
    NSADD_DONE=1
    honor_deferred_signal; stop_all || true; exit 1
  fi
  python3 "$OWNED_HELPER" registry-append "$PIDS" 'NS_CREATED=1' || { stop_all || true; exit 1; }
  NS_CREATED=1
  python3 "$OWNED_HELPER" registry-append "$PIDS" 'NSADD_DONE=1' || { stop_all || true; exit 1; }
  NSADD_DONE=1
  honor_deferred_signal
  launch_owned UE root "$RUN/srsue_stdout.log" "" "$RUN/claims/UE" \
    env ${SRSUE_ENV:-} "$SRSUE_BIN" "$OC/ue.srsue.ocudu.conf" \
    --log.filename="$RUN/srsue.log" ${SRSUE_EXTRA:-} || { honor_deferred_signal; stop_all || true; exit 1; }
  honor_deferred_signal
  wait_for "$RUN/srsue_stdout.log" "PDU Session Establishment successful" 600 "srsUE attached + PDU session" $UE_PID \
    || { stop_all; exit 1; }
  UE_IP=$(grep -oE "PDU Session Establishment successful. IP: [0-9.]+" "$RUN/srsue_stdout.log" | awk '{print $NF}' | tail -1)
  echo "  UE IP $UE_IP"
fi

if [ "$PX" = 1 ] && [ "$PX_FIRST" != 1 ]; then start_px; fi
[ "$PX" = 1 ] && [ "$PX_FIRST" != 1 ] && wait_for "$RUN/passive.log" "SIB1 decoded" 300 "passive SSB/MIB/SIB1" "$PX_PID"

# ---- 4. traffic --------------------------------------------------------------------------------
if [ "$NO_UE" != 1 ] && [ -n "${UE_IP:-}" ]; then
  if [ "$DL_RATE" != 0 ]; then
    launch_owned TRAFFIC root-ns "$RUN/udp_recv.log" "" "$RUN/claims/UDP_RECV" "$NS" \
      python3 -u "$HERE/udp_dl.py" recv --port 5201 --dur "$DUR" \
      || { honor_deferred_signal; stop_all || true; exit 1; }
    honor_deferred_signal
    launch_owned TRAFFIC local "$RUN/udp_send.log" "" "$RUN/claims/UDP_SEND" \
      python3 -u "$HERE/udp_dl.py" send --dst "$UE_IP" --port 5201 --rate "$DL_RATE" --dur "$DUR" \
      || { honor_deferred_signal; stop_all || true; exit 1; }
    honor_deferred_signal
  fi
  if [ "$UL_PING" != 0 ]; then
    launch_owned TRAFFIC root-ns "$RUN/ping_ul.log" "" "$RUN/claims/PING_UL" "$NS" \
      ping -i "$UL_PING" -s 600 -w "$DUR" 10.45.0.1 \
      || { honor_deferred_signal; stop_all || true; exit 1; }
    honor_deferred_signal
  fi
fi

# ---- 5. dwell, supervised ----------------------------------------------------------------------
echo "--- dwell ${DUR}s ---"
t=0
while [ "$t" -lt "$DUR" ]; do
  sleep 10; t=$((t+10))
  for v in BROKER_PID GNB_PID UE_PID PX_PID; do
    p="${!v:-}"; [ -n "$p" ] && [ ! -d "/proc/$p" ] && { echo "  [DIED] $v at ${t}s"; DIED=1; }
  done
  [ -n "${DIED:-}" ] && break
  [ $((t % 60)) -eq 0 ] && tail -1 "$RUN/broker.log"
done

stop_all
stop_status=$?
if [ "$stop_status" -ne 0 ]; then
  echo "cleanup failed with status $stop_status; summary withheld and recovery state preserved" >&2
  exit "$stop_status"
fi
trap - HUP INT TERM

# ---- 6. summary (gNB log = ground truth, validation only) ------------------------------------
{
  echo "== ground truth (OCUDU) =="
  grep -oE "tc-rnti=0x[0-9a-f]+" "$RUN/gnb.log" | sort | uniq -c
  grep -oE "PDCCH: rnti=0x[0-9a-f]+ ss_id=[0-9]+ format=[0-9_]+ .*al=[0-9]+" "$RUN/gnb.log" \
    | sed -E 's/ cce=[0-9]+//' | sort | uniq -c | sort -rn | head -12
  echo "== passive =="
  grep -m3 -E "SIB1 decoded|Technique D ARMED" "$RUN/passive.log"
  grep -E "Technique D CONVERGED|DCI 1_1 length locked|BWP CORESET found|UL automatic DCI length locked" "$RUN/passive.log" | head -8
  grep "blind PDCCH monitor summary" "$RUN/passive.log" | tail -1 \
    | grep -oE "occasions=[0-9]+|dci10\[[^]]*\]|dci01\[[^]]*\]|pdsch_decode\[[^]]*\]|scanq\[[^]]*\]"
  grep -E "PDSCHQ (queued|per-rnti)" "$RUN/passive.log" | tail -2
  grep -iE "PUSCH.*(crc|decod)" "$RUN/passive.log" | tail -3
  grep -oE "rnti_seen .*rnti=0x[0-9a-f]+" "$RUN/passive.log" | grep -oE "rnti=0x[0-9a-f]+" | sort | uniq -c | sort -rn | head -5
} | tee "$RUN/summary.txt"
