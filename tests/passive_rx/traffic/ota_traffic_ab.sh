#!/bin/bash
# ota_traffic_ab.sh -- OTA A/B of legacy (L) vs traffic mode (T), 4 RX, same BUILD for both arms (legacy = no mode key).
# Cases: single 415, single 923, pair 415+923; L/T alternating x3 per case; DUR s each. Modelled on rr-orchestration/ota/run_ota_20261102.sh.
# SMOKE=1: one traffic run on single 415 only (tag tmT0S415). Night runs are [OTA LOW LOAD] functional only; repeat after 08:30.
set -u
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
unset ISAC_RX_BRANCH_FO
O=/home/nicola/NICOLA/wt/rr-orchestration; D=$O/ota
BUILD=${BUILD:-/home/nicola/NICOLA/wt/rr-twomodes/cmake_targets/ran_build/build}
CD=/home/nicola/NICOLA/wt/rr-twomodes/tests/passive_rx/traffic
ENVF=$D/MPCI_20261025.env; DUR=${DUR:-240}; SETTLE=${SETTLE:-45}; SMOKE=${SMOKE:-0}
V=3680010000:1171:-4827:217; T=3759990000:233:-4832:217
JB=$D/mpci_cells/v415_t923.json
CAMP=/home/nicola/NICOLA/ota_runs/tm_ota_$(date +%Y%m%d_%H%M%S); mkdir -p $CAMP/cpu
say() { echo "$(date +%H:%M:%S) $*" | tee -a $CAMP/campaign.log; }
[ -e $O/X410_UNAVAILABLE ] && { say "X410_UNAVAILABLE present"; exit 1; }
[ "$(ls $BUILD/*.so | wc -l)" = 24 ] && [ -e $BUILD/liboai_device.so ] || { say "build check failed"; exit 1; }
exec 9>$O/locks/heavy.lock; flock -x -w 3600 9 || exit 1; export HEAVY_HELD=1; exec 8>/tmp/td_measure.lock; flock -x -w 3600 8 || exit 1
say "build=$BUILD $(git -C $BUILD/../../.. log --oneline -1) DUR=$DUR SMOKE=$SMOKE"
# read-only CPU sidecar: utime+stime ticks of the radio receiver every 5 s
( while [ ! -e $CAMP/cpu/STOP ]; do for p in $(pgrep -x nr-uesoftmodem); do tr "\0" " " < /proc/$p/cmdline 2>/dev/null | grep -q usrp-args || continue
    read -r -a s < <(sed 's/^[0-9]* ([^)]*) //' /proc/$p/stat 2>/dev/null) && echo "$(date +%s) $((${s[11]}+${s[12]}))" >> $CAMP/cpu/$p.tsv; done; sleep 5; done ) &
SAMP=$!; trap "touch $CAMP/cpu/STOP; wait $SAMP" EXIT
pcis_ok() { local r=$1 want=$2 log k=0 w bad=0; log=$(sed 's/\x1b\[[0-9;]*m//g' $r/run.log)
  if [ "$(echo $want | wc -w)" = 1 ]; then [ "$(echo "$log" | grep -o 'Initial sync successful, PCI: [0-9]*' | awk '{print $NF}' | sort -u | tr '\n' ' ')" = "$want " ]; return; fi
  for w in $want; do echo "$log" | grep -q "MPCI_SYNC cell=$k want=$w got=$w locked" || bad=1; k=$((k+1)); done; [ $bad = 0 ]; }
run() { # <tag> <cells.json|-> <tune> <pcis> <L|T>
  local tag=$1 cj=$2 f ssb fo rb prev r try t ok pre="" cfg pid; IFS=: read -r f ssb fo rb <<< "$3"; [ "$cj" != "-" ] && pre="$D/cellswrap.sh $cj"
  cfg=$CD/legacy_ota.dgx.cfg; [ $5 = T ] && cfg=$CD/traffic_ota.dgx.cfg
  for try in 1 2; do
    t=$tag; [ $try = 2 ] && t=${tag}r; say "-- $t cells=${cj##*/} tune=$3 want=[$4] cfg=${cfg##*/}"
    pgrep -x nr-uesoftmodem >/dev/null && { say "$t: another receiver running, waiting"; while pgrep -x nr-uesoftmodem >/dev/null; do sleep 30; done; }
    rm -f /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl; prev=$(cat $D/lastrun 2>/dev/null)
    OTA_PREFIX="$pre" BDIR=$BUILD FREQ=$f SSBOFF=$ssb IFO=$fo RB=$rb DUR=$DUR CFG=$cfg $D/starttest_auto.sh $t 1 65536 $(head -1 $ENVF) ISAC_SIB1_CACHE=0 | tee -a $CAMP/campaign.log
    r=$(cat $D/lastrun); [ "$r" = "$prev" ] && { say "$t NOT STARTED"; sleep 20; continue; }
    cp $D/last_ota_out.txt $r/ota_out.txt 2>/dev/null
    ls /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl >/dev/null 2>&1 && mv /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl $r/ 2>/dev/null
    pid=$(grep -o 'pid [0-9]*' $r/ota_out.txt | head -1 | awk '{print $2}')
    [ -n "$pid" ] && [ -s $CAMP/cpu/$pid.tsv ] && awk 'NR==1{t0=$1;c0=$2} {t1=$1;c1=$2} END{if(t1>t0) printf "%.3f\n",(c1-c0)/100/(t1-t0)}' $CAMP/cpu/$pid.tsv > $r/cpu_cores.txt
    pcis_ok $r "$4" && ok=OK || ok=MISMATCH
    printf '%s\t%s\t%s\t%s\n' "$t" "$r" "$4" "$ok" >> $CAMP/runs.tsv; say "$t -> $r ($(cat $r/rx_exit.txt 2>/dev/null)) pci=$ok cpu=$(cat $r/cpu_cores.txt 2>/dev/null)"
    sleep $SETTLE; [ $ok = OK ] && return
  done
}
if [ $SMOKE = 1 ]; then run tmT0S415 - $V "415" T; say "smoke done $CAMP"; exit 0; fi
for n in 1 2 3; do run tmL${n}S415 - $V "415" L; run tmT${n}S415 - $V "415" T; done
for n in 1 2 3; do run tmL${n}S923 - $T "923" L; run tmT${n}S923 - $T "923" T; done
for n in 1 2 3; do run tmL${n}B $JB $V "415 923" L; run tmT${n}B $JB $V "415 923" T; done
say "done $CAMP"
