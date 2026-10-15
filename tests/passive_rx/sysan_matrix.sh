#!/bin/bash
# SYSAN ablation matrix: t26f + ISAC_E2E_TRACE=1 per arm, interleaved, CAPSETs b and tim, REPS repetitions.
# usage: REPS=2 ARMS="F P0 R0 U0 D0" sysan_matrix.sh TAG   (each replay takes heavy.lock itself)
cd "$(dirname "$0")/../.."
E=/home/nicola/NICOLA/wt/rr-orchestration/ota/${ENVNAME:-t26f}.env
arm() { case $1 in
  F) echo ;;
  P0) echo ISAC_PDSCH_SOLVER_PROBE_RATE=0 ;;
  R0) echo ISAC_PDSCH_TDRA_RETRY=0 ISAC_PDSCH_CSI_RETRY=0 ISAC_PDSCH_DMRS_CDM_RETRY=0 ISAC_PDSCH_CSI_POPULATION_RETRY=0 ISAC_PDSCH_WIDE_MRC_RETRY=0 ;;
  U0) echo ISAC_UL_PUSCH_CHAIN=0 ;;
  D0) echo ISAC_PDCCH_NID_GPU=0 ISAC_CSIRS_DISC=0 ;;
  *) echo "${!1}" ;;   # custom arm: env var named like the arm holds its tokens
esac; }
for r in $(seq 1 ${REPS:-2}); do for c in ${CAPS:-b tim}; do for a in ${ARMS:-F P0 R0 U0 D0}; do
  CAPSET=$c ENVFILE=$E tests/passive_rx/p99_replay.sh "$1_${c}_${a}_$r" ISAC_E2E_TRACE=1 $(arm $a)
  echo "done $1_${c}_${a}_$r rc=$?"
done; done; done
