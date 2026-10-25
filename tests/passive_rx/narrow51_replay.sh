#!/bin/bash
# Replay the 1 s PCI 675 survey capture (c3430, 122.88 MS/s, centre 3430 MHz) on a wide (217 PRB) or narrow (51 PRB, decimated x4) grid.
# usage: narrow51_replay.sh <wide|n51> <name> [ENV=VAL ...]   (B=build dir, CAP=capture dir, FOFF=initial fo)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration
B=${B:-/home/nicola/NICOLA/wt/rr-main-merge/cmake_targets/ran_build/build_t26}
CAP=${CAP:-/home/nicola/NICOLA/wt/rr-n51/replays/cap3x}  # 3 x the 1 s survey capture back to back (acquisition needs >1 s of samples before it searches)
case $1 in
  wide) RB=217; RXC=3426570000; SSB=1171; DEC=1 ;;
  n51)  RB=51;  RXC=3425010000; SSB=227;  DEC=4 ;;
  *) echo "wide|n51"; exit 2 ;;
esac
RXC=${RXC_OVR:-$RXC}; SSB=${SSB_OVR:-$SSB}
shift; name=$1; shift
read -r -a envs < "${ENVFILE:-$O/ota/FROZEN_20261018.env}"
base=(); for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*) ;; *) base+=("$kv") ;; esac; done
R=$ROOT/replays/n51_${name}_$(date +%H%M%S); mkdir -p "$R"; echo "$R"
{ cat "${CFG:-$O/ota/sd_t26ul.cfg}"; echo 'device = { name = "raw_iq"; };'; echo "loader = { raw_iq = { shlibpath = \"${RAWLIB:-$B}\"; }; };"; } > "$R/replay.cfg"
cd "$R"
HEAVY_CPUS=${HEAVY_CPUS:-0-16} "$O/heavy.sh" timeout --signal=INT -k 60 ${TMO:-150} env \
  ISAC_MLOCK=0 ISAC_RAW_IQ_DIRECTORY="$CAP" ISAC_RAW_IQ_SAMPLES=${CAPSAMPLES:-368640000} ISAC_RAW_IQ_FIRST_TICK=${CAPTICK:-194970955} \
  ISAC_RAW_IQ_CHANNELS=4 ISAC_RAW_IQ_RATE=122880000 ISAC_RAW_IQ_CENTER=${CAPC:-3430000000} ISAC_RAW_IQ_SPEED=${SPEED:-0.25} ISAC_RAW_IQ_SHIFT=2 ISAC_RAW_IQ_DECIM=$DEC \
  LD_LIBRARY_PATH="$B:/usr/local/lib" CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps \
  ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 "${base[@]}" ISAC_PDSCH_CORES=8,9,12,13,14 ISAC_PDCCH_NID_GPU_CORE=11 ISAC_MLOCK=0 "$@" \
  "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r $RB --numerology 1 --band 78 \
  -C $RXC --ssb $SSB --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo ${FOFF:--4825} --ue-fo-compensation \
  --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/run.log" 2>&1 || true
sed 's/\x1b\[[0-9;]*m//g' "$R/run.log" | grep -E "Initial sync successful|MIB fields|rsrp|CHRMS|Error decoding PBCH|RFCENSUS|RFSTALL|TRACKLOCK" | cut -c1-200 | sort | uniq -c | sort -rn | head -12
