#!/bin/bash
# replay1.sh <name> <build_dir> <cell id> <capture dir> <speed> <dur_s> <cfg> : one single-cell shm_iq ring replay (same recipe as the 10-31 rt/replay415.sh),
# grid from the inventory (centre, n_rb, cfo) and the SSB subcarrier derived from ssb_hz. Output: $OUTD/<name>_<time>/rx.log
set -uo pipefail
name=$1; B=$2; CELL=$3; CAP=$4; SPEED=$5; DUR=$6; CFG=$7
INV=/home/nicola/NICOLA/wt/rr-int-1031/tests/passive_rx/mpci/inventory/live_tuned_20261023.json
O=/home/nicola/NICOLA/wt/rr-orchestration; ENVFILE=$O/ota/MPCI_20261025.env
OUTD=${OUTD:?}; R=$OUTD/${name}_$(date +%H%M%S); mkdir -p $R
while pgrep -x nr-uesoftmodem >/dev/null; do sleep 20; done
read -r C RB FO SSB < <(python3 -I -c "
import json,sys
c=[c for c in json.load(open(sys.argv[1]))['cells'] if c['id']==sys.argv[2]][0]
print(c['centre_hz'], c['n_rb_hint'], c['cfo_hz'], round((c['ssb_hz']-c['centre_hz'])/30e3 + c['n_rb_hint']*6 - 120))" $INV $CELL)
src=$(python3 -I -c "import json,sys;d=json.load(open(sys.argv[2]+'/capture_info.json'));print('%s=%s,%d,%d,%d,%d,%d,2'%(sys.argv[1],sys.argv[2],d['samples_per_channel'],d['first_sample_tick'],d['channels'],d['sample_rate_hz'],d['rf_channels'][0]['frequency_hz']))" nrcell_$CELL "$CAP")
python3 -I - $INV $R/cells.json $CELL <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); d['cells']=[c for c in d['cells'] if c['id']==sys.argv[3]]; d.pop('_comment',None); json.dump(d,open(sys.argv[2],'w'),indent=1)
PY
{ sed "s#^\( *records_path *= *\).*#\1\"$R/sense_records.jsonl\";#" $CFG; echo 'device = { name = "shm_iq"; };'; echo "loader = { shm_iq = { shlibpath = \"$B\"; }; };"; } > $R/replay.cfg
read -r -a envs < <(head -1 $ENVFILE); base=()
for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_*_CORES=*|ISAC_*_CORE=*) ;; *) base+=("$kv") ;; esac; done
base+=(ISAC_SIB1_CACHE=0)
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
unset ISAC_RX_BRANCH_FO
echo "name=$name build=$B cell=$CELL cap=$CAP speed=$SPEED dur=$DUR cfg=$CFG C=$C RB=$RB FO=$FO SSB=$SSB build_id=${BUILD_ID:-?}" > $R/manifest.txt
nice -n 10 $B/g5_fast_pub $SPEED $((DUR+60)) "$src" > $R/pub.log 2>&1 &
PUB=$!
for i in $(seq 1 150); do grep -q "^MPCI_PUB ring=" $R/pub.log 2>/dev/null && break; sleep 0.2; done
cd $R
env ISAC_MLOCK=0 LD_LIBRARY_PATH="$B:/usr/local/lib" "${base[@]}" \
  timeout --signal=INT -k 60 $DUR nice -n 10 ${WRAP:-} $B/nr-uesoftmodem -O $R/replay.cfg --passive-rx -r $RB --numerology 1 --band 78 \
  -C $C --ssb $SSB --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo $FO --ue-fo-compensation \
  --usrp-args nrcell_$CELL --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 \
  < /dev/null > $R/rx.log 2>&1
rc=$?; kill -INT $PUB 2>/dev/null; wait $PUB 2>/dev/null
echo "rc=$rc run=$R"
