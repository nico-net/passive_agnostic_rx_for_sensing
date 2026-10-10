#!/bin/bash
# replay_survey.sh CAPDIR CAPCENTER_HZ RXC_HZ [SPEED] : raw_iq replay of a survey capture, receiver tuned (digitally) to RXC_HZ.
# Run via heavy.sh. Receiver args = ota recipe (-r 217 --ssb ${SSB:-1171}) with -C = SSB GSCN + 330 kHz.
CAP=$1; CC=$2; RXC=$3; SPEED=${4:-0.5}
B=${B:-/home/nicola/NICOLA/wt/rr-csd-a/cmake_targets/ran_build/build_ota}
O=/home/nicola/NICOLA/wt/rr-orchestration
NS=$(python3 -c "import json;print(json.load(open('$CAP/capture_info.json'))['samples_per_channel'])")
TICK=$(python3 -c "import json;print(json.load(open('$CAP/capture_info.json'))['first_sample_tick'])")
R=/home/nicola/NICOLA/ota_runs/survey_replay_$(basename $CAP)_${RXC}_$(date +%H%M%S); mkdir -p $R
{ cat $O/csd_replay_base.cfg; echo 'device = { name = "raw_iq"; };'; echo "loader = { raw_iq = { shlibpath = \"$B\"; }; };"; } > $R/replay.cfg
cd $R
env ISAC_CORE_PLAN=0 ISAC_MLOCK=0 ISAC_RAW_IQ_DIRECTORY=$CAP ISAC_RAW_IQ_SAMPLES=$NS ISAC_RAW_IQ_FIRST_TICK=$TICK ISAC_RAW_IQ_CHANNELS=4 \
  ISAC_RAW_IQ_RATE=122880000 ISAC_RAW_IQ_CENTER=$CC ISAC_RAW_IQ_SPEED=$SPEED ISAC_RAW_IQ_SHIFT=2 LD_LIBRARY_PATH=$B:/usr/local/lib ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 \
  timeout --signal=INT -k 60 600 $B/nr-uesoftmodem -O $R/replay.cfg --passive-rx -r 217 --numerology 1 --band 78 -C $RXC --ssb ${SSB:-1171} \
  --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo ${FO:-0} --ue-fo-compensation --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > run.log 2>&1
echo "done $R"; sed 's/\x1b\[[0-9;]*m//g' run.log | grep -E "Initial sync|MIB|SIB1|synch Failed|Cell ID|PCI|RAW_IQ_EOF|CSS0" | head -20
