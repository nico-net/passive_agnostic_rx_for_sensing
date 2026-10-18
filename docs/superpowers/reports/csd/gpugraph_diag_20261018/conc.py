import json,sys,bisect
from collections import Counter
def q(v,f):
    v=sorted(v); return v[min(len(v)-1,int(f*len(v)))] if v else float('nan')
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    R=[r for r in json.load(open(p[3]+'/nsys_trace.sqlite.rows.json')) if 'body' in r]
    iv=sorted((r['tend']-r['dec']*1000,r['tend']) for r in R); st=[x[0] for x in iv]
    def inflight(r):
        a,b=r['tend']-r['dec']*1000,r['tend']
        i=bisect.bisect_right(st,b); return sum(1 for x in iv[max(0,i-200):i] if x[1]>a)-1
    R.sort(key=lambda r:r['tot']); n=len(R); B=R[int(.98*n):int(.995*n)]
    big=Counter()
    for r in R[int(.99*n):]:
        for k in ('a2e','qw','pre','host_pre','q_C','x_C','x_D','x_L','host_post'):
            if (r.get(k) or 0)>=2000: big[k]+=1
    print(f"{p[0]:3} {p[1]:4} {p[2]} real grants in flight during dec: all med {q([inflight(r) for r in R[::5]],.5)} p90 {q([inflight(r) for r in R[::5]],.9)} | band med {q([inflight(r) for r in B],.5)} p90 {q([inflight(r) for r in B],.9)} | top-1% grants with a part >= 2 ms: {dict(big)} of {n-int(.99*n)}")
