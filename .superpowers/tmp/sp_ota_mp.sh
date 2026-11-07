#!/bin/bash
# sp_ota_mp.sh -- multi-PCI OTA campaign 2026-11-07 (modelled on sp_ota_q1.sh and rr-orchestration/ota/run_mpci_ota_20261031.sh). Build rr-single1q (twomodes/single1-q ce696543f7), not rebuilt.
# usage: sp_ota_mp.sh campaign            -> whole plan (step 1 attempt at 400 MHz span, step 2 at 200 MHz: traffic then sensing)
#        sp_ota_mp.sh arm <tag> <cells.json|-> <f:ssb:fo:rb> <want pcis> <T|S> <dur> <campdir>   (called by the campaign under the locks, one arm each)
# Locks per arm: flock -x NICOLA/heavy.lock (what the other agent's builds take shared), flock -x orchestration/locks/heavy.lock (what ota.sh takes), then /tmp/td_measure.lock.
set -u
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
unset ISAC_RX_BRANCH_FO
O=/home/nicola/NICOLA/wt/rr-orchestration; D=$O/ota; W=/home/nicola/NICOLA/wt/rr-single1q; B=$W/cmake_targets/ran_build/build; T=$W/.superpowers/tmp
ENVF=$D/MPCI_20261025.env; SETTLE=${SETTLE:-45}; SELF=$T/sp_ota_mp.sh
V=3680010000:1171:-4827:217; Tt=3759990000:233:-4832:217
CJ=$T/mp_cells; say() { echo "$(date +%H:%M:%S) $*" | tee -a $CAMP/campaign.log; }
pcis_ok() { local r=$1 want=$2 log k=0 w bad=0; log=$(sed 's/\x1b\[[0-9;]*m//g' $r/run.log)
  if [ "$(echo $want | wc -w)" = 1 ]; then [ "$(echo "$log" | grep -o 'Initial sync successful, PCI: [0-9]*' | awk '{print $NF}' | sort -u | tr '\n' ' ')" = "$want " ]; return; fi
  for w in $want; do echo "$log" | grep -q "MPCI_SYNC cell=$k want=$w got=$w locked" || bad=1; k=$((k+1)); done; [ $bad = 0 ]; }

if [ "${1:-}" = arm ]; then
  tag=$2; cj=$3; tune=$4; want=$5; mode=$6; DUR=$7; CAMP=$8; IFS=: read -r f ssb fo rb <<< "$tune"; pre=""; [ "$cj" != "-" ] && pre="$D/cellswrap.sh $cj"
  cfg=$W/tests/passive_rx/traffic/traffic_ota.dgx.cfg; [ $mode = S ] && cfg=$W/tests/passive_rx/sensing/sensing_ota.dgx.cfg
  [ -e $O/X410_UNAVAILABLE ] && { say "X410_UNAVAILABLE present"; exit 1; }
  while pgrep -x nr-uesoftmodem >/dev/null; do say "$tag: nr-uesoftmodem running (owner/cmd: $(ps -o user=,args= -p $(pgrep -x nr-uesoftmodem | head -1) | cut -c1-120)); waiting"; sleep 30; done
  say "-- $tag cells=${cj##*/} tune=$tune want=[$want] mode=$mode dur=$DUR cfg=${cfg##*/} build=$(git -C $W log --oneline -1 | cut -c1-60)"
  rm -f /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl; prev=$(cat $D/lastrun 2>/dev/null); mkdir -p $CAMP/cpu
  ( while [ ! -e $CAMP/cpu/STOP.$tag ]; do for p in $(pgrep -x nr-uesoftmodem); do tr "\0" " " < /proc/$p/cmdline 2>/dev/null | grep -q usrp-args || continue
      read -r -a s < <(sed 's/^[0-9]* ([^)]*) //' /proc/$p/stat 2>/dev/null) && echo "$(date +%s) $((${s[11]}+${s[12]}))" >> $CAMP/cpu/$tag.$p.tsv; done; sleep 5; done ) &
  SAMP=$!
  ( while [ ! -e $CAMP/cpu/STOP.$tag ]; do nvidia-smi --query-gpu=utilization.gpu,clocks.sm,power.draw --format=csv,noheader 2>/dev/null; sleep 2; done ) > $CAMP/cpu/$tag.gpu.tsv &
  SMI=$!
  OTA_PREFIX="$pre" BDIR=$B FREQ=$f SSBOFF=$ssb IFO=$fo RB=$rb DUR=$DUR CFG=$cfg $D/starttest_auto.sh $tag 1 65536 $(head -1 $ENVF) ISAC_SIB1_CACHE=0 | tee -a $CAMP/campaign.log
  touch $CAMP/cpu/STOP.$tag; wait $SAMP $SMI 2>/dev/null
  r=$(cat $D/lastrun); [ "$r" = "$prev" ] && { say "$tag NOT STARTED"; sleep 20; exit 3; }
  cp $D/last_ota_out.txt $r/ota_out.txt 2>/dev/null
  for x in /home/nicola/NICOLA/ota_runs/sense_records_last*.jsonl; do [ -e $x ] && mv $x $r/; done
  pid=$(grep -o 'pid [0-9]*' $r/ota_out.txt | head -1 | awk '{print $2}')
  [ -n "$pid" ] && [ -s $CAMP/cpu/$tag.$pid.tsv ] && awk 'NR==1{t0=$1;c0=$2} {t1=$1;c1=$2} END{if(t1>t0) printf "%.3f\n",(c1-c0)/100/(t1-t0)}' $CAMP/cpu/$tag.$pid.tsv > $r/cpu_cores.txt
  cp $CAMP/cpu/$tag.gpu.tsv $r/gpu.tsv 2>/dev/null
  pcis_ok $r "$want" && ok=OK || ok=MISMATCH
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$tag" "$r" "$want" "$ok" "$mode" "$(date +%H:%M:%S)" >> $CAMP/runs.tsv
  say "$tag -> $r ($(cat $r/rx_exit.txt 2>/dev/null)) pci=$ok cpu=$(cat $r/cpu_cores.txt 2>/dev/null)"
  python3 -I $T/m_mp.py $r 2>&1 | tail -2 | tee -a $CAMP/campaign.log
  sleep $SETTLE; [ $ok = OK ] && exit 0 || exit 10
fi

# ---------------- campaign driver (no locks held between arms)
CAMP=/home/nicola/NICOLA/ota_runs/mpcamp_$(date +%Y%m%d_%H%M%S); mkdir -p $CAMP/cpu
echo "$CAMP" > $T/mp_last_campaign
LK=(flock -x -w 14400 /home/nicola/NICOLA/heavy.lock flock -x -w 14400 $O/locks/heavy.lock flock -x -w 14400 /tmp/td_measure.lock env HEAVY_HELD=1)
runarm() { "${LK[@]}" bash $SELF arm "$@" $CAMP; }   # tag cj tune want mode dur
say "campaign start build=$(git -C $W log --oneline -1) env=$(basename $ENVF) gain=38"
# step 1: 400 MHz span set (415+923+508+403), one 120 s attempt, sensing cfg: records what the planner / driver / receiver do (no rerun)
[ "${SKIP1:-0}" = 1 ] || runarm m4S_attempt $CJ/span400_415_923_508_403.json $V "415 923 508 403" S 120
# step 2: 200 MHz (245.76 MS/s) pair 415+923; interleaved pair / solo arms, traffic then sensing; failed arms rerun (max 2 rounds)
FAILS=()
for mode in T S; do
  for a in P1:pair S415:415 P2:pair S923:923 P3:pair; do
    n=${a%%:*}; w=${a##*:}; tag=mp2${mode}_$n
    case $w in pair) cj=$CJ/pair415_923.json; tune=$V; want="415 923";; 415) cj=-; tune=$V; want=415;; 923) cj=-; tune=$Tt; want=923;; esac
    runarm $tag $cj $tune "$want" $mode ${DUR:-240}; [ $? = 0 ] || FAILS+=("$tag|$cj|$tune|$want|$mode")
  done
done
for rnd in 1 2; do N=("${FAILS[@]}"); FAILS=()
  for f in "${N[@]}"; do IFS='|' read -r t c u w m <<< "$f"; say "RERUN$rnd $t"; runarm ${t}R$rnd $c $u "$w" $m ${DUR:-240}; [ $? = 0 ] || FAILS+=("$t|$c|$u|$w|$m"); done
done
say "done $CAMP"
