#!/bin/bash
# run_live_replay.sh: MPCI-LIVE (A12) end-to-end fixture -- ONE nr-uesoftmodem, TWO cells from file-published IQ, and the inventory <run dir>/cells.json REWRITTEN
#   (atomic rename) while it runs: t=T1 cell 1 (Iliad 403) removed -> after the retire hysteresis its slot thread parks outside the radio gate, cell 0 (Vodafone 415)
#   keeps decoding; t=T2 cell 1 back -> its ring resumes, the slot re-activates, resyncs and decodes; t=T3 a third inventory cell (TIM 923) appears -> no free slot:
#   refused and logged (`MPCI_CELL ... event=refuse`), cell 0 and 1 untouched. The publisher (mpci_file_pub watch=) pauses/resumes the rings like the in-process
#   channelizer does. No radio, no X410; published-ring input (the feed/radio path of live changes is covered by test_iq_live).
# usage: run_live_replay.sh [name]      env as run_two_cell_replay.sh: B=<build dir> SPEED=0.25 CAP415=.. CAP403=..
# Host rules: never while a receiver runs or the heavy lock is held; shared td_measure lock; nice 10; SIGINT only.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration
name=${1:-live}
B=${B:-$ROOT/cmake_targets/ran_build/build}; CFG=${CFG:-$O/ota/sd_t26ul.cfg}; ENVFILE=${ENVFILE:-$O/ota/FROZEN_20261018.env}
SPEED=${SPEED:-0.25}; T1=60; T2=130; T3=185; DUR=250
CAP415=${CAP415:-/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b}; CAP403=${CAP403:-/home/nicola/NICOLA/ota_runs/raw_iliad403_4s_1023_2315}
if pgrep -u "$(id -un)" -x nr-uesoftmodem >/dev/null; then echo "a receiver is running: not starting" >&2; exit 3; fi # our receivers only (another user runs a docker UE)
if ! flock -n "$O/locks/heavy.lock" true; then echo "heavy.lock is held: not starting" >&2; exit 3; fi
src() { # <ring> <capture dir> -> publisher source argument (geometry from the capture's own capture_info.json)
  python3 -I -c "import json,sys;d=json.load(open(sys.argv[2]+'/capture_info.json'));print('%s=%s,%d,%d,%d,%d,%d,2'%(sys.argv[1],sys.argv[2],d['samples_per_channel'],d['first_sample_tick'],d['channels'],d['sample_rate_hz'],d['rf_channels'][0]['frequency_hz']))" "$1" "$2"
}
R=$ROOT/replays/${name}_$(date +%H%M%S); mkdir -p "$R"; echo "$R" > "$ROOT/replays/last"
# the inventory the receiver reads at start (its run dir = the directory of its stdout)
python3 -I - "$ROOT/tests/passive_rx/mpci/inventory/live_tuned_20261023.json" "$R/cells.json" <<'PY'
import json, os, sys
d = json.load(open(sys.argv[1])); keep = {'pci415', 'pci403'}
d['cells'] = [c for c in d['cells'] if c['id'] in keep]
d.pop('_comment', None)
tmp = sys.argv[2] + '.tmp'; json.dump(d, open(tmp, 'w'), indent=1); os.rename(tmp, sys.argv[2])
PY
{ cat "$CFG"; echo 'device = { name = "shm_iq"; };'; echo "loader = { shm_iq = { shlibpath = \"$B\"; }; };"; } > "$R/replay.cfg"
read -r -a envs < "$ENVFILE"; base=()
for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_*_CORES=*|ISAC_*_CORE=*) ;; *) base+=("$kv") ;; esac; done
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
exec 9>/tmp/td_measure.lock; flock -s 9
nice -n 10 "$B/mpci_file_pub" "$SPEED" "$((DUR + 60))" "watch=$R/cells.json" "$(src nrcell_pci415 "$CAP415")" "$(src nrcell_pci403 "$CAP403")" > "$R/pub.log" 2>&1 &
PUB=$!
trap 'kill -INT $PUB 2>/dev/null || true' EXIT
for i in $(seq 1 100); do grep -q "ring=/nrcell_pci403" "$R/pub.log" 2>/dev/null && break; sleep 0.2; done
cd "$R"
rewrite() { # <ids...>: atomic rewrite of cells.json with the given fixture ids
  python3 -I - "$ROOT/tests/passive_rx/mpci/inventory/live_tuned_20261023.json" "$R/cells.json" "$@" <<'PY'
import json, os, sys
d = json.load(open(sys.argv[1])); keep = set(sys.argv[3:])
d['cells'] = [c for c in d['cells'] if c['id'] in keep]; d.pop('_comment', None)
tmp = sys.argv[2] + '.tmp'; json.dump(d, open(tmp, 'w'), indent=1); os.rename(tmp, sys.argv[2])
PY
}
( sleep $T1; rewrite pci415; echo "t=$T1 cell 1 removed" >> "$R/driver.log"
  sleep $((T2 - T1)); rewrite pci415 pci403; echo "t=$T2 cell 1 back" >> "$R/driver.log"
  sleep $((T3 - T2)); rewrite pci415 pci403 pci923; echo "t=$T3 third cell added" >> "$R/driver.log" ) &
DRV=$!
set +e
env ISAC_MLOCK=0 LD_LIBRARY_PATH="$B:/usr/local/lib" ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 "${base[@]}" \
  timeout --signal=INT -k 60 "$DUR" nice -n 10 "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r 217 --numerology 1 --band 78 \
  -C 3680010000 --ssb 1171 --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo -4827 --ue-fo-compensation \
  --usrp-args nrcell_pci415 --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/run.log" 2>&1
rc=$?
kill $DRV 2>/dev/null; kill -INT $PUB 2>/dev/null; wait $PUB 2>/dev/null
echo "rc=$rc run=$R"
# STOP evidence (A12): per phase counts of cell-1 / cell-0 decode lines, the lifecycle lines, no radio event
echo "== driver"; cat driver.log
echo "== lifecycle"; grep -E "MPCI_LIFE|MPCI_CELL id=|event=(retire|add|refuse)|MPCI_PUB" run.log pub.log | head -40
for c in 0 1; do
  echo "== cell $c: sync $(grep -c "^\[c$c\].*UE synchronized" run.log) RXDISCONT $(grep -c "^\[c$c\].*RXDISCONT" run.log) DCI $(grep -c "^\[c$c\].*DCI" run.log) CRC-ok-lines $(grep -c "^\[c$c\].*CRC.*ok" run.log)"
done
echo "== radio events (expected 0): $(grep -cE "USRP_RING_RECOVER|USRP_RING_RESTART|RFSTALL|nrue_ru_restart|reinit" run.log)"
