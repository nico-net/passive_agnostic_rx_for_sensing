#!/bin/bash
# usage: gpupass_ab.sh <capset b|tim> <n> [seeded]   (each replay takes heavy.lock via p99_replay.sh)
# Interleaved arms OFF / ON / ONREC per run index; "seeded" runs only the validation-only ON arm with
# ISAC_UL_GPU_PASS_SEED=1 (the captures hold no real UL CRC-OK TB). Arms differ only in the pass/recovery flags;
# t26f.env already sets ISAC_CSIRS_DISC=1 (needed by the async recovery).
set -e
E="ISAC_E2E_TRACE=1"  # same diagnostic in every arm (per-grant e2e timing for sysan_e2e.py); passed as arg: sudo drops the env
S=$(cd "$(dirname "$0")" && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration/ota
if [ "${3:-}" = seeded ]; then
  CAPSET=$1 ENVFILE=$O/t26f.env "$S/p99_replay.sh" "gp_${1}_SEED_1" $E ISAC_GPU_PASS=1 ISAC_UL_GPU_PASS=1 ISAC_UL_GPU_PASS_SEED=1
  exit
fi
for i in $(seq 1 "$2"); do
  for arm in OFF ON ONREC; do
    case $arm in
      OFF)   X="$E";;
      ON)    X="$E ISAC_GPU_PASS=1 ISAC_UL_GPU_PASS=1";;
      ONREC) X="$E ISAC_GPU_PASS=1 ISAC_UL_GPU_PASS=1 ISAC_PDSCH_RECOVERY_ASYNC=1";;
    esac
    CAPSET=$1 ENVFILE=$O/t26f.env "$S/p99_replay.sh" "gp_${1}_${arm}_$i" $X || echo "FAILED gp_${1}_${arm}_$i rc=$?"
  done
done
