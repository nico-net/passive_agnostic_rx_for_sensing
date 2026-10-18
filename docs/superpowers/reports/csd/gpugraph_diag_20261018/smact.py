import sqlite3,sys,json,bisect
def q(v,f):
    v=sorted(v); return v[min(len(v)-1,int(f*len(v)))] if v else float('nan')
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    d=p[3]; db=sqlite3.connect(d+'/nsys_trace.sqlite')
    names=dict(db.execute('select metricId,metricName from TARGET_INFO_GPU_METRICS'))
    mid={v:k for k,v in names.items()}
    def series(name):
        v=list(db.execute('select timestamp,value from GPU_METRICS where metricId=? order by timestamp',(mid[name],))); return [x[0] for x in v],[x[1] for x in v]
    ts,sm=series('SMs Active [Throughput %]'); _,iss=series('SM Issue [Throughput %]'); _,clk=series('GPC Clock Frequency [MHz]')
    dr=[k for k in mid if 'DRAM' in k or 'Memory' in k]
    R=[r for r in json.load(open(d+'/nsys_trace.sqlite.rows.json')) if 'body' in r]
    R.sort(key=lambda r:r['tot']); n=len(R); B=R[int(.98*n):int(.995*n)]
    def avg_over(a,b,v):
        i=bisect.bisect_left(ts,a); j=bisect.bisect_right(ts,b); s=v[i:j]; return sum(s)/len(s) if s else None
    def dec_window(r): return (r['tend']-r['dec']*1000, r['tend'])
    life=lambda G:[x for x in (avg_over(*dec_window(r),sm) for r in G) if x is not None]
    allv=sum(sm)/len(sm)
    print(f"{p[0]:3} {p[1]:4} {p[2]} SM active mean {allv:.1f} % (p90 {q(sm,.9):.0f}, p99 {q(sm,.99):.0f}) issue {sum(iss)/len(iss):.1f} % GPC MHz p50 {q(clk,.5):.0f} p1 {q(clk,.01):.0f} | SM active during dec: all grants {q(life(R),.5):.1f} band {q(life(B),.5):.1f} | {'DRAM metrics: '+','.join(dr) if dr else ''}")
