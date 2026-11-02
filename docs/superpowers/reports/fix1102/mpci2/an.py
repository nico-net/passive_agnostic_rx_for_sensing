# an.py <run dir> ...: per cell: syncs, e2e per stage p50/p99, CRC (rv0 non-SI), drops. Splits rx.log by [cN] tag; untagged = solo cell 0.
import sys,re,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from parse import parse
ansi=re.compile(r'\x1b\[[0-9;]*m')
def q(v,p): v=sorted(v); return v[min(len(v)-1,int(len(v)*p))] if v else -1
for d in sys.argv[1:]:
    cells={}
    for l in open(d+'/rx.log',errors='ignore'):
        l=ansi.sub('',l); m=re.match(r'\[c(\d)\]',l); c=m.group(1) if m else '0'
        cells.setdefault(c,[]).append(l)
    for c,ls in sorted(cells.items()):
        p=f'{d}/c{c}.log'; open(p,'w').writelines(ls)
        txt=''.join(ls)
        sy=len(re.findall(r'SYNC_SCAN.*detected=1',txt)); fl=len(re.findall(r'SYNC_SCAN.*detected=0',txt))
        R=[r for r in parse(p) if r['probe']==0 and r['rnti']!='0xffff' and r['rv']==0]
        E=[r for r in R if r['e2e']]
        crc=100*sum(r['crc'] for r in R)/max(len(R),1)
        st=' '.join(f"{q([r['e2e'][i] for r in E],.5)}/{q([r['e2e'][i] for r in E],.99)}" for i in range(7)) if E else ''
        last={}
        for l in ls:
            if 'ISAC_METRICS' in l: last=dict((m.group(1),int(m.group(2))) for m in re.finditer(r'"(\w+)":(-?\d+)',l))
        qd=last.get('sense_async_queued_pdsch_data',0); dr=last.get('sense_async_drop_pdsch_data',0)
        print(f"{os.path.basename(d)} c{c} syncOK={sy} syncFail={fl} grants={len(R)} crc={crc:.1f} e2e[arr2enq,qwait,pre,dec,fb,bodies,tot] p50/p99: {st} | asyncdrop={100*dr/max(qd,1):.1f}% scanq_drop_full={last.get('scanq_drop_full',-1)} gpu_gives={last.get('gpu_gives',-1)}")
