#!/usr/bin/env bash
# GPU5 references from the integrated t26g CPU decoder. Offline IQ only.
set -euo pipefail
root=$(readlink -f "$(dirname "$0")/../../..")
orch=/home/nicola/NICOLA/wt/rr-orchestration
capture=${1:?b or old}; shift
case "$capture" in
  b) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b; tick=208029025 ;;
  old) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008; tick=210036277 ;;
  *) exit 2 ;;
esac
read -r -a envs < "$orch/ota/t26g.env"
base=()
for kv in "${envs[@]}"; do
  case "$kv" in
    ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*) ;;
    *) base+=("$kv") ;;
  esac
done
base+=(ISAC_MLOCK=0 NR_GPU_FEP=0 ISAC_PDSCH_GPU=0 ISAC_PDCCH_GPU_FEP=0
       ISAC_PDSCH_CORES=10,11,12,13,14 ISAC_CSIRS_DISC=1 ISAC_DCI_RAW_LOG=1
       ISAC_PDCCH_DISCOVERED_FILE="$root/tests/passive_rx/dgx/scanfast_coresets.txt"
       ISAC_PDSCH_GPU_DUMP="$root/replays/gpu5_cpu_${capture}.bin" ISAC_PDSCH_GPU_DUMP_MAX=512
       ISAC_PDSCH_GPU_STAGE_DUMP=1 ISAC_PDSCH_GPU_DUMP_FULL=1
       ISAC_PDCCH_GPU_DUMP="$root/replays/gpu5_pdcch_${capture}.bin" ISAC_PDCCH_GPU_DUMP_MAX=512)
mkdir -p "$root/replays"
# Never append a fresh run to an earlier reference.
for path in "$root/replays/gpu5_cpu_${capture}.bin" "$root/replays/gpu5_pdcch_${capture}.bin"; do
  if [[ -e $path ]]; then mv "$path" "$path.$(date +%s).previous"; fi
done
printf '%s\n' "${base[@]}" "$@" > "$root/replays/gpu5_${capture}.env"
exec env CAP="$cap" CAP_TICK="$tick" CAP_SAMPLES=1474560000 \
  REPLAY_PREFIX='' REPLAY_OUT="$root/replays" REPLAY_LAST="$root/replays/last" \
  BASECFG="$root/tests/passive_rx/dgx/ue.passive.ota.scanfast.dgx.cfg" \
  "$orch/heavy.sh" "$orch/csd_replay_user.sh" "gpu5_cpu_$capture" \
  "$root/cmake_targets/ran_build/build" 0.25 "${base[@]}" "$@"
