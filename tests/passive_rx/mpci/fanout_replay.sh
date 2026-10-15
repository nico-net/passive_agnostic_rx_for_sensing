#!/bin/bash
# MPCI offline validation on a recorded capture (default: newcells_tiled/l0942_c3610x8 = WindTre PCI 508 on a 217-PRB grid + Iliad
# PCI 403 on a 51-PRB grid, one 122.88 MS/s window centred at 3610 MHz). Run it INSIDE heavy.sh (exclusive heavy.lock held).
#   MODE=solo CELL=A|B BIN=<build>                      single process, raw_iq replay (baseline)
#   MODE=fan  BIN=<build> [CELLS="A B"] [SCHED=1] [CHAN=cpu|gpu]
#                                                       shm_iq_file_pub -> [shm_iq_chan ->] one nr-uesoftmodem per cell (shm_iq device)
#   OUT=<dir> ENVFILE=<single-line OTA env> (default ota/t26f.env) SPEED (0.25) TMO (180 s)
# Core pins of the env are replaced per cell (identical in solo and fan arms, so a fan/solo difference is not a core-count one):
#   A: mask 2,8-12  PDSCH 8,9,12  scan 10,11  DM-RS scan 2  UL 0      B: mask 1,3,4,13,14  PDSCH 13,14  scan 3,4  DM-RS scan 1  UL 1
#   publisher core 0; channelizer cores 0,1. Writes ue_<cell>.log, pub.log, chan.log, mem.tsv (MemAvailable, RSS/HWM, CPU s), gpu.tsv.
set -u
WT=$(cd "$(dirname "$0")/../../.." && pwd)
CAP=${CAP:-/home/nicola/NICOLA/ota_runs/newcells_tiled/l0942_c3610x8}
MODE=${MODE:?}; BIN=${BIN:?}; OUT=${OUT:?}; SPEED=${SPEED:-0.25}; TMO=${TMO:-180}
ENVFILE=${ENVFILE:-/home/nicola/NICOLA/wt/rr-orchestration/ota/t26f.env}
mkdir -p "$OUT"; cd "$OUT"
read -r -a envs < "$ENVFILE"
base=(ISAC_MLOCK=0 ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1)
for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_PDSCH_CORES=*|ISAC_GPU_WORKER_CORE=*|ISAC_PDCCH_NID_GPU_CORE=*|ISAC_PDCCH_USS_CORE=*|ISAC_PUSCH_CORES=*|ISAC_DMRS_SCAN_CORE=*|ISAC_UL_SNAP_CORE=*) ;; *) base+=("$kv");; esac; done
base+=(ISAC_MLOCK=0 LD_LIBRARY_PATH=$BIN:/usr/local/lib)
RAW=(ISAC_RAW_IQ_DIRECTORY=$CAP ISAC_RAW_IQ_SAMPLES=983040000 ISAC_RAW_IQ_FIRST_TICK=194443608 ISAC_RAW_IQ_CHANNELS=4 ISAC_RAW_IQ_RATE=122880000
     ISAC_RAW_IQ_CENTER=3610000000 ISAC_RAW_IQ_SPEED=$SPEED ISAC_RAW_IQ_SHIFT=2)
COMMON="--passive-rx --numerology 1 --band 78 --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --ue-fo-compensation --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90"
declare -A C=([A]=3590190000 [B]=3630000000) R=([A]=217 [B]=51) SSB=([A]=517 [B]=18) FO=([A]=-4842 [B]=2164) MASK=([A]=2,8-12 [B]=1,3,4,13,14) \
  PD=([A]=8,9,12 [B]=13,14) SC=([A]=10 [B]=3) DM=([A]=2 [B]=1) UL=([A]=0 [B]=1) PCI=([A]=508 [B]=403) DEC=([A]=1 [B]=4) BW=([A]=78120000 [B]=18360000)
mkcfg() { sed "s|pdcch_blind_monitor_scan_thread = \"2:32:10\"|pdcch_blind_monitor_scan_thread = \"2:32:${SC[$1]}\"|" $WT/tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg > cell_$1.cfg
  printf '%s\n' "$2" >> cell_$1.cfg; }
cellenv() { echo "ISAC_PDSCH_CORES=${PD[$1]} ISAC_DMRS_SCAN_CORE=${DM[$1]} ISAC_PUSCH_CORES=${UL[$1]} ISAC_UL_SNAP_CORE=${UL[$1]} ISAC_TARGET_PCI=${PCI[$1]}"; }
rxpid() { local c; c=$(pgrep -P $1 -x nr-uesoftmodem 2>/dev/null | head -1); echo ${c:-$1}; }
sampler() { while :; do
    ma=$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo); line="$(date +%s) memavail_gb=$ma"
    for p0 in $(cat pids 2>/dev/null); do p=$(rxpid $p0); [ -r /proc/$p/status ] || continue
      line+=" $p0:$(awk '/VmRSS/{r=$2}/VmHWM/{h=$2}/RssAnon/{a=$2}END{print int(r/1024)"/"int(h/1024)"/"int(a/1024)}' /proc/$p/status)MB:cpu=$(awk '{print ($14+$15)/100}' /proc/$p/stat 2>/dev/null)s"; done
    echo "$line" >> mem.tsv
    if [ "$ma" -lt 16 ]; then echo "MEMORY FLOOR: SIGINT all" >> mem.tsv; for p0 in $(cat pids); do kill -INT $(rxpid $p0) 2>/dev/null; done; fi
    nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader 2>/dev/null | tr '\n' ' ' >> gpu.tsv; echo >> gpu.tsv
    sleep 2; done; }
: > pids
if [ "$MODE" = solo ]; then
  T=${CELL:?}
  mkcfg $T "device = { name = \"raw_iq\"; };
loader = { raw_iq = { shlibpath = \"$BIN\"; }; };"
  extra=(); [ "${DEC[$T]}" != 1 ] && extra=(ISAC_RAW_IQ_DECIM=${DEC[$T]})
  ( exec env "${base[@]}" "${RAW[@]}" "${extra[@]}" $(cellenv $T) taskset -c ${MASK[$T]} timeout --signal=INT -k 60 $TMO $BIN/nr-uesoftmodem -O cell_$T.cfg \
      -C ${C[$T]} -r ${R[$T]} --ssb ${SSB[$T]} --initial-fo ${FO[$T]} $COMMON ) > ue_$T.log 2>&1 &
  echo $! > pids; sampler & SP=$!; wait $(cat pids); kill $SP 2>/dev/null; exit 0
fi
CELLS=${CELLS:-A B}; RING=mpr_iq_$$; GO=$OUT/go; rm -f $GO
taskset -c 0 $BIN/shm_iq_file_pub --dir $CAP --ring $RING --speed $SPEED --go-file $GO > pub.log 2>&1 &
PUBPID=$!; echo $PUBPID >> pids; sleep 1
if [ -n "${CHAN:-}" ]; then
  chanargs="--threads 2 --cores 0,1 --block 61440"; [ "$CHAN" = gpu ] && chanargs="--gpu --block 61440"
  cells=""; for T in $CELLS; do cells+=" --cell $T:${C[$T]}:$((122880000 / ${DEC[$T]})):${BW[$T]}"; done
  taskset -c 0,1 $BIN/shm_iq_chan --ring $RING $cells $chanargs --stat-s 5 > chan.log 2>&1 &
  echo $! >> pids
  for T in $CELLS; do until [ -e /dev/shm/${RING}_$T ]; do sleep 0.2; done; done
fi
for T in $CELLS; do
  ring=$RING; [ -n "${CHAN:-}" ] && ring=${RING}_$T
  mkcfg $T "device = { name = \"shm_iq\"; };
loader = { shm_iq = { shlibpath = \"$BIN\"; }; };"
  sched=(); [ "${SCHED:-0}" = 1 ] && sched=(ISAC_MPCI_SCHED=mpr_sched_$$ ISAC_MPCI_GPU_SHARE_PCT=100)
  mkdir -p cell_$T
  ( cd cell_$T && exec env "${base[@]}" ISAC_CELL_INSTANCE=mpr$T ISAC_SHM_IQ_CELL=mpr$T ISAC_SHM_IQ_NAME=$ring ISAC_SHM_IQ_ATTACH_S=120 "${sched[@]}" $(cellenv $T) \
      taskset -c ${MASK[$T]} timeout --signal=INT -k 60 $TMO $BIN/nr-uesoftmodem -O ../cell_$T.cfg -C ${C[$T]} -r ${R[$T]} --ssb ${SSB[$T]} --initial-fo ${FO[$T]} $COMMON ) > ue_$T.log 2>&1 &
  echo $! >> pids
done
sampler & SP=$!
[ "${SCHED:-0}" = 1 ] && { ( while :; do $BIN/mpci_sched_stat mpr_sched_$$ 1 2>&1 | head -5; sleep 4; done ) > sched.log 2>&1 & STP=$!; }
# go once every consumer reads (SHMIQ_START). With the channelizer a cell cannot finish device_init before data flows: fixed 20 s.
t0=$SECONDS
allstart() { for T in $CELLS; do grep -q SHMIQ_START ue_$T.log || return 1; done; }
if [ -n "${CHAN:-}" ]; then sleep 20; else until allstart; do sleep 0.5; [ $((SECONDS-t0)) -gt 120 ] && break; done; fi
touch $GO; echo "go after $((SECONDS-t0)) s" > go.txt
wait $PUBPID
for p in $(tail -n +2 pids); do wait $p 2>/dev/null; done
kill $SP ${STP:-} 2>/dev/null; rm -f /dev/shm/mpr_sched_$$
exit 0
