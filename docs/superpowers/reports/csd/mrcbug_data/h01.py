import sys,collections
sys.path.insert(0,'.')
from evm import *
import glob
for tag in ['H0','H1']:
    rows=[]
    for f in sorted(glob.glob(f'/home/nicola/NICOLA/ota_runs/ab_{tag}[abc]_1_08*/run.log')):
        rows+=parse(f)
    rows=[r for r in rows if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1]
    print(tag,'n',len(rows))
    for comb in (0,1,3):
        sub=[r for r in rows if r['comb']==comb]
        print(' comb',comb,'n',len(sub))
        for b in ['0-4','5-9','10-16','17+']:
            s=[r for r in sub if bucket(r['mcs'])==b and r['snr']>=10]
            if s: print(f'   {b:6s} n={len(s):5d} crc={100*sum(r["crc"] for r in s)/len(s):5.1f}% msnr={sum(r["snr"] for r in s)/len(s):.1f}')
