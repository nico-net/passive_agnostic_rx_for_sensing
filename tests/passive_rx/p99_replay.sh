#!/bin/bash
# P99 "air-like" replay: the closest offline emulation of an OTA run that this host allows.
# Differences from timacq_replay.sh (which runs every thread unpinned, SCHED_OTHER, nice 19, without the UL monitor):
#  - the OTA cfg (default rr-orchestration/ota/sd_t26ul.cfg: UL-UCI monitor on, scan pool "2:32:6") + the raw_iq device;
#  - the OTA env file kept whole except the radio-only keys (ISAC_USRP_*, ISAC_UHD_*, ISAC_RX_RECOVER*), so the OTA core
#    map stays (PDSCH 7,8,9,3,4, PUSCH 6, scan 6,7, nID 13, USS 12, GPU worker 10, ...), plus the core keys that
#    starttest.sh prepends on air (CSIRS_DISC_CORE 11, DETECT 12-14, UE_RT 5) and --thread-pool 8,9,15;
#  - run through sudo so threadCreate() gets SYS_NICE: pinned SCHED_FIFO threads at the OTA priorities (scan 50,
#    PDSCH 40, PUSCH 30), as on air. Real-time pacing (SPEED=1) by raw_iq.
# Not emulated: DPDK/USRP producer load, the agents_freeze of the OTA window (other agents may run), live traffic beyond
# the 12 s capture (the replay ends at EOF), radio impairments.
# usage: [CAPSET=b|tim] [ENVFILE=...] [CFG=...] [B=build dir] [TMO=s] p99_replay.sh <name> [ENV=VAL ...]
# Stats: tests/passive_rx/p99_stats.py <dir>/run.log. Takes heavy.lock (heavy.sh, HEAVY_CPUS=0-16); exits 99 without running
# when another receiver is up. Stops with SIGINT (timeout --signal=INT).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
O=/home/nicola/NICOLA/wt/rr-orchestration
B=${B:-$ROOT/cmake_targets/ran_build/build_t26}
case ${CAPSET:-b} in
  b) CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b; CAP_SAMPLES=1474560000; CAP_TICK=208029025
     CAPC=3680010000; RXC=3680010000; SSB=1171; FO=6900 ;;
  tim) CAP=/home/nicola/NICOLA/ota_runs/raw_pci923_tim_12s_tile3; CAP_SAMPLES=1474560000; CAP_TICK=195322029
     CAPC=3731910000; RXC=3759990000; SSB=233; FO=-4800 ;;
  *) echo "CAPSET b|tim"; exit 2 ;;
esac
read -r -a envs < "${ENVFILE:-$O/ota/t26bKn.env}"
base=()
for kv in "${envs[@]}"; do
  case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*) ;; *) base+=("$kv") ;; esac
done
name=$1; shift
R=$ROOT/replays/replay_${name}_$(date +%H%M%S); mkdir -p "$R"; echo "$R" > "$ROOT/replays/last"; echo "replay dir $R"
{ cat "${CFG:-$O/ota/sd_t26ul.cfg}"; echo 'device = { name = "raw_iq"; };'
  echo "loader = { raw_iq = { shlibpath = \"$B\"; }; };"; } > "$R/replay.cfg"
cd "$R"
echo "CAPSET=${CAPSET:-b} CAP=$CAP ENVFILE=${ENVFILE:-$O/ota/t26bKn.env} CFG=${CFG:-$O/ota/sd_t26ul.cfg} extra: $*" > args.txt
# inside heavy.lock: refuse (exit 99, file rx_busy) if any other receiver runs, so timing is never taken next to one
HEAVY_CPUS=${HEAVY_CPUS:-0-16} exec "$O/heavy.sh" bash -c 'pgrep -x nr-uesoftmodem >/dev/null && { echo busy > "$0/rx_busy"; exit 99; }
  exec "$@"' "$R" sudo -n timeout --signal=INT -k 120 ${TMO:-90} env \
  ISAC_MLOCK=0 ISAC_RAW_IQ_DIRECTORY="$CAP" ISAC_RAW_IQ_SAMPLES="$CAP_SAMPLES" ISAC_RAW_IQ_FIRST_TICK="$CAP_TICK" \
  ISAC_RAW_IQ_CHANNELS=4 ISAC_RAW_IQ_RATE=122880000 ISAC_RAW_IQ_CENTER="$CAPC" ISAC_RAW_IQ_SPEED=1 ISAC_RAW_IQ_SHIFT=2 \
  LD_LIBRARY_PATH="$B:/usr/local/lib" ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CORE=11 ISAC_CSIRS_DISC_DETECT_CORES=12,13,14 \
  ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_UE_RT_CORE=5 ISAC_PDCCH_USS_CORE=7 "${base[@]}" ISAC_MLOCK=0 "$@" \
  "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r 217 --numerology 1 --band 78 \
  -C "$RXC" --ssb "$SSB" --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo "$FO" --ue-fo-compensation \
  --thread-pool 8,9,15 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/run.log" 2>&1
