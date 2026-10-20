"""near-p99 band (tot rank p98..p99.5): what sets p99. Per run: medians of e2e parts and pass-body parts; path share; and the 'excess' of each part over its all-grant median."""
import json,sys
def q(v,f):
    v=sorted(x for x in v if x is not None); return v[min(len(v)-1,int(f*len(v)))] if v else float('nan')
def med(G,k): return q([r.get(k) for r in G],.5)
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    cap,arm,i,d=p
    R=[r for r in json.load(open(d+'/nsys_trace.sqlite.rows.json')) if 'body' in r]
    R.sort(key=lambda r:r['tot']); n=len(R)
    B=R[int(.98*n):int(.995*n)]
    P=[r for r in B if r.get('path')=='pass']; St=[r for r in B if r.get('path')=='stage']
    for r in P: r['gpu_part']=r['body']-r['host_pre']-r['host_post']
    print(f"{cap:3} {arm:4} {i} p99={q([r['tot'] for r in R],.99)} band n={len(B)} pass={len(P)} stage={len(St)} | med tot {med(B,'tot'):.0f} a2e {med(B,'a2e'):.0f} (scan_wait {med(B,'scan_wait'):.0f} scan_to_enq {med(B,'scan_to_enq'):.0f}) qw {med(B,'qw'):.0f} pre {med(B,'pre'):.0f} dec {med(B,'dec'):.0f} nrb {med(B,'nrb'):.0f}"
          f" || pass: dec {med(P,'dec'):.0f} hpre {med(P,'host_pre'):.0f} C {med(P,'q_C'):.0f}/{med(P,'x_C'):.0f} D {med(P,'q_D'):.0f}/{med(P,'x_D'):.0f} L {med(P,'q_L'):.0f}/{med(P,'x_L'):.0f} poll {med(P,'poll_lat'):.0f} post {med(P,'host_post'):.0f} own_busy {med(P,'own_busy'):.0f} oth {med(P,'oth_busy'):.0f}"
          f" || stage: dec {med(St,'dec'):.0f} busy {med(St,'own_busy'):.0f} gap {med(St,'host_gap'):.0f} nrb {med(St,'nrb'):.0f}")
