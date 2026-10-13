import numpy as np
rng=np.random.default_rng(1)
def dd_snr(qm,true_snr_db,n=200000):
    lmax=2**(qm//2)-1; lv=np.arange(-lmax,lmax+1,2); ideal=np.mean(lv**2)*2
    x=rng.choice(lv,n)+1j*rng.choice(lv,n)
    if true_snr_db is None: y=(rng.normal(size=n)+1j*rng.normal(size=n))
    else:
        s=np.sqrt(ideal/10**(true_snr_db/10)/2); y=x+s*(rng.normal(size=n)+1j*rng.normal(size=n))
    y=y*np.sqrt(ideal/np.mean(abs(y)**2))
    d=lambda v: np.clip(2*np.floor(v/2)+1,-lmax,lmax)
    e=np.mean((y.real-d(y.real))**2+(y.imag-d(y.imag))**2)/ideal
    return -10*np.log10(e)
print('DD SNR of pure noise: '+'  '.join(f'Qm{qm}: {dd_snr(qm,None):.1f} dB' for qm in (2,4,6,8)))
for qm in (2,4,6):
    print(f'Qm{qm} true->DD:',' '.join(f'{t}->{dd_snr(qm,t):.1f}' for t in (0,3,6,9,12,15,18,21,24)))
import sys,glob,statistics
sys.path.insert(0,'.')
from evm import *
rows=[]
for f in sorted(glob.glob('/home/nicola/NICOLA/ota_runs/ab_H[01][abc]_1_08*/run.log')): rows+=parse(f)
rows=[r for r in rows if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1]
for qm in (2,4,6):
  for comb in (1,3):
    s=sorted(r['snr'] for r in rows if r['qm']==qm and r['comb']==comb and r['crc']==0)
    if len(s)>10: print(f'Qm{qm} comb{comb} CRC-FAIL n={len(s)} snr p5={s[len(s)//20]:.1f} p25={s[len(s)//4]:.1f} p50={s[len(s)//2]:.1f}')
import sys,glob,statistics,math
sys.path.insert(0,'.')
from evm import *
lin=lambda x:10**(x/10)
rows=[]
for f in sorted(glob.glob('/home/nicola/NICOLA/ota_runs/ab_H[01][abc]_1_08*/run.log')): rows+=parse(f)
rows=[r for r in rows if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1]
for r in rows:
    r['ideal']=10*math.log10(sum(lin(b) for b in r['br'] if b>-90)); r['best']=max(r['br'])
print('reported snr minus the LS-branch bound (comb1: best br_snr; comb3: ideal MRC sum), by Qm and CRC; n, mean, p50, p90, share > bound+1 dB')
for qm in (2,4,6):
  for comb,key in ((1,'best'),(3,'ideal')):
    for crc in (1,0):
        s=[r['snr']-r[key] for r in rows if r['qm']==qm and r['comb']==comb and r['crc']==crc]
        if len(s)>10:
            s.sort(); print(f'Qm{qm} comb{comb} crc={crc} n={len(s):5d} mean={statistics.mean(s):5.1f} p50={s[len(s)//2]:5.1f} p90={s[9*len(s)//10]:5.1f} above+1dB={100*sum(1 for x in s if x>1)/len(s):4.1f}%')
print()
print('comb3 failing grants: weakest-branch structure. share of grants where >=2 branches have br_snr < 0 dB, by Qm, CRC')
for qm in (2,4,6):
    for crc in (1,0):
        s=[r for r in rows if r['qm']==qm and r['comb']==3 and r['crc']==crc]
        if s: print(f'Qm{qm} crc={crc} n={len(s)} >=2 branches <0dB: {100*sum(1 for r in s if sum(1 for b in r["br"] if b<0)>=2)/len(s):.0f}%  mean best br {statistics.mean(r["best"] for r in s):.1f} mean ideal {statistics.mean(r["ideal"] for r in s):.1f} mean snr {statistics.mean(r["snr"] for r in s):.1f}')
