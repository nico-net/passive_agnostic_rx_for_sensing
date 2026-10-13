import sys,collections,glob,math
sys.path.insert(0,'.')
from evm import *
rows=[]
for f in sorted(glob.glob('/home/nicola/NICOLA/ota_runs/ab_H[01][abc]_1_08*/run.log')):
    rows+=parse(f)
rows=[r for r in rows if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1]
def lin(x): return 10**(x/10)
for r in rows:
    br=r['br']; nv=r['nvar']
    pw=[lin(b)*lin(n) for b,n in zip(br,nv)]  # branch channel power proxy (signal part)
    pwn=[(lin(b)+1)*lin(n) for b,n in zip(br,nv)] # incl noise (what avg|h|^2 sees)
    mx=max(pwn); r['contrib']=sum(1 for p in pwn if p>=mx*lin(-12))
    r['ideal']=10*math.log10(sum(lin(b) for b in br)); r['bestbr']=max(br)
    r['gain']=r['ideal']-r['bestbr']
# kappa by comb, Qm
for comb in (1,3):
    for qm in (2,4,6):
        s=[r for r in rows if r['comb']==comb and r['qm']==qm and r['snr']>=10]
        if s: print('comb',comb,'Qm',qm,'n',len(s),'kappa mean %.3f'%(sum(r['kappa'] for r in s)/len(s)),'rho %.2f'%(sum(r['rho'] for r in s)/len(s)),
                    'snr-ideal %.1f'%(sum(r['snr']-r['ideal'] for r in s)/len(s)), 'snr-best %.1f'%(sum(r['snr']-r['bestbr'] for r in s)/len(s)))
print('contrib distribution comb3:',collections.Counter(r['contrib'] for r in rows if r['comb']==3))
# CRC vs snr for comb3 by MRC gain (ideal-best) small vs large
bins=[(10,13),(13,16),(16,19),(19,22),(22,25),(25,99)]
for b in ['5-9','10-16']:
  for lo_g,hi_g,name in [(-99,1.5,'gain<1.5'),(1.5,3,'1.5-3'),(3,99,'gain>=3')]:
    out=[]
    for lo,hi in bins:
        s=[r for r in rows if r['comb']==3 and bucket(r['mcs'])==b and lo<=r['snr']<hi and lo_g<=r['gain']<hi_g]
        out.append(f'{100*sum(r["crc"] for r in s)/len(s):5.1f}({len(s):4d})' if s else '   --(   0)')
    print('comb3',b,name,' '.join(out))
# snr - ideal distribution for comb3: is snr above ideal MRC of branch snrs?
import statistics
s=[r for r in rows if r['comb']==3 and r['snr']>=10]
d=[r['snr']-r['ideal'] for r in s]
print('comb3 snr-ideal: mean %.2f median %.2f p10 %.2f p90 %.2f'%(statistics.mean(d),statistics.median(d),sorted(d)[len(d)//10],sorted(d)[9*len(d)//10]))
s=[r for r in rows if r['comb']==1 and r['snr']>=10]
d=[r['snr']-r['bestbr'] for r in s]
print('comb1 snr-best: mean %.2f median %.2f'%(statistics.mean(d),statistics.median(d)))
