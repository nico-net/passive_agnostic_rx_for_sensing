#!/bin/bash
# TIMRM A/B matrix over the four raw captures, one replay at a time (each through heavy.sh via timacq_replay.sh).
# usage: ENVFILE=<t26z.env> B=<build dir> ARMS="off rv rf" [TAG=rmx] tests/passive_rx/rmx_replay_matrix.sh
#   arms: off = nothing, rv = ISAC_PDSCH_READING_RV=1, rf = ISAC_PDSCH_REP_FIELDS=1
#   logs: replays/replay_<TAG>_<cap>_<arm><rep>_*/run.log ; replays/MATRIX_DONE when finished
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
O=/home/nicola/NICOLA/ota_runs
ARMS=${ARMS:-"off rf"}; TAG=${TAG:-rmx}
cd "$ROOT"
run() { # cap_name reps ; env: CAP CAP_SAMPLES CAP_TICK CAPC RXC SSB FO
  local n=$1 reps=$2
  for rep in $(seq 1 "$reps"); do
    for arm in $ARMS; do
      X=()
      case $arm in rv) X=(ISAC_PDSCH_READING_RV=1) ;; rf) X=(ISAC_PDSCH_REP_FIELDS=1) ;; esac
      tests/passive_rx/timacq_replay.sh "${TAG}_${n}_${arm}${rep}" "${X[@]}"
    done
  done
}
export CAP=$O/raw_pci923_tim_12s_tile3 CAP_SAMPLES=1474560000 CAP_TICK=195322029 CAPC=3731910000 RXC=3759990000 SSB=233 FO=-4800
run tile3 ${REPS_TILE3:-3}
export CAP=$O/raw_pci923_tim_4s_1010_1853 CAP_SAMPLES=491520000 CAP_TICK=195322029 CAPC=3731910000 RXC=3731910000 SSB=1169 FO=-4800
run tim4s ${REPS_TIM4S:-1}
export CAP=$O/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900
run b415 ${REPS_B415:-2}
export CAP=$O/raw_pci415_12s_20261008 CAP_SAMPLES=1474560000 CAP_TICK=210036277 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900
run c415 ${REPS_C415:-2}
echo MATRIX_DONE > replays/MATRIX_DONE
