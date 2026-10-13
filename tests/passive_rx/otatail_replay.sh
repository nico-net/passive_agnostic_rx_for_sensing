#!/bin/bash
# OTATAIL replay arm: Vodafone PCI 415 capture b at live speed with the OTA env ota/t26bI.env (radio flags dropped by
# timacq_replay.sh), build_t26 (CUDA). usage: otatail_replay.sh <name> [ENV=VAL ...]
#   A arm: otatail_replay.sh A ISAC_GPU_HEARTBEAT=1
#   B arm: otatail_replay.sh B ISAC_GPU_HEARTBEAT=1 ISAC_CSIRS_DISC_BUDGET=1 ISAC_PDCCH_NID_GPU_RECHECK_MS=10000
# Stats: otatail_stats.py <replay dir>/run.log
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
O=/home/nicola/NICOLA/wt/rr-orchestration
exec env B=${B:-$ROOT/cmake_targets/ran_build/build_t26} ENVFILE=${ENVFILE:-$O/ota/t26bI.env} \
  CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 \
  CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900 SPEED=${SPEED:-1} TMO=${TMO:-150} \
  "$ROOT/tests/passive_rx/timacq_replay.sh" "$@"
