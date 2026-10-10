#!/usr/bin/env bash
# GPU-DEMOD offline A/B on recorded raw IQ (never radio): ota/t26l.env minus radio-only knobs, host-safe cores.
#   gpudemod_replay.sh <b|old> <name> [ENV=...]
# The base already carries t26l's GPU6 front end (ISAC_PDSCH_GPU_EXACT_FEP=1, ISAC_CHEST_GPU_DELAY=1); add
# ISAC_PDSCH_GPU_EXACT_DEMOD=1 [ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=1] etc. as extra arguments.
set -euo pipefail
root=$(readlink -f "$(dirname "$0")/../../..")
orch=/home/nicola/NICOLA/wt/rr-orchestration
capture=${1:?b or old}; name=${2:?name}; shift 2
case "$capture" in
  b) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b; tick=208029025 ;;
  old) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008; tick=210036277 ;;
  *) exit 2 ;;
esac
read -r -a envs < "$orch/ota/t26l.env"
base=()
for kv in "${envs[@]}"; do
  case "$kv" in
    ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*) ;;
    *) base+=("$kv") ;;
  esac
done
base+=(ISAC_MLOCK=0 NR_GPU_FEP=0 ISAC_PDSCH_GPU=0 ISAC_PDCCH_GPU_FEP=0
       ISAC_PDSCH_CORES=10,11,12,13,14 ISAC_DCI_RAW_LOG=1 ISAC_PDSCH_SLOT_TIMING=1
       ISAC_PDCCH_DISCOVERED_FILE="$root/tests/passive_rx/dgx/scanfast_coresets.txt")
mkdir -p "$root/replays"
printf '%s\n' "${base[@]}" "$@" > "$root/replays/gpudemod_${capture}_${name}.env"
exec env CAP="$cap" CAP_TICK="$tick" CAP_SAMPLES=1474560000 \
  REPLAY_PREFIX='' REPLAY_OUT="$root/replays" REPLAY_LAST="$root/replays/last" \
  BASECFG="$root/tests/passive_rx/dgx/ue.passive.ota.scanfast.dgx.cfg" \
  "$orch/heavy.sh" "$orch/csd_replay_user.sh" "gd_${name}_$capture" \
  "$root/cmake_targets/ran_build/build" 0.25 "${base[@]}" "$@"
