import sys; sys.path.insert(0,'.')
from e2eall import rows
from collections import defaultdict
import statistics as st
B=[(1,24),(25,48),(49,96),(97,160),(161,273)]
agg=defaultdict(lambda: defaultdict(list))
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    cap,arm,i,d=p
    if arm not in ('OFF','ON'): continue
    for r in rows(d+'/run.log'):
        b=next(k for k,(lo,hi) in enumerate(B) if lo<=r['nrb']<=hi)
        path='stage' if r['stage'] else 'pass'
        if arm=='OFF': path='off'
        agg[(cap,b)][path].append(r['first'])
def q(v,f): v=sorted(v); return v[min(len(v)-1,int(f*len(v)))]
for (cap,b),d in sorted(agg.items()):
    print(cap,B[b],' | '.join(f"{p}: n={len(v)} p50={q(v,.5)} p90={q(v,.9)} p99={q(v,.99)}" for p,v in sorted(d.items())))
