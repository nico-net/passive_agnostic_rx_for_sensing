#!/bin/bash
# run_two_cell_replay.sh: multi-PCI A8 STOP fixture -- ONE nr-uesoftmodem process, TWO cells, from file-published IQ (no radio, no X410).
#   cell 0 = Vodafone PCI 415 (started cell, command-line tuning), cell 1 = Iliad PCI 403 (from the inventory <run dir>/cells.json).
#   IQ: tests/passive_rx/mpci/mpci_file_pub publishes the two stored captures as POSIX rings nrcell_pci415 / nrcell_pci403; the receiver's
#   cells read them through the shm_iq consumer device (configured device "shm_iq"). Tunings: the live front-end tunings of
#   tests/passive_rx/mpci/inventory/live_tuned_20261023.json (receiver-side detections; no gNB log or config is an input).
# usage: run_two_cell_replay.sh [name]      env: B=<build dir> SPEED=0.25 DUR=240 CAP415=.. CAP403=.. (paths of the stored captures, read-only)
# Host rules (CLAUDE.md, A8 brief): never while a receiver runs or the heavy lock is held; shared td_measure lock; nice 10; SIGINT only.
# NOT RUN in A8 (operator stop 2026-10-26): this script is the procedure for the orchestrator.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration
name=${1:-two_cell}
B=${B:-$ROOT/cmake_targets/ran_build/build}; CFG=${CFG:-$O/ota/sd_t26ul.cfg}; ENVFILE=${ENVFILE:-$O/ota/FROZEN_20261018.env}
SPEED=${SPEED:-0.25}; DUR=${DUR:-240}
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
nice -n 10 "$B/mpci_file_pub" "$SPEED" "$((DUR + 60))" "$(src nrcell_pci415 "$CAP415")" "$(src nrcell_pci403 "$CAP403")" > "$R/pub.log" 2>&1 &
PUB=$!
trap 'kill -INT $PUB 2>/dev/null || true' EXIT
for i in $(seq 1 100); do grep -q "ring=/nrcell_pci403" "$R/pub.log" 2>/dev/null && break; sleep 0.2; done
cd "$R"
set +e
env ISAC_MLOCK=0 LD_LIBRARY_PATH="$B:/usr/local/lib" ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 "${base[@]}" \
  timeout --signal=INT -k 60 "$DUR" nice -n 10 "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r 217 --numerology 1 --band 78 \
  -C 3680010000 --ssb 1171 --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo -4827 --ue-fo-compensation \
  --usrp-args nrcell_pci415 --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/run.log" 2>&1
rc=$?
kill -INT $PUB 2>/dev/null; wait $PUB 2>/dev/null
echo "rc=$rc run=$R"
# STOP evidence (A8): both cells sync, decode MIB/SIB1 and DCIs, lines tagged [c0]/[c1]
for c in 0 1; do
  echo "== cell $c: sync $(grep -c "^\[c$c\].*UE synchronized" run.log) MIB $(grep -c "^\[c$c\].*MIB" run.log) SIB1 $(grep -c "^\[c$c\].*SIB1" run.log) DCI $(grep -c "^\[c$c\].*DCI" run.log)"
done
grep -E "MPCI_(ADMIT|CELL|CELL_SLOT|LIFE|RT|INPUT)|CORE_PLAN|SHMIQ_READY" run.log | head -20
