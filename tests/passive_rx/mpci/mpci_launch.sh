#!/bin/bash
# MPCI launcher: one radio publisher + one nr-uesoftmodem consumer PROCESS per cell, all fed by the shm IQ ring.
# usage: [DRY=1] RING=isac_iq OUT=/path BASE_CFG=cell.cfg [BUILD=dir] [PUB_CMD="..."] [PUB_CORES=0-1] \
#          mpci_launch.sh [start|stop] TAG:C:R:SSB:FO:CORES ...
#   optional 7th field ENV=K=V,K=V : extra env for that cell only (no ':' ' ' ',' in values), e.g. A:..:4-7:ISAC_TARGET_PCI=4,ISAC_SSS_MULTI=2.5
#   APPEND=1 keeps OUT/pids (restart of one cell; give BASE_PORT=<base+index> to keep its endpoint port).
#   C = carrier Hz (-C), R = N_RB (-r), SSB = --ssb, FO = --initial-fo ("-" = omit), CORES = taskset list for that cell.
#   example (cores are an example only): A:3619200000:106:516:-:4-7  B:3649440000:51:200:-:8-11
# Per cell: ISAC_CELL_INSTANCE/ISAC_SHM_IQ_CELL=TAG, ISAC_SHM_IQ_NAME=RING, metrics/uectx/obs under OUT/TAG, own cwd,
# own cfg copy (device shm_iq, per-cell sensing paths; endpoint ports BASE_PORT+index).
# Stop = SIGINT to the PIDs this script started (OUT/pids). Never SIGKILL, never pkill by name (X410 claim hygiene).
set -u
CMD=start; case "${1:-}" in start|stop) CMD=$1; shift ;; esac
DRY=${DRY:-0}; RING=${RING:-isac_iq}; OUT=${OUT:?OUT required}; BUILD=${BUILD:-$(cd "$(dirname "$0")/../../.." && pwd)/cmake_targets/ran_build/build}
BASE_CFG=${BASE_CFG:-}; PUB_CMD=${PUB_CMD:-}; PUB_CORES=${PUB_CORES:-}; BASE_PORT=${BASE_PORT:-5556}
if [ "$CMD" = stop ]; then
  [ -r "$OUT/pids" ] || { echo "no $OUT/pids" >&2; exit 1; }
  while read -r tag pid; do kill -INT "$pid" 2>/dev/null && echo "SIGINT $tag $pid"; done < "$OUT/pids"
  exit 0
fi
[ $# -ge 1 ] || { echo "no cell specs" >&2; exit 2; }
[ -n "$BASE_CFG" ] && { [ -r "$BASE_CFG" ] || { echo "BASE_CFG unreadable: $BASE_CFG" >&2; exit 2; }; }
run() { if [ "$DRY" = 1 ]; then echo "$*"; else eval "$*"; fi; }
[ "$DRY" = 1 ] || { mkdir -p "$OUT"; [ "${APPEND:-0}" = 1 ] || : > "$OUT/pids"; }
if [ -n "$PUB_CMD" ]; then
  pin=""; [ -n "$PUB_CORES" ] && pin="taskset -c $PUB_CORES "
  if [ "$DRY" = 1 ]; then echo "PUB env ISAC_SHM_IQ_PUB=$RING ${pin}$PUB_CMD"
  else ISAC_SHM_IQ_PUB=$RING bash -c "exec ${pin}$PUB_CMD" > "$OUT/pub.log" 2>&1 & echo "pub $!" >> "$OUT/pids"; fi
fi
i=0; declare -A seen
for spec in "$@"; do
  IFS=: read -r tag c r ssb fo cores xenv extra <<< "$spec"
  [[ "$tag" =~ ^[A-Za-z0-9_-]{1,32}$ && -n "$c" && -n "$r" && -n "$ssb" && -n "$fo" && -n "$cores" && -z "${extra:-}" ]] \
    && [[ -z "${xenv:-}" || "$xenv" =~ ^[A-Za-z_][A-Za-z0-9_]*=[^[:space:],:]*(,[A-Za-z_][A-Za-z0-9_]*=[^[:space:],:]*)*$ ]] \
    || { echo "bad cell spec '$spec' (TAG:C:R:SSB:FO:CORES[:K=V,K=V])" >&2; exit 2; }
  [ -z "${seen[$tag]:-}" ] || { echo "duplicate tag $tag" >&2; exit 2; }; seen[$tag]=1
  d=$OUT/$tag; cfg=$d/cell.cfg; port=$((BASE_PORT+i))
  foarg=""; [ "$fo" != "-" ] && foarg="--initial-fo $fo"
  [ "$DRY" = 1 ] || mkdir -p "$d"
  if [ -n "$BASE_CFG" ]; then
    # per-cell sensing sinks (O_TRUNC + bind would collide), then device/loader for the shm_iq radio
    gen() { sed -E \
      -e "s|^([[:space:]]*(records_path\|cfr_path\|out_path\|report_path)[[:space:]]*=[[:space:]]*\")([^\"]*)\"|\1\3.$tag\"|" \
      -e "s|^([[:space:]]*(endpoint\|report_endpoint)[[:space:]]*=[[:space:]]*\"tcp://[^:\"]*:)[0-9]+\"|\1$port\"|" "$BASE_CFG"
      printf '\ndevice = { name = "shm_iq"; };\nloader = { shm_iq = { shlibpath = "%s"; }; };\n' "$BUILD"; }
    if [ "$DRY" = 1 ]; then echo "CFG $tag $cfg"; else gen > "$cfg"; fi
  fi
  envs="ISAC_CELL_INSTANCE=$tag ISAC_SHM_IQ_CELL=$tag ISAC_SHM_IQ_NAME=$RING ISAC_METRICS_PATH=$d/metrics.jsonl ISAC_UECTX_PATH=$d/uectx.json ISAC_OBS_PATH=$d/obs.bin${xenv:+ ${xenv//,/ }}"
  cmdline="taskset -c $cores $BUILD/nr-uesoftmodem${BASE_CFG:+ -O $cfg} -C $c -r $r --ssb $ssb${foarg:+ $foarg}"
  if [ "$DRY" = 1 ]; then echo "CELL $tag cwd=$d env $envs $cmdline"
  else (cd "$d" && exec env $envs $cmdline) > "$d/ue.log" 2>&1 & echo "$tag $!" >> "$OUT/pids"; fi
  i=$((i+1))
done
