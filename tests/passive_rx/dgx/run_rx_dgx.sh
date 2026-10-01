#!/bin/bash
# DGX receiver launcher with the X925 core map (PROJECT_MEMORY 14.3). sens6 uses captures/run_arm.sh (frozen).
# usage: INSTANCE=A|B [BUILD=..] [ONLINE_CPUS_OVERRIDE=0-19] run_rx_dgx.sh [--dry-run] -- <nr-uesoftmodem args...>
# Refuses to pin to a CPU that is not in /sys/devices/system/cpu/online (or ONLINE_CPUS_OVERRIDE, for tests).
# Conf-embedded cores (scan_thread, pdsch, ul_thread) come from the caller's -O conf; checked here (online, RT/USS collision,
# instance B must not pin cluster 0). The dgx .cfg is an instance-A conf; instance B needs its own conf.
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
for c in $RT $SCAN $USS $ACT $(echo "$TP" | tr ',' ' ') $((A_PDSCH_FIRST+off)) $((A_PDSCH_FIRST+off+1)) $PDSCH_LAST; do
  echo "$ONL" | grep -qx "$c" || { echo "core $c is not online (online=$online); refusing to apply the instance $INST map" >&2; exit 3; }
done
# Conf-embedded pins (final review I3). Specs: scan_thread "n:depth:core", ul_thread "n:depth:core",
# pdsch "dec:mcs:xoh:rv0:max:n:depth:core"; consumer i of an n-consumer pool is pinned to core+i; core -1/absent = unpinned.
prev=""; CONF=""
for a in "$@"; do [ "$prev" = "-O" ] && CONF=$a; prev=$a; done
confval() { grep -E "^[[:space:]]*(sensing\.)?$2[[:space:]]*=" "$1" | grep -v '^[[:space:]]*//' | tail -1 | sed -E 's/^[^"]*"([^"]*)".*/\1/'; }
chk_pin() { # name first n   (first<0 or non-numeric: unpinned)
  local name=$1 first=$2 n=$3 i c
  [[ "$first" =~ ^[0-9]+$ && "$n" =~ ^[0-9]+$ ]] || return 0
  [ "$n" -ge 1 ] || n=1
  for ((i=0;i<n;i++)); do
    c=$((first+i))
    echo "$ONL" | grep -qx "$c" || { echo "core $c ($name in $CONF) is not online (online=$online); refusing" >&2; exit 3; }
    if [ "$INST" = B ] && [ "$c" -lt 10 ]; then
      echo "core $c ($name in $CONF) is a cluster-0 core (instance A); instance B needs its own conf with cores 10-19; refusing" >&2; exit 3
    fi
  done
}
if [ -n "$CONF" ] && [ -r "$CONF" ]; then
  v=$(confval "$CONF" pdcch_blind_monitor_scan_thread)
  sc=${v##*:}
  if [[ "$v" == *:*:* && "$sc" =~ ^[0-9]+$ ]]; then
    if [ "$sc" -eq "$RT" ] || [ "$sc" -eq "$USS" ]; then
      echo "scan_thread core $sc in $CONF collides with the RT core ($RT) or USS core ($USS); refusing" >&2; exit 3
    fi
    # scan_thread warning: A725 cores = 0-4 (A) / 10-14 (B).
    if [ "$sc" -ge "$off" ] && [ "$sc" -le $((off+4)) ]; then
      echo "WARNING: scan_thread core $sc in $CONF is an A725 core (A725 = $off-$((off+4))); use $SCAN (X925)" >&2
    fi
    IFS=: read -r sn _ sc0 <<< "$v"; chk_pin scan_thread "$sc0" "$sn"
  fi
  v=$(confval "$CONF" pdcch_blind_monitor_pdsch)
  IFS=: read -r _ _ _ _ _ pn _ pc <<< "$v"; chk_pin pdsch "${pc:-}" "${pn:-1}"
  v=$(confval "$CONF" pdcch_blind_monitor_ul_thread)
  IFS=: read -r un _ uc <<< "$v"; chk_pin ul_thread "${uc:-}" "${un:-1}"
fi
ENVS="ISAC_UE_RT_CORE=$RT ISAC_PDCCH_USS_CORE=$USS"
ARGS="--thread-pool $TP --sync-actor-core $ACT --dl-actor-core-start $ACT --ul-actor-core-start $ACT"
echo "COREMAP applied: instance=$INST $ENVS $ARGS (scan=$SCAN pdsch=$((A_PDSCH_FIRST+off))-$PDSCH_LAST online=$online)"
[ "$DRY" = 1 ] && { echo "env $ENVS $B/nr-uesoftmodem $* $ARGS"; exit 0; }
# shellcheck disable=SC2086
exec env $ENVS "$B/nr-uesoftmodem" "$@" $ARGS
