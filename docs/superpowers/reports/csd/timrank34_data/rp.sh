#!/bin/bash
# rp.sh <cap: tim|tim4|b|old|p421> <name> [ENV=..]...   (ENVF overrides env file)
SP=${SP:-/tmp}   # CUDA_MPS_PIPE_DIRECTORY=$SP/nomps: private, bypasses a root MPS daemon
W=$(cd "$(dirname "$0")/../../../../.." && pwd)
cap=$1; shift; name=$1; shift
export B=$W/cmake_targets/ran_build/build_t26 ENVFILE=${ENVF:-/home/nicola/NICOLA/wt/rr-orchestration/ota/t26mtd.env} SPEED=${SPEED:-0.5}
O=/home/nicola/NICOLA/ota_runs
case $cap in
 tim) export CAP=$O/raw_pci923_tim_12s_tile3 CAPC=3731910000 RXC=3759990000 SSB=233 FO=-4800 CAP_TICK=195322029 CAP_SAMPLES=${CS:-1474560000};;
 tim4) export CAP=$O/raw_pci923_tim_4s_1010_1853 CAPC=3731910000 RXC=3759990000 SSB=233 FO=-4800 CAP_TICK=195322029 CAP_SAMPLES=491520000;;
 b) export CAP=$O/raw_pci415_12s_b CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900 CAP_TICK=208029025 CAP_SAMPLES=${CS:-1474560000};;
 old) export CAP=$O/raw_pci415_12s_20261008 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900 CAP_TICK=210036277 CAP_SAMPLES=${CS:-1474560000};;
 p421) export CAP=$O/raw_pci421_aligned_20261006 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900 CAP_TICK=209744976 CAP_SAMPLES=491520000;;
 *) echo bad cap; exit 2;;
esac
exec $W/tests/passive_rx/timacq_replay.sh ${cap}_$name CUDA_MPS_PIPE_DIRECTORY=$SP/nomps "$@"
