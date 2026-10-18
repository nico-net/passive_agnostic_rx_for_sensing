"""For pass bodies whose C queue wait or C/D exec span exceeds 1 ms (GPU stall), vs all pass bodies: fraction with a long (>1 ms) disc/other kernel running at C launch,
and which kernel names. Also host_pre > 1 ms: what the thread did (API calls > 200 us inside)."""
import sqlite3,sys,json,bisect
from collections import Counter
db=sqlite3.connect(sys.argv[1]); S=dict(db.execute('select id,value from StringIds'))
LK=[(s,e,S[n],g) for s,e,n,g in db.execute('select start,end,shortName,greenContextId from CUPTI_ACTIVITY_KIND_KERNEL where end-start>1000000')]
LK.sort(); st=[x[0] for x in LK]
def running(t):
    i=bisect.bisect_right(st,t); return [x for x in LK[max(0,i-400):i] if x[1]>t]
R=[r for r in json.load(open(sys.argv[1]+'.rows.json')) if r.get('path')=='pass' and 'x_C' in r and 'q_C' in r]
stall=[r for r in R if (r['q_C'] or 0)>1000 or (r['x_C'] or 0)>2000 or (r.get('x_D') or 0)>2000]
def frac(G):
    c=Counter(); n=0
    for r in G:
        t=r['tend']-r['dec']*1000+r['host_pre']*1000+500_000  # ~ C launch
        k=running(t); n+=bool(k)
        for x in set(y[2] for y in k): c[x]+=1
    return n,c
n1,c1=frac(stall); n2,c2=frac(R)
print(sys.argv[1].split('/')[-2],'pass bodies',len(R),'GPU-stalled (qC>1ms or xC/xD>2ms):',len(stall),'| long kernel running at C: stalled %d/%d, all %d/%d'%(n1,len(stall),n2,len(R)))
print('   stalled:',dict(c1.most_common(5)),' all:',dict(c2.most_common(5)))
