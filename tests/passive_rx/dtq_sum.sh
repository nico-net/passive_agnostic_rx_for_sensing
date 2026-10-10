#!/bin/bash
# dtq_sum.sh <replay dir>...: last PDTIM / PDTIMPCT / PDSCHQ / UL_PUSCH_SOLVER lines of each run, ANSI stripped
for d in "$@"; do
  echo "== $d"
  sed 's/\x1b\[[0-9;]*m//g' "$d/run.log" | grep -a "PDTIM calls\|PDTIMPCT\|SENSING: PDSCHQ queued\|UL_PUSCH_SOLVER grants\|DMRS_SCAN full\|PDSCH decode pool\|PUSCH decode pool" | awk '/PDTIM calls/{a=$0} /PDTIMPCT/{b=$0} /PDSCHQ queued/{c=$0} /UL_PUSCH_SOLVER/{e=$0} /DMRS_SCAN full/{f=$0} /decode pool/{print substr($0,1,200)} END{print a; print b; print c; print e; print substr(f,1,300)}'
  sed 's/\x1b\[[0-9;]*m//g' "$d/run.log" | grep -ac "DECODETDRA"
done
