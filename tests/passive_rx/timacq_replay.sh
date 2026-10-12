#!/bin/bash
# TIM-ACQ replay: live ota/t26m.env (the env of the 2026-10-10 18:47 PCI 923 OTA runs) minus radio-only/core-pinning
# flags, PDSCH cores 8,9,12,13,14, any capture centre / receiver tuning. Replay only (no RF). Takes heavy.lock itself.
# usage: [CAP=... CAP_SAMPLES=... CAP_TICK=... CAPC=<capture centre Hz> RXC=<-C Hz> SSB=<--ssb> FO=<--initial-fo>
#         SPEED=0.25 B=<build dir> ENVFILE=<env file> PREFIX=<e.g. "gdb -q -batch -x cmds --args">] timacq_replay.sh <name> [ENV=...]
# Defaults = PCI 923 capture raw_pci923_tim_4s_1010_1853 with the live T1 arguments (-C 3731910000 --ssb 1169 -4800).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
O=/home/nicola/NICOLA/wt/rr-orchestration
B=${B:-$ROOT/cmake_targets/ran_build/build}
CAP=${CAP:-/home/nicola/NICOLA/ota_runs/raw_pci923_tim_4s_1010_1853}
CAP_SAMPLES=${CAP_SAMPLES:-491520000}; CAP_TICK=${CAP_TICK:-195322029}
CAPC=${CAPC:-3731910000}; RXC=${RXC:-3731910000}; SSB=${SSB:-1169}; FO=${FO:--4800}; SPEED=${SPEED:-0.25}
read -r -a envs < "${ENVFILE:-$O/ota/t26m.env}"
base=()
for kv in "${envs[@]}"; do
  case "$kv" in
    ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*) ;;
    *) base+=("$kv") ;;
  esac
done
name=$1; shift
R=$ROOT/replays/replay_${name}_$(date +%H%M%S); mkdir -p "$R"; echo "$R" > "$ROOT/replays/last"; echo "replay dir $R"
{ cat "$ROOT/tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg"; echo 'device = { name = "raw_iq"; };'
  echo "loader = { raw_iq = { shlibpath = \"$B\"; }; };"; } > "$R/replay.cfg"
cd "$R"
echo "CAP=$CAP CAPC=$CAPC RXC=$RXC SSB=$SSB FO=$FO SPEED=$SPEED extra: $*" > args.txt
exec "$O/heavy.sh" env ISAC_MLOCK=0 ISAC_RAW_IQ_DIRECTORY="$CAP" ISAC_RAW_IQ_SAMPLES="$CAP_SAMPLES" ISAC_RAW_IQ_FIRST_TICK="$CAP_TICK" \
  ISAC_RAW_IQ_CHANNELS=4 ISAC_RAW_IQ_RATE=122880000 ISAC_RAW_IQ_CENTER="$CAPC" ISAC_RAW_IQ_SPEED="$SPEED" ISAC_RAW_IQ_SHIFT=2 \
  LD_LIBRARY_PATH="$B:/usr/local/lib" ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 "${base[@]}" ISAC_PDSCH_CORES=8,9,12,13,14 ISAC_MLOCK=0 "$@" \
  timeout --signal=INT -k 60 ${TMO:-3600} ${PREFIX:-} "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r 217 --numerology 1 --band 78 \
  -C "$RXC" --ssb "$SSB" --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo "$FO" --ue-fo-compensation \
  --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/run.log" 2>&1
