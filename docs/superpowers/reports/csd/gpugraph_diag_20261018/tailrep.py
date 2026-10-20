import json,sys,statistics as st
from collections import defaultdict
def m(v): v=[x for x in v if x is not None]; return sum(v)/len(v) if v else float('nan')
def q(v,f): v=sorted(x for x in v if x is not None); return v[min(len(v)-1,int(f*len(v)))] if v else float('nan')
for f in sys.argv[1:]:
    R=json.load(open(f)); R=[r for r in R if 'tot' in r]
    p99=q([r['tot'] for r in R],.99); T=[r for r in R if r['tot']>=p99]
    P=[r for r in T if r.get('path')=='pass']; Sg=[r for r in T if r.get('path')=='stage']
    print(f"== {f.split('/')[-2]} n={len(R)} p99={p99} tail={len(T)} pass={len(P)} stage={len(Sg)}")
    keys=['busy_cons','wq','a2e','scan_wait','scan_to_enq','p2wait','scan_run','qw','pre','dec','body','first']
    print(' tail med   '+' '.join(f"{k}={q([r.get(k) for r in T],.5):.0f}" for k in keys))
    print(' tail mean  '+' '.join(f"{k}={m([r.get(k) for r in T]):.0f}" for k in keys))
    print(' all  p50   '+' '.join(f"{k}={q([r.get(k) for r in R],.5):.0f}" for k in keys))
    pk=['host_pre','pprep','api_C','q_C','x_C','b_C','q_D','x_D','b_D','q_L','x_L','b_L','poll_lat','wait','host_post','own_busy','own_span','oth_busy','npass']
    for lab,G in (('tail pass',P),('all pass',[r for r in R if r.get('path')=='pass'])):
        print(f' {lab:9} mean '+' '.join(f"{k}={m([r.get(k) for r in G]):.0f}" for k in pk))
        print(f' {lab:9} med  '+' '.join(f"{k}={q([r.get(k) for r in G],.5):.0f}" for k in pk))
    for lab,G in (('tail stage',Sg),('all stage',[r for r in R if r.get('path')=='stage'])):
        print(f' {lab:10} mean '+' '.join(f"{k}={m([r.get(k) for r in G]):.0f}" for k in ['body','own_busy','own_span','host_gap','oth_busy']))
    print(' tail gpu_busy over life %.2f vs all %.2f'%(m([r.get('gpu_busy_life') for r in T]),m([r.get('gpu_busy_life') for r in R])))
    on=defaultdict(float)
    for r in T:
        for k,v in (r.get('oth_names') or {}).items(): on[k]+=v/len(T)
    print(' tail other-kernel overlap us/grant:',', '.join(f'{k} {v:.0f}' for k,v in sorted(on.items(),key=lambda z:-z[1])[:10]))
