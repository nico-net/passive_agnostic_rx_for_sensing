import sys,collections,glob,math,statistics,re
sys.path.insert(0,'.')
from evm import *
def load(log):
    rows=[r for r in parse(log) if r['probe']==0 and r['rnti']!='0xffff']
    rm={}
    for line in open(log,errors='replace'):
        if 'RMMAP' not in line: continue
        d=dict(re.findall(r'(\w+)=([^\s\[]+)',line)); m=re.search(r'fs=(\d+)\.(\d+)',line)
        rm[(d['rnti'],int(m.group(1)),int(m.group(2)))]=dict(err=float(d['err_pct']),zero=float(d['zero_pct']))
    for r in rows:
        g=rm.get((r['rnti'],r['frame'],r['slot'])); r['err']=g['err'] if g else None; r['zero']=g['zero'] if g else None
    return rows
bins=[(6,10),(10,13),(13,16),(16,19),(19,22),(22,99)]
def crc_table(rows,label):
    n1=[r for r in rows if r['nl']==1]
    print(f'{label}: Nl=1 grants {len(n1)} comb dist {dict(collections.Counter(r["comb"] for r in n1))}')
    for comb in (1,3):
      for b in ['0-4','5-9','10-16','17+']:
        out=[]
        for lo,hi in bins:
            s=[r for r in n1 if r['comb']==comb and bucket(r['mcs'])==b and lo<=r['snr']<hi]
            out.append(f'{100*sum(r["crc"] for r in s)/len(s):5.1f}({len(s):4d})' if s else '   --(   0)')
        print(f'  comb {comb} {b:6s}',' '.join(out))
    print('  all Nl=1 at snr>=10 by bucket, comb1 / comb3:')
    for b in ['0-4','5-9','10-16','17+']:
        s1=[r for r in n1 if r['comb']==1 and bucket(r['mcs'])==b and r['snr']>=10]
        s3=[r for r in n1 if r['comb']==3 and bucket(r['mcs'])==b and r['snr']>=10]
        f=lambda s: f'{100*sum(r["crc"] for r in s)/len(s):5.1f}% ({len(s)})' if s else '--'
        print(f'    {b:6s} {f(s1):>14s} / {f(s3):>14s}')
    print('  kappa mean Nl=1 QAM comb1/comb3 by snr bin:')
    for comb in (1,3):
        out=[]
        for lo,hi in bins:
            s=[r for r in n1 if r['comb']==comb and r['qm']>2 and lo<=r['snr']<hi]
            out.append(f'{statistics.mean(r["kappa"] for r in s):.3f}({len(s):3d})' if len(s)>2 else '    --     ')
        print(f'    comb {comb}',' '.join(out))
    print('  genie err_pct/zero_pct on CRC-OK TBs, Nl=1, comb1/comb3 by snr bin (all Qm):')
    for comb in (1,3):
        out=[]
        for lo,hi in bins:
            s=[r for r in n1 if r['comb']==comb and lo<=r['snr']<hi and r['err'] is not None]
            out.append(f'{statistics.mean(r["err"] for r in s):4.2f}/{statistics.mean(r["zero"] for r in s):4.1f}({len(s):3d})' if len(s)>2 else '      --      ')
        print(f'    comb {comb}',' '.join(out))
    if any(r['l2']>=0 for r in n1):
        print('  l2 (log2_maxh) distribution Nl=1 by comb:')
        for comb in (1,3):
            print(f'    comb {comb}',sorted(collections.Counter(r['l2'] for r in n1 if r['comb']==comb).items()))
for a in sys.argv[1:]:
    crc_table(load(a+'/run.log'),a.split('/')[-1])
