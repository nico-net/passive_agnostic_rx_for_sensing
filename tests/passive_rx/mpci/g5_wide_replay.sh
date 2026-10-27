#!/bin/bash
# g5_wide_replay.sh: MPCI G5 in-context bench -- ONE nr-uesoftmodem process, TWO cells decoding (Vodafone PCI 415 + TIM PCI 923), file input, no radio.
#   arm gpu|cpu: the receiver's WIDEBAND path (G6: "radio" -> wideband ring -> in-process channelizer -> cell rings), the "radio" being the shm_iq device
#               reading a POSIX ring that tests/passive_rx/mpci/mpci_file_pub fills, at real time, from the synthetic 245.76 MS/s x 4 capture made by
#               g5_make_wideband.py from the two single-cell day captures (centre 3720 MHz: the plan of the 2026-10-27 OTA, pci415:d2:-39.990,
#               pci923:d2:+39.990). The configured device is named "shm" with loader shlibversion "_iq" (= libshm_iq.so): any device name but
#               "shm_iq" is a radio for iq_feed_input_mode, so the receiver takes the wideband path; configuration of the test bed only.
#               gpu = automatic CHAN path (GPU on this SHARED tier); cpu = the same with NR_GPU_PATH_CHAN=cpu (the per-path override of nr_gpu_path).
#   arm ctl:    the pre-channelized control (A8 rings, plan B.6 criterion): the two single-cell captures published as the cell rings nrcell_pci415 /
#               nrcell_pci923, configured device shm_iq; no channelizer in the receiver.
# Same cfg, env, cells.json (the 415 + 923 entries of tests/passive_rx/mpci/inventory/live_tuned_20261023.json), command line and speed in every arm.
# usage: g5_wide_replay.sh <gpu|cpu|ctl> [name]     env: B=<build dir> DUR=600 SPEED=1 WIDE=<synthetic capture dir> CAP415=.. CAP923=..
# Host rules: our receivers only (pgrep -u), exclusive td_measure lock, nice 10, SIGINT stop. gNB data is never an input.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration
arm=${1:?arm gpu|cpu|ctl}; name=${2:-g5_$arm}
B=${B:-$ROOT/cmake_targets/ran_build/build}; CFG=${CFG:-$O/ota/sd_t26ul.cfg}; ENVFILE=${ENVFILE:-$O/ota/MPCI_20261025.env}
SPEED=${SPEED:-1}; DUR=${DUR:-600}
WIDE=${WIDE:-/home/nicola/NICOLA/ota_runs/g5_wide_415_923_day_4s}
CAP415=${CAP415:-/home/nicola/NICOLA/ota_runs/raw_pci415_day_4s_1024_1001}; CAP923=${CAP923:-/home/nicola/NICOLA/ota_runs/raw_pci923_day_4s_1024_1001}
if pgrep -u "$(id -un)" -x nr-uesoftmodem >/dev/null; then echo "a receiver of ours is running: not starting" >&2; exit 3; fi
src() { # <ring> <capture dir> -> publisher source argument (geometry from the capture's own capture_info.json)
  python3 -I -c "import json,sys;d=json.load(open(sys.argv[2]+'/capture_info.json'));print('%s=%s,%d,%d,%d,%d,%d,2'%(sys.argv[1],sys.argv[2],d['samples_per_channel'],d['first_sample_tick'],d['channels'],d['sample_rate_hz'],d['rf_channels'][0]['frequency_hz']))" "$1" "$2"
}
R=$ROOT/replays/${name}_$(date +%H%M%S); mkdir -p "$R"; echo "$R" > "$ROOT/replays/last"
python3 -I - "$ROOT/tests/passive_rx/mpci/inventory/live_tuned_20261023.json" "$R/cells.json" <<'PY'
import json, os, sys
d = json.load(open(sys.argv[1])); keep = {'pci415', 'pci923'}
d['cells'] = [c for c in d['cells'] if c['id'] in keep]
d.pop('_comment', None)
tmp = sys.argv[2] + '.tmp'; json.dump(d, open(tmp, 'w'), indent=1); os.rename(tmp, sys.argv[2])
PY
case $arm in
  gpu|cpu) DEV='device = { name = "shm"; };'; LOADER="loader = { shm = { shlibpath = \"$B\"; shlibversion = \"_iq\"; }; shm_iq = { shlibpath = \"$B\"; }; };"
           RING=g5wide; PUBSRC=("$(src g5wide "$WIDE")") ;;
  ctl)     DEV='device = { name = "shm_iq"; };'; LOADER="loader = { shm_iq = { shlibpath = \"$B\"; }; };"
           RING=nrcell_pci415; PUBSRC=("$(src nrcell_pci415 "$CAP415")" "$(src nrcell_pci923 "$CAP923")") ;;
  *) echo "arm gpu|cpu|ctl" >&2; exit 2 ;;
esac
{ cat "$CFG"; echo "$DEV"; echo "$LOADER"; } > "$R/replay.cfg"
read -r -a envs < "$ENVFILE"; base=()
for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_*_CORES=*|ISAC_*_CORE=*) ;; *) base+=("$kv") ;; esac; done
[ "$arm" = cpu ] && base+=("NR_GPU_PATH_CHAN=cpu")
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
exec 9>/tmp/td_measure.lock; flock -x 9
nice -n 10 "$B/mpci_file_pub" "$SPEED" "$((DUR + 90))" "${PUBSRC[@]}" > "$R/pub.log" 2>&1 &
PUB=$!
trap 'kill -INT $PUB 2>/dev/null || true' EXIT
for i in $(seq 1 150); do n=$(grep -c "^MPCI_PUB ring=" "$R/pub.log" 2>/dev/null || true); [ "${n:-0}" -ge "${#PUBSRC[@]}" ] && break; sleep 0.2; done
cd "$R"
set +e
( while sleep 10; do nvidia-smi --query-gpu=utilization.gpu,clocks.sm,power.draw --format=csv,noheader; done ) > "$R/gpu.tsv" 2>/dev/null &
SMI=$!
env ISAC_MLOCK=0 LD_LIBRARY_PATH="$B:/usr/local/lib" "${base[@]}" \
  timeout --signal=INT -k 60 "$DUR" nice -n 10 "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r 217 --numerology 1 --band 78 \
  -C 3680010000 --ssb 1171 --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo -4827 --ue-fo-compensation \
  --usrp-args "$RING" --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/rx.log" 2>&1
rc=$?
kill $SMI 2>/dev/null
kill -INT $PUB 2>/dev/null; wait $PUB 2>/dev/null
echo "rc=$rc run=$R"
grep -E "MPCI_(ADMIT|INPUT|SPAN|FEED started)|IQ_CHAN_GPU|GPU_PATH path=chan|HOST_MEM wideband" rx.log | head -12
grep "MPCI_FEED rate" rx.log | tail -1
python3 -I "$ROOT/tests/passive_rx/mpci/mpci_runsum.py" "$R/rx.log" || true
