#!/usr/bin/env bash
set -euo pipefail
# Recorded CPU reference and paired GPU comparison, never radio. heavy.sh owns
# the machine-wide lock. On the sandbox, run cpu_dump only (no GPU device).
root=$(readlink -f "$(dirname "$0")/../../..")
orch=/home/nicola/NICOLA/wt/rr-orchestration
build=$root/cmake_targets/ran_build/build
mode=${1:?mode: cpu_dump or gpu_compare}
capture=${2:-b}
case "$capture" in
  b) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b; tick=208029025 ;;
  old) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008; tick=210036277 ;;
  *) echo "unknown capture $capture" >&2; exit 2 ;;
esac
extra=()
case "$mode" in
  cpu_dump)
    extra+=(ISAC_PDSCH_GPU_DUMP="$root/replays/gpu_cpu_${capture}.bin" ISAC_PDSCH_GPU_DUMP_MAX=128
            ISAC_PDCCH_GPU_DUMP="$root/replays/pdcch_cpu_${capture}.bin" ISAC_PDCCH_GPU_DUMP_MAX=128
            ISAC_GPU_SELFCHECK=0) ;;
  gpu_compare)
    extra+=(ISAC_PDCCH_GPU_FEP=1 ISAC_PDSCH_GPU=1 ISAC_GPU_SELFCHECK=128
            ISAC_GPU_SELFCHECK_ENFORCE=1 ISAC_TD_CB0_BACKEND=gpu
            ISAC_GPU_DUMP="$root/replays/gpu_compare_${capture}.bin") ;;
  *) echo "unknown mode $mode" >&2; exit 2 ;;
esac
mkdir -p "$root/replays"
if [[ $mode == cpu_dump ]]; then
  rm -f "$root/replays/gpu_cpu_${capture}.bin" "$root/replays/pdcch_cpu_${capture}.bin"
else
  rm -f "$root/replays/gpu_compare_${capture}.bin"
fi
env CAP="$cap" CAP_SAMPLES=1474560000 CAP_TICK="$tick" \
  BASECFG="$root/tests/passive_rx/dgx/ue.passive.ota.scanfast.dgx.cfg" \
  REPLAY_PREFIX="" REPLAY_OUT="$root/replays" REPLAY_LAST="$root/replays/last" ISAC_MLOCK=0 \
  "$orch/heavy.sh" "$orch/csd_replay_user.sh" "gpu_${mode}_${capture}" "$build" 0.25 \
  ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_JOINT=1 ISAC_CORE_PLAN=0 \
  ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=0 \
  ISAC_PDCCH_DISCOVERED_FILE="$root/tests/passive_rx/dgx/scanfast_coresets.txt" \
  ISAC_COREMAP_IDSWEEP_DUR=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_CSIRS_DISC=1 \
  ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 \
  ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1 \
  ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_SCAN_TIMING=1 \
  ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 \
  ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 \
  ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 \
  ISAC_PDSCH_SOLVER_REUSE=1 ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1 \
  ISAC_PDSCH_SOLVER_PROBE_RATE=0.3 ISAC_PDSCH_SOLVER_PROBE_BURST=16 \
  ISAC_PDSCH_SOLVER_PROBE_FILL=25 ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_EVM_GRANT=1 \
  ISAC_PDSCH_SOLVER_SCR=1 ISAC_PDCCH_JANON_ALL_CORESETS=1 \
  ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1 \
  ISAC_CHEST_DELAY_SHARE=0 ISAC_CHEST_PDP_DIAG=0 \
  ISAC_RX_MRC_PER_RNTI=1 ISAC_RX_MRC_ALIGN=2 \
  ISAC_PDSCH_SOLVER_BACKOFF=200 ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1 \
  ISAC_PDCCH_JOINT_ANON_RATE=0.1 ISAC_PDCCH_DED_BUDGET_SHARE=2 \
  ISAC_PDSCH_CORES=10,11,12,13,14 ISAC_PDCCH_TIMING=1 ISAC_DCI_RAW_LOG=1 \
  "${extra[@]}"
run=$(cat "$root/replays/last")
if ! rg -q 'SCANTIME' "$run/run.log"; then echo "no SCANTIME: $run/run.log" >&2; exit 1; fi
echo "$run"
