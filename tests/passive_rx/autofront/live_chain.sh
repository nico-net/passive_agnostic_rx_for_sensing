#!/bin/bash
# live_chain.sh : automatic blind front end for the LIVE chain (no hand -C / --ssb / --initial-fo). n78/n77, 30 kHz SCS.
#   live_chain.sh capture OUT CENTER_MHZ...   ORCHESTRATOR ONLY (radio): survey_capture.sh (one heavy.lock hold, X410 RX1, gain 38); SAMPLES=122880000 for 1 s
#                                             (default 0.3 s = 15 chunks: enough for strong cells, weak ones want 1 s). Captures land in OUT/c<CENTER>.
#   live_chain.sh discover OUT CAPDIR...      fastscan.py (~7 s per 1 s x 4 ch capture) -> OUT/cells.json, OUT/triples.txt (PCI, GSCN, pass-1 -C/--ssb/--initial-fo),
#                                             OUT/specs.txt (OTA window spec lines TAG:ENV:CFG:FREQ:SSB:FO; ENVF/CFGF select env/cfg)
#   live_chain.sh all OUT CENTER_MHZ...       capture, then discover on OUT/c*
#   live_chain.sh pass2 OUT PCI RUN.LOG [ENVF CFGF]      after the first receiver run of that cell synced+MIB: k_SSB -> --ssb 1164+k_SSB/2, SIB1 PRB/offsetToPointA/PLMN
#                                             into OUT/cells.json, OUT/specs.txt rewritten with pass-2 triples; retune_args.py line if the log has an
#                                             'ACQ carrier MISMATCH' (-C/-r/--ssb that put the SIB1 carrier inside an RB-aligned grid).
# discover/pass2 are offline (no radio). Receiver start per cell: nr-uesoftmodem -C <C> --ssb <ssb> --initial-fo <fo> (triples.txt), SIGINT to stop.
set -euo pipefail
D=$(cd "$(dirname "$0")/../../.." && pwd); AF="$D/tests/passive_rx/autofront"; H=/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh
case ${1:-} in
capture) out=$2; shift 2; "$D/tests/passive_rx/survey/survey_capture.sh" "$out" "$@" ;;
discover)
  out=$2; shift 2; mkdir -p "$out"
  "$H" python3 -I "$AF/fastscan.py" "$@" --out "$out/cells.json" 2> "$out/discover.log" > "$out/discover.out"; head -n 1 "$out/discover.out"
  python3 -I "$AF/autofront.py" args "$out/cells.json" | tee "$out/triples.txt"
  EF=${ENVF:-ota/t26t.env}; CF=${CFGF:-tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg}; echo "$EF $CF" > "$out/envcfg.txt"  # pass2 reuses them
  python3 -I "$AF/autofront.py" specs "$out/cells.json" "$EF" "$CF" > "$out/specs.txt"
  echo "specs: $out/specs.txt ($(wc -l < "$out/specs.txt") cells)" ;;
all) out=$2; shift 2; "$0" capture "$out" "$@"; "$0" discover "$out" "$out"/c* ;;
pass2)
  out=$2; pci=$3; log=$4
  # env/cfg: 5th/6th argument, else ENVF/CFGF, else what discover stored in OUT/envcfg.txt, else the defaults
  [ -f "$out/envcfg.txt" ] && read -r E0 C0 < "$out/envcfg.txt" || { E0=ota/t26t.env; C0=tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg; }
  EF=${5:-${ENVF:-$E0}}; CF=${6:-${CFGF:-$C0}}; echo "$EF $CF" > "$out/envcfg.txt"
  python3 -I "$AF/autofront.py" refine "$out/cells.json" "$pci" "$log"
  python3 -I "$AF/autofront.py" specs "$out/cells.json" "$EF" "$CF" > "$out/specs.txt"
  python3 -I "$AF/autofront.py" args "$out/cells.json" --log "$log"
  python3 -I "$D/tests/passive_rx/survey/retune_args.py" "$log" || true ;;
*) sed -n '2,13p' "$0"; exit 2 ;;
esac
