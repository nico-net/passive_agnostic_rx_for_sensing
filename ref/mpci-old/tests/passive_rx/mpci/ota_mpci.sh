#!/bin/bash
# OTA multi-cell run on the X410 through the shm IQ fan-out: ONE radio claim (shm_iq_uhd_pub, root), optional GPU channelizer
# (shm_iq_chan, user), one nr-uesoftmodem per cell reading the ring (shm_iq device, user, no radio access).
# Caller holds heavy.lock (flock -x ... with HEAVY_HELD=1) and makes sure no radio receiver runs. SIGINT only, never SIGKILL.
#   usage: OUT=<run dir> FREQ=<ring centre Hz> [RATE=122880000] [DUR=150] [GAIN=38] [ENVFILE=ota/t26f.env] [CFG=ota/sd_t26ul.cfg]
#          [BIN=<build dir>] [CHAN_CELLS="TAG:C:RATE:BW ..."] ota_mpci.sh CELLSPEC...
#   CELLSPEC = TAG:C:R:SSB:FO:MASK:PDSCH:SCAN:DMRS:UL[:K=V,K=V]   (core lists with '+', e.g. PDSCH 7+8)
#   A cell whose TAG is listed in CHAN_CELLS reads <ring>_<TAG> (channelizer output, decim 1); the others read the wideband ring.
# Abort rules (logged to abort.txt, everything SIGINTed): publisher not READY in 60 s; SHMIQ_UHD_FAIL; > 3 SHMIQ_UHD_RESTART;
# MemAvailable < 16 GB; channelizer overruns > 20. Per-cell SHMIQ_OVERRUN is reported, not an abort.
set -u
WT=$(cd "$(dirname "$0")/../../.." && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration
BIN=${BIN:-$WT/cmake_targets/ran_build/build_t26}; OUT=${OUT:?}; FREQ=${FREQ:?}; RATE=${RATE:-122880000}; DUR=${DUR:-150}; GAIN=${GAIN:-38}
ENVFILE=${ENVFILE:-$O/ota/t26f.env}; CFG=${CFG:-$O/ota/sd_t26ul.cfg}; RING=${RING:-isac_iq}
UHD_ARGS=${UHD_ARGS:-type=x4xx,addr=192.168.10.2,mgmt_addr=192.168.30.2,use_dpdk=1,dpdk_num_mbufs=65536}
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps; mkdir -p /tmp/nicola_nomps
mkdir -p "$OUT"; cd "$OUT"; : > abort.txt
uhd_find_devices --args mgmt_addr=192.168.30.2 2>/dev/null | grep -q 'claimed: False' || { echo "X410 claimed or absent" | tee -a abort.txt; exit 3; }
sudo rm -f /dev/hugepages/rtemap_* 2>/dev/null
read -r -a envs < "$ENVFILE"
base=(ISAC_MLOCK=0)
for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*|ISAC_PUSCH_CORES=*|ISAC_DMRS_SCAN_CORE=*|ISAC_UL_SNAP_CORE=*|ISAC_MLOCK=*) ;; *) base+=("$kv");; esac; done
base+=(LD_LIBRARY_PATH=$BIN:/usr/local/lib)
echo "$(date -Is) FREQ=$FREQ RATE=$RATE DUR=$DUR CHAN_CELLS=${CHAN_CELLS:-} cells: $*" > cmd.txt
# 1. publisher (root: DPDK). rx_shift 2 = usrp_lib X4x0 level.
sudo env ISAC_UHD_CTRL_CORES=15 $BIN/shm_iq_uhd_pub --args "$UHD_ARGS" --rate $RATE --freq $FREQ --gain $GAIN --channels 4 --antenna RX1 \
  --ring $RING ${CAPACITY:+--capacity $CAPACITY} --duration $((DUR + 120)) ${PUB_EXTRA:-} > pub.log 2>&1 &
PUBPID=""; for k in $(seq 1 60); do PUBPID=$(pgrep -x shm_iq_uhd_pub | head -1); grep -q SHMIQ_UHD_READY pub.log 2>/dev/null && break; grep -q SHMIQ_UHD_FAIL pub.log 2>/dev/null && break; sleep 1; done
stop_all() { for p in $(cat cell.pids 2>/dev/null); do kill -INT $p 2>/dev/null; done; [ -n "${CHPID:-}" ] && kill -INT $CHPID 2>/dev/null
  sleep 5; [ -n "$PUBPID" ] && sudo kill -INT $PUBPID 2>/dev/null
  for i in $(seq 1 90); do pgrep -x shm_iq_uhd_pub > /dev/null || break; sleep 1; done
  pgrep -x shm_iq_uhd_pub > /dev/null && echo "WARNING publisher still alive after SIGINT + 90 s (no SIGKILL)" | tee -a abort.txt; }
if ! grep -q SHMIQ_UHD_READY pub.log; then echo "publisher not READY" | tee -a abort.txt; stop_all; exit 4; fi
# 2. optional GPU channelizer (user)
CHPID=""
if [ -n "${CHAN_CELLS:-}" ]; then
  args=""; for c in $CHAN_CELLS; do args+=" --cell $c"; done
  taskset -c 10 $BIN/shm_iq_chan --ring $RING $args --gpu ${CHANBLK:+--block $CHANBLK} --stat-s 5 --attach-s 60 > chan.log 2>&1 & CHPID=$!
  for c in $CHAN_CELLS; do t=${c%%:*}; for k in $(seq 1 60); do [ -e /dev/shm/${RING}_$t ] && break; sleep 0.5; done; done
fi
# 3. cells (user)
: > cell.pids
for spec in "$@"; do
  IFS=: read -r T C R SSB FO MASK PD SC DM UL X <<< "$spec"
  ring=$RING; [[ " ${CHAN_CELLS:-} " == *" $T:"* ]] && ring=${RING}_$T
  { sed "s|pdcch_blind_monitor_scan_thread = \"2:32:6\"|pdcch_blind_monitor_scan_thread = \"2:32:$SC\"|" "$CFG"
    printf 'device = { name = "shm_iq"; };\nloader = { shm_iq = { shlibpath = "%s"; }; };\n' "$BIN"; } > cell_$T.cfg
  xv=${X:-}; xv=${xv//,/ }; xv=${xv//+/,}
  mkdir -p cell_$T
  ( cd cell_$T && exec env "${base[@]}" ISAC_CELL_INSTANCE=$T ISAC_SHM_IQ_CELL=$T ISAC_SHM_IQ_NAME=$ring ISAC_SHM_IQ_ATTACH_S=60 \
      ISAC_PDSCH_CORES=${PD//+/,} ISAC_DMRS_SCAN_CORE=$DM ISAC_PUSCH_CORES=$UL ISAC_UL_SNAP_CORE=$UL $xv \
      taskset -c ${MASK//+/,} $BIN/nr-uesoftmodem -O ../cell_$T.cfg -C $C -r $R --ssb $SSB --initial-fo $FO --passive-rx --numerology 1 --band 78 \
      --ue-rxgain $GAIN --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --ue-fo-compensation --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 ) > ue_$T.log 2>&1 &
  echo $! >> cell.pids
done
# 4. watch
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt $DUR ]; do
  ma=$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)
  echo "$(date +%s) memavail_gb=$ma $(for p in $(cat cell.pids); do awk '/VmRSS/{r=$2}/RssAnon/{a=$2}END{printf "%d/%dMB ", r/1024, a/1024}' /proc/$p/status 2>/dev/null; done)" >> mem.tsv
  nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader 2>/dev/null | tr '\n' ' ' >> gpu.tsv; echo >> gpu.tsv
  [ "$ma" -lt 16 ] && { echo "MemAvailable $ma GB < 16" | tee -a abort.txt; break; }
  grep -q SHMIQ_UHD_FAIL pub.log && { echo "publisher FAIL" | tee -a abort.txt; break; }
  [ $(grep -c SHMIQ_UHD_RESTART pub.log) -gt 3 ] && { echo "publisher restarted > 3 times" | tee -a abort.txt; break; }
  [ -n "$CHPID" ] && [ "$(grep -o 'overruns=[0-9]*' chan.log | tail -1 | cut -d= -f2)" -gt 20 ] 2>/dev/null && { echo "channelizer overruns > 20" | tee -a abort.txt; break; }
  sleep 2
done
stop_all
for p in $(cat cell.pids); do for i in $(seq 1 60); do kill -0 $p 2>/dev/null || break; sleep 1; done; done
uhd_find_devices --args mgmt_addr=192.168.30.2 2>/dev/null | grep -q 'claimed: False' && echo "X410 released" >> abort.txt || echo "X410 NOT released yet" >> abort.txt
echo "done $OUT"
