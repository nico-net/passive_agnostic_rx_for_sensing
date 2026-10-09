#!/bin/bash
# QAM-FIX replay: live t26f.env minus radio-only/core-pinning flags, PDSCH cores 8,9,12,13,14. Replay only (no RF).
# usage: CAP=... CAP_TICK=... qamfix_replay.sh <name> [ENV=...]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
O=/home/nicola/NICOLA/wt/rr-orchestration
B=$ROOT/cmake_targets/ran_build/build
export REPLAY_PREFIX='' REPLAY_OUT=$ROOT/replays REPLAY_LAST=$ROOT/replays/last
export BASECFG=$ROOT/tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg
export CAP=${CAP:-/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b}
export CAP_SAMPLES=1474560000 CAP_TICK=${CAP_TICK:-208029025} ISAC_MLOCK=0
read -r -a envs < $O/ota/t26f.env
base=()
for kv in "${envs[@]}"; do
  case "$kv" in
    ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*) ;;
    *) base+=("$kv") ;;
  esac
done
name=$1; shift
exec "$O/heavy.sh" "$O/csd_replay_user.sh" "$name" "$B" 0.25 "${base[@]}" ISAC_PDSCH_CORES=8,9,12,13,14 "$@"
