#!/bin/bash
# ota_traffic_ab.sh -- OTA A/B of legacy (L) vs traffic mode (T), 4 RX, same BUILD for both arms (legacy = no mode key).
# Cases: single 415, single 923, pair 415+923; L/T alternating x3 per case; DUR s each. Modelled on rr-orchestration/ota/run_ota_20261102.sh.
# SMOKE=1: one traffic run on single 415 only (tag tmT0S415). Night runs are [OTA LOW LOAD] functional only; repeat after 08:30.
set -u
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
unset ISAC_RX_BRANCH_FO
O=/home/nicola/NICOLA/wt/rr-orchestration; D=$O/ota
FIXW=/home/nicola/NICOLA/wt/rr-twomodes; BASEW=/home/nicola/NICOLA/wt/rr-sbase
ENVF=$D/MPCI_20261025.env; DUR=${DUR:-240}; SETTLE=${SETTLE:-45}; SMOKE=${SMOKE:-0}
V=3680010000:1171:-4827:217; T=3759990000:233:-4832:217; W=3590010000:193:-4868:162; L=3630000000:18:2178:51
CAMP=/home/nicola/NICOLA/ota_runs/sp_ota_day_$(date +%Y%m%d_%H%M%S); mkdir -p $CAMP/cpu
say() { echo "$(date +%H:%M:%S) $*" | tee -a $CAMP/campaign.log; }
[ -e $O/X410_UNAVAILABLE ] && { say "X410_UNAVAILABLE present"; exit 1; }
for WT in $FIXW $BASEW; do B=$WT/cmake_targets/ran_build/build; [ "$(ls $B/*.so | wc -l)" = 24 ] && [ -e $B/liboai_device.so ] || { say "build check failed $WT"; exit 1; }; done
exec 9>/home/nicola/NICOLA/heavy.lock; flock -x -w 3600 9 || exit 1; export HEAVY_HELD=1; exec 8>/tmp/td_measure.lock; flock -x -w 3600 8 || exit 1
say "fix=$(git -C $FIXW log --oneline -1) base=$(git -C $BASEW log --oneline -1) DUR=$DUR"
# read-only CPU sidecar: utime+stime ticks of the radio receiver every 5 s
( while [ ! -e $CAMP/cpu/STOP ]; do for p in $(pgrep -x nr-uesoftmodem); do tr "\0" " " < /proc/$p/cmdline 2>/dev/null | grep -q usrp-args || continue
    read -r -a s < <(sed 's/^[0-9]* ([^)]*) //' /proc/$p/stat 2>/dev/null) && echo "$(date +%s) $((${s[11]}+${s[12]}))" >> $CAMP/cpu/$p.tsv; done; sleep 5; done ) &
SAMP=$!; trap "touch $CAMP/cpu/STOP; wait $SAMP" EXIT
pcis_ok() { local r=$1 want=$2 log k=0 w bad=0; log=$(sed 's/\x1b\[[0-9;]*m//g' $r/run.log)
  if [ "$(echo $want | wc -w)" = 1 ]; then [ "$(echo "$log" | grep -o 'Initial sync successful, PCI: [0-9]*' | awk '{print $NF}' | sort -u | tr '\n' ' ')" = "$want " ]; return; fi
  for w in $want; do echo "$log" | grep -q "MPCI_SYNC cell=$k want=$w got=$w locked" || bad=1; k=$((k+1)); done; [ $bad = 0 ]; }
run() { # <tag> <cells.json|-> <tune> <pcis> <T|S> <fix|base>
  local tag=$1 cj=$2 f ssb fo rb prev r try t ok pre="" cfg pid; IFS=: read -r f ssb fo rb <<< "$3"; [ "$cj" != "-" ] && pre="$D/cellswrap.sh $cj"
  local W=$FIXW BUILD; [ $6 = base ] && W=$BASEW; BUILD=$W/cmake_targets/ran_build/build; CD=$W/tests/passive_rx
  cfg=$CD/traffic/traffic_ota.dgx.cfg; [ $5 = S ] && cfg=$CD/sensing/sensing_ota.dgx.cfg
  for try in $(seq 1 ${TRIES:-1}); do
    t=$tag; [ $try -gt 1 ] && t=${tag}r$try; say "-- $t arm=$6 cells=${cj##*/} tune=$3 want=[$4] cfg=${cfg##*/}"
    pgrep -x nr-uesoftmodem >/dev/null && { say "$t: another receiver running, waiting"; while pgrep -x nr-uesoftmodem >/dev/null; do sleep 30; done; }
    rm -f /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl; prev=$(cat $D/lastrun 2>/dev/null)
    OTA_PREFIX="$pre" BDIR=$BUILD FREQ=$f SSBOFF=$ssb IFO=$fo RB=$rb DUR=$DUR CFG=$cfg $D/starttest_auto.sh $t 1 65536 $(head -1 $ENVF) ISAC_SIB1_CACHE=0 | tee -a $CAMP/campaign.log
    r=$(cat $D/lastrun); [ "$r" = "$prev" ] && { say "$t NOT STARTED"; sleep 20; continue; }
    cp $D/last_ota_out.txt $r/ota_out.txt 2>/dev/null
    ls /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl >/dev/null 2>&1 && mv /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl $r/ 2>/dev/null
    pid=$(grep -o 'pid [0-9]*' $r/ota_out.txt | head -1 | awk '{print $2}')
    [ -n "$pid" ] && [ -s $CAMP/cpu/$pid.tsv ] && awk 'NR==1{t0=$1;c0=$2} {t1=$1;c1=$2} END{if(t1>t0) printf "%.3f\n",(c1-c0)/100/(t1-t0)}' $CAMP/cpu/$pid.tsv > $r/cpu_cores.txt
    pcis_ok $r "$4" && ok=OK || ok=MISMATCH
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$t" "$r" "$4" "$ok" "$6" "$(date +%H:%M:%S)" >> $CAMP/runs.tsv; say "$t -> $r ($(cat $r/rx_exit.txt 2>/dev/null)) pci=$ok cpu=$(cat $r/cpu_cores.txt 2>/dev/null)"
    sleep $SETTLE; [ $ok = OK ] && return
  done
}
for c in "415 $V" "923 $T" "508 $W" "403 $L"; do set -- $c; p=$1; tn=$2; case " ${CELLS:-415 923 508 403} " in *" $p "*) ;; *) continue;; esac
  for x in T1:T:fix S1:S:base S2:S:fix T2:T:fix S3:S:fix S4:S:base; do IFS=: read -r n m a <<< "$x"
    if [ $p = 403 ]; then [ $n = T1 ] || [ $n = S1 ] || [ $n = S2 ] || continue; fi
    TRIES=2; [ $p = 403 ] && TRIES=1; run sp${p}${m}${a:0:1}${n} - $tn "$p" $m $a; done
done
say "done $CAMP"
