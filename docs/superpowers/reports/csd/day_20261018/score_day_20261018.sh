#!/bin/bash
# Score the 2026-10-18 daytime runs with goal_metrics.py: one DL and one UL line per run (p99 < 3 ms verdict).
GM=/home/nicola/NICOLA/wt/rr-csd-int16/tests/passive_rx/goal_metrics.py
for d in $(grep -o '/home/nicola/NICOLA/ota_runs/ab_d[TV][GLC][0-9]_[^ ]*' /home/nicola/NICOLA/wt/rr-orchestration/ota/day_20261018_out.txt); do
  r=$(basename $d | cut -d_ -f2)
  python3 $GM $d/run.log | sed "s|$d/run.log|$r|" | awk '{printf "%s %s %-22s", $1,$2,$3; for(i=4;i<=NF;i++) if($i ~ /^(n_real|tot_p50|tot_p99|crc_all|crc_q|drops_pct)=/) printf " %s", $i; print ""}'
done
