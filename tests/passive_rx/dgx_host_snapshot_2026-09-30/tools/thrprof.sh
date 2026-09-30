#!/bin/bash
# thrprof.sh <arm> : wait until the arm's receiver has run 60 s, then profile per-thread CPU for 20 s
P=$(cd "$(dirname "$0")" && pwd); A=$P/$1
until [ -f $A/rx/rx.log ] && [ $(wc -l < $A/rx/rx.log) -gt 0 ] && awk 'END{exit !($1>60)}' $A/rx/rx.log; do sleep 2; done
pid=$(ps -eo pid,comm | awk '$2=="nr-uesoftmodem"{print $1; exit}')
snap(){ for t in /proc/$pid/task/*; do s=$(cat $t/stat 2>/dev/null) || continue; n=$(tr ' ' '_' < $t/comm); r=${s##*) }; set -- $r; echo "$(basename $t) $n ${12} ${13} ${37}"; done | sort; }
snap > $A/thr_a.txt; sleep 20; snap > $A/thr_b.txt
join $A/thr_a.txt $A/thr_b.txt | awk '{cpu=(($7+$8)-($3+$4))/20; printf "%-18s %6.1f%% last_core=%s\n",$2,cpu,$10}' | sort -k2 -nr > $A/thrprof.txt
awk '{gsub("%","",$2); s+=$2} END{printf "TOTAL %.1f%% threads=%d\n", s, NR}' $A/thrprof.txt >> $A/thrprof.txt
echo PROFILED $1
