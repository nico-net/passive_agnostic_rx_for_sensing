#!/usr/bin/env bash
set -euo pipefail
# Offline T26d replay with host-safe core assignments. Run one variant/capture
# at a time; heavy.sh takes the global host lock before the receiver starts.
root=$(readlink -f "$(dirname "$0")/../../..")
orchestrator=/home/nicola/NICOLA/wt/rr-orchestration
build=$root/cmake_targets/ran_build/build
variant=${1:?variant: baseline cache prescreen length neon combined cache_verify par combined_par combined_par_cap audit}
capture=${2:?capture: b old}
case "$capture" in
  b) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b; tick=208029025 ;;
  old) cap=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008; tick=210036277 ;;
  *) echo "unknown capture $capture" >&2; exit 2 ;;
esac
extra=()
basecfg="$root/tests/passive_rx/dgx/ue.passive.ota.scanfast.dgx.cfg"
case "$variant" in
  baseline) ;;
  cache) extra+=(ISAC_PDCCH_LLR_CACHE=1) ;;
  prescreen) extra+=(ISAC_PDCCH_SHARED_PRESCREEN=1) ;;
  length) extra+=(ISAC_PDCCH_LEN_MEMORY=1) ;;
  neon) extra+=(ISAC_PDCCH_NEON_SCORE=1) ;;
  combined) extra+=(ISAC_PDCCH_LLR_CACHE=1 ISAC_PDCCH_SHARED_PRESCREEN=1
                    ISAC_PDCCH_LEN_MEMORY=1 ISAC_PDCCH_NEON_SCORE=1) ;;
  cache_verify) extra+=(ISAC_PDCCH_LLR_CACHE=1 ISAC_PDCCH_LLR_CACHE_VERIFY=1) ;;
  par) basecfg="$root/tests/passive_rx/dgx/ue.passive.ota.scanfast_par.dgx.cfg"
       extra+=(ISAC_PDCCH_PAR_BANKS=1 ISAC_PDCCH_PAR_CORE=9) ;;
  combined_par) basecfg="$root/tests/passive_rx/dgx/ue.passive.ota.scanfast_par.dgx.cfg"
       extra+=(ISAC_PDCCH_PAR_BANKS=1 ISAC_PDCCH_PAR_CORE=9
               ISAC_PDCCH_LLR_CACHE=1 ISAC_PDCCH_SHARED_PRESCREEN=1
               ISAC_PDCCH_LEN_MEMORY=1 ISAC_PDCCH_NEON_SCORE=1) ;;
  combined_par_cap) basecfg="$root/tests/passive_rx/dgx/ue.passive.ota.scanfast_par.dgx.cfg"
       extra+=(ISAC_PDCCH_PAR_BANKS=1 ISAC_PDCCH_PAR_CORE=9
               ISAC_PDCCH_LLR_CACHE=1 ISAC_PDCCH_SHARED_PRESCREEN=1
               ISAC_PDCCH_LEN_MEMORY=1 ISAC_PDCCH_NEON_SCORE=1
               ISAC_PDCCH_TASK_CAP64=1) ;;
  audit) extra+=(ISAC_PDCCH_SHARED_PRESCREEN=1 ISAC_PDCCH_SHARED_PRESCREEN_MIN=0.66
                  ISAC_PDCCH_SHARED_PRESCREEN_AUDIT=1
                  ISAC_PDCCH_TASK_CAP64=1) ;;
  *) echo "unknown variant $variant" >&2; exit 2 ;;
esac
env CAP="$cap" CAP_SAMPLES=1474560000 CAP_TICK="$tick" \
  BASECFG="$basecfg" \
  REPLAY_PREFIX="" REPLAY_OUT="$root/replays" REPLAY_LAST="$root/replays/last" ISAC_MLOCK=0 \
  "$orchestrator/heavy.sh" "$orchestrator/csd_replay_user.sh" "scanfast_${variant}_${capture}" "$build" 0.25 \
  ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_JOINT=1 ISAC_CORE_PLAN=0 \
  ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=0 \
  ISAC_PDCCH_DISCOVERED_FILE="$root/tests/passive_rx/dgx/scanfast_coresets.txt" \
  ISAC_COREMAP_IDSWEEP_DUR=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_CSIRS_DISC=1 \
  ISAC_GPU_SELFCHECK=0 ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 \
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
  ISAC_CHEST_DELAY_SHARE=1 ISAC_PDSCH_SOLVER_BACKOFF=200 \
  ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1 \
  ISAC_PDCCH_JOINT_ANON_RATE=0.1 ISAC_PDCCH_DED_BUDGET_SHARE=2 \
  ISAC_PDSCH_CORES=10,11,12,13,14 ISAC_PDCCH_TIMING=1 ISAC_DCI_RAW_LOG=1 \
  "${extra[@]}"
run=$(cat "$root/replays/last")
if ! rg -q 'SCANTIME' "$run/run.log"; then
  echo "replay produced no SCANTIME: $run/run.log" >&2
  exit 1
fi
