#!/bin/bash
# wide.sh <name> <build dir> <cells: 2|415|923> <duration s> [speed] : wideband replay of the 2026-11-02 11:47 capture (245.76 MS/s, 3720 MHz, 4 ch)
# through the receiver's in-process channelizer, OTA env (MPCI_20261025.env + ISAC_SIB1_CACHE=0 + starttest_auto extras), OTA sensing cfg. Output $OUTD/<name>_<time>/rx.log
# Run under: HEAVY_CPUS=0-19 heavy.sh flock -x -w 7200 /tmp/td_measure.lock wide.sh ...   (no lock taken here)
set -uo pipefail
name=$1; B=$2; MODE=$3; DUR=$4; SPEED=${5:-1}
O=/home/nicola/NICOLA/wt/rr-orchestration; ROOT=${ROOT:-/home/nicola/NICOLA/wt/rr-candgpuA}
WIDE=/home/nicola/NICOLA/ota_runs/raw_v415t923_245_2s_1102_1147
CFG=/home/nicola/NICOLA/wt/rr-int-1031/tests/passive_rx/dgx/ue.passive.ota.sense.dgx.cfg
ENVFILE=$O/ota/MPCI_20261025.env; OUTD=${OUTD:?}
C=3680010000; SSB=1171; FO=-4827; [ "$MODE" = 923 ] && { C=3759990000; SSB=233; FO=-4832; }
pgrep -x nr-uesoftmodem >/dev/null && { echo "receiver running"; exit 3; }
R=$OUTD/${name}_$(date +%H%M%S); mkdir -p $R
python3 -I - $O/ota/mpci_cells/v415_t923.json $R/cells.json $MODE <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); m=sys.argv[3]
if m!='2':  # solo: the wanted cell + a dummy cell outside the 3720 MHz span (unserved, paused ring): a 1-cell inventory would take the direct (non-channelizer) path
    keep=[c for c in d['cells'] if c['pci']==int(m)]
    dummy=dict(d['cells'][0]); dummy.update(id='dummy', centre_hz=3900000000, ssb_hz=3880000000, pci=999, gscn=0)
    d['cells']=keep+[dummy]
d.pop('_comment',None); json.dump(d,open(sys.argv[2],'w'),indent=1)
PY
if [ "${CTL:-0}" = 1 ]; then
  s1=$(python3 -I -c "import json,sys;d=json.load(open(sys.argv[2]+'/capture_info.json'));print('%s=%s,%d,%d,%d,%d,%d,2'%(sys.argv[1],sys.argv[2],d['samples_per_channel'],d['first_sample_tick'],d['channels'],d['sample_rate_hz'],d['rf_channels'][0]['frequency_hz']))" nrcell_pci415 /home/nicola/NICOLA/ota_runs/raw_pci415_4s_1102_0951)
  s2=$(python3 -I -c "import json,sys;d=json.load(open(sys.argv[2]+'/capture_info.json'));print('%s=%s,%d,%d,%d,%d,%d,2'%(sys.argv[1],sys.argv[2],d['samples_per_channel'],d['first_sample_tick'],d['channels'],d['sample_rate_hz'],d['rf_channels'][0]['frequency_hz']))" nrcell_pci923 /home/nicola/NICOLA/ota_runs/raw_pci923_4s_1102_0951)
  PUBARGS=("$s1" "$s2"); DEVL='device = { name = "shm_iq"; };'; LDR="loader = { shm_iq = { shlibpath = \"$B\"; }; };"; RING=nrcell_pci415; NPUB=2
else
src=$(python3 -I -c "import json,sys;d=json.load(open(sys.argv[1]+'/capture_info.json'));print('g5wide=%s,%d,%d,%d,%d,%d,2'%(sys.argv[1],d['samples_per_channel'],d['first_sample_tick'],d['channels'],d['sample_rate_hz'],d['rf_channels'][0]['frequency_hz']))" $WIDE)
PUBARGS=("$src"); DEVL='device = { name = "shm"; };'; LDR="loader = { shm = { shlibpath = \"$B\"; shlibversion = \"_iq\"; }; shm_iq = { shlibpath = \"$B\"; }; };"; RING=g5wide; NPUB=1
fi
{ sed "s#^\( *records_path *= *\).*#\1\"$R/sense_records.jsonl\";#" $CFG; echo "$DEVL"; echo "$LDR"; } > $R/replay.cfg
read -r -a envs < <(head -1 $ENVFILE); base=()
for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*) ;; *) base+=("$kv") ;; esac; done
base+=(ISAC_SIB1_CACHE=0 ${EXTRA_ENV:-})
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
unset ISAC_RX_BRANCH_FO
echo "name=$name build=$B mode=$MODE dur=$DUR speed=$SPEED" > $R/manifest.txt
nice -n 10 $B/g5_fast_pub $SPEED $((DUR+90)) "${PUBARGS[@]}" > $R/pub.log 2>&1 &
PUB=$!
for i in $(seq 1 300); do [ $(grep -c "^MPCI_PUB ring=" $R/pub.log 2>/dev/null) -ge $NPUB ] && break; sleep 0.2; done
cd $R
[ -n "${SAMPLE:-}" ] && { $(dirname $0)/thr.sh $R/thr & }
( while sleep 2; do nvidia-smi --query-gpu=utilization.gpu,clocks.sm,power.draw --format=csv,noheader; done ) > $R/gpu.tsv 2>/dev/null & SMI=$!
env ISAC_MLOCK=0 LD_LIBRARY_PATH="$B:/usr/local/lib" "${base[@]}" \
  timeout --signal=INT -k 60 $DUR nice -n 10 $B/nr-uesoftmodem -O $R/replay.cfg --passive-rx -r 217 --numerology 1 --band 78 \
  -C $C --ssb $SSB --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo $FO --ue-fo-compensation \
  --usrp-args $RING --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > $R/rx.log 2>&1
rc=$?; kill $SMI 2>/dev/null; kill -INT $PUB 2>/dev/null; wait $PUB 2>/dev/null
echo "rc=$rc run=$R"
