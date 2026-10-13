import sys,collections,glob,statistics
sys.path.insert(0,'.')
from evm import *
for tag in ['H0','H1']:
    rows=[]
    for f in sorted(glob.glob(f'/home/nicola/NICOLA/ota_runs/ab_{tag}[abc]_1_08*/run.log')): rows+=parse(f)
    rows=[r for r in rows if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1 and r['qm']>2 and r['comb']==3]
    k=[r['kappa'] for r in rows]
    print(tag,'comb3 QAM n',len(k),'kappa>1.1: %.1f%%'%(100*sum(1 for x in k if x>1.1)/len(k)),'kappa>1.3: %.1f%%'%(100*sum(1 for x in k if x>1.3)/len(k)),
          'CRC kappa<=1.1 %.1f%% (n %d) / >1.1 %.1f%% (n %d)'%(100*statistics.mean(r['crc'] for r in rows if r['kappa']<=1.1),sum(1 for r in rows if r['kappa']<=1.1),
           100*statistics.mean(r['crc'] for r in rows if r['kappa']>1.1),sum(1 for r in rows if r['kappa']>1.1)))
