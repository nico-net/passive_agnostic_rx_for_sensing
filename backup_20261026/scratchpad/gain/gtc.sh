#!/bin/bash
# wall-clock per-RNTI DL recall + pdsch crc
for r in "$@"; do d=/home/sens/NICOLA/captures/$r; e=$(ls -d $d/*/export | head -1); o=/tmp/claude-1000/-home-sens-NICOLA-adaptive-rx-UL-DL/6de2bc9d-5f38-466a-9776-f15a57560647/scratchpad/gt_$r
(cd /home/sens/NICOLA/agn-wt/hmcs && systemd-run --user --scope --quiet -p MemoryMax=2G -p MemorySwapMax=0 choom -n 1000 -- nice -n 19 python3 -I tools/mobisys/groundtruth.py --rx $e --gnb-csv $d/gnb/sched.csv --out $o >/dev/null 2>&1)
python3 -c "
import json
j=json.load(open('$o/summary.json'))
for k,v in j['clock_matched']['per_rnti'].items():
  if v.get('gnb',0)>1000: print('$r'[:6],k,'recall',round(v['recall'],3),'prec',round(v.get('precision') or 0,3),'pdsch_crc',v.get('pdsch_crc') and round(v['pdsch_crc'],3))
"; done
