#!/bin/bash
# newcell_replay.sh CAPNAME CAPC_HZ GSCN_SSB_MHZ SSB FO [SHIFT_HZ] : one timacq_replay.sh run on a 1 s survey capture (ota_runs/survey_1s_1011/CAPNAME),
# -C = GSCN + (1182-SSB)*0.03 MHz + SHIFT_HZ. Prints sync/MIB/SIB1 lines. Takes heavy.lock via timacq_replay.sh. Replay only.
set -u
D=$(cd "$(dirname "$0")/../../.." && pwd); CN=$1; CAPC=$2; G=$3; SSB=$4; FO=$5; SH=${6:-0}
RXC=$(python3 -c "print(round($G*1e6+(1182-$SSB)*30e3+$SH))")
CAP=${TILED:-/home/nicola/NICOLA/ota_runs/newcells_tiled}/$CN
J=$CAP/capture_info.json
export CAP CAPC RXC SSB FO SPEED=${SPEED:-0.25} B=${B:-$D/cmake_targets/ran_build/build} ENVFILE=${ENVFILE:-/home/nicola/NICOLA/wt/rr-orchestration/ota/t26u.env}
export CAP_SAMPLES=$(python3 -c "import json;print(json.load(open('$J'))['samples_per_channel'])") CAP_TICK=$(python3 -c "import json;print(json.load(open('$J'))['first_sample_tick'])")
"$D/tests/passive_rx/survey/timacq_nant.sh" "nc_${CN}_${SSB}_${FO}_${SH}_n${NANT:-4}" >/dev/null 2>&1; RC=$?
R=$(cat "$D/replays/last"); echo "== n=${NANT:-4} rc=$RC $CN ssb=$SSB C=$RXC fo=$FO -> $R"
sed 's/\x1b\[[0-9;]*m//g' "$R/run.log" | grep -E "Segmentation|SEGV|Assertion|stack|Initial sync|k_ssb|kssb|MIB|SIB1|PLMN|plmn|synch Failed|SIB1 DERIV|acq_sib1|MIB fields|CRC|offsetToPointA|RAW_IQ_EOF|CSS0" | sort | uniq -c | sort -rn | head -${LINES_MAX:-12}
