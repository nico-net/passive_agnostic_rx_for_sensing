#!/bin/bash
# DGX receiver launcher with the X925 core map (PROJECT_MEMORY 14.3). sens6 uses captures/run_arm.sh (frozen).
# usage: INSTANCE=A|B [BUILD=..] [ONLINE_CPUS_OVERRIDE=0-19] run_rx_dgx.sh [--dry-run] -- <nr-uesoftmodem args...>
# Refuses to pin to a CPU that is not in /sys/devices/system/cpu/online (or ONLINE_CPUS_OVERRIDE, for tests).
# Conf-embedded cores (scan_thread, pdsch, ul_thread) come from the caller's -O conf; only checked here.
set -u
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../../.." && pwd); B=${BUILD:-$R/cmake_targets/ran_build/build}
DRY=0; [ "${1:-}" = "--dry-run" ] && { DRY=1; shift; }; [ "${1:-}" = "--" ] && shift
. "$H/coremap_dgx.env"
INST=${INSTANCE:-A}
case "$INST" in A) off=0 ;; B) off=10 ;; *) echo "INSTANCE must be A or B (got '$INST')" >&2; exit 2 ;; esac
RT=$((A_RT_CORE+off)); USS=$((A_USS_CORE+off)); ACT=$((A_ACTORS+off)); SCAN=$((A_SCAN_CORE+off))
TP=$(echo "$A_TPOOL" | tr ',' '\n' | while read -r c; do echo $((c+off)); done | paste -sd,)
online=${ONLINE_CPUS_OVERRIDE:-$(cat /sys/devices/system/cpu/online)}
expand() { echo "$1" | tr ',' '\n' | while IFS=- read -r a b; do seq "$a" "${b:-$a}"; done; }
ONL=$(expand "$online")
PDSCH_LAST=$((A_PDSCH_FIRST+off+2))
for c in $RT $SCAN $USS $ACT $(echo "$TP" | tr ',' ' ') $((A_PDSCH_FIRST+off)) $PDSCH_LAST; do
  echo "$ONL" | grep -qx "$c" || { echo "core $c is not online (online=$online); refusing to apply the instance $INST map" >&2; exit 3; }
done
# scan_thread warning: value "n:depth:core"; core -1 = unpinned (no warning); A725 cores = 0-4 (A) / 10-14 (B).
prev=""; CONF=""
for a in "$@"; do [ "$prev" = "-O" ] && CONF=$a; prev=$a; done
if [ -n "$CONF" ] && [ -r "$CONF" ]; then
  v=$(grep -E '^[[:space:]]*(sensing\.)?pdcch_blind_monitor_scan_thread[[:space:]]*=' "$CONF" | grep -v '^[[:space:]]*//' | tail -1 | sed -E 's/^[^"]*"([^"]*)".*/\1/')
  sc=${v##*:}
  if [[ "$v" == *:*:* && "$sc" =~ ^[0-9]+$ ]] && [ "$sc" -ge "$off" ] && [ "$sc" -le $((off+4)) ]; then
    echo "WARNING: scan_thread core $sc in $CONF is an A725 core (A725 = $off-$((off+4))); use $SCAN (X925)" >&2
  fi
fi
ENVS="ISAC_UE_RT_CORE=$RT ISAC_PDCCH_USS_CORE=$USS"
ARGS="--thread-pool $TP --sync-actor-core $ACT --dl-actor-core-start $ACT --ul-actor-core-start $ACT"
echo "COREMAP applied: instance=$INST $ENVS $ARGS (scan=$SCAN pdsch=$((A_PDSCH_FIRST+off))-$PDSCH_LAST online=$online)"
[ "$DRY" = 1 ] && { echo "env $ENVS $B/nr-uesoftmodem $* $ARGS"; exit 0; }
# shellcheck disable=SC2086
exec env $ENVS "$B/nr-uesoftmodem" "$@" $ARGS
