import sqlite3,pickle,sys,collections,bisect
c=sqlite3.connect(sys.argv[1]).cursor(); P=pickle.load(open(sys.argv[2],'rb'))['passes']
S={i:s for i,s in c.execute("select id,value from StringIds")}
L=sorted(c.execute("select k.start,k.end,s.value from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.shortName where k.end-k.start>5000000 and s.value in ('refine_kernel','segment','lattice_screen','sweep','energies','normalize','finish','combine')").fetchall())
def inside(t):
    i=bisect.bisect_right(L,(t,1<<62,''))-1
    for j in range(max(0,i-3),i+1):
        if L[j][0]<=t<=L[j][1]: return L[j]
    return None
def q(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else float('nan')
R=collections.defaultdict(list)
for p in P:
    for sb in p['subs']:
        k=sb[0]; cs,ce=sb[1],sb[2]
        blk=inside(cs)
        R[(k,'during long disc kernel (>5ms)' if blk else 'otherwise')].append(((ce-cs)/1e3, blk and abs(ce-blk[1])<2_000_000))
for k,v in sorted(R.items()):
    d=[a for a,_ in v]
    print(f"{k[0]} launch {k[1]:32s} n={len(v):6d} call p50 {q(d,.5):7.1f} p99 {q(d,.99):9.1f} us; >5 ms: {sum(1 for a in d if a>5000)}, of which returned within 2 ms of that disc kernel's end: {sum(1 for a,b in v if a>5000 and b)}")
