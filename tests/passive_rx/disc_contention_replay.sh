#!/bin/bash
# rank2_iter_replay.sh : one raw_iq replay of a stored capture at a given PDSCH LDPC iteration cap (DISC-CONTENTION, replay only, no RF).
# usage: CAP=<capture dir> CAPC=<capture centre Hz> RXC=<-C Hz> SSB=<--ssb> FO=<--initial-fo> [RB=217] [SPEED=1] [DUR=45] [B=<dir with nr-uesoftmodem + libraw_iq.so + module libs>] [EXTRA_ENV="K=V ..."] disc_contention_replay.sh <name> <arm-label>
#        [B=<build dir with nr-uesoftmodem + libraw_iq.so>] [ENVFILE=<default rr-orchestration/ota/FROZEN_20261018.env>]
#        rank2_iter_replay.sh <name> <cap>      (cap = NR_TEST_PDSCH_LDPC_ITERS, test-only)
# Same recipe as survey/timacq_nant.sh of csd/blind-front (main env FROZEN_20261018 minus radio-only/core flags, cfg sd_t26ul, 4 RX).
# Goes through heavy.sh and holds /tmp/td_measure.lock (timing is measured). Output: replays/<name>_cap<cap>_<time>/run.log (path in replays/last).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration
name=$1; arm=$2
B=${B:-$ROOT/cmake_targets/ran_build/build_t26}; ENVFILE=${ENVFILE:-$O/ota/FROZEN_20261018.env}; CFG=${CFG:-$O/ota/sd_t26ul.cfg}
SPEED=${SPEED:-1}; RB=${RB:-217}; DUR=${DUR:-45}
J=$CAP/capture_info.json
CAP_SAMPLES=$(python3 -I -c "import json;print(json.load(open('$J'))['samples_per_channel'])"); CAP_TICK=$(python3 -I -c "import json;print(json.load(open('$J'))['first_sample_tick'])")
read -r -a envs < "$ENVFILE"; base=()
for kv in "${envs[@]}"; do
  case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*) ;; *) base+=("$kv") ;; esac
done
case $RB in 51) DEC=4 ;; 106|133|162) DEC=2 ;; *) DEC=1 ;; esac
R=${OUT:-$ROOT/replays}/${name}_${arm}_$(date +%H%M%S); mkdir -p "$R"; echo "$R" > "$ROOT/replays/last"
{ cat "$CFG"; echo 'device = { name = "raw_iq"; };'; echo "loader = { raw_iq = { shlibpath = \"$B\"; }; };"; } > "$R/replay.cfg"
echo "CAP=$CAP CAPC=$CAPC RXC=$RXC SSB=$SSB FO=$FO RB=$RB SPEED=$SPEED ARM=$arm DECIM=$DEC" > "$R/args.txt"
cd "$R"
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
exec "$O/heavy.sh" flock -x /tmp/td_measure.lock env ISAC_MLOCK=0 ISAC_RAW_IQ_DIRECTORY="$CAP" ISAC_RAW_IQ_SAMPLES="$CAP_SAMPLES" ISAC_RAW_IQ_FIRST_TICK="$CAP_TICK" \
  ISAC_RAW_IQ_CHANNELS=4 ISAC_RAW_IQ_RATE=122880000 ISAC_RAW_IQ_CENTER="$CAPC" ISAC_RAW_IQ_SPEED="$SPEED" ISAC_RAW_IQ_SHIFT=2 ISAC_RAW_IQ_DECIM=$DEC \
  LD_LIBRARY_PATH="$B:/usr/local/lib" ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 "${base[@]}" ISAC_PDSCH_CORES=8,9,12,13,14 ISAC_PDCCH_NID_GPU_CORE=${NID_CORE:-11} \
  timeout --signal=INT -k 60 "$DUR" ${NSYS:-} "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r "$RB" --numerology 1 --band 78 \
  -C "$RXC" --ssb "$SSB" --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo "$FO" --ue-fo-compensation \
  --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/run.log" 2>&1
