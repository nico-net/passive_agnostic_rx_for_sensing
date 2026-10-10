#!/bin/bash
# DECQUEUE replay arm: arm=A|B|C, cap=b|c, script=timacq_replay.sh|timul_replay.sh, SPEED default 0.25. Prints the replay dir.
# A = t26q (OTA 2026-10-10 env), B = A + UL/scan isolation, C = B + retry prediction/no-restore.
set -u
arm=$1; cap=$2; script=${3:-timul_replay.sh}
D=$(cd "$(dirname "$0")" && pwd)
case $cap in
  b) CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b; TICK=208029025;;
  c) CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008; TICK=210036277;;
esac
base="ISAC_PDSCH_DMRS_ID_SCAN=16 ISAC_UL_LOCK_SIB1_FIRST=1 ISAC_UL_LOCK_CRC_FIRST=1 ISAC_UL_LOCK_CRC_K=3 ISAC_PDCCH_DISCOVERED_FILE=/home/nicola/NICOLA/wt/rr-csd-dtq/tests/passive_rx/dgx/scanfast_coresets.txt ISAC_PDSCH_CORES=7,8,9,3,4 ISAC_GPU_WORKER_CORE=10 ISAC_PDCCH_NID_GPU_CORE=13 ISAC_PDCCH_USS_CORE=12 ISAC_PDTIM_PCT=1"
iso="ISAC_PUSCH_CORES=6 ISAC_PUSCH_PRIO=30 ISAC_PDSCH_DMRS_SCAN_ASYNC=1 ISAC_DMRS_SCAN_CORE=2"
ret="ISAC_PDSCH_RETRY_PREDICT=1 ISAC_PDSCH_RETRY_NORESTORE=1"
case $arm in A) x="$base";; B) x="$base $iso";; C) x="$base $iso $ret";; esac
export HEAVY_CPUS=${HEAVY_CPUS:-0-14}
CAP=$CAP CAP_TICK=$TICK CAP_SAMPLES=1474560000 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900 SPEED=${SPEED:-0.25} \
  ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26q.env "$D/$script" "dtq${arm}${cap}" $x
