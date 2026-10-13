import sys,collections,glob,math,statistics
sys.path.insert(0,'.')
from evm import *
rows=[]
for f in sorted(glob.glob('/home/nicola/NICOLA/ota_runs/ab_H[01][abc]_1_08*/run.log')):
    rows+=parse(f)
rows=[r for r in rows if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1 and r['qm']>2]
bins=[(10,13),(13,16),(16,19),(19,22),(22,25),(25,99)]
print('kappa mean by snr bin')
for comb in (1,3):
  for qm in (4,6):
    out=[]
    for lo,hi in bins:
        s=[r for r in rows if r['comb']==comb and r['qm']==qm and lo<=r['snr']<hi]
        out.append(f'{statistics.mean(r["kappa"] for r in s):.3f}({len(s):4d})' if len(s)>3 else '   --      ')
    print('comb',comb,'Qm',qm,' '.join(out))
print('CRC by kappa class, comb3, Qm6, snr bins')
for lo_k,hi_k in [(0,1.05),(1.05,1.15),(1.15,1.3),(1.3,9)]:
    out=[]
    for lo,hi in bins:
        s=[r for r in rows if r['comb']==3 and r['qm']==6 and lo<=r['snr']<hi and lo_k<=r['kappa']<hi_k]
        out.append(f'{100*sum(r["crc"] for r in s)/len(s):5.1f}({len(s):4d})' if s else '   --(   0)')
    print(f'kappa {lo_k}-{hi_k}',' '.join(out))
print('CRC by kappa class, comb3, Qm4, snr bins')
for lo_k,hi_k in [(0,1.02),(1.02,1.06),(1.06,9)]:
    out=[]
    for lo,hi in bins:
        s=[r for r in rows if r['comb']==3 and r['qm']==4 and lo<=r['snr']<hi and lo_k<=r['kappa']<hi_k]
        out.append(f'{100*sum(r["crc"] for r in s)/len(s):5.1f}({len(s):4d})' if s else '   --(   0)')
    print(f'kappa {lo_k}-{hi_k}',' '.join(out))
# kappa vs rb count (comb3 Qm6)
print('kappa vs nRB, comb3 Qm6')
for lo,hi in [(0,20),(20,60),(60,150),(150,300)]:
    s=[r for r in rows if r['comb']==3 and r['qm']==6 and lo<=int(r['rb'].split('+')[1])<hi]
    if s: print(lo,hi,len(s),'%.3f'%statistics.mean(r['kappa'] for r in s))
# kappa vs branch spread (max br - min br)
print('kappa vs br_snr spread, comb3 Qm6')
for lo,hi in [(0,5),(5,10),(10,15),(15,99)]:
    s=[r for r in rows if r['comb']==3 and r['qm']==6 and lo<=max(r['br'])-min(r['br'])<hi]
    if s: print(lo,hi,len(s),'%.3f'%statistics.mean(r['kappa'] for r in s),'crc %.1f'%(100*sum(r['crc'] for r in s)/len(s)),'snr %.1f'%statistics.mean(r['snr'] for r in s))
