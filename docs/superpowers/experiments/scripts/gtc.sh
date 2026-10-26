#!/bin/bash
# wall-clock per-RNTI DL recall + pdsch crc
for r in "$@"; do d=/home/sens/NICOLA/captures/$r; e=$(ls -d $d/*/export | head -1); o=${OUT:-/tmp}/gt_$r
(cd "$(cd "$(dirname "$0")/../../../.." && pwd)" && systemd-run --user --scope --quiet -p MemoryMax=2G -p MemorySwapMax=0 choom -n 1000 -- nice -n 19 python3 -I tools/mobisys/groundtruth.py --rx $e --gnb-csv $d/gnb/sched.csv --out $o >/dev/null 2>&1)
python3 -c "
import json
j=json.load(open('$o/summary.json'))
for k,v in j['clock_matched']['per_rnti'].items():
  if v.get('gnb',0)>1000: print('$r'[:6],k,'recall',round(v['recall'],3),'prec',round(v.get('precision') or 0,3),'pdsch_crc',v.get('pdsch_crc') and round(v['pdsch_crc'],3))
"; done
