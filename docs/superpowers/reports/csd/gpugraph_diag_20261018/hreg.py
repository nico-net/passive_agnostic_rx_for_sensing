import sqlite3,sys,json
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    db=sqlite3.connect(p[3]+'/nsys_trace.sqlite'); S=dict(db.execute('select id,value from StringIds'))
    TN={t:S.get(n,'') for t,n in db.execute('select globalTid,nameId from ThreadNames')}
    R=[r for r in json.load(open(p[3]+'/nsys_trace.sqlite.rows.json')) if 'body' in r]
    t0=min(r['tarr'] for r in R)
    ev=[(s,e,TN.get(t),S[n].split('_v')[0]) for s,e,t,n in db.execute("select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where nameId in (select id from StringIds where value like 'cudaHostRegister%' or value like 'cudaHostUnregister%' or value like 'cudaMalloc%' or value like 'cudaFree%' or value like 'cuMemHostRegister%') and start>?",(t0,))]
    R.sort(key=lambda r:r['tot']); n=len(R); T=R[int(.99*n):]; B=R[int(.98*n):int(.995*n)]
    hit=lambda G:sum(1 for r in G if any(s<r['tend'] and e>r['tarr'] and e-s>300000 for s,e,_,_ in ev))
    from collections import Counter
    c=Counter((x[2],x[3]) for x in ev); dur=sum(e-s for s,e,_,_ in ev)/1e6
    print(f"{p[0]:3} {p[1]:4} {p[2]} runtime alloc/register calls after RX start: {dict(c)} total {dur:.0f} ms, >5 ms: {sum(1 for s,e,_,_ in ev if e-s>5e6)} | top-1% grants overlapping a >0.3 ms one: {hit(T)}/{len(T)}, band {hit(B)}/{len(B)}, all {hit(R)}/{n}")
