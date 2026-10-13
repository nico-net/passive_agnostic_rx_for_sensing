import sys,collections,glob
sys.path.insert(0,'.')
from evm import *
bins=[(10,13),(13,16),(16,19),(19,22),(22,25),(25,99)]
for tag in ['H0','H1']:
    rows=[]
    for f in sorted(glob.glob(f'/home/nicola/NICOLA/ota_runs/ab_{tag}[abc]_1_08*/run.log')):
        rows+=parse(f)
    rows=[r for r in rows if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1]
    for comb in (1,3):
        for b in ['5-9','10-16','17+']:
            out=[]
            for lo,hi in bins:
                s=[r for r in rows if r['comb']==comb and bucket(r['mcs'])==b and lo<=r['snr']<hi]
                out.append(f'{100*sum(r["crc"] for r in s)/len(s):5.1f}({len(s):4d})' if s else '   --(   0)')
            print(tag,'comb',comb,f'{b:6s}',' '.join(out))
