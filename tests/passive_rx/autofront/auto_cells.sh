#!/bin/bash
# auto_cells.sh : automatic blind front end driver (offline). No hand -C / --ssb / --initial-fo.
#   auto_cells.sh discover OUTDIR CAPDIR...      cells.json + args.txt (receiver args, pass 1) + specs.txt (OTA window specs TAG:ENV:CFG:FREQ:SSB:FO)
#   auto_cells.sh replay CELLS_JSON PCI CAPDIR [NAME]   replay CAPDIR (raw_iq, long/tiled enough for start-up) with the AUTO triple, then pass 2
#                                                from the MIB (k_SSB) of that run and a second replay. Prints sync/MIB/SIB1 lines.
# Python heavy parts run via heavy.sh (one heavy job at a time). Env: ENVF (receiver env file), CFGF (receiver cfg) for specs.txt; B build dir for replay.
# Live use (orchestrator): short wideband capture (survey_capture.sh style, 1 s x 4 ch per centre) -> discover -> specs.txt -> window script;
# several cells at once = one receiver per spec once the multi-cell receiver (docs/.../AUTOFRONT-20261012.md, "N receivers") exists.
set -euo pipefail
D=$(cd "$(dirname "$0")/../../.." && pwd); AF="$D/tests/passive_rx/autofront/autofront.py"; H=/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh
case ${1:-} in
discover)
  out=$2; shift 2; mkdir -p "$out"
  "$H" python3 -I "$AF" discover "$@" --out "$out/cells.json" > "$out/discover.log" 2>&1; tail -n 12 "$out/discover.log" | tr '\r' '\n' | grep -v '^ *chunk'
  python3 -I "$AF" args "$out/cells.json" | tee "$out/args.txt"
  python3 -I "$AF" specs "$out/cells.json" "${ENVF:-ota/t26t.env}" "${CFGF:-tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg}" > "$out/specs.txt" ;;
replay)
  cells=$2; pci=$3; cap=$4; name=${5:-af$pci}; J=$cap/capture_info.json
  export CAP=$cap CAPC=$(python3 -I -c "import json;print(json.load(open('$J'))['rf_channels'][0]['frequency_hz'])")
  export CAP_SAMPLES=$(python3 -I -c "import json;print(json.load(open('$J'))['samples_per_channel'])") CAP_TICK=$(python3 -I -c "import json;print(json.load(open('$J'))['first_sample_tick'])")
  export B=${B:-/home/nicola/NICOLA/wt/rr-csd-b/cmake_targets/ran_build/build_t26} ENVFILE=${ENVFILE:-/home/nicola/NICOLA/wt/rr-orchestration/ota/t26w.env} SPEED=${SPEED:-0.25}
  log=
  for pass in 1 2; do
    read -r RXC SSB FO < <(python3 -I "$AF" triple "$cells" "$pci" $log); export RXC SSB FO
    [ "$pass" = 2 ] && [ "$RXC $SSB $FO" = "$prev" ] && { echo "== pass 2 triple identical to pass 1: done"; break; }; prev="$RXC $SSB $FO"
    echo "== pass $pass: -C $RXC --ssb $SSB --initial-fo $FO"
    "$D/tests/passive_rx/survey/timacq_nant.sh" "${name}_p$pass" > /dev/null 2>&1 || true
    R=$(cat "$D/replays/last"); log=$R/run.log
    sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -E "Initial sync successful|MIB fields|SIB1 CELL|extracted from SIB1|synch Failed|Segmentation|Assertion" | sort | uniq -c | sort -rn | head -8
    grep -q "MIB fields" "$log" || break
  done ;;
*) sed -n '2,9p' "$0"; exit 2 ;;
esac
