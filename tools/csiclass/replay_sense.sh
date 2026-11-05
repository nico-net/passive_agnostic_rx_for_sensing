#!/bin/bash
# CSIRS-CLASS: replay a stored 4-RX raw capture with the sensing export on (records + CFR with csi_rs, pdsch_dmrs), same recipe as
# tests/passive_rx/rank2_iter_replay.sh (FROZEN_20261018 env minus radio flags). Diagnostic only, no receiver change.
# usage: CAP=<dir> CAPC=<capture centre Hz> RXC=<-C Hz> SSB=<n> FO=<hz> [RB=217] [SPEED=0.25] [DUR=120] [B=<build>] replay_sense.sh <name>
set -euo pipefail
O=/home/nicola/NICOLA/wt/rr-orchestration; name=$1
B=${B:-/home/nicola/NICOLA/wt/rr-twomodes/cmake_targets/ran_build/build}; ENVFILE=${ENVFILE:-$O/ota/FROZEN_20261018.env}; CFG=${CFG:-$O/ota/sd_t26ul.cfg}
SPEED=${SPEED:-0.25}; RB=${RB:-217}; DUR=${DUR:-120}; J=$CAP/capture_info.json
CAP_SAMPLES=$(python3 -I -c "import json;print(json.load(open('$J'))['samples_per_channel'])"); CAP_TICK=$(python3 -I -c "import json;print(json.load(open('$J'))['first_sample_tick'])")
read -r -a envs < "$ENVFILE"; base=()
for kv in "${envs[@]}"; do
  case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*) ;; *) base+=("$kv") ;; esac
done
R=/home/nicola/NICOLA/wt/rr-csiclass/.superpowers/tmp/rep_$name; mkdir -p "$R"
{ cat "$CFG"; echo 'device = { name = "raw_iq"; };'; echo "loader = { raw_iq = { shlibpath = \"$B\"; }; };"; } > "$R/replay.cfg"
echo "CAP=$CAP CAPC=$CAPC RXC=$RXC SSB=$SSB FO=$FO RB=$RB SPEED=$SPEED B=$B CAP_TICK=$CAP_TICK" > "$R/args.txt"
cd "$R"; export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
unset ISAC_RX_BRANCH_FO
cat > "$R/inner.sh" <<'INNER'
#!/bin/bash
# tries the FO list; keeps the first attempt that reports SYNC_SCAN detected=1 within 100 s
cd "$R"
for FOV in $FO_LIST; do
  : > run.log
  env ISAC_MLOCK=0 ISAC_RAW_IQ_DIRECTORY="$CAP" ISAC_RAW_IQ_SAMPLES="$CAP_SAMPLES" ISAC_RAW_IQ_FIRST_TICK="$CAP_TICK" ISAC_RAW_IQ_CHANNELS=4 ISAC_RAW_IQ_RATE=122880000 \
   ISAC_RAW_IQ_CENTER="$CAPC" ISAC_RAW_IQ_SPEED="$SPEED" ISAC_RAW_IQ_SHIFT=2 ISAC_RAW_IQ_DECIM=1 LD_LIBRARY_PATH="$B:/usr/local/lib" ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 "${base[@]}" \
   ISAC_PDSCH_CORES=8,9,12,13,14 ISAC_PDCCH_NID_GPU_CORE=11 \
   timeout --signal=INT -k 60 "$DUR" "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r "$RB" --numerology 1 --band 78 -C "$RXC" --ssb "$SSB" --ue-rxgain 38 \
   --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo "$FOV" --ue-fo-compensation --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 \
   --sensing.enable 1 --sensing.sources csi_rs,pdsch_dmrs --sensing.records_path "$R/rec.jsonl" --sensing.cfr_path "$R/rec.cfr" --sensing.cfr_antennas 4 \
   --sensing.cfr_ring_frames 64 --sensing.cfr_max_re 46200 --sensing.rec_ring_slots 2048 < /dev/null > run.log 2>&1 &
  PID=$!; ok=0
  for i in $(seq 1 100); do sleep 1; kill -0 $PID 2>/dev/null || break; if grep -q "SYNC_SCAN.*detected=1" run.log; then ok=1; break; fi; done
  echo "FO=$FOV sync_ok=$ok" >> fo_tries.txt
  if [ $ok = 1 ]; then wait $PID; exit 0; fi
  pkill -INT -x nr-uesoftmodem -P $$ 2>/dev/null; kill -INT $PID 2>/dev/null; wait $PID
done
INNER
chmod +x "$R/inner.sh"
export CAP CAPC RXC SSB RB SPEED DUR B CAP_SAMPLES CAP_TICK R FO_LIST="${FO_LIST:-$FO}"; export -a base 2>/dev/null || true
exec "$O/heavy.sh" flock -x /tmp/td_measure.lock bash -c 'declare -a base=('"${base[*]}"'); source "$R/inner.sh"'
